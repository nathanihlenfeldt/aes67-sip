# aes67-sip REST API contract (v0.1.0)

The gateway exposes its own REST API (default `http://0.0.0.0:8081`) and also serves the
built web UI from `/` when `webui_dir` is configured and exists.

All request/response bodies are `application/json` unless stated otherwise.
Errors use the HTTP status code plus a `text/plain` body with a human readable message
(this deliberately matches the aes67-daemon API style).

The web UI polls `GET /api/status` every 500 ms. There is no websocket/SSE endpoint.

---

## 1. `GET /api/version`

```json
{ "name": "aes67-sip", "version": "0.1.0", "build": "pjsip=1 alsa=1" }
```

## 2. `GET /api/config`

Returns the full runtime configuration (see `config/aes67-sip.conf` for the schema).

## 3. `POST /api/config`

Body: a full or partial config document (object merge, arrays are replaced wholesale).
Returns the effective configuration after the change. Fields that require a restart are
reported as changed but take effect only after `POST /api/system/restart`.

The merged document is validated before anything is written or applied, and a
configuration that could not work is refused with `400` and a plain text reason naming
the offending lines, channels, bindings or totals — two party lines with the same name
or id, two endpoints with the same id, two endpoints claiming one device channel, a
member channel outside the shape its endpoint declared, a member binding that names an
endpoint nobody declares, or declared endpoint shapes wider than `audio.channels` opens.
On a refusal neither the configuration file nor the running appliance is touched, so the
reason can be fixed and resubmitted. The web UI shows the body as `save failed: …`
(Settings) or `not saved: …` (the party-lines page, which also marks the entry the reason
names).

The file is written *before* the change is applied: if it cannot be written the request
fails with `500` and the reason, and neither the file nor the running configuration
changes. A save that succeeds is applied through the same restart handler
`POST /api/system/restart` uses; if *applying* it fails the request fails with `500` but
the change stays in the file, so the next restart applies it.

## 4. `GET /api/status`

Single poll endpoint used by the UI.

```json
{
  "version": "0.1.0",
  "uptime_sec": 1234,
  "audio": {
    "backend": "ravenna",
    "device": "plughw:RAVENNA",
    "sample_rate": 48000,
    "channels": 16,
    "period_frames": 48,
    "state": "running",
    "error": "",
    "rx_overruns": 0,
    "tx_underruns": 0,
    "levels_dbfs": [-60.0, -12.3, "-inf", 0.0]
  },
  "aes67": {
    "connected": true,
    "address": "127.0.0.1:8080",
    "error": "",
    "ptp": { "status": "locked", "gmid": "00-10-4B-FF-FE-7A-87-FC", "jitter": 0 },
    "sinks": [
      { "id": 0, "name": "Stage Left", "receiving": true, "muted": false,
        "map": [0, 1], "seq_error": false, "ssrc_error": false, "pt_error": false }
    ],
    "sources": [
      { "id": 0, "name": "Stage Left", "enabled": true, "map": [0, 1],
        "address": "239.1.0.1", "payload_type": 98 }
    ]
  },
  "sip": {
    "engine": "pjsip",
    "enabled": true,
    "local_port": 5060,
    "transport": "udp",
    "accounts": [
      { "id": "pbx", "state": "registered", "uri": "sip:site-gw@pbx.example.com",
        "error": "" }
    ]
  },
  "lines": [
    {
      "id": 0,
      "name": "Stage Left",
      "enabled": true,
      "state": "in_call",
      "state_code": 200,
      "call": { "remote_uri": "sip:1001@pbx.example.com", "duration_sec": 42,
                "last_error": "" },
      "aes67": { "sink_id": 0, "source_id": 0, "channels": [0], "receiving": true,
                 "error": false, "sdp_source": "discovered" },
      "levels": { "rx_dbfs": -18.2, "tx_dbfs": -60.0,
                  "sip_rx_dbfs": -22.0, "sip_tx_dbfs": -18.2 },
      "gain_db": 0.0,
      "mute": false,
      "ptt": false,
      "test_tone": false
    }
  ]
}
```

`state` is one of `disabled`, `idle`, `dialing`, `ringing`, `in_call`, `error`.
Levels are dBFS floats; silence is serialised as `null` (including the meter floor, so a
level that has been quiet for a while reads as `null` rather than as a huge negative
number). `test_tone` says whether the commissioning tone is currently on that line's AES67
output channels (see `POST /api/lines/{id}/tone`).

`conference` is the off-site conference leg — one call whose media is the matrix's
conference sides rather than a line's channels. `state` is the same vocabulary as a line
(`disabled` / `idle` / `dialing` / `ringing` / `in_call` / `error`), so an operator can
tell "nobody is on the off-site call" (`idle`, `dialing`) from "the link is broken"
(`error`, with `detail` and `state_code` carrying the reason the PBX gave). `enabled` is
false when no leg is configured, which is also how a site that never uses one reports
itself. The levels are named for the conference, not for an endpoint: `to_conference_dbfs`
is the mix the matrix builds for it (what it hears) and `from_conference_dbfs` is what it
sent into the site's mixes. The party lines that claim it are the `party_lines[]` entries
with `claims_conference`.

