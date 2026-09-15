#include "aes67/daemon_client.hpp"

#include <map>
#include <mutex>
#include <sstream>

#include <httplib.h>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

/** Human readable description of a failed HTTP call. */
std::string describe(const httplib::Result& result) {
  if (!result) {
    return "no response from the AES67 daemon (" +
           httplib::to_string(result.error()) + ")";
  }
  std::ostringstream out;
  out << "AES67 daemon returned HTTP " << result->status;
  const std::string body = trim(result->body);
  if (!body.empty()) {
    out << ": " << body.substr(0, 200);
  }
  return out.str();
}

json parse_or_empty(const std::string& body) {
  if (trim(body).empty()) {
    return json::object();
  }
  try {
    return json::parse(body);
  } catch (const json::exception&) {
    return json::object();
  }
}

/** Client for a real `aes67-daemon` over its REST API. */
class HttpDaemonClient : public DaemonClient {
 public:
  explicit HttpDaemonClient(const Aes67DaemonConfig& config)
      : config_(config),
        client_(config.address, config.port),
        endpoint_(config.address + ":" + std::to_string(config.port)) {
    client_.set_connection_timeout(1, 0);
    client_.set_read_timeout(3, 0);
    client_.set_write_timeout(3, 0);
  }

  bool connected() const override { return connected_.load(); }

  std::string last_error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
  }

  std::string endpoint() const override { return endpoint_; }

  bool get_version(std::string* version, std::string* error) override {
    json document;
    if (!get("/api/version", &document, error)) {
      return false;
    }
    if (version != nullptr) {
      *version = json_get<std::string>(document, "version", "");
      if (version->empty() && document.is_object()) {
        *version = document.dump();
      }
    }
    return true;
  }

  bool get_config(json* config, std::string* error) override {
    return get("/api/config", config, error);
  }

  bool set_config(const json& config, std::string* error) override {
    return post("/api/config", config, error);
  }

  bool get_ptp_status(PtpStatus* status, std::string* error) override {
    json document;
    if (!get("/api/ptp/status", &document, error)) {
      return false;
    }
    if (status != nullptr) {
      status->status = json_get<std::string>(document, "status", "unknown");
      status->gmid = json_get<std::string>(document, "gmid", "");
      status->jitter = json_get<int>(document, "jitter", 0);
    }
    return true;
  }

  bool get_sinks(json* sinks, std::string* error) override {
    return get("/api/sinks", sinks, error);
  }

  bool get_sources(json* sources, std::string* error) override {
    return get("/api/sources", sources, error);
  }

  bool put_sink(int id, const json& sink, std::string* error) override {
    return put("/api/sink/" + std::to_string(id), sink, error);
  }

  bool delete_sink(int id, std::string* error) override {
    return remove("/api/sink/" + std::to_string(id), error);
  }

  bool put_source(int id, const json& source, std::string* error) override {
    return put("/api/source/" + std::to_string(id), source, error);
  }

  bool delete_source(int id, std::string* error) override {
    return remove("/api/source/" + std::to_string(id), error);
  }

  bool get_sink_status(int id, SinkStatus* status, std::string* error) override {
    json document;
    if (!get("/api/sink/status/" + std::to_string(id), &document, error)) {
      return false;
    }
    if (status != nullptr) {
      const json flags = json_get<json>(document, "sink_flags", json::object());
      status->receiving_rtp_packet =
          json_get<bool>(flags, "receiving_rtp_packet", false);
      status->muted = json_get<bool>(flags, "muted", false);
      status->rtp_seq_id_error = json_get<bool>(flags, "rtp_seq_id_error", false);
      status->rtp_ssrc_error = json_get<bool>(flags, "rtp_ssrc_error", false);
      status->rtp_payload_type_error =
          json_get<bool>(flags, "rtp_payload_type_error", false);
      status->rtp_sac_error = json_get<bool>(flags, "rtp_sac_error", false);
    }
    return true;
  }

  bool get_source_sdp(int id, std::string* sdp, std::string* error) override {
    auto result = client_.Get("/api/source/sdp/" + std::to_string(id));
    if (!accept(result, error)) {
      return false;
    }
    if (sdp != nullptr) {
      *sdp = result->body;
    }
    return true;
  }

  bool browse_sources(const std::string& kind, json* sources,
                      std::string* error) override {
    const std::string which = (kind == "mdns" || kind == "sap") ? kind : "all";
    return get("/api/browse/sources/" + which, sources, error);
  }

 private:
  void mark_connected() {
    connected_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
  }

  bool fail(const std::string& message, std::string* error) {
    connected_ = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_error_ = message;
    }
    if (error != nullptr) {
      *error = message;
    }
    return false;
  }

  bool accept(const httplib::Result& result, std::string* error) {
    if (!result || result->status != 200) {
      return fail(describe(result), error);
    }
    mark_connected();
    return true;
  }

  bool get(const std::string& path, json* document, std::string* error) {
    auto result = client_.Get(path);
    if (!accept(result, error)) {
      return false;
    }
    if (document != nullptr) {
      *document = parse_or_empty(result->body);
    }
    return true;
  }

  bool post(const std::string& path, const json& body, std::string* error) {
    return accept(client_.Post(path, body.dump(), "application/json"), error);
  }

  bool put(const std::string& path, const json& body, std::string* error) {
    return accept(client_.Put(path, body.dump(), "application/json"), error);
  }

  bool remove(const std::string& path, std::string* error) {
    return accept(client_.Delete(path), error);
  }

  Aes67DaemonConfig config_;
  httplib::Client client_;
  std::string endpoint_;
  std::atomic<bool> connected_{false};
  mutable std::mutex mutex_;
  std::string last_error_;
};

