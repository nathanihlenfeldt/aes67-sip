#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio/backend.hpp"
#include "audio/resampler.hpp"
#include "audio/ring.hpp"

namespace aes67sip {

/**
 * The routing core of the gateway.
 *
 * One real time thread owns the RAVENNA/ALSA device and moves audio between the
 * AES67 channels and the per line ring buffers used by the SIP media threads:
 *
 *   AES67 capture (endpoint -> gateway) -> downmix, gain, PTT detection
 *                                       -> resample to the codec rate
 *                                       -> tx ring -> SIP port (towards PBX)
 *
 *   SIP port (from the PBX) -> rx ring -> resample to 48 kHz -> AES67 playback
 *
 * Only the audio thread touches the resamplers and the backend; the SIP media
 * thread only moves samples through the rings.
 */
class AudioRouter {
 public:
  struct LineParams {
    std::vector<unsigned> channels{0, 1};  // RAVENNA channels carrying the line
    double gain_db{0.0};                   // applied to both directions
    double rx_gain_db{0.0};                // PBX -> AES67
    double tx_gain_db{0.0};                // AES67 -> PBX
    bool mute{false};
    bool enabled{true};
    bool call_active{false};  // when false the line sends/plays silence
    double ptt_threshold_dbfs{-45.0};
    int ptt_hold_ms{400};
  };

  struct LineMeters {
    double capture_dbfs{-1000.0};   // raw AES67 input on the line channels
    double to_sip_dbfs{-1000.0};    // what is handed to the SIP engine
    double from_sip_dbfs{-1000.0};  // what the SIP engine handed over
    double to_aes67_dbfs{-1000.0};  // what is played to the AES67 channels
    bool clipping{false};
    bool ptt{false};
    uint64_t tx_frames{0};
    uint64_t rx_frames{0};
    uint64_t tx_underruns{0};  // SIP asked for more audio than was available
    uint64_t rx_overruns{0};   // SIP pushed audio faster than we consumed it
    uint64_t rx_underruns{0};  // a block towards AES67 had to be padded
  };

  AudioRouter(AudioBackend* backend, AudioFormat format);
  ~AudioRouter();

  AudioRouter(const AudioRouter&) = delete;
  AudioRouter& operator=(const AudioRouter&) = delete;

  /** Opens the backend and starts the routing thread. */
  bool start(std::string* error);
  void stop();
  bool running() const { return running_.load(); }

  // ---- control side (any non audio thread) -------------------------------
  void add_line(int line_id, const LineParams& params, unsigned sip_rate);
  void remove_line(int line_id);
  void set_line_params(int line_id, const LineParams& params);
  void set_line_call_active(int line_id, bool active);
  void set_line_enabled(int line_id, bool enabled);
  void set_line_mute(int line_id, bool mute);
  void set_line_gain_db(int line_id, double gain_db);
  void set_line_channels(int line_id, const std::vector<unsigned>& channels);
  void set_line_sip_rate(int line_id, unsigned rate);
  void set_line_ptt_params(int line_id, double threshold_dbfs, int hold_ms);

  LineMeters meters(int line_id) const;
  /** Peak held level of the line's AES67 input. */
  double capture_dbfs(int line_id) const;
  /** True while the line's AES67 input has been above the PTT threshold. */
  bool ptt_active(int line_id) const;

  /** Per channel meters of the whole RAVENNA device. */
  std::vector<double> capture_channel_dbfs() const;
  std::vector<double> playback_channel_dbfs() const;

  /** Writes a test tone to the line's AES67 channels for `seconds`. */
  void start_test_tone(int line_id, double hz, double seconds);
  bool test_tone_running(int line_id) const;

  const AudioFormat& format() const { return format_; }
  std::string backend_kind() const;
  std::string backend_detail() const;
  unsigned backend_overruns() const;
  unsigned backend_underruns() const;

  // ---- SIP media side ----------------------------------------------------
  /** Fills `frames` mono samples towards the PBX; pads with silence. */
  size_t pull_to_sip(int line_id, float* destination, size_t frames);
  /** Queues `frames` mono samples received from the PBX. */
  void push_from_sip(int line_id, const float* source, size_t frames);

 private:
  struct Line {
    LineParams params;
    unsigned sip_rate{8000};
    unsigned pending_rate{0};
    std::unique_ptr<Resampler> to_sip;    // 48k -> codec rate
    std::unique_ptr<Resampler> to_aes67;  // codec rate -> 48k
    SpscRing tx_ring{8192};
    SpscRing rx_ring{8192};

    std::vector<float> downmix;    // 48k mono, from the AES67 capture
    std::vector<float> resampled;  // codec rate mono, towards the PBX
    std::vector<float> rx_mono;    // codec rate mono, from the PBX
    std::vector<float> upsampled;  // 48k mono, towards the AES67 playback

    double tone_hz{0.0};
    double tone_until{-1.0};
    double tone_phase{0.0};
    int64_t rx_accumulator{0};  // drift free 48k <-> codec rate conversion

    std::atomic<bool> flush_requested{false};
    std::atomic<bool> tx_primed{false};
    std::atomic<bool> ptt{false};
    std::atomic<int64_t> ptt_until_ms{0};
    std::atomic<bool> clipping{false};

    std::atomic<double> capture_peak_db{-1000.0};
    std::atomic<double> to_sip_db{-1000.0};
    std::atomic<double> from_sip_db{-1000.0};
    std::atomic<double> to_aes67_db{-1000.0};

    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> tx_underruns{0};
    std::atomic<uint64_t> rx_overruns{0};
    std::atomic<uint64_t> rx_underruns{0};
  };

  void run();
  void process_block();
  /** Resolves a line without holding the map lock during the caller's work. */
  std::shared_ptr<Line> line(int line_id) const;
  /** Snapshot of one line plus its parameters, taken once per audio block. */
  struct RouteTarget {
    std::shared_ptr<Line> line;
    LineParams params;
  };
  /** Runs `fn` on the line under the map lock; false when it does not exist. */
  template <typename Fn>
  bool with_line(int line_id, Fn&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it == lines_.end()) {
      return false;
    }
    fn(*it->second);
    return true;
  }
  void set_line_rate_locked(Line& line, unsigned rate);

  AudioBackend* backend_;
  AudioFormat format_;

  mutable std::mutex mutex_;
  std::unordered_map<int, std::shared_ptr<Line>> lines_;

  std::vector<float> capture_;
  std::vector<float> playback_;
  std::vector<double> capture_channel_db_;
  std::vector<double> playback_channel_db_;
  std::vector<RouteTarget> targets_;  // audio thread scratch, warmed up in place

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace aes67sip

