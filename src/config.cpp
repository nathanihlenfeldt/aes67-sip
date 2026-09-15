#include "config.hpp"

#include <cstdio>
#include <fstream>

#include "log.hpp"

namespace aes67sip {

namespace {

// ---------------------------------------------------------------------------
// serialisation helpers
// ---------------------------------------------------------------------------

json account_to_json(const SipAccountConfig& account) {
  return json{{"id", account.id},
              {"registrar", account.registrar},
              {"username", account.username},
              {"password", account.password},
              {"auth_realm", account.auth_realm},
              {"outbound_proxy", account.outbound_proxy},
              {"display_name", account.display_name}};
}

SipAccountConfig account_from_json(const json& document) {
  SipAccountConfig account;
  account.id = json_get<std::string>(document, "id", account.id);
  account.registrar = json_get<std::string>(document, "registrar", account.registrar);
  account.username = json_get<std::string>(document, "username", account.username);
  account.password = json_get<std::string>(document, "password", "");
  account.auth_realm = json_get<std::string>(document, "auth_realm", "");
  account.outbound_proxy = json_get<std::string>(document, "outbound_proxy", "");
  account.display_name =
      json_get<std::string>(document, "display_name", account.display_name);
  return account;
}

json line_to_json(const LineConfig& line) {
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
  return json{{"id", line.id},
              {"name", line.name},
              {"enabled", line.enabled},
              {"gain_db", line.gain_db},
              {"rx_gain_db", line.rx_gain_db},
              {"tx_gain_db", line.tx_gain_db},
              {"mute", line.mute},
              {"aes67", aes67},
              {"sip", sip}};
}

/** Fills in the defaults that keep hand written config files short. */
void apply_line_defaults(LineConfig* line) {
  if (line->name.empty()) {
    line->name = "Line " + std::to_string(line->id);
  }
  if (line->sip.extension.empty()) {
    line->sip.extension = "line" + std::to_string(line->id);
  }
  if (line->sip.display_name.empty()) {
    line->sip.display_name = line->name;
  }
  if (line->aes67.stream_name.empty()) {
    line->aes67.stream_name = line->name;
  }
  if (line->aes67.channels.empty()) {
    line->aes67.channels = {0, 1};
  }
}

LineConfig line_from_json(const json& document) {
  LineConfig line;
  line.id = json_get<int>(document, "id", line.id);
  line.name = json_get<std::string>(document, "name", line.name);
  line.enabled = json_get<bool>(document, "enabled", line.enabled);
  line.gain_db = json_get<double>(document, "gain_db", line.gain_db);
  line.rx_gain_db = json_get<double>(document, "rx_gain_db", line.rx_gain_db);
  line.tx_gain_db = json_get<double>(document, "tx_gain_db", line.tx_gain_db);
  line.mute = json_get<bool>(document, "mute", line.mute);

  if (json_has(document, "aes67")) {
    const auto& a = document.at("aes67");
    line.aes67.sink_id = json_get<int>(a, "sink_id", line.aes67.sink_id);
    line.aes67.source_id = json_get<int>(a, "source_id", line.aes67.source_id);
    line.aes67.channels =
        json_get<std::vector<unsigned>>(a, "channels", line.aes67.channels);
    line.aes67.stream_name = json_get<std::string>(a, "stream_name", "");
    line.aes67.auto_create_streams =
        json_get<bool>(a, "auto_create_streams", line.aes67.auto_create_streams);
    line.aes67.remote_source_id =
        json_get<std::string>(a, "remote_source_id", "");
    line.aes67.remote_sdp = json_get<std::string>(a, "remote_sdp", "");
    line.aes67.ignore_refclk_gmid =
        json_get<bool>(a, "ignore_refclk_gmid", line.aes67.ignore_refclk_gmid);
  }
  if (json_has(document, "sip")) {
    const auto& s = document.at("sip");
    line.sip.account = json_get<std::string>(s, "account", line.sip.account);
    line.sip.extension = json_get<std::string>(s, "extension", "");
    line.sip.display_name = json_get<std::string>(s, "display_name", "");
    line.sip.call_mode = json_get<std::string>(s, "call_mode", line.sip.call_mode);
    line.sip.dial_target = json_get<std::string>(s, "dial_target", "");
    line.sip.auto_answer_code =
        json_get<int>(s, "auto_answer_code", line.sip.auto_answer_code);
    line.sip.ptt_threshold_dbfs =
        json_get<double>(s, "ptt_threshold_dbfs", line.sip.ptt_threshold_dbfs);
    line.sip.ptt_hangup_ms =
        json_get<int>(s, "ptt_hangup_ms", line.sip.ptt_hangup_ms);
  }

  apply_line_defaults(&line);
  return line;
}

/** Recursive object merge used by POST /api/config. */
void merge_object(json& target, const json& patch) {
  if (!target.is_object() || !patch.is_object()) {
    target = patch;
    return;
  }
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    if (it.value().is_object() && target.contains(it.key()) &&
        target.at(it.key()).is_object()) {
      merge_object(target[it.key()], it.value());
    } else {
      target[it.key()] = it.value();
    }
  }
}

}  // namespace


