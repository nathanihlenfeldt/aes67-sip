#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <httplib.h>

#include "audio/backend.hpp"
#include "audio/router.hpp"
#include "bridge/line_manager.hpp"
#include "config.hpp"
#include "http/api_server.hpp"
#include "sip/engine.hpp"
#include "test_framework.hpp"

using namespace aes67sip;

namespace {

constexpr int kTestPort = 18099;
const std::string kTestConfigPath = "aes67-sip-gateway-test.conf";

Config test_config() {
  Config config = Config::from_json(json::object());
  config.http_addr = "127.0.0.1";
  config.http_port = kTestPort;
  config.webui_dir = "";  // no static files during the tests
  config.audio.backend = "null";
  config.audio.null_tone_hz = 1000.0;
  config.audio.channels = 4;
  config.audio.period_frames = 48;
  config.aes67_daemon.fake = true;
  config.sip.engine = "stub";
  config.accounts[0].id = "pbx";
  config.accounts[0].registrar = "sip:pbx.example.com";
  config.lines[0].id = 0;
  config.lines[0].name = "Stage Left";
  config.lines[0].aes67.channels = {0, 1};
  config.lines[0].sip.extension = "1001";
  config.lines[0].sip.call_mode = "manual";
  config.lines[0].sip.dial_target = "sip:1001@pbx.example.com";
  return config;
}

/** Brings up the whole gateway with the simulated AES67 daemon and stub SIP. */
struct Gateway {
  Config config{test_config()};
  std::unique_ptr<DaemonClient> daemon;
  std::unique_ptr<AudioBackend> backend;
  std::unique_ptr<AudioRouter> router;
  std::unique_ptr<IntercomMatrix> matrix;
  std::unique_ptr<LineManager> lines;
  std::unique_ptr<SipEngine> engine;
  std::unique_ptr<ApiServer> api;
  std::string error;

  /**
   * `tweak` runs before anything is built, which is how a test declares endpoints
   * and party lines.  A null tweak leaves the default single-line gateway.
   */
  explicit Gateway(bool with_sip = true,
                   const std::function<void(Config&)>& tweak = {}) {
    if (tweak) {
      tweak(config);
    }
    if (with_sip) {
      config.sip.enabled = true;
    }
    daemon = DaemonClient::create(config.aes67_daemon);
    backend = create_audio_backend(config.audio);
    AudioFormat format;
    format.sample_rate = config.audio.sample_rate;
    format.channels = config.audio.channels;
    format.period_frames = config.audio.period_frames;
    router = std::make_unique<AudioRouter>(backend.get(), format);
    matrix = std::make_unique<IntercomMatrix>(config.audio.sample_rate);
    router->attach_matrix(matrix.get());
    lines = std::make_unique<LineManager>(&config, kTestConfigPath, daemon.get(),
                                          router.get(), nullptr);
    lines->attach_matrix(matrix.get());
    engine = SipEngine::create(config.sip, lines.get(), lines.get(), &error);
    lines->attach_engine(engine.get());
    api = std::make_unique<ApiServer>(&config, kTestConfigPath, daemon.get(),
                                      router.get(), lines.get(), engine.get());
    api->attach_matrix(matrix.get());
  }

  bool start() {
    if (!router->start(&error)) {
      return false;
    }
    if (engine != nullptr && !engine->start(&error)) {
      return false;
    }
    if (!lines->apply_configuration(&error)) {
      return false;
    }
    lines->start_supervision();
    return api->start(&error);
  }

  void stop() {
    if (api != nullptr) {
      api->stop();
    }
    lines->stop();
    if (engine != nullptr) {
      engine->stop();
    }
    router->stop();
  }

