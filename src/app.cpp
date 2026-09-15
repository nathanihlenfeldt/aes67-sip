#include "app.hpp"

#include <csignal>
#include <thread>

#include "log.hpp"
#include "version.hpp"

namespace aes67sip {

namespace {

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int /*signal*/) {
  g_shutdown_requested = true;
}

/** Everything about the audio device that can only change with a restart. */
std::string audio_signature(const Config& config) {
  return config.audio.backend + "|" + config.audio.device + "|" +
         config.audio.format + "|" + std::to_string(config.audio.channels) + "|" +
         std::to_string(config.audio.sample_rate) + "|" +
         std::to_string(config.audio.period_frames);
}

}  // namespace

void App::request_shutdown() {
  g_shutdown_requested = true;
}

bool App::shutdown_requested() {
  return g_shutdown_requested.load();
}

App::App(AppOptions options) : options_(std::move(options)) {}

App::~App() {
  shutdown();
}

bool App::apply_log_severity(std::string* error) {
  (void)error;
  LogSeverity severity = LogSeverity::kInfo;
  if (options_.debug || config_.log_severity >= 3) {
    severity = LogSeverity::kDebug;
  } else if (config_.log_severity == 2) {
    severity = LogSeverity::kInfo;
  } else if (config_.log_severity == 1) {
    severity = LogSeverity::kWarn;
  } else {
    severity = LogSeverity::kError;
  }
  Log::instance().set_severity(severity);
  return true;
}

bool App::initialise(std::string* error) {
  // ---- configuration -----------------------------------------------------
  std::string load_error;
  if (!Config::load(options_.config_path, &config_, &load_error)) {
    if (error != nullptr) {
      *error = load_error;
    }
    return false;
  }
  config_path_ = options_.config_path;
  if (!options_.http_addr.empty()) {
    config_.http_addr = options_.http_addr;
  }
  if (options_.http_port > 0) {
    config_.http_port = options_.http_port;
  }
  if (options_.force_fake) {
    config_.audio.backend = "null";
    config_.aes67_daemon.fake = true;
    config_.sip.engine = "stub";
  }
  apply_log_severity(error);
  LOG_INFO("aes67-sip ", version(), " (", build_info(), ") starting");
  LOG_INFO("configuration loaded from ", config_path_);

  // ---- AES67 daemon ------------------------------------------------------
  daemon_ = DaemonClient::create(config_.aes67_daemon);

  // ---- audio -------------------------------------------------------------
  backend_ = create_audio_backend(config_.audio);
  if (backend_ == nullptr) {
    if (error != nullptr) {
      *error = "cannot create the audio backend";
    }
    return false;
  }
  AudioFormat format;
  format.sample_rate = config_.audio.sample_rate;
  format.channels = config_.audio.channels;
  format.period_frames = config_.audio.period_frames;
  router_ = std::make_unique<AudioRouter>(backend_.get(), format);

  // ---- lines (need a SIP engine, which needs the line manager) -----------
  lines_ = std::make_unique<LineManager>(&config_, config_path_, daemon_.get(),
                                        router_.get(), nullptr);
  if (!config_.lines.empty() && config_.audio.channels < config_.lines.size()) {
    LOG_WARN("only ", config_.audio.channels, " RAVENNA channels for ",
             config_.lines.size(),
             " lines: check audio.channels and the line channel maps");
  }

  // ---- SIP ---------------------------------------------------------------
  if (config_.sip.enabled) {
    std::string sip_error;
    engine_ = SipEngine::create(config_.sip, lines_.get(), lines_.get(), &sip_error);
    if (engine_ == nullptr) {
      if (error != nullptr) {
        *error = "cannot create the SIP engine: " + sip_error;
      }
      return false;
    }
    LOG_INFO("SIP engine: ", engine_->engine_name(), ", ", config_.sip.transport,
             " port ", config_.sip.local_port, ", ",
             config_.sip.codecs.empty() ? "default codecs"
                                        : config_.sip.codecs.front() + " preferred");
  } else {
    LOG_WARN("SIP is disabled in the configuration");
  }
  lines_->attach_engine(engine_.get());

  // ---- REST API + web UI -------------------------------------------------
  api_ = std::make_unique<ApiServer>(&config_, config_path_, daemon_.get(),
                                     router_.get(), lines_.get(), engine_.get());
  api_->set_restart_handler(
      [this](std::string* restart_error) { return restart(restart_error); });

  audio_signature_ = audio_signature(config_);
  return true;
}

bool App::restart(std::string* error) {
  LOG_INFO("re-applying the configuration");
  std::string load_error;
  Config reloaded;
  if (Config::load(config_path_, &reloaded, &load_error)) {
    const std::string previous_signature = audio_signature_;
    if (!options_.http_addr.empty()) {
      reloaded.http_addr = options_.http_addr;
    }
    if (options_.http_port > 0) {
      reloaded.http_port = options_.http_port;
    }
    config_ = reloaded;
    apply_log_severity(nullptr);
    if (audio_signature(config_) != previous_signature) {
      LOG_WARN("audio device settings changed: restart the service "
               "(systemctl restart aes67-sip) to apply them");
    }
  } else {
    LOG_WARN("cannot reload ", config_path_, ": ", load_error,
             " (keeping the running configuration)");
  }
  if (lines_ == nullptr) {
    if (error != nullptr) {
      *error = "not initialised";
    }
    return false;
  }
  return lines_->apply_configuration(error);
}

bool App::save_config(std::string* error) {
  return config_.save(config_path_, error);
}

int App::run() {
  std::string error;
  if (!initialise(&error)) {
    LOG_ERROR("cannot start: ", error);
    return 1;
  }
  if (options_.validate_only) {
    LOG_INFO("configuration is valid");
    return 0;
  }

  // ---- signals -----------------------------------------------------------
  struct sigaction action {};
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  signal(SIGPIPE, SIG_IGN);

  // ---- audio -------------------------------------------------------------
  if (!router_->start(&error)) {
    LOG_ERROR("cannot start the audio path: ", error);
    return 1;
  }

  // ---- SIP + lines -------------------------------------------------------
  if (engine_ != nullptr) {
    if (!engine_->start(&error)) {
      LOG_ERROR("cannot start the SIP engine: ", error);
      router_->stop();
      return 1;
    }
  }
  if (!lines_->apply_configuration(&error)) {
    LOG_WARN("configuration applied with errors: ", error);
  }
  if (!lines_->start_supervision()) {
    LOG_WARN("line supervision did not start");
  }

  // ---- REST API ----------------------------------------------------------
  if (!api_->start(&error)) {
    LOG_ERROR("cannot start the REST API: ", error);
    lines_->stop();
    if (engine_ != nullptr) {
      engine_->stop();
    }
    router_->stop();
    return 1;
  }
  LOG_INFO("REST API listening on http://", config_.http_addr, ":",
           config_.http_port, " (web UI served from /)");
  running_ = true;

  while (!shutdown_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  LOG_INFO("shutdown requested");
  shutdown();
  return 0;
}

void App::shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  LOG_INFO("stopping aes67-sip");
  if (api_ != nullptr) {
    api_->stop();
  }
  if (lines_ != nullptr) {
    lines_->stop();
  }
  if (engine_ != nullptr) {
    engine_->stop();
  }
  if (router_ != nullptr) {
    router_->stop();
  }
  LOG_INFO("stopped");
}

}  // namespace aes67sip

