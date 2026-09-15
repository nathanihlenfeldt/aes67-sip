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
find_library(PJSIP_LIBRARY NAMES pjsip HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
find_library(PJNATH_LIBRARY NAMES pjnath HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
find_library(PJ_LIBRARY NAMES pj HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)

# Some builds (Homebrew, cross builds) install the static archives with a
# version/target suffix, e.g. libpjsua2-aarch64-apple-darwin24.0.0.a, which the
# plain `NAMES pjsua2` lookup above cannot match.  Fall back to a glob.
set(_pjsip_lib_dirs ${_pjsip_hints} ${PJSIP_INCLUDE_DIR})
foreach(_pair "PJSUA2_LIBRARY;pjsua2" "PJSUA_LIBRARY;pjsua"
              "PJMEDIA_LIBRARY;pjmedia" "PJSIP_LIBRARY;pjsip"
              "PJNATH_LIBRARY;pjnath" "PJ_LIBRARY;pj")
  list(GET _pair 0 _var)
  list(GET _pair 1 _base)
  if(NOT ${_var})
    foreach(_dir ${_pjsip_lib_dirs})
      file(GLOB _candidates
        "${_dir}/lib${_base}.*"
        "${_dir}/lib${_base}-*"
        "${_dir}/lib${_base}.so*"
        "${_dir}/lib${_base}.dylib"
        "${_dir}/lib${_base}.a")
      if(_candidates)
        list(SORT _candidates)
        list(GET _candidates 0 _first)
        set(${_var} "${_first}")
        break()
      endif()
    endforeach()
  endif()
endforeach()
unset(_candidates)
unset(_first)
unset(_pair)

find_package_handle_standard_args(PJSIP
  REQUIRED_VARS
    PJSIP_INCLUDE_DIR
    PJSUA2_LIBRARY
    PJSUA_LIBRARY
    PJMEDIA_LIBRARY
    PJSIP_LIBRARY
    PJ_LIBRARY
  VERSION_VAR PJSIP_VERSION)

if(PJSIP_FOUND)
  # NOTE: only the include *root* may be on the search path.  pjproject also ships
  # pj/string.h, pj/limits.h and pj/errno.h; adding <root>/pj as an include
  # directory makes the system <string.h>/<limits.h>/<errno.h> resolve to those
  # files and breaks every translation unit with "Endianness must be declared".
  set(PJSIP_INCLUDE_DIRS ${PJSIP_INCLUDE_DIR})

  # Prefer pkg-config when pjproject provides it (complete link line).
  find_package(PkgConfig QUIET)
  set(_pjsip_link_libs "")
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_PJSIP QUIET libpjproject)
    if(PC_PJSIP_FOUND)
      # _LINK_LIBRARIES holds absolute paths, which is what a static pjproject
      # build with version-suffixed archives needs (libpjsua2-<triple>.a).
      set(_pjsip_link_libs ${PC_PJSIP_LINK_LIBRARIES})
      if(NOT _pjsip_link_libs)
        set(_pjsip_link_libs ${PC_PJSIP_LIBRARIES})
      endif()
      set(PJSIP_LINK_DIRS ${PC_PJSIP_LIBRARY_DIRS})
      set(PJSIP_INCLUDE_DIRS ${PC_PJSIP_INCLUDE_DIRS} ${PJSIP_INCLUDE_DIRS})
    endif()
  endif()

  if(NOT _pjsip_link_libs)
    # Classic pjproject link order: pjsua2 -> pjsua -> pjmedia -> pjsip -> pjnath -> pj
    set(_pjsip_link_libs
        ${PJSUA2_LIBRARY}
        ${PJSUA_LIBRARY}
        ${PJMEDIA_LIBRARY}
        ${PJSIP_LIBRARY}
        ${PJNATH_LIBRARY}
        ${PJ_LIBRARY})
  endif()

  # Codec / helper archives are appended for both paths: pkg-config files
  # frequently omit some of them (Homebrew's libpjproject.pc leaves out
  # libwebrtc, which libpjmedia's echo canceller needs).
  foreach(_extra pjmedia-codec gsmcodec speex ilbccodec g7221codec resample srtp
                 yuv webrtc)
    find_library(PJSIP_EXTRA_${_extra} NAMES ${_extra}
                 HINTS ${_pjsip_hints} PATH_SUFFIXES lib lib64)
    if(NOT PJSIP_EXTRA_${_extra})
      # Homebrew/cross builds use version-suffixed static archives
      foreach(_dir ${_pjsip_lib_dirs})
        file(GLOB _extra_candidates "${_dir}/lib${_extra}-*" "${_dir}/lib${_extra}.*")
        if(_extra_candidates)
          list(SORT _extra_candidates)
          list(GET _extra_candidates 0 PJSIP_EXTRA_${_extra})
          break()
        endif()
      endforeach()
    endif()
    if(PJSIP_EXTRA_${_extra})
      list(APPEND _pjsip_link_libs ${PJSIP_EXTRA_${_extra}})
    endif()
  endforeach()
  unset(_extra_candidates)

  # system libraries a static pjproject build needs
  foreach(_sys m dl)
    find_library(PJSIP_SYS_${_sys} NAMES ${_sys})
    if(PJSIP_SYS_${_sys})
      list(APPEND _pjsip_link_libs ${PJSIP_SYS_${_sys}})
    endif()
  endforeach()

  set(PJSIP_LIBRARIES ${_pjsip_link_libs})

  if(NOT TARGET PJSIP::PJSUA2)
    add_library(PJSIP::PJSUA2 UNKNOWN IMPORTED)
    set_target_properties(PJSIP::PJSUA2 PROPERTIES
      IMPORTED_LOCATION "${PJSUA2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PJSIP_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${PJSIP_LIBRARIES}"
      INTERFACE_LINK_DIRECTORIES "${PJSIP_LINK_DIRS}")
  endif()
endif()

mark_as_advanced(PJSIP_INCLUDE_DIR PJSUA2_LIBRARY PJSUA_LIBRARY PJMEDIA_LIBRARY
                 PJSIP_LIBRARY PJNATH_LIBRARY PJ_LIBRARY)

unset(_pjsip_lib_dirs)