// ---------------------------------------------------------------------------
// enums
// ---------------------------------------------------------------------------

AudioBackendKind AudioConfig::backend_kind() const {
  const std::string value = to_lower(backend);
  if (value == "null" || value == "none" || value == "fake") {
    return AudioBackendKind::kNull;
  }
  return AudioBackendKind::kRavenna;
}

PcmFormat AudioConfig::pcm_format() const {
  const std::string value = to_lower(format);
  if (value == "s24_3le" || value == "s24le" || value == "s24") {
    return PcmFormat::kS24_3Le;
  }
  if (value == "s32_le" || value == "s32le" || value == "s32") {
    return PcmFormat::kS32Le;
  }
  return PcmFormat::kS16Le;
}

// ---------------------------------------------------------------------------
// Config <- JSON
// ---------------------------------------------------------------------------

LineConfig Config::parse_line(const json& document) {
  return line_from_json(document);
}

Config Config::from_json(const json& document) {
  Config config;
  config.log_severity = json_get<int>(document, "log_severity", config.log_severity);
  config.http_addr = json_get<std::string>(document, "http_addr", config.http_addr);
  config.http_port = json_get<int>(document, "http_port", config.http_port);
  config.webui_dir = json_get<std::string>(document, "webui_dir", config.webui_dir);
  config.webui_api_auth =
      json_get<bool>(document, "webui_api_auth", config.webui_api_auth);

  if (json_has(document, "audio")) {
    const auto& a = document.at("audio");
    config.audio.backend = json_get<std::string>(a, "backend", config.audio.backend);
    config.audio.device = json_get<std::string>(a, "device", config.audio.device);
    config.audio.sample_rate =
        json_get<unsigned>(a, "sample_rate", config.audio.sample_rate);
    config.audio.channels = json_get<unsigned>(a, "channels", config.audio.channels);
    config.audio.period_frames =
        json_get<unsigned>(a, "period_frames", config.audio.period_frames);
    config.audio.periods = json_get<unsigned>(a, "periods", config.audio.periods);
    config.audio.format = json_get<std::string>(a, "format", config.audio.format);
    config.audio.null_tone_hz =
        json_get<double>(a, "null_tone_hz", config.audio.null_tone_hz);
  }

  if (json_has(document, "aes67_daemon")) {
    const auto& d = document.at("aes67_daemon");
    config.aes67_daemon.address =
        json_get<std::string>(d, "address", config.aes67_daemon.address);
    config.aes67_daemon.port = json_get<int>(d, "port", config.aes67_daemon.port);
    config.aes67_daemon.fake = json_get<bool>(d, "fake", config.aes67_daemon.fake);
    config.aes67_daemon.auto_configure =
        json_get<bool>(d, "auto_configure", config.aes67_daemon.auto_configure);
    config.aes67_daemon.sink_delay_samples = json_get<unsigned>(
        d, "sink_delay_samples", config.aes67_daemon.sink_delay_samples);
    config.aes67_daemon.source_payload_type = json_get<int>(
        d, "source_payload_type", config.aes67_daemon.source_payload_type);
    config.aes67_daemon.source_ttl =
        json_get<int>(d, "source_ttl", config.aes67_daemon.source_ttl);
    config.aes67_daemon.source_dscp =
        json_get<int>(d, "source_dscp", config.aes67_daemon.source_dscp);
    config.aes67_daemon.rtp_mcast_base = json_get<std::string>(
        d, "rtp_mcast_base", config.aes67_daemon.rtp_mcast_base);
  }

  if (json_has(document, "sip")) {
    const auto& s = document.at("sip");
    config.sip.enabled = json_get<bool>(s, "enabled", config.sip.enabled);
    config.sip.engine = json_get<std::string>(s, "engine", config.sip.engine);
    config.sip.transport = json_get<std::string>(s, "transport", config.sip.transport);
    config.sip.local_port = json_get<int>(s, "local_port", config.sip.local_port);
    config.sip.codecs =
        json_get<std::vector<std::string>>(s, "codecs", config.sip.codecs);
    config.sip.ptime_ms = json_get<int>(s, "ptime_ms", config.sip.ptime_ms);
    config.sip.registration_timeout = json_get<int>(
        s, "registration_timeout", config.sip.registration_timeout);
    config.sip.retry_interval =
        json_get<int>(s, "retry_interval", config.sip.retry_interval);
    config.sip.keep_alive_interval =
        json_get<int>(s, "keep_alive_interval", config.sip.keep_alive_interval);
    config.sip.stun_server = json_get<std::string>(s, "stun_server", "");
    config.sip.srtp = json_get<std::string>(s, "srtp", config.sip.srtp);
  }

  if (json_has(document, "accounts")) {
    config.accounts.clear();
    for (const auto& entry : document.at("accounts")) {
      config.accounts.push_back(account_from_json(entry));
    }
  }

  if (json_has(document, "lines")) {
    config.lines.clear();
    for (const auto& entry : document.at("lines")) {
      config.lines.push_back(line_from_json(entry));
    }
  }

  // keep a usable configuration even when the file only overrides a few keys
  if (config.accounts.empty()) {
    config.accounts.push_back(SipAccountConfig{});
  }
  if (config.lines.empty()) {
    config.lines.push_back(line_from_json(json::object()));
  }
  if (config.sip.codecs.empty()) {
    config.sip.codecs = {"PCMU/8000/1"};
  }
  return config;
}

