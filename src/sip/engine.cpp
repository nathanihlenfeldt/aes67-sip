// SIP engine factory and the stub (development) engine.

#include "sip/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "log.hpp"
#include "util.hpp"

#ifdef WITH_PJSIP
#include "sip/pjsip_engine.hpp"
#endif

namespace aes67sip {

const char* to_string(LineState state) {
  switch (state) {
    case LineState::kDisabled:
      return "disabled";
    case LineState::kIdle:
      return "idle";
    case LineState::kDialing:
      return "dialing";
    case LineState::kRinging:
      return "ringing";
    case LineState::kInCall:
      return "in_call";
    case LineState::kError:
      return "error";
  }
  return "error";
}

std::unique_ptr<SipEngine> SipEngine::create(const SipConfig& config,
                                             SipEngineCallback* callback,
                                             SipMediaSource* media,
                                             std::string* error) {
  if (!config.enabled) {
    if (error) {
      *error = "SIP is disabled in the configuration";
    }
    return std::make_unique<StubSipEngine>(config, callback, media);
  }
  if (to_lower(config.engine) == "fake" || to_lower(config.engine) == "stub") {
    return std::make_unique<StubSipEngine>(config, callback, media);
  }
#ifdef WITH_PJSIP
  (void)error;
  return std::make_unique<PjsipSipEngine>(config, callback, media);
#else
  if (error) {
    *error =
        "this build has no PJSIP support, using the stub SIP engine "
        "(run scripts/build-pjsip.sh and rebuild with -DWITH_PJSIP=ON)";
  }
  return std::make_unique<StubSipEngine>(config, callback, media);
#endif
}

// ---------------------------------------------------------------------------
// StubSipEngine
// ---------------------------------------------------------------------------

struct StubSipEngine::Impl {
  struct LineRuntime {
    LineConfig config;
    LineState state{LineState::kIdle};
    CallStatus call;
    bool hold{false};
    int64_t connected_at_ms{0};
    int64_t transition_at_ms{0};  // simulated progress of the current call
    bool inbound{false};
  };

  Impl(const SipConfig& cfg, SipEngineCallback* cb, SipMediaSource* src)
      : config(cfg), callback(cb), media(src) {}

  SipConfig config;
  SipEngineCallback* callback{nullptr};
  SipMediaSource* media{nullptr};

  mutable std::mutex mutex;
  std::vector<SipAccountConfig> accounts;
  std::map<int, LineRuntime> lines;
  std::thread worker;
  std::thread media_worker;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  unsigned call_rate{8000};
  unsigned ptime_ms{20};

  unsigned frames_per_pull() const {
    return std::max(1U, call_rate * ptime_ms / 1000);
  }

  void notify_state(LineRuntime& line, LineState state, int code,
                    const std::string& detail) {
    line.state = state;
    line.call.state_code = code;
    line.call.last_error = detail;
    if (callback != nullptr) {
      callback->on_line_state(line.config.id, state, code, detail);
    }
  }

  void media_loop() {
    // Everything received from the "PBX" is sent straight back, so the AES67
    // routing chain can be exercised end to end without a PBX.
    std::vector<float> buffer(frames_per_pull() * 4 + 64, 0.0F);
    while (!stop_requested.load()) {
      bool busy = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : lines) {
          LineRuntime& line = entry.second;
          if (line.state != LineState::kInCall || line.hold) {
            continue;
          }
          busy = true;
          const size_t frames = frames_per_pull();
          if (buffer.size() < frames) {
            buffer.resize(frames, 0.0F);
          }
          if (media != nullptr) {
            media->pull_to_sip(line.config.id, buffer.data(), frames);
            media->push_from_sip(line.config.id, buffer.data(), frames);
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(
          busy ? std::max(1U, ptime_ms) : 20U));
    }
  }

  void tick() {
    const int64_t now = now_ms();
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& entry : lines) {
      LineRuntime& line = entry.second;
      if (line.state == LineState::kDialing && now >= line.transition_at_ms) {
        line.call.state = "ringing";
        notify_state(line, LineState::kRinging, 180, "ringing");
        line.transition_at_ms = now + 1000;
      } else if (line.state == LineState::kRinging && !line.inbound &&
                 now >= line.transition_at_ms) {
        line.call.active = true;
        line.call.state = "in_call";
        line.connected_at_ms = now;
        if (callback != nullptr) {
          callback->on_media_start(line.config.id, call_rate);
        }
        notify_state(line, LineState::kInCall, 200, "answered");
      } else if (line.state == LineState::kInCall) {
        line.call.duration_sec =
            static_cast<int>((now - line.connected_at_ms) / 1000);
      }
    }
  }

