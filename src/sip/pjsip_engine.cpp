// PJSIP (pjsua2) implementation of the SIP engine.

#include "sip/pjsip_engine.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <vector>

#include "log.hpp"
#include "sip/pjsip_audio_port.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

/** Codecs the gateway knows about; everything else is disabled. */
const char* kKnownCodecs[] = {"PCMU/8000/1",   "PCMA/8000/1",  "G722/16000/1",
                              "G729/8000/1",   "opus/48000/2", "iLBC/8000/1",
                              "speex/16000/1", "speex/8000/1", "GSM/8000/1"};

}  // namespace

/**
 * One call per line.
 *
 * pjsua2 delivers these callbacks on a library thread, so they only publish
 * state through `PjsipSipEngine`'s thread-safe accessors and never touch the
 * engine's pjsip-thread-only members.
 */
class SipCall : public pj::Call {
 public:
  SipCall(PjsipSipEngine* engine, int line_id, pj::Account& account,
          int call_id = PJSUA_INVALID_ID)
      : pj::Call(account, call_id), engine_(engine), line_id_(line_id) {}

  void onCallState(pj::OnCallStateParam&) override {
    try {
      const pj::CallInfo info = getInfo();
      engine_->notify_call_state(line_id_, info);
    } catch (const pj::Error& err) {
      LOG_WARN("line ", line_id_, ": cannot read call state: ", err.info());
    }
  }

  void onCallMediaState(pj::OnCallMediaStateParam&) override {
    try {
      engine_->notify_media_state(line_id_, getInfo());
    } catch (const pj::Error& err) {
      LOG_WARN("line ", line_id_, ": cannot read media state: ", err.info());
    }
  }

  void onDtmfDigit(pj::OnDtmfDigitParam& prm) override {
    LOG_DEBUG("line ", line_id_, ": DTMF '", prm.digit, "' received");
  }

  int line_id() const { return line_id_; }

 private:
  PjsipSipEngine* engine_{nullptr};
  int line_id_{0};
};

/** Registration and inbound-call callbacks for one line. */
class SipAccount : public pj::Account {
 public:
  SipAccount(PjsipSipEngine* engine, int line_id)
      : engine_(engine), line_id_(line_id) {}

  void onRegState(pj::OnRegStateParam& prm) override {
    engine_->notify_reg_state(line_id_, prm.code, prm.reason);
  }

  void onIncomingCall(pj::OnIncomingCallParam& prm) override {
    auto call = std::make_shared<SipCall>(engine_, line_id_, *this, prm.callId);
    std::string remote;
    try {
      remote = call->getInfo().remoteUri;
    } catch (const pj::Error&) {
    }
    engine_->store_call(line_id_, call);
    engine_->notify_incoming_call(line_id_, remote);
  }

  int line_id() const { return line_id_; }

 private:
  PjsipSipEngine* engine_{nullptr};
  int line_id_{0};
};

namespace {

/** Maps `sip.transport` onto the pjsip transport type. */
pjsip_transport_type_e transport_type(const std::string& name) {
  const std::string value = to_lower(name);
  if (value == "tcp") {
    return PJSIP_TRANSPORT_TCP;
  }
  if (value == "tls") {
    return PJSIP_TRANSPORT_TLS;
  }
  return PJSIP_TRANSPORT_UDP;
}

}  // namespace

PjsipSipEngine::PjsipSipEngine(const SipConfig& config, SipEngineCallback* callback,
                               SipMediaSource* media)
    : config_(config), callback_(callback), media_(media) {
  ptime_ms_ = config_.ptime_ms > 0 ? static_cast<unsigned>(config_.ptime_ms) : 20;
  clock_rate_ = 48000;  // AES67 native rate for the conference bridge
}

PjsipSipEngine::~PjsipSipEngine() {
  stop();
}

