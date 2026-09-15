#!/usr/bin/env bash
#
# Installs the build dependencies of aes67-sip on Debian/Ubuntu (the supported
# target platforms, including SBCs such as Raspberry Pi OS / Ubuntu arm64).
#
# Usage:  ./scripts/install-deps.sh          (uses sudo when needed)
#         sudo ./scripts/install-deps.sh     (already root, e.g. in CI)
#
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "error: this script supports Debian/Ubuntu only" >&2
  exit 1
fi

PACKAGES=(
  build-essential
  clang
  cmake
  ninja-build
  git
  curl
  pkg-config
  # audio backend (RAVENNA/ALSA device)
  libasound2-dev
  alsa-utils
  # PJSIP: TLS/SRTP and PTP tooling
  libssl-dev
  linuxptp
  # aes67-daemon integration (mDNS discovery)
  libavahi-client-dev
  # tools used by the daemon's own scripts and diagnostics
  net-tools
  iproute2
  usbutils
)

echo "==> installing build dependencies: ${PACKAGES[*]}"
$SUDO apt-get update -qq
$SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${PACKAGES[@]}"

# Kernel headers are needed to build the Merging RAVENNA/AES67 LKM on site.
if [[ -n "${INSTALL_KERNEL_HEADERS:-}" ]]; then
  $SUDO apt-get install -y -qq "linux-headers-$(uname -r)"
fi

echo "==> done. Next: ./scripts/build-pjsip.sh"
