#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace aes67sip {

/** PCM sample format used on the RAVENNA/ALSA device. */
enum class PcmFormat { kS16Le, kS24_3Le, kS32Le };

/** Which audio backend to instantiate. */
enum class AudioBackendKind { kRavenna, kNull };

struct AudioConfig {
  std::string backend{"ravenna"};      // ravenna | null
  std::string device{"plughw:RAVENNA"};
  unsigned sample_rate{48000};         // AES67 native rate, do not change
  unsigned channels{16};               // channels opened on the RAVENNA device
  unsigned period_frames{48};          // 1 ms at 48 kHz, matches the daemon
  unsigned periods{3};
  std::string format{"s16_le"};        // s16_le | s24_3le | s32_le
  double null_tone_hz{0.0};            // null backend test tone (0 = silence)

  AudioBackendKind backend_kind() const;
  PcmFormat pcm_format() const;
};

/**
 * Connection to the AES67 daemon REST API.  `fake` runs the in-process
 * simulation used for development on machines without the RAVENNA driver.
 */
struct Aes67DaemonConfig {
  std::string address{"127.0.0.1"};
  int port{8080};
  bool fake{false};
  bool auto_configure{true};           // create/update streams from line config
  unsigned sink_delay_samples{576};
  int source_payload_type{98};
  int source_ttl{15};
  int source_dscp{34};
  std::string rtp_mcast_base{"239.1.0.1"};
};

/** SIP registrar credentials shared by one or more lines. */
struct SipAccountConfig {
  std::string id{"pbx"};
  std::string registrar{"sip:pbx.example.com"};
  std::string username{"site-gateway"};
  std::string password;
  std::string auth_realm;
  std::string outbound_proxy;
  std::string display_name{"AES67 Gateway"};
};

struct SipConfig {
  bool enabled{true};
  std::string engine{"pjsip"};         // pjsip | fake
  std::string transport{"udp"};        // udp | tcp | tls
  int local_port{5060};
  std::vector<std::string> codecs{"G722/16000/1", "PCMU/8000/1", "PCMA/8000/1"};
  int ptime_ms{20};
  int registration_timeout{300};
  int retry_interval{30};
  int keep_alive_interval{15};
  std::string stun_server;
  std::string srtp{"disabled"};        // disabled | optional | mandatory
};

/** AES67 side of a line: which RAVENNA channels carry the intercom audio. */
struct LineAes67Config {
  int sink_id{0};                      // daemon sink = endpoint -> gateway
  int source_id{0};                    // daemon source = gateway -> endpoint
  std::vector<unsigned> channels{0, 1};
  std::string stream_name;             // defaults to the line name
  bool auto_create_streams{true};
  /**
   * Where the *endpoint's* stream description comes from: the sink has to
   * describe the intercom endpoint's RTP stream, so it is either taken from a
   * discovered source (SAP/mDNS id as listed by the daemon) or pasted in.
   */
  std::string remote_source_id;
  std::string remote_sdp;
  bool ignore_refclk_gmid{false};      // skip the SDP/PTP grandmaster check
};

/**
 * How a SIP call for a line is brought up:
 *  - manual      : operator dials/answers from the UI
 *  - auto_answer : inbound calls are answered immediately
 *  - dial_out    : the gateway dials `dial_target` and keeps the call up
 *  - ptt         : like dial_out but the call is raised by AES67 input energy
 *                  and torn down after `ptt_hangup_ms` of silence
 */
struct LineSipConfig {
  std::string account{"pbx"};
  std::string extension;               // local AOR; defaults to account username
  std::string display_name;
  std::string call_mode{"manual"};
  std::string dial_target;
  int auto_answer_code{200};
  double ptt_threshold_dbfs{-40.0};
  int ptt_hangup_ms{1500};
};

struct LineConfig {
  int id{0};
  std::string name;
  bool enabled{true};
  double gain_db{0.0};                 // applied to both directions
  double rx_gain_db{0.0};              // remote -> AES67
  double tx_gain_db{0.0};              // AES67 -> remote
  bool mute{false};
  LineAes67Config aes67;
  LineSipConfig sip;
};

/**
 * Full gateway configuration.  Serialised 1:1 to the JSON config file
 * (see config/aes67-sip.conf) and to `GET/POST /api/config`.
 */
struct Config {
  int log_severity{2};
  std::string http_addr{"0.0.0.0"};
  int http_port{8081};
  std::string webui_dir{"/usr/local/share/aes67-sip/webui"};
  bool webui_api_auth{false};          // reserved: HTTP basic auth for the API
  AudioConfig audio;
  Aes67DaemonConfig aes67_daemon;
  SipConfig sip;
  std::vector<SipAccountConfig> accounts;
  std::vector<LineConfig> lines;

  json to_json() const;
  static Config from_json(const json& document);

  /** Loads a config file; returns false and fills `error` when unreadable. */
  static bool load(const std::string& path, Config* config, std::string* error);

  /** Writes the config file (pretty printed). */
  bool save(const std::string& path, std::string* error) const;

  /** Applies a partial JSON document (object merge, arrays replace). */
  void merge(const json& patch);

  /** Returns the account with `id`, or nullptr. */
  const SipAccountConfig* find_account(const std::string& id) const;

  /** Returns the line with `id`, or nullptr. */
  const LineConfig* find_line(int id) const;

  /** Index in `lines` of the line with `id`, or -1. */
  int line_index(int id) const;

  /** Parses a single line document (used by partial line updates). */
  static LineConfig parse_line(const json& document);
};

}  // namespace aes67sip
