#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

#include <httplib.h>

#include "audio/backend.hpp"
#include "audio/null_backend.hpp"
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
  std::string config_path_;

  /**
   * `tweak` runs before anything is built, which is how a test declares endpoints
   * and party lines.  A null tweak leaves the default single-line gateway.
   * `config_path` is where a save is attempted: a path that cannot be written is
   * how the "refused rather than appearing to succeed" case is set up.
   */
  explicit Gateway(bool with_sip = true,
                   const std::function<void(Config&)>& tweak = {},
                   std::string config_path = kTestConfigPath)
      : config_path_(std::move(config_path)) {
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
    lines = std::make_unique<LineManager>(&config, config_path_, daemon.get(),
                                          router.get(), nullptr);
    lines->attach_matrix(matrix.get());
    engine = SipEngine::create(config.sip, lines.get(), lines.get(), &error);
    lines->attach_engine(engine.get());
    api = std::make_unique<ApiServer>(&config, config_path_, daemon.get(),
                                      router.get(), lines.get(), engine.get());
    api->attach_matrix(matrix.get());
  }

  /**
   * The ports live gateways hold.  Every gateway in this suite binds the same test
   * port, so two of them at once means an HTTP request is answered by whichever OS
   * happens to win the connection race - macOS lets the second bind succeed, Linux
   * does not - which turned a test mistake into a platform-dependent result.
   * Refusing the second start makes that mistake fail on both.
   */
  static std::set<int>& live_ports() {
    static std::set<int> ports;
    return ports;
  }

  bool start() {
    // A test must not inherit the configuration file a previous run left behind:
    // `restart_from_file` reads this path, and a run killed mid-save leaves a
    // partial document that turns the next run's assertions into nonsense.
    std::remove(config_path_.c_str());
    if (!live_ports().insert(config.http_port).second) {
      error =
          "port " + std::to_string(config.http_port) +
          " is already held by another gateway: stop it before starting this one";
      return false;
    }
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

  /**
   * The appliance's restart handler, as `App::restart` behaves: re-read the file
   * and apply *that*, so a test can assert that what was saved is what runs.
   */
  bool restart_from_file(std::string* restart_error) {
    Config reloaded;
    std::string load_error;
    if (Config::load(config_path_, &reloaded, &load_error)) {
      config = reloaded;
    }
    return lines->apply_configuration(restart_error);
  }

  /**
   * The stub engine, for its documented test hooks.  Null when the test built a
   * real engine (the suite always runs the stub).
   */
  StubSipEngine* stub() { return dynamic_cast<StubSipEngine*>(engine.get()); }

  void stop() {
    live_ports().erase(config.http_port);
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

/**
 * Starts the gateway, or ends the test where it stands.
 *
 * A gateway that cannot come up - most often because another process holds the
 * test port - leaves every later assertion meaningless: the requests are answered
 * by whoever holds the port, or by nobody, so the failures that follow name the
 * wrong cause.  Throwing turns that into one failure carrying the reason (the
 * framework reports an unexpected exception), instead of a cascade nobody can read.
 */
void gateway_up(Gateway& gateway) {
  if (gateway.start()) {
    return;
  }
  throw std::runtime_error("the gateway did not start: " + gateway.error);
}

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
  gateway_up(gateway);

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
  // This fixture claims no conference, so the field is reported and stays false: a
  // gateway serving only SIP lines behaves exactly as it always has.
  CHECK_EQ(line.at("claims_conference").get<bool>(), false);

  const json first = member_status(status, 0, 0);
  CHECK_EQ(first.at("endpoint").get<std::string>(), std::string("a"));
  CHECK_EQ(first.at("name").get<std::string>(), std::string("Camera 1"));
  // The bindings travel too, so the UI can show which channel feeds which line.
  CHECK_EQ(first.at("talk_channel").get<int>(), 0);
  CHECK_EQ(first.at("listen_channel").get<int>(), 0);
  // The contribution level travels configuration -> plan -> status untouched.
  CHECK_NEAR(first.at("contribution_db").get<double>(), -3.0, 1e-9);
  CHECK(first.at("level_dbfs").get<double>() > -60.0);

  gateway.stop();
}

TEST_CASE(rest_api_reports_no_party_lines_when_none_are_configured) {
  Gateway gateway;
  gateway_up(gateway);

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
  gateway_up(gateway);

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

// ---------------------------------------------------------------------------
// scale: per-endpoint provisioning at 32 channels
// ---------------------------------------------------------------------------

namespace {

/**
 * The reference site's configuration: sixteen endpoints of two talk and two
 * listen channels each (32 channels per direction), with each of an endpoint's
 * two pairs on a different one of eight party lines, so all 32 channels are live
 * in both directions.  `device_channels` is what the appliance is told to open,
 * so a test can ask for a device too narrow for the declared shapes.
 *
 * Each endpoint's talk stream is described here the way discovery would supply
 * it on site, because a talk stream cannot be subscribed to without one.
 */
void scale_matrix(Config* config, unsigned device_channels = 32) {
  config->audio.channels = device_channels;
  for (unsigned i = 0; i < 16; ++i) {
    EndpointConfig endpoint;
    endpoint.id = "pack-" + std::to_string(i + 1);
    endpoint.name = "Beltpack " + std::to_string(i + 1);
    endpoint.talk_channels = {2 * i, 2 * i + 1};
    endpoint.listen_channels = {2 * i, 2 * i + 1};
    endpoint.aes67.remote_sdp =
        "v=0\r\ns=Beltpack\r\nm=audio 5004 RTP/AVP 98\r\n"
        "a=rtpmap:98 L24/48000/2\r\n";
    config->endpoints.push_back(endpoint);
  }
  for (unsigned index = 0; index < 8; ++index) {
    PartyLineConfig line;
    line.id = "pl" + std::to_string(index + 1);
    line.name = "PL " + std::to_string(index + 1);
    config->party_lines.push_back(line);
  }
  for (unsigned i = 0; i < 16; ++i) {
    for (unsigned pair = 0; pair < 2; ++pair) {
      PartyLineMemberConfig member;
      member.endpoint = "pack-" + std::to_string(i + 1);
      member.talk_channel = static_cast<int>(pair);
      member.listen_channel = static_cast<int>(pair);
      // A level per line, low enough that four summed contributions stay inside
      // the device's range and high enough to stay well above the meter floor.
      member.contribution_db = -3.0 * static_cast<double>((i + pair) % 8);
      config->party_lines[(i + pair) % 8].members.push_back(member);
    }
  }
}

/** The streams of `document` ("sinks" / "sources") whose name matches `suffix`. */
std::vector<json> endpoint_streams(const json& document,
                                   const std::string& suffix) {
  std::vector<json> result;
  for (const auto& stream :
       document.at(document.contains("sinks") ? "sinks" : "sources")) {
    const std::string name = json_get<std::string>(stream, "name", "");
    if (name.find("Beltpack ") != std::string::npos &&
        name.find(suffix) != std::string::npos) {
      result.push_back(stream);
    }
  }
  return result;
}

/** The one stream called `name`, or null.  A null result fails the checks below. */
json stream_named(const std::vector<json>& streams, const std::string& name) {
  for (const auto& stream : streams) {
    if (json_get<std::string>(stream, "name", "") == name) {
      return stream;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE(rest_api_provisions_one_stream_per_endpoint_direction) {
  Gateway gateway(true, [](Config& config) { scale_matrix(&config); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // One stream per direction per endpoint, carrying *all* of that direction's
  // channels: 16 talk streams and 16 listen streams, not 32 of each.
  const json sinks = get_json(client, "/api/aes67/sinks");
  const json sources = get_json(client, "/api/aes67/sources");
  CHECK(!sinks.is_null());
  CHECK(!sources.is_null());
  const std::vector<json> talk = endpoint_streams(sinks, "(talk)");
  const std::vector<json> listen = endpoint_streams(sources, "(listen)");
  CHECK_EQ(talk.size(), 16U);
  CHECK_EQ(listen.size(), 16U);
  for (const auto& stream : talk) {
    CHECK_EQ(stream.at("map").size(), 2U);
  }
  for (const auto& stream : listen) {
    CHECK_EQ(stream.at("map").size(), 2U);
  }

  // The names carry the endpoint, so a human matches a stream to an endpoint in
  // a vendor's routing grid without a lookup table.
  const json first_talk = stream_named(talk, "Beltpack 1 (talk)");
  const json first_listen = stream_named(listen, "Beltpack 1 (listen)");
  CHECK(!first_talk.is_null());
  CHECK(!first_listen.is_null());
  CHECK_EQ(first_talk.at("map"), json({0, 1}));
  CHECK_EQ(first_listen.at("map"), json({0, 1}));
  const json last_talk = stream_named(talk, "Beltpack 16 (talk)");
  CHECK(!last_talk.is_null());
  CHECK_EQ(last_talk.at("map"), json({30, 31}));

  // The configuration file is the source of truth: endpoints configured away
  // take their streams with them, so the daemon does not keep sending a mix that
  // nothing routes any more.
  gateway.config.endpoints.clear();
  gateway.config.party_lines.clear();
  std::string error;
  CHECK(gateway.lines->apply_configuration(&error));
  const json sources_after = get_json(client, "/api/aes67/sources");
  const json sinks_after = get_json(client, "/api/aes67/sinks");
  CHECK_EQ(endpoint_streams(sources_after, "(listen)").size(), 0U);
  CHECK_EQ(endpoint_streams(sinks_after, "(talk)").size(), 0U);
  CHECK_EQ(sources_after.at("sources").size(), 1U);  // only the SIP line's own
  CHECK_EQ(sinks_after.at("sinks").size(), 0U);

  gateway.stop();
}

TEST_CASE(gateway_refuses_a_configuration_wider_than_the_device) {
  // The declared shapes need 32 channels per direction and the device is opened
  // at 16: the configuration is refused, and refused as a whole.
  Gateway gateway(true, [](Config& config) { scale_matrix(&config, 16); });

  std::string error;
  CHECK(!gateway.lines->apply_configuration(&error));
  // both totals, in the roles they play
  CHECK(error.find("32 device channels") != std::string::npos);       // declared
  CHECK(error.find("audio.channels opens 16") != std::string::npos);  // available

  // Nothing was provisioned and nothing was routed: no half-wired site.
  json sources;
  std::string daemon_error;
  CHECK(gateway.daemon->get_sources(&sources, &daemon_error));
  CHECK_EQ(sources.at("sources").size(), 0U);
  json sinks;
  CHECK(gateway.daemon->get_sinks(&sinks, &daemon_error));
  CHECK_EQ(sinks.at("sinks").size(), 0U);
  CHECK(gateway.matrix->empty());

  // The same configuration is accepted once the device is wide enough: what was
  // refused is the fit, not the configuration.
  gateway.config.audio.channels = 32;
  CHECK(gateway.lines->apply_configuration(&error));
  CHECK(gateway.daemon->get_sources(&sources, &daemon_error));
  CHECK_EQ(sources.at("sources").size(), 17U);  // 16 endpoints + the SIP line
}

TEST_CASE(gateway_mixes_the_reference_scale_in_a_single_run) {
  Gateway gateway(true, [](Config& config) { scale_matrix(&config); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // Eight party lines of four members each, every member's audio arriving, in
  // one run: the whole path - configuration -> per-endpoint streams -> matrix ->
  // audio thread -> status contract.
  const bool mixed = gateway.wait_for(
      [](const json& status) {
        const auto& lines = status.at("party_lines");
        if (lines.size() != 8) {
          return false;
        }
        for (const auto& line : lines) {
          if (line.at("members").size() != 4) {
            return false;
          }
          for (const auto& member : line.at("members")) {
            if (!member.at("arriving").get<bool>()) {
              return false;
            }
          }
        }
        return true;
      },
      10000);
  CHECK(mixed);

  // and the other direction of the same claim: every endpoint's mix is present
  // on the channels it listens on, at the level of the line that channel is bound
  // to.  The simulated device carries a 0.25 sine on every capture channel, so a
  // listen channel must read its line's *other three* members summed - a channel
  // carrying the wrong line, or the listener's own audio, would read differently.
  const json status = get_json(client, "/api/status");
  CHECK(!status.is_null());
  CHECK_EQ(status.at("party_lines").size(), 8U);
  const json& playback = status.at("audio").at("playback_dbfs");
  CHECK_EQ(playback.size(), 32U);
  for (unsigned endpoint = 0; endpoint < 16; ++endpoint) {
    for (unsigned pair = 0; pair < 2; ++pair) {
      const double expected = linear_to_dbfs(
          3.0 * 0.25 / std::sqrt(2.0) *
          db_to_linear(-3.0 * static_cast<double>((endpoint + pair) % 8)));
      const json& level = playback[2 * endpoint + pair];
      CHECK(!level.is_null());  // the endpoint hears its line
      if (!level.is_null()) {
        CHECK_NEAR(level.get<double>(), expected, 2.0);
      }
    }
  }

  gateway.stop();
}

TEST_CASE(rest_api_refuses_a_configuration_that_could_not_work) {
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  // the same wiring as the appliance: an accepted edit is applied, a refused one
  // never gets that far
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // A second party line is accepted, so the editor still works...
  const json before = get_json(client, "/api/config");
  CHECK(!before.is_null());
  json lines = before.at("party_lines");
  CHECK_EQ(lines.size(), 1U);
  json second = lines[0];
  second["id"] = "stage";
  second["name"] = "Stage";
  lines.push_back(second);
  // The patch the commissioning page posts: both of the arrays it owns, as they
  // are.
  const json patch =
      json{{"endpoints", before.at("endpoints")}, {"party_lines", lines}};
  const auto accepted =
      client.Post("/api/config", patch.dump(), "application/json");
  CHECK(accepted && accepted->status == 200);
  CHECK_EQ(get_json(client, "/api/config").at("party_lines").size(), 2U);
  // ...and what was saved is what the appliance is running.
  CHECK_EQ(get_json(client, "/api/status").at("party_lines").size(), 2U);
  Config persisted;
  std::string error;
  CHECK(Config::load(kTestConfigPath, &persisted, &error));
  CHECK_EQ(persisted.party_lines.size(), 2U);

  // The edits the commissioning page makes on a member - a contribution level and
  // an unbound listen slot - are what the running matrix then reports.
  const json before_edit = get_json(client, "/api/config");
  json edited = before_edit.at("party_lines");
  edited[1]["members"][1]["contribution_db"] = -6.0;
  edited[1]["members"][1]["listen_channel"] = -1;
  const auto changed = client.Post(
      "/api/config",
      json{{"endpoints", before_edit.at("endpoints")}, {"party_lines", edited}}
          .dump(),
      "application/json");
  CHECK(changed && changed->status == 200);
  const json running = get_json(client, "/api/status").at("party_lines")[1];
  CHECK_NEAR(running.at("members")[1].at("contribution_db").get<double>(), -6.0,
             1e-9);
  CHECK_EQ(running.at("members")[1].at("listen_channel").get<int>(), -1);

  // A reload re-reads the file, and the saved configuration is still the one that
  // runs - which is what makes the file the thing a rebuild restores.
  const auto restarted = client.Post("/api/system/restart", "", "application/json");
  CHECK(restarted && restarted->status == 200);
  const json after_restart = get_json(client, "/api/status").at("party_lines")[1];
  CHECK_NEAR(after_restart.at("members")[1].at("contribution_db").get<double>(),
             -6.0, 1e-9);
  CHECK_EQ(after_restart.at("members")[1].at("listen_channel").get<int>(), -1);

  // ...and one that names a line twice is refused where it is edited: 400 with
  // the reason, and neither the running configuration nor the file changed.
  json clashing = get_json(client, "/api/config").at("party_lines");
  clashing[1]["name"] = "Cameras";
  const auto refused = client.Post(
      "/api/config", json{{"party_lines", clashing}}.dump(), "application/json");
  CHECK(refused && refused->status == 400);
  CHECK(refused->body.find("Cameras") != std::string::npos);
  CHECK_EQ(get_json(client, "/api/config").at("party_lines").size(), 2U);
  Config still;
  CHECK(Config::load(kTestConfigPath, &still, &error));
  CHECK_EQ(still.party_lines.size(), 2U);
  CHECK_EQ(still.party_lines[1].name, std::string("Stage"));

  // A valid document with no party lines at all still loads, as it always did:
  // the validation is additive, not a new requirement to declare a matrix.
  const auto cleared =
      client.Post("/api/config", json{{"party_lines", json::array()}}.dump(),
                  "application/json");
  CHECK(cleared && cleared->status == 200);
  CHECK(get_json(client, "/api/status").at("party_lines").empty());
  Config no_lines;
  CHECK(Config::load(kTestConfigPath, &no_lines, &error));
  CHECK(no_lines.party_lines.empty());

  gateway.stop();
  std::remove(kTestConfigPath.c_str());
}

TEST_CASE(gateway_refuses_an_invalid_configuration_as_a_whole) {
  // Every refusing case, checked the same way: the refusal names what is wrong,
  // and no part of the configuration reaches SIP, the daemon or the matrix.
  struct Case {
    const char* what;
    void (*break_it)(Config&);
    const char* expect;
  };
  const Case cases[] = {
      {"a second line with the same name",
       [](Config& config) {
         PartyLineConfig second = config.party_lines[0];
         second.id = "stage";  // a distinct id: the name is the duplicate
         second.name = "Cameras";
         config.party_lines.push_back(second);
       },
       "two party lines are called 'Cameras'"},
      {"a second endpoint with the same id",
       [](Config& config) { config.endpoints.push_back(config.endpoints[0]); },
       "two endpoints share the id 'a'"},
      {"a device channel claimed by two endpoints",
       [](Config& config) { config.endpoints[1].talk_channels = {0}; },
       "device channel 0 is claimed twice"},
      {"a member channel outside its endpoint's shape",
       [](Config& config) { config.party_lines[0].members[1].listen_channel = 3; },
       "binds listen_channel 3 of endpoint 'b' (Camera 2)"},
      {"a binding to an endpoint nobody declares",
       [](Config& config) { config.party_lines[0].members[1].endpoint = "ghost"; },
       "binds endpoint 'ghost', which no endpoint declares"},
  };

  for (const auto& entry : cases) {
    Gateway gateway(true, [&entry](Config& config) {
      config = party_line_config();
      entry.break_it(config);
    });
    std::string error;
    CHECK(!gateway.lines->apply_configuration(&error));
    CHECK(error.find(entry.expect) != std::string::npos);

    // Nothing applied: no SIP registration, no daemon stream, no routing.  A
    // half-wired site is what this refusal is for.
    CHECK(gateway.engine->account_status().empty());
    json sources;
    json sinks;
    std::string daemon_error;
    CHECK(gateway.daemon->get_sources(&sources, &daemon_error));
    CHECK_EQ(sources.at("sources").size(), 0U);
    CHECK(gateway.daemon->get_sinks(&sinks, &daemon_error));
    CHECK_EQ(sinks.at("sinks").size(), 0U);
    CHECK(gateway.matrix->empty());
  }

  // The same configuration with nothing broken is applied - and then the accounts
  // are registered, so the checks above discriminate rather than passing because
  // the engine and the daemon never report anything.
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  std::string error;
  CHECK(gateway.lines->apply_configuration(&error));
  CHECK(!gateway.engine->account_status().empty());
}

TEST_CASE(rest_api_refuses_an_edit_it_cannot_persist) {
  // A configuration path that cannot be written: the appliance has to say so
  // rather than appearing to have saved, because the file is what a rebuild
  // restores.
  Gateway gateway(
      true, [](Config& config) { config = party_line_config(); },
      "no-such-directory/aes67-sip.conf");
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const json before = get_json(client, "/api/config");
  CHECK(!before.is_null());
  json lines = before.at("party_lines");
  json second = lines[0];
  second["id"] = "stage";
  second["name"] = "Stage";
  lines.push_back(second);
  const auto refused = client.Post(
      "/api/config", json{{"party_lines", lines}}.dump(), "application/json");
  CHECK(refused && refused->status == 500);
  CHECK(refused->body.find("cannot write config file") != std::string::npos);

  // The edit is neither running nor remembered: the page shows the reason
  // instead of a saved state the next restart would lose.
  CHECK_EQ(get_json(client, "/api/config").at("party_lines").size(), 1U);
  CHECK_EQ(get_json(client, "/api/status").at("party_lines").size(), 1U);

  gateway.stop();
}

// ---------------------------------------------------------------------------
// the conference leg: a SIP call whose media is the matrix's conference sides
// ---------------------------------------------------------------------------

namespace {

/**
 * Two party lines of two endpoints each, one of which claims the conference, plus
 * the conference leg itself.  The four endpoints sit on four device channels, so
 * the second line's channels are free of the first's.
 */
Config conference_config(bool cameras_claims, double tone_hz = 1000.0) {
  Config config = test_config();
  config.audio.null_tone_hz = tone_hz;
  config.audio.channels = 4;

  const struct {
    const char* id;
    const char* name;
    unsigned talk;
    unsigned listen;
  } packs[] = {
      {"pack-1", "Camera 1", 0, 1},
      {"pack-2", "Camera 2", 1, 0},
      {"pack-3", "Camera 3", 2, 3},
      {"pack-4", "Camera 4", 3, 2},
  };
  for (const auto& pack : packs) {
    EndpointConfig endpoint;
    endpoint.id = pack.id;
    endpoint.name = pack.name;
    endpoint.talk_channels = {pack.talk};
    endpoint.listen_channels = {pack.listen};
    config.endpoints.push_back(endpoint);
  }

  const auto make_line = [](const std::string& id, const std::string& name,
                            bool claims, const std::string& first,
                            const std::string& second) {
    PartyLineConfig line;
    line.id = id;
    line.name = name;
    line.claims_conference = claims;
    for (const std::string& endpoint : {first, second}) {
      PartyLineMemberConfig member;
      member.endpoint = endpoint;
      member.talk_channel = 0;
      member.listen_channel = 0;
      line.members.push_back(member);
    }
    return line;
  };
  config.party_lines = {
      make_line("cameras", "Cameras", cameras_claims, "pack-1", "pack-2"),
      make_line("stage", "Stage", false, "pack-3", "pack-4")};

  config.conference.enabled = true;
  config.conference.account = "pbx";
  config.conference.target = "sip:conf@pbx.example.com";
  config.conference.display_name = "Conference";
  return config;
}

/** A playback channel's held level, with digital silence read as the floor. */
double playback_level(const json& status, size_t channel) {
  const json& levels = status.at("audio").at("playback_dbfs");
  if (channel >= levels.size() || levels[channel].is_null()) {
    return kSilenceDbfs;
  }
  return levels[channel].get<double>();
}

/** True when a reported dBFS value means digital silence, however it is serialised.
 */
bool is_silent(const json& value) {
  return value.is_null() || value.get<double>() <= -60.0;
}

/** True while the conference leg reports itself in a call. */
bool conference_in_call(const json& status) {
  return json_get_path<std::string>(status, {"conference", "state"}, "") ==
         "in_call";
}

/** True while the conference leg reports the given state. */
bool conference_reports(const json& status, const std::string& state) {
  return json_get_path<std::string>(status, {"conference", "state"}, "") == state;
}

/**
 * The configuration file the gateway writes, read back as the file on disk.  A
 * test that claims "the file is what a rebuild restores" has to read the file:
 * `GET /api/config` answers from the running configuration, which a save that
 * wrote nothing would leave looking correct.
 */
json saved_config_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return nullptr;
  }
  try {
    return json::parse(in);
  } catch (const json::exception&) {
    return nullptr;
  }
}

}  // namespace

TEST_CASE(conference_leg_carries_only_the_lines_that_claim_it) {
  // The simulated device carries no tone, so what a listen channel shows is what
  // the matrix put there - here, the conference's own audio.
  Gateway gateway(true,
                  [](Config& config) { config = conference_config(true, 0.0); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // The stub dials, rings and answers on its own, so the leg comes up.
  CHECK(gateway.wait_for(conference_in_call, 8000));
  const json conference = get_json(client, "/api/status").at("conference");
  CHECK(conference.at("enabled").get<bool>());
  CHECK_EQ(conference.at("target").get<std::string>(),
           std::string("sip:conf@pbx.example.com"));

  // Nothing is heard on either line before the conference says anything.
  const json before = get_json(client, "/api/status");
  CHECK(is_silent(before.at("audio").at("playback_dbfs")[1]));
  CHECK(is_silent(before.at("audio").at("playback_dbfs")[3]));

  // What the conference says arrives on the SIP leg: the mix-minus mixes of the
  // lines that claim it must carry it, and no other channel must.
  std::vector<float> from_conference(800, 0.25F);
  gateway.lines->push_from_sip(kConferenceLineId, from_conference.data(),
                               from_conference.size());
  CHECK(gateway.wait_for(
      [](const json& status) { return playback_level(status, 1) > -30.0; }, 4000));

  const json status = get_json(client, "/api/status");
  CHECK(playback_level(status, 1) > -30.0);  // Camera 1 (claims) hears it
  CHECK(playback_level(status, 0) > -30.0);  // so does Camera 2
  CHECK(is_silent(status.at("audio").at("playback_dbfs")[3]));  // Stage: untouched
  CHECK(is_silent(status.at("audio").at("playback_dbfs")[2]));

  gateway.stop();
}

TEST_CASE(conference_leg_sends_the_claiming_lines_mix) {
  // The device carries a tone on every capture channel, so every member is
  // talking: the conference hears the sum of the claiming line's members.
  Gateway gateway(true, [](Config& config) { config = conference_config(true); });
  gateway_up(gateway);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  // Pull from the leg the way its media thread does, until the mix comes through
  // its resampler.
  bool heard = false;
  for (int attempt = 0; attempt < 60 && !heard; ++attempt) {
    std::vector<float> mix(160, 0.0F);
    gateway.lines->pull_to_sip(kConferenceLineId, mix.data(), mix.size());
    for (const float sample : mix) {
      if (std::fabs(sample) > 1e-4F) {
        heard = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(heard);
  gateway.stop();

  // A site whose lines do not claim the conference sends it nothing at all.
  Gateway quiet(true, [](Config& config) { config = conference_config(false); });
  gateway_up(quiet);
  CHECK(quiet.wait_for(conference_in_call, 8000));
  bool leaked = false;
  for (int attempt = 0; attempt < 60 && !leaked; ++attempt) {
    std::vector<float> mix(160, 0.0F);
    quiet.lines->pull_to_sip(kConferenceLineId, mix.data(), mix.size());
    for (const float sample : mix) {
      if (std::fabs(sample) > 1e-4F) {
        leaked = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(!leaked);
  quiet.stop();
}

TEST_CASE(conference_leg_that_cannot_be_established_is_reported) {
  Gateway gateway(true, [](Config& config) { config = conference_config(true); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  // The PBX refuses the call (busy): the leg reports it, with the reason...
  CHECK(gateway.stub() != nullptr);
  CHECK(gateway.stub()->simulate_call_failure(kConferenceLineId, "486 busy here",
                                              &gateway.error));
  CHECK(gateway.wait_for(
      [](const json& status) { return conference_reports(status, "error"); },
      3000));
  const json conference = get_json(client, "/api/status").at("conference");
  CHECK_EQ(conference.at("state_code").get<int>(), 486);
  CHECK(conference.at("detail").get<std::string>().find("486 busy here") !=
        std::string::npos);

  // ...and the site's own talk paths keep working without it: every member of
  // every line is still arriving, and the matrix is still mixing.
  const json party_lines = get_json(client, "/api/status").at("party_lines");
  CHECK_EQ(party_lines.size(), 2U);
  unsigned members = 0;
  bool every_member_arriving = true;
  for (const auto& line : party_lines) {
    for (const auto& member : line.at("members")) {
      ++members;
      every_member_arriving =
          every_member_arriving && member.at("arriving").get<bool>();
    }
  }
  CHECK_EQ(members, 4U);
  CHECK(every_member_arriving);

  // The self-test reports the leg as a failure with the same reason, rather than
  // claiming health or hiding it.
  const auto response =
      client.Post("/api/system/self-test", "", "application/json");
  CHECK(response && response->status == 200);
  const json self_test = json::parse(response->body);
  bool saw_conference = false;
  for (const auto& check : self_test.at("checks")) {
    if (check.at("name") != "conference leg") {
      continue;
    }
    saw_conference = true;
    CHECK(!check.at("ok").get<bool>());
    CHECK(check.at("detail").get<std::string>().find("486 busy here") !=
          std::string::npos);
  }
  CHECK(saw_conference);
  gateway.stop();
}

TEST_CASE(rest_api_reports_a_conference_leg_that_is_not_configured) {
  // The default fixture has no conference block: the status says so, the leg is
  // not a line, and the self-test does not judge it.
  Gateway gateway;
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const json status = get_json(client, "/api/status");
  const json& conference = status.at("conference");
  CHECK(!conference.at("enabled").get<bool>());
  CHECK_EQ(conference.at("state").get<std::string>(), std::string("disabled"));
  CHECK(is_silent(conference.at("levels").at("to_conference_dbfs")));
  CHECK(is_silent(conference.at("levels").at("from_conference_dbfs")));

  // It is not one of the lines: `lines[]` is unchanged, and the line count the
  // rest of the appliance uses does not include it.
  CHECK_EQ(status.at("lines").size(), 1U);
  CHECK_EQ(gateway.lines->line_count(), 1);

  const auto response =
      client.Post("/api/system/self-test", "", "application/json");
  CHECK(response && response->status == 200);
  const json self_test = json::parse(response->body);
  for (const auto& check : self_test.at("checks")) {
    CHECK(check.at("name") != "conference leg");
  }

  gateway.stop();
}

TEST_CASE(rest_api_refuses_a_conference_leg_that_could_not_be_dialled) {
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // Enabled with nowhere to dial is refused where the configuration is edited,
  // and nothing of it is applied.
  const auto refused = client.Post(
      "/api/config",
      json{{"conference", json{{"enabled", true}, {"target", ""}}}}.dump(),
      "application/json");
  CHECK(refused && refused->status == 400);
  CHECK(refused->body.find("conference leg is enabled but has no target") !=
        std::string::npos);
  CHECK(
      !get_json(client, "/api/status").at("conference").at("enabled").get<bool>());

  // A target with an account nobody declares is refused too: the leg would
  // register nowhere.
  const auto unknown_account =
      client.Post("/api/config",
                  json{{"conference", json{{"enabled", true},
                                           {"target", "sip:conf@pbx.example.com"},
                                           {"account", "ghost"}}}}
                      .dump(),
                  "application/json");
  CHECK(unknown_account && unknown_account->status == 400);
  CHECK(unknown_account->body.find("account 'ghost'") != std::string::npos);

  gateway.stop();
}

TEST_CASE(conference_leg_survives_a_configuration_edit) {
  // An operator nudging a level or a binding while the site is on air must not
  // take the off-site call's audio away: the call gate belongs to the call.
  Gateway gateway(true, [](Config& config) { config = conference_config(true); });
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  const auto mix_is_heard = [&gateway]() {
    for (int attempt = 0; attempt < 25; ++attempt) {
      std::vector<float> mix(160, 0.0F);
      gateway.lines->pull_to_sip(kConferenceLineId, mix.data(), mix.size());
      for (const float sample : mix) {
        if (std::fabs(sample) > 1e-4F) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  };
  CHECK(mix_is_heard());

  // The edit the commissioning page would make: a member's contribution level.
  const json before = get_json(client, "/api/config");
  json lines = before.at("party_lines");
  lines[0]["members"][0]["contribution_db"] = -6.0;
  const auto edited = client.Post(
      "/api/config",
      json{{"endpoints", before.at("endpoints")}, {"party_lines", lines}}.dump(),
      "application/json");
  CHECK(edited && edited->status == 200);
  CHECK(gateway.wait_for(conference_in_call, 4000));

  // The leg is still carrying audio in both directions, and still only to the
  // lines that claim it.
  CHECK(mix_is_heard());
  std::vector<float> from_conference(800, 0.25F);
  gateway.lines->push_from_sip(kConferenceLineId, from_conference.data(),
                               from_conference.size());
  CHECK(gateway.wait_for(
      [](const json& status) { return playback_level(status, 1) > -30.0; }, 4000));

  gateway.stop();
}

TEST_CASE(conference_leg_can_be_switched_off) {
  Gateway gateway(true, [](Config& config) { config = conference_config(true); });
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  // Switching the leg off takes the call, the routing and the registration with
  // it, and says so.
  const auto disabled = client.Post(
      "/api/config", json{{"conference", json{{"enabled", false}}}}.dump(),
      "application/json");
  CHECK(disabled && disabled->status == 200);
  CHECK(gateway.wait_for(
      [](const json& status) { return conference_reports(status, "disabled"); },
      4000));
  const json status = get_json(client, "/api/status");
  CHECK(!status.at("conference").at("enabled").get<bool>());
  CHECK(is_silent(status.at("conference").at("levels").at("to_conference_dbfs")));
  CHECK_EQ(status.at("lines").size(), 1U);  // the lines the site runs are untouched
  CHECK_EQ(status.at("party_lines").size(), 2U);

  // ...and switching it back on brings it up again, so the removal was clean.
  const auto enabled =
      client.Post("/api/config",
                  json{{"conference", json{{"enabled", true},
                                           {"account", "pbx"},
                                           {"target", "sip:conf@pbx.example.com"}}}}
                      .dump(),
                  "application/json");
  CHECK(enabled && enabled->status == 200);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  gateway.stop();
}

TEST_CASE(conference_is_set_up_from_the_api_like_a_line) {
  // A site with no off-site leg: the fields the form has are enough to put one up,
  // and the file is what a rebuild restores - so the file is read back, not only
  // the running configuration.  Its own config path, so what another test wrote
  // cannot be mistaken for what this one did.
  const std::string path = "aes67-sip-conference-setup-test.conf";
  Gateway gateway(
      true,
      [](Config& config) {
        config = conference_config(false);
        config.conference.enabled = false;
        config.conference.target.clear();
      },
      path);
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const json before = get_json(client, "/api/status").at("conference");
  CHECK(!before.at("enabled").get<bool>());
  CHECK_EQ(before.at("state").get<std::string>(), std::string("disabled"));

  const auto saved = client.Post("/api/conference/config",
                                 json{{"enabled", true},
                                      {"account", "pbx"},
                                      {"target", "sip:conf@pbx.example.com"},
                                      {"display_name", "FreePBX"}}
                                     .dump(),
                                 "application/json");
  CHECK(saved && saved->status == 200);
  const json set_up = json::parse(saved->body);
  CHECK(set_up.at("enabled").get<bool>());
  CHECK_EQ(set_up.at("target").get<std::string>(),
           std::string("sip:conf@pbx.example.com"));
  CHECK_EQ(set_up.at("name").get<std::string>(), std::string("FreePBX"));

  CHECK(gateway.wait_for(conference_in_call, 8000));
  const json persisted = get_json(client, "/api/config").at("conference");
  CHECK(persisted.at("enabled").get<bool>());
  CHECK_EQ(persisted.at("account").get<std::string>(), std::string("pbx"));
  CHECK_EQ(persisted.at("display_name").get<std::string>(), std::string("FreePBX"));

  const json on_disk = saved_config_file(path);
  CHECK(!on_disk.is_null());
  CHECK(on_disk.at("conference").at("enabled").get<bool>());
  CHECK_EQ(on_disk.at("conference").at("target").get<std::string>(),
           std::string("sip:conf@pbx.example.com"));
  CHECK_EQ(on_disk.at("conference").at("display_name").get<std::string>(),
           std::string("FreePBX"));

  // A partial update is partial: the keys it does not name keep their values, in
  // the reply and in the file.
  const auto retargeted = client.Post(
      "/api/conference/config",
      json{{"target", "sip:room2@pbx.example.com"}}.dump(), "application/json");
  CHECK(retargeted && retargeted->status == 200);
  const json moved = json::parse(retargeted->body);
  CHECK(moved.at("enabled").get<bool>());
  CHECK_EQ(moved.at("account").get<std::string>(), std::string("pbx"));
  CHECK_EQ(moved.at("name").get<std::string>(), std::string("FreePBX"));
  CHECK_EQ(moved.at("target").get<std::string>(),
           std::string("sip:room2@pbx.example.com"));
  const json file_after = saved_config_file(path).at("conference");
  CHECK(file_after.at("enabled").get<bool>());
  CHECK_EQ(file_after.at("account").get<std::string>(), std::string("pbx"));
  CHECK_EQ(file_after.at("display_name").get<std::string>(),
           std::string("FreePBX"));
  CHECK_EQ(file_after.at("target").get<std::string>(),
           std::string("sip:room2@pbx.example.com"));

  gateway.stop();
}

TEST_CASE(conference_setup_refuses_what_could_not_work) {
  const std::string path = "aes67-sip-conference-refuse-test.conf";
  Gateway gateway(
      true,
      [](Config& config) {
        config = conference_config(false);
        config.conference.enabled = false;
        config.conference.target.clear();
      },
      path);
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // Enabled with nothing to dial.
  const auto no_target = client.Post(
      "/api/conference/config",
      json{{"enabled", true}, {"account", "pbx"}, {"target", ""}}.dump(),
      "application/json");
  CHECK(no_target && no_target->status == 400);
  CHECK(no_target->body.find("conference.target") != std::string::npos);

  // An account nobody declared.
  const auto unknown_account =
      client.Post("/api/conference/config",
                  json{{"enabled", true},
                       {"account", "nosuchaccount"},
                       {"target", "sip:conf@pbx.example.com"}}
                      .dump(),
                  "application/json");
  CHECK(unknown_account && unknown_account->status == 400);
  CHECK(unknown_account->body.find("nosuchaccount") != std::string::npos);

  // A key the block does not have is a refusal, not a typo nothing reads.
  const auto typo = client.Post(
      "/api/conference/config",
      json{{"targert", "sip:conf@pbx.example.com"}}.dump(), "application/json");
  CHECK(typo && typo->status == 400);
  CHECK(typo->body.find("targert") != std::string::npos);

  // A call control on a leg that is not configured says which switch is off.
  const auto dial_disabled = client.Post(
      "/api/conference/call", json{{"action", "dial"}}.dump(), "application/json");
  CHECK(dial_disabled && dial_disabled->status == 400);
  CHECK(dial_disabled->body.find("conference.enabled") != std::string::npos);

  // None of the four applied anything: the site still has no leg, and nothing was
  // written to the file a rebuild would restore.
  const json status = get_json(client, "/api/status").at("conference");
  CHECK(!status.at("enabled").get<bool>());
  CHECK_EQ(status.at("state").get<std::string>(), std::string("disabled"));
  CHECK(saved_config_file(path).is_null());

  gateway.stop();
}

TEST_CASE(conference_hang_up_holds_the_leg_down_until_dial) {
  // Its own config path: this case saves through POST /api/config, and the shared
  // one is re-read by whichever test restarts from the file next.
  const std::string path = "aes67-sip-conference-hold-test.conf";
  Gateway gateway(
      true, [](Config& config) { config = conference_config(true); }, path);
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  // Hang up by hand: the call goes, and the five second retry does not raise it
  // again - without that, the button would be a lie.
  const auto hung_up =
      client.Post("/api/conference/call", json{{"action", "hangup"}}.dump(),
                  "application/json");
  CHECK(hung_up && hung_up->status == 200);
  const json after_hangup = json::parse(hung_up->body).at("conference");
  CHECK_EQ(after_hangup.at("state").get<std::string>(), std::string("idle"));
  CHECK(after_hangup.at("redial_paused").get<bool>());

  // Longer than the retry interval: unheld, the supervisor would have dialled it.
  std::this_thread::sleep_for(std::chrono::milliseconds(6000));
  const json still_down = get_json(client, "/api/status").at("conference");
  CHECK(still_down.at("redial_paused").get<bool>());
  CHECK_EQ(still_down.at("state").get<std::string>(), std::string("idle"));

  // Dial raises it now and clears the hold.
  const auto dialled = client.Post(
      "/api/conference/call", json{{"action", "dial"}}.dump(), "application/json");
  CHECK(dialled && dialled->status == 200);
  CHECK(gateway.wait_for(conference_in_call, 8000));
  CHECK(!get_json(client, "/api/status")
             .at("conference")
             .at("redial_paused")
             .get<bool>());

  // An action the leg does not have is refused rather than ignored.
  const auto unknown =
      client.Post("/api/conference/call", json{{"action", "answer"}}.dump(),
                  "application/json");
  CHECK(unknown && unknown->status == 400);
  CHECK(unknown->body.find("dial or hangup") != std::string::npos);

  // A hang up by hand holds the leg down, but a change that would raise a
  // *different* call is the operator saying "try it again" - and it clears through
  // the generic config route as well, which is the Settings editor's path rather
  // than this card's.
  const auto held =
      client.Post("/api/conference/call", json{{"action", "hangup"}}.dump(),
                  "application/json");
  CHECK(held && held->status == 200);
  CHECK(json::parse(held->body).at("conference").at("redial_paused").get<bool>());
  const auto retargeted = client.Post(
      "/api/config",
      json{{"conference", json{{"target", "sip:other@pbx.example.com"}}}}.dump(),
      "application/json");
  CHECK(retargeted && retargeted->status == 200);
  CHECK(gateway.wait_for(
      [](const json& status) {
        return conference_in_call(status) &&
               !json_get_path<bool>(status, {"conference", "redial_paused"}, true);
      },
      8000));

  gateway.stop();
}

TEST_CASE(conference_leg_isolation_holds_in_both_directions) {
  // One site, two lines: "cameras" claims the conference, "stage" does not.  With
  // the tone on, every member of both lines is talking, so the two directions can
  // be told apart by muting one line's members at a time:
  //
  //   - stage's members audible, cameras' muted: the stage line mixes them for
  //     each other, and the conference hears nothing at all;
  //   - cameras' members audible too: now the conference hears them.
  //
  // A leg that read a device channel, or that mixed a non-claiming line in, fails
  // the first half.
  Gateway gateway(true, [](Config& config) {
    config = conference_config(true);
    for (auto& member : config.party_lines[0].members) {
      member.mute = true;  // cameras: audible only in the second half
    }
  });
  gateway.api->set_restart_handler([&gateway](std::string* restart_error) {
    return gateway.restart_from_file(restart_error);
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(gateway.wait_for(conference_in_call, 8000));

  const auto conference_hears = [&gateway]() {
    for (int attempt = 0; attempt < 25; ++attempt) {
      std::vector<float> mix(160, 0.0F);
      gateway.lines->pull_to_sip(kConferenceLineId, mix.data(), mix.size());
      for (const float sample : mix) {
        if (std::fabs(sample) > 1e-4F) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  };

  // The stage line's members are working - they hear each other on their own
  // channels - while the conference hears none of it.
  CHECK(gateway.wait_for(
      [](const json& status) { return playback_level(status, 3) > -30.0; }, 4000));
  CHECK(!conference_hears());

  // Unmute the claiming line's members: their audio, and only theirs, now reaches
  // the conference.
  const json before = get_json(client, "/api/config");
  json lines = before.at("party_lines");
  for (auto& member : lines[0]["members"]) {
    member["mute"] = false;
  }
  const auto unmuted = client.Post(
      "/api/config",
      json{{"endpoints", before.at("endpoints")}, {"party_lines", lines}}.dump(),
      "application/json");
  CHECK(unmuted && unmuted->status == 200);
  CHECK(conference_hears());

  gateway.stop();
}

// ---------------------------------------------------------------------------
// the matrix in the self-test: who is on each line, and who has gone silent
// ---------------------------------------------------------------------------

namespace {

/** The self-test check named `name`, or null. */
json self_test_check(const json& result, const std::string& name) {
  if (!result.is_object() || !result.contains("checks") ||
      !result.at("checks").is_array()) {
    return nullptr;
  }
  for (const auto& check : result.at("checks")) {
    if (json_get<std::string>(check, "name", "") == name) {
      return check;
    }
  }
  return nullptr;
}

/** The `party_lines[]` entry for `id`, or null. */
json summary_of(const json& status, const std::string& id) {
  if (!status.is_object() || !status.contains("party_lines")) {
    return nullptr;
  }
  for (const auto& line : status.at("party_lines")) {
    if (json_get<std::string>(line, "id", "") == id) {
      return line;
    }
  }
  return nullptr;
}

/** Runs the self-test over HTTP. */
json run_self_test(httplib::Client& client) {
  const auto response =
      client.Post("/api/system/self-test", "", "application/json");
  if (!response || response->status != 200) {
    return nullptr;
  }
  return json::parse(response->body);
}

/** The simulated device, so a test can make every member talk or stop talking. */
NullAudioBackend* null_backend(AudioBackend* backend) {
  return dynamic_cast<NullAudioBackend*>(backend);
}

}  // namespace

TEST_CASE(rest_api_self_test_reports_each_party_line_and_who_is_silent) {
  Gateway gateway(true, [](Config& config) {
    config = party_line_config();
    config.audio.null_tone_hz = 0.0;  // silence first: the members are quiet
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // The device is silent, so no member is arriving - and that is not a failure:
  // the report says nobody is talking, and names the members it cannot hear from.
  json result = run_self_test(client);
  CHECK(!result.is_null());
  json check = self_test_check(result, "party line cameras (Cameras)");
  CHECK(!check.is_null());
  CHECK(check.at("detail").get<std::string>().find("2 member(s), 0 arriving") !=
        std::string::npos);
  CHECK(check.at("detail").get<std::string>().find(
            "2 silent (Camera 1, Camera 2)") != std::string::npos);
  CHECK(check.at("detail").get<std::string>().find("nobody is talking") !=
        std::string::npos);
  CHECK(check.at("ok").get<bool>());  // quiet is not broken

  json line = summary_of(get_json(client, "/api/status"), "cameras");
  CHECK(!line.is_null());
  CHECK_EQ(line.at("state").get<std::string>(), std::string("quiet"));
  CHECK_EQ(line.at("summary").at("members").get<unsigned>(), 2U);
  CHECK_EQ(line.at("summary").at("arriving").get<unsigned>(), 0U);
  CHECK(line.at("quiet").get<bool>());
  CHECK(line.at("can_be_heard").get<bool>());

  // Audio starts arriving: the same report follows within a bounded time, which is
  // also what the dashboard shows, since it polls these fields.
  CHECK(null_backend(gateway.backend.get()) != nullptr);
  null_backend(gateway.backend.get())->set_tone_hz(1000.0);
  CHECK(gateway.wait_for(
      [](const json& status) {
        const json line = summary_of(status, "cameras");
        return !line.is_null() &&
               line.at("summary").at("arriving").get<unsigned>() == 2U;
      },
      4000));
  result = run_self_test(client);
  check = self_test_check(result, "party line cameras (Cameras)");
  CHECK(!check.is_null());
  CHECK(check.at("detail").get<std::string>().find(
            "2 arriving (Camera 1, Camera 2)") != std::string::npos);
  CHECK(check.at("detail").get<std::string>().find("nobody is talking") ==
        std::string::npos);
  CHECK(check.at("ok").get<bool>());

  // ...and when it stops again, the members are reported silent within the bound.
  null_backend(gateway.backend.get())->set_tone_hz(0.0);
  CHECK(gateway.wait_for(
      [](const json& status) {
        const json line = summary_of(status, "cameras");
        return !line.is_null() && line.at("quiet").get<bool>();
      },
      4000));
  result = run_self_test(client);
  check = self_test_check(result, "party line cameras (Cameras)");
  CHECK(!check.is_null());
  CHECK(check.at("detail").get<std::string>().find(
            "2 silent (Camera 1, Camera 2)") != std::string::npos);
  CHECK(check.at("ok").get<bool>());

  gateway.stop();
}

TEST_CASE(rest_api_self_test_reports_a_line_nobody_can_talk_on) {
  // Both members listen and neither has a talk channel bound: the line cannot
  // carry a contribution at all, which is a different thing from a quiet line.
  Gateway gateway(true, [](Config& config) {
    config = party_line_config();
    for (auto& member : config.party_lines[0].members) {
      member.talk_channel = -1;
    }
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const json result = run_self_test(client);
  const json check = self_test_check(result, "party line cameras (Cameras)");
  CHECK(!check.is_null());
  CHECK(!check.at("ok").get<bool>());
  CHECK(check.at("detail").get<std::string>().find("with no talk channel bound") !=
        std::string::npos);
  CHECK(check.at("detail").get<std::string>().find("no member can be heard") !=
        std::string::npos);

  const json line = summary_of(get_json(client, "/api/status"), "cameras");
  CHECK(!line.is_null());
  CHECK(!line.at("can_be_heard").get<bool>());
  CHECK(!line.at("quiet").get<bool>());
  CHECK_EQ(line.at("summary").at("unbound").get<unsigned>(), 2U);

  gateway.stop();
}

TEST_CASE(rest_api_self_test_is_honest_when_it_cannot_judge_the_matrix) {
  // The matrix mixes on the audio path and needs nothing of SIP: a dead audio path
  // must not be reported as broken party lines, and an unavailable SIP engine must
  // not stop the matrix from being judged.
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);
  CHECK(!self_test_check(run_self_test(client), "party line cameras (Cameras)")
             .is_null());

  // Audio down: nothing is claimed about the lines at all, and the audio backend
  // carries both the failure and what it costs the site.
  gateway.router->stop();
  const json result = run_self_test(client);
  CHECK(self_test_check(result, "party line cameras (Cameras)").is_null());
  CHECK(self_test_check(result, "party lines").is_null());
  const json backend = self_test_check(result, "audio backend");
  CHECK(!backend.is_null());
  CHECK(!backend.at("ok").get<bool>());
  CHECK(backend.at("detail").get<std::string>().find(
            "the party lines are not being mixed while the audio path is down") !=
        std::string::npos);
  gateway.stop();

  // SIP disabled: the matrix is judged on its own, with no false failure.
  Gateway no_sip(true, [](Config& config) {
    config = party_line_config();
    config.sip.enabled = false;
  });
  CHECK(no_sip.start());
  const json with_sip_down = run_self_test(client);
  const json judged =
      self_test_check(with_sip_down, "party line cameras (Cameras)");
  CHECK(!judged.is_null());
  CHECK(judged.at("ok").get<bool>());
  CHECK(judged.at("detail").get<std::string>().find("2 arriving") !=
        std::string::npos);
  no_sip.stop();
}

TEST_CASE(rest_api_plays_a_commissioning_tone_on_a_line) {
  // The tone is how a path is proven without a PBX or an endpoint: it goes onto the
  // line's AES67 output channels, so it must show up in the device's playback
  // levels, be visible as running, and stop when told to.  The default fixture has
  // one line on channels 0 and 1 and no matrix writing those channels.
  Gateway gateway;
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // Silent to begin with: no call is up, so nothing is played to the device.
  const json before = get_json(client, "/api/status");
  CHECK(is_silent(before.at("audio").at("playback_dbfs")[0]));
  CHECK(is_silent(before.at("audio").at("playback_dbfs")[1]));
  CHECK(!before.at("lines")[0].at("test_tone").get<bool>());

  // A bare body gets the commissioning default: 1 kHz for five seconds.
  const auto started = client.Post("/api/lines/0/tone", "{}", "application/json");
  CHECK(started && started->status == 200);
  const json started_line = json::parse(started->body).at("line");
  CHECK(started_line.at("test_tone").get<bool>());
  // The tone reaches the line's channels on the device, and the meter says so.
  CHECK(gateway.wait_for(
      [](const json& status) {
        return playback_level(status, 0) > -30.0 &&
               playback_level(status, 1) > -30.0;
      },
      4000));

  // Stopping it takes it off the channels again, within the meter's bounded decay.
  const auto stopped = client.Post(
      "/api/lines/0/tone", json{{"action", "stop"}}.dump(), "application/json");
  CHECK(stopped && stopped->status == 200);
  CHECK(!json::parse(stopped->body).at("line").at("test_tone").get<bool>());
  CHECK(gateway.wait_for(
      [](const json& status) {
        return is_silent(status.at("audio").at("playback_dbfs")[0]);
      },
      3000));

  // Refusals: an unknown line, an unknown action and a nonsense frequency are all
  // answered with the reason rather than accepted.
  const auto unknown_line =
      client.Post("/api/lines/9/tone", "{}", "application/json");
  CHECK(unknown_line && unknown_line->status == 400);
  CHECK(unknown_line->body.find("unknown line") != std::string::npos);
  const auto unknown_action = client.Post(
      "/api/lines/0/tone", json{{"action", "sing"}}.dump(), "application/json");
  CHECK(unknown_action && unknown_action->status == 400);
  CHECK(unknown_action->body.find("unknown tone action") != std::string::npos);
  const auto bad_hz =
      client.Post("/api/lines/0/tone", json{{"action", "start"}, {"hz", 1}}.dump(),
                  "application/json");
  CHECK(bad_hz && bad_hz->status == 400);
  CHECK(bad_hz->body.find("hz must be between") != std::string::npos);

  gateway.stop();
}

TEST_CASE(rest_api_reports_silence_as_null_not_as_ever_decreasing_db) {
  // A meter that decays without a floor reports -47796 dBFS after a quiet minute,
  // which reads like a broken meter rather than a quiet one; silence is the floor,
  // and it serialises as null - which is what the API documents and what the UI
  // renders as "-inf".
  Gateway gateway(true, [](Config& config) {
    config = party_line_config();
    config.audio.null_tone_hz = 0.0;  // nothing is talking anywhere
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // Let the meters sit in silence for a while, then read them: every level is the
  // floor, so every level is null - not a huge negative number.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const json status = get_json(client, "/api/status");
  CHECK_EQ(status.at("party_lines").size(), 1U);
  for (const auto& level : status.at("audio").at("playback_dbfs")) {
    CHECK(level.is_null());
  }
  for (const auto& level : status.at("audio").at("levels_dbfs")) {
    CHECK(level.is_null());
  }
  for (const auto& member : status.at("party_lines")[0].at("members")) {
    CHECK(member.at("level_dbfs").is_null());
    CHECK(!member.at("arriving").get<bool>());
  }

  gateway.stop();
}

TEST_CASE(rest_api_refuses_a_tone_on_a_line_with_no_channel_on_the_device) {
  // A line whose channels are all outside the opened device cannot put a tone
  // anywhere: better a refusal than a "running" tone that is never written.
  Gateway gateway(true, [](Config& config) {
    config.lines[0].aes67.channels = {12};  // device opens 4 channels
    config.audio.channels = 4;
  });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  const auto refused = client.Post("/api/lines/0/tone", "{}", "application/json");
  CHECK(refused && refused->status == 400);
  CHECK(refused->body.find("no AES67 channel inside the device") !=
        std::string::npos);
  CHECK(!get_json(client, "/api/lines/0").at("test_tone").get<bool>());
  gateway.stop();

  // A value of the wrong type is refused rather than quietly replaced by the
  // default, which would make the tone look like it had been honoured.  Its own
  // gateway, on the same port as the one above only because that one has stopped.
  Gateway ok(true, [](Config& config) { config = test_config(); });
  CHECK(ok.start());
  httplib::Client ok_client("127.0.0.1", kTestPort);
  ok_client.set_connection_timeout(2, 0);
  const auto wrong_type = ok_client.Post(
      "/api/lines/0/tone", json{{"action", "start"}, {"hz", "loud"}}.dump(),
      "application/json");
  CHECK(wrong_type && wrong_type->status == 400);
  CHECK(wrong_type->body.find("hz must be a number") != std::string::npos);
  ok.stop();
}

TEST_CASE(rest_api_does_not_blame_the_daemon_for_a_sink_without_a_stream) {
  // The gateway polls each line's daemon sink every couple of seconds.  A real
  // daemon answers 400 "stream not in use" for a sink it has no stream for, so the
  // status must say *that* rather than surfacing a daemon error - which is what the
  // front end flashed - and must not report the daemon as unreachable.
  Gateway gateway(true, [](Config& config) { config = party_line_config(); });
  gateway_up(gateway);
  httplib::Client client("127.0.0.1", kTestPort);
  client.set_connection_timeout(2, 0);

  // The fixture's line has a sink id the daemon has nothing on (no endpoint SDP is
  // configured, so no sink was created).
  CHECK(gateway.wait_for(
      [](const json& status) {
        const json& line = status.at("lines")[0].at("aes67");
        return line.contains("sink_in_use") && !line.at("sink_in_use").get<bool>();
      },
      4000));
  const json status = get_json(client, "/api/status");
  CHECK(status.at("aes67").at("connected").get<bool>());
  CHECK(status.at("aes67").at("error").get<std::string>().empty());
  CHECK(!status.at("lines")[0].at("aes67").at("receiving").get<bool>());

  // ...and the self-test says which of the two problems it is: there is nothing to
  // receive on, rather than a stream that has gone quiet.
  const json result = run_self_test(client);
  const json check = self_test_check(result, "line 0 (Stage Left)");
  CHECK(!check.is_null());
  CHECK(check.at("detail").get<std::string>().find(
            "the daemon has no stream on sink") != std::string::npos);

  gateway.stop();
}

TEST_CASE(rest_api_self_test_reports_checks) {
  Gateway gateway;
  gateway_up(gateway);
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
