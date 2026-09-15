#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

#include "audio/backend.hpp"

namespace aes67sip {

/**
 * Development / test backend.
 *
 * Emulates the timing of the RAVENNA device by pacing itself at the period
 * interval and produces either silence or a low level test tone.  It also keeps
 * a copy of the last written block so a loopback test can verify the whole
 * routing chain without hardware.
 */
class NullAudioBackend : public AudioBackend {
 public:
  explicit NullAudioBackend(double tone_hz = 0.0);

  bool open(const AudioFormat& format, std::string* error) override;
  void close() override;
  bool is_open() const override { return open_; }

  bool read(float* destination, unsigned frames, std::string* error) override;
  bool write(const float* source, unsigned frames, std::string* error) override;

  std::string kind() const override { return "null"; }
  std::string detail() const override;
  const AudioFormat& format() const override { return format_; }

  unsigned overruns() const override { return 0; }
  unsigned underruns() const override { return 0; }

  /** Changes the generated tone at runtime (0 = silence). */
  void set_tone_hz(double tone_hz);

  /** RMS level of the most recent block written to the backend. */
  double last_written_dbfs() const;

 private:
  void fill_tone(float* destination, unsigned frames);

  AudioFormat format_;
  std::atomic<bool> open_{false};
  std::atomic<double> tone_hz_{0.0};
  double phase_{0.0};
  double amplitude_{0.25};
  std::chrono::steady_clock::time_point next_deadline_{};
  std::vector<float> last_written_;
  std::atomic<unsigned> write_count_{0};
};

}  // namespace aes67sip
