#!/usr/bin/env bash
#
# Builds PJSIP (pjproject) with the options this gateway needs and installs it
# into third_party/pjsip-install (override with PJSIP_PREFIX).
#
# The result is cached by CI, and CMake picks it up with -DPJSIP_ROOT=<prefix>.
#
# Usage:  ./scripts/build-pjsip.sh [--force]
#
set -euo pipefail

PJSIP_VERSION="${PJSIP_VERSION:-2.17}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PJSIP_PREFIX="${PJSIP_PREFIX:-${REPO_ROOT}/third_party/pjsip-install}"
PJSIP_SRC="${PJSIP_SRC:-${REPO_ROOT}/third_party/pjproject}"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

if [[ ${FORCE} -eq 0 ]] && compgen -G "${PJSIP_PREFIX}/lib/libpjsua2*" >/dev/null; then
  echo "==> PJSIP already installed in ${PJSIP_PREFIX} (use --force to rebuild)"
  exit 0
fi

mkdir -p "${REPO_ROOT}/third_party"

if [[ ! -d "${PJSIP_SRC}/.git" ]]; then
  echo "==> cloning pjproject ${PJSIP_VERSION}"
  rm -rf "${PJSIP_SRC}"
  git clone --depth 1 --branch "${PJSIP_VERSION}" \
    https://github.com/pjsip/pjproject.git "${PJSIP_SRC}"
fi

cd "${PJSIP_SRC}"

# Audio-only build: the gateway owns the audio path through the RAVENNA ALSA
# device and PJSIP is used for signalling, RTP, SRTP and TLS.
if [[ ! -f config.status ]]; then
  echo "==> configuring pjproject (prefix ${PJSIP_PREFIX})"
  ./configure \
    --prefix="${PJSIP_PREFIX}" \
    --enable-shared \
    --disable-video \
    --disable-libwebrtc \
    --disable-opencore-amr \
    --disable-silk \
    --disable-speex-codec \
    --disable-speex-aec \
    --disable-ilbc-codec \
    --disable-libyuv \
    --disable-openh264 \
    --disable-sdl \
    --disable-v4l2 \
    --disable-ffmpeg
fi

echo "==> building PJSIP (this takes a few minutes)"
make -j"$(nproc)" dep
make -j"$(nproc)"
make install

echo "==> PJSIP ${PJSIP_VERSION} installed in ${PJSIP_PREFIX}"
ls "${PJSIP_PREFIX}/lib" | grep -E 'pjsua2|pjsua|pjmedia|libpj\.' || true
