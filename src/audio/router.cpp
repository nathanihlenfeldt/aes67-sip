// Gateway audio routing core.
//
// This translation unit owns the single thread that pumps the RAVENNA/ALSA
// device and bridges it to the per line rings used by the SIP media threads.

#include "audio/router.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

double interleaved_rms_dbfs(const float* data, unsigned frames, unsigned stride,
                            unsigned offset) {
  double sum = 0.0;
  for (unsigned frame = 0; frame < frames; ++frame) {
    const double value = data[static_cast<size_t>(frame) * stride + offset];
    sum += value * value;
  }
  return linear_to_dbfs(std::sqrt(sum / std::max(1U, frames)));
}

double peak_dbfs(const float* data, size_t count) {
  double peak = 0.0;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(static_cast<double>(data[i])));
  }
  return linear_to_dbfs(peak);
}

}  // namespace

AudioRouter::AudioRouter(AudioBackend* backend, AudioFormat format)
    : backend_(backend), format_(format) {
  if (format_.channels == 0) {
    format_.channels = 1;
  }
  if (format_.period_frames == 0) {
    format_.period_frames = 48;
  }
  capture_.assign(format_.frames_to_samples(format_.period_frames), 0.0F);
  playback_.assign(format_.frames_to_samples(format_.period_frames), 0.0F);
  capture_channel_db_.assign(format_.channels, kSilenceDbfs);
  playback_channel_db_.assign(format_.channels, kSilenceDbfs);
}

AudioRouter::~AudioRouter() {
  stop();
}

bool AudioRouter::start(std::string* error) {
  if (running_.load()) {
    return true;
  }
  if (!backend_->is_open() && !backend_->open(format_, error)) {
    // Remember why: the gateway keeps running so the UI can explain it.
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error != nullptr ? *error : "cannot open the audio device";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
  }
  stop_requested_ = false;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  LOG_INFO("audio router started: ", backend_->detail(), ", ",
           format_.period_frames, " frames/period");
  return true;
}

std::string AudioRouter::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

void AudioRouter::stop() {
  if (!running_.load()) {
    return;
  }
  stop_requested_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
  backend_->close();
  LOG_INFO("audio router stopped");
}

void AudioRouter::run() {
  set_thread_name("aes67-audio");
  set_realtime_thread_priority(80);
  unsigned consecutive_errors = 0;
  while (!stop_requested_.load()) {
    std::string error;
    if (!backend_->read(capture_.data(), format_.period_frames, &error)) {
      ++consecutive_errors;
      if (consecutive_errors == 1 || consecutive_errors % 100 == 0) {
        LOG_ERROR("audio capture failed (", consecutive_errors, "): ", error);
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::min<unsigned>(100, consecutive_errors)));
      continue;
    }
    consecutive_errors = 0;
    process_block();

    std::string write_error;
    if (!backend_->write(playback_.data(), format_.period_frames, &write_error)) {
      LOG_ERROR("audio playback failed: ", write_error);
    }
  }
}

std::shared_ptr<AudioRouter::Line> AudioRouter::line(int line_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = lines_.find(line_id);
  return it == lines_.end() ? nullptr : it->second;
}

void AudioRouter::set_line_rate_locked(Line& line, unsigned rate) {
  if (rate == 0) {
    rate = 8000;
  }
  line.sip_rate = rate;
  line.pending_rate = 0;
  line.to_sip = std::make_unique<Resampler>(format_.sample_rate, rate, 1);
  line.to_aes67 = std::make_unique<Resampler>(rate, format_.sample_rate, 1);
  line.rx_accumulator = 0;
  LOG_INFO("line resamplers built for a ", rate, " Hz codec rate");
}

// ---------------------------------------------------------------------------
// control side
// ---------------------------------------------------------------------------

void AudioRouter::add_line(int line_id, const LineParams& params,
                           unsigned sip_rate) {
  auto fresh = std::make_shared<Line>();
  fresh->params = params;
  fresh->pending_rate = sip_rate == 0 ? 8000 : sip_rate;
  fresh->flush_requested = true;
  fresh->tx_primed = false;
  std::lock_guard<std::mutex> lock(mutex_);
  set_line_rate_locked(*fresh, fresh->pending_rate);
  // The call gate belongs to the media callbacks, not to configuration: a line
  // that is already in a call keeps it, because a configuration edit must not
  // silence a live line - the call is still up, so nothing would raise the gate
  // again.  (The engine protects a call from a configuration update for the same
  // reason.)  On the first add there is nothing to preserve, so the caller's value
  // stands.
  const auto existing = lines_.find(line_id);
  if (existing != lines_.end()) {
    fresh->params.call_active = existing->second->params.call_active;
  }
  lines_[line_id] = std::move(fresh);
}

void AudioRouter::remove_line(int line_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  lines_.erase(line_id);
}

