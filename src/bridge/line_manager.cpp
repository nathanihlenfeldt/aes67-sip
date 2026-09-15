// Line manager: configuration, AES67 stream provisioning and call modes.

#include "bridge/line_manager.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

constexpr int kSupervisionIntervalMs = 100;
constexpr int64_t kSinkPollIntervalMs = 2000;

/**
 * Host part of a registrar string: strips an optional sip:/sips: scheme and an
 * optional user@ part, so "sip:2001@10.0.0.5:5060" becomes "10.0.0.5:5060".
 */
std::string registrar_host(const std::string& registrar) {
  std::string value = registrar;
  for (const std::string scheme : {"sips:", "sip:"}) {
    const size_t pos = value.find(scheme);
    if (pos != std::string::npos) {
      value = value.substr(pos + scheme.size());
      break;
    }
  }
  const size_t at = value.find('@');
  if (at != std::string::npos) {
    value = value.substr(at + 1);
  }
  return value;
}

/** DNS host (no port) of a registrar, for the reachability check. */
std::string registrar_dns_host(const std::string& registrar) {
  std::string value = registrar_host(registrar);
  const size_t colon = value.find(':');
  if (colon != std::string::npos) {
    value = value.substr(0, colon);
  }
  return value;
}

bool is_auto_answer_mode(const std::string& mode) {
  const std::string value = to_lower(mode);
  // `permanent` answers an inbound INVITE and then holds the call up for as long
  // as it lasts (4-wire intercom: once the call is up it stays up).
  return value == "auto_answer" || value == "auto" || value == "ptt" ||
         value == "permanent";
}

bool is_ptt_mode(const std::string& mode) {
  return to_lower(mode) == "ptt";
}

}  // namespace

LineManager::LineManager(Config* config, std::string config_path,
                         DaemonClient* daemon, AudioRouter* router,
                         SipEngine* engine)
    : config_(config),
      config_path_(std::move(config_path)),
      daemon_(daemon),
      router_(router),
      engine_(engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& line : config_->lines) {
    auto runtime = std::make_shared<LineRuntime>();
    runtime->config = line;
    runtime->state = line.enabled ? LineState::kIdle : LineState::kDisabled;
    lines_[line.id] = std::move(runtime);
  }
}

LineManager::~LineManager() {
  stop();
}

int LineManager::line_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(lines_.size());
}

std::string LineManager::resolve_endpoint_sdp(const LineConfig& line,
                                              std::string* error) {
  // 1. an SDP pasted into the configuration wins
  if (!trim(line.aes67.remote_sdp).empty()) {
    return line.aes67.remote_sdp;
  }

  // 2. a discovered SAP/mDNS source selected in the UI
  if (!trim(line.aes67.remote_source_id).empty()) {
    json discovered;
    std::string browse_error;
    if (daemon_->browse_sources("all", &discovered, &browse_error)) {
      const auto it = discovered.find("remote_sources");
      if (it != discovered.end() && it->is_array()) {
        for (const auto& source : *it) {
          if (json_get<std::string>(source, "id", "") ==
              line.aes67.remote_source_id) {
            const std::string sdp = json_get<std::string>(source, "sdp", "");
            if (!trim(sdp).empty()) {
              return sdp;
            }
          }
        }
      }
      if (error != nullptr) {
        *error = "discovered source '" + line.aes67.remote_source_id +
                 "' is not currently announced";
      }
    } else if (error != nullptr) {
      *error = browse_error;
    }
    return {};
  }

  // 3. nothing configured: loop our own source back so the AES67 path can be
  //    commissioned (test tone) before the endpoints are wired up
  std::string own_sdp;
  std::string sdp_error;
  if (daemon_->get_source_sdp(line.aes67.source_id, &own_sdp, &sdp_error)) {
    LOG_INFO("line ", line.id,
             ": no endpoint SDP configured, looping back AES67 "
             "source ",
             line.aes67.source_id, " for commissioning");
    return own_sdp;
  }
  if (error != nullptr) {
    *error =
        "no endpoint SDP for line " + std::to_string(line.id) + ": " + sdp_error;
  }
  return {};
}

void LineManager::apply_line_to_router(const LineConfig& line) {
  AudioRouter::LineParams params;
  params.channels = line.aes67.channels;
  params.gain_db = line.gain_db;
  params.rx_gain_db = line.rx_gain_db;
  params.tx_gain_db = line.tx_gain_db;
  params.mute = line.mute;
  params.enabled = line.enabled;
  params.ptt_threshold_dbfs = line.sip.ptt_threshold_dbfs;
  params.ptt_hold_ms = line.sip.ptt_hangup_ms;
  router_->set_line_params(line.id, params);
  router_->set_line_ptt_params(line.id, line.sip.ptt_threshold_dbfs,
                               line.sip.ptt_hangup_ms);
}

