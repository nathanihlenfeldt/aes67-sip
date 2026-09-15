#include "audio/null_backend.hpp"

#include <algorithm>
#include <limits>
#include <thread>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

NullAudioBackend::NullAudioBackend(double tone_hz) : tone_hz_(tone_hz) {}

bool NullAudioBackend::open(const AudioFormat& format, std::string* error) {
  (void)error;
  format_ = format;
  if (format_.sample_rate == 0) {
    format_.sample_rate = 48000;
  }
  if (format_.channels == 0) {
    format_.channels = 1;
  }
  if (format_.period_frames == 0) {
    format_.period_frames = 48;
  }
  last_written_.assign(format_.frames_to_samples(format_.period_frames), 0.0F);
  phase_ = 0.0;
  next_deadline_ = std::chrono::steady_clock::now();
  open_ = true;
  LOG_INFO("null audio backend opened (", format_.sample_rate, " Hz, ",
           format_.channels, " ch, ", format_.period_frames, " frames/period)");
  return true;
}

void NullAudioBackend::close() {
  open_ = false;
}

void NullAudioBackend::set_tone_hz(double tone_hz) {
  tone_hz_ = tone_hz;
  LOG_INFO("null audio backend tone set to ", tone_hz, " Hz");
}

void NullAudioBackend::fill_tone(float* destination, unsigned frames) {
  const double tone = tone_hz_.load();
  if (tone <= 0.0) {
    std::fill(destination, destination + format_.frames_to_samples(frames), 0.0F);
    return;
  }
  const double step = 2.0 * M_PI * tone / static_cast<double>(format_.sample_rate);
  for (unsigned frame = 0; frame < frames; ++frame) {
    const auto value = static_cast<float>(amplitude_ * std::sin(phase_));
    for (unsigned channel = 0; channel < format_.channels; ++channel) {
      destination[frame * format_.channels + channel] = value;
    }
    phase_ += step;
    if (phase_ > 2.0 * M_PI) {
      phase_ -= 2.0 * M_PI;
    }
  }
}

bool NullAudioBackend::read(float* destination, unsigned frames, std::string* error) {
  (void)error;
  if (!open_) {
    return false;
  }
  fill_tone(destination, frames);

  // emulate the device cadence so the routing thread runs at real speed
  const auto period = std::chrono::nanoseconds(
      static_cast<long long>(frames) * 1000000000LL /
      static_cast<long long>(format_.sample_rate));
  next_deadline_ += period;
  const auto now = std::chrono::steady_clock::now();
  if (next_deadline_ > now) {
    std::this_thread::sleep_until(next_deadline_);
  } else {
    // fell behind, resynchronise instead of accumulating drift
    next_deadline_ = now;
  }
  return true;
}

bool NullAudioBackend::write(const float* source, unsigned frames, std::string* error) {
  (void)error;
  if (!open_) {
    return false;
  }
  const size_t samples = format_.frames_to_samples(frames);
  if (samples == last_written_.size()) {
    std::copy(source, source + samples, last_written_.begin());
  } else {
    last_written_.assign(source, source + samples);
  }
  write_count_++;
  return true;
}

std::string NullAudioBackend::detail() const {
  return "null device " + std::to_string(format_.channels) + "ch @" +
         std::to_string(format_.sample_rate) + "Hz";
}

double NullAudioBackend::last_written_dbfs() const {
  if (last_written_.empty()) {
    return -std::numeric_limits<double>::infinity();
  }
  double sum = 0.0;
  for (const float sample : last_written_) {
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  }
  return linear_to_dbfs(std::sqrt(sum / static_cast<double>(last_written_.size())));
}

}  // namespace aes67sip
