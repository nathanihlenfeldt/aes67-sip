#pragma once

#include <memory>
#include <string>

#include "config.hpp"

namespace aes67sip {

/**
 * Audio format of the AES67 audio path.  Everything between the backend and the
 * SIP ports is float32 interleaved at this sample rate; the backend converts to
 * and from the hardware PCM format.
 */
struct AudioFormat {
  unsigned sample_rate{48000};
  unsigned channels{16};
  unsigned period_frames{48};

  size_t frames_to_samples(unsigned frames) const {
    return static_cast<size_t>(frames) * channels;
  }
};

/**
 * Abstraction over the AES67 audio source/sink.
 *
 * `read()` returns audio *from* the AES67 endpoints (the daemon's sinks map onto
 * the RAVENNA capture substream) and `write()` sends audio *to* the AES67
 * endpoints (the daemon's sources map onto the RAVENNA playback substream).
 *
 * Implementations must be safe to call from a single audio thread only.
 */
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  /** Opens the device and configures it for `format`. */
  virtual bool open(const AudioFormat& format, std::string* error) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  /** Reads exactly `frames` interleaved frames; pads with silence on underrun. */
  virtual bool read(float* destination, unsigned frames, std::string* error) = 0;

  /** Writes exactly `frames` interleaved frames. */
  virtual bool write(const float* source, unsigned frames, std::string* error) = 0;

  /** Backend type, e.g. "ravenna" or "null". */
  virtual std::string kind() const = 0;

  /** Short human readable description for the UI, e.g. "plughw:RAVENNA 16ch". */
  virtual std::string detail() const = 0;

  virtual const AudioFormat& format() const = 0;

  /** Number of capture xruns since the device was opened. */
  virtual unsigned overruns() const = 0;

  /** Number of playback xruns since the device was opened. */
  virtual unsigned underruns() const = 0;
};

/**
 * Creates the backend selected by `config.audio.backend`.  When the RAVENNA
 * backend is requested but the gateway was built without ALSA support (or the
 * device cannot be opened by `open()`), an error is returned by `open()`.
 */
std::unique_ptr<AudioBackend> create_audio_backend(const AudioConfig& config);

/** True when this build contains the RAVENNA/ALSA backend. */
bool ravenna_backend_available();

}  // namespace aes67sip