void LineManager::configure_daemon_streams(const LineConfig& line) {
  if (!config_->aes67_daemon.auto_configure || !line.enabled ||
      !line.aes67.auto_create_streams) {
    return;
  }
  std::string error;

  const json source = DaemonClient::make_source(config_->aes67_daemon, line);
  if (!source.is_null()) {
    if (daemon_->put_source(line.aes67.source_id, source, &error)) {
      LOG_INFO("line ", line.id, ": AES67 source ", line.aes67.source_id, " '",
               line.aes67.stream_name, "' configured");
    } else {
      LOG_WARN("line ", line.id, ": cannot configure AES67 source ",
               line.aes67.source_id, ": ", error);
    }
  }

  json sink;
  std::string sdp_error;
  const std::string remote_sdp = resolve_endpoint_sdp(line, &sdp_error);
  if (!DaemonClient::make_sink(config_->aes67_daemon, line, remote_sdp, &sink,
                               &error)) {
    LOG_WARN("line ", line.id, ": AES67 sink ", line.aes67.sink_id,
             " not configured: ", error);
    return;
  }
  if (daemon_->put_sink(line.aes67.sink_id, sink, &error)) {
    LOG_INFO("line ", line.id, ": AES67 sink ", line.aes67.sink_id, " '",
             line.aes67.stream_name, "' configured");
  } else {
    LOG_WARN("line ", line.id, ": cannot configure AES67 sink ", line.aes67.sink_id,
             ": ", error);
  }
}

void LineManager::remove_daemon_streams(const LineConfig& line) {
  std::string error;
  daemon_->delete_sink(line.aes67.sink_id, &error);
  daemon_->delete_source(line.aes67.source_id, &error);
}

bool LineManager::apply_configuration(std::string* error) {
  std::vector<LineConfig> lines;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // pick up lines added or removed while the gateway was running
    for (auto it = lines_.begin(); it != lines_.end();) {
      if (config_->line_index(it->first) < 0) {
        it = lines_.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& line : config_->lines) {
      auto it = lines_.find(line.id);
      if (it == lines_.end()) {
        auto runtime = std::make_shared<LineRuntime>();
        runtime->state = line.enabled ? LineState::kIdle : LineState::kDisabled;
        runtime->config = line;
        it = lines_.emplace(line.id, std::move(runtime)).first;
      }
      it->second->config = line;
      if (!line.enabled) {
        it->second->state = LineState::kDisabled;
      } else if (it->second->state == LineState::kDisabled) {
        it->second->state = LineState::kIdle;
      }
      lines.push_back(line);
    }
  }

  if (engine_ == nullptr) {
    if (error != nullptr) {
      *error = "the SIP engine is not available";
    }
    return false;
  }
  if (!engine_->reload_accounts(config_->accounts, error)) {
    return false;
  }

  for (const auto& line : lines) {
    AudioRouter::LineParams params;
    params.channels = line.aes67.channels;
    params.gain_db = line.gain_db;
    params.rx_gain_db = line.rx_gain_db;
    params.tx_gain_db = line.tx_gain_db;
    params.mute = line.mute;
    params.enabled = line.enabled;
    params.call_active = false;
    params.ptt_threshold_dbfs = line.sip.ptt_threshold_dbfs;
    params.ptt_hold_ms = line.sip.ptt_hangup_ms;
    router_->add_line(line.id, params, 8000);

    std::string line_error;
    if (!engine_->add_line(line, &line_error)) {
      LOG_WARN("line ", line.id, ": SIP registration failed: ", line_error);
    }
    configure_daemon_streams(line);
  }
  LOG_INFO("configuration applied: ", lines.size(), " line(s), SIP engine ",
           engine_->engine_name());
  return true;
}

// ---------------------------------------------------------------------------
// supervision: dial_out / ptt / sink polling
// ---------------------------------------------------------------------------

bool LineManager::start_supervision() {
  if (running_.load()) {
    return true;
  }
  stop_requested_ = false;
  running_ = true;
  supervisor_ = std::thread([this] { supervise(); });
  return true;
}