// ---------------------------------------------------------------------------
// Config -> JSON
// ---------------------------------------------------------------------------

json Config::to_json() const {
  json audio_json{{"backend", this->audio.backend},
                  {"device", this->audio.device},
                  {"sample_rate", this->audio.sample_rate},
                  {"channels", this->audio.channels},
                  {"period_frames", this->audio.period_frames},
                  {"periods", this->audio.periods},
                  {"format", this->audio.format},
                  {"null_tone_hz", this->audio.null_tone_hz}};

  json daemon_json{{"address", aes67_daemon.address},
                   {"port", aes67_daemon.port},
                   {"fake", aes67_daemon.fake},
                   {"auto_configure", aes67_daemon.auto_configure},
                   {"sink_delay_samples", aes67_daemon.sink_delay_samples},
                   {"source_payload_type", aes67_daemon.source_payload_type},
                   {"source_ttl", aes67_daemon.source_ttl},
                   {"source_dscp", aes67_daemon.source_dscp},
                   {"rtp_mcast_base", aes67_daemon.rtp_mcast_base}};

  json sip_json{{"enabled", this->sip.enabled},
                {"engine", this->sip.engine},
                {"transport", this->sip.transport},
                {"local_port", this->sip.local_port},
                {"codecs", this->sip.codecs},
                {"ptime_ms", this->sip.ptime_ms},
                {"registration_timeout", this->sip.registration_timeout},
                {"retry_interval", this->sip.retry_interval},
                {"keep_alive_interval", this->sip.keep_alive_interval},
                {"stun_server", this->sip.stun_server},
                {"srtp", this->sip.srtp}};

  json accounts_json = json::array();
  for (const auto& account : accounts) {
    accounts_json.push_back(account_to_json(account));
  }

  json lines_json = json::array();
  for (const auto& line : lines) {
    lines_json.push_back(line_to_json(line));
  }

  return json{{"log_severity", log_severity},
              {"http_addr", http_addr},
              {"http_port", http_port},
              {"webui_dir", webui_dir},
              {"webui_api_auth", webui_api_auth},
              {"audio", audio_json},
              {"aes67_daemon", daemon_json},
              {"sip", sip_json},
              {"accounts", accounts_json},
              {"lines", lines_json}};
}

// ---------------------------------------------------------------------------
// partial update / persistence
// ---------------------------------------------------------------------------

void Config::merge(const json& patch) {
  json document = to_json();
  merge_object(document, patch);
  Config merged = Config::from_json(document);
  // `from_json` fills in defaults for empty collections; merging must preserve
  // an intentionally empty list, so copy arrays back when the patch cleared them.
  if (json_has(patch, "accounts") && patch.at("accounts").empty()) {
    merged.accounts = accounts;
  }
  *this = merged;
}

bool Config::load(const std::string& path, Config* config, std::string* error) {
  std::ifstream input(path);
  if (!input.is_open()) {
    if (error) {
      *error = "cannot open config file '" + path + "'";
    }
    return false;
  }
  try {
    json document;
    input >> document;
    *config = Config::from_json(document);
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("cannot parse config file '") + path + "': " + ex.what();
    }
    return false;
  }
  return true;
}

bool Config::save(const std::string& path, std::string* error) const {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output.is_open()) {
      if (error) {
        *error = "cannot write config file '" + temporary + "'";
      }
      return false;
    }
    output << to_json().dump(2) << std::endl;
    if (!output.good()) {
      if (error) {
        *error = "error while writing '" + temporary + "'";
      }
      return false;
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    if (error) {
      *error = "cannot replace config file '" + path + "'";
    }
    return false;
  }
  return true;
}

const SipAccountConfig* Config::find_account(const std::string& id) const {
  for (const auto& account : accounts) {
    if (account.id == id) {
      return &account;
    }
  }
  return nullptr;
}

const LineConfig* Config::find_line(int id) const {
  const int index = line_index(id);
  return index < 0 ? nullptr : &lines[static_cast<size_t>(index)];
}

int Config::line_index(int id) const {
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace aes67sip

