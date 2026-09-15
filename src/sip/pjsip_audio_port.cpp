#include "sip/pjsip_audio_port.hpp"

#include <algorithm>

#include "log.hpp"

namespace aes67sip {

PjsipAudioPort::PjsipAudioPort(int line_id, SipMediaSource* media)
    : line_id_(line_id), media_(media) {}

void PjsipAudioPort::open(const std::string& name, unsigned rate,
                          unsigned ptime_ms) {
  if (rate == 0) {
    rate = 8000;
  }
  rate_ = rate;

  pj::MediaFormatAudio format;
  format.init(PJMEDIA_FORMAT_PCM, rate, 1,
              static_cast<int>(ptime_ms == 0 ? 20 : ptime_ms), 16);
  format.clockRate = rate;
  format.channelCount = 1;
  format.frameTimeUsec = (ptime_ms == 0 ? 20 : ptime_ms) * 1000;
  format.bitsPerSample = 16;

  createPort(name, format);
  opened_ = true;
  LOG_DEBUG("PJSIP audio port '", name, "' created at ", rate, " Hz");
}

void PjsipAudioPort::shutdown() {
  if (!opened_) {
    return;
  }
  opened_ = false;
  try {
    unregisterMediaPort();
  } catch (const pj::Error& error) {
    LOG_WARN("cannot unregister the PJSIP audio port of line ", line_id_, ": ",
             error.info());
  }
}

void PjsipAudioPort::onFrameRequested(pj::MediaFrame& frame) {
  // The engine asks for `frame.size` bytes of mono 16 bit PCM to send to the
  // remote party; pull them from the router (which pads with silence).
  const size_t samples = frame.size / sizeof(int16_t);
  if (frame.buf.size() < frame.size) {
    frame.buf.resize(frame.size);
  }
  frame.type = PJMEDIA_FRAME_TYPE_AUDIO;

  if (media_ == nullptr || samples == 0) {
    std::fill(frame.buf.begin(), frame.buf.end(), 0);
    return;
  }

  std::vector<float> floats(samples, 0.0F);
  media_->pull_to_sip(line_id_, floats.data(), samples);

  auto* output = reinterpret_cast<int16_t*>(frame.buf.data());
  for (size_t i = 0; i < samples; ++i) {
    const float clamped = std::clamp(floats[i], -1.0F, 1.0F);
    output[i] = static_cast<int16_t>(clamped * 32767.0F);
  }
}

void PjsipAudioPort::onFrameReceived(pj::MediaFrame& frame) {
  if (media_ == nullptr || frame.size == 0) {
    return;
  }
  const size_t samples = frame.size / sizeof(int16_t);
  if (samples == 0) {
    return;
  }
  const auto* input = reinterpret_cast<const int16_t*>(frame.buf.data());
  std::vector<float> floats(samples, 0.0F);
  for (size_t i = 0; i < samples; ++i) {
    floats[i] = static_cast<float>(input[i]) / 32768.0F;
  }
  media_->push_from_sip(line_id_, floats.data(), samples);
}

}  // namespace aes67sip