void LineManager::stop() {
  if (!running_.load()) {
    return;
  }
  stop_requested_ = true;
  if (supervisor_.joinable()) {
    supervisor_.join();
  }
  running_ = false;
}

void LineManager::supervise() {
  set_thread_name("line-supervisor");
  int64_t last_sink_poll_ms = 0;

  while (!stop_requested_.load()) {
    const int64_t now = now_ms();
    const bool poll_sinks = now - last_sink_poll_ms >= kSinkPollIntervalMs;
    if (poll_sinks) {
      last_sink_poll_ms = now;
    }

    // Snapshot the lines first: `mutex_` must never be held while calling into
    // the SIP engine or the daemon client, both of which call back into this
    // object from their own threads (which would deadlock on a non recursive
    // mutex).
    std::vector<std::shared_ptr<LineRuntime>> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot.reserve(lines_.size());
      for (const auto& entry : lines_) {
        snapshot.push_back(entry.second);
      }
    }

    for (const auto& handle : snapshot) {
      LineRuntime& line = *handle;
      const int line_id = line.config.id;

      if (poll_sinks && line.config.enabled) {
        SinkStatus sink;
        std::string error;
        const bool ok =
            daemon_->get_sink_status(line.config.aes67.sink_id, &sink, &error);
        std::lock_guard<std::mutex> lock(line.mutex);
        line.sink_receiving = ok && sink.receiving_rtp_packet;
        line.sink_error = ok && (sink.rtp_seq_id_error || sink.rtp_ssrc_error ||
                                 sink.rtp_payload_type_error || sink.rtp_sac_error);
      }

      if (!line.config.enabled) {
        continue;
      }

      LineState state = LineState::kIdle;
      int64_t ptt_hangup_at_ms = 0;
      int64_t last_dial_ms = 0;
      std::string dial_target;
      std::string mode;
      {
        std::lock_guard<std::mutex> lock(line.mutex);
        state = line.state;
        ptt_hangup_at_ms = line.ptt_hangup_at_ms;
        last_dial_ms = line.last_dial_ms;
        dial_target = line.config.sip.dial_target;
        mode = to_lower(line.config.sip.call_mode);
      }

      std::string error;

      if (is_ptt_mode(mode)) {
        // Push to talk: raise the call when the endpoint speaks, tear it down
        // after `ptt_hangup_ms` of silence.  The transmit direction is gated in
        // apply_line_to_router() so nothing leaks through while idle.
        const bool talking = router_->ptt_active(line_id);
        if (talking) {
          std::lock_guard<std::mutex> lock(line.mutex);
          line.ptt_hangup_at_ms = now + line.config.sip.ptt_hangup_ms;
        } else {
          std::lock_guard<std::mutex> lock(line.mutex);
          ptt_hangup_at_ms = line.ptt_hangup_at_ms;
        }

        if (state == LineState::kIdle && talking) {
          if (engine_ != nullptr && !engine_->dial(line_id, dial_target, &error)) {
            LOG_WARN("line ", line_id, ": PTT dial failed: ", error);
          }
        } else if (state == LineState::kRinging && talking) {
          if (engine_ != nullptr) {
            engine_->answer(line_id, &error);
          }
        } else if (state == LineState::kInCall && !talking &&
                   ptt_hangup_at_ms != 0 && now > ptt_hangup_at_ms) {
          if (engine_ != nullptr) {
            engine_->hangup(line_id, &error);
          }
          std::lock_guard<std::mutex> lock(line.mutex);
          line.ptt_hangup_at_ms = 0;
        }
      } else if (mode == "dial_out" || mode == "permanent") {
        // Keep a call up permanently, retrying every 5 s after a failure.
        const bool idle = state == LineState::kIdle || state == LineState::kError;
        if (idle && dial_target.size() > 0 && now - last_dial_ms > 5000) {
          {
            std::lock_guard<std::mutex> lock(line.mutex);
            line.last_dial_ms = now;
          }
          if (engine_ != nullptr && !engine_->dial(line_id, dial_target, &error)) {
            LOG_WARN("line ", line_id, ": cannot dial ", dial_target, ": ", error);
          }
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kSupervisionIntervalMs));
  }
}

// ---------------------------------------------------------------------------
// SipEngineCallback
// ---------------------------------------------------------------------------

void LineManager::on_media_start(int line_id, unsigned sample_rate) {
  router_->set_line_sip_rate(line_id, sample_rate);
  router_->set_line_call_active(line_id, true);
  LOG_DEBUG("line ", line_id, ": media started, ", sample_rate, " Hz codec rate");
}