  /** Polls /api/status until the predicate is true or the timeout expires. */
  bool wait_for(const std::function<bool(const json&)>& predicate,
                int timeout_ms = 6000) {
    httplib::Client client("127.0.0.1", kTestPort);
    const int64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
      const auto response = client.Get("/api/status");
      if (response && response->status == 200) {
        const json status = json::parse(response->body);
        if (predicate(status)) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }
};

json get_json(httplib::Client& client, const std::string& path,
              int* status = nullptr) {
  const auto response = client.Get(path);
  if (status != nullptr) {
    *status = response ? response->status : 0;
  }
  if (!response || response->status != 200) {
    return nullptr;
  }
  try {
    return json::parse(response->body);
  } catch (const json::exception&) {
    return nullptr;
  }
}

}  // namespace

namespace {

/**
 * The reference deployment's shape: two single-pair endpoints sharing one party
 * line, each declaring its own channels rather than being assumed.
 */
Config party_line_config() {
  Config config = test_config();
  EndpointConfig first;
  first.id = "a";
  first.name = "Camera 1";
  first.talk_channels = {0};
  first.listen_channels = {1};
  EndpointConfig second;
  second.id = "b";
  second.name = "Camera 2";
  second.talk_channels = {1};
  second.listen_channels = {0};
  config.endpoints = {first, second};

  PartyLineConfig line;
  line.id = "cameras";
  line.name = "Cameras";
  PartyLineMemberConfig first_member;
  first_member.endpoint = "a";
  first_member.talk_channel = 0;
  first_member.listen_channel = 0;
  first_member.contribution_db = -3.0;
  PartyLineMemberConfig second_member;
  second_member.endpoint = "b";
  second_member.talk_channel = 0;
  second_member.listen_channel = 0;
  line.members = {first_member, second_member};
  config.party_lines = {line};
  return config;
}

/** The status entry for one member of one party line. */
json member_status(const json& status, size_t line_index, size_t member_index) {
  return status.at("party_lines")[line_index].at("members")[member_index];
}

}  // namespace

TEST_CASE(rest_api_reports_party_lines_and_their_members) {
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  CHECK(gateway.start());

  // The mixes are fed from the audio thread, and the simulated device carries a
  // tone on every channel, so both members must report as arriving.  This walks
  // the whole path: configuration -> matrix -> audio thread -> status contract.
  const bool reported = gateway.wait_for([](const json& status) {
    if (!status.contains("party_lines") || status.at("party_lines").empty()) {
      return false;
    }
    const json& members = status.at("party_lines")[0].at("members");
    return members.size() == 2 && members[0].at("arriving").get<bool>() &&
           members[1].at("arriving").get<bool>();
  });
  CHECK(reported);

  httplib::Client client("127.0.0.1", kTestPort);
  const json status = get_json(client, "/api/status");
  const json& line = status.at("party_lines")[0];
  CHECK_EQ(line.at("id").get<std::string>(), std::string("cameras"));
  CHECK_EQ(line.at("name").get<std::string>(), std::string("Cameras"));
  CHECK_EQ(line.at("members").size(), 2U);

  const json first = member_status(status, 0, 0);
  CHECK_EQ(first.at("endpoint").get<std::string>(), std::string("a"));
  CHECK_EQ(first.at("name").get<std::string>(), std::string("Camera 1"));
  // The contribution level travels configuration -> plan -> status untouched.
  CHECK_NEAR(first.at("contribution_db").get<double>(), -3.0, 1e-9);
  CHECK(first.at("level_dbfs").get<double>() > -60.0);

  gateway.stop();
}

TEST_CASE(rest_api_reports_no_party_lines_when_none_are_configured) {
  Gateway gateway;
  CHECK(gateway.start());

  httplib::Client client("127.0.0.1", kTestPort);
  const json status = get_json(client, "/api/status");
  // Additive: a site with no endpoints and no party lines still gets the key.
  CHECK(status.contains("party_lines"));
  CHECK_EQ(status.at("party_lines").size(), 0U);

  gateway.stop();
}

TEST_CASE(line_manager_reaches_configured_state) {
  Gateway gateway;
  std::string error;
  CHECK(gateway.router->start(&error));
  CHECK(gateway.engine->start(&error));
  CHECK(gateway.lines->apply_configuration(&error));
  CHECK_EQ(gateway.lines->line_count(), 1);

  const json status = gateway.lines->line_status(0);
  CHECK(!status.is_null());
  CHECK_EQ(status.at("id").get<int>(), 0);
  CHECK_EQ(status.at("name").get<std::string>(), std::string("Stage Left"));
  CHECK_EQ(status.at("state").get<std::string>(), std::string("idle"));
  CHECK_EQ(status.at("aes67").at("channels"), json({0, 1}));
  CHECK(status.at("levels").contains("rx_dbfs"));
  CHECK(status.at("levels").contains("sip_tx_dbfs"));
  CHECK(status.at("call").contains("remote_uri"));

  CHECK(gateway.lines->line_status(99).is_null());
  gateway.stop();
}

TEST_CASE(line_manager_dials_and_hangs_up_through_the_stub_engine) {
  Gateway gateway;
  std::string error;
  CHECK(gateway.router->start(&error));
  CHECK(gateway.engine->start(&error));
  CHECK(gateway.lines->apply_configuration(&error));

  CHECK(gateway.lines->call_action(0, "dial", json::object(), &error));

  // the stub engine rings for a while and then answers
  const int64_t deadline = now_ms() + 4000;
  bool in_call = false;
  while (now_ms() < deadline && !in_call) {
    in_call = gateway.lines->line_status(0).at("state") == "in_call";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(in_call);

  const json status = gateway.lines->line_status(0);
  CHECK(status.at("call").at("state").get<std::string>() == "in_call");
  CHECK_EQ(status.at("state_code").get<int>(), 200);
  CHECK(status.at("ptt").is_boolean());

  CHECK(gateway.lines->call_action(0, "dtmf", json{{"digits", "123"}}, &error));
  CHECK(gateway.lines->call_action(0, "hold", json{{"hold", true}}, &error));
  CHECK(gateway.lines->call_action(0, "hangup", json::object(), &error));
  CHECK(gateway.lines->line_status(0).at("state") == "idle");

  CHECK(!gateway.lines->call_action(0, "nonsense", json::object(), &error));
  CHECK(!error.empty());
  gateway.stop();
}

TEST_CASE(rest_api_serves_status_and_controls_lines) {
  Gateway gateway;
  std::string error;
  CHECK(gateway.start());

  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const json version = get_json(client, "/api/version");
  CHECK(!version.is_null());
  CHECK_EQ(version.at("name").get<std::string>(), std::string("aes67-sip"));

  const json status = get_json(client, "/api/status");
  CHECK(!status.is_null());
  CHECK_EQ(status.at("audio").at("backend").get<std::string>(),
           std::string("null"));
  CHECK_EQ(status.at("audio").at("channels").get<int>(), 4);
  CHECK(status.at("audio").at("state") == "running");
  CHECK(status.at("aes67").at("ptp").at("status") == "locked");
  CHECK(status.at("sip").at("engine") == "stub");
  CHECK_EQ(status.at("lines").size(), 1U);
  CHECK_EQ(status.at("lines")[0].at("name").get<std::string>(),
           std::string("Stage Left"));

  // the simulated daemon must have been provisioned from the line config; the
  // sink is deliberately NOT created (no endpoint SDP and commissioning_loopback
  // is off) so that a sink wired up elsewhere is never overwritten
  const json sinks = get_json(client, "/api/aes67/sinks");
  const json sources = get_json(client, "/api/aes67/sources");
  CHECK_EQ(sources.at("sources").size(), 1U);
  CHECK_EQ(sinks.at("sinks").size(), 0U);
  CHECK_EQ(status.at("lines")[0].at("aes67").at("sdp_source").get<std::string>(),
           std::string("unmanaged"));

  // ... and the commissioning loopback is opt-in
  CHECK(client.Post("/api/lines/0/config",
                    json{{"aes67", {{"commissioning_loopback", true}}}}.dump(),
                    "application/json"));
  const json looped = get_json(client, "/api/aes67/sinks");
  CHECK_EQ(looped.at("sinks").size(), 1U);
  CHECK(gateway.wait_for([](const json& document) {
    return document.at("lines")[0].at("aes67").at("sdp_source") == "loopback";
  }));

  const json startup_config = get_json(client, "/api/lines/0/config");
  CHECK_EQ(startup_config.at("sip").at("extension").get<std::string>(),
           std::string("1001"));

  // start a call through the API and check it reaches in_call
  const auto dial = client.Post(
      "/api/lines/0/call", json{{"action", "dial"}}.dump(), "application/json");
  CHECK(dial && dial->status == 200);
  CHECK(gateway.wait_for([](const json& document) {
    return document.at("lines")[0].at("state") == "in_call";
  }));

  const json levels = get_json(client, "/api/lines/0/levels");
  CHECK(!levels.is_null());
  CHECK(levels.contains("sip_tx_dbfs"));

  // mute + gain update goes through the config endpoint
  const auto updated = client.Post("/api/lines/0/config",
                                   json{{"mute", true},
                                        {"gain_db", -6.0},
                                        {"sip", {{"call_mode", "auto_answer"}}}}
                                       .dump(),
                                   "application/json");
  CHECK(updated && updated->status == 200);
  const json line = json::parse(updated->body);
  CHECK(line.at("mute").get<bool>());
  CHECK_NEAR(line.at("gain_db").get<double>(), -6.0, 1e-6);
  CHECK_EQ(line.at("call").at("state").get<std::string>(), std::string("in_call"));

  // the change must be persisted
  Config reloaded;
  CHECK(Config::load(kTestConfigPath, &reloaded, &error));
  CHECK(reloaded.lines[0].mute);
  CHECK_EQ(reloaded.lines[0].sip.call_mode, std::string("auto_answer"));

  const auto hangup = client.Post(
      "/api/lines/0/call", json{{"action", "hangup"}}.dump(), "application/json");
  CHECK(hangup && hangup->status == 200);

  const auto bogus = client.Post(
      "/api/lines/0/call", json{{"action", "sing"}}.dump(), "application/json");
  CHECK(bogus && bogus->status == 400);

  int missing_status = 0;
  CHECK(get_json(client, "/api/lines/7", &missing_status).is_null());
  CHECK_EQ(missing_status, 404);

  const json log = get_json(client, "/api/log?lines=50");
  CHECK(!log.is_null());
  CHECK(log.at("lines").is_array());

  gateway.stop();
  std::remove(kTestConfigPath.c_str());
}

TEST_CASE(rest_api_self_test_reports_checks) {
  Gateway gateway;
  CHECK(gateway.start());
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const auto response =
      client.Post("/api/system/self-test", "", "application/json");
  CHECK(response && response->status == 200);
  const json result = json::parse(response->body);
  CHECK(result.contains("ok"));
  CHECK(result.at("checks").is_array());
  CHECK(result.at("checks").size() >= 4);

  bool saw_backend = false;
  for (const auto& check : result.at("checks")) {
    CHECK(check.contains("name"));
    CHECK(check.contains("detail"));
    if (check.at("name") == "audio backend") {
      saw_backend = true;
      CHECK(check.at("ok").get<bool>());
    }
  }
  CHECK(saw_backend);

  gateway.stop();
}