bool PjsipSipEngine::start(std::string* error) {
  if (running_.load()) {
    return true;
  }
  auto startup = std::make_shared<std::promise<std::string>>();
  std::future<std::string> result = startup->get_future();
  stop_requested_ = false;
  runner_ = std::thread([this, startup] { runner(startup.get()); });

  std::string failure;
  if (result.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
    failure = result.get();
  } else {
    failure = "PJSIP did not initialise within 10 s";
  }
  if (!failure.empty()) {
    stop();
    if (error != nullptr) {
      *error = failure;
    }
    return false;
  }
  running_ = true;
  LOG_INFO("PJSIP engine ready (", config_.transport, " port ", config_.local_port,
           ", bridge ", clock_rate_, " Hz, ", ptime_ms_, " ms ptime)");
  return true;
}

void PjsipSipEngine::stop() {
  if (!runner_.joinable()) {
    running_ = false;
    return;
  }
  post([this] {
    std::vector<std::shared_ptr<SipCall>> calls;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (auto& entry : calls_) {
        if (entry.second) {
          calls.push_back(entry.second);
        }
      }
    }
    for (auto& call : calls) {
      try {
        pj::CallOpParam prm;
        call->hangup(prm);
      } catch (const pj::Error&) {
      }
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop_requested_ = true;
  queue_cv_.notify_all();
  runner_.join();
  running_ = false;
  started_ = false;
  LOG_INFO("PJSIP engine stopped");
}

void PjsipSipEngine::post(std::function<void()> job) {
  if (!job) {
    return;
  }
  if (std::this_thread::get_id() == runner_id_) {
    job();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(job));
  }
  queue_cv_.notify_one();
}

template <typename T>
T PjsipSipEngine::run_on_pjsip(std::function<T()> job, T fallback) {
  if (!runner_.joinable()) {
    return fallback;
  }
  if (std::this_thread::get_id() == runner_id_) {
    return job();
  }
  auto promise = std::make_shared<std::promise<T>>();
  std::future<T> future = promise->get_future();
  post([promise, job, fallback] {
    try {
      promise->set_value(job());
    } catch (const std::exception& ex) {
      LOG_WARN("PJSIP call failed: ", ex.what());
      try {
        promise->set_value(fallback);
      } catch (const std::exception&) {
      }
    }
  });
  if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    LOG_WARN("PJSIP call timed out");
    return fallback;
  }
  try {
    return future.get();
  } catch (const std::exception&) {
    return fallback;
  }
}

void PjsipSipEngine::runner(std::promise<std::string>* startup) {
  runner_id_ = std::this_thread::get_id();
  std::string failure;
  try {
    endpoint_.libCreate();
    pj::EpConfig ep_config;
    ep_config.logConfig.level = 2;  // our own logger is the interface
    ep_config.logConfig.consoleLevel = 0;
    ep_config.uaConfig.maxCalls = 32;
    ep_config.uaConfig.threadCnt = 1;  // pjsua owns its message queue threads
    ep_config.medConfig.clockRate = clock_rate_;
    ep_config.medConfig.sndClockRate = clock_rate_;
    ep_config.medConfig.channelCount = 1;
    ep_config.medConfig.audioFramePtime = ptime_ms_;
    ep_config.medConfig.noVad = true;
    ep_config.medConfig.ecTailLen = 0;
    ep_config.medConfig.hasIoqueue = false;
    endpoint_.libInit(ep_config);

    // There is no sound card: the null device becomes the conference bridge
    // master port and all line audio arrives through the AES67 media ports.
    endpoint_.audDevManager().setNullDev();

    pj::TransportConfig transport;
    transport.port = static_cast<unsigned>(config_.local_port);
    endpoint_.transportCreate(transport_type(config_.transport), transport);
    set_codec_priorities();
    endpoint_.libStart();
    started_ = true;
  } catch (const pj::Error& err) {
    failure = std::string("PJSIP initialisation failed: ") + err.info();
  } catch (const std::exception& ex) {
    failure = std::string("PJSIP initialisation failed: ") + ex.what();
  }

  if (startup != nullptr) {
    startup->set_value(failure);
  }
  if (!failure.empty()) {
    LOG_ERROR(failure);
    try {
      endpoint_.libDestroy();
    } catch (const pj::Error&) {
    }
    return;
  }

  while (!stop_requested_.load()) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
        return !queue_.empty() || stop_requested_.load();
      });
      if (!queue_.empty()) {
        job = std::move(queue_.front());
        queue_.pop_front();
      }
    }
    if (job) {
      try {
        job();
      } catch (const pj::Error& err) {
        LOG_WARN("PJSIP job failed: ", err.info());
      } catch (const std::exception& ex) {
        LOG_WARN("PJSIP job failed: ", ex.what());
      }
    }
  }

  try {
    endpoint_.libDestroy();
  } catch (const pj::Error& err) {
    LOG_DEBUG("PJSIP destroy: ", err.info());
  }
  LOG_DEBUG("PJSIP library destroyed");
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

