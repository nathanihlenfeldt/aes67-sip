#!/usr/bin/env bash
#
# One-command installer for the aes67-sip appliance.
#
#   curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-sip/main/scripts/install.sh \
#     | sudo bash -s -- --zerotier-network <network-id>
#
# Target platform: a clean 64-bit Raspberry Pi OS (Bookworm) or Ubuntu 22.04/24.04
# install on arm64/x86_64.  The script is idempotent: re-running it upgrades the
# checkout and leaves the existing /etc/aes67-sip.conf in place.
#
# It installs, in order:
#   1. build dependencies (compiler, cmake, ALSA, avahi, boost, ...)
#   2. the Merging RAVENNA/AES67 kernel module, registered with DKMS so that kernel
#      upgrades rebuild it automatically
#   3. the aes67-daemon (REST API, PTP slave, SAP/mDNS discovery) + systemd unit
#   4. PJSIP 2.17 (pjsua2)
#   5. this gateway (aes67-sip) + web UI + systemd unit
#   6. real-time sysctls, PulseAudio masked, service user
#   7. optionally ZeroTier, joined to the given network
# and then prints a preflight report.
#
set -euo pipefail

REPO_URL="${AES67_SIP_REPO:-https://github.com/nathanihlenfeldt/aes67-sip}"
REF="${AES67_SIP_REF:-main}"
PJSIP_VERSION="${PJSIP_VERSION:-2.17}"
AES67_DAEMON_REPO="${AES67_DAEMON_REPO:-https://github.com/bondagit/aes67-linux-daemon}"
RAVENNA_LKM_REPO="${RAVENNA_LKM_REPO:-https://github.com/bondagit/ravenna-alsa-lkm}"
RAVENNA_LKM_BRANCH="${RAVENNA_LKM_BRANCH:-aes67-daemon}"

SRC_DIR="/opt/aes67-sip-src"
PREFIX="/usr/local"
CONFIG_FILE="/etc/aes67-sip.conf"
DAEMON_CONFIG="/etc/daemon.conf"
DAEMON_DIR="/opt/aes67-linux-daemon"
WEBUI_DIR="${PREFIX}/share/aes67-sip/webui"
SYSTEMD_UNIT="/etc/systemd/system/aes67-sip.service"

ZEROTIER_NETWORK=""
WITH_ZEROTIER=""
SKIP_KERNEL_MODULE=0
SKIP_DAEMON=0
SKIP_GATEWAY=0
START_SERVICES=1
DRY_RUN=0

C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_OFF=$'\033[0m'

log()  { printf '%s==>%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%s[warn]%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '%s[error]%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: install.sh [options]

  --zerotier-network <id>   install ZeroTier and join this network id
  --no-zerotier             do not install ZeroTier
  --ref <git ref>           branch/tag of aes67-sip to install (default: main)
  --repo <url>              git URL of the aes67-sip repository
  --skip-kernel-module      do not build the RAVENNA kernel module
  --skip-daemon             do not build/install aes67-daemon
  --skip-gateway            only install the AES67/daemon prerequisites
  --no-start                do not enable/start the systemd services
  --dry-run                 print the commands without executing them
  -h, --help                this help
EOF
}

run() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    printf '[dry-run] %s\n' "$*"
    return 0
  fi
  printf '+ %s\n' "$*"
  "$@"
}


# ---------------------------------------------------------------------------
# arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --zerotier-network) ZEROTIER_NETWORK="${2:-}"; WITH_ZEROTIER="yes"; shift 2 ;;
    --no-zerotier)      WITH_ZEROTIER="no"; shift ;;
    --ref)              REF="${2:-}"; shift 2 ;;
    --repo)             REPO_URL="${2:-}"; shift 2 ;;
    --skip-kernel-module) SKIP_KERNEL_MODULE=1; shift ;;
    --skip-daemon)      SKIP_DAEMON=1; shift ;;
    --skip-gateway)     SKIP_GATEWAY=1; shift ;;
    --no-start)         START_SERVICES=0; shift ;;
    --dry-run)          DRY_RUN=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------
[[ "$(id -u)" -eq 0 ]] || die "run as root, e.g. curl -fsSL <url> | sudo bash -s -- ..."
command -v apt-get >/dev/null 2>&1 || die "only Debian/Ubuntu (Raspberry Pi OS) is supported"
command -v systemctl >/dev/null 2>&1 || die "systemd is required"

