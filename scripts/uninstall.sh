#!/usr/bin/env bash
#
# Removes the aes67-sip appliance again - the reverse of scripts/install.sh and
# scripts/setup-ravenna.sh - and leaves the machine as the distribution shipped
# it, with one deliberate exception: **ZeroTier is never touched**.  The package,
# its service, /var/lib/zerotier-one and the joined network stay exactly as they
# are, because that is the NAT-free path to the FreePBX: the appliance can be
# rebuilt on site without asking the network controller for a new membership.
#
#   sudo ./scripts/uninstall.sh              # shows a plan, asks, then removes
#   sudo ./scripts/uninstall.sh --dry-run    # prints the commands, changes nothing
#   sudo ./scripts/uninstall.sh --yes        # unattended
#
# What it removes:
#   * units          aes67-sip, aes67-daemon, aes67-cpu-governor
#   * gateway        /usr/local/bin/aes67-sip, web UI, /etc/aes67-sip.conf,
#                    /var/lib/aes67-sip, /var/log/aes67-sip
#   * daemon         /usr/local/bin/aes67-daemon, web UI + scripts,
#                    /etc/daemon.conf, /etc/status.json, /var/lib/aes67-daemon
#   * PJSIP          /usr/local/lib/libpj*.so*, /etc/ld.so.conf.d/aes67-sip.conf
#   * RAVENNA module unloaded, DKMS entry, /usr/src/ravenna-alsa-lkm*,
#                    /etc/modules-load.d/ravenna.conf
#   * kernel tuning  /etc/sysctl.d/90-aes67.conf (values reset), the CPU
#                    governor unit, the governor itself
#   * users          aes67-sip (with its home), aes67-daemon, group aes67-sip
#   * source trees   /opt/aes67-sip-src, /opt/aes67-linux-daemon
#
# It keeps: ZeroTier, the kernel headers, the build dependencies (unless
# --purge-deps) and anything else on the machine.
#
set -euo pipefail

SRC_DIR="/opt/aes67-sip-src"
DAEMON_DIR="/opt/aes67-linux-daemon"
PREFIX="/usr/local"
CONFIG_FILE="/etc/aes67-sip.conf"
DAEMON_CONFIG="/etc/daemon.conf"
DAEMON_STATUS="/etc/status.json"
WEBUI_DIR="${PREFIX}/share/aes67-sip"
DAEMON_SHARE="${PREFIX}/share/aes67-daemon"
SYSTEMD_DIR="/etc/systemd/system"
LD_CONF="/etc/ld.so.conf.d/aes67-sip.conf"
GOVERNOR_UNIT="${SYSTEMD_DIR}/aes67-cpu-governor.service"
SYSCTL_FILE="/etc/sysctl.d/90-aes67.conf"
MODULES_LOAD="/etc/modules-load.d/ravenna.conf"
DKMS_PACKAGE="ravenna-alsa-lkm"
MODULE_NAME="MergingRavennaALSA"

# The kernel parameters setup-ravenna.sh raised, and the kernel's own defaults.
# A value is only put back when it still carries our number, so a distribution
# file (or an operator's own tuning) always wins.
SYSCTL_KEYS=(net.ipv4.igmp_max_memberships kernel.sched_rt_runtime_us
             kernel.perf_cpu_time_max_percent)
SYSCTL_OURS=(66 1000000 0)
SYSCTL_DEFAULTS=(20 950000 25)

# Packages install.sh added that nothing else on a stock Raspberry Pi OS /
# Ubuntu needs.  Deliberately *not* listed: build-essential, cmake, clang, git,
# curl, python3, alsa-utils, net-tools, iproute2 and bc, which are broadly
# useful and usually part of the base image already.
PURGE_PACKAGES=(
  dkms linuxptp libasound2-dev libssl-dev libavahi-client-dev libsystemd-dev
  libboost-all-dev libfaac-dev ninja-build pkg-config
)

