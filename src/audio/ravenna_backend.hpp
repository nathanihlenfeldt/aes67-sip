#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <alsa/asoundlib.h>

#include "audio/backend.hpp"

namespace aes67sip {

/**
 * AES67 audio backend for the Merging RAVENNA ALSA device.
 *
 * The kernel module exposes one PCM device (default `plughw:RAVENNA`) whose
 * channels are bound to the AES67 sources and sinks by the daemon's channel
 * maps, so this backend only has to behave like a plain multichannel sound
 * card at the AES67 native rate (48 kHz):
 *
 *   capture  substream -> audio coming from the intercom endpoints
 *   playback substream -> audio going to the intercom endpoints
 *
 * The device only produces/consumes samples once the PTP slave is locked *and*
 * the substream has been triggered (`snd_pcm_start()`): an untriggered stream
 * stays silent for ever, which is why open() starts both directions.  `read()`
 * returns silence (rather than failing) while PTP is not locked.
 */
class RavennaAudioBackend : public AudioBackend {
 public:
  explicit RavennaAudioBackend(const AudioConfig& config);
  ~RavennaAudioBackend() override;

  bool open(const AudioFormat& format, std::string* error) override;
  void close() override;
  bool is_open() const override { return open_; }

  bool read(float* destination, unsigned frames, std::string* error) override;
  bool write(const float* source, unsigned frames, std::string* error) override;

  std::string kind() const override { return "ravenna"; }
  std::string detail() const override;
  const AudioFormat& format() const override { return format_; }

  unsigned overruns() const override { return overruns_.load(); }
  unsigned underruns() const override { return underruns_.load(); }

  /** Device name as configured, e.g. "plughw:RAVENNA". */
  const std::string& device() const { return config_.device; }

 private:
  bool open_stream(snd_pcm_stream_t direction, snd_pcm_t** handle,
                   std::string* error);
  /**
   * Trigger a substream so the driver's audio engine starts ticking.
   *
   * The RAVENNA kernel module only moves audio for a substream that has been
   * started (its ALSA trigger callback marks the direction as running); a
   * prepared but untriggered stream never produces or consumes a single frame.
   * For playback the ring is primed with silence first, otherwise the first
   * tick underruns and the driver drops the stream again.
   */
  bool start_stream(snd_pcm_t* handle, std::string* error);
  /**
   * Reopen the device when the driver's engine has gone idle.
   *
   * The RAVENNA module stops its 1 ms audio engine when the daemon tells it to
   * restart (starting or restarting `aes67-daemon` does that, as does a sample
   * rate change): both substreams stay open and report RUNNING, but not one
   * frame moves - nothing is captured, our sources transmit nothing and the
   * endpoints hear nothing, with no error anywhere.  Closing and reopening the
   * PCM (what restarting the gateway does by hand) re-triggers the streams, so
   * do exactly that, throttled, when no frames have arrived.
   */
  void recover_if_stalled();
  /** Device buffer -> float samples; returns the number of frames converted. */
  size_t to_float(const void* source, size_t samples, float* destination) const;
  /** float samples -> device buffer; returns the number of frames converted. */
  size_t from_float(const float* source, size_t samples, void* destination) const;
  size_t bytes_per_sample() const;
  void fill_silence(float* destination, unsigned frames) const;

  AudioConfig config_;
  AudioFormat format_{};
  snd_pcm_format_t pcm_format_{SND_PCM_FORMAT_S16_LE};

  snd_pcm_t* capture_{nullptr};
  snd_pcm_t* playback_{nullptr};

  std::vector<uint8_t> capture_raw_;
  std::vector<uint8_t> playback_raw_;

  /** Negotiated playback geometry, used to prime the ring before triggering. */
  snd_pcm_uframes_t playback_period_frames_{0};
  snd_pcm_uframes_t playback_buffer_frames_{0};

  /** monotonic time of the last captured frame, and of the last recovery. */
  double last_frames_at_{0.0};
  double last_recover_at_{0.0};

  std::atomic<unsigned> overruns_{0};
  std::atomic<unsigned> underruns_{0};
  bool open_{false};
};

}  // namespace aes67sip