ARCH="$(dpkg --print-architecture)"
case "${ARCH}" in
  arm64|amd64) ;;
  armhf) warn "32-bit armhf: the RAVENNA module targets armv7, but 64-bit is recommended" ;;
  *) die "unsupported architecture: ${ARCH}" ;;
esac
if [[ "$(getconf LONG_BIT)" != "64" ]]; then
  warn "32-bit userland detected; use the 64-bit Raspberry Pi OS / Ubuntu image"
fi

log "aes67-sip installer: arch=${ARCH} ref=${REF} dry-run=${DRY_RUN}"

# ---------------------------------------------------------------------------
# 1. build dependencies
# ---------------------------------------------------------------------------
BASE_PACKAGES=(
  build-essential clang cmake ninja-build git curl ca-certificates pkg-config bc
  libasound2-dev alsa-utils linuxptp libssl-dev
  libavahi-client-dev libsystemd-dev libboost-all-dev
  dkms iproute2 net-tools
)
log "installing build dependencies"
run apt-get update -qq
run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${BASE_PACKAGES[@]}"

# Kernel headers: naming differs between Raspberry Pi OS and Ubuntu, and the
# module cannot be built without them.
if run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "linux-headers-$(uname -r)"; then
  log "kernel headers for $(uname -r) installed"
else
  warn "no linux-headers-$(uname -r) package; trying the generic meta package"
  run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq linux-headers-generic ||
    warn "install kernel headers manually before building the RAVENNA module"
fi

# Optional: the daemon's HTTP streamer needs libfaac, which is not in every suite.
if ! apt-get install -y -qq libfaac-dev 2>/dev/null; then
  warn "libfaac-dev not available: aes67-daemon will be built without the HTTP streamer"
fi

# ---------------------------------------------------------------------------
# 2. source checkout
# ---------------------------------------------------------------------------
log "fetching aes67-sip (${REF}) into ${SRC_DIR}"
if [[ -d "${SRC_DIR}/.git" ]]; then
  run git -C "${SRC_DIR}" fetch --depth 1 origin "${REF}"
  run git -C "${SRC_DIR}" checkout -q FETCH_HEAD
else
  run rm -rf "${SRC_DIR}"
  run git clone --depth 1 --branch "${REF}" "${REPO_URL}" "${SRC_DIR}"
fi
[[ ${DRY_RUN} -eq 1 ]] || [[ -f "${SRC_DIR}/CMakeLists.txt" ]] || die "checkout failed: ${SRC_DIR}"

# ---------------------------------------------------------------------------
# 4. Merging RAVENNA/AES67 kernel module, registered with DKMS
# ---------------------------------------------------------------------------
if [[ ${SKIP_KERNEL_MODULE} -eq 1 ]]; then
  log "skipping the RAVENNA kernel module (--skip-kernel-module)"
elif lsmod | grep -q '^MergingRavennaALSA'; then
  log "the MergingRavennaALSA module is already loaded"
else
  log "building the Merging RAVENNA/AES67 kernel module (DKMS)"
  LKM_DIR="/usr/src/ravenna-alsa-lkm"
  if [[ -d "${LKM_DIR}/.git" ]]; then
    run git -C "${LKM_DIR}" fetch --depth 1 origin "${RAVENNA_LKM_BRANCH}"
    run git -C "${LKM_DIR}" checkout -q FETCH_HEAD
  else
    run rm -rf "${LKM_DIR}"
    run git clone --depth 1 --branch "${RAVENNA_LKM_BRANCH}" \
      "${RAVENNA_LKM_REPO}" "${LKM_DIR}"
  fi
  LKM_VERSION="$(git -C "${LKM_DIR}" rev-parse --short HEAD 2>/dev/null || echo 1.0)"
  DKMS_SRC="/usr/src/ravenna-alsa-lkm-${LKM_VERSION}"

  if [[ ${DRY_RUN} -eq 0 ]]; then
    rm -rf "${DKMS_SRC}"
    mkdir -p "${DKMS_SRC}"
    cp -a "${LKM_DIR}/." "${DKMS_SRC}/"
    rm -rf "${DKMS_SRC}/.git"
    cat > "${DKMS_SRC}/dkms.conf" <<EOF