PjsipSipEngine::LineContext* PjsipSipEngine::find(int line_id) {
  const auto it = lines_.find(line_id);
  return it == lines_.end() ? nullptr : it->second.get();
}

const SipAccountConfig* PjsipSipEngine::account_config(
    const std::string& id) const {
  for (const auto& account : accounts_) {
    if (account.id == id) {
      return &account;
    }
  }
  return nullptr;
}

std::string PjsipSipEngine::local_aor(const LineConfig& line) const {
  std::string host = "localhost";
  const SipAccountConfig* account = account_config(line.sip.account);
  if (account != nullptr && !account->registrar.empty()) {
    // Accept "sip:host", "sip:host:port", "sip:user@host", "user@host:port",
    // "host" and "host:port": strip an optional scheme and an optional user part,
    // otherwise a registrar like "sip:2001@10.0.0.5" would yield an invalid AOR
    // ("sip:2001@2001@10.0.0.5") and registration could never succeed.
    std::string registrar = account->registrar;
    for (const std::string scheme : {"sips:", "sip:"}) {
      const size_t pos = registrar.find(scheme);
      if (pos != std::string::npos) {
        registrar = registrar.substr(pos + scheme.size());
        break;
      }
    }
    const size_t at = registrar.find('@');
    if (at != std::string::npos) {
      registrar = registrar.substr(at + 1);
    }
    if (!registrar.empty()) {
      host = registrar;
    }
  }
  const std::string user = line.sip.extension.empty()
                               ? (account != nullptr ? account->username : "")
                               : line.sip.extension;
  return "sip:" + user + "@" + host;
}

void PjsipSipEngine::set_codec_priorities() {
  // configured codecs first, in order, then silence everything else
  int priority = 255;
  for (const auto& codec : config_.codecs) {
    try {
      endpoint_.codecSetPriority(codec, static_cast<pj_uint8_t>(priority));
    } catch (const pj::Error&) {
      LOG_WARN("unknown codec '", codec, "' in sip.codecs");
    }
    priority = std::max(1, priority - 10);
  }
  for (const char* codec : kKnownCodecs) {
    const bool configured = std::find(config_.codecs.begin(), config_.codecs.end(),
                                      codec) != config_.codecs.end();
    if (!configured) {
      try {
        endpoint_.codecSetPriority(codec, 0);
      } catch (const pj::Error&) {
      }
    }
  }
  LOG_DEBUG("codec priorities set: ", config_.codecs.size(), " configured");
}

// ---------------------------------------------------------------------------
// bookkeeping shared with the callback threads
// ---------------------------------------------------------------------------

void PjsipSipEngine::store_call(int line_id, std::shared_ptr<SipCall> call) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  calls_[line_id] = std::move(call);
}

std::shared_ptr<SipCall> PjsipSipEngine::call_for(int line_id) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto it = calls_.find(line_id);
  return it == calls_.end() ? nullptr : it->second;
}

