#include "audio/backend.hpp"

#include <algorithm>

#include "audio/null_backend.hpp"
#include "log.hpp"

#ifdef WITH_ALSA
#include "audio/ravenna_backend.hpp"
#endif

namespace aes67sip {

bool ravenna_backend_available() {
#ifdef WITH_ALSA
  return true;
#else
  return false;
#endif
}

std::unique_ptr<AudioBackend> create_audio_backend(const AudioConfig& config) {
  switch (config.backend_kind()) {
    case AudioBackendKind::kNull:
      return std::make_unique<NullAudioBackend>(config.null_tone_hz);
    case AudioBackendKind::kRavenna:
      break;
  }

#ifdef WITH_ALSA
  return std::make_unique<RavennaAudioBackend>(config);
#else
  LOG_WARN(
      "Ravenna audio backend requested but this build has no ALSA support; "
      "falling back to the null backend");
  return std::make_unique<NullAudioBackend>(config.null_tone_hz);
#endif
}

}  // namespace aes67sip
