#!/usr/bin/env bash
#
# Prepares a Linux host (mini PC or SBC) to run aes67-sip:
#   * kernel parameters required by the RAVENNA/AES67 driver
#   * PulseAudio disabled (it destabilises the RAVENNA ALSA device)
#   * the Merging RAVENNA/AES67 kernel module built and loaded
#   * the aes67-sip service user
#
# Usage:  sudo ./scripts/setup-ravenna.sh [--with-daemon] [--dry-run]
#
set -euo pipefail

DRY_RUN=0
WITH_DAEMON=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    --with-daemon) WITH_DAEMON=1 ;;
    *) echo "unknown option: ${arg}" >&2; exit 2 ;;
  esac
done

run() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    echo "+ $*"
    "$@"
  fi
}

if [[ "$(id -u)" -ne 0 ]]; then
  echo "error: run as root (sudo)" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. kernel parameters
# ---------------------------------------------------------------------------
# igmp_max_memberships: one membership per AES67 stream the gateway joins.
# sched_rt_runtime_us: give real time audio tasks the whole CPU budget.
# perf_cpu_time_max_percent: CPU frequency scaling events disturb AES67 streams.
echo "==> configuring kernel parameters"
run tee /etc/sysctl.d/90-aes67.conf >/dev/null <<'EOF'
# Added by aes67-sip/scripts/setup-ravenna.sh
net.ipv4.igmp_max_memberships = 66
kernel.sched_rt_runtime_us = 1000000
kernel.perf_cpu_time_max_percent = 0
EOF
run sysctl --system >/dev/null

# ---------------------------------------------------------------------------
# 2. PulseAudio / PipeWire must not touch the RAVENNA device
# ---------------------------------------------------------------------------
echo "==> disabling PulseAudio"
if command -v pulseaudio >/dev/null 2>&1; then
  run systemctl --global mask pulseaudio.service pulseaudio.socket 2>/dev/null || true
  run systemctl mask pulseaudio.service pulseaudio.socket 2>/dev/null || true
  run pkill -x pulseaudio 2>/dev/null || true
fi
if command -v pipewire >/dev/null 2>&1; then
  echo "    note: PipeWire is installed; make sure it does not grab hw:RAVENNA"
fi

# ---------------------------------------------------------------------------
# 3. Merging RAVENNA/AES67 kernel module
# ---------------------------------------------------------------------------
echo "==> checking the RAVENNA kernel module"
if lsmod | grep -q '^MergingRavennaALSA'; then
  echo "    MergingRavennaALSA is already loaded"
else
  MODULE_PATH="$(find / -name 'MergingRavennaALSA.ko' -not -path '*/proc/*' 2>/dev/null | head -n 1 || true)"
  if [[ -n "${MODULE_PATH}" ]]; then
    echo "    loading ${MODULE_PATH}"
    run insmod "${MODULE_PATH}"
  else
    cat <<'EOF'
    MergingRavennaALSA.ko was not found on this system.

    Build it (kernel headers are required) with:
      sudo apt-get install -y linux-headers-$(uname -r)
      git clone -b aes67-daemon https://github.com/bondagit/ravenna-alsa-lkm.git
      cd ravenna-alsa-lkm/driver && make -j"$(nproc)" && sudo insmod MergingRavennaALSA.ko

    Then re-run this script and check the device with:
      aplay -l | grep -i ravenna
      arecord -D plughw:RAVENNA -c 2 -f cd -r 48000 -d 5 /tmp/probe.wav
EOF
  fi
fi

# ---------------------------------------------------------------------------
# 4. aes67-daemon (optional convenience build)
# ---------------------------------------------------------------------------
if [[ ${WITH_DAEMON} -eq 1 ]]; then
  echo "==> building aes67-daemon"
  if [[ ! -d /opt/aes67-linux-daemon ]]; then
    run git clone --depth 1 https://github.com/bondagit/aes67-linux-daemon.git /opt/aes67-linux-daemon
  fi
  run bash -c 'cd /opt/aes67-linux-daemon && ./build.sh'
  echo "    install the systemd unit with:"
  echo "      sudo /opt/aes67-linux-daemon/systemd/install.sh"
fi

# ---------------------------------------------------------------------------
# 5. service user
# ---------------------------------------------------------------------------
echo "==> creating the aes67-sip service user"
if ! id aes67-sip >/dev/null 2>&1; then
  run useradd --system --home-dir /var/lib/aes67-sip --create-home \
    --shell /usr/sbin/nologin aes67-sip
fi
run usermod -aG audio aes67-sip || true

cat <<'EOF'

==> done. Verify with:
      aplay -l | grep -i ravenna                 # ALSA device present
      cat /proc/asound/cards                     # card list
      systemctl status aes67-daemon              # daemon running, PTP locked
      curl -s http://127.0.0.1:8080/api/ptp/status

The gateway itself is started with:
      sudo systemctl enable --now aes67-sip
EOF
