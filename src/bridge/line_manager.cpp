// Line manager: configuration, AES67 stream provisioning and call modes.

#include "bridge/line_manager.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <set>
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

namespace {

/**
 * The conference leg as the SIP engine and the supervisor see it: a line that
 * dials the conference and keeps the call up, with no AES67 side of its own.
 */
LineConfig conference_line_config(const ConferenceConfig& conference) {
  LineConfig line;
  line.id = kConferenceLineId;
  line.name =
      conference.display_name.empty() ? "Conference" : conference.display_name;
  line.enabled = conference.enabled;
  // No device channels and no daemon streams: its media is the matrix's
  // conference sides (see AudioRouter::LineParams::conference).
  line.aes67.channels.clear();
  line.aes67.auto_create_streams = false;
  line.sip.account = conference.account;
  line.sip.display_name = line.name;
  line.sip.call_mode = "dial_out";  // dialled and retried, like a dial_out line
  line.sip.dial_target = conference.target;
  return line;
}

}  // namespace

int LineManager::line_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(lines_.size());
}

std::string LineManager::resolve_remote_sdp(const std::string& remote_sdp,
                                            const std::string& remote_source_id,
                                            const std::string& subject,
                                            std::string* origin,
                                            std::string* error) {
  const auto set_origin = [origin](const std::string& value) {
    if (origin != nullptr) {
      *origin = value;
    }
  };

  // 1. an SDP pasted into the configuration wins
  if (!trim(remote_sdp).empty()) {
    set_origin("pasted");
    return remote_sdp;
  }

  // 2. a discovered SAP/mDNS source selected in the UI
  if (!trim(remote_source_id).empty()) {
    json discovered;
    std::string browse_error;
    if (daemon_->browse_sources("all", &discovered, &browse_error)) {
      const auto it = discovered.find("remote_sources");
      if (it != discovered.end() && it->is_array()) {
        for (const auto& source : *it) {
          if (json_get<std::string>(source, "id", "") == remote_source_id) {
            const std::string sdp = json_get<std::string>(source, "sdp", "");
            if (!trim(sdp).empty()) {
              set_origin("discovered");
              return sdp;
            }
          }
        }
      }
      if (error != nullptr) {
        *error = subject + ": discovered source '" + remote_source_id +
                 "' is not currently announced";
      }
    } else if (error != nullptr) {
      *error = browse_error;
    }
    set_origin("none");
    return {};
  }

  set_origin("none");
  return {};
}

