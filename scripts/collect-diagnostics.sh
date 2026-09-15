#!/usr/bin/env bash
#
# Collects everything needed to diagnose an aes67-sip appliance in one go.
#
#   sudo ./scripts/collect-diagnostics.sh > /tmp/aes67-diag.txt
#
# It is read-only: no configuration is changed, no service is restarted.  The
# SIP account passwords are redacted, so the output is safe to paste into a
# ticket or a chat.
#
set -uo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "note: running without root, some sections will be incomplete (use sudo)" >&2
fi

section() { printf '\n===== %s =====\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

section "host"
uname -a
grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release 2>/dev/null
uptime

section "gateway version"
/usr/local/bin/aes67-sip --version 2>&1

section "services"
for service in aes67-daemon zerotier-one aes67-sip; do
  printf '%-16s %-12s %s\n' "${service}" \
    "$(systemctl is-active "${service}" 2>&1)" \
    "$(systemctl is-enabled "${service}" 2>&1)"
done

section "gateway configuration (passwords redacted)"
if [[ -f /etc/aes67-sip.conf ]]; then
  python3 - <<'PY'
import json
try:
    config = json.load(open('/etc/aes67-sip.conf'))
except Exception as exc:
    print(f'cannot parse /etc/aes67-sip.conf: {exc}')
else:
    for account in config.get('accounts', []):
        if account.get('password'):
            account['password'] = '***redacted***'
    print(json.dumps(config, indent=2, sort_keys=True))
PY
else
  echo "/etc/aes67-sip.conf is missing"
fi

section "gateway status"
if have curl; then
  curl -fsS --max-time 5 http://127.0.0.1:8081/api/status 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -120 ||
    echo "the gateway API is unreachable on 127.0.0.1:8081"
else
  echo "curl is not installed"
fi

section "self-test"
if have curl; then
  curl -fsS --max-time 30 -X POST http://127.0.0.1:8081/api/system/self-test 2>/dev/null |
    python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception as exc:
    print("self-test did not return JSON:", exc)
else:
    print("ok:", data.get("ok"))
    for check in data.get("checks", []):
        print(" ", "PASS" if check.get("ok") else "FAIL", check.get("name"), "->", check.get("detail"))
' 2>/dev/null || echo "self-test request failed"
fi

section "aes67-daemon"
if have curl; then
  printf 'ptp: '; curl -fsS --max-time 3 http://127.0.0.1:8080/api/ptp/status 2>&1; echo
  echo "sinks:"; curl -fsS --max-time 3 http://127.0.0.1:8080/api/sinks 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -40
  echo "sources:"; curl -fsS --max-time 3 http://127.0.0.1:8080/api/sources 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -40
fi

section "journal: aes67-sip (filtered, last 40)"
journalctl -u aes67-sip -n 500 --no-pager 2>/dev/null |
  grep -iE 'registration|registrar|media|call|overrun|underrun|xrun|endpoint sdp|codec|loopback' |
  tail -40

section "journal: aes67-daemon (last 15)"
journalctl -u aes67-daemon -n 15 --no-pager 2>/dev/null

section "ALSA / RAVENNA device"
if have aplay; then
  aplay -l 2>/dev/null | grep -i ravenna || echo "no RAVENNA card in 'aplay -l'"
fi
lsmod | grep -i ravenna || echo "the MergingRavennaALSA module is not loaded"
if have arecord; then
  echo "-- 8 channels, S24_3LE (the gateway default format):"
  arecord -D plughw:RAVENNA -c 8 -f S24_3LE -r 48000 -d 1 /tmp/aes67-probe24.wav 2>&1 | tail -2
  echo "-- 8 channels, S16_LE:"
  arecord -D plughw:RAVENNA -c 8 -f S16_LE -r 48000 -d 1 /tmp/aes67-probe16.wav 2>&1 | tail -2
fi

section "kernel parameters"
sysctl kernel.sched_rt_runtime_us kernel.perf_cpu_time_max_percent \
  net.ipv4.igmp_max_memberships 2>/dev/null
echo "scaling governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"

section "network"
ip -brief address show 2>/dev/null
echo
ip route show default 2>/dev/null
if have zerotier-cli; then
  echo "-- zerotier:"
  zerotier-cli listnetworks 2>/dev/null | head -5
fi

section "gateway link"
if have ldd; then
  ldd /usr/local/bin/aes67-sip 2>/dev/null | grep -i 'not found' ||
    echo "all shared libraries resolved"
fi

section "resources"
free -m 2>/dev/null

echo
echo "done. Paste this output (it contains no passwords)."
