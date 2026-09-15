#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "util.hpp"

namespace aes67sip {

/** Lifecycle state of a line, as reported to the UI. */
enum class LineState {
  kDisabled,
  kIdle,
  kDialing,
  kRinging,
  kInCall,
  kError,
};

/** "disabled" / "idle" / "dialing" / "ringing" / "in_call" / "error" */
const char* to_string(LineState state);

/** Registration state of one SIP account. */
struct AccountStatus {
  std::string id;
  std::string state{
      "unregistered"};  // registered | registering | unregistered | error
  std::string uri;
  std::string error;
};

/** State of the (single) call of a line. */
struct CallStatus {
  bool active{false};
  std::string remote_uri;
  std::string state;  // idle | calling | ringing | in_call | ...
  int state_code{0};
  int duration_sec{0};
  std::string last_error;
};

/**
 * Handoff between the SIP media ports and the audio router.  Implemented by the
 * LineManager; called from the SIP media thread.
 */
class SipMediaSource {
 public:
  virtual ~SipMediaSource() = default;
  /** Mono samples to send to the remote, at the negotiated rate. */
  virtual size_t pull_to_sip(int line_id, float* destination, size_t frames) = 0;
  /** Mono samples received from the remote, at the negotiated rate. */
  virtual void push_from_sip(int line_id, const float* source, size_t frames) = 0;
};

/** Events pushed by the SIP engine towards the LineManager. */
class SipEngineCallback {
 public:
  virtual ~SipEngineCallback() = default;
  /** The negotiated codec rate for this call is known; media is now flowing. */
  virtual void on_media_start(int line_id, unsigned sample_rate) = 0;
  virtual void on_media_stop(int line_id) = 0;
  virtual void on_line_state(int line_id, LineState state, int state_code,
                             const std::string& detail) = 0;
  /** An inbound call arrived on one of the line's accounts. */
  virtual void on_incoming_call(int line_id, const std::string& remote_uri) = 0;
};

/**
 * SIP engine abstraction.
 *
 * Two implementations:
 *  - PJSIP (pjsua2) when built with WITH_PJSIP and `sip.engine == "pjsip"`
 *  - a stub engine used for development/tests and when SIP is disabled, which
 *    simulates registration and call setup without any network signalling
 */
class SipEngine {
 public:
  virtual ~SipEngine() = default;

  /** "pjsip" or "stub". */
  virtual std::string engine_name() const = 0;
  virtual bool start(std::string* error) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;

  /** (Re)creates the accounts from configuration. */
  virtual bool reload_accounts(const std::vector<SipAccountConfig>& accounts,
                               std::string* error) = 0;
  virtual std::vector<AccountStatus> account_status() const = 0;

  /** Registers a line: its AOR, credentials and attach point for calls. */
  virtual bool add_line(const LineConfig& line, std::string* error) = 0;
  virtual bool remove_line(int line_id) = 0;

  virtual bool dial(int line_id, const std::string& target, std::string* error) = 0;
  virtual bool answer(int line_id, std::string* error) = 0;
  virtual bool hangup(int line_id, std::string* error) = 0;
  virtual bool set_hold(int line_id, bool hold, std::string* error) = 0;
  virtual bool send_dtmf(int line_id, const std::string& digits,
                         std::string* error) = 0;

  virtual CallStatus call_status(int line_id) const = 0;

  /**
   * Creates the engine selected by the configuration.  Falls back to the stub
   * engine (and fills `error` with the reason) when PJSIP is unavailable.
   */
  static std::unique_ptr<SipEngine> create(const SipConfig& config,
                                           SipEngineCallback* callback,
                                           SipMediaSource* media,
                                           std::string* error);
};

/**
 * Development / test engine.
 *
 * Simulates registration and call setup, and loops the line's AES67 audio back
 * to itself so the complete routing chain can be validated without a PBX.
 */
class StubSipEngine : public SipEngine {
 public:
  StubSipEngine(const SipConfig& config, SipEngineCallback* callback,
                SipMediaSource* media);
  ~StubSipEngine() override;

  std::string engine_name() const override { return "stub"; }
  bool start(std::string* error) override;
  void stop() override;
  bool running() const override;
  bool reload_accounts(const std::vector<SipAccountConfig>& accounts,
                       std::string* error) override;
  std::vector<AccountStatus> account_status() const override;
  bool add_line(const LineConfig& line, std::string* error) override;
  bool remove_line(int line_id) override;
  bool dial(int line_id, const std::string& target, std::string* error) override;
  bool answer(int line_id, std::string* error) override;
  bool hangup(int line_id, std::string* error) override;
  bool set_hold(int line_id, bool hold, std::string* error) override;
  bool send_dtmf(int line_id, const std::string& digits,
                 std::string* error) override;
  CallStatus call_status(int line_id) const override;

  /** Test hook: pretends the PBX is calling `line_id`. */
  bool simulate_incoming_call(int line_id, const std::string& remote_uri,
                              std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aes67sip