void AudioRouter::set_line_params(int line_id, const LineParams& params) {
  with_line(line_id, [&](Line& line) {
    const bool call_active = line.params.call_active;
    line.params = params;
    line.params.call_active = call_active;  // the call, not the configuration
  });
}

void AudioRouter::set_line_call_active(int line_id, bool active) {
  with_line(line_id, [&](Line& line) {
    if (active && !line.params.call_active) {
      line.flush_requested = true;
      line.tx_primed = false;
    }
    line.params.call_active = active;
  });
}

void AudioRouter::set_line_enabled(int line_id, bool enabled) {
  with_line(line_id, [&](Line& line) { line.params.enabled = enabled; });
}

void AudioRouter::set_line_mute(int line_id, bool mute) {
  with_line(line_id, [&](Line& line) { line.params.mute = mute; });
}

void AudioRouter::set_line_gain_db(int line_id, double gain_db) {
  with_line(line_id, [&](Line& line) { line.params.gain_db = gain_db; });
}

void AudioRouter::set_line_channels(int line_id,
                                    const std::vector<unsigned>& channels) {
  with_line(line_id, [&](Line& line) { line.params.channels = channels; });
}

void AudioRouter::set_line_sip_rate(int line_id, unsigned rate) {
  with_line(line_id,
            [&](Line& line) { line.pending_rate = rate == 0 ? 8000 : rate; });
}

void AudioRouter::set_line_ptt_params(int line_id, double threshold_dbfs,
                                      int hold_ms) {
  with_line(line_id, [&](Line& line) {
    line.params.ptt_threshold_dbfs = threshold_dbfs;
    line.params.ptt_hold_ms = std::max(0, hold_ms);
  });
}

void AudioRouter::start_test_tone(int line_id, double hz, double seconds) {
  with_line(line_id, [&](Line& line) {
    line.tone_hz = hz;
    line.tone_until = monotonic_seconds() + std::max(0.1, seconds);
    line.tone_phase = 0.0;
  });
  LOG_INFO("test tone ", hz, " Hz -> line ", line_id, " for ", seconds, " s");
}

void AudioRouter::stop_test_tone(int line_id) {
  with_line(line_id, [&](Line& line) { line.tone_until = 0.0; });
  LOG_INFO("test tone stopped on line ", line_id);
}

bool AudioRouter::test_tone_running(int line_id) const {
  const auto target = line(line_id);
  return target != nullptr && target->tone_until > monotonic_seconds();
}

// ---------------------------------------------------------------------------
// audio thread: one period of audio between the AES67 device and the lines
// ---------------------------------------------------------------------------

