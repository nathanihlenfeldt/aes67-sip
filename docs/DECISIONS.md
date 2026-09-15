# Design decisions

Status: **agreed with the site owner** (2026-09-15). This file is the single source of
truth for scope; code, config, docs and the installer must follow it. Anything not
listed here is either an implementation detail or an open item at the bottom.

## What this is

A Linux appliance that bridges **AES67 intercom endpoints on a production site** to a
**conference on an off-site FreePBX**, using the Merging RAVENNA ALSA driver plus the
`aes67-daemon` (bondagit/aes67-linux-daemon) for the AES67 side and PJSIP for SIP.
It is not a softphone for a human: there is no handset, no ring tone, no PTT.

```
 SITE                                            OFF-SITE
 ┌──────────────────────────┐                    ┌──────────────────────┐
 │ 4-wire intercom endpoints│                    │  FreePBX             │
 │  (AES67 send + receive)  │                    │   conference 1001    │
 └───────┬──────────────────┘                    └──────────▲───────────┘
         │ AES67 / RTP L16 48 kHz over multicast            │ SIP + RTP
         │ (1 channel per line)                             │ G.722, 20 ms
 ┌───────▼──────────────────────────────────────────┐       │
 │ Raspberry Pi appliance                           │       │
 │  RAVENNA ALSA (plughw:RAVENNA, 48 kHz, N ch)      │       │
 │  aes67-daemon (REST :8080, PTP slave, SAP/mDNS)   │       │
 │  aes67-sip gateway:                               │       │
 │    48k << >> 8k/16k resample, per-line gain/mute  │       │
 │    PJSIP: one account + one permanent call/line   ├───────┘
 │    web UI + REST API on :8081                     │  ZeroTier (preferred)
 └───────────────────────────────────────────────────┘  or public FreePBX IP
## Frozen decisions

| # | Decision | Notes / where it lands |
| --- | --- | --- |
| 1 | **4-wire, no PTT.** Endpoint audio is continuous; there is no keying signal | `call_mode: ptt` stays in the code but is not a target mode and is not configured |
| 2 | **Calls are permanent.** The call comes up and stays up until it fails; on failure the gateway re-dials every 5 s | `call_mode: permanent` (dial out) plus auto-answer for PBX-originated legs |
| 3 | **Either direction is valid:** we may dial the PBX conference, or the PBX may dial our extension and we answer | `permanent` is in the auto-answer set so an inbound INVITE is answered, then held |
| 4 | **One AES67 channel per line, one call per channel** | `aes67.channels: [n]` (mono); multichannel lines are not used |
| 5 | **G.722 is the payload** (16 kHz, 20 ms ptime), G.711u only as a fallback | `sip.codecs: ["G722/16000/1","PCMU/8000/1"]`; the router converts 48k<->16k exactly (/3) |
| 6 | **PBX is FreePBX with a conference**; the conference is the intercom matrix | One extension per line; `dial_target` points at the conference |
| 7 | **NAT-free via ZeroTier** on the appliance, or a public FreePBX IP | Installer flag `--zerotier-network <id>`; no SRTP/TLS for now (`srtp: disabled`, transport udp) |
| 8 | **AES67 streams are found by discovery (SAP/mDNS)** | The daemon browses; the gateway matches a discovered source per line and programs the sink with that SDP (`use_sdp: true`). A manual SDP override stays available |
| 9 | **Deployment: clean install on a Raspberry Pi from a single public URL, one command** | `scripts/install.sh`; installs kernel module (DKMS), daemon, PJSIP, gateway, web UI, systemd units, sysctls |
| 10 | **GPL-3.0** (PJSIP is GPLv2+, aes67-daemon is GPLv3) | `LICENSE` |
| 11 | 8 lines / 8 RAVENNA channels to start, expandable | `config/aes67-sip.conf`; the daemon supports up to 64 |
| 12 | **No echo cancellation**, gateway latency target <30 ms | Endpoints and PBX side own their echo; AEC is out of scope |

## Audio path and levels

- AES67 side: L16 48 kHz, 1 ms packets (`period_frames: 48`) on the RAVENNA device;
  ALSA PCM format `s16_le` (the driver also supports `s24_3le`/`s32_le`).
- SIP side: G.722 16 kHz mono, 20 ms ptime.
- The resampler is an integer-ratio polyphase FIR, so 48k<->16k is exact and drift
  free; the ring buffers absorb the jitter between the 1 ms AES67 cadence and 20 ms
  SIP frames.
- Per line: `gain_db` (both directions), `rx_gain_db` (PBX -> AES67), `tx_gain_db`
  (AES67 -> PBX), `mute`. A line that is not in a call sends and plays silence.

## Deployment (single command)

```
curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-sip/main/scripts/install.sh \
  | sudo bash -s -- --zerotier-network <network-id>
```

The installer is idempotent and performs, in order: OS/arch preflight (64-bit
Raspberry Pi OS or Ubuntu), dependencies, **RAVENNA kernel module built through
DKMS** (so kernel upgrades rebuild it), `aes67-daemon` + its systemd unit, PJSIP 2.17,
this gateway + web UI + systemd unit, real-time sysctls
(`kernel.sched_rt_runtime_us=1000000`, `kernel.perf_cpu_time_max_percent=0`,
`net.ipv4.igmp_max_memberships=66`), PulseAudio disabled, optional ZeroTier join, then
a preflight report (PTP lock, daemon reachable, ALSA device present).

## Open items (need confirmation before commissioning)

1. **Public URL for `curl | bash`**: the repository is public, so the documented
   command works anonymously. GPL-3.0 distribution also requires source availability.
2. **ZeroTier network id**: the installer now performs the whole ZeroTier setup
   (install, join, report the node address, wait for authorisation) - only the network
   id itself is still needed, passed as `--zerotier-network <id>` or answered at the
   prompt. The node must be authorised in `my.zerotier.com` (the installer reports
   `ACCESS_DENIED` until then).
3. **How the endpoints subscribe to our stream**: the gateway's *source* is created on
   the RAVENNA device and advertised over SAP; confirm the panels either subscribe by
   SAP/Ravenna or are configured with our multicast address by hand.
4. **Is there a PTP grandmaster on the AES67 VLAN?** Without one the RAVENNA device
   never locks and no audio flows (`GET /api/aes67/ptp/status` shows `unlocked`).
5. Panel model/brand, and the final line count at commissioning.
6. Whether the appliance gets a static IP / DHCP reservation on the AES67 VLAN
   (recommended).

```