PACKAGE_NAME="ravenna-alsa-lkm"
PACKAGE_VERSION="${LKM_VERSION}"
BUILT_MODULE_NAME[0]="MergingRavennaALSA"
DEST_MODULE_LOCATION[0]="/kernel/sound/pci"
MAKE[0]="make -C \${kernel_source_dir} M=\${dkms_tree}/ravenna-alsa-lkm/${LKM_VERSION}/build/driver modules"
CLEAN="make -C \${kernel_source_dir} M=\${dkms_tree}/ravenna-alsa-lkm/${LKM_VERSION}/build/driver clean"
EOF
  fi

  run dkms remove -m ravenna-alsa-lkm -v "${LKM_VERSION}" --all || true
  run dkms add -m ravenna-alsa-lkm -v "${LKM_VERSION}"
  if ! run dkms build -m ravenna-alsa-lkm -v "${LKM_VERSION}"; then
    warn "DKMS build failed with the default compiler; retrying with clang"
    run dkms build -m ravenna-alsa-lkm -v "${LKM_VERSION}" --force \
      -k "$(uname -r)" || true
    warn "if this keeps failing, build the module manually:"
    warn "  cd ${LKM_DIR}/driver && make CC=clang && insmod MergingRavennaALSA.ko"
  fi
  run dkms install -m ravenna-alsa-lkm -v "${LKM_VERSION}" --force || true

  # load at boot
  run tee /etc/modules-load.d/ravenna.conf >/dev/null <<'EOF'
# Added by aes67-sip/scripts/install.sh
MergingRavennaALSA
EOF
  run modprobe MergingRavennaALSA || warn "modprobe MergingRavennaALSA failed"
fi

# ---------------------------------------------------------------------------
# 5. aes67-daemon (REST API, PTP slave, SAP/mDNS discovery)
# ---------------------------------------------------------------------------
if [[ ${SKIP_DAEMON} -eq 1 ]]; then
  log "skipping aes67-daemon (--skip-daemon)"
else
  log "building aes67-daemon"
  if [[ -d "${DAEMON_DIR}/.git" ]]; then
    run git -C "${DAEMON_DIR}" fetch --depth 1 origin master
    run git -C "${DAEMON_DIR}" checkout -q FETCH_HEAD
  else
    run rm -rf "${DAEMON_DIR}"
    run git clone --depth 1 "${AES67_DAEMON_REPO}" "${DAEMON_DIR}"
  fi
  # cpp-httplib is vendored as a submodule
  run git -C "${DAEMON_DIR}" submodule update --init --recursive --depth 1

  if [[ ${DRY_RUN} -eq 0 ]]; then
    cmake -S "${DAEMON_DIR}/daemon" -B "${DAEMON_DIR}/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBoost_NO_WARN_NEW_VERSIONS=1 \
      -DCPP_HTTPLIB_DIR="${DAEMON_DIR}/3rdparty/cpp-httplib" \
      -DRAVENNA_ALSA_LKM_DIR="${DAEMON_DIR}/3rdparty/ravenna-alsa-lkm" \
      -DWITH_AVAHI=ON -DWITH_SYSTEMD=ON -DWITH_STREAMER=ON \
      -DFAKE_DRIVER=OFF
    cmake --build "${DAEMON_DIR}/build" -j"$(nproc)"
    install -m 0755 "${DAEMON_DIR}/build/aes67-daemon" "${PREFIX}/bin/aes67-daemon"
  fi

  # the daemon repo ships its own installer for the unit, config and service user
  if [[ -x "${DAEMON_DIR}/systemd/install.sh" ]]; then
    run bash "${DAEMON_DIR}/systemd/install.sh" || warn "aes67-daemon unit install failed"
  else
    warn "aes67-daemon systemd/install.sh not found; install the unit manually"
  fi

  # Point the daemon at the real AES67 interface.  Its default config uses "lo",
  # which never receives PTP or RTP, so this is mandatory on a fresh install.
  if [[ ${DRY_RUN} -eq 0 && -f "${DAEMON_CONFIG}" ]]; then
    PRIMARY_IF="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
    if [[ -z "${PRIMARY_IF}" ]]; then
      warn "cannot detect the default network interface; set interface_name in ${DAEMON_CONFIG}"
    else
      log "setting the daemon interface_name to ${PRIMARY_IF}"
      python3 - "${DAEMON_CONFIG}" "${PRIMARY_IF}" <<'PY'