DRY_RUN=0
ASSUME_YES=0
KEEP_CONFIG=0
KEEP_SOURCE=0
KEEP_MODULE=0
PURGE_DEPS=0
GOVERNOR=""

C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_OFF=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%s[warn]%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '%s[error]%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: uninstall.sh [options]

  --dry-run             print the commands without executing them
  --yes, -y             do not ask for confirmation (unattended)
  --keep-config         keep /etc/aes67-sip.conf, /etc/daemon.conf, /etc/status.json
  --keep-source         keep the /opt source checkouts (~300 MB of build trees)
  --keep-module         keep the RAVENNA kernel module (DKMS entry and loaded)
  --governor <name>     CPU governor to restore now (default: ondemand when the
                        kernel offers it, otherwise only print the hint)
  --purge-deps          also apt-purge the appliance-only build dependencies
  -h, --help            this help

ZeroTier is never touched, whatever the options.
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

report_line() { printf '  %-26s %s\n' "$1" "$2"; }

# ---------------------------------------------------------------------------
# removal primitives
# ---------------------------------------------------------------------------

# Every path goes through here.  It refuses anything that is not an absolute
# path, refuses the managed directories themselves, and skips ZeroTier outright:
# an uninstall must never cost the site its NAT-free link to the PBX.
remove_path() {
  local path="$1"
  [[ -n "${path}" ]] || die "remove_path: empty path"
  [[ "${path}" == /* ]] || die "remove_path: '${path}' is not absolute"
  case "${path}" in
    /|/etc|/usr|/usr/local|/var|/var/lib|/var/log|/opt|/home|/lib|/lib/modules|\
/etc/systemd|/etc/systemd/system|/etc/sysctl.d|/etc/modules-load.d|\
/etc/ld.so.conf.d)
      die "refusing to remove the system directory '${path}'" ;;
  esac
  if [[ "${path}" == *zerotier* ]]; then
    warn "skipping '${path}': ZeroTier stays installed"
    return 0
  fi
  [[ -e "${path}" || -L "${path}" ]] || return 0
  run rm -rf -- "${path}"
}

stop_unit() {
  local unit="$1"
  command -v systemctl >/dev/null 2>&1 || return 0
  if systemctl list-unit-files "${unit}" >/dev/null 2>&1; then
    run systemctl disable --now "${unit}" || warn "cannot disable ${unit}"
  elif systemctl is-active --quiet "${unit}" 2>/dev/null; then
    run systemctl stop "${unit}" || warn "cannot stop ${unit}"
  fi
}

dkms_versions() {
  local dir="/var/lib/dkms/${DKMS_PACKAGE}" entry
  [[ -d "${dir}" ]] || return 0
  # The tree holds one directory per registered version plus one per kernel
  # (kernel-6.x-arch) - only the version names are arguments to `dkms remove`.
  for entry in "${dir}"/*; do
    [[ -d "${entry}" ]] || continue
    entry="${entry##*/}"
    if [[ "${entry}" != kernel-* ]]; then
      printf '%s\n' "${entry}"
    fi
  done
}

sysctl_proc_path() { printf '/proc/sys/%s' "$(printf '%s' "$1" | tr '.' '/')"; }

# A value is only put back when it still carries our number, so a distribution
# file (or an operator's own tuning) is never overwritten.
reset_sysctls() {
  local i key ours default current proc
  for i in "${!SYSCTL_KEYS[@]}"; do
    key="${SYSCTL_KEYS[$i]}"
    ours="${SYSCTL_OURS[$i]}"
    default="${SYSCTL_DEFAULTS[$i]}"
    proc="$(sysctl_proc_path "${key}")"
    [[ -r "${proc}" ]] || continue
    current="$(cat "${proc}" 2>/dev/null || true)"
    [[ "${current}" == "${ours}" ]] || continue
    run sysctl -q -w "${key}=${default}"
  done
}

restore_governor() {
  local wanted="${GOVERNOR}" available current f
  available="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null || true)"
  if [[ -z "${available}" ]]; then
    report_line "CPU governor" "no cpufreq on this machine"
    return 0
  fi
  if [[ -z "${wanted}" && " ${available} " == *" ondemand "* ]]; then
    # Nothing on this distribution configures a governor (there is no
    # /etc/default/cpufrequtils), so the kernel's compiled-in default - ondemand
    # on Raspberry Pi OS - is what the machine boots with.
    wanted="ondemand"
  fi
  current="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)"
  if [[ -z "${wanted}" ]]; then
    if [[ "${current}" == "performance" ]]; then
      report_line "CPU governor" "still 'performance' - reboot to restore (or --governor <name>)"
    else
      report_line "CPU governor" "${current:-unknown}"
    fi
    return 0
  fi
  if [[ "${current}" != "${wanted}" ]]; then
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      [[ -w "${f}" ]] || continue
      run sh -c "echo '${wanted}' > '${f}'" || warn "cannot set ${f} to ${wanted}"
    done
  fi
  if [[ ${DRY_RUN} -eq 1 ]]; then
    report_line "CPU governor" "would set '${wanted}' (now '${current}')"
  else
    report_line "CPU governor" "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) (was 'performance')"
  fi
}

# ---------------------------------------------------------------------------
# arguments and preflight
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)      DRY_RUN=1; shift ;;
    --yes|-y)       ASSUME_YES=1; shift ;;
    --keep-config)  KEEP_CONFIG=1; shift ;;
    --keep-source)  KEEP_SOURCE=1; shift ;;
    --keep-module)  KEEP_MODULE=1; shift ;;
    --governor)     GOVERNOR="${2:-}"; [[ -n "${GOVERNOR}" ]] || die "--governor needs a name"; shift 2 ;;
    --purge-deps)   PURGE_DEPS=1; shift ;;
    -h|--help)      usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

