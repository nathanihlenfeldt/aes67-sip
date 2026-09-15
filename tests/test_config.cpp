#include <fstream>

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
                    {"lines", json::array({json{{"id", 0},
                                                {"name", "Line A"}}})}});

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
  const json document{{"id", 3},
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

TEST_CASE(util_db_conversions) {
  CHECK_NEAR(linear_to_dbfs(1.0), 0.0, 1e-9);
  CHECK_NEAR(linear_to_dbfs(0.5), -6.0206, 1e-3);
  CHECK(std::isinf(linear_to_dbfs(0.0)));
  CHECK_NEAR(db_to_linear(-6.0206), 0.5, 1e-3);
  CHECK_NEAR(db_to_linear(0.0), 1.0, 1e-12);
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
