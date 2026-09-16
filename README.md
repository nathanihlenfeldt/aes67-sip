# aes67-sip

[![build](https://github.com/nathanihlenfeldt/aes67-sip/actions/workflows/build.yml/badge.svg)](https://github.com/nathanihlenfeldt/aes67-sip/actions/workflows/build.yml)
[![release](https://github.com/nathanihlenfeldt/aes67-sip/actions/workflows/release.yml/badge.svg)](https://github.com/nathanihlenfeldt/aes67-sip/actions/workflows/release.yml)
[![licence: GPL-3.0](https://img.shields.io/badge/licence-GPL--3.0-blue.svg)](LICENSE)

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
  ┌──────────────────────┐    AES67 / L24 48k multicast    ┌──────────────────────┐
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


## Uninstall

`scripts/uninstall.sh` reverses the installer and leaves the machine as the
distribution shipped it: the units, the gateway and its web UI, `aes67-daemon`,
the PJSIP libraries, the RAVENNA kernel module with its DKMS entry, the kernel
tuning (sysctls and CPU governor), the service users and the `/opt` source trees.

**ZeroTier is deliberately never touched** - the package, the node identity in
`/var/lib/zerotier-one` and the network membership stay exactly as they are, so an
appliance that is rebuilt on site (or restored from an image) does not need a new
authorisation in `my.zerotier.com`.

```bash
sudo ./scripts/uninstall.sh --dry-run   # print the plan, change nothing
sudo ./scripts/uninstall.sh             # show the plan, ask, then remove
```

Other flags: `--yes` (unattended), `--keep-config` (keep `/etc/aes67-sip.conf`,
`/etc/daemon.conf`, `/etc/status.json`), `--keep-source` (keep the `/opt`
checkouts), `--keep-module`, `--governor <name>` and `--purge-deps` (apt-purge the
appliance-only build dependencies). It finishes with a report, including anything
it could not remove, and the values it restored.


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
  ],
  "conference": { "enabled": true, "account": "pbx",
                  "target": "sip:conf@pbx.example.com", "display_name": "Conference" }
}
```

- `sip.call_mode`:
  - `permanent` *(the intercom mode)* - the line dials `dial_target` and keeps the call
    up, retrying every 5 s after a failure; an inbound INVITE from the PBX is answered
    and also held up.
  - `auto_answer` - answer inbound calls only. `dial_out` - dial out only.
  - `manual` - operator dials from the UI. `ptt` - legacy energy-triggered mode.
- `conference` is the **off-site leg**: one call, independent of the per-line legs, whose
  media is the matrix's conference sides rather than a line's channels. What the
  conference says is mixed into every party line that claims it (`party_lines[].claims_conference`)
  and what it hears is those lines' members summed at their contribution levels. Disabled
  by default; `target` must be set when it is enabled, and `GET /api/status` reports the
  leg's call state and levels under `conference`.
- One AES67 **channel per line** (`aes67.channels: [n]`) and one call per channel.
- `aes67.codec` is the RTP payload our **source** advertises: **`L24` by default**
  (Dante and most AES67 devices), or `L16`/`L2432`/`AM824`/`L32`. The sink's payload
  isn't configured here - it comes from the endpoint's SDP.
- `audio.format` is the ALSA sample format on the RAVENNA device: **`s24_3le` by
  default**, which preserves the 24 bit resolution of an L24 stream. If the driver
  refuses it the backend falls back to `s16_le` (16 bit, the low bits of the L24
  words stay zero) and says so in the log.
- `aes67.remote_source_id` (or `remote_sdp`) selects the *endpoint's* stream for the
  sink; leave `remote_source_id` empty and pick it in the UI from SAP/mDNS discovery.
  With neither set the gateway **leaves the sink alone** (`aes67.sdp_source:
  "unmanaged"`) - it may have been wired up in Dante Controller/Q-SYS and must not be
  overwritten. `aes67.commissioning_loopback: true` opts into subscribing the sink to
  our own source instead, for bench testing without endpoints.
- `aes67.ignore_refclk_gmid` skips the daemon's PTP grandmaster check on the SDP - only
  needed when an endpoint advertises a different grandmaster than the locked one.

## Commissioning

1. `sudo systemctl status aes67-daemon aes67-sip`
2. PTP must be **locked** before any audio flows, both ways:
   `curl -s http://<appliance>:8080/api/ptp/status` -> `{"status":"locked", ...}`
   With no grandmaster on the VLAN, run one from the appliance:
   `sudo ptp4l -i eth0 -m -l7 -E -S`
3. Open `http://<appliance>:8081`:
   - **Dashboard** - PTP, daemon reachability, SIP registrations, per-line state/levels,
     the conference leg when one is configured, and each party line with who is arriving
     and who has gone silent
   - **Lines** - channel mapping, gain/mute, call controls, DTMF, test tone
   - **Party lines** - the commissioning view and editor: each line with its members,
     their contribution levels and live levels, and membership, levels and per-channel
     bindings edited here rather than in JSON
   - **AES67** - daemon config, sinks/sources, SAP/mDNS browser (pick a source per line)
   - **Diagnostics** - log tail, self-test, per-channel meters
4. Check ALSA directly if a line looks dead:
   `arecord -D plughw:RAVENNA -c 1 -f S16_LE -r 48000 -d 5 /tmp/probe.wav`
   (stop `aes67-sip` first: the gateway holds the PCM). The substream state is the
   thing to look at - `cat /proc/asound/RAVENNA/pcm0c/sub0/status` - it must read
   `state: RUNNING` with a `hw_ptr` that moves. The RAVENNA driver only exchanges
   audio while a substream is triggered, so a `PREPARED` stream (or a `hw_ptr` of
   0) means the device is idle: no capture, no RTP out, nothing played to the
   endpoints. The gateway starts both directions itself; `GET /api/status`
   reports the states in `audio.detail`.
   When the driver's engine stops underneath a running gateway (restarting
   `aes67-daemon` does that: the substreams keep reporting RUNNING while not a
   frame moves), the gateway notices that no frames arrive, reopens the PCM and
   logs `no audio from 'plughw:RAVENNA' for N s: the RAVENNA engine looks
   stopped ... reopening the device`. No operator action is needed.

The built-in self-test (`POST /api/system/self-test`) checks the audio backend, the
daemon, PTP, SIP registration, each line and each party line in one go. A party-line
check names its members and says who is arriving and who has gone silent, so a dead
endpoint, a muted headset or a pulled cable is findable from the UI rather than by reading
journals: a line whose members are simply quiet says so and names them, while a line where
nobody *can* talk - no member has a talk channel bound - is the one reported as broken.
The appliance does not claim to tell a quiet room from every cable on a line being pulled;
the names are what a maintenance engineer works from. When the audio path is not running
the party lines are not judged at all, and the audio backend check says what that costs
the site; the same facts are on the dashboard, which polls them every 500 ms.

### Interop notes (Q-SYS, Dante)

- Our sources advertise the **explicit PTP grandmaster clock ID** in
  `a=ts-refclk` (`aes67.refclk_ptp_traceable: false`, the default). Receivers that
  compare the announced reference with the PTP domain's grandmaster - Q-SYS shows
  this as a *grandmaster mismatch* and refuses the flow - cannot use the RFC 7273
  `traceable` keyword we used to send, so only set that option to `true` for
  receivers that insist on it.
- Receivers ignore a stream while the realm's PTP domain or grandmaster differs;
  both sides must show the same GMID (`GET /api/status` -> `aes67.ptp.gmid` should
  equal the `a=ts-refclk` value in the endpoint's SDP).

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

Tests: `ctest --test-dir build --output-on-failure`, or run everything CI runs (formatting,
build, tests, script syntax) with `./scripts/check.sh` before committing.

## Layout

```
src/audio/    RAVENNA/ALSA + null backends, polyphase resampler, lock-free rings, router
src/matrix/   the intercom matrix: party lines, endpoints and their mixes
src/aes67/    aes67-daemon REST client (+ an in-process simulation for development)
src/sip/      SipEngine interface, PJSIP engine, custom pjsua2 audio port, stub engine
src/bridge/   line manager: stream provisioning, call modes, supervision, meters
src/http/     REST API used by the UI (see docs/api.md)
webui/        Vite + React UI (dark operator console)
scripts/      install.sh, uninstall.sh, build-pjsip.sh, install-deps.sh, setup-ravenna.sh
systemd/      aes67-sip.service
docs/         DECISIONS.md (agreed scope), api.md (REST contract)
```

## Limitations

- The **AES67 path and a SIP call are validated on the target appliance**
  (Raspberry Pi 5, kernel 6.18, Q-SYS Core as the PTP grandmaster): a Q-SYS source
  received on one RAVENNA channel and returned as L24 48 kHz / 1 ms RTP, and a call
  with the FreePBX conference came up (`CONFIRMED`) with SIP audio - both
  directions with no xruns.  Note that `sip.accounts[].code` is only updated by a
  registration event, so it can still show the previous 401 while calls succeed.
- **Kernel 6.15 and newer** run the RAVENNA driver's 1 ms audio tick as a soft hrtimer,
  so RTP is emitted in bursts rather than evenly (upstream issue
  [bondagit/ravenna-alsa-lkm#39](https://github.com/bondagit/ravenna-alsa-lkm/issues/39),
  open). Receivers with small playout buffers can reject the stream; an LTS kernel
  (6.6/6.8) uses the hard timer path. The installer warns about this.
- AES67 audio requires a **PTP grandmaster**; without one the RAVENNA device never locks
  and there is no audio to route.
- `aes67-daemon`'s HTTP streamer must stay disabled (`streamer_enabled: false`) because
  it captures the same RAVENNA device the gateway uses.
- No AEC, no video, no TLS/SRTP yet (the site link is a ZeroTier/VPN path).
- The gateway is a **client** of the FreePBX; it does not accept registrations itself.

## Licence

GPL-3.0 - see [LICENSE](LICENSE). PJSIP is GPLv2-or-later with a commercial option, and
`aes67-daemon` plus the RAVENNA driver are GPL-3.0, so this combination must be GPL-3.0.