[[ "$(id -u)" -eq 0 ]] || die "run as root: sudo ./scripts/uninstall.sh [options]"
command -v systemctl >/dev/null 2>&1 || die "systemd is required"

log "aes67-sip uninstaller (dry-run=${DRY_RUN})"
echo "  removes: the gateway, aes67-daemon, PJSIP, the RAVENNA kernel module,"
echo "           the kernel tuning + service users, the /opt source trees"
echo "  keeps:   ZeroTier (service, /var/lib/zerotier-one and the joined network)"
[[ ${KEEP_CONFIG} -eq 1 ]] && echo "  --keep-config:  /etc/aes67-sip.conf, /etc/daemon.conf and /etc/status.json stay"
[[ ${KEEP_SOURCE} -eq 1 ]] && echo "  --keep-source:  ${SRC_DIR} and ${DAEMON_DIR} stay"
[[ ${KEEP_MODULE} -eq 1 ]] && echo "  --keep-module:  the RAVENNA kernel module stays"
[[ ${PURGE_DEPS} -eq 1 ]] && echo "  --purge-deps:   apt-purge ${PURGE_PACKAGES[*]}"

if [[ ${DRY_RUN} -eq 0 && ${ASSUME_YES} -eq 0 ]]; then
  if [[ -r /dev/tty ]]; then
    printf '%sProceed? [y/N] %s' "${C_WARN}" "${C_OFF}"
    read -r answer </dev/tty || answer=""
    [[ "${answer}" =~ ^[Yy] ]] || { echo "aborted, nothing changed"; exit 0; }
  else
    die "no terminal to confirm on; pass --yes for unattended use"
  fi
fi

# ---------------------------------------------------------------------------
# 1. services
# ---------------------------------------------------------------------------
log "stopping and removing the systemd units"
stop_unit aes67-sip.service
stop_unit aes67-daemon.service
stop_unit aes67-cpu-governor.service
remove_path "${SYSTEMD_DIR}/aes67-sip.service"
remove_path "${SYSTEMD_DIR}/aes67-daemon.service"
remove_path "${GOVERNOR_UNIT}"
if [[ ${DRY_RUN} -eq 0 ]]; then
  run systemctl daemon-reload
  run systemctl reset-failed || true
