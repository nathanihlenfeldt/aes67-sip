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
      "aes67": { "sink_id": 0, "source_id": 0, "channels": [0, 1], "receiving": true },
      "levels": { "rx_dbfs": -18.2, "tx_dbfs": -60.0 },
      "gain_db": 0.0,
      "mute": false,
      "ptt": false
    }
  ]
}
```

`state` is one of `disabled`, `idle`, `dialing`, `ringing`, `in_call`, `error`.
Levels are dBFS floats; `-inf` is serialised as `null`.

## 5. Lines

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/api/lines` | array of line status objects (same shape as `status.lines`) |
| `GET` | `/api/lines/{id}` | one line status object |
| `GET` | `/api/lines/{id}/config` | line configuration object |
| `POST` | `/api/lines/{id}/config` | partial line configuration update (persisted) |
| `POST` | `/api/lines/{id}/call` | call control, see below |
| `GET` | `/api/lines/{id}/levels` | `{ "rx_dbfs": -18.2, "tx_dbfs": -60.0 }` |

`POST /api/lines/{id}/config` accepts:

```json
{ "enabled": true, "gain_db": -3.0, "mute": false, "ptt": false,
  "sip": { "call_mode": "ptt", "dial_target": "1001" },
  "aes67": { "channels": [2, 3] } }
```

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
