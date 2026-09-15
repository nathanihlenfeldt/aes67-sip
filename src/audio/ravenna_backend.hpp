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
 * The device only produces/consumes samples once the PTP slave is locked, so
 * `read()` returns silence (rather than failing) while it is not.
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

  std::atomic<unsigned> overruns_{0};
  std::atomic<unsigned> underruns_{0};
  bool open_{false};
};

}  // namespace aes67sip