void PjsipSipEngine::clear_call(int line_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  calls_.erase(line_id);
}

void PjsipSipEngine::store_call_status(int line_id, const CallStatus& status) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  call_status_[line_id] = status;
}

void PjsipSipEngine::store_port(int line_id, std::unique_ptr<PjsipAudioPort> port) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ports_[line_id] = std::move(port);
}

std::unique_ptr<PjsipAudioPort> PjsipSipEngine::take_port(int line_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto it = ports_.find(line_id);
  if (it == ports_.end()) {
    return nullptr;
  }
  auto port = std::move(it->second);
  ports_.erase(it);
  return port;
}

void PjsipSipEngine::store_reg_state(int line_id, const AccountStatus& status) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  reg_status_[line_id] = status;
}

// ---------------------------------------------------------------------------
// media: connect the AES67 port of a line to its call
// ---------------------------------------------------------------------------

void PjsipSipEngine::setup_line_media(int line_id) {
  auto call = call_for(line_id);
  if (!call) {
    return;
  }
  teardown_line_media(line_id);

  pj::CallInfo info;
  try {
    info = call->getInfo();
  } catch (const pj::Error& err) {
    LOG_WARN("line ", line_id, ": cannot read call info: ", err.info());
    return;
  }

  pj::AudioMedia* audio = nullptr;
  unsigned rate = clock_rate_;
  for (unsigned i = 0; i < info.media.size(); ++i) {
    const auto& media = info.media[i];
    if (media.type != PJMEDIA_TYPE_AUDIO ||
        media.status != PJSUA_CALL_MEDIA_ACTIVE) {
      continue;
    }
    try {
      audio = static_cast<pj::AudioMedia*>(call->getMedia(i));
    } catch (const pj::Error& err) {
      LOG_WARN("line ", line_id, ": cannot get media ", i, ": ", err.info());
      continue;
    }
    if (audio == nullptr) {
      continue;
    }
    try {
      // the negotiated codec rate, e.g. 16000 for G.722
      rate = audio->getPortInfo().format.clockRate;
    } catch (const pj::Error&) {
      rate = clock_rate_;
    }
    break;
  }
  if (audio == nullptr) {
    LOG_DEBUG("line ", line_id, ": no active audio media yet");
    return;
  }

  auto port = std::make_unique<PjsipAudioPort>(line_id, media_);
  port->open("aes67-line" + std::to_string(line_id), rate, ptime_ms_);
  try {
    audio->startTransmit(*port);  // remote -> AES67 playback
    port->startTransmit(*audio);  // AES67 capture -> remote
  } catch (const pj::Error& err) {
    LOG_ERROR("line ", line_id, ": cannot connect the media port: ", err.info());
    return;
  }
  store_port(line_id, std::move(port));
  if (callback_ != nullptr) {
    callback_->on_media_start(line_id, rate);
  }
  LOG_INFO("line ", line_id, ": media connected at ", rate, " Hz");
}

void PjsipSipEngine::teardown_line_media(int line_id) {
  auto port = take_port(line_id);
  if (!port) {
    return;
  }
  port.reset();  // unregisters the port from the conference bridge
  if (callback_ != nullptr) {
    callback_->on_media_stop(line_id);
  }
  LOG_DEBUG("line ", line_id, ": media port removed");
}

// ---------------------------------------------------------------------------
// callbacks from the pjsip threads
// ---------------------------------------------------------------------------

void PjsipSipEngine::notify_media_state(int line_id, const pj::CallInfo& info) {
  bool active = false;
  for (const auto& media : info.media) {
    if (media.type == PJMEDIA_TYPE_AUDIO &&
        media.status == PJSUA_CALL_MEDIA_ACTIVE) {
      active = true;
      break;
    }
  }
  // Port creation and connection are library calls, so they run on the pjsip
  // thread rather than on the thread delivering this callback.
  post([this, line_id, active] {
    if (active) {
      setup_line_media(line_id);
    } else {
      teardown_line_media(line_id);
    }
  });
}

