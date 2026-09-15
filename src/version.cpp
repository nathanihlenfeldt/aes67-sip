#include "version.hpp"

#ifndef AES67_SIP_VERSION
#define AES67_SIP_VERSION "0.0.0-dev"
#endif

namespace aes67sip {

std::string version() {
  return AES67_SIP_VERSION;
}

std::string build_info() {
  std::string info;
#ifdef WITH_PJSIP
  info += "pjsip=1";
#else
  info += "pjsip=0";
#endif
#ifdef WITH_ALSA
  info += " alsa=1";
#else
  info += " alsa=0";
#endif
  return info;
}

}  // namespace aes67sip