/**
 * In-process simulation of the AES67 daemon.
 *
 * Enabled with `-f` / `aes67_daemon.fake` so the gateway, its REST API and the
 * web UI can be developed and tested without the RAVENNA kernel module.  It
 * keeps the configured streams in memory and reports a locked PTP slave.
 */
class FakeDaemonClient : public DaemonClient {
 public:
  explicit FakeDaemonClient(const Aes67DaemonConfig& config) : config_(config) {
    LOG_WARN(
        "using the simulated AES67 daemon (fake); no audio reaches the network");
    config_json_ =
        json{{"interface_name", "lo"}, {"http_port", config_.port},
             {"sample_rate", 48000},   {"rtp_mcast_base", config_.rtp_mcast_base},
             {"rtp_port", 5004},       {"tic_frame_size_at_1fs", 48}};
  }

  bool connected() const override { return true; }
  std::string last_error() const override { return {}; }
  std::string endpoint() const override { return "127.0.0.1 (fake)"; }

  bool get_version(std::string* version, std::string* error) override {
    (void)error;
    if (version != nullptr) {
      *version = "0.0.0-fake";
    }
    return true;
  }

  bool get_config(json* config, std::string* error) override {
    (void)error;
    if (config != nullptr) {
      *config = config_json_;
    }
    return true;
  }

  bool set_config(const json& config, std::string* error) override {
    (void)error;
    for (auto it = config.begin(); it != config.end(); ++it) {
      config_json_[it.key()] = it.value();
    }
    LOG_DEBUG("fake daemon config update: ", config.dump());
    return true;
  }

  bool get_ptp_status(PtpStatus* status, std::string* error) override {
    (void)error;
    if (status != nullptr) {
      status->status = "locked";
      status->gmid = "00-10-4B-FF-FE-7A-87-FC";
      status->jitter = 0;
    }
    return true;
  }

  bool get_sinks(json* sinks, std::string* error) override {
    (void)error;
    if (sinks != nullptr) {
      json list = json::array();
      for (const auto& entry : sinks_) {
        json sink = entry.second;
        sink["id"] = entry.first;
        list.push_back(sink);
      }
      *sinks = json{{"sinks", list}};
    }
    return true;
  }

  bool get_sources(json* sources, std::string* error) override {
    (void)error;
    if (sources != nullptr) {
      json list = json::array();
      for (const auto& entry : sources_) {
        json source = entry.second;
        source["id"] = entry.first;
        list.push_back(source);
      }
      *sources = json{{"sources", list}};
    }
    return true;
  }

  bool put_sink(int id, const json& sink, std::string* error) override {
    (void)error;
    sinks_[id] = sink;
    LOG_INFO("fake daemon: sink ", id, " <- ", sink.dump());
    return true;
  }

  bool delete_sink(int id, std::string* error) override {
    (void)error;
    sinks_.erase(id);
    return true;
  }

  bool put_source(int id, const json& source, std::string* error) override {
    (void)error;
    sources_[id] = source;
    LOG_INFO("fake daemon: source ", id, " <- ", source.dump());
    return true;
  }

  bool delete_source(int id, std::string* error) override {
    (void)error;
    sources_.erase(id);
    return true;
  }

  bool get_sink_status(int id, SinkStatus* status, std::string* error) override {
    (void)error;
    if (status != nullptr) {
      status->receiving_rtp_packet = sinks_.find(id) != sinks_.end();
      status->muted = false;
    }
    return true;
  }