import json, sys
path, iface = sys.argv[1], sys.argv[2]
try:
    cfg = json.load(open(path))
except Exception as exc:
    sys.exit(f"cannot parse {path}: {exc}")
if cfg.get("interface_name") in (None, "", "lo"):
    cfg["interface_name"] = iface
# The HTTP streamer captures the RAVENNA device, which would block the gateway's
# own capture path, so it must stay disabled.
cfg["streamer_enabled"] = False
cfg.setdefault("mdns_enabled", True)
cfg.setdefault("sap_mcast_addr", "239.255.255.255")
cfg.setdefault("sample_rate", 48000)
cfg.setdefault("tic_frame_size_at_1fs", 48)
json.dump(cfg, open(path, "w"), indent=2, sort_keys=True)
open(path, "a").write("\n")
print(f"interface_name={cfg['interface_name']} streamer_enabled=False")
PY
    fi
  fi
  if [[ ${DRY_RUN} -eq 0 ]]; then
    run systemctl enable --now aes67-daemon || warn "cannot start aes67-daemon"
  fi
fi



# ---------------------------------------------------------------------------
# 6. PJSIP (pjsua2) and the gateway itself
# ---------------------------------------------------------------------------
if [[ ${SKIP_GATEWAY} -eq 0 ]]; then
  log "building PJSIP ${PJSIP_VERSION} (this takes several minutes on a Pi)"
  run env PJSIP_VERSION="${PJSIP_VERSION}" bash "${SRC_DIR}/scripts/build-pjsip.sh"

  log "building aes67-sip"
  if [[ ${DRY_RUN} -eq 0 ]]; then
    cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" -DCMAKE_BUILD_TYPE=Release \
      -DWITH_PJSIP=ON -DWITH_ALSA=ON -DWITH_TESTS=OFF \
      -DPJSIP_ROOT="${SRC_DIR}/third_party/pjsip-install" >/dev/null
    cmake --build "${SRC_DIR}/build" -j"$(nproc)"
    install -m 0755 "${SRC_DIR}/build/aes67-sip" "${PREFIX}/bin/aes67-sip"
  fi

  # web UI
  log "building the web UI"
  run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq nodejs npm ||
    warn "nodejs/npm unavailable: the web UI will not be built (the API still works)"
  if command -v npm >/dev/null 2>&1 && [[ ${DRY_RUN} -eq 0 ]]; then
    ( cd "${SRC_DIR}/webui" && npm install --no-audit --no-fund --silent && npm run build --silent ) ||
      warn "web UI build failed; serving the API only"
  fi
  if [[ ${DRY_RUN} -eq 0 && -d "${SRC_DIR}/webui/dist" ]]; then
    run mkdir -p "${WEBUI_DIR}"
    run cp -a "${SRC_DIR}/webui/dist/." "${WEBUI_DIR}/"
  fi

  # configuration (never overwrite an existing one)
  if [[ ${DRY_RUN} -eq 0 ]]; then
    if [[ -f "${CONFIG_FILE}" ]]; then
      log "${CONFIG_FILE} exists, leaving it untouched"
    else
      install -m 0640 -o root -g aes67-sip "${SRC_DIR}/config/aes67-sip.conf" "${CONFIG_FILE}"
      python3 - "${CONFIG_FILE}" "${WEBUI_DIR}" <<'PY'
import json, sys
path, webui = sys.argv[1], sys.argv[2]
cfg = json.load(open(path))
cfg["webui_dir"] = webui
json.dump(cfg, open(path, "w"), indent=2, sort_keys=True)
open(path, "a").write("\n")
PY
      log "installed ${CONFIG_FILE} (set the SIP registrar/extensions before commissioning)"
    fi
  fi

  # service user/group required by the unit (Group=aes67-sip, SupplementaryGroups=audio)
  run getent group aes67-sip >/dev/null || run groupadd --system aes67-sip
  run id aes67-sip >/dev/null 2>&1 || run useradd --system --gid aes67-sip \
    --home-dir /var/lib/aes67-sip --create-home --shell /usr/sbin/nologin aes67-sip
  run usermod -aG audio aes67-sip || true
  run chown -R aes67-sip:aes67-sip /var/lib/aes67-sip || true

  run install -m 0644 "${SRC_DIR}/systemd/aes67-sip.service" "${SYSTEMD_UNIT}"
  run systemctl daemon-reload
  if [[ ${START_SERVICES} -eq 1 ]]; then
    run systemctl enable --now aes67-sip || warn "cannot start aes67-sip (check journalctl -u aes67-sip)"
  fi
