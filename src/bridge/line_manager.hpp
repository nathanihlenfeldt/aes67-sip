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
#include "sip/engine.hpp"

namespace aes67sip {

/**
 * Owns the runtime state of every configured line and ties the three planes
 * together:
 *
 *   configuration -> AES67 daemon streams (sink/source per line)
 *                 -> audio router (channels, gains, PTT detection)
 *                 -> SIP engine (one account per line, registration, calls)
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

  /** Starts the line supervision thread (dial_out / ptt / auto answer). */
  bool start_supervision();
  void stop();

  int line_count() const;

  /** One line, shaped exactly like the `lines[]` entries of /api/status. */
  json line_status(int line_id) const;
  json lines_status() const;
  /** Configuration view of one line (`GET /api/lines/{id}/config`). */
  bool line_config(int line_id, json* config, std::string* error) const;

  /** Applies a partial configuration update and persists it. */
  bool update_line(int line_id, const json& patch, std::string* error);

  /** Call control used by `POST /api/lines/{id}/call`. */
  bool call_action(int line_id, const std::string& action, const json& body,
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
    LineState state{LineState::kIdle};
    int state_code{0};
    std::string detail;
    bool stream_configured{false};
    int64_t ptt_hangup_at_ms{0};
    int64_t last_dial_ms{0};
    // cached daemon sink state, refreshed by the supervision thread
    bool sink_receiving{false};
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
  /** Creates or updates the daemon sink/source of a line. */
  void configure_daemon_streams(const LineConfig& line);
  void remove_daemon_streams(const LineConfig& line);
  /**
   * SDP describing the intercom endpoint's RTP stream, needed by the daemon
   * sink.  Taken from `aes67.remote_sdp`, from the discovered source selected
   * with `aes67.remote_source_id`, or (for commissioning) from our own source.
   */
  std::string resolve_endpoint_sdp(const LineConfig& line, std::string* error);
  /**
   * Resolves the *endpoint's* SDP for a line: an inline document
   * (`aes67.remote_sdp`) or a source discovered by SAP/mDNS
   * (`aes67.remote_source_id`).
   */
  bool resolve_remote_sdp(const LineConfig& line, std::string* sdp,
                          std::string* error);
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

  mutable std::mutex mutex_;
  std::map<int, std::shared_ptr<LineRuntime>> lines_;

  std::thread supervisor_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
};

}  // namespace aes67sip