```json
"conference": {
  "enabled": true, "name": "Conference", "account": "pbx",
  "target": "sip:conf@pbx.example.com",
  "state": "in_call", "state_code": 200, "detail": "answered",
  "levels": { "to_conference_dbfs": -18.0, "from_conference_dbfs": -21.5 }
}
```

`party_lines[]` reports the intercom matrix: each party line with its members, and
for each member the endpoint that owns it, the endpoint's own channel indices it
bound (`talk_channel` / `listen_channel`, each `-1` where that direction is
unbound), that member's contribution level, the held level of its audio and whether
that audio is currently arriving. `claims_conference` says whether that line's
members are on the off-site call, so the choice is visible without reading the
configuration. It is an empty array when no party lines are declared, which is also
how a gateway that only bridges SIP lines reports itself.

Each line also carries the derived `summary` the dashboard and the self-test read, so
"who has gone silent" is answered one way: `members`, `arriving`, `silent` (a talk channel
is bound but nothing is arriving) and `unbound` (no talk channel bound, so that member
cannot be heard on this line at all), with `arriving_names` / `silent_names` /
`unbound_names` naming them. `state` is the one word to show: `active` (someone is being
heard), `quiet` (the line works and nobody is talking) or `cannot_be_heard` (no member has
a talk channel bound - the one state that is broken rather than silent). `can_be_heard` and
`quiet` are the same two facts as booleans.

A line whose members are *all* silent is `quiet`, and the API does not claim more than the
appliance knows: with per-member level meters alone, a quiet room and every cable on that
line being pulled look the same. What makes it findable is the names - `silent_names`
carries every member that has gone quiet - rather than a verdict the appliance cannot
support.

```json
"party_lines": [
  { "id": "cameras", "name": "Cameras", "claims_conference": true,
    "state": "active", "can_be_heard": true, "quiet": false,
    "summary": { "members": 2, "arriving": 1, "silent": 1, "unbound": 0,
                 "arriving_names": ["Camera 1"], "silent_names": ["Camera 2"],
                 "unbound_names": [] },
    "members": [
      { "endpoint": "a", "name": "Camera 1", "talk_channel": 0, "listen_channel": 0,
        "contribution_db": 0.0, "level_dbfs": -6.0, "arriving": true },
      { "endpoint": "b", "name": "Camera 2", "talk_channel": 1, "listen_channel": 1,
        "contribution_db": -3.0, "level_dbfs": -1000.0, "arriving": false }
    ] }
]
```

`audio.error` is empty while the audio path is healthy. When the RAVENNA device
cannot be opened it holds the reason and `audio.state` is `stopped`; the gateway
keeps serving this API (and retries the audio path every 5 s) instead of exiting,
so the UI stays usable while the audio problem is diagnosed.

`sip.accounts[].code` is the SIP status of the last registration: `200`
registered, `401` credentials rejected, `403` refused by the PBX, `408` no answer,
`502` the registrar name could not be resolved. `sip.accounts[].error` keeps the
reason of the last failure (it is not cleared by a later empty update).

`aes67.sdp_source` says where the line's sink SDP came from: `pasted` (from
`aes67.remote_sdp`) or `discovered` (from SAP/mDNS) bridge the real endpoint;
`loopback` means `aes67.commissioning_loopback` is on and the sink is subscribed
to our own source - a bench aid that receives no endpoint audio; `unmanaged` means
no endpoint SDP is configured so the gateway leaves the sink alone (it may have
been wired up in Dante Controller, Q-SYS or the daemon UI); `external` means the
daemon reports a sink subscription that is not our own source, i.e. a real
endpoint was wired to that channel (the gateway's own supervision of a line that
has no SDP configured); `none` means there is no sink at all.

Level directions: `rx_dbfs` / `tx_dbfs` are measured at the **AES67 (on site)
side** of the gateway - `rx_dbfs` is what the intercom endpoint sends us,
`tx_dbfs` is what we send back to it. `sip_rx_dbfs` is audio received from the
PBX and `sip_tx_dbfs` is audio sent to the PBX, so `sip_tx_dbfs` (before gain)
tracks `rx_dbfs`. `audio.levels_dbfs[]` are the per RAVENNA channel capture
levels and `audio.playback_dbfs[]` the per channel playback levels; both are
used by the diagnostics page.

`aes67.receiving` comes from the daemon's sink status (`receiving_rtp_packet`)
and is refreshed every 2 s, `aes67.error` aggregates the RTP error flags
(sequence/SSRC/payload type/timestamp).