void LineManager::on_media_stop(int line_id) {
  router_->set_line_call_active(line_id, false);
  LOG_DEBUG("line ", line_id, ": media stopped");
}

void LineManager::on_line_state(int line_id, LineState state, int state_code,
                                const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it == lines_.end()) {
      return;
    }
    it->second->state = state;
    it->second->state_code = state_code;
    it->second->detail = detail;
  }
  if (state != LineState::kInCall) {
    router_->set_line_call_active(line_id, false);
  }
}

void LineManager::on_incoming_call(int line_id, const std::string& remote_uri) {
  bool answer = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it == lines_.end()) {
      return;
    }
    answer = is_auto_answer_mode(it->second->config.sip.call_mode);
    it->second->detail = "incoming call from " + remote_uri;
  }
  LOG_INFO("line ", line_id, ": incoming call from ", remote_uri,
           answer ? " (auto answering)" : " (waiting for the operator)");
  if (answer && engine_ != nullptr) {
    std::string error;
    if (!engine_->answer(line_id, &error)) {
      LOG_WARN("line ", line_id, ": cannot auto answer: ", error);
    }
  }
}

// ---------------------------------------------------------------------------
// SipMediaSource
// ---------------------------------------------------------------------------

size_t LineManager::pull_to_sip(int line_id, float* destination, size_t frames) {
  return router_->pull_to_sip(line_id, destination, frames);
}

void LineManager::push_from_sip(int line_id, const float* source, size_t frames) {
  router_->push_from_sip(line_id, source, frames);
}

// ---------------------------------------------------------------------------
// line status / configuration JSON
// ---------------------------------------------------------------------------

json LineManager::line_status(int line_id) const {
  CallStatus call;
  LineConfig config;
  LineState state = LineState::kIdle;
  int state_code = 0;
  std::string detail;
  bool receiving = false;
  bool sink_error = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it == lines_.end()) {
      return nullptr;
    }
    const LineRuntime& line = *it->second;
    call = engine_ != nullptr ? engine_->call_status(line_id) : CallStatus{};
    config = line.config;
    state = line.state;
    state_code = line.state_code;
    detail = line.detail;
    receiving = line.sink_receiving;
    sink_error = line.sink_error;
  }

  const AudioRouter::LineMeters meters = router_->meters(line_id);

  json result;
  result["id"] = config.id;
  result["name"] = config.name;
  result["enabled"] = config.enabled;
  result["state"] = to_string(state);
  result["state_code"] = state_code;
  result["gain_db"] = std::round(config.gain_db * 10.0) / 10.0;
  result["mute"] = config.mute;
  result["ptt"] = meters.ptt;

  json call_json;
  call_json["remote_uri"] = call.remote_uri;
  call_json["state"] = call.state;
  call_json["duration_sec"] = call.duration_sec;
  call_json["last_error"] = !call.last_error.empty() ? call.last_error : detail;
  result["call"] = call_json;

  json aes67;
  aes67["sink_id"] = config.aes67.sink_id;
  aes67["source_id"] = config.aes67.source_id;
  aes67["channels"] = config.aes67.channels;
  aes67["receiving"] = receiving;
  aes67["error"] = sink_error;
  result["aes67"] = aes67;

  // rx/tx follow the AES67 (on site) point of view, which is what the operator
  // sees; the SIP facing levels are reported alongside for diagnostics.
  json levels;
  levels["rx_dbfs"] = dbfs_to_json(meters.capture_dbfs);
  levels["tx_dbfs"] = dbfs_to_json(meters.to_aes67_dbfs);
  levels["sip_rx_dbfs"] = dbfs_to_json(meters.from_sip_dbfs);
  levels["sip_tx_dbfs"] = dbfs_to_json(meters.to_sip_dbfs);
  result["levels"] = levels;
  return result;
}

json LineManager::lines_status() const {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : lines_) {
      ids.push_back(entry.first);
    }
  }
  json result = json::array();
  for (const int id : ids) {
    json status = line_status(id);
    if (!status.is_null()) {
      result.push_back(status);
    }
  }
  return result;
}

