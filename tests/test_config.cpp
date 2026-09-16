#include <fstream>
#include <limits>

#include "config.hpp"
#include "test_framework.hpp"
#include "util.hpp"

using namespace aes67sip;

TEST_CASE(config_defaults_are_usable) {
  const Config config = Config::from_json(json::object());
  CHECK_EQ(config.http_port, 8081);
  CHECK_EQ(config.audio.sample_rate, 48000U);
  CHECK_EQ(config.audio.device, std::string("plughw:RAVENNA"));
  CHECK_EQ(config.accounts.size(), 1U);
  CHECK_EQ(config.lines.size(), 1U);
  CHECK_EQ(config.lines[0].sip.extension, std::string("line0"));
  CHECK(!config.lines[0].aes67.channels.empty());
}

TEST_CASE(config_json_round_trip) {
  Config original = Config::from_json(json::object());
  original.http_port = 9099;
  original.audio.channels = 32;
  original.sip.codecs = {"PCMA/8000/1", "G722/16000/1"};
  original.accounts[0].registrar = "sip:pbx.example.com";
  original.accounts[0].username = "gateway";
  original.lines[0].name = "Stage Left";
  original.lines[0].aes67.channels = {4, 5};
  original.lines[0].sip.call_mode = "ptt";
  original.lines[0].gain_db = -3.5;

  const Config restored = Config::from_json(original.to_json());
  CHECK_EQ(restored.http_port, 9099);
  CHECK_EQ(restored.audio.channels, 32U);
  CHECK_EQ(restored.sip.codecs.size(), 2U);
  CHECK_EQ(restored.sip.codecs[0], std::string("PCMA/8000/1"));
  CHECK_EQ(restored.accounts[0].username, std::string("gateway"));
  CHECK_EQ(restored.lines[0].name, std::string("Stage Left"));
  CHECK_EQ(restored.lines[0].aes67.channels, std::vector<unsigned>({4, 5}));
  CHECK_EQ(restored.lines[0].sip.call_mode, std::string("ptt"));
  CHECK_NEAR(restored.lines[0].gain_db, -3.5, 1e-9);
}

TEST_CASE(config_partial_merge_keeps_untouched_values) {
  Config config = Config::from_json(json::object());
  config.accounts[0].username = "keepme";
  config.lines[0].name = "Line A";

  config.merge(json{{"sip", {{"ptime_ms", 10}}},
                    {"lines", json::array({json{{"id", 0}, {"name", "Line A"}}})}});

  CHECK_EQ(config.sip.ptime_ms, 10);
  CHECK_EQ(config.accounts[0].username, std::string("keepme"));
  CHECK_EQ(config.sip.local_port, 5060);  // untouched
}

TEST_CASE(config_load_and_save) {
  const std::string path = "aes67-sip-test-config.json";
  Config config = Config::from_json(json::object());
  config.http_port = 18081;
  config.lines[0].name = "Saved Line";
  std::string error;
  CHECK(config.save(path, &error));

  Config loaded;
  CHECK(Config::load(path, &loaded, &error));
  CHECK_EQ(loaded.http_port, 18081);
  CHECK_EQ(loaded.lines[0].name, std::string("Saved Line"));

  Config missing;
  CHECK(!Config::load("does-not-exist.json", &missing, &error));
  CHECK(!error.empty());
  std::remove(path.c_str());
}

TEST_CASE(config_parse_line_document) {
  const json document{
      {"id", 3},
      {"name", "Camera 3"},
      {"enabled", false},
      {"gain_db", 2.0},
      {"aes67", {{"sink_id", 3}, {"source_id", 3}, {"channels", {6}}}},
      {"sip", {{"extension", "1003"}, {"call_mode", "dial_out"}}}};
  const LineConfig line = Config::parse_line(document);
  CHECK_EQ(line.id, 3);
  CHECK_EQ(line.name, std::string("Camera 3"));
  CHECK(!line.enabled);
  CHECK_EQ(line.aes67.channels, std::vector<unsigned>({6}));
  CHECK_EQ(line.sip.extension, std::string("1003"));
  CHECK_EQ(line.sip.call_mode, std::string("dial_out"));
}

