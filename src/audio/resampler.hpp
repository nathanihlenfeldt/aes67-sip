#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aes67sip {

/**
 * Streaming sample rate converter for interleaved float32 audio.
 *
 * The AES67 side always runs at 48 kHz while SIP codecs use 8 kHz (G.711) or
 * 16 kHz (G.722), so all conversions this gateway needs are integer ratios.  For
 * those an L/M polyphase FIR is used; for anything else a linear interpolating
 * fallback keeps the code working (with a comment in the log).
 *
 * Instances are not thread safe; use one per direction and per line.
 */
class Resampler {
 public:
  /** `channels` is the number of interleaved channels in both streams. */
  Resampler(unsigned in_rate, unsigned out_rate, unsigned channels,
            unsigned taps_per_phase = 16);

  unsigned in_rate() const { return in_rate_; }
  unsigned out_rate() const { return out_rate_; }
  unsigned channels() const { return channels_; }

  /** True when the ratio is an exact integer L/M and the polyphase path is used. */
  bool is_integer_ratio() const { return integer_ratio_; }

  /** Worst case number of output frames produced by `process()`. */
  size_t max_output_frames(size_t in_frames) const;

  /**
   * Converts `in_frames` frames (interleaved) into `output`; returns the number
   * of frames written.  `output` must have room for max_output_frames().
   */
  size_t process(const float* input, size_t in_frames, float* output);

  /** Clears the filter/phase state (call when switching streams). */
  void reset();

 private:
  /** Designs the low pass FIR used by the polyphase path (gain = L). */
  void design_filter();
  size_t process_polyphase(const float* input, size_t in_frames, float* output);
  size_t process_linear(const float* input, size_t in_frames, float* output);

  unsigned in_rate_{48000};
  unsigned out_rate_{8000};
  unsigned channels_{1};
  unsigned taps_{16};
  bool integer_ratio_{true};
  unsigned up_{1};    // L
  unsigned down_{1};  // M

  // prototype filter, `taps_` coefficients per phase, `up_` phases
  std::vector<double> filter_;
  std::vector<float> history_;  // interleaved filter delay line (taps_ frames)
  int64_t base_{0};             // output position * down_, in input frame units
  int64_t input_frames_{0};     // frames fed so far (absolute input indexing)

  // linear interpolation state
  double position_{0.0};
  std::vector<float> previous_;  // last input frame, interleaved
};

}  // namespace aes67sip
