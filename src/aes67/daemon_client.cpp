#include "aes67/daemon_client.hpp"

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
