#pragma once

#include <string>

namespace aes67sip {

/** Gateway version string (from CMake project version). */
std::string version();

/** Human readable build info, e.g. "pjsip=1 alsa=1". */
std::string build_info();

}  // namespace aes67sip