void AudioRouter::process_block() {
  const unsigned frames = format_.period_frames;
  const unsigned channels = format_.channels;
  const double decay_db = kPeakDecayDbPerMs * 1000.0 * frames /
                          static_cast<double>(format_.sample_rate);
  const double now = monotonic_seconds();
  const int64_t now_ms_value = now_ms();

  std::fill(playback_.begin(), playback_.end(), 0.0F);
  for (unsigned channel = 0; channel < channels; ++channel) {
    capture_channel_db_[channel] = hold_peak(
        capture_channel_db_[channel],
        interleaved_rms_dbfs(capture_.data(), frames, channels, channel), decay_db);
  }

  // Take a consistent snapshot of the lines (and their parameters) so control
  // threads can reconfigure the gateway without blocking the audio thread.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    targets_.clear();
    targets_.reserve(lines_.size());
    for (const auto& entry : lines_) {
      RouteTarget target;
      target.line = entry.second;
      target.params = entry.second->params;
      targets_.push_back(std::move(target));
    }
  }

  for (RouteTarget& target : targets_) {
    Line& line = *target.line;
    const LineParams& params = target.params;

    if (line.pending_rate != 0 && line.pending_rate != line.sip_rate) {
      set_line_rate_locked(line, line.pending_rate);
      line.flush_requested = true;
    }
    if (line.flush_requested.exchange(false)) {
      line.to_sip->reset();
      line.to_aes67->reset();
      line.rx_accumulator = 0;
      line.rx_ring.discard(line.rx_ring.available());
      line.capture_peak_db = kSilenceDbfs;
    }

    bool mapped = false;
    for (const unsigned channel : params.channels) {
      if (channel < channels) {
        mapped = true;
        break;
      }
    }
    // The conference leg has no device channels: its media is the matrix's
    // conference sides, which exist only while it has a call.
    const bool conference_up =
        params.conference && params.enabled && params.call_active && !params.mute;
    if (line.downmix.size() != frames) {
      line.downmix.assign(frames, 0.0F);
    }

    // ---- AES67 capture -> PBX -------------------------------------------
    if (params.conference) {
      // What the conference hears: the sum of the members of the party lines that
      // claim it, each at its contribution level, which the matrix builds.
      if (conference_up && matrix_ != nullptr) {
        matrix_->pull_conference_audio(line.downmix.data(), frames);
      } else {
        std::fill(line.downmix.begin(), line.downmix.end(), 0.0F);
      }
    } else if (mapped) {
      for (unsigned frame = 0; frame < frames; ++frame) {
        double sum = 0.0;
        const size_t base = static_cast<size_t>(frame) * channels;
        for (const unsigned channel : params.channels) {
          if (channel < channels) {
            sum += capture_[base + channel];
          }
        }
        line.downmix[frame] =
            static_cast<float>(sum / static_cast<double>(params.channels.size()));
      }
    } else {
      std::fill(line.downmix.begin(), line.downmix.end(), 0.0F);
    }

    const double block_peak = peak_dbfs(line.downmix.data(), line.downmix.size());
    line.capture_peak_db =
        hold_peak(line.capture_peak_db.load(), block_peak, decay_db);
    line.clipping = block_peak > -0.01;  // about 1 dB of headroom left
    if (block_peak > params.ptt_threshold_dbfs) {
      line.ptt = true;
      line.ptt_until_ms = now_ms_value + std::max(0, params.ptt_hold_ms);
    } else if (now_ms_value > line.ptt_until_ms.load()) {
      line.ptt = false;
    }

    const bool send = params.enabled && (mapped || params.conference) &&
                      params.call_active && !params.mute;
    if (send) {
      const double tx_gain = db_to_linear(params.gain_db + params.tx_gain_db);
      if (tx_gain != 1.0) {
        for (float& sample : line.downmix) {
          sample = static_cast<float>(sample * tx_gain);
        }
      }
      const size_t capacity = line.to_sip->max_output_frames(frames);
      if (line.resampled.size() < capacity) {
        line.resampled.resize(capacity, 0.0F);
      }
      const size_t produced =
          line.to_sip->process(line.downmix.data(), frames, line.resampled.data());
      const size_t written = line.tx_ring.write(line.resampled.data(), produced);
      line.tx_frames += written;
      if (written < produced) {
        line.tx_underruns++;  // the SIP side is not draining fast enough
      }
      line.to_sip_db =
          hold_peak(line.to_sip_db.load(),
                    peak_dbfs(line.resampled.data(), produced), decay_db);
    } else {
      line.to_sip_db = hold_peak(line.to_sip_db.load(), kSilenceDbfs, decay_db);
    }

    // ---- PBX -> AES67 playback ------------------------------------------
    line.rx_accumulator += static_cast<int64_t>(frames) * line.sip_rate;
    const size_t rx_needed =
        static_cast<size_t>(line.rx_accumulator / format_.sample_rate);
    line.rx_accumulator -=
        static_cast<int64_t>(rx_needed) * static_cast<int64_t>(format_.sample_rate);
    if (line.rx_mono.size() < rx_needed) {
      line.rx_mono.assign(rx_needed, 0.0F);
    }
    if (params.enabled && params.call_active && !params.mute) {
      const size_t received = line.rx_ring.read(line.rx_mono.data(), rx_needed);
      if (received < rx_needed) {
        std::fill(line.rx_mono.begin() + static_cast<long>(received),
                  line.rx_mono.begin() + static_cast<long>(rx_needed), 0.0F);
        line.rx_underruns++;
      }
      line.rx_frames += received;
      const double rx_gain = db_to_linear(params.gain_db + params.rx_gain_db);
      if (rx_gain != 1.0) {
        for (size_t i = 0; i < rx_needed; ++i) {
          line.rx_mono[i] = static_cast<float>(line.rx_mono[i] * rx_gain);
        }
      }
      line.from_sip_db =
          hold_peak(line.from_sip_db.load(),
                    peak_dbfs(line.rx_mono.data(), rx_needed), decay_db);
    } else {
      line.rx_ring.discard(line.rx_ring.available());
      std::fill(line.rx_mono.begin(),
                line.rx_mono.begin() + static_cast<long>(rx_needed), 0.0F);
      line.from_sip_db = hold_peak(line.from_sip_db.load(), kSilenceDbfs, decay_db);
    }

    const size_t capacity = line.to_aes67->max_output_frames(rx_needed);
    if (line.upsampled.size() < capacity) {
      line.upsampled.resize(capacity, 0.0F);
    }
    const size_t upsampled = line.to_aes67->process(line.rx_mono.data(), rx_needed,
                                                    line.upsampled.data());

    double playback_peak = kSilenceDbfs;
    if (params.conference) {
      // What the conference says goes into the matrix, which mixes it into the
      // party lines that claim it.  No device channel is involved.
      if (send && matrix_ != nullptr && upsampled > 0) {
        const unsigned count = std::min(static_cast<unsigned>(upsampled), frames);
        matrix_->push_conference_audio(line.upsampled.data(), count);
        playback_peak = peak_dbfs(line.upsampled.data(), count);
      }
    } else if (line.tone_until > now && mapped) {
      // commissioning tone on the AES67 output of this line
      const double step =
          2.0 * M_PI * line.tone_hz / static_cast<double>(format_.sample_rate);
      for (unsigned frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<float>(0.25 * std::sin(line.tone_phase));
        const size_t base = static_cast<size_t>(frame) * channels;
        for (const unsigned channel : params.channels) {
          if (channel < channels) {
            playback_[base + channel] = value;
          }
        }
        line.tone_phase += step;
      }
      playback_peak = linear_to_dbfs(0.25);
    } else if (upsampled > 0 && send) {
      const unsigned count = std::min(static_cast<unsigned>(upsampled), frames);
      for (unsigned frame = 0; frame < count; ++frame) {
        const float value = line.upsampled[frame];
        const size_t base = static_cast<size_t>(frame) * channels;
        for (const unsigned channel : params.channels) {
          if (channel < channels) {
            playback_[base + channel] += value;
          }
        }
      }
      playback_peak = peak_dbfs(line.upsampled.data(), count);
    }
    line.to_aes67_db = hold_peak(line.to_aes67_db.load(), playback_peak, decay_db);
  }

  // The intercom matrix writes the channels its party lines own, built from the
  // captured channels.  It runs after the SIP-facing lines so that its channels
  // are the ones it mixes, and before clipping and metering so that its output is
  // limited and measured like everything else.
  if (matrix_ != nullptr) {
    matrix_->process(capture_.data(), playback_.data(), channels, frames);
  }

  for (float& sample : playback_) {
    sample = std::max(-1.0F, std::min(1.0F, sample));
  }
  for (unsigned channel = 0; channel < channels; ++channel) {
    playback_channel_db_[channel] =
        hold_peak(playback_channel_db_[channel],
                  interleaved_rms_dbfs(playback_.data(), frames, channels, channel),
                  decay_db);
  }
}