  void work() {
    set_thread_name("sip-stub");
    while (!stop_requested.load()) {
      tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
};

StubSipEngine::StubSipEngine(const SipConfig& config, SipEngineCallback* callback,
                             SipMediaSource* media)
    : impl_(std::make_unique<Impl>(config, callback, media)) {}

StubSipEngine::~StubSipEngine() {
  stop();
}

bool StubSipEngine::start(std::string* error) {
  (void)error;
  if (impl_->running.load()) {
    return true;
  }
  impl_->stop_requested = false;
  impl_->running = true;
  impl_->worker = std::thread([this] { impl_->work(); });
  impl_->media_worker = std::thread([this] { impl_->media_loop(); });
  LOG_WARN("SIP engine is the stub: calls are simulated and no SIP signalling "
           "is performed (use the pjsip engine in production)");
  return true;
}

void StubSipEngine::stop() {
  if (!impl_->running.load()) {
    return;
  }
  impl_->stop_requested = true;
  if (impl_->media_worker.joinable()) {
    impl_->media_worker.join();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->running = false;
}

bool StubSipEngine::running() const {
  return impl_->running.load();
}

bool StubSipEngine::reload_accounts(const std::vector<SipAccountConfig>& accounts,
                                    std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->accounts = accounts;
  return true;
}

std::vector<AccountStatus> StubSipEngine::account_status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<AccountStatus> result;
  for (const auto& account : impl_->accounts) {
    AccountStatus status;
    status.id = account.id;
    status.uri = account.registrar;
    status.state = impl_->config.enabled ? "registered" : "unregistered";
    status.error = impl_->config.enabled ? "" : "SIP disabled in configuration";
    result.push_back(status);
  }
  return result;
}

bool StubSipEngine::add_line(const LineConfig& line, std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line.id);
  if (it == impl_->lines.end()) {
    auto& fresh = impl_->lines[line.id];
    fresh.config = line;
    fresh.state = line.enabled ? LineState::kIdle : LineState::kDisabled;
    fresh.call = CallStatus{};
    fresh.call.remote_uri = line.sip.dial_target;
    return true;
  }
  // configuration updates must not drop a call that is already up
  Impl::LineRuntime& runtime = it->second;
  runtime.config = line;
  if (!line.enabled) {
    runtime.state = LineState::kDisabled;
    runtime.call = CallStatus{};
    runtime.hold = false;
    runtime.inbound = false;
  } else if (runtime.state == LineState::kDisabled) {
    runtime.state = LineState::kIdle;
  }
  return true;
}

bool StubSipEngine::remove_line(int line_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->lines.erase(line_id);
  return true;
}

bool StubSipEngine::dial(int line_id, const std::string& target,
                         std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  if (it == impl_->lines.end()) {
    if (error) {
      *error = "unknown line " + std::to_string(line_id);
    }
    return false;
  }
  Impl::LineRuntime& line = it->second;
  if (line.state == LineState::kInCall || line.state == LineState::kRinging) {
    if (error) {
      *error = "line already has a call";
    }
    return false;
  }
  const std::string destination =
      target.empty() ? line.config.sip.dial_target : target;
  if (destination.empty()) {
    if (error) {
      *error = "no dial target configured for line " + std::to_string(line_id);
    }
    return false;
  }
  line.call = CallStatus{};
  line.call.active = true;
  line.call.remote_uri = destination;
  line.call.state = "calling";
  line.inbound = false;
  line.transition_at_ms = now_ms() + 500;
  impl_->notify_state(line, LineState::kDialing, 100, "calling " + destination);
  return true;
}

bool StubSipEngine::answer(int line_id, std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  if (it == impl_->lines.end() || !it->second.call.active) {
    if (error) {
      *error = "no incoming call on line " + std::to_string(line_id);
    }
    return false;
  }
  Impl::LineRuntime& line = it->second;
  line.call.state = "in_call";
  line.connected_at_ms = now_ms();
  if (impl_->callback != nullptr) {
    impl_->callback->on_media_start(line.config.id, impl_->call_rate);
  }
  impl_->notify_state(line, LineState::kInCall, 200, "answered");
  return true;
}

bool StubSipEngine::hangup(int line_id, std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  if (it == impl_->lines.end()) {
    return false;
  }
  Impl::LineRuntime& line = it->second;
  const bool was_in_call = line.state == LineState::kInCall;
  line.call = CallStatus{};
  line.hold = false;
  line.inbound = false;
  if (impl_->callback != nullptr && was_in_call) {
    impl_->callback->on_media_stop(line.config.id);
  }
  impl_->notify_state(
      line, line.config.enabled ? LineState::kIdle : LineState::kDisabled, 0, "");
  return true;
}

bool StubSipEngine::set_hold(int line_id, bool hold, std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  if (it == impl_->lines.end()) {
    return false;
  }
  it->second.hold = hold;
  it->second.call.state = hold ? "held" : "in_call";
  return true;
}

bool StubSipEngine::send_dtmf(int line_id, const std::string& digits,
                              std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->lines.find(line_id) == impl_->lines.end()) {
    return false;
  }
  LOG_INFO("stub engine: DTMF '", digits, "' on line ", line_id);
  return true;
}

CallStatus StubSipEngine::call_status(int line_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  return it == impl_->lines.end() ? CallStatus{} : it->second.call;
}

bool StubSipEngine::simulate_incoming_call(int line_id,
                                           const std::string& remote_uri,
                                           std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->lines.find(line_id);
  if (it == impl_->lines.end()) {
    if (error) {
      *error = "unknown line " + std::to_string(line_id);
    }
    return false;
  }
  Impl::LineRuntime& line = it->second;
  line.call = CallStatus{};
  line.call.active = true;
  line.call.remote_uri = remote_uri;
  line.call.state = "ringing";
  line.inbound = true;
  if (impl_->callback != nullptr) {
    impl_->callback->on_incoming_call(line.config.id, remote_uri);
  }
  impl_->notify_state(line, LineState::kRinging, 180, "incoming " + remote_uri);
  return true;
}

}  // namespace aes67sip