bool LineManager::line_config(int line_id, json* config, std::string* error) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = lines_.find(line_id);
  if (it == lines_.end()) {
    if (error != nullptr) {
      *error = "unknown line " + std::to_string(line_id);
    }
    return false;
  }
  const LineConfig& line = it->second->config;
  json aes67{{"sink_id", line.aes67.sink_id},
             {"source_id", line.aes67.source_id},
             {"channels", line.aes67.channels},
             {"stream_name", line.aes67.stream_name},
             {"auto_create_streams", line.aes67.auto_create_streams},
             {"remote_source_id", line.aes67.remote_source_id},
             {"remote_sdp", line.aes67.remote_sdp},
             {"ignore_refclk_gmid", line.aes67.ignore_refclk_gmid}};
  json sip{{"account", line.sip.account},
           {"extension", line.sip.extension},
           {"display_name", line.sip.display_name},
           {"call_mode", line.sip.call_mode},
           {"dial_target", line.sip.dial_target},
           {"auto_answer_code", line.sip.auto_answer_code},
           {"ptt_threshold_dbfs", line.sip.ptt_threshold_dbfs},
           {"ptt_hangup_ms", line.sip.ptt_hangup_ms}};
  *config = json{{"id", line.id},
                 {"name", line.name},
                 {"enabled", line.enabled},
                 {"gain_db", line.gain_db},
                 {"rx_gain_db", line.rx_gain_db},
                 {"tx_gain_db", line.tx_gain_db},
                 {"mute", line.mute},
                 {"aes67", aes67},
                 {"sip", sip}};
  return true;
}

bool LineManager::update_line(int line_id, const json& patch, std::string* error) {
  if (!patch.is_object()) {
    if (error != nullptr) {
      *error = "the request body must be a JSON object";
    }
    return false;
  }
  const int index = config_->line_index(line_id);
  if (index < 0) {
    if (error != nullptr) {
      *error = "unknown line " + std::to_string(line_id);
    }
    return false;
  }

  LineConfig merged;
  {
    // Merge onto the serialised configuration so that the same defaults are
    // applied as when the configuration file is loaded.
    json document = config_->to_json();
    json& entry = document["lines"][static_cast<size_t>(index)];
    for (auto it = patch.begin(); it != patch.end(); ++it) {
      if (it.key() == "id") {
        continue;  // the id is the key, it cannot be patched
      }
      if (it.value().is_object() && entry.contains(it.key()) &&
          entry[it.key()].is_object()) {
        for (auto sub = it.value().begin(); sub != it.value().end(); ++sub) {
          entry[it.key()][sub.key()] = sub.value();
        }
      } else {
        entry[it.key()] = it.value();
      }
    }
    const Config candidate = Config::from_json(document);
    merged = candidate.lines[static_cast<size_t>(index)];
    *config_ = candidate;
  }

  std::string save_error;
  if (!config_->save(config_path_, &save_error)) {
    LOG_WARN("line ", line_id, " updated but not persisted: ", save_error);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it != lines_.end()) {
      it->second->config = merged;
      it->second->stream_configured = false;
      if (!merged.enabled) {
        it->second->state = LineState::kDisabled;
      } else if (it->second->state == LineState::kDisabled) {
        it->second->state = LineState::kIdle;
      }
    }
  }

  if (engine_ != nullptr) {
    std::string engine_error;
    if (!engine_->add_line(merged, &engine_error)) {
      LOG_WARN("line ", line_id,
               ": cannot update the SIP registration: ", engine_error);
    }
  }
  apply_line_to_router(merged);
  configure_daemon_streams(merged);
  LOG_INFO("line ", line_id, " (", merged.name, ") updated");
  return true;
}

bool LineManager::call_action(int line_id, const std::string& action,
                              const json& body, std::string* error) {
  if (engine_ == nullptr) {
    if (error != nullptr) {
      *error = "SIP is disabled in the configuration";
    }
    return false;
  }
  const std::string verb = to_lower(action);

  if (verb == "dial") {
    std::string target = json_get<std::string>(body, "target", "");
    if (trim(target).empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = lines_.find(line_id);
      if (it == lines_.end()) {
        if (error != nullptr) {
          *error = "unknown line " + std::to_string(line_id);
        }
        return false;
      }
      target = it->second->config.sip.dial_target;
    }
    if (trim(target).empty()) {
      if (error != nullptr) {
        *error = "no dial target given and sip.dial_target is empty for line " +
                 std::to_string(line_id);
      }
      return false;
    }
    return engine_->dial(line_id, target, error);
  }
  if (verb == "answer") {
    return engine_->answer(line_id, error);
  }
  if (verb == "hangup") {
    return engine_->hangup(line_id, error);
  }
  if (verb == "hold") {
    return engine_->set_hold(line_id, json_get<bool>(body, "hold", true), error);
  }
  if (verb == "dtmf") {
    const std::string digits = json_get<std::string>(body, "digits", "");
    if (digits.empty()) {
      if (error != nullptr) {
        *error = "no digits given";
      }
      return false;
    }
    return engine_->send_dtmf(line_id, digits, error);
  }
  if (error != nullptr) {
    *error = "unknown call action '" + action +
             "' (expected dial, answer, hangup, hold or dtmf)";
  }
  return false;
}