std::string LineManager::resolve_endpoint_sdp(const LineConfig& line,
                                              std::string* origin,
                                              std::string* error) {
  const auto set_origin = [origin](const std::string& value) {
    if (origin != nullptr) {
      *origin = value;
    }
  };

  const std::string remote = resolve_remote_sdp(
      line.aes67.remote_sdp, line.aes67.remote_source_id,
      "line " + std::to_string(line.id) + " (" + line.name + ")", origin, error);
  if (!remote.empty()) {
    return remote;
  }

  // 3. Nothing configured.  Looping our own source back lets the AES67 path be
  //    commissioned with the test tone before the endpoints are wired up, but it
  //    must be loud: the gateway would otherwise appear healthy while receiving
  //    nothing from site.
  if (!line.aes67.commissioning_loopback) {
    // Nothing to program.  Do NOT touch the sink: it may have been wired up on
    // purpose (Dante Controller, Q-SYS, the daemon UI) and overwriting it with a
    // loopback would silently break a working installation.
    LOG_WARN("line ", line.id,
             ": no endpoint SDP configured (aes67.remote_source_id or "
             "aes67.remote_sdp) - leaving the daemon sink ",
             line.aes67.sink_id,
             " as it is (set aes67.commissioning_loopback to loop our own source "
             "back for bench testing)");
    set_origin("unmanaged");
    return {};
  }

  std::string own_sdp;
  std::string sdp_error;
  if (daemon_->get_source_sdp(line.aes67.source_id, &own_sdp, &sdp_error)) {
    LOG_WARN("line ", line.id,
             ": commissioning_loopback is on: the sink subscribes to our OWN "
             "source, so no endpoint audio is bridged");
    set_origin("loopback");
    return own_sdp;
  }
  if (error != nullptr) {
    *error =
        "no endpoint SDP for line " + std::to_string(line.id) + ": " + sdp_error;
  }
  set_origin("none");
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
  if (!config_->aes67_daemon.auto_configure || !line.aes67.auto_create_streams) {
    return;
  }
  std::string error;

  // The source is programmed even for a disabled line, with `enabled: false`:
  // a stream created while the line was enabled lives on in the daemon (its
  // state is restored from the status file), so skipping disabled lines here
  // left stray AES67 sources transmitting silence for ever - a receiver such as
  // Q-SYS keeps showing a stream it can never use.
  const json source = DaemonClient::make_source(config_->aes67_daemon, line);
  if (!source.is_null()) {
    if (daemon_->put_source(line.aes67.source_id, source, &error)) {
      LOG_INFO("line ", line.id, ": AES67 source ", line.aes67.source_id, " '",
               line.aes67.stream_name, "' configured",
               line.enabled ? "" : " (disabled)");
    } else {
      LOG_WARN("line ", line.id, ": cannot configure AES67 source ",
               line.aes67.source_id, ": ", error);
    }
  }

  // A disabled line has no endpoint to describe, and the sink may have been
  // wired up by hand elsewhere (Dante Controller, Q-SYS, the daemon UI), so
  // leave it exactly as it is.
  if (!line.enabled) {
    return;
  }

  json sink;
  std::string sdp_error;
  std::string sdp_origin;
  const std::string remote_sdp =
      resolve_endpoint_sdp(line, &sdp_origin, &sdp_error);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line.id);
    if (it != lines_.end()) {
      it->second->sdp_source = sdp_origin;
    }
  }
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

void LineManager::configure_endpoint_streams() {
  if (!config_->aes67_daemon.auto_configure) {
    return;
  }

  // Which daemon ids are free: the lines' are configured by hand and must not be
  // taken.  The endpoints take the lowest free ones in configuration order, one
  // id per endpoint for both of its directions, so the ids do not depend on how
  // many lines the site happens to have.  Ids are bookkeeping - what a human
  // matches a stream on is its name, which carries the endpoint.
  std::set<int> taken;
  for (const auto& line : config_->lines) {
    taken.insert(line.aes67.sink_id);
    taken.insert(line.aes67.source_id);
  }
  int next_id = 0;
  std::map<std::string, int> allocated;

  for (const auto& endpoint : config_->endpoints) {
    while (taken.count(next_id) > 0) {
      ++next_id;
    }
    const int id = next_id++;
    if (!endpoint.aes67.auto_create_streams) {
      // Hand wired elsewhere (the daemon's own config, a vendor's tool): leave
      // its streams exactly as they are, but keep the id out of circulation so
      // the endpoints after it do not move.
      LOG_INFO("endpoint ", endpoint.id,
               ": aes67.auto_create_streams is off - leaving its AES67 streams "
               "as they are");
      continue;
    }
    std::string error;

    // Appliance -> endpoint: the mixes the matrix writes for it, on one stream.
    if (!endpoint.listen_channels.empty()) {
      const json source =
          DaemonClient::make_endpoint_source(config_->aes67_daemon, endpoint);
      if (daemon_->put_source(id, source, &error)) {
        LOG_INFO("endpoint ", endpoint.id, ": AES67 source ", id, " '",
                 DaemonClient::endpoint_listen_stream_name(endpoint),
                 "' configured (", endpoint.listen_channels.size(),
                 " listen channel(s))");
      } else {
        LOG_WARN("endpoint ", endpoint.id, ": cannot configure AES67 source ", id,
                 ": ", error);
      }
    }

    // Endpoint -> appliance: one sink carrying all of its talk channels.  Like a
    // line's sink it needs the endpoint's own SDP, and without one the sink is
    // left alone rather than pointed at whatever we happen to have.
    if (!endpoint.talk_channels.empty()) {
      std::string sdp_origin;
      std::string sdp_error;
      const std::string remote_sdp = resolve_remote_sdp(
          endpoint.aes67.remote_sdp, endpoint.aes67.remote_source_id,
          "endpoint " + endpoint.id + " (" + endpoint.name + ")", &sdp_origin,
          &sdp_error);
      json sink;
      if (!DaemonClient::make_endpoint_sink(config_->aes67_daemon, endpoint,
                                            remote_sdp, &sink, &error)) {
        LOG_WARN("endpoint ", endpoint.id, " (", endpoint.name, "): AES67 sink ",
                 id, " not configured: ", error);
      } else if (daemon_->put_sink(id, sink, &error)) {
        LOG_INFO("endpoint ", endpoint.id, ": AES67 sink ", id, " '",
                 DaemonClient::endpoint_talk_stream_name(endpoint),
                 "' configured (", endpoint.talk_channels.size(),
                 " talk channel(s), ", sdp_origin, " sdp)");
      } else {
        LOG_WARN("endpoint ", endpoint.id, ": cannot configure AES67 sink ", id,
                 ": ", error);
      }
    }
    // Remembered so the streams can be deleted if the endpoint is configured
    // away: the configuration file is the source of truth, and a stream left
    // behind would keep sending a mix nothing routes any more.
    allocated[endpoint.id] = id;
  }

  remove_endpoint_streams(allocated);
}