// ---------------------------------------------------------------------------
// monitoring side
// ---------------------------------------------------------------------------

AudioRouter::LineMeters AudioRouter::meters(int line_id) const {
  LineMeters result;
  const auto target = line(line_id);
  if (target == nullptr) {
    return result;
  }
  result.capture_dbfs = target->capture_peak_db.load();
  result.to_sip_dbfs = target->to_sip_db.load();
  result.from_sip_dbfs = target->from_sip_db.load();
  result.to_aes67_dbfs = target->to_aes67_db.load();
  result.clipping = target->clipping.load();
  result.ptt = target->ptt.load();
  result.tx_frames = target->tx_frames.load();
  result.rx_frames = target->rx_frames.load();
  result.tx_underruns = target->tx_underruns.load();
  result.rx_overruns = target->rx_overruns.load();
  result.rx_underruns = target->rx_underruns.load();
  return result;
}

double AudioRouter::capture_dbfs(int line_id) const {
  const auto target = line(line_id);
  return target == nullptr ? kSilenceDbfs : target->capture_peak_db.load();
}

bool AudioRouter::ptt_active(int line_id) const {
  const auto target = line(line_id);
  return target != nullptr && target->ptt.load();
}

std::vector<double> AudioRouter::capture_channel_dbfs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capture_channel_db_;
}

std::vector<double> AudioRouter::playback_channel_dbfs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return playback_channel_db_;
}

std::string AudioRouter::backend_kind() const {
  return backend_->kind();
}

std::string AudioRouter::backend_detail() const {
  return backend_->detail();
}

unsigned AudioRouter::backend_overruns() const {
  return backend_->overruns();
}

unsigned AudioRouter::backend_underruns() const {
  return backend_->underruns();
}

// ---------------------------------------------------------------------------
// SIP media side
// ---------------------------------------------------------------------------

size_t AudioRouter::pull_to_sip(int line_id, float* destination, size_t frames) {
  const auto target = line(line_id);
  if (target == nullptr) {
    std::fill(destination, destination + frames, 0.0F);
    return 0;
  }
  // the first pull of a call drops whatever was left over from the last one
  if (!target->tx_primed.exchange(true)) {
    target->tx_ring.discard(target->tx_ring.available());
    std::fill(destination, destination + frames, 0.0F);
    return 0;
  }
  const size_t read = target->tx_ring.read(destination, frames);
  if (read < frames) {
    std::fill(destination + read, destination + frames, 0.0F);
    target->tx_underruns++;
  }
  return read;
}

void AudioRouter::push_from_sip(int line_id, const float* source, size_t frames) {
  const auto target = line(line_id);
  if (target == nullptr) {
    return;
  }
  const size_t written = target->rx_ring.write(source, frames);
  if (written < frames) {
    target->rx_overruns++;
  }
}

}  // namespace aes67sip
