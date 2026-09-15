# FindPJSIP.cmake
#
# Locates an already built PJSIP (pjsua2) installation.
#
# pjproject does not install a pkg-config file or a CMake package config, so we
# look for the headers and libraries directly.  Point PJSIP_ROOT at the install
# prefix produced by scripts/build-pjsip.sh (default: third_party/pjsip-install).
#
#   PJSIP_ROOT        - install prefix (cache variable, optional)
#   PJSIP_INCLUDE_DIR - directory containing pjsua2.hpp (and pj/ subdir)
#   PJSUA2_LIBRARY    - path to libpjsua2
#   PJSUA_LIBRARY     - path to libpjsua
#   PJMEDIA_LIBRARY   - path to libpjmedia
#   PJ_LIBRARY        - path to libpj
#   PJSIP_FOUND       - true when all of the above were resolved

include(FindPackageHandleStandardArgs)

set(PJSIP_ROOT "" CACHE PATH "PJSIP (pjproject) installation prefix")

set(_pjsip_hints ${PJSIP_ROOT})
if(PJSIP_ROOT)
  list(APPEND _pjsip_hints
       ${PJSIP_ROOT}/include
       ${PJSIP_ROOT}/lib
       ${PJSIP_ROOT}/lib64)
endif()

find_path(PJSIP_INCLUDE_DIR
  NAMES pjsua2.hpp
  HINTS ${_pjsip_hints}
  PATH_SUFFIXES include
  DOC "PJSIP include directory")

find_library(PJSUA2_LIBRARY NAMES pjsua2 HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
find_library(PJSUA_LIBRARY NAMES pjsua HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
find_library(PJMEDIA_LIBRARY NAMES pjmedia HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
find_library(PJ_LIBRARY NAMES pj HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)

find_package_handle_standard_args(PJSIP
  REQUIRED_VARS
    PJSIP_INCLUDE_DIR
    PJSUA2_LIBRARY
    PJSUA_LIBRARY
    PJMEDIA_LIBRARY
    PJ_LIBRARY
  VERSION_VAR PJSIP_VERSION)

if(PJSIP_FOUND)
  # pjproject installs its public headers both directly and under a pj/ prefix
  # directory, so both include paths are required.
  set(PJSIP_INCLUDE_DIRS ${PJSIP_INCLUDE_DIR})
  if(EXISTS "${PJSIP_INCLUDE_DIR}/pj")
    list(APPEND PJSIP_INCLUDE_DIRS "${PJSIP_INCLUDE_DIR}/pj")
  endif()

  set(PJSIP_LIBRARIES
      ${PJSUA2_LIBRARY}
      ${PJSUA_LIBRARY}
      ${PJMEDIA_LIBRARY}
      ${PJ_LIBRARY})

  if(NOT TARGET PJSIP::PJSUA2)
    add_library(PJSIP::PJSUA2 UNKNOWN IMPORTED)
    set_target_properties(PJSIP::PJSUA2 PROPERTIES
      IMPORTED_LOCATION "${PJSUA2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PJSIP_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${PJSIP_LIBRARIES}")
  endif()
endif()

mark_as_advanced(PJSIP_INCLUDE_DIR PJSUA2_LIBRARY PJSUA_LIBRARY PJMEDIA_LIBRARY PJ_LIBRARY)
