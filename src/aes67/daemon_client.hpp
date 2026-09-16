#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"
#include "util.hpp"

namespace aes67sip {

/** AES67 daemon PTP slave state. */
struct PtpStatus {
  std::string status{"unknown"};  // unlocked | locking | locked
  std::string gmid;
  int jitter{0};
};

/** Per sink RTP reception flags as reported by the daemon. */
struct SinkStatus {
  /**
   * False when the daemon has no stream on this sink at all (nothing subscribed
   * or configured there) — a normal state for a stream that is hand-wired or not
   * set up yet, and the difference between "nothing is arriving" and "there is
   * nothing to arrive on".
   */
  bool in_use{true};
  bool receiving_rtp_packet{false};
  bool muted{false};
  bool rtp_seq_id_error{false};
  bool rtp_ssrc_error{false};
  bool rtp_payload_type_error{false};
  bool rtp_sac_error{false};
};

/**
 * Client for the AES67 daemon REST API (default http://127.0.0.1:8080).
 *
 * Two implementations exist:
 *  - HTTP client talking to a real `aes67-daemon`
 *  - an in-process simulation (`fake`) used when developing without the
 *    RAVENNA kernel module and in unit tests
 */
class DaemonClient {
 public:
  virtual ~DaemonClient() = default;

  /** True when the daemon answered the last request. */
  virtual bool connected() const = 0;

  /** Last transport/HTTP error, empty when healthy. */
  virtual std::string last_error() const = 0;

  /** "127.0.0.1:8080" style description for status output. */
  virtual std::string endpoint() const = 0;

  virtual bool get_version(std::string* version, std::string* error) = 0;
  virtual bool get_config(json* config, std::string* error) = 0;
  virtual bool set_config(const json& config, std::string* error) = 0;
  virtual bool get_ptp_status(PtpStatus* status, std::string* error) = 0;

  /** Raw daemon `GET /api/sinks` document. */
  virtual bool get_sinks(json* sinks, std::string* error) = 0;
  /** Raw daemon `GET /api/sources` document. */
  virtual bool get_sources(json* sources, std::string* error) = 0;

  virtual bool put_sink(int id, const json& sink, std::string* error) = 0;
  virtual bool delete_sink(int id, std::string* error) = 0;
  virtual bool put_source(int id, const json& source, std::string* error) = 0;
  virtual bool delete_source(int id, std::string* error) = 0;

  virtual bool get_sink_status(int id, SinkStatus* status, std::string* error) = 0;

  /** SDP document of a local source, used to wire sinks to our own sources. */
  virtual bool get_source_sdp(int id, std::string* sdp, std::string* error) = 0;

  /**
   * True when a daemon response means "there is no such stream" rather than a
   * failure: the daemon answers 400/404 on its per-stream paths
   * (`/api/sink/status/N`,
   * `/api/source/sdp/N`) for a sink or source that has no stream yet - a hand-wired
   * stream, or one whose SDP has not been configured.  That is a normal state and
   * must not be reported as the daemon being unreachable: the gateway polls sink
   * status every couple of seconds for every configured line, and blaming the
   * daemon for the ones it has no stream for made the UI flash a daemon error.
   */
  static bool stream_absent(int http_status);

  /** kind is one of "all", "mdns", "sap". */
  virtual bool browse_sources(const std::string& kind, json* sources,
                              std::string* error) = 0;

  /** Factory: HTTP client, or the simulation when `config.fake` is set. */
  static std::unique_ptr<DaemonClient> create(const Aes67DaemonConfig& config);

  /**
   * Builds the sink/source JSON documents for a line, following the daemon
   * schema (see docs/architecture.md).
   *
   * A *source* is the gateway -> endpoint direction and can always be created
   * from the line configuration.  A *sink* describes the endpoint's own RTP
   * stream, so it needs the endpoint SDP: either picked up from SAP/mDNS
   * discovery (`line.aes67.remote_source_id`) or supplied inline
   * (`line.aes67.remote_sdp`).  When neither is available the sink is not
   * created and `error` explains why.
   */
  static json make_source(const Aes67DaemonConfig& daemon_config,
                          const LineConfig& line);
  static bool make_sink(const Aes67DaemonConfig& daemon_config,
                        const LineConfig& line, const std::string& remote_sdp,
                        json* sink, std::string* error);

  /**
   * Names of the two streams an endpoint is provisioned with.  Both carry the
   * endpoint's own name and end in the direction the *endpoint* sees: `(talk)`
   * for the stream it sends, `(listen)` for the one it receives.  That is what
   * lets a human match a stream to an endpoint in a vendor's routing grid
   * without a lookup table, so the names are part of the contract, not a log
   * string.
   */
  static std::string endpoint_talk_stream_name(const EndpointConfig& endpoint);
  static std::string endpoint_listen_stream_name(const EndpointConfig& endpoint);

  /**
   * One *source* for an endpoint: its listen channels, carrying all of them in a
   * single stream.  Built from the endpoint's declared shape, so a two channel
   * beltpack and a many channel console take the same path.
   */
  static json make_endpoint_source(const Aes67DaemonConfig& daemon_config,
                                   const EndpointConfig& endpoint);

  /**
   * One *sink* for an endpoint: its talk channels, carrying all of them in a
   * single stream.  Like a line's sink it needs the endpoint's own SDP; without
   * one there is nothing to subscribe to and `error` says so.
   */
  static bool make_endpoint_sink(const Aes67DaemonConfig& daemon_config,
                                 const EndpointConfig& endpoint,
                                 const std::string& remote_sdp, json* sink,
                                 std::string* error);
};

}  // namespace aes67sip
