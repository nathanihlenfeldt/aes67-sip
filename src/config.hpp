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
  std::string backend{"ravenna"};  // ravenna | null
  std::string device{"plughw:RAVENNA"};
  unsigned sample_rate{48000};  // AES67 native rate, do not change
  unsigned channels{16};        // channels opened on the RAVENNA device
  unsigned period_frames{48};   // 1 ms at 48 kHz, matches the daemon
  unsigned periods{8};
  std::string format{"s24_3le"};  // s16_le | s24_3le | s32_le
  double null_tone_hz{0.0};       // null backend test tone (0 = silence)

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
  bool auto_configure{true};  // create/update streams from line config
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
  std::string engine{"pjsip"};   // pjsip | fake
  std::string transport{"udp"};  // udp | tcp | tls
  int local_port{5060};
  std::vector<std::string> codecs{"G722/16000/1", "PCMU/8000/1", "PCMA/8000/1"};
  int ptime_ms{20};
  int registration_timeout{300};
  int retry_interval{30};
  int keep_alive_interval{15};
  std::string stun_server;
  std::string srtp{"disabled"};  // disabled | optional | mandatory
  /**
   * pjsip log level, 0-5.  Anything above 0 makes pjsip write the SIP messages it
   * sends and receives to stderr (see `journalctl -u aes67-sip`), which is how you
   * inspect a 401/403 challenge and the credentials that answered it.
   */
  int debug_log_level{0};
};

/** AES67 side of a line: which RAVENNA channels carry the intercom audio. */
struct LineAes67Config {
  int sink_id{0};    // daemon sink = endpoint -> gateway
  int source_id{0};  // daemon source = gateway -> endpoint
  std::vector<unsigned> channels{0, 1};
  std::string stream_name;  // defaults to the line name
  bool auto_create_streams{true};
  /**
   * RTP payload our *source* advertises: L24 (default) or L16.  Dante and most
   * modern AES67 devices use L24, which is also the aes67-daemon's own default.
   * The sink's payload is not configurable here: it comes from the endpoint's
   * SDP.  Supported values: L16, L24, L2432, AM824, L32.
   */
  std::string codec{"L24"};
  /**
   * Where the *endpoint's* stream description comes from: the sink has to
   * describe the intercom endpoint's RTP stream, so it is either taken from a
   * discovered source (SAP/mDNS id as listed by the daemon) or pasted in.
   */
  std::string remote_source_id;
  std::string remote_sdp;
  bool ignore_refclk_gmid{false};  // skip the SDP/PTP grandmaster check
  /**
   * Advertise `a=ts-refclk:ptp=IEEE1588-2008:traceable` in our source's SDP
   * instead of the explicit grandmaster clock ID.  Default off: receivers that
   * check the announced reference against the PTP domain's grandmaster - Q-SYS
   * reports this as a "grandmaster mismatch" and refuses the flow - need the
   * explicit ID we are actually locked to.  Only set it to true for receivers
   * that insist on the RFC 7273 `traceable` keyword.
   */
  bool refclk_ptp_traceable{false};
  /**
   * Commissioning aid: when no endpoint SDP is configured, subscribe the sink to
   * our *own* source rather than leaving it alone.  No audio comes back through
   * that subscription on the reference appliance - the daemon does not loop its own
   * multicast into its own receiver (measured on the Pi: the RTP leaves the
   * interface while the local sink reports no reception) - so it exercises the
   * sink's configuration path, not the audio.  Off by default, because the gateway
   * must never overwrite a sink that was wired up elsewhere (Dante Controller,
   * Q-SYS, the daemon UI) with a loopback.
   */
  bool commissioning_loopback{false};
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
  std::string extension;  // local AOR; defaults to account username
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
  double gain_db{0.0};     // applied to both directions
  double rx_gain_db{0.0};  // remote -> AES67
  double tx_gain_db{0.0};  // AES67 -> remote
  bool mute{false};
  LineAes67Config aes67;
  LineSipConfig sip;
};

