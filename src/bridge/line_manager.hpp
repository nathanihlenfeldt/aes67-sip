#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aes67/daemon_client.hpp"
#include "audio/router.hpp"
#include "config.hpp"
#include "matrix/intercom_matrix.hpp"
#include "sip/engine.hpp"

namespace aes67sip {

/**
 * Owns the runtime state of every configured line and ties the planes together:
 *
 *   configuration -> AES67 daemon streams (sink/source per line)
 *                 -> audio router (channels, gains, PTT detection)
 *                 -> SIP engine (one account per line, registration, calls)
 *                 -> the intercom matrix (endpoints and party lines)
 *
 * It also implements the call state machine for the different call modes
 * (`manual`, `auto_answer`, `dial_out`, `ptt`).
 */
class LineManager : public SipEngineCallback, public SipMediaSource {
 public:
  LineManager(Config* config, std::string config_path, DaemonClient* daemon,
              AudioRouter* router, SipEngine* engine);
  ~LineManager() override;

  /** Pushes the configuration to the daemon, the router and the SIP engine. */
  bool apply_configuration(std::string* error);

  /**
   * Connects the SIP engine.  The engine is created by App with `this` as its
   * callback and media source, so the pointer can only be handed over once
   * both objects exist.
   */
  void attach_engine(SipEngine* engine) { engine_ = engine; }

  /**
   * Connects the intercom matrix, which `apply_configuration` then fills from the
   * configuration's endpoints and party lines.  Optional: without it the gateway
   * only bridges its SIP lines, as before.
   */
  void attach_matrix(IntercomMatrix* matrix) { matrix_ = matrix; }

  /** Starts the line supervision thread (dial_out / ptt / auto answer). */
  bool start_supervision();
  void stop();

  int line_count() const;

  /** One line, shaped exactly like the `lines[]` entries of /api/status. */
  json line_status(int line_id) const;
  json lines_status() const;
  /**
   * The off-site conference leg, as `status.conference`: its call state, the
   * account and target it dials, why it is not up, and the levels in each
   * direction.  Present even when no leg is configured, so a UI can tell "not
   * configured" from "configured but down".
   */
  json conference_status() const;
  /** Configuration view of one line (`GET /api/lines/{id}/config`). */
  bool line_config(int line_id, json* config, std::string* error) const;

  /** Applies a partial configuration update and persists it.  Unlike the
   * conference block, a line patch accepts any key it carries (a typo is merged
   * and ignored rather than refused) - `id` excepted, which names the line. */
  bool update_line(int line_id, const json& patch, std::string* error);

  /**
   * Applies a partial update to the conference block (`enabled`, `account`,
   * `target`, `display_name`) and persists it, used by
   * `POST /api/conference/config`.  The candidate configuration is validated as a
   * whole before anything is written or dialled, so a leg that could not work -
   * enabled with no target, an account nobody declared - is refused with the file
   * and the running appliance untouched.
   */
  bool update_conference(const json& patch, std::string* error);

  /**
   * Call control for the conference leg, used by `POST /api/conference/call`.
   * `dial` raises the call now, and `hangup` drops it and holds it down: the
   * supervisor does not raise a leg the operator has just hung up until the next
   * `dial`, a change that would raise a different call (`enabled`, `account` or
   * `target`), or a restart.
   */
  bool conference_action(const std::string& action, std::string* error);

  /** Call control used by `POST /api/lines/{id}/call`. */
  bool call_action(int line_id, const std::string& action, const json& body,
                   std::string* error);

  /**
   * The commissioning test tone on a line's AES67 output channels, used by
   * `POST /api/lines/{id}/tone`.  `action` is `start` (with `hz` and `seconds` in
   * the body) or `stop`.  This is how a line is proven end to end without a PBX or
   * an endpoint: the tone leaves on the line's RAVENNA channels, so a party line
   * that carries them (or a loopback sink) makes it audible somewhere real.
   */
  bool test_tone(int line_id, const std::string& action, const json& body,
                 std::string* error);

  /** `POST /api/system/self-test` checks. */
  json self_test();

  // ---- SipEngineCallback -------------------------------------------------
  void on_media_start(int line_id, unsigned sample_rate) override;
  void on_media_stop(int line_id) override;
  void on_line_state(int line_id, LineState state, int state_code,
                     const std::string& detail) override;
  void on_incoming_call(int line_id, const std::string& remote_uri) override;

  // ---- SipMediaSource ----------------------------------------------------
  size_t pull_to_sip(int line_id, float* destination, size_t frames) override;
  void push_from_sip(int line_id, const float* source, size_t frames) override;

