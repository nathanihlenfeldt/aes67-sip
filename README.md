# aes67-sip

A Linux appliance that bridges **AES67 intercom endpoints on a production site** to a
**conference on an off-site FreePBX**.

It pairs the [Merging RAVENNA/AES67 ALSA driver](https://github.com/bondagit/ravenna-alsa-lkm)
and the [`aes67-daemon`](https://github.com/bondagit/aes67-linux-daemon) (PTP, RTP,
SAP/mDNS discovery, REST API) with [PJSIP](https://www.pjsip.org/) for SIP, adds the
sample rate conversion between AES67 (48 kHz) and the SIP codec, and ships a small web
UI for commissioning and monitoring.

It is **not** a softphone for a person: there is no handset, no ringing and no
push-to-talk. Audio is 4-wire and continuous; a call, once up, stays up.

```
  4-wire intercom endpoints          Raspberry Pi appliance                off-site
  ┌──────────────────────┐    AES67 / L16 48k multicast    ┌──────────────────────┐
  │ panels: send+receive │ ──────────────────────────────► │ RAVENNA ALSA device  │
  └──────────────────────┘ ◄────────────────────────────── │ aes67-daemon  :8080  │
                                                           │ aes67-sip     :8081  │
                                                           └──────────┬───────────┘
                                                                      │ SIP + RTP
                                                          G.722, 20 ms, ZeroTier
                                                                      ▼
                                                             FreePBX (conference)
```

Scope, decisions and open items are recorded in **[docs/DECISIONS.md](docs/DECISIONS.md)**;
the REST contract is in **[docs/api.md](docs/api.md)**.

## Install (Raspberry Pi, one command)

On a clean **64-bit Raspberry Pi OS (Bookworm)** or Ubuntu 22.04/24.04:

```bash
curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-sip/main/scripts/install.sh \
  | sudo bash -s -- --zerotier-network <your-network-id>
```

The installer is idempotent and installs everything: build dependencies, the RAVENNA
kernel module **through DKMS** (so kernel upgrades rebuild it), `aes67-daemon` plus its
systemd unit, PJSIP 2.17, this gateway plus the web UI and its systemd unit, the
real-time sysctls, and optionally ZeroTier. It finishes with a preflight report covering
the kernel module, the ALSA device, PTP lock, the daemon API and the gateway API.

Useful flags: `--no-zerotier`, `--ref <tag>`, `--skip-kernel-module`, `--skip-daemon`,
`--skip-gateway`, `--no-start`, `--dry-run`, `--help`.

**ZeroTier is configured by the installer** (that is the NAT-free path to the FreePBX):
it installs ZeroTier, joins `--zerotier-network <id>` - or asks for the network id on
the terminal when the flag is omitted - then prints the node address and waits for the
controller's reply, telling you clearly whether the node is `OK` or still
`ACCESS_DENIED` (i.e. waiting to be authorised in `my.zerotier.com`). Pass
`--no-zerotier` if the appliance reaches a public FreePBX endpoint instead.


## Configuration

`/etc/aes67-sip.conf` (installed once, never overwritten). The essentials for the
intercom use case:

```json
{
  "accounts": [
    { "id": "pbx", "registrar": "sip:10.147.20.5", "username": "2001",
      "password": "...", "display_name": "AES67 Gateway" }
  ],
  "sip":  { "engine": "pjsip", "codecs": ["G722/16000/1", "PCMU/8000/1"],
            "ptime_ms": 20, "transport": "udp", "srtp": "disabled" },
  "audio": { "backend": "ravenna", "device": "plughw:RAVENNA",
             "sample_rate": 48000, "channels": 8, "period_frames": 48 },
  "lines": [
    { "id": 0, "name": "Intercom 1", "enabled": true,
      "aes67": { "sink_id": 0, "source_id": 0, "channels": [0],
                 "stream_name": "Intercom 1", "auto_create_streams": true },
      "sip":  { "account": "pbx", "extension": "2001", "call_mode": "permanent",
                "dial_target": "sip:1001@pbx.example.com" } }
  ]
}
```

- `sip.call_mode`:
  - `permanent` *(the intercom mode)* - the line dials `dial_target` and keeps the call
    up, retrying every 5 s after a failure; an inbound INVITE from the PBX is answered
    and also held up.
  - `auto_answer` - answer inbound calls only. `dial_out` - dial out only.
  - `manual` - operator dials from the UI. `ptt` - legacy energy-triggered mode.
- One AES67 **channel per line** (`aes67.channels: [n]`) and one call per channel.
- `aes67.remote_source_id` (or `remote_sdp`) selects the *endpoint's* stream for the
  sink; leave `remote_source_id` empty and pick it in the UI from SAP/mDNS discovery.
- `aes67.ignore_refclk_gmid` skips the daemon's PTP grandmaster check on the SDP - only
  needed when an endpoint advertises a different grandmaster than the locked one.

## Commissioning

1. `sudo systemctl status aes67-daemon aes67-sip`
2. PTP must be **locked** before any audio flows, both ways:
   `curl -s http://<appliance>:8080/api/ptp/status` -> `{"status":"locked", ...}`
   With no grandmaster on the VLAN, run one from the appliance:
   `sudo ptp4l -i eth0 -m -l7 -E -S`
3. Open `http://<appliance>:8081`:
   - **Dashboard** - PTP, daemon reachability, SIP registrations, per-line state/levels
   - **Lines** - channel mapping, gain/mute, call controls, DTMF, test tone
   - **AES67** - daemon config, sinks/sources, SAP/mDNS browser (pick a source per line)
   - **Diagnostics** - log tail, self-test, per-channel meters
4. Check ALSA directly if a line looks dead:
   `arecord -D plughw:RAVENNA -c 1 -f S16_LE -r 48000 -d 5 /tmp/probe.wav`

The built-in self-test (`POST /api/system/self-test`) checks the audio backend, the
daemon, PTP, SIP registration and each line in one go.

## Development

macOS or any machine without ALSA/PJSIP builds and runs in fake mode (null audio
backend, simulated daemon, stub SIP engine, built-in 1 kHz tone), which is enough to
exercise the whole gateway, the API and the UI:

```bash
cmake -S . -B build -DWITH_PJSIP=OFF -DWITH_ALSA=OFF
cmake --build build -j
./build/aes67-sip -f -c config/aes67-sip.dev.conf   # http://127.0.0.1:8093

cd webui && npm install && npm run build            # production UI bundle
```

With real SIP: `./scripts/build-pjsip.sh`, then configure with
`-DWITH_PJSIP=ON -DPJSIP_ROOT=third_party/pjsip-install`.

Tests: `ctest --test-dir build --output-on-failure`.

## Layout

```
src/audio/    RAVENNA/ALSA + null backends, polyphase resampler, lock-free rings, router
src/aes67/    aes67-daemon REST client (+ an in-process simulation for development)
src/sip/      SipEngine interface, PJSIP engine, custom pjsua2 audio port, stub engine
src/bridge/   line manager: stream provisioning, call modes, supervision, meters
src/http/     REST API used by the UI (see docs/api.md)
webui/        Vite + React UI (dark operator console)
scripts/      install.sh, build-pjsip.sh, install-deps.sh, setup-ravenna.sh
systemd/      aes67-sip.service
docs/         DECISIONS.md (agreed scope), api.md (REST contract)
```

## Limitations

- Validated end to end only in **fake mode** so far: the RAVENNA/ALSA path and real SIP
  signalling still need validation on the target appliance (see docs/DECISIONS.md).
- AES67 audio requires a **PTP grandmaster**; without one the RAVENNA device never locks
  and there is no audio to route.
- `aes67-daemon`'s HTTP streamer must stay disabled (`streamer_enabled: false`) because
  it captures the same RAVENNA device the gateway uses.
- No AEC, no video, no TLS/SRTP yet (the site link is a ZeroTier/VPN path).
- The gateway is a **client** of the FreePBX; it does not accept registrations itself.

## Licence

GPL-3.0 - see [LICENSE](LICENSE). PJSIP is GPLv2-or-later with a commercial option, and
`aes67-daemon` plus the RAVENNA driver are GPL-3.0, so this combination must be GPL-3.0.