  bool get_source_sdp(int id, std::string* sdp, std::string* error) override {
    (void)error;
    if (sdp != nullptr) {
      *sdp = intercom_sdp(id, 0x40000000u + static_cast<unsigned>(id));
    }
    return true;
  }

  bool browse_sources(const std::string& kind, json* sources,
                      std::string* error) override {
    (void)error;
    if (sources != nullptr) {
      json list = json::array();
      for (int id = 0; id < 3; ++id) {
        list.push_back(json{{"source", kind == "mdns" ? "mDNS" : "SAP"},
                            {"id", "fake" + std::to_string(id)},
                            {"name", "Intercom endpoint " + std::to_string(id + 1)},
                            {"domain", ""},
                            {"address", "10.0.0." + std::to_string(21 + id)},
                            {"sdp", intercom_sdp(id + 1, 0x40000000u + 1u + id)},
                            {"last_seen", 3 * id},
                            {"announce_period", 30}});
      }
      *sources = json{{"remote_sources", list}};
    }
    return true;
  }

 private:
  /** Plausible AES67 SDP for an intercom endpoint (L16, 48 kHz, stereo). */
  static std::string intercom_sdp(int id, unsigned ssrc) {
    const std::string multicast = next_multicast(0, id);
    std::ostringstream out;
    out << "v=0\r\n"
        << "o=- " << ssrc << " 0 IN IP4 127.0.0.1\r\n"
        << "s=Intercom " << id << "\r\n"
        << "c=IN IP4 " << multicast << "/15\r\n"
        << "t=0 0\r\n"
        << "a=clock-domain:PTPv2 0\r\n"
        << "m=audio 5004 RTP/AVP 98\r\n"
        << "c=IN IP4 " << multicast << "/15\r\n"
        << "a=rtpmap:98 L16/48000/2\r\n"
        << "a=sync-time:0\r\n"
        << "a=framecount:48\r\n"
        << "a=ptime:1\r\n"
        << "a=mediaclk:direct=0\r\n"
        << "a=ts-refclk:ptp=IEEE1588-2008:00-10-4B-FF-FE-7A-87-FC:0\r\n"
        << "a=recvonly\r\n";
    return out.str();
  }

  static std::string next_multicast(int base_id, int id) {
    return "239.1.0." + std::to_string(base_id + id + 1);
  }

  Aes67DaemonConfig config_;
  json config_json_;
  std::map<int, json> sinks_;
  std::map<int, json> sources_;
};

}  // namespace

// ---------------------------------------------------------------------------
// factory and stream document builders
// ---------------------------------------------------------------------------

std::unique_ptr<DaemonClient> DaemonClient::create(
    const Aes67DaemonConfig& config) {
  if (config.fake) {
    return std::make_unique<FakeDaemonClient>(config);
  }
  return std::make_unique<HttpDaemonClient>(config);
}

json DaemonClient::make_source(const Aes67DaemonConfig& daemon_config,
                               const LineConfig& line) {
  return json{{"enabled", line.enabled},
              {"name", line.aes67.stream_name},
              {"io", "Audio Device"},
              {"codec", "L16"},
              {"address", ""},  // let the daemon pick from its multicast base
              {"max_samples_per_packet", 48},
              {"ttl", daemon_config.source_ttl},
              {"payload_type", daemon_config.source_payload_type},
              {"dscp", daemon_config.source_dscp},
              {"refclk_ptp_traceable", true},
              {"map", line.aes67.channels}};
}

bool DaemonClient::make_sink(const Aes67DaemonConfig& daemon_config,
                             const LineConfig& line, const std::string& remote_sdp,
                             json* sink, std::string* error) {
  if (sink == nullptr) {
    if (error != nullptr) {
      *error = "internal error: sink document pointer is null";
    }
    return false;
  }
  if (trim(remote_sdp).empty()) {
    if (error != nullptr) {
      *error = "line " + std::to_string(line.id) + " (" + line.name +
               ") has no endpoint SDP: pick a discovered SAP/mDNS source or paste "
               "the endpoint SDP (aes67.remote_source_id / aes67.remote_sdp)";
    }
    return false;
  }

  // use_sdp = true selects the inline `sdp` document; the daemon only fetches
  // from the `source` URL when use_sdp is false.
  *sink = json{{"name", line.aes67.stream_name},
               {"io", "Audio Device"},
               {"delay", daemon_config.sink_delay_samples},
               {"use_sdp", true},
               {"source", ""},
               {"sdp", remote_sdp},
               {"ignore_refclk_gmid", line.aes67.ignore_refclk_gmid},
               {"map", line.aes67.channels}};
  return true;
}

}  // namespace aes67sip