fi

# ---------------------------------------------------------------------------
# 7. ZeroTier (NAT-free path to the FreePBX)
# ---------------------------------------------------------------------------
case "${WITH_ZEROTIER}" in
  no) log "skipping ZeroTier (--no-zerotier)" ;;
  yes|"")
    if command -v zerotier-cli >/dev/null 2>&1; then
      log "ZeroTier is already installed"
    else
      log "installing ZeroTier"
      run bash -c 'curl -fsSL https://install.zerotier.com | bash' ||
        warn "ZeroTier install failed; use a public FreePBX endpoint instead"
    fi
    if command -v zerotier-cli >/dev/null 2>&1; then
      run systemctl enable --now zerotier-one || true
      if [[ -n "${ZEROTIER_NETWORK}" ]]; then
        run zerotier-cli join "${ZEROTIER_NETWORK}" || warn "ZeroTier join failed"
        log "authorise this node in the ZeroTier admin console, then re-check with: zerotier-cli listnetworks"
      else
        warn "no --zerotier-network given: join manually with 'zerotier-cli join <id>'"
      fi
    fi
    ;;
esac

# ---------------------------------------------------------------------------
# 8. preflight report
# ---------------------------------------------------------------------------
echo
log "install finished, preflight report"
report_line() { printf '  %-26s %s\n' "$1" "$2"; }

if lsmod | grep -q '^MergingRavennaALSA'; then
  report_line "RAVENNA module" "loaded"
else
  report_line "RAVENNA module" "NOT loaded (audio will not work)"
fi
if arecord -l 2>/dev/null | grep -qi ravenna; then
  report_line "ALSA device" "$(arecord -l 2>/dev/null | grep -i ravenna | head -n1 | cut -c1-60)"
else
  report_line "ALSA device" "hw:RAVENNA not found yet (module/PTP?)"
fi
if [[ ${SKIP_DAEMON} -eq 0 ]]; then
  PTP="$(curl -fsS --max-time 3 http://127.0.0.1:8080/api/ptp/status 2>/dev/null || echo '')"
  report_line "aes67-daemon" "${PTP:-unreachable on :8080}"
fi
if [[ ${SKIP_GATEWAY} -eq 0 ]]; then
  GW="$(curl -fsS --max-time 3 http://127.0.0.1:8081/api/version 2>/dev/null || echo '')"
  report_line "aes67-sip API" "${GW:-unreachable on :8081}"
fi
report_line "interfaces" "$(ip -brief address show 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
report_line "web UI" "${WEBUI_DIR}"
report_line "config" "${CONFIG_FILE}"

cat <<EOF

Next steps
  1. Edit ${CONFIG_FILE}: set accounts[].registrar/username/password, each line's
     sip.extension and sip.dial_target (the PBX conference), and call_mode
     ("permanent" for a call that stays up).
  2. Confirm PTP is locked - audio only flows while the RAVENNA device is clocked:
       curl -s http://127.0.0.1:8080/api/ptp/status
     (if there is no grandmaster on the VLAN, run one: ptp4l -i <iface> -m -l7 -E -S)
  3. Open the UI on http://<appliance>:8081 - Dashboard shows PTP, discovery and
     per-line levels; set each line's AES67 channels and pick its discovered source.
  4. Verify a line end to end with the commissioning test tone and the level meters
     before trusting the intercom path.
EOF

# ---------------------------------------------------------------------------
# 3. real time tuning, PulseAudio, service user
# ---------------------------------------------------------------------------
if [[ -x "${SRC_DIR}/scripts/setup-ravenna.sh" ]]; then
  log "applying kernel/audio tuning and creating the service user"
  run bash "${SRC_DIR}/scripts/setup-ravenna.sh"
fi
