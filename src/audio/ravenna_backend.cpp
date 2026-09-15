#include "audio/ravenna_backend.hpp"

#include <algorithm>
#include <cmath>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

constexpr int kWaitTimeoutMs = 500;

/** No captured frames for this long means the RAVENNA engine stopped ticking. */
constexpr double kStallSeconds = 2.0;

/** Do not reopen the device more often than this while it stays idle. */
constexpr double kRecoverIntervalSeconds = 15.0;

snd_pcm_format_t to_alsa_format(PcmFormat format) {
  switch (format) {
    case PcmFormat::kS16Le:
      return SND_PCM_FORMAT_S16_LE;
    case PcmFormat::kS24_3Le:
      return SND_PCM_FORMAT_S24_3LE;
    case PcmFormat::kS32Le:
      return SND_PCM_FORMAT_S32_LE;
  }
  return SND_PCM_FORMAT_S16_LE;
}

/** 24 bit samples are packed in three bytes, little endian. */
float convert_s24_3le(const uint8_t* source) {
  int32_t value = static_cast<int32_t>(source[0]) |
                  (static_cast<int32_t>(source[1]) << 8) |
                  (static_cast<int32_t>(source[2]) << 16);
  if ((value & 0x00800000) != 0) {
    value |= static_cast<int32_t>(0xFF000000);  // sign extend
  }
  return static_cast<float>(value) / 8388608.0F;
}

}  // namespace

RavennaAudioBackend::RavennaAudioBackend(const AudioConfig& config)
    : config_(config), pcm_format_(to_alsa_format(config.pcm_format())) {}

RavennaAudioBackend::~RavennaAudioBackend() {
  close();
}

size_t RavennaAudioBackend::bytes_per_sample() const {
  switch (pcm_format_) {
    case SND_PCM_FORMAT_S16_LE:
      return 2;
    case SND_PCM_FORMAT_S24_3LE:
      return 3;
    default:
      return 4;
  }
}

bool RavennaAudioBackend::open(const AudioFormat& format, std::string* error) {
  format_ = format;
  if (format_.sample_rate == 0) {
    format_.sample_rate = 48000;
  }
  if (format_.channels == 0) {
    format_.channels = 2;
  }
  if (format_.period_frames == 0) {
    format_.period_frames = 48;
  }

  const auto open_both = [this](std::string* open_error) {
    if (!open_stream(SND_PCM_STREAM_CAPTURE, &capture_, open_error)) {
      close();
      return false;
    }
    if (!open_stream(SND_PCM_STREAM_PLAYBACK, &playback_, open_error)) {
      close();
      return false;
    }
    return true;
  };

  std::string first_error;
  if (!open_both(&first_error)) {
    // The driver may not accept the configured sample format (3 byte formats are
    // not available everywhere).  Fall back to 16 bit rather than leaving the
    // appliance without audio, and say so - the AES67 payload stays L24 either
    // way, only the resolution of the ALSA side changes.
    if (pcm_format_ == SND_PCM_FORMAT_S16_LE) {
      if (error != nullptr) {
        *error = first_error;
      }
      return false;
    }
    LOG_WARN("the RAVENNA device rejected audio.format '", config_.format, "' (",
             first_error, "); retrying with s16_le (16 bit resolution)");
    pcm_format_ = SND_PCM_FORMAT_S16_LE;
    if (!open_both(error)) {
      return false;
    }
  }

  const size_t raw_bytes =
      format_.frames_to_samples(format_.period_frames) * bytes_per_sample();
  capture_raw_.assign(raw_bytes, 0);
  playback_raw_.assign(raw_bytes, 0);
  open_ = true;
  last_frames_at_ = monotonic_seconds();
  last_recover_at_ = 0.0;

  LOG_INFO("RAVENNA audio device '", config_.device, "' opened: ", format_.channels,
           " ch, ", format_.sample_rate, " Hz, ", format_.period_frames,
           " frames/period, ", config_.periods, " periods, format ",
           config_.format);
  return true;
}