void LineManager::remove_endpoint_streams(
    const std::map<std::string, int>& allocated) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& previous : endpoint_stream_ids_) {
    if (allocated.count(previous.first) > 0) {
      continue;
    }
    std::string error;
    daemon_->delete_sink(previous.second, &error);
    daemon_->delete_source(previous.second, &error);
    LOG_INFO("endpoint ", previous.first,
             ": no longer configured - its AES67 "
             "streams (sink/source ",
             previous.second, ") were deleted");
  }
  endpoint_stream_ids_ = allocated;
}

void LineManager::remove_daemon_streams(const LineConfig& line) {
  std::string error;
  daemon_->delete_sink(line.aes67.sink_id, &error);
  daemon_->delete_source(line.aes67.source_id, &error);
}

void LineManager::apply_conference() {
  const ConferenceConfig& conference = config_->conference;
  std::shared_ptr<LineRuntime> existing;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(kConferenceLineId);
    if (it != lines_.end()) {
      existing = it->second;
    }
  }

  if (!conference.enabled) {
    if (existing == nullptr) {
      return;  // no leg was configured and none is configured now
    }
    std::string error;
    if (engine_ != nullptr) {
      engine_->hangup(kConferenceLineId, &error);
      engine_->remove_line(kConferenceLineId);
    }
    router_->remove_line(kConferenceLineId);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.erase(kConferenceLineId);
    }
    LOG_INFO(
        "conference: the leg is disabled - call, routing and registration removed");
    return;
  }

  const LineConfig line = conference_line_config(conference);
  if (existing == nullptr) {
    auto runtime = std::make_shared<LineRuntime>();
    runtime->is_conference = true;
    runtime->config = line;
    runtime->state = LineState::kIdle;
    std::lock_guard<std::mutex> lock(mutex_);
    lines_[kConferenceLineId] = std::move(runtime);
  } else {
    std::lock_guard<std::mutex> lock(mutex_);
    existing->config = line;
    if (existing->state == LineState::kDisabled) {
      existing->state = LineState::kIdle;
    }
  }

  if (engine_ != nullptr) {
    std::string engine_error;
    if (!engine_->add_line(line, &engine_error)) {
      LOG_WARN("conference: cannot register the leg with the SIP engine: ",
               engine_error);
    }
  }

  // Registered with the router like a line, but with no channels: both directions
  // are the matrix's conference sides.  The supervisor raises and keeps the call,
  // exactly as it does for a `dial_out` line.
  AudioRouter::LineParams params;
  params.conference = true;
  params.channels.clear();
  params.enabled = true;
  params.call_active = false;                // raised by the call, not by config
  params.ptt_threshold_dbfs = kSilenceDbfs;  // never push-to-talk: not an endpoint
  router_->add_line(kConferenceLineId, params, 8000);
  LOG_INFO("conference: leg on account '", conference.account, "' dialling '",
           conference.target, "'");
}

