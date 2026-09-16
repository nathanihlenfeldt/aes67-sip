#pragma once

#include <pjsua2.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sip/engine.hpp"
#include "sip/pjsip_audio_port.hpp"

namespace aes67sip {

class SipAccount;
class SipCall;

/**
 * PJSIP (pjsua2) implementation of the SIP engine.
 *
 * Threading: pjsua2 requires that everything touching the library runs on the
 * thread that created the endpoint, so every public method marshals its work
 * onto a dedicated "pjsip" thread through a job queue.  The account and call
 * callbacks arrive on PJSIP's own threads and only publish state through
 * atomics / short critical sections.
 *
 * Audio: one AudioMediaPort per line is created at the conference bridge rate
 * (48 kHz mono), so the AES67 path needs no sample rate conversion; the bridge
 * resamples between that port and the negotiated codec (8 kHz G.711, 16 kHz
 * G.722, ...).
 */
class PjsipSipEngine : public SipEngine {
 public:
  friend class SipCall;
  friend class SipAccount;

  PjsipSipEngine(const SipConfig& config, SipEngineCallback* callback,
                 SipMediaSource* media);
  ~PjsipSipEngine() override;

  std::string engine_name() const override { return "pjsip"; }
  bool start(std::string* error) override;
  void stop() override;
  bool running() const override { return running_.load(); }

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

  // ---- called by SipAccount / SipCall (PJSIP threads) --------------------
  /** `code` is the SIP status of the registration (200 = ok). */
  void notify_reg_state(int line_id, int code, const std::string& reason);
  void notify_call_state(int line_id, const pj::CallInfo& info);
  void notify_media_state(int line_id, const pj::CallInfo& info);
  void notify_incoming_call(int line_id, const std::string& remote_uri);

  /** Conference bridge rate used for the line ports. */
  unsigned clock_rate() const { return clock_rate_; }
  unsigned ptime_ms() const { return ptime_ms_; }

 private:
  struct LineContext {
    LineConfig config;
    std::unique_ptr<SipAccount> account;
    bool ever_connected{false};
  };

  /**
   * Runs `job` on the pjsip thread, waits for the result.  pjsua2 requires all
   * library calls to come from the thread that created the endpoint.
   */
  template <typename T>
  T run_on_pjsip(std::function<T()> job, T fallback);
  void post(std::function<void()> job);
  void runner(std::promise<std::string>* startup);
  LineContext* find(int line_id);
  /** Creates/connects (or destroys) the AES67 media port of a line. */
  void setup_line_media(int line_id);
  void teardown_line_media(int line_id);
  /** Releases a line's account, and with `forget_status` everything reported from
   * it.  Runs on the pjsip thread with `mutex_` held (see the threading note
   * above). */
  void release_line_account(int line_id, bool forget_status);
  void set_codec_priorities();
  std::string local_aor(const LineConfig& line) const;
  const SipAccountConfig* account_config(const std::string& id) const;

  /** Call/port bookkeeping shared with the pjsip callback threads. */
  void store_call(int line_id, std::shared_ptr<SipCall> call);
  std::shared_ptr<SipCall> call_for(int line_id) const;
  void clear_call(int line_id);
  void store_call_status(int line_id, const CallStatus& status);
  void store_port(int line_id, std::unique_ptr<PjsipAudioPort> port);
  std::unique_ptr<PjsipAudioPort> take_port(int line_id);
  void store_reg_state(int line_id, const AccountStatus& status);

  SipConfig config_;
  SipEngineCallback* callback_{nullptr};
  SipMediaSource* media_{nullptr};

  /** Owned by the pjsip thread only. */
  mutable std::mutex mutex_;
  std::map<int, std::unique_ptr<LineContext>> lines_;
  std::vector<SipAccountConfig> accounts_;

  /** Shared with the pjsip callback threads. */
  mutable std::mutex state_mutex_;
  std::map<int, std::shared_ptr<SipCall>> calls_;
  std::map<int, CallStatus> call_status_;
  std::map<int, AccountStatus> reg_status_;
  std::map<int, std::unique_ptr<PjsipAudioPort>> ports_;
  /** Connect time per line, used to report the call duration. */
  std::map<int, int64_t> call_connected_at_ms_;
  /**
   * Raw pointers to the accounts, so a callback thread can query registration
   * state without touching `lines_` (which belongs to the pjsip thread).
   */
  std::map<int, SipAccount*> account_index_;

  pj::Endpoint endpoint_;
  std::thread runner_;
  std::thread::id runner_id_{};
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::function<void()>> queue_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  unsigned clock_rate_{48000};
  unsigned ptime_ms_{20};
};

class SipAccount;
class SipCall;

}  // namespace aes67sip