json LineManager::self_test() {
  json checks = json::array();
  bool all_ok = true;
  const auto add = [&checks, &all_ok](const std::string& name, bool ok,
                                      const std::string& detail) {
    all_ok = all_ok && ok;
    checks.push_back(json{{"name", name}, {"ok", ok}, {"detail", detail}});
  };

  // ---- audio path --------------------------------------------------------
  const bool audio_ok = router_ != nullptr && router_->running();
  add("audio backend", audio_ok,
      router_ != nullptr ? router_->backend_detail() : "not started");

  // ---- AES67 daemon ------------------------------------------------------
  std::string daemon_version;
  std::string daemon_error;
  const bool daemon_ok =
      daemon_ != nullptr && daemon_->get_version(&daemon_version, &daemon_error);
  add("aes67 daemon", daemon_ok,
      daemon_ok ? "version " + daemon_version : daemon_error);

  PtpStatus ptp;
  std::string ptp_error;
  const bool ptp_ok =
      daemon_ != nullptr && daemon_->get_ptp_status(&ptp, &ptp_error);
  add("ptp", ptp_ok && ptp.status == "locked",
      ptp_ok ? ptp.status + ", gmid " + ptp.gmid +
                   " (audio flows only while the PTP slave is locked)"
             : ptp_error);

  // ---- SIP ---------------------------------------------------------------
  if (engine_ != nullptr) {
    for (const auto& account : engine_->account_status()) {
      std::string detail = account.state + " " + account.uri;
      if (account.code != 0) {
        detail += " (sip " + std::to_string(account.code) + ")";
      }
      if (!account.error.empty()) {
        detail += " " + account.error;
      }
      add("sip account " + account.id, account.state == "registered", detail);
    }

    // Can we even resolve the registrar?  This is the most common reason a
    // registration silently never leaves the appliance.
    for (const auto& account : config_->accounts) {
      const std::string host = registrar_dns_host(account.registrar);
      if (host.empty()) {
        add("sip registrar " + account.id, false, "no registrar configured");
        continue;
      }
      struct addrinfo hints {};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_DGRAM;
      struct addrinfo* result = nullptr;
      const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
      std::string detail;
      const bool ok = rc == 0 && result != nullptr;
      if (ok) {
        char text[INET6_ADDRSTRLEN] = {0};
        const void* address =
            result->ai_family == AF_INET
                ? static_cast<const void*>(
                      &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr)
                : static_cast<const void*>(
                      &reinterpret_cast<sockaddr_in6*>(result->ai_addr)->sin6_addr);
        ::inet_ntop(result->ai_family, address, text, sizeof(text));
        detail = host + " resolves to " + text;
      } else {
        detail = host + " does not resolve (" + ::gai_strerror(rc) + ")";
      }
      if (result != nullptr) {
        ::freeaddrinfo(result);
      }
      add("sip registrar " + account.id, ok, detail);
    }
  } else {
    add("sip engine", false, "SIP is disabled");
  }

  // ---- lines -------------------------------------------------------------
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : lines_) {
      ids.push_back(entry.first);
    }
  }
  for (const int id : ids) {
    const json status = line_status(id);
    if (status.is_null()) {
      continue;
    }
    const std::string state = json_get<std::string>(status, "state", "unknown");
    const bool enabled = json_get<bool>(status, "enabled", true);
    const bool receiving =
        json_get_path<bool>(status, {"aes67", "receiving"}, false);
    const double capture =
        json_get_path<double>(status, {"levels", "aes67_dbfs"}, -1000.0);

    std::ostringstream detail;
    detail << state;
    if (enabled && !receiving) {
      detail << ", no RTP from the endpoint (check the sink SDP, the multicast "
                "group and PTP lock)";
    } else if (capture > -999.0) {
      detail << ", input " << static_cast<int>(capture) << " dBFS";
    }
    add("line " + std::to_string(id) + " (" +
            json_get<std::string>(status, "name", "") + ")",
        !enabled || state != "error", detail.str());
  }

  return json{{"ok", all_ok}, {"checks", checks}};
}

}  // namespace aes67sip
