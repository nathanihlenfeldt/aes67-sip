#include "http/api_server.hpp"

#include <sys/stat.h>

#include <httplib.h>

#include "log.hpp"
#include "version.hpp"

namespace aes67sip {

namespace {

bool directory_exists(const std::string& path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return (info.st_mode & S_IFDIR) != 0;
}

void reply_json(httplib::Response& response, const json& body, int status = 200) {
  response.status = status;
  response.set_content(body.dump(), "application/json");
}

void reply_error(httplib::Response& response, int status,
                 const std::string& message) {
  response.status = status;
  response.set_content(message, "text/plain");
}

json parse_body(const httplib::Request& request, std::string* error) {
  if (trim(request.body).empty()) {
    *error = "empty request body";
    return nullptr;
  }
  try {
    return json::parse(request.body);
  } catch (const json::exception& ex) {
    *error = std::string("invalid JSON body: ") + ex.what();
    return nullptr;
  }
}

/** Extracts a numeric path parameter ("/api/lines/([0-9]+)"). */
bool path_int(const httplib::Request& request, int* value) {
  if (request.matches.size() < 2) {
    return false;
  }
  try {
    *value = std::stoi(request.matches[1].str());
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string path_group(const httplib::Request& request, size_t index) {
  if (request.matches.size() <= index) {
    return {};
  }
  return request.matches[index].str();
}

}  // namespace

ApiServer::ApiServer(Config* config, std::string config_path, DaemonClient* daemon,
                     AudioRouter* router, LineManager* lines, SipEngine* engine)
    : config_(config),
      config_path_(std::move(config_path)),
      daemon_(daemon),
      router_(router),
      lines_(lines),
      engine_(engine) {}

ApiServer::~ApiServer() {
  stop();
}

void ApiServer::set_restart_handler(RestartHandler handler) {
  restart_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// status document (single poll endpoint used by the web UI)
// ---------------------------------------------------------------------------

json ApiServer::build_status() const {
  const AudioRouter::LineMeters unused{};
  (void)unused;

  json status;
  status["version"] = version();
  status["build"] = build_info();
  status["uptime_sec"] = static_cast<int64_t>((now_ms() - started_at_ms_) / 1000);

  json audio;
  audio["backend"] = router_->backend_kind();
  audio["device"] = config_->audio.device;
  audio["sample_rate"] = router_->format().sample_rate;
  audio["channels"] = router_->format().channels;
  audio["period_frames"] = router_->format().period_frames;
  audio["state"] = router_->running() ? "running" : "stopped";
  audio["error"] = router_->last_error();
  audio["rx_overruns"] = router_->backend_overruns();
  audio["tx_underruns"] = router_->backend_underruns();
  json channel_levels = json::array();
  for (const double level : router_->capture_channel_dbfs()) {
    channel_levels.push_back(dbfs_to_json(level));
  }
  audio["levels_dbfs"] = channel_levels;
  json playback_levels = json::array();
  for (const double level : router_->playback_channel_dbfs()) {
    playback_levels.push_back(dbfs_to_json(level));
  }
  audio["playback_dbfs"] = playback_levels;
  status["audio"] = audio;

  json aes67;
  aes67["connected"] = daemon_->connected();
  aes67["address"] = daemon_->endpoint();
  aes67["fake"] = config_->aes67_daemon.fake;
  aes67["error"] = daemon_->last_error();
  PtpStatus ptp;
  std::string ptp_error;
  if (daemon_->get_ptp_status(&ptp, &ptp_error)) {
    aes67["ptp"] =
        json{{"status", ptp.status}, {"gmid", ptp.gmid}, {"jitter", ptp.jitter}};
  } else {
    aes67["ptp"] = json{{"status", "unknown"}, {"gmid", ""}, {"jitter", 0}};
  }
  status["aes67"] = aes67;

  json sip;
  sip["engine"] = engine_ != nullptr ? engine_->engine_name() : "none";
  sip["enabled"] = config_->sip.enabled;
  sip["local_port"] = config_->sip.local_port;
  sip["transport"] = config_->sip.transport;
  sip["codecs"] = config_->sip.codecs;
  json accounts = json::array();
  if (engine_ != nullptr) {
    for (const auto& account : engine_->account_status()) {
      accounts.push_back(json{{"id", account.id},
                              {"state", account.state},
                              {"uri", account.uri},
                              {"error", account.error}});
    }
  }
  sip["accounts"] = accounts;
  status["sip"] = sip;

  status["lines"] = lines_->lines_status();
  return status;
}

// ---------------------------------------------------------------------------
// routes
// ---------------------------------------------------------------------------

void ApiServer::register_routes() {
  httplib::Server* svr = server_.get();

  svr->Options("/api/.*", [](const httplib::Request&, httplib::Response& response) {
    response.status = 204;
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods",
                        "GET,POST,PUT,DELETE,OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
  });

  svr->Get("/api/version",
           [](const httplib::Request&, httplib::Response& response) {
             reply_json(response, json{{"name", "aes67-sip"},
                                       {"version", version()},
                                       {"build", build_info()}});
           });

  svr->Get("/api/config",
           [this](const httplib::Request&, httplib::Response& response) {
             reply_json(response, config_->to_json());
           });

  svr->Post("/api/config",
            [this](const httplib::Request& request, httplib::Response& response) {
              std::string error;
              const json patch = parse_body(request, &error);
              if (patch.is_null()) {
                reply_error(response, 400, error);
                return;
              }
              config_->merge(patch);
              if (!config_->save(config_path_, &error)) {
                reply_error(response, 500, error);
                return;
              }
              if (restart_ && !restart_(&error)) {
                reply_error(response, 500, error);
                return;
              }
              reply_json(response, config_->to_json());
            });

  svr->Get("/api/status",
           [this](const httplib::Request&, httplib::Response& response) {
             reply_json(response, build_status());
           });

  svr->Get("/api/lines",
           [this](const httplib::Request&, httplib::Response& response) {
             reply_json(response, lines_->lines_status());
           });

  svr->Get(R"(/api/lines/([0-9]+))", [this](const httplib::Request& request,
                                            httplib::Response& response) {
    int line_id = 0;
    if (!path_int(request, &line_id)) {
      reply_error(response, 400, "invalid line id");
      return;
    }
    const json status = lines_->line_status(line_id);
    if (status.is_null()) {
      reply_error(response, 404, "unknown line " + std::to_string(line_id));
      return;
    }
    reply_json(response, status);
  });

  svr->Get(R"(/api/lines/([0-9]+)/config)",
           [this](const httplib::Request& request, httplib::Response& response) {
             int line_id = 0;
             if (!path_int(request, &line_id)) {
               reply_error(response, 400, "invalid line id");
               return;
             }
             json config;
             std::string error;
             if (!lines_->line_config(line_id, &config, &error)) {
               reply_error(response, 404, error);
               return;
             }
             reply_json(response, config);
           });

  svr->Post(R"(/api/lines/([0-9]+)/config)",
            [this](const httplib::Request& request, httplib::Response& response) {
              int line_id = 0;
              if (!path_int(request, &line_id)) {
                reply_error(response, 400, "invalid line id");
                return;
              }
              std::string error;
              const json patch = parse_body(request, &error);
              if (patch.is_null()) {
                reply_error(response, 400, error);
                return;
              }
              if (!lines_->update_line(line_id, patch, &error)) {
                reply_error(response, 400, error);
                return;
              }
              reply_json(response, lines_->line_status(line_id));
            });

  svr->Post(R"(/api/lines/([0-9]+)/call)",
            [this](const httplib::Request& request, httplib::Response& response) {
              int line_id = 0;
              if (!path_int(request, &line_id)) {
                reply_error(response, 400, "invalid line id");
                return;
              }
              std::string error;
              const json body = parse_body(request, &error);
              if (body.is_null()) {
                reply_error(response, 400, error);
                return;
              }
              const std::string action = json_get<std::string>(body, "action", "");
              if (action.empty()) {
                reply_error(response, 400, "missing 'action'");
                return;
              }
              if (!lines_->call_action(line_id, action, body, &error)) {
                reply_error(response, 400, error);
                return;
              }
              reply_json(response, json{{"ok", true},
                                        {"line", lines_->line_status(line_id)}});
            });

  svr->Get(R"(/api/lines/([0-9]+)/levels)", [this](const httplib::Request& request,
                                                   httplib::Response& response) {
    int line_id = 0;
    if (!path_int(request, &line_id)) {
      reply_error(response, 400, "invalid line id");
      return;
    }
    const json status = lines_->line_status(line_id);
    if (status.is_null()) {
      reply_error(response, 404, "unknown line " + std::to_string(line_id));
      return;
    }
    reply_json(response, status.value("levels", json::object()));
  });

  svr->Get("/api/log", [](const httplib::Request& request,
                          httplib::Response& response) {
    size_t count = 200;
    if (request.has_param("lines")) {
      try {
        count = static_cast<size_t>(std::stoul(request.get_param_value("lines")));
      } catch (const std::exception&) {
        count = 200;
      }
    }
    reply_json(response, json{{"lines", Log::instance().tail(count)}});
  });

  svr->Post("/api/system/restart", [this](const httplib::Request&,
                                          httplib::Response& response) {
    if (!restart_) {
      reply_error(response, 501, "restart is not supported in this build");
      return;
    }
    std::string error;
    if (!restart_(&error)) {
      reply_error(response, 500, error);
      return;
    }
    reply_json(response, json{{"ok", true}});
  });

  svr->Post("/api/system/self-test",
            [this](const httplib::Request&, httplib::Response& response) {
              reply_json(response, lines_->self_test());
            });

  // ---- AES67 daemon passthrough ------------------------------------------
  svr->Get("/api/aes67/config",
           [this](const httplib::Request&, httplib::Response& response) {
             json daemon_config;
             std::string error;
             if (!daemon_->get_config(&daemon_config, &error)) {
               reply_error(response, 502, error);
               return;
             }
             reply_json(response, daemon_config);
           });

  svr->Post("/api/aes67/config",
            [this](const httplib::Request& request, httplib::Response& response) {
              std::string error;
              const json body = parse_body(request, &error);
              if (body.is_null()) {
                reply_error(response, 400, error);
                return;
              }
              if (!daemon_->set_config(body, &error)) {
                reply_error(response, 502, error);
                return;
              }
              reply_json(response, json{{"ok", true}});
            });

  svr->Get("/api/aes67/ptp/status", [this](const httplib::Request&,
                                           httplib::Response& response) {
    PtpStatus ptp;
    std::string error;
    if (!daemon_->get_ptp_status(&ptp, &error)) {
      reply_error(response, 502, error);
      return;
    }
    reply_json(
        response,
        json{{"status", ptp.status}, {"gmid", ptp.gmid}, {"jitter", ptp.jitter}});
  });

  svr->Get("/api/aes67/sinks",
           [this](const httplib::Request&, httplib::Response& response) {
             json sinks;
             std::string error;
             if (!daemon_->get_sinks(&sinks, &error)) {
               reply_error(response, 502, error);
               return;
             }
             reply_json(response, sinks);
           });

  svr->Get("/api/aes67/sources",
           [this](const httplib::Request&, httplib::Response& response) {
             json sources;
             std::string error;
             if (!daemon_->get_sources(&sources, &error)) {
               reply_error(response, 502, error);
               return;
             }
             reply_json(response, sources);
           });

  svr->Get(
      R"(/api/aes67/browse/sources/(all|mdns|sap))",
      [this](const httplib::Request& request, httplib::Response& response) {
        json sources;
        std::string error;
        if (!daemon_->browse_sources(path_group(request, 1), &sources, &error)) {
          reply_error(response, 502, error);
          return;
        }
        reply_json(response, sources);
      });

  svr->Get(R"(/api/aes67/source/sdp/([0-9]+))",
           [this](const httplib::Request& request, httplib::Response& response) {
             int source_id = 0;
             if (!path_int(request, &source_id)) {
               reply_error(response, 400, "invalid source id");
               return;
             }
             std::string sdp;
             std::string error;
             if (!daemon_->get_source_sdp(source_id, &sdp, &error)) {
               reply_error(response, 502, error);
               return;
             }
             response.status = 200;
             response.set_content(sdp, "application/sdp");
           });

  svr->Put(R"(/api/aes67/sinks/([0-9]+))",
           [this](const httplib::Request& request, httplib::Response& response) {
             int sink_id = 0;
             if (!path_int(request, &sink_id)) {
               reply_error(response, 400, "invalid sink id");
               return;
             }
             std::string error;
             const json body = parse_body(request, &error);
             if (body.is_null()) {
               reply_error(response, 400, error);
               return;
             }
             if (!daemon_->put_sink(sink_id, body, &error)) {
               reply_error(response, 502, error);
               return;
             }
             reply_json(response, json{{"ok", true}});
           });

  svr->Delete(R"(/api/aes67/sinks/([0-9]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                int sink_id = 0;
                if (!path_int(request, &sink_id)) {
                  reply_error(response, 400, "invalid sink id");
                  return;
                }
                std::string error;
                if (!daemon_->delete_sink(sink_id, &error)) {
                  reply_error(response, 502, error);
                  return;
                }
                reply_json(response, json{{"ok", true}});
              });

  svr->Put(R"(/api/aes67/sources/([0-9]+))",
           [this](const httplib::Request& request, httplib::Response& response) {
             int source_id = 0;
             if (!path_int(request, &source_id)) {
               reply_error(response, 400, "invalid source id");
               return;
             }
             std::string error;
             const json body = parse_body(request, &error);
             if (body.is_null()) {
               reply_error(response, 400, error);
               return;
             }
             if (!daemon_->put_source(source_id, body, &error)) {
               reply_error(response, 502, error);
               return;
             }
             reply_json(response, json{{"ok", true}});
           });

  svr->Delete(R"(/api/aes67/sources/([0-9]+))",
              [this](const httplib::Request& request, httplib::Response& response) {
                int source_id = 0;
                if (!path_int(request, &source_id)) {
                  reply_error(response, 400, "invalid source id");
                  return;
                }
                std::string error;
                if (!daemon_->delete_source(source_id, &error)) {
                  reply_error(response, 502, error);
                  return;
                }
                reply_json(response, json{{"ok", true}});
              });

  // ---- static web UI -----------------------------------------------------
  if (!config_->webui_dir.empty() && directory_exists(config_->webui_dir)) {
    svr->set_mount_point("/", config_->webui_dir);
    const std::string index = config_->webui_dir + "/index.html";
    struct stat info {};
    if (stat(index.c_str(), &info) == 0) {
      // single page application fallback for client side routes
      svr->Get("/(.*)", [index](const httplib::Request& request,
                                httplib::Response& response) {
        if (request.path.rfind("/api/", 0) == 0) {
          reply_error(response, 404, "unknown API endpoint " + request.path);
          return;
        }
        std::ifstream input(index, std::ios::binary);
        if (!input.is_open()) {
          reply_error(response, 500, "cannot read " + index);
          return;
        }
        std::stringstream buffer;
        buffer << input.rdbuf();
        response.status = 200;
        response.set_content(buffer.str(), "text/html");
      });
    }
    LOG_INFO("serving web UI from ", config_->webui_dir);
  } else {
    LOG_WARN("web UI directory '", config_->webui_dir,
             "' not found: only the REST API is served");
  }

  svr->set_error_handler([](const httplib::Request&, httplib::Response& response) {
    if (response.status == 404 && response.body.empty()) {
      response.set_content("not found", "text/plain");
    }
  });

  // A handler that throws (bad JSON, unexpected state) must return a useful
  // error instead of silently dropping the connection.
  svr->set_exception_handler([](const httplib::Request& request,
                                httplib::Response& response,
                                std::exception_ptr error) {
    std::string message = "internal error";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& ex) {
      message = std::string("internal error: ") + ex.what();
    } catch (...) {
      message = "internal error: unknown exception";
    }
    LOG_ERROR("request ", request.method, " ", request.path, " failed: ", message);
    reply_error(response, 500, message);
  });
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool ApiServer::start(std::string* error) {
  if (running_) {
    return true;
  }
  server_ = std::make_unique<httplib::Server>();
  register_routes();

  const std::string address =
      config_->http_addr.empty() ? "0.0.0.0" : config_->http_addr;
  if (!server_->bind_to_port(address, config_->http_port)) {
    if (error) {
      *error = "cannot bind " + address + ":" + std::to_string(config_->http_port) +
               " (is another instance already running?)";
    }
    server_.reset();
    return false;
  }

  started_at_ms_ = now_ms();
  running_ = true;
  thread_ = std::thread([this] {
    set_thread_name("aes67-http");
    LOG_INFO("REST API listening on http://", config_->http_addr, ":",
             config_->http_port);
    server_->listen_after_bind();
    running_ = false;
  });
  return true;
}

void ApiServer::stop() {
  if (!running_ && !thread_.joinable()) {
    return;
  }
  if (server_) {
    server_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  server_.reset();
  running_ = false;
}

}  // namespace aes67sip
