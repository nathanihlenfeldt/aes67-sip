#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "aes67/daemon_client.hpp"
#include "audio/backend.hpp"
#include "audio/router.hpp"
#include "bridge/line_manager.hpp"
#include "config.hpp"
#include "http/api_server.hpp"
#include "sip/engine.hpp"
#include "util.hpp"

namespace aes67sip {

/** Command line options, see `aes67-sip --help`. */
struct AppOptions {
  std::string config_path{"/etc/aes67-sip.conf"};
  std::string http_addr;      // overrides config->http_addr when set
  int http_port{0};           // overrides config->http_port when non zero
  bool force_fake{false};     // null audio + stub SIP + simulated daemon
  bool debug{false};          // raise the log severity to debug
  bool validate_only{false};  // load the configuration and exit
};

/**
 * Owns the process: configuration, the AES67 audio path, the SIP engine, the
 * line manager and the REST API.
 *
 * Construction order matters: the SIP engine needs the line manager as its
 * callback and media source, so the line manager is created first with a null
 * engine and receives it afterwards through `attach_engine()`.
 */
class App {
 public:
  explicit App(AppOptions options);
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  /** Runs until a termination signal arrives; returns the process exit code. */
  int run();

  /** Called from the signal handler (async-signal-safe). */
  static void request_shutdown();
  static bool shutdown_requested();

 private:
  bool initialise(std::string* error);
  void shutdown();

  /** `POST /api/system/restart`: re-applies the configuration to all planes. */
  bool restart(std::string* error);
  bool save_config(std::string* error);
  bool apply_log_severity(std::string* error);

  AppOptions options_;
  Config config_;
  std::string config_path_;

  std::unique_ptr<DaemonClient> daemon_;
  std::unique_ptr<AudioBackend> backend_;
  std::unique_ptr<AudioRouter> router_;
  std::unique_ptr<LineManager> lines_;
  std::unique_ptr<SipEngine> engine_;
  std::unique_ptr<ApiServer> api_;

  std::string audio_signature_;  // detects audio settings that need a restart
  std::atomic<bool> running_{false};
};

}  // namespace aes67sip