TEST_CASE(config_endpoint_streams_round_trip) {
  const json document{
      {"endpoints", json::array({json{{"id", "pack-01"},
                                      {"name", "Camera 1"},
                                      {"talk_channels", {0, 1}},
                                      {"listen_channels", {2, 3}},
                                      {"aes67",
                                       {{"stream_name", "Camera 1 feed"},
                                        {"auto_create_streams", false},
                                        {"codec", "L16"},
                                        {"remote_source_id", "SAP:abc"},
                                        {"remote_sdp", "v=0"},
                                        {"ignore_refclk_gmid", true},
                                        {"refclk_ptp_traceable", true}}}}})}};
  const Config config = Config::from_json(document);
  CHECK_EQ(config.endpoints.size(), 1U);
  const EndpointConfig& endpoint = config.endpoints[0];
  CHECK_EQ(endpoint.id, std::string("pack-01"));
  CHECK_EQ(endpoint.name, std::string("Camera 1"));
  CHECK_EQ(endpoint.talk_channels, std::vector<unsigned>({0, 1}));
  CHECK_EQ(endpoint.listen_channels, std::vector<unsigned>({2, 3}));
  CHECK_EQ(endpoint.aes67.stream_name, std::string("Camera 1 feed"));
  CHECK(!endpoint.aes67.auto_create_streams);
  CHECK_EQ(endpoint.aes67.codec, std::string("L16"));
  CHECK_EQ(endpoint.aes67.remote_source_id, std::string("SAP:abc"));
  CHECK_EQ(endpoint.aes67.remote_sdp, std::string("v=0"));
  CHECK(endpoint.aes67.ignore_refclk_gmid);
  CHECK(endpoint.aes67.refclk_ptp_traceable);

  // and the whole block survives a save/load round trip
  const Config restored = Config::from_json(config.to_json());
  CHECK_EQ(restored.endpoints.size(), 1U);
  CHECK_EQ(restored.endpoints[0].aes67.stream_name, std::string("Camera 1 feed"));
  CHECK_EQ(restored.endpoints[0].aes67.codec, std::string("L16"));
  CHECK_EQ(restored.endpoints[0].listen_channels, std::vector<unsigned>({2, 3}));
  CHECK(restored.endpoints[0].aes67.refclk_ptp_traceable);

  // An endpoint that only declares an id is still named, because both of its
  // stream names are built from that name.
  const Config minimal = Config::from_json(
      json{{"endpoints", json::array({json{{"id", "pack-09"},
                                           {"talk_channels", {0}},
                                           {"listen_channels", {1}}}})}});
  CHECK_EQ(minimal.endpoints[0].name, std::string("pack-09"));
  CHECK_EQ(minimal.endpoints[0].aes67.stream_name, std::string("pack-09"));
  CHECK_EQ(minimal.endpoints[0].aes67.codec, std::string("L24"));
  CHECK(minimal.endpoints[0].aes67.auto_create_streams);
}

TEST_CASE(config_conference_block_round_trip) {
  // Absent means disabled, so a configuration that does not mention the leg keeps
  // the behaviour it had before the block existed.
  const Config defaults = Config::from_json(json::object());
  CHECK(!defaults.conference.enabled);
  CHECK_EQ(defaults.conference.display_name, std::string("Conference"));
  CHECK(defaults.conference.target.empty());

  const Config config = Config::from_json(
      json{{"conference", json{{"enabled", true},
                               {"account", "pbx"},
                               {"target", "sip:conf@pbx.example.com"},
                               {"display_name", "FreePBX room"}}}});
  CHECK(config.conference.enabled);
  CHECK_EQ(config.conference.account, std::string("pbx"));
  CHECK_EQ(config.conference.target, std::string("sip:conf@pbx.example.com"));
  CHECK_EQ(config.conference.display_name, std::string("FreePBX room"));

  const Config restored = Config::from_json(config.to_json());
  CHECK(restored.conference.enabled);
  CHECK_EQ(restored.conference.target, std::string("sip:conf@pbx.example.com"));
  CHECK_EQ(restored.conference.display_name, std::string("FreePBX room"));

  // A nameless leg is still named, the way a line's display name defaults.
  const Config unnamed = Config::from_json(json{
      {"conference",
       json{{"enabled", true}, {"target", "sip:conf@pbx"}, {"display_name", ""}}}});
  CHECK_EQ(unnamed.conference.display_name, std::string("Conference"));
}

namespace {
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
}  // namespace

TEST_CASE(util_db_conversions) {
  CHECK_NEAR(linear_to_dbfs(1.0), 0.0, 1e-9);
  CHECK_NEAR(linear_to_dbfs(0.5), -6.0206, 1e-3);
  CHECK(std::isinf(linear_to_dbfs(0.0)));
  CHECK_NEAR(db_to_linear(-6.0206), 0.5, 1e-3);
  CHECK_NEAR(db_to_linear(0.0), 1.0, 1e-12);

  // A held peak decays, but never below the silence floor: a meter that keeps
  // falling past it reads as -47796 dBFS after a quiet minute, which is a number
  // nobody can act on.
  CHECK_NEAR(hold_peak(-6.0, -3.0, 0.5), -3.0, 1e-9);  // a louder reading wins
  CHECK_NEAR(hold_peak(-6.0, -9.0, 0.5), -6.5, 1e-9);  // otherwise it decays
  CHECK_NEAR(hold_peak(kSilenceDbfs, kNegInf, 0.5), kSilenceDbfs, 1e-9);

  // Silence serialises as null however it got there, which is what the API
  // documents and what the UI renders as "-inf".
  CHECK(dbfs_to_json(kNegInf).is_null());
  CHECK(dbfs_to_json(kSilenceDbfs).is_null());
  CHECK(dbfs_to_json(-59.94) == json(-59.9));
}

TEST_CASE(util_formatting_and_parsing) {
  CHECK_EQ(format_duration(0), std::string("00:00"));
  CHECK_EQ(format_duration(61), std::string("01:01"));
  CHECK_EQ(format_duration(3661), std::string("01:01:01"));
  CHECK_EQ(trim("  hi \n"), std::string("hi"));
  CHECK_EQ(split("a,b,,c", ',').size(), 4U);
  CHECK_EQ(join({"a", "b"}, "-"), std::string("a-b"));
  CHECK_EQ(to_lower("G722/16000/1"), std::string("g722/16000/1"));
  CHECK_EQ(*parse_double(" -12.5 "), -12.5);
  CHECK(!parse_double("abc").has_value());
  CHECK_EQ(json_get<int>(json::object(), "missing", 7), 7);
  CHECK_EQ(json_get<std::string>(json{{"a", "b"}}, "a", "x"), std::string("b"));
}
