#include "audio/resampler.hpp"

#include <algorithm>
#include <cmath>

#include "log.hpp"

namespace aes67sip {

namespace {

unsigned gcd(unsigned a, unsigned b) {
  while (b != 0) {
    const unsigned t = a % b;
    a = b;
    b = t;
  }
  return a;
}

double sinc(double x) {
  if (std::fabs(x) < 1e-9) {
    return 1.0;
  }
  const double pix = M_PI * x;
  return std::sin(pix) / pix;
}

double blackman(size_t index, size_t length) {
  if (length <= 1) {
    return 1.0;
  }
  const double n = static_cast<double>(index);
  const double n1 = static_cast<double>(length - 1);
  return 0.42 - 0.5 * std::cos(2.0 * M_PI * n / n1) +
         0.08 * std::cos(4.0 * M_PI * n / n1);
}

}  // namespace

Resampler::Resampler(unsigned in_rate, unsigned out_rate, unsigned channels,
                     unsigned taps_per_phase)
    : in_rate_(in_rate == 0 ? 8000 : in_rate),
      out_rate_(out_rate == 0 ? 8000 : out_rate),
      channels_(channels == 0 ? 1 : channels),
      taps_(std::max(4U, taps_per_phase)) {
  const unsigned divisor = gcd(in_rate_, out_rate_);
  up_ = out_rate_ / divisor;   // L
  down_ = in_rate_ / divisor;  // M
  integer_ratio_ = up_ <= 32 && down_ <= 32 && up_ * down_ <= 64;

  if (in_rate_ == out_rate_) {
    return;
  }
  if (integer_ratio_) {
    design_filter();
  } else {
    LOG_WARN("resampler ", in_rate_, " -> ", out_rate_,
             " Hz is not an integer ratio, using linear interpolation");
    previous_.assign(channels_, 0.0F);
  }
}

void Resampler::design_filter() {
  const unsigned length = taps_ * up_;  // N = taps per phase * L

  // Prototype low pass at the interpolation rate (L * in_rate), cutoff at the
  // lower of the two Nyquist frequencies, with a small transition margin.
  const double interpolation_rate = static_cast<double>(in_rate_) * up_;
  const double cutoff = 0.5 * static_cast<double>(std::min(in_rate_, out_rate_));
  const double fc = 0.92 * cutoff / interpolation_rate;
  const double center = 0.5 * static_cast<double>(length - 1);

  std::vector<double> prototype(length, 0.0);
  double sum = 0.0;
  for (unsigned n = 0; n < length; ++n) {
    const double value = 2.0 * fc *
                         sinc(2.0 * fc * (static_cast<double>(n) - center)) *
                         blackman(n, length);
    prototype[n] = value;
    sum += value;
  }

  // Normalise to a DC gain of `up_` so an upsample-by-L keeps unity gain and a
  // decimate-by-M does not change the level.
  const double scale = static_cast<double>(up_) / (sum == 0.0 ? 1.0 : sum);
  for (double& value : prototype) {
    value *= scale;
  }

  // Store phase major: filter_[phase * taps_ + i] = prototype[phase + i * up_]
  filter_.assign(static_cast<size_t>(up_) * taps_, 0.0);
  for (unsigned phase = 0; phase < up_; ++phase) {
    for (unsigned i = 0; i < taps_; ++i) {
      const unsigned index = phase + i * up_;
      if (index < length) {
        filter_[static_cast<size_t>(phase) * taps_ + i] = prototype[index];
      }
    }
  }

  history_.assign(static_cast<size_t>(taps_) * channels_, 0.0F);
  base_ = 0;
  input_frames_ = 0;
}

size_t Resampler::max_output_frames(size_t in_frames) const {
  const size_t scaled = (in_frames * out_rate_ + in_rate_ - 1) / in_rate_;
  return scaled + taps_ + 4;
}

void Resampler::reset() {
  std::fill(history_.begin(), history_.end(), 0.0F);
  base_ = 0;
  input_frames_ = 0;
  position_ = 0.0;
  std::fill(previous_.begin(), previous_.end(), 0.0F);
}

size_t Resampler::process(const float* input, size_t in_frames, float* output) {
  if (in_frames == 0) {
    return 0;
  }
  if (in_rate_ == out_rate_) {
    std::copy(input, input + in_frames * channels_, output);
    return in_frames;
  }
  if (integer_ratio_) {
    return process_polyphase(input, in_frames, output);
  }
  return process_linear(input, in_frames, output);
}

size_t Resampler::process_polyphase(const float* input, size_t in_frames,
                                    float* output) {
  const size_t history_frames = history_.size() / channels_;
  const size_t total_frames = history_frames + in_frames;

  std::vector<float> buffer(total_frames * channels_);
  if (history_frames > 0) {
    std::copy(history_.begin(), history_.end(), buffer.begin());
  }
  std::copy(input, input + in_frames * channels_,
            buffer.begin() + static_cast<long>(history_frames * channels_));

  size_t produced = 0;
  // Absolute index of the newest input frame available to this call.  Indices
  // are global (they never restart at each call) so the polyphase phase and the
  // filter history stay consistent across blocks.
  const int64_t last_available =
      input_frames_ + static_cast<int64_t>(in_frames) - 1;
  const int64_t buffer_offset =
      static_cast<int64_t>(history_frames) - input_frames_;

  while (true) {
    const int64_t k0 = base_ / static_cast<int64_t>(up_);
    if (k0 > last_available) {
      break;
    }
    const unsigned phase = static_cast<unsigned>(base_ % up_);
    const double* coefficients = &filter_[static_cast<size_t>(phase) * taps_];

    for (unsigned channel = 0; channel < channels_; ++channel) {
      double accumulator = 0.0;
      for (unsigned i = 0; i < taps_; ++i) {
        const int64_t source_frame = k0 - static_cast<int64_t>(i);
        const int64_t buffer_index = source_frame + buffer_offset;
        if (buffer_index < 0 ||
            buffer_index >= static_cast<int64_t>(total_frames)) {
          continue;  // not yet available: treat missing history as silence
        }
        accumulator +=
            static_cast<double>(
                buffer[static_cast<size_t>(buffer_index) * channels_ + channel]) *
            coefficients[i];
      }
      output[produced * channels_ + channel] = static_cast<float>(accumulator);
    }
    ++produced;
    base_ += static_cast<int64_t>(down_);
  }
  input_frames_ += static_cast<int64_t>(in_frames);

  // keep the newest taps_ input frames as filter history
  if (total_frames >= taps_) {
    std::copy(
        buffer.begin() + static_cast<long>((total_frames - taps_) * channels_),
        buffer.end(), history_.begin());
  } else {
    std::fill(history_.begin(), history_.end(), 0.0F);
    std::copy(
        buffer.begin(), buffer.end(),
        history_.begin() + static_cast<long>((taps_ - total_frames) * channels_));
  }
  return produced;
}

size_t Resampler::process_linear(const float* input, size_t in_frames,
                                 float* output) {
  const double ratio =
      static_cast<double>(in_rate_) / static_cast<double>(out_rate_);
  size_t produced = 0;
  while (position_ < static_cast<double>(in_frames)) {
    const size_t index = static_cast<size_t>(position_);
    const double fraction = position_ - static_cast<double>(index);
    for (unsigned channel = 0; channel < channels_; ++channel) {
      const float current = input[index * channels_ + channel];
      const float next = (index + 1 < in_frames)
                             ? input[(index + 1) * channels_ + channel]
                             : current;
      output[produced * channels_ + channel] = static_cast<float>(
          static_cast<double>(current) +
          (static_cast<double>(next) - static_cast<double>(current)) * fraction);
    }
    ++produced;
    position_ += ratio;
  }
  position_ -= static_cast<double>(in_frames);
  if (position_ < 0.0) {
    position_ = 0.0;
  }
  return produced;
}

}  // namespace aes67sip