## 5. Lines

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/api/lines` | array of line status objects (same shape as `status.lines`) |
| `GET` | `/api/lines/{id}` | one line status object |
| `GET` | `/api/lines/{id}/config` | line configuration object |
| `POST` | `/api/lines/{id}/config` | partial line configuration update (persisted); returns the updated line **status** object |
| `POST` | `/api/lines/{id}/call` | call control, see below |
| `POST` | `/api/lines/{id}/tone` | the commissioning test tone on the line's AES67 output channels, see below |
| `GET` | `/api/lines/{id}/levels` | `{ "rx_dbfs": -18.2, "tx_dbfs": -60.0, "sip_rx_dbfs": -60.0, "sip_tx_dbfs": -18.2 }` |

`POST /api/lines/{id}/tone` puts a tone on the RAVENNA **output** channels of one line -
what the daemon publishes as that line's source - which is how an outbound path is proved
without a PBX call or an endpoint: subscribe an endpoint (or the daemon) to that source and
listen, or subscribe the appliance's own sink back to it (`aes67.commissioning_loopback`)
so the tone returns as a *capture* channel that a party line's members can then be bound
to.  It is written only to that line's own channels, and only while the line has at least
one channel inside the opened device. Body: `{ "action": "start" | "stop", "hz": 1000,
"seconds": 5 }`; a bare `{}` starts the commissioning default (1 kHz for five seconds). The
reply is `{ "ok": true, "line": <line status> }`, and `lines[].test_tone` says whether a
tone is running. An unknown line, the conference leg, an unknown action, a value of the
wrong type, a frequency outside 20-20000 Hz, a duration outside 0.1-600 s, or a line with
no channel on the device is refused with `400` and the reason.

`POST /api/lines/{id}/config` accepts:

```json
{ "enabled": true, "gain_db": -3.0, "mute": false, "ptt": false,
  "sip": { "call_mode": "ptt", "dial_target": "1001" },
  "aes67": { "channels": [2, 3], "codec": "L24" } }
```

`aes67.codec` is the RTP payload our *source* advertises: `L24` (default, Dante
and most AES67 devices), `L16`, `L2432`, `AM824` or `L32`. The sink's payload is
not configurable here because it comes from the endpoint's SDP.
`aes67.commissioning_loopback` (default `false`) subscribes the sink to our own
source for bench testing when no endpoint SDP is configured.

`POST /api/lines/{id}/call` body:

```json
{ "action": "dial", "target": "sip:1001@pbx.example.com" }
```

`action` is one of:

| action | body fields | effect |
| --- | --- | --- |
| `dial` | `target` (optional, defaults to `sip.dial_target`) | place an outbound call |
| `answer` | – | answer an inbound call |
| `hangup` | – | drop the current call |
| `hold` | `hold` (bool, default true) | hold/resume |
| `dtmf` | `digits` | send RFC2833 DTMF |

Response: `{ "ok": true, "line": <line status object> }`.

## 6. AES67 daemon passthrough

These endpoints proxy (and normalise) the `aes67-daemon` REST API so the UI has a single
origin. `{id}` is 0-63.

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/api/aes67/config` | daemon config |
| `POST` | `/api/aes67/config` | daemon config update |
| `GET` | `/api/aes67/ptp/status` | `{ "status": "...", "gmid": "...", "jitter": 0 }` |
| `GET` | `/api/aes67/sinks` | daemon `GET /api/sinks` |
| `GET` | `/api/aes67/sources` | daemon `GET /api/sources` |
| `PUT` | `/api/aes67/sinks/{id}` | create/update sink |
| `DELETE` | `/api/aes67/sinks/{id}` | remove sink |
| `PUT` | `/api/aes67/sources/{id}` | create/update source |
| `DELETE` | `/api/aes67/sources/{id}` | remove source |
| `GET` | `/api/aes67/browse/sources/{all\|mdns\|sap}` | discovered remote sources |
| `GET` | `/api/aes67/source/sdp/{id}` | SDP of a local source (for sink wiring) |

## 7. System

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/api/log?lines=200` | `{ "lines": ["..."] }` tail of the in-memory log |
| `POST` | `/api/system/restart` | restart audio + SIP engines (config reload) |
| `POST` | `/api/system/self-test` | run built-in checks, see below |

`POST /api/system/self-test` returns:

```json
{ "ok": true, "checks": [
  { "name": "audio backend", "ok": true, "detail": "plughw:RAVENNA 16ch @48000" },
  { "name": "aes67 daemon", "ok": true, "detail": "version 0.9.0" },
  { "name": "ptp", "ok": false, "detail": "unlocked" },
  { "name": "sip account pbx", "ok": true, "detail": "registered" },
  { "name": "line 0 loopback", "ok": true, "detail": "rx -18.2 dBFS" } ] }
```

## 8. Command line

```
aes67-sip [-c <config file>] [-a <http addr>] [-p <http port>] [-f] [-v] [-h]
```

* `-c` configuration file (default `/etc/aes67-sip.conf`)
* `-p` override HTTP port, `-a` override HTTP bind address
* `-f` force fake mode: null audio backend + stub SIP engine + fake AES67 daemon
* `-v` print version, `-d` raise log severity to debug