/**
 * The AES67 streams one endpoint is provisioned with: **one per direction**,
 * each carrying every channel of that direction rather than one stream per
 * channel.
 *
 * The *sink* is the endpoint's talk stream (endpoint -> appliance); like a line's
 * sink it describes the endpoint's own RTP stream, so it needs that endpoint's
 * SDP - discovered by SAP/mDNS or pasted in.  The *source* is the endpoint's
 * listen stream (appliance -> endpoint), carrying the mixes the matrix writes for
 * it, and needs no remote description at all.
 */
struct EndpointAes67Config {
  /**
   * Base of both stream names; defaults to the endpoint's name.  The names carry
   * the endpoint so a human can match a stream to an endpoint in a vendor's
   * routing grid without a lookup table (see `DaemonClient`).
   */
  std::string stream_name;
  bool auto_create_streams{true};
  std::string codec{"L24"};      // payload our *source* advertises (L16, L24, ...)
  std::string remote_source_id;  // discovered SAP/mDNS source: the endpoint's talk
  std::string remote_sdp;        // ...or that stream's SDP pasted in
  bool ignore_refclk_gmid{false};
  bool refclk_ptp_traceable{false};
};

/**
 * Reserved line id of the conference leg.
 *
 * The leg is a line to the SIP engine - the same state machine, retries and media
 * handoff as any other - but not to the device: its media is the matrix's
 * conference sides rather than RAVENNA channels.  It therefore carries an id no
 * configured line may use (see `validate_configuration`).
 */
constexpr int kConferenceLineId = -1;

/**
 * The off-site conference: one call whose media is the matrix's conference sides,
 * independent of the per-line legs.
 *
 * `target` is who the gateway dials; the leg is kept up and retried like a
 * `dial_out` line.  Disabled by default, so a site that does not use it behaves
 * exactly as before.
 */
struct ConferenceConfig {
  bool enabled{false};
  std::string account{"pbx"};
  std::string target;  // e.g. sip:conference@pbx.example.com
  std::string display_name{"Conference"};
};

/**
 * An endpoint on the site, and the shape it presents to the matrix.
 *
 * Nothing here names a device model: an endpoint says how many talk channels it
 * sends and how many listen channels it receives, and the channels are the
 * RAVENNA/ALSA channels those map onto. A two-channel beltpack and a console with
 * many channels are the same thing to the matrix, differing only in these counts.
 */
struct EndpointConfig {
  std::string id;                         // stable key that party lines refer to
  std::string name;                       // defaults to the id
  std::vector<unsigned> talk_channels;    // device capture channels, one each
  std::vector<unsigned> listen_channels;  // device playback channels, one each
  EndpointAes67Config aes67;              // the two streams it is provisioned
};

/**
 * One endpoint's place on a party line.
 *
 * `talk_channel` and `listen_channel` index the endpoint's own declared channels
 * rather than device channels, and either may be absent (-1): an endpoint can hear
 * a line without talking on it, or talk without hearing it. `contribution_db`
 * trims what the others hear and never changes what this endpoint hears; `mute`
 * stops it being heard entirely without removing it from the line.
 */
struct PartyLineMemberConfig {
  std::string endpoint;
  int talk_channel{-1};
  int listen_channel{-1};
  double contribution_db{0.0};
  bool mute{false};
};

/** A named bus with a set of members. */
struct PartyLineConfig {
  std::string id;
  std::string name;  // defaults to the id
  /**
   * Whether this line claims the conference: its members then hear what arrives
   * on the SIP leg, and the conference hears them.  Per line, so one department,
   * several or all of them can be on the off-site call.
   */
  bool claims_conference{false};
  std::vector<PartyLineMemberConfig> members;
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
  bool webui_api_auth{false};  // reserved: HTTP basic auth for the API
  AudioConfig audio;
  Aes67DaemonConfig aes67_daemon;
  SipConfig sip;
  std::vector<SipAccountConfig> accounts;
  std::vector<LineConfig> lines;
  /** The off-site conference: one call whose media is the matrix's conference
   * sides. */
  ConferenceConfig conference;
  /** The intercom matrix: the site's endpoints and the party lines they share. */
  std::vector<EndpointConfig> endpoints;
  std::vector<PartyLineConfig> party_lines;

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