bool LineManager::apply_configuration(std::string* error) {
  // The whole configuration is validated *before* anything is touched, so a
  // configuration that could not work is refused as a whole (see
  // `validate_configuration`) rather than applied down to the parts that happen to
  // fit: no lines registered and no streams provisioned from a configuration that
  // is going to be rejected.  The plan comes out of the same call, so the matrix
  // is resolved once.
  MatrixPlan plan;
  if (!validate_configuration(*config_, &plan, error)) {
    return false;
  }

  std::vector<LineConfig> lines;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // pick up lines added or removed while the gateway was running
    for (auto it = lines_.begin(); it != lines_.end();) {
      // The conference leg is not a configured line: it is created and removed by
      // apply_conference() from the conference block.
      if (it->first != kConferenceLineId && config_->line_index(it->first) < 0) {
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
  // The intercom matrix is applied from the plan resolved above: endpoints
  // declare their shape and party lines name their members.  A matrix that
  // cannot be applied is an error, not a warning: a mistyped binding would
  // otherwise leave a department silent while the gateway reported success.
  if (matrix_ != nullptr) {
    if (!matrix_->configure(plan, error)) {
      return false;
    }
    // And the channels it mixes are what the endpoints' streams carry: without
    // the matrix those channels are nobody's routing, so nothing is provisioned.
    configure_endpoint_streams();
  }
  // The conference leg is applied whatever the matrix is doing: switching it off
  // has to remove a leg that is already up, and with no matrix attached the router
  // simply has no conference sides to carry (it checks).
  apply_conference();

  LOG_INFO("configuration applied: ", lines.size(), " line(s), ",
           config_->endpoints.size(), " endpoint(s), SIP engine ",
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

      // Refresh the sink SDP map and our own node id: together they let the
      // status tell "our own source" (commissioning loopback) from a sink that
      // is really bridging an endpoint, however it was configured.
      json sinks;
      std::string sink_error;
      if (daemon_->get_sinks(&sinks, &sink_error) && sinks.contains("sinks") &&
          sinks.at("sinks").is_array()) {
        std::lock_guard<std::mutex> lock(sink_sdp_mutex_);
        sink_sdps_.clear();
        for (const auto& entry : sinks.at("sinks")) {
          sink_sdps_[json_get<int>(entry, "id", -1)] =
              json_get<std::string>(entry, "sdp", "");
        }
      }
      if (own_node_id_.empty()) {
        json daemon_config;
        if (daemon_->get_config(&daemon_config, &sink_error)) {
          own_node_id_ = json_get<std::string>(daemon_config, "node_id", "");
        }
      }
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

        // The sink may also have been wired up directly on the AES67 page (or by
        // another tool).  If its SDP is not our own source's, it is bridging a
        // real endpoint - do not keep warning about a commissioning loopback.
        std::string sink_sdp;
        {
          std::lock_guard<std::mutex> lock(sink_sdp_mutex_);
          const auto it = sink_sdps_.find(line.config.aes67.sink_id);
          if (it != sink_sdps_.end()) {
            sink_sdp = it->second;
          }
        }

        std::lock_guard<std::mutex> lock(line.mutex);
        line.sink_receiving = ok && sink.receiving_rtp_packet;
        if (ok) {
          line.sink_in_use = sink.in_use;
        }
        line.sink_error = ok && (sink.rtp_seq_id_error || sink.rtp_ssrc_error ||
                                 sink.rtp_payload_type_error || sink.rtp_sac_error);
        if (!sink_sdp.empty()) {
          const bool is_our_own = !own_node_id_.empty() &&
                                  sink_sdp.find(own_node_id_) != std::string::npos;
          if (!is_our_own) {
            line.sdp_source = "external";
          } else if (line.sdp_source != "pasted" &&
                     line.sdp_source != "discovered") {
            line.sdp_source = "loopback";
          }
        }
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
  bool sink_in_use = true;
  bool sink_error = false;
  std::string sdp_source{"none"};
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
    sink_in_use = line.sink_in_use;
    sink_error = line.sink_error;
    sdp_source = line.sdp_source;
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
  // No stream on the sink at all is a configuration gap, not a daemon failure: the
  // difference between "nothing is arriving" and "there is nothing to arrive on".
  aes67["sink_in_use"] = sink_in_use;
  aes67["error"] = sink_error;
  // Where the sink's SDP came from: "pasted" or "discovered" bridge the real
  // endpoint; "loopback" means the sink is subscribed to our own source (a
  // commissioning aid), so no endpoint audio is being received; "none" means
  // there is no sink at all.
  aes67["sdp_source"] = sdp_source;
  result["aes67"] = aes67;

  // rx/tx follow the AES67 (on site) point of view, which is what the operator
  // sees; the SIP facing levels are reported alongside for diagnostics.
  json levels;
  levels["rx_dbfs"] = dbfs_to_json(meters.capture_dbfs);
  levels["tx_dbfs"] = dbfs_to_json(meters.to_aes67_dbfs);
  levels["sip_rx_dbfs"] = dbfs_to_json(meters.from_sip_dbfs);
  levels["sip_tx_dbfs"] = dbfs_to_json(meters.to_sip_dbfs);
  result["levels"] = levels;
  // Whether the commissioning tone is currently on this line's AES67 output.
  result["test_tone"] = router_ != nullptr && router_->test_tone_running(line_id);
  return result;
}

json LineManager::lines_status() const {
  std::vector<int> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : lines_) {
      // The conference leg has its own view (`status.conference`): it carries no
      // device channels, so it is not one of the lines this list is about.
      if (!entry.second->is_conference) {
        ids.push_back(entry.first);
      }
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

json LineManager::conference_status() const {
  const ConferenceConfig& conference = config_->conference;
  const std::string name =
      conference.display_name.empty() ? "Conference" : conference.display_name;
  LineState state = conference.enabled ? LineState::kIdle : LineState::kDisabled;
  int state_code = 0;
  std::string detail;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(kConferenceLineId);
    if (it != lines_.end()) {
      state = it->second->state;
      state_code = it->second->state_code;
      detail = it->second->detail;
    }
  }

  // The leg's directions are named for the conference, not for an endpoint: the
  // router's "capture" side of it is the mix the matrix builds *for* the
  // conference, and its "playback" side is what arrived from the PBX and went into
  // the mixes.
  AudioRouter::LineMeters levels;
  if (router_ != nullptr && conference.enabled) {
    levels = router_->meters(kConferenceLineId);
  }

  return json{{"enabled", conference.enabled},
              {"name", name},
              {"account", conference.account},
              {"target", conference.target},
              {"state", to_string(state)},
              {"state_code", state_code},
              {"detail", detail},
              {"levels",
               json{{"to_conference_dbfs", dbfs_to_json(levels.capture_dbfs)},
                    {"from_conference_dbfs", dbfs_to_json(levels.to_aes67_dbfs)}}}};
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

bool LineManager::test_tone(int line_id, const std::string& action,
                            const json& body, std::string* error) {
  bool conference = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    if (it == lines_.end()) {
      if (error != nullptr) {
        *error = "unknown line " + std::to_string(line_id);
      }
      return false;
    }
    conference = it->second->is_conference;
  }
  if (conference) {
    if (error != nullptr) {
      *error = "the conference leg carries no device channels to put a tone on";
    }
    return false;
  }
  if (router_ == nullptr) {
    if (error != nullptr) {
      *error = "the audio path is not available";
    }
    return false;
  }

  const std::string verb = to_lower(action);
  if (verb == "stop") {
    router_->stop_test_tone(line_id);
    return true;
  }
  if (verb != "start") {
    if (error != nullptr) {
      *error = "unknown tone action '" + action + "' (start | stop)";
    }
    return false;
  }

  // A line whose channels all sit outside the opened device cannot put a tone
  // anywhere: refusing is better than reporting a running tone that is never
  // written (the router only writes channels the device actually has).
  bool on_device = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lines_.find(line_id);
    for (const unsigned channel : it->second->config.aes67.channels) {
      if (channel < config_->audio.channels) {
        on_device = true;
        break;
      }
    }
  }
  if (!on_device) {
    if (error != nullptr) {
      *error = "line " + std::to_string(line_id) +
               " has no AES67 channel inside the device (aes67.channels vs "
               "audio.channels " +
               std::to_string(config_->audio.channels) + ")";
    }
    return false;
  }

  // The numbers are checked as numbers: a value of the wrong JSON type is refused
  // rather than silently replaced by the default.
  double hz = 1000.0;
  if (body.is_object() && body.contains("hz") && !body.at("hz").is_null()) {
    if (!body.at("hz").is_number()) {
      if (error != nullptr) {
        *error = "hz must be a number";
      }
      return false;
    }
    hz = body.at("hz").get<double>();
  }
  if (!(hz >= 20.0 && hz <= 20000.0)) {
    if (error != nullptr) {
      *error = "hz must be between 20 and 20000 (got " + std::to_string(hz) + ")";
    }
    return false;
  }
  double seconds = 5.0;
  if (body.is_object() && body.contains("seconds") &&
      !body.at("seconds").is_null()) {
    if (!body.at("seconds").is_number()) {
      if (error != nullptr) {
        *error = "seconds must be a number";
      }
      return false;
    }
    seconds = body.at("seconds").get<double>();
  }
  if (!(seconds >= 0.1 && seconds <= 600.0)) {
    if (error != nullptr) {
      *error = "seconds must be between 0.1 and 600 (got " +
               std::to_string(seconds) + ")";
    }
    return false;
  }
  router_->start_test_tone(line_id, hz, seconds);
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
  std::string audio_detail =
      router_ != nullptr ? router_->backend_detail() : "not started";
  // The party lines cannot be mixed or judged without the audio path, so the
  // consequence is stated where the cause is: the matrix is not accused of a
  // failure it cannot have caused, and neither is it reported healthy.
  const bool matrix_configured = matrix_ != nullptr && !matrix_->empty();
  if (!audio_ok && matrix_configured) {
    audio_detail +=
        " - the party lines are not being mixed while the audio path is down";
  }
  add("audio backend", audio_ok, audio_detail);

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
      // The conference leg is judged separately below: it has no endpoint, no
      // sink and no device channels to be silent.
      if (!entry.second->is_conference) {
        ids.push_back(entry.first);
      }
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
    // levels.rx_dbfs is the AES67 input (the SIP facing ones are sip_rx_dbfs and
    // sip_tx_dbfs); reading the wrong key here made this check report nothing.
    const double capture =
        json_get_path<double>(status, {"levels", "rx_dbfs"}, -1000.0);
    const double sip_in =
        json_get_path<double>(status, {"levels", "sip_rx_dbfs"}, -1000.0);
    const std::string sdp_source =
        json_get_path<std::string>(status, {"aes67", "sdp_source"}, "none");
    const bool sink_in_use =
        json_get_path<bool>(status, {"aes67", "sink_in_use"}, true);

    // Only expect an endpoint SDP when this gateway is supposed to create the
    // streams: a line with auto_create_streams=false is wired up by hand and must
    // not be reported as broken.
    const LineConfig* line_config = config_->find_line(id);
    const bool expects_endpoint_sdp = line_config != nullptr &&
                                      line_config->enabled &&
                                      line_config->aes67.auto_create_streams &&
                                      config_->aes67_daemon.auto_configure;

    std::ostringstream detail;
    detail << state;
    if (enabled && !receiving) {
      if (!sink_in_use) {
        // There is nothing to receive on, which is a different problem from a
        // stream that is configured but silent - and is not the daemon's fault.
        detail << ", the daemon has no stream on sink "
               << (line_config != nullptr ? line_config->aes67.sink_id : 0)
               << " yet (pick or paste the endpoint SDP, or wire it up outside the "
                  "gateway)";
      } else {
        detail << ", no RTP from the endpoint (check the sink SDP, the multicast "
                  "group and PTP lock)";
      }
    } else if (capture > -999.0) {
      detail << ", AES67 input " << static_cast<int>(capture) << " dBFS";
    }
    if (enabled) {
      detail << ", endpoint sdp: " << sdp_source;
      if (sdp_source == "loopback") {
        detail << " (commissioning loopback: no endpoint audio is bridged)";
      } else if (sdp_source == "unmanaged") {
        detail << " (sink managed outside the gateway: Dante/Q-SYS/daemon UI)";
      }
      if (state == "in_call") {
        detail << ", sip in "
               << (sip_in > -999.0 ? std::to_string(static_cast<int>(sip_in))
                                   : std::string("silent"))
               << " dBFS";
      }
    }
    // Judge the line on what is observable rather than on the configured mode:
    // an error state, a call that is silent in both directions, or an endpoint
    // that should be streaming (discovered/pasted SDP) but is not.
    const bool dead_bridge =
        enabled && state == "in_call" && capture < -999.0 && sip_in < -999.0;
    const bool endpoint_silent =
        expects_endpoint_sdp &&
        (sdp_source == "discovered" || sdp_source == "pasted") && !receiving;
    add("line " + std::to_string(id) + " (" +
            json_get<std::string>(status, "name", "") + ")",
        !enabled || (state != "error" && !dead_bridge && !endpoint_silent),
        detail.str());
  }

  // ---- the matrix: the party lines and who is on them ---------------------
  // Judged only when the audio path is up: without it the matrix cannot mix, so
  // there is nothing to judge and nothing to blame - the audio backend check says
  // what the consequence is (see its detail), and no line is reported healthy.
  if (matrix_configured && audio_ok) {
    for (const auto& line : matrix_->status()) {
      const MatrixLineSummary& summary = line.summary;
      std::ostringstream detail;
      detail << summary.members << " member(s), " << summary.arriving
             << " arriving";
      if (!summary.arriving_names.empty()) {
        detail << " (" << join(summary.arriving_names, ", ") << ")";
      }
      if (summary.silent > 0) {
        detail << ", " << summary.silent << " silent ("
               << join(summary.silent_names, ", ") << ")";
      }
      if (summary.unbound > 0) {
        detail << ", " << summary.unbound << " with no talk channel bound ("
               << join(summary.unbound_names, ", ") << ")";
      }
      if (summary.quiet()) {
        detail << " - nobody is talking on this line";
      }
      if (!summary.can_be_heard()) {
        detail << " - no member can be heard on this line";
      }
      // Not ok only when the line cannot carry anybody at all: a line whose
      // members are simply silent is nobody talking, not a fault.
      add("party line " + line.id + " (" + line.name + ")", summary.can_be_heard(),
          detail.str());
    }
  }

  // ---- conference leg ----------------------------------------------------
  // Reported only when one is configured, and reported honestly: an idle or
  // dialling leg is not a failure, an error is, and neither claims health the
  // state does not support.
  if (config_->conference.enabled) {
    const json conference = conference_status();
    const std::string state = json_get<std::string>(conference, "state", "unknown");
    const std::string detail = json_get<std::string>(conference, "detail", "");
    std::string summary = "conference leg " + state;
    if (!detail.empty()) {
      summary += ": " + detail;
    }
    add("conference leg", state != "error", summary);
  }

  return json{{"ok", all_ok}, {"checks", checks}};
}
}  // namespace aes67sip
