cmake_minimum_required(VERSION 3.16)

project(aes67-sip
  VERSION 0.1.0
  DESCRIPTION "AES67 <-> SIP intercom gateway"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
#
# All hardware / platform specific pieces default to AUTO so that the project
# can be configured and unit tested on a development machine (e.g. macOS)
# without ALSA, the RAVENNA kernel module or PJSIP installed.
# ---------------------------------------------------------------------------
option(WITH_PJSIP "Build against PJSIP/pjsua2 (AUTO = when found)" AUTO)
option(WITH_ALSA "Build the RAVENNA/ALSA audio backend (AUTO = when found)" AUTO)
option(WITH_TESTS "Build unit tests" ON)
option(WITH_WEBUI "Build the React web UI (requires npm)" OFF)
option(FETCH_DEPS "Download missing dependencies with FetchContent" ON)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# --- nlohmann/json (JSON config file, REST payloads) -----------------------
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found and FETCH_DEPS=OFF")
  endif()
  message(STATUS "nlohmann_json not found, fetching v3.12.0")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- cpp-httplib (REST server for the local API, client for the daemon) -----
find_package(httplib QUIET)
if(NOT httplib_FOUND)
  if(NOT FETCH_DEPS)
    message(FATAL_ERROR "cpp-httplib not found and FETCH_DEPS=OFF")
  endif()
  message(STATUS "cpp-httplib not found, fetching v0.56.0")
  set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.56.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(httplib)
endif()

find_package(Threads REQUIRED)


# --- PJSIP / pjsua2 --------------------------------------------------------
set(PJSIP_AVAILABLE FALSE)
if(NOT WITH_PJSIP STREQUAL "OFF")
  find_package(PJSIP QUIET)
  if(PJSIP_FOUND)
    set(PJSIP_AVAILABLE TRUE)
    message(STATUS "PJSIP found: ${PJSUA2_LIBRARY}")
  elseif(WITH_PJSIP STREQUAL "ON")
    message(FATAL_ERROR
      "WITH_PJSIP=ON but PJSIP was not found. "
      "Run scripts/build-pjsip.sh and pass -DPJSIP_ROOT=<prefix>")
  else()
    message(WARNING
      "PJSIP not found - building with the stub SIP engine "
      "(run scripts/build-pjsip.sh for real SIP support)")
  endif()
endif()

# --- ALSA ------------------------------------------------------------------
set(ALSA_AVAILABLE FALSE)
if(NOT WITH_ALSA STREQUAL "OFF")
  find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
  find_library(ALSA_LIBRARY NAMES asound)
  if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
    set(ALSA_AVAILABLE TRUE)
    message(STATUS "ALSA found: ${ALSA_LIBRARY}")
  elseif(WITH_ALSA STREQUAL "ON")
    message(FATAL_ERROR "WITH_ALSA=ON but libasound headers/library were not found")
  else()
    message(WARNING
      "ALSA not found - building with the null audio backend "
      "(install libasound2-dev for the RAVENNA backend)")
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(AES67_SIP_SOURCES
  src/version.cpp
  src/log.cpp
  src/util.cpp
  src/config.cpp
  src/app.cpp
  src/audio/backend.cpp
  src/audio/null_backend.cpp
  src/audio/resampler.cpp
  src/audio/router.cpp
  src/aes67/daemon_client.cpp
  src/sip/engine.cpp
  src/bridge/line_manager.cpp
  src/http/api_server.cpp)

if(ALSA_AVAILABLE)
  list(APPEND AES67_SIP_SOURCES src/audio/ravenna_backend.cpp)
endif()

if(PJSIP_AVAILABLE)
  list(APPEND AES67_SIP_SOURCES
    src/sip/pjsip_engine.cpp
    src/sip/pjsip_audio_port.cpp)
endif()

add_library(aes67-sip-core STATIC ${AES67_SIP_SOURCES})

target_include_directories(aes67-sip-core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${ALSA_INCLUDE_DIR})

target_link_libraries(aes67-sip-core
  PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
    Threads::Threads)

target_compile_definitions(aes67-sip-core PUBLIC
  AES67_SIP_VERSION="${PROJECT_VERSION}")

if(PJSIP_AVAILABLE)
  target_link_libraries(aes67-sip-core PRIVATE ${PJSIP_LIBRARIES})
  target_include_directories(aes67-sip-core PRIVATE ${PJSIP_INCLUDE_DIRS})
  target_compile_definitions(aes67-sip-core PUBLIC WITH_PJSIP=1)
  if(APPLE)
    # pjproject 2.17's pj/compat/m_auto.h does not detect the endianness of
    # clang on Apple Silicon, so pj/config.h aborts with "Endianness must be
    # declared for this processor".  All supported Apple CPUs are little endian.
    target_compile_definitions(aes67-sip-core PRIVATE
      PJ_IS_LITTLE_ENDIAN=1 PJ_IS_BIG_ENDIAN=0)
    # The macOS pjproject binaries expect the system audio/network frameworks.
    target_link_libraries(aes67-sip-core PRIVATE
      "-framework CoreAudio"
      "-framework AudioToolbox"
      "-framework AudioUnit"
      "-framework CoreFoundation"
      "-framework CFNetwork"
      "-framework Security"
      "-framework SystemConfiguration")
  endif()
endif()

if(ALSA_AVAILABLE)
  target_link_libraries(aes67-sip-core PRIVATE ${ALSA_LIBRARY})
  target_compile_definitions(aes67-sip-core PUBLIC WITH_ALSA=1)
endif()

target_compile_options(aes67-sip-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Daemon executable
# ---------------------------------------------------------------------------
add_executable(aes67-sip src/main.cpp)
target_link_libraries(aes67-sip PRIVATE aes67-sip-core)
target_compile_options(aes67-sip PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(WITH_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Web UI
# ---------------------------------------------------------------------------
if(WITH_WEBUI)
  find_program(NPM_EXECUTABLE npm)
  if(NOT NPM_EXECUTABLE)
    message(FATAL_ERROR "WITH_WEBUI=ON but npm was not found")
  endif()
  add_custom_target(webui
    COMMAND ${NPM_EXECUTABLE} install
    COMMAND ${NPM_EXECUTABLE} run build
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/webui
    COMMENT "Building web UI")
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "aes67-sip ${PROJECT_VERSION} configuration")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  PJSIP/pjsua2 : ${PJSIP_AVAILABLE}")
message(STATUS "  ALSA/RAVENNA : ${ALSA_AVAILABLE}")
message(STATUS "  tests        : ${WITH_TESTS}")
message(STATUS "")
