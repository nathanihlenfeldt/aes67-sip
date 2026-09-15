#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "aes67/daemon_client.hpp"
#include "audio/router.hpp"
#include "bridge/line_manager.hpp"
#include "config.hpp"
#include "sip/engine.hpp"
#include "util.hpp"

namespace httplib {
class Server;
}  // namespace httplib

namespace aes67sip {

/**
 * Local REST API and static web UI host.
 *
 * Implements the endpoints documented in docs/api.md, including the AES67
 * daemon passthrough routes, so that the web UI only ever talks to one origin.
 */
class ApiServer {
 public:
  using RestartHandler = std::function<bool(std::string*)>;

  ApiServer(Config* config, std::string config_path, DaemonClient* daemon,
            AudioRouter* router, LineManager* lines, SipEngine* engine);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  void set_restart_handler(RestartHandler handler);

  bool start(std::string* error);
  void stop();

  int port() const { return config_->http_port; }

 private:
  void register_routes();
  json build_status() const;

  Config* config_{nullptr};
  std::string config_path_;
  DaemonClient* daemon_{nullptr};
  AudioRouter* router_{nullptr};
  LineManager* lines_{nullptr};
  SipEngine* engine_{nullptr};

  RestartHandler restart_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  int64_t started_at_ms_{0};
  bool running_{false};
};

}  // namespace aes67sip