bool RavennaAudioBackend::open_stream(snd_pcm_stream_t direction,
                                      snd_pcm_t** handle, std::string* error) {
  const bool capture = direction == SND_PCM_STREAM_CAPTURE;
  const char* what = capture ? "capture" : "playback";

  int rc = snd_pcm_open(handle, config_.device.c_str(), direction, 0);
  if (rc < 0) {
    if (error) {
      *error = std::string("cannot open ") + what + " device '" + config_.device +
               "': " + snd_strerror(rc);
    }
    return false;
  }

  snd_pcm_hw_params_t* params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  if ((rc = snd_pcm_hw_params_any(*handle, params)) < 0) {
    if (error) {
      *error = std::string("cannot read hw params of '") + config_.device +
               "': " + snd_strerror(rc);
    }
    return false;
  }
  if ((rc = snd_pcm_hw_params_set_access(*handle, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    if (error) {
      *error = std::string("interleaved access not supported on '") +
               config_.device + "': " + snd_strerror(rc);
    }
    return false;
  }
  if ((rc = snd_pcm_hw_params_set_format(*handle, params, pcm_format_)) < 0) {
    if (error) {
      *error = std::string("sample format '") + config_.format +
               "' not supported on '" + config_.device + "': " + snd_strerror(rc);
    }
    return false;
  }
  unsigned channels = format_.channels;
  if ((rc = snd_pcm_hw_params_set_channels(*handle, params, channels)) < 0) {
    if (error) {
      *error = "cannot set " + std::to_string(channels) + " channels on '" +
               config_.device + "': " + snd_strerror(rc) +
               " (check the ravenna-alsa-lkm max_channels module parameter)";
    }
    return false;
  }

  unsigned rate = format_.sample_rate;
  int rate_direction = 0;
  if ((rc = snd_pcm_hw_params_set_rate_near(*handle, params, &rate,
                                            &rate_direction)) < 0) {
    if (error) {
      *error = std::string("cannot set ") + std::to_string(format_.sample_rate) +
               " Hz on '" + config_.device + "': " + snd_strerror(rc);
    }
    return false;
  }
  if (rate != format_.sample_rate) {
    LOG_WARN("device '", config_.device, "' runs at ", rate,
             " Hz instead of the requested ", format_.sample_rate,
             " Hz - the AES67 clock and the driver sample rate must match");
  }

  int period_direction = 0;
  snd_pcm_uframes_t period = format_.period_frames;
  if ((rc = snd_pcm_hw_params_set_period_size_near(*handle, params, &period,
                                                   &period_direction)) < 0) {
    if (error) {
      *error = std::string("cannot set period size on '") + config_.device +
               "': " + snd_strerror(rc);
    }
    return false;
  }
  // The device is clocked at 1 ms periods.  Keep at least 6 periods of buffer:
  // on kernels >= 6.15 the driver's tick is a soft hrtimer that delivers audio
  // in bursts, and a 3 ms buffer overruns as soon as the thread is delayed.
  snd_pcm_uframes_t buffer = period * std::max(6U, config_.periods);
  snd_pcm_hw_params_set_buffer_size_near(*handle, params, &buffer);

  if ((rc = snd_pcm_hw_params(*handle, params)) < 0) {
    if (error) {
      *error = std::string("cannot apply hw params on '") + config_.device +
               "': " + snd_strerror(rc);
    }
    return false;
  }
  if ((rc = snd_pcm_prepare(*handle)) < 0) {
    if (error) {
      *error = std::string("cannot prepare '") + config_.device +
               "': " + snd_strerror(rc);
    }
    return false;
  }

  snd_pcm_uframes_t period_frames = period;
  snd_pcm_uframes_t buffer_frames = buffer;
  snd_pcm_get_params(*handle, &buffer_frames, &period_frames);
  if (direction == SND_PCM_STREAM_PLAYBACK) {
    playback_period_frames_ = period_frames;
    playback_buffer_frames_ = buffer_frames;
  }
  if (!start_stream(*handle, error)) {
    return false;
  }

  LOG_DEBUG(what, " stream opened and started: period ", period_frames,
            " frames, buffer ", buffer_frames);
  return true;
}

bool RavennaAudioBackend::start_stream(snd_pcm_t* handle, std::string* error) {
  if (handle == nullptr) {
    return false;
  }
  const bool playback = snd_pcm_stream(handle) == SND_PCM_STREAM_PLAYBACK;

  // The RAVENNA driver only exchanges audio while a substream is *triggered*:
  // its ALSA trigger callback (start_interrupts -> startIO) marks the direction
  // as running, and the driver's 1 ms audio tick only copies frames between the
  // ALSA rings and the RTP streams when that flag is set (and PTP is locked).
  //
  // A prepared-but-never-started substream therefore leaves hw_ptr at 0 for
  // ever: no capture data reaches us, our sources transmit nothing, and the
  // audio the daemon's sinks receive is never played out.  Waiting for data
  // before reading (snd_pcm_wait()) cannot recover from that - the device
  // would have to be started first - so start both directions here, and again
  // after every recovery prepare below.
  if (playback) {
    // Prime the ring with silence so triggering it does not underrun on the
    // very first tick (an empty playback ring underruns immediately and the
    // driver stops the stream again).
    const snd_pcm_uframes_t prime = playback_buffer_frames_;
    if (prime > 0) {
      std::vector<uint8_t> silence(
          static_cast<size_t>(prime) * format_.channels * bytes_per_sample(), 0);
      snd_pcm_sframes_t written = snd_pcm_writei(handle, silence.data(), prime);
      if (written < 0) {
        LOG_DEBUG("playback prime write: ", snd_strerror(written),
                  " (continuing, the stream is started anyway)");
      }
    }
  }

  if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED) {
    const int rc = snd_pcm_start(handle);
    if (rc < 0) {
      if (error != nullptr) {
        *error = std::string("cannot start '") + config_.device + "' (" +
                 (playback ? "playback" : "capture") + "): " + snd_strerror(rc);
      }
      return false;
    }
  }
  return true;
}

void RavennaAudioBackend::close() {
  if (capture_ != nullptr) {
    snd_pcm_close(capture_);
    capture_ = nullptr;
  }
  if (playback_ != nullptr) {
    snd_pcm_close(playback_);
    playback_ = nullptr;
  }
  open_ = false;
}

void RavennaAudioBackend::fill_silence(float* destination, unsigned frames) const {
  std::fill(destination, destination + format_.frames_to_samples(frames), 0.0F);
}

size_t RavennaAudioBackend::to_float(const void* source, size_t samples,
                                     float* destination) const {
  switch (pcm_format_) {
    case SND_PCM_FORMAT_S16_LE: {
      const auto* data = static_cast<const int16_t*>(source);
      for (size_t i = 0; i < samples; ++i) {
        destination[i] = static_cast<float>(data[i]) / 32768.0F;
      }
      break;
    }
    case SND_PCM_FORMAT_S24_3LE: {
      const auto* data = static_cast<const uint8_t*>(source);
      for (size_t i = 0; i < samples; ++i) {
        destination[i] = convert_s24_3le(data + i * 3);
      }
      break;
    }
    default: {
      const auto* data = static_cast<const int32_t*>(source);
      for (size_t i = 0; i < samples; ++i) {
        destination[i] = static_cast<float>(data[i]) / 2147483648.0F;
      }
      break;
    }
  }
  return samples / format_.channels;
}

size_t RavennaAudioBackend::from_float(const float* source, size_t samples,
                                       void* destination) const {
  switch (pcm_format_) {
    case SND_PCM_FORMAT_S16_LE: {
      auto* data = static_cast<int16_t*>(destination);
      for (size_t i = 0; i < samples; ++i) {
        const float clamped = std::max(-1.0F, std::min(1.0F, source[i]));
        data[i] = static_cast<int16_t>(std::lrint(clamped * 32767.0F));
      }
      break;
    }
    case SND_PCM_FORMAT_S24_3LE: {
      auto* data = static_cast<uint8_t*>(destination);
      for (size_t i = 0; i < samples; ++i) {
        const float clamped = std::max(-1.0F, std::min(1.0F, source[i]));
        const int32_t value =
            static_cast<int32_t>(std::lrint(clamped * 8388607.0F));
        data[i * 3 + 0] = static_cast<uint8_t>(value & 0xFF);
        data[i * 3 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        data[i * 3 + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
      }
      break;
    }
    default: {
      auto* data = static_cast<int32_t*>(destination);
      for (size_t i = 0; i < samples; ++i) {
        const float clamped = std::max(-1.0F, std::min(1.0F, source[i]));
        data[i] = static_cast<int32_t>(std::lrint(clamped * 2147483647.0F));
      }
      break;
    }
  }
  return samples / format_.channels;
}

bool RavennaAudioBackend::read(float* destination, unsigned frames,
                               std::string* error) {
  if (!open_ || capture_ == nullptr) {
    if (error) {
      *error = "capture device is not open";
    }
    return false;
  }
  unsigned remaining = frames;
  float* output = destination;
  while (remaining > 0) {
    const int ready = snd_pcm_wait(capture_, kWaitTimeoutMs);
    if (ready < 0) {
      if (error) {
        *error = "capture wait failed: " + std::string(snd_strerror(ready));
      }
      fill_silence(destination, frames);
      return false;
    }
    if (ready == 0) {
      LOG_DEBUG("capture timed out after ", kWaitTimeoutMs,
                " ms (is the PTP clock locked?)");
      // A device that never produces anything is not just idle: the RAVENNA
      // engine has stopped (see recover_if_stalled).
      recover_if_stalled();
      fill_silence(destination, frames);
      return false;
    }
    const snd_pcm_sframes_t got =
        snd_pcm_readi(capture_, capture_raw_.data(), remaining);
    if (got == -EPIPE) {
      // Overrun: the device buffer was not drained in time.  Recover and keep
      // the 1 ms cadence by returning silence for this block - returning false
      // made the caller sleep up to 100 ms, which turned one overrun into a
      // cascade (and the error text was empty, so the log explained nothing).
      overruns_++;
      if (snd_pcm_prepare(capture_) < 0) {
        if (error) {
          *error = "capture overrun and prepare failed";
        }
        fill_silence(destination, frames);
        return false;
      }
      // prepare() leaves the stream in PREPARED, which stops the driver's tick
      // for this direction - trigger it again or the capture never resumes.
      std::string restart_error;
      if (!start_stream(capture_, &restart_error)) {
        LOG_WARN("cannot restart capture after an overrun: ", restart_error);
      }
      if (error) {
        *error = "capture overrun (recovered)";
      }
      fill_silence(destination, frames);
      return true;
    }
    if (got < 0) {
      if (error) {
        *error = "capture read failed: " + std::string(snd_strerror(got));
      }
      if (snd_pcm_recover(capture_, static_cast<int>(got), 1) < 0) {
        fill_silence(destination, frames);
        return false;
      }
      continue;
    }
    if (got == 0) {
      continue;
    }
    to_float(capture_raw_.data(), static_cast<size_t>(got) * format_.channels,
             output);
    output += static_cast<size_t>(got) * format_.channels;
    remaining -= static_cast<unsigned>(got);
    last_frames_at_ = monotonic_seconds();
  }
  return true;
}

void RavennaAudioBackend::recover_if_stalled() {
  if (!open_) {
    return;
  }
  const double now = monotonic_seconds();
  if (last_frames_at_ > 0.0 && now - last_frames_at_ < kStallSeconds) {
    return;
  }
  if (last_recover_at_ > 0.0 && now - last_recover_at_ < kRecoverIntervalSeconds) {
    return;
  }
  last_recover_at_ = now;

  // Reopening the PCM re-triggers both substreams, which is what makes the
  // driver's audio engine run again after the daemon restarted it underneath
  // us (the streams keep reporting RUNNING while nothing moves).
  LOG_WARN("no audio from '", config_.device, "' for ",
           static_cast<int>(now - last_frames_at_),
           " s: the RAVENNA engine looks stopped (a daemon restart does that) - "
           "reopening the device");
  const AudioFormat format = format_;
  close();
  std::string error;
  if (!open(format, &error)) {
    LOG_ERROR("cannot reopen '", config_.device, "': ", error);
  }
}

bool RavennaAudioBackend::write(const float* source, unsigned frames,
                                std::string* error) {
  if (!open_ || playback_ == nullptr) {
    if (error) {
      *error = "playback device is not open";
    }
    return false;
  }
  unsigned remaining = frames;
  const float* input = source;
  while (remaining > 0) {
    const int ready = snd_pcm_wait(playback_, kWaitTimeoutMs);
    if (ready < 0) {
      if (error) {
        *error = "playback wait failed: " + std::string(snd_strerror(ready));
      }
      return false;
    }
    if (ready == 0) {
      LOG_DEBUG("playback timed out after ", kWaitTimeoutMs,
                " ms (is the PTP clock locked?)");
      return false;
    }
    from_float(input, static_cast<size_t>(remaining) * format_.channels,
               playback_raw_.data());
    const snd_pcm_sframes_t written =
        snd_pcm_writei(playback_, playback_raw_.data(), remaining);
    if (written == -EPIPE) {
      // Underrun: nothing to send yet.  Recover and drop this block rather than
      // failing the whole write (see the matching comment in read()).  The
      // recovery prepare stops the driver's tick for the playback direction,
      // so re-prime the ring with silence and trigger it again.
      underruns_++;
      if (snd_pcm_prepare(playback_) < 0) {
        if (error) {
          *error = "playback underrun and prepare failed";
        }
        return false;
      }
      std::string restart_error;
      if (!start_stream(playback_, &restart_error)) {
        LOG_WARN("cannot restart playback after an underrun: ", restart_error);
      }
      if (error) {
        *error = "playback underrun (recovered)";
      }
      return true;
    }
    if (written < 0) {
      if (error) {
        *error = "playback write failed: " + std::string(snd_strerror(written));
      }
      if (snd_pcm_recover(playback_, static_cast<int>(written), 1) < 0) {
        return false;
      }
      continue;
    }
    if (written == 0) {
      continue;
    }
    input += static_cast<size_t>(written) * format_.channels;
    remaining -= static_cast<unsigned>(written);
  }
  return true;
}

std::string RavennaAudioBackend::detail() const {
  std::string out = config_.device + " " + std::to_string(format_.channels) +
                    "ch @" + std::to_string(format_.sample_rate) + "Hz " +
                    config_.format;
  if (open_ && capture_ != nullptr && playback_ != nullptr) {
    // Report the substream state: a PREPARED stream means the driver's audio
    // engine never started, which is silent (no capture, no RTP out) without
    // any error to see.
    out += std::string(", capture ") + snd_pcm_state_name(snd_pcm_state(capture_)) +
           ", playback " + snd_pcm_state_name(snd_pcm_state(playback_));
  }
  return out;
}

}  // namespace aes67sip
