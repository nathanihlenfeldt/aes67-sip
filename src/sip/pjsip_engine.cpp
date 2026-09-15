#include "sip/pjsip_engine.hpp"

#include <algorithm>
#include <chrono>
#include <future>

#include "log.hpp"
#include "util.hpp"
#include "version.hpp"

namespace aes67sip {

namespace {

/** Codecs the gateway knows about; anything not configured gets disabled. */
const char* kKnownCodecs[] = {"PCMU/8000/1", "PCMA/8000/1", "G722/16000/1",
                              "opus/48000/2", "iLBC/8000/1", "GSM/8000/1",
                              "speex/8000/1", "speex/16000/1"};

pjsip_transport_type_e transport_type(const std::string& value) {
  const std::string type = to_lower(value);
  if (type == "tcp") {
    return PJSIP_TRANSPORT_TCP;
  }
  if (type == "tls") {
    return PJSIP_TRANSPORT_TLS;
  }
  return PJSIP_TRANSPORT_UDP;
}

pjmedia_srtp_use srtp_use(const std::string& value) {
  const std::string type = to_lower(value);
  if (type == "mandatory" || type == "required") {
    return PJMEDIA_SRTP_MANDATORY;
  }
  if (type == "optional") {
    return PJMEDIA_SRTP_OPTIONAL;
  }
  return PJMEDIA_SRTP_DISABLED;
}

/** "sip:pbx.example.com" or "pbx.example.com:5060" -> "pbx.example.com" */
std::string registrar_host(const std::string& registrar) {
  std::string value = registrar;
  if (value.rfind("sips:", 0) == 0) {
    value = value.substr(5);
  } else if (value.rfind("sip:", 0) == 0) {
    value = value.substr(4);
  }
  const size_t at = value.find('@');
  if (at != std::string::npos) {
    value = value.substr(at + 1);
  }
  const size_t port = value.find(':');
  if (port != std::string::npos) {
    value = value.substr(0, port);
  }
  return value;
}

/** Maps a PJSIP call state onto the gateway's line state. */
LineState line_state_for(pjsip_inv_state state, int status_code,
                         bool ever_connected) {
  switch (state) {
    case PJSIP_INV_STATE_CALLING:
      return LineState::kDialing;
    case PJSIP_INV_STATE_INCOMING:
    case PJSIP_INV_STATE_EARLY:
      return LineState::kRinging;
    case PJSIP_INV_STATE_CONNECTING:
    case PJSIP_INV_STATE_CONFIRMED:
      return LineState::kInCall;
    case PJSIP_INV_STATE_DISCONNECTED:
      if (!ever_connected && status_code >= 300) {
        return LineState::kError;
      }
      return LineState::kIdle;
    case PJSIP_INV_STATE_NULL:
      break;
  }
  return LineState::kIdle;
}

}  // namespace

PjsipSipEngine::PjsipSipEngine(const SipConfig& config,
                               SipEngineCallback* callback, SipMediaSource* media)
    : config_(config), callback_(callback), media_(media) {
  if (config_.ptime_ms > 0) {
    ptime_ms_ = static_cast<unsigned>(config_.ptime_ms);
  }
}

PjsipSipEngine::~PjsipSipEngine() {
  stop();
}

void PjsipSipEngine::post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(job));
  }
  queue_cv_.notify_one();
}

template <typename T>
T PjsipSipEngine::run_on_pjsip(std::function<T()> job, T fallback) {
  if (!started_.load() || runner_id_ == std::this_thread::get_id()) {
    return job();
  }
  auto promise = std::make_shared<std::promise<T>>();
  auto future = promise->get_future();
  post([job, promise] {
    try {
      promise->set_value(job());
    } catch (const pj::Error& error) {
      LOG_ERROR("pjsip job failed: ", error.info());
      try {
        promise->set_value(T{});
      } catch (...) {
      }
    } catch (...) {
      try {
        promise->set_value(T{});
      } catch (...) {
      }
    }
  });
  if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    LOG_ERROR("the pjsip thread did not answer within 10 s");
    return fallback;
  }
  return future.get();
}

PjsipSipEngine::LineContext* PjsipSipEngine::find(int line_id) {
  const auto it = lines_.find(line_id);
  return it == lines_.end() ? nullptr : it->second.get();
}

const SipAccountConfig* PjsipSipEngine::account_config(const std::string& id) const {
  for (const auto& account : accounts_) {
    if (account.id == id) {
      return &account;
    }
  }
  return nullptr;
}

std::string PjsipSipEngine::local_aor(const LineConfig& line) const {
  const SipAccountConfig* account = account_config(line.sip.account);
  const std::string user =
      line.sip.extension.empty()
          ? (account != nullptr ? account->username
                                : "line" + std::to_string(line.id))
          : line.sip.extension;
  const std::string host =
      account != nullptr ? registrar_host(account->registrar) : "localhost";
  return "sip:" + user + "@" + host;
}

void PjsipSipEngine::set_codec_priorities() {
  pj_uint8_t priority = 255;
  for (const auto& codec : config_.codecs) {
    try {
      endpoint_.codecSetPriority(codec, priority);
      priority = static_cast<pj_uint8_t>(priority > 8 ? priority - 8 : 0);
    } catch (const pj::Error&) {
      LOG_WARN("unknown codec '", codec, "' in sip.codecs");
    }
  }
  for (const char* codec : kKnownCodecs) {
    const bool configured = std::find(config_.codecs.begin(), config_.codecs.end(),
                                      codec) != config_.codecs.end();
    if (configured) {
      continue;
    }
    try {
      endpoint_.codecSetPriority(codec, 0);
    } catch (const pj::Error&) {
      // not built into this PJSIP: nothing to disable
    }
  }
}

// ---------------------------------------------------------------------------
// state shared with the pjsip callback threads
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

void PjsipSipEngine::store_port(int line_id,
                               std::unique_ptr<PjsipAudioPort> port) {
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