fi

# ---------------------------------------------------------------------------
# 2. the gateway
# ---------------------------------------------------------------------------
log "removing the gateway"
remove_path "${PREFIX}/bin/aes67-sip"
remove_path "${WEBUI_DIR}"
remove_path /var/lib/aes67-sip
remove_path /var/log/aes67-sip
if [[ ${KEEP_CONFIG} -eq 1 ]]; then
  log "keeping ${CONFIG_FILE} (--keep-config)"
else
  # the file and the backups the installer/UI wrote next to it
  for config in /etc/aes67-sip.conf*; do
    remove_path "${config}"
  done
fi

# ---------------------------------------------------------------------------
# 3. aes67-daemon
# ---------------------------------------------------------------------------
log "removing aes67-daemon"
remove_path "${PREFIX}/bin/aes67-daemon"
remove_path "${DAEMON_SHARE}"
remove_path /var/lib/aes67-daemon
if [[ ${KEEP_CONFIG} -eq 0 ]]; then
  remove_path "${DAEMON_CONFIG}"
  remove_path "${DAEMON_STATUS}"
fi

# ---------------------------------------------------------------------------
# 4. PJSIP (pjproject) shared libraries
# ---------------------------------------------------------------------------
log "removing the pjproject shared libraries"
for lib in "${PREFIX}"/lib/libpj*.so*; do
  remove_path "${lib}"
done
remove_path "${LD_CONF}"
if [[ ${DRY_RUN} -eq 0 ]]; then
  run ldconfig || warn "ldconfig failed"
fi

# ---------------------------------------------------------------------------
# 5. Merging RAVENNA/AES67 kernel module
# ---------------------------------------------------------------------------
if [[ ${KEEP_MODULE} -eq 1 ]]; then
  log "keeping the RAVENNA kernel module (--keep-module)"
else
  log "unloading and removing the RAVENNA kernel module"
  if lsmod | grep -q "^${MODULE_NAME}"; then
    run modprobe -r "${MODULE_NAME}" || warn "${MODULE_NAME} is still in use"
  fi
  if command -v dkms >/dev/null 2>&1; then
    while read -r version; do
      [[ -n "${version}" ]] || continue
      run dkms remove -m "${DKMS_PACKAGE}" -v "${version}" --all ||
        warn "dkms remove ${DKMS_PACKAGE}/${version} failed"
    done < <(dkms_versions)
  fi
  remove_path "/var/lib/dkms/${DKMS_PACKAGE}"
  for src in /usr/src/ravenna-alsa-lkm /usr/src/ravenna-alsa-lkm-*; do
    remove_path "${src}"
  done
  remove_path "${MODULES_LOAD}"
  if [[ ${DRY_RUN} -eq 0 ]]; then
    run depmod -a || warn "depmod failed"
  fi
fi

# ---------------------------------------------------------------------------
# 6. kernel tuning
# ---------------------------------------------------------------------------
log "reverting the kernel tuning"
remove_path "${SYSCTL_FILE}"
if [[ ${DRY_RUN} -eq 0 ]]; then
  run sh -c 'sysctl --system >/dev/null' || warn "sysctl --system failed"
fi
reset_sysctls
restore_governor

# ---------------------------------------------------------------------------
# 7. service users and group
# ---------------------------------------------------------------------------
log "removing the service users and group"
if id aes67-sip >/dev/null 2>&1; then
  run userdel aes67-sip || warn "cannot remove the aes67-sip user"
fi
remove_path /var/lib/aes67-sip
if getent group aes67-sip >/dev/null 2>&1; then
  run groupdel aes67-sip || warn "cannot remove the aes67-sip group"
fi
if id aes67-daemon >/dev/null 2>&1; then
  run userdel aes67-daemon || warn "cannot remove the aes67-daemon user"
fi
remove_path /home/aes67-daemon