void PjsipSipEngine::notify_reg_state(int line_id, int code,
                                      const std::string& reason) {
  bool registered = false;
  std::string uri;
  SipAccount* account = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto it = account_index_.find(line_id);
    if (it != account_index_.end()) {
      account = it->second;
    }
  }
  if (account != nullptr) {
    try {
      const pj::AccountInfo info = account->getInfo();
      registered = info.regIsActive;
      uri = info.uri;
    } catch (const pj::Error&) {
    }
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  AccountStatus& status = reg_status_[line_id];
  if (!uri.empty()) {
    status.uri = uri;
  }
  status.code = code;
  if (registered) {
    status.state = "registered";
    status.error.clear();
  } else if (code == 0) {
    status.state = "unregistered";
    // Keep the last real reason: a later event with an empty reason (the
    // periodic "unregistered" notification) must not wipe 401/403/408 text.
    if (!reason.empty()) {
      status.error = reason;
    }
  } else {
    status.state = "error";
    status.error = reason.empty()
                       ? ("registration failed (" + std::to_string(code) + ")")
                       : (std::to_string(code) + " " + reason);
  }
  LOG_INFO("line ", line_id, ": registration ", status.state,
           status.error.empty() ? "" : (" (" + status.error + ")"));
}

void PjsipSipEngine::notify_incoming_call(int line_id,
                                          const std::string& remote_uri) {
  CallStatus status;
  status.active = true;
  status.remote_uri = remote_uri;
  status.state = "incoming";
  status.state_code = 180;
  store_call_status(line_id, status);
  if (callback_ != nullptr) {
    callback_->on_incoming_call(line_id, remote_uri);
  }
}

void PjsipSipEngine::notify_call_state(int line_id, const pj::CallInfo& info) {
  LineState state = LineState::kIdle;
  switch (info.state) {
    case PJSIP_INV_STATE_CALLING:
      state = LineState::kDialing;
      break;
    case PJSIP_INV_STATE_INCOMING:
    case PJSIP_INV_STATE_EARLY:
      state = LineState::kRinging;
      break;
    case PJSIP_INV_STATE_CONNECTING:
    case PJSIP_INV_STATE_CONFIRMED:
      state = LineState::kInCall;
      break;
    case PJSIP_INV_STATE_DISCONNECTED:
    case PJSIP_INV_STATE_NULL:
      state = LineState::kIdle;
      break;
  }

  CallStatus status;
  status.active = info.state != PJSIP_INV_STATE_DISCONNECTED &&
                  info.state != PJSIP_INV_STATE_NULL;
  status.remote_uri = info.remoteUri;
  status.state = info.stateText;
  status.state_code = info.lastStatusCode;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (info.state == PJSIP_INV_STATE_CONFIRMED &&
        call_connected_at_ms_.find(line_id) == call_connected_at_ms_.end()) {
      call_connected_at_ms_[line_id] = now_ms();
    }
    const auto it = call_connected_at_ms_.find(line_id);
    if (it != call_connected_at_ms_.end()) {
      const int64_t elapsed = (now_ms() - it->second) / 1000;
      status.duration_sec = elapsed > 0 ? static_cast<int>(elapsed) : 0;
    }
    if (status.state_code >= 300) {
      status.last_error =
          info.stateText.empty()
              ? ("call failed (" + std::to_string(status.state_code) + ")")
              : (info.stateText + " (" + std::to_string(status.state_code) + ")");
    }
    if (!status.active) {
      call_connected_at_ms_.erase(line_id);
    }
  }
  store_call_status(line_id, status);

  if (callback_ != nullptr) {
    callback_->on_line_state(line_id, state, info.lastStatusCode, info.stateText);
  }

  if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
    LOG_INFO("line ", line_id, ": call ended (", info.lastStatusCode, " ",
             info.stateText, ")");
    // the call object may not be destroyed from inside its own callback
    post([this, line_id] {
      teardown_line_media(line_id);
      clear_call(line_id);
    });
  }
}