 private:
  struct LineRuntime {
    LineConfig config;
    /**
     * The conference leg: a line to the SIP engine whose media is the matrix's
     * conference sides instead of RAVENNA channels.  It is kept out of `lines[]`,
     * the line editor and the per-line self-test, because none of those make sense
     * for it; `conference_status()` is its view.
     */
    bool is_conference{false};
    LineState state{LineState::kIdle};
    int state_code{0};
    std::string detail;
    bool stream_configured{false};
    /** Where the sink SDP came from: pasted | discovered | loopback | none. */
    std::string sdp_source{"none"};
    int64_t ptt_hangup_at_ms{0};
    int64_t last_dial_ms{0};
    // cached daemon sink state, refreshed by the supervision thread
    bool sink_receiving{false};
    /**
     * False when the daemon has no stream on this line's sink at all — nothing is
     * configured to receive on.  Distinct from "nothing is arriving": the first is
     * a configuration gap, the second is a silent or dead endpoint.
     */
    bool sink_in_use{true};
    bool sink_error{false};
    int64_t sink_status_at_ms{0};

    /**
     * Guards every member above.  It is never held while calling into the SIP
     * engine or the daemon client, because both can call back into the line
     * manager (media callbacks, state notifications) from their own threads.
     */
    mutable std::mutex mutex;
  };

  void supervise();
  void apply_line_to_router(const LineConfig& line);
  /**
   * Creates, refreshes or removes the conference leg: a call to the off-site
   * conference whose media is the matrix's conference sides.  It is registered
   * with the SIP engine and the router like a line, so the supervisor keeps it up
   * and reports why it is not - but it owns no daemon streams and no channels.
   */
  void apply_conference();
  /** Creates or updates the daemon sink/source of a line. */
  void configure_daemon_streams(const LineConfig& line);
  void remove_daemon_streams(const LineConfig& line);
  /**
   * SDP describing the intercom endpoint's RTP stream, needed by the daemon
   * sink.  Taken from `aes67.remote_sdp`, from the discovered source selected
   * with `aes67.remote_source_id`, or (for commissioning) from our own source.
   */
  std::string resolve_endpoint_sdp(const LineConfig& line, std::string* origin,
                                   std::string* error);
  /**
   * The *endpoint's* SDP for a stream description: an inline document
   * (`remote_sdp`) or a source discovered by SAP/mDNS (`remote_source_id`).
   * Empty when neither is configured; `subject` names the owner of the setting
   * in the messages ("line 0 (Stage Left)", "endpoint pack-01 (Camera 1)").
   */
  std::string resolve_remote_sdp(const std::string& remote_sdp,
                                 const std::string& remote_source_id,
                                 const std::string& subject, std::string* origin,
                                 std::string* error);

  /**
   * Provisions every declared endpoint: one daemon stream per direction, each
   * carrying all of that direction's channels, named after the endpoint.  The
   * matrix writes those channels; this is what puts them on the wire.
   */
  void configure_endpoint_streams();

  /**
   * Deletes the streams of endpoints that are no longer configured, and records
   * the ids of the ones that are.  `allocated` is keyed by endpoint id.
   */
  void remove_endpoint_streams(const std::map<std::string, int>& allocated);

  /** Updates the cached state and notifies listeners (callbacks, log). */
  void set_state(LineRuntime& line, LineState state, int code,
                 const std::string& detail);
  std::shared_ptr<LineRuntime> find(int line_id) const;
  json sip_status_json() const;

  Config* config_{nullptr};
  std::string config_path_;
  DaemonClient* daemon_{nullptr};
  AudioRouter* router_{nullptr};
  SipEngine* engine_{nullptr};
  IntercomMatrix* matrix_{nullptr};

  /** Our own daemon's node_id: a sink SDP carrying it is our own source. */
  std::string own_node_id_;
  /** sink id -> SDP, refreshed by the sink poll (see supervise()). */
  std::map<int, std::string> sink_sdps_;
  mutable std::mutex sink_sdp_mutex_;

  mutable std::mutex mutex_;
  std::map<int, std::shared_ptr<LineRuntime>> lines_;
  /**
   * True while the conference leg was hung up by hand: the supervisor then leaves
   * it alone instead of dialling it again five seconds later.  Cleared by a dial
   * request, by a change that would raise a different call, and by a restart.
   * Guarded by `mutex_`.
   */
  bool conference_held_{false};
  /**
   * The conference block as last applied, so `apply_conference` can tell a change
   * that would raise a *different* call - which is what an operator means by
   * "try it again" - from an edit that leaves the call alone (a rename).  It is
   * what makes the hold clear the same way whichever route changed the block.
   * Guarded by `mutex_`.
   */
  ConferenceConfig applied_conference_{};
  /**
   * Daemon ids of the endpoint streams created by the last apply, so the ones
   * whose endpoint is configured away can be deleted again.  Guarded by `mutex_`.
   */
  std::map<std::string, int> endpoint_stream_ids_;

  std::thread supervisor_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace aes67sip