# ---------------------------------------------------------------------------
# 8. source trees, the installer's swap file, optional dependency purge
# ---------------------------------------------------------------------------
if [[ ${KEEP_SOURCE} -eq 1 ]]; then
  log "keeping the source trees (--keep-source)"
else
  log "removing the source trees"
  remove_path "${SRC_DIR}"
  remove_path "${DAEMON_DIR}"
fi
remove_path /var/swap-aes67-installer

if [[ ${PURGE_DEPS} -eq 1 ]]; then
  log "purging the appliance-only build dependencies"
  run env DEBIAN_FRONTEND=noninteractive apt-get purge -y -qq "${PURGE_PACKAGES[@]}" ||
    warn "apt-get purge reported an error"
  run env DEBIAN_FRONTEND=noninteractive apt-get --purge autoremove -y -qq ||
    warn "apt-get autoremove reported an error"
fi

# ---------------------------------------------------------------------------
# 9. report
# ---------------------------------------------------------------------------
echo
log "uninstall finished"
if [[ ${DRY_RUN} -eq 1 ]]; then
  report_line "mode" "dry-run - nothing was changed"
fi

unit_state() {
  if systemctl cat "$1" >/dev/null 2>&1; then
    printf 'still present'
  else
    printf 'removed'
  fi
}
report_line "aes67-sip.service" "$(unit_state aes67-sip.service)"
report_line "aes67-daemon.service" "$(unit_state aes67-daemon.service)"
report_line "aes67-cpu-governor" "$(unit_state aes67-cpu-governor.service)"
if lsmod 2>/dev/null | grep -q "^${MODULE_NAME}"; then
  report_line "RAVENNA module" "still loaded (a reboot clears it)"
else
  report_line "RAVENNA module" "not loaded"
fi

# Anything that survived, minus what the operator asked to keep.
leftovers=()
for path in "${PREFIX}/bin/aes67-sip" "${PREFIX}/bin/aes67-daemon" \
            "${WEBUI_DIR}" "${DAEMON_SHARE}" "${CONFIG_FILE}" \
            "${DAEMON_CONFIG}" "${DAEMON_STATUS}" /var/lib/aes67-sip \
            /var/log/aes67-sip /var/lib/aes67-daemon "${SRC_DIR}" \
            "${DAEMON_DIR}" "${SYSCTL_FILE}" "${MODULES_LOAD}" "${LD_CONF}" \
            "/var/lib/dkms/${DKMS_PACKAGE}"; do
  if [[ -e "${path}" || -L "${path}" ]]; then
    if [[ ${KEEP_CONFIG} -eq 1 && "${path}" == /etc/* ]]; then continue; fi
    if [[ ${KEEP_SOURCE} -eq 1 && "${path}" == /opt/* ]]; then continue; fi
    leftovers+=("${path}")
  fi
done
if [[ ${#leftovers[@]} -eq 0 ]]; then
  report_line "leftovers" "none"
else
  report_line "leftovers" "${leftovers[*]}"
fi
if id aes67-sip >/dev/null 2>&1 || id aes67-daemon >/dev/null 2>&1; then
  report_line "service users" "still present"
else
  report_line "service users" "removed"
fi
if command -v zerotier-cli >/dev/null 2>&1; then
  report_line "ZeroTier" "$(zerotier-cli info 2>/dev/null | awk '{print $3, $5}') - untouched"
else
  report_line "ZeroTier" "not installed on this machine"
fi

cat <<EOF

ZeroTier was not touched: the node, its identity in /var/lib/zerotier-one and its
network membership are exactly as they were, so a rebuild does not need a new
authorisation in my.zerotier.com.

A reboot clears whatever is still in memory (${MODULE_NAME}, the CPU governor and
the pjproject libraries a running process may still have mapped).

To put the appliance back (keeping this machine's ZeroTier membership):
  curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-sip/main/scripts/install.sh \\
    | sudo bash -s -- --no-zerotier
EOF