// ---------------------------------------------------------------------------
// accounts and lines
// ---------------------------------------------------------------------------

bool PjsipSipEngine::reload_accounts(const std::vector<SipAccountConfig>& accounts,
                                     std::string* error) {
  (void)error;
  if (!running_.load()) {
    std::lock_guard<std::mutex> lock(mutex_);
    accounts_ = accounts;
    return true;
  }
  return run_on_pjsip<bool>(
      [this, accounts] {
        std::lock_guard<std::mutex> lock(mutex_);
        accounts_ = accounts;
        LOG_INFO("SIP accounts updated (", accounts_.size(), ")");
        return true;
      },
      false);
}

bool PjsipSipEngine::add_line(const LineConfig& line, std::string* error) {
  if (!running_.load()) {
    if (error != nullptr) {
      *error = "the SIP engine is not running";
    }
    return false;
  }
  return run_on_pjsip<bool>(
      [this, line, error]() -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        const SipAccountConfig* account = account_config(line.sip.account);
        if (account == nullptr) {
          if (error != nullptr) {
            *error = "unknown SIP account '" + line.sip.account + "'";
          }
          return false;
        }

        // replace an existing registration for this line
        const auto existing = lines_.find(line.id);
        if (existing != lines_.end()) {
          if (existing->second->account) {
            try {
              existing->second->account->shutdown();
            } catch (const pj::Error&) {
            }
          }
          lines_.erase(existing);
          std::lock_guard<std::mutex> state_lock(state_mutex_);
          account_index_.erase(line.id);
        }

        auto context = std::make_unique<LineContext>();
        context->config = line;
        context->account = std::make_unique<SipAccount>(this, line.id);

        const std::string aor = local_aor(line);
        pj::AccountConfig account_config;
        account_config.idUri = aor;
        if (!account->registrar.empty()) {
          account_config.regConfig.registrarUri = account->registrar;
        }
        account_config.regConfig.timeoutSec =
            static_cast<unsigned>(std::max(30, config_.registration_timeout));
        account_config.regConfig.retryIntervalSec =
            static_cast<unsigned>(std::max(5, config_.retry_interval));
        account_config.regConfig.firstRetryIntervalSec =
            static_cast<unsigned>(std::max(5, config_.retry_interval));
        account_config.regConfig.randomRetryIntervalSec =
            static_cast<unsigned>(std::max(5, config_.retry_interval));
        if (!account->outbound_proxy.empty()) {
          account_config.sipConfig.proxies.push_back(account->outbound_proxy);
        }
        if (!account->username.empty()) {
          // pjsip only uses a credential whose realm matches the challenge, and
          // "*" is the documented wildcard ("use '*' to make a credential that
          // can be used to authenticate against any challenges").  Passing an
          // empty realm silently matches nothing, which leaves REGISTER and
          // INVITE stuck at 401 against PBXs that use a realm (Asterisk uses
          // "asterisk").
          const std::string realm =
              account->auth_realm.empty() ? "*" : account->auth_realm;
          account_config.sipConfig.authCreds.push_back(pj::AuthCredInfo(
              "digest", realm, account->username, 0, account->password));
        }
        if (config_.keep_alive_interval > 0) {
          account_config.natConfig.udpKaIntervalSec =
              static_cast<unsigned>(config_.keep_alive_interval);
        }
        const std::string srtp = to_lower(config_.srtp);
        if (srtp == "mandatory" || srtp == "required") {
          account_config.mediaConfig.srtpUse = PJMEDIA_SRTP_MANDATORY;
        } else if (srtp == "optional") {
          account_config.mediaConfig.srtpUse = PJMEDIA_SRTP_OPTIONAL;
        } else {
          account_config.mediaConfig.srtpUse = PJMEDIA_SRTP_DISABLED;
        }

        try {
          context->account->create(account_config, false);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = "cannot create the SIP account for line " +
                     std::to_string(line.id) + ": " + err.info();
          }
          LOG_ERROR("line ", line.id,
                    ": cannot create the SIP account: ", err.info());
          return false;
        }

        {
          std::lock_guard<std::mutex> state_lock(state_mutex_);
          account_index_[line.id] = context->account.get();
          const auto existing = reg_status_.find(line.id);
          if (existing == reg_status_.end()) {
            AccountStatus initial;
            initial.id = account->id;
            initial.uri = aor;
            initial.state = "unregistered";
            reg_status_[line.id] = initial;
          } else {
            // A registration callback can arrive before this point (it does for
            // a DNS failure: account->create() above reports it in the same
            // millisecond), so never clear an existing state/reason - doing so
            // left the UI showing a bare "unregistered" with no explanation.
            if (existing->second.id.empty()) {
              existing->second.id = account->id;
            }
            existing->second.uri = aor;
          }
        }

        lines_[line.id] = std::move(context);
        LOG_INFO("line ", line.id, ": registered as ", aor, " through account '",
                 account->id, "'");
        return true;
      },
      false);
}

