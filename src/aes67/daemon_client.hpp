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
};

}  // namespace aes67sip