bool PjsipSipEngine::remove_line(int line_id) {
  if (!running_.load()) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.erase(line_id);
    return true;
  }
  return run_on_pjsip<bool>(
      [this, line_id] {
        std::lock_guard<std::mutex> lock(mutex_);
        teardown_line_media(line_id);
        const auto it = lines_.find(line_id);
        if (it != lines_.end()) {
          if (it->second->account) {
            try {
              it->second->account->shutdown();
            } catch (const pj::Error&) {
            }
          }
          lines_.erase(it);
        }
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        account_index_.erase(line_id);
        reg_status_.erase(line_id);
        call_status_.erase(line_id);
        call_connected_at_ms_.erase(line_id);
        return true;
      },
      false);
}

// ---------------------------------------------------------------------------
// call control
// ---------------------------------------------------------------------------

bool PjsipSipEngine::dial(int line_id, const std::string& target,
                          std::string* error) {
  if (!running_.load()) {
    if (error != nullptr) {
      *error = "the SIP engine is not running";
    }
    return false;
  }
  return run_on_pjsip<bool>(
      [this, line_id, target, error]() -> bool {
        LineContext* context = find(line_id);
        if (context == nullptr) {
          if (error != nullptr) {
            *error = "unknown line " + std::to_string(line_id);
          }
          return false;
        }
        if (call_for(line_id) != nullptr) {
          if (error != nullptr) {
            *error = "line " + std::to_string(line_id) + " already has a call";
          }
          return false;
        }
        const std::string uri =
            target.empty() ? context->config.sip.dial_target : target;
        if (uri.empty()) {
          if (error != nullptr) {
            *error =
                "no dial target configured for line " + std::to_string(line_id);
          }
          return false;
        }

        auto call = std::make_shared<SipCall>(this, line_id, *context->account);
        try {
          pj::CallOpParam prm(true);  // use the default call setting
          prm.opt.audioCount = 1;
          prm.opt.videoCount = 0;
          call->makeCall(uri, prm);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = "cannot call " + uri + ": " + err.info();
          }
          return false;
        }
        store_call(line_id, call);
        LOG_INFO("line ", line_id, ": calling ", uri);
        return true;
      },
      false);
}

bool PjsipSipEngine::answer(int line_id, std::string* error) {
  return run_on_pjsip<bool>(
      [this, line_id, error]() -> bool {
        auto call = call_for(line_id);
        if (call == nullptr) {
          if (error != nullptr) {
            *error = "line " + std::to_string(line_id) + " has no call to answer";
          }
          return false;
        }
        int code = 200;
        if (LineContext* context = find(line_id)) {
          const int configured = context->config.sip.auto_answer_code;
          if (configured >= 200 && configured < 300) {
            code = configured;
          }
        }
        try {
          pj::CallOpParam prm;
          prm.statusCode = static_cast<pjsip_status_code>(code);
          call->answer(prm);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = std::string("cannot answer: ") + err.info();
          }
          return false;
        }
        LOG_INFO("line ", line_id, ": call answered with ", code);
        return true;
      },
      false);
}

bool PjsipSipEngine::hangup(int line_id, std::string* error) {
  return run_on_pjsip<bool>(
      [this, line_id, error]() -> bool {
        auto call = call_for(line_id);
        if (call == nullptr) {
          if (error != nullptr) {
            *error = "line " + std::to_string(line_id) + " has no call";
          }
          return false;
        }
        try {
          pj::CallOpParam prm;
          call->hangup(prm);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = std::string("cannot hang up: ") + err.info();
          }
          return false;
        }
        LOG_INFO("line ", line_id, ": call hangup requested");
        return true;
      },
      false);
}

bool PjsipSipEngine::set_hold(int line_id, bool hold, std::string* error) {
  return run_on_pjsip<bool>(
      [this, line_id, hold, error]() -> bool {
        auto call = call_for(line_id);
        if (call == nullptr) {
          if (error != nullptr) {
            *error = "line " + std::to_string(line_id) + " has no call";
          }
          return false;
        }
        try {
          pj::CallOpParam prm(true);
          prm.opt.audioCount = 1;
          prm.opt.videoCount = 0;
          if (!hold) {
            prm.opt.flag |= PJSUA_CALL_UNHOLD;
          }
          call->setHold(prm);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = std::string("cannot change hold: ") + err.info();
          }
          return false;
        }
        LOG_INFO("line ", line_id, ": call ", hold ? "held" : "resumed");
        return true;
      },
      false);
}

bool PjsipSipEngine::send_dtmf(int line_id, const std::string& digits,
                               std::string* error) {
  if (digits.empty()) {
    return true;
  }
  return run_on_pjsip<bool>(
      [this, line_id, digits, error]() -> bool {
        auto call = call_for(line_id);
        if (call == nullptr) {
          if (error != nullptr) {
            *error = "line " + std::to_string(line_id) + " has no call";
          }
          return false;
        }
        try {
          call->dialDtmf(digits);
        } catch (const pj::Error& err) {
          if (error != nullptr) {
            *error = std::string("cannot send DTMF: ") + err.info();
          }
          return false;
        }
        LOG_INFO("line ", line_id, ": DTMF '", digits, "' sent");
        return true;
      },
      false);
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

CallStatus PjsipSipEngine::call_status(int line_id) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto it = call_status_.find(line_id);
  if (it == call_status_.end()) {
    CallStatus idle;
    idle.state = "idle";
    return idle;
  }
  return it->second;
}

std::vector<AccountStatus> PjsipSipEngine::account_status() const {
  const auto rank = [](const std::string& state) {
    if (state == "error") {
      return 3;
    }
    if (state == "unregistered") {
      return 2;
    }
    if (state == "registering") {
      return 1;
    }
    return 0;
  };

  std::lock_guard<std::mutex> lock(state_mutex_);
  std::map<std::string, AccountStatus> aggregated;
  for (const auto& entry : reg_status_) {
    AccountStatus status = entry.second;
    if (status.id.empty()) {
      status.id = "line " + std::to_string(entry.first);
    }
    const auto it = aggregated.find(status.id);
    if (it == aggregated.end()) {
      aggregated[status.id] = status;
      continue;
    }
    // worst state wins, so the UI never reports a healthy account while one of
    // the lines sharing it is down
    if (rank(status.state) > rank(it->second.state)) {
      it->second.state = status.state;
      it->second.error = status.error;
      it->second.code = status.code;
    }
    it->second.uri = status.uri;
  }

  std::vector<AccountStatus> result;
  result.reserve(aggregated.size());
  for (auto& entry : aggregated) {
    result.push_back(entry.second);
  }
  return result;
}

}  // namespace aes67sip
