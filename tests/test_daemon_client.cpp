#include "aes67/daemon_client.hpp"
#include "test_framework.hpp"

using namespace aes67sip;

namespace {

Aes67DaemonConfig fake_daemon_config() {
  Aes67DaemonConfig config;
  config.fake = true;
  return config;
}

LineConfig make_line(int id, const std::vector<unsigned>& channels) {
  const json document{
      {"id", id},
      {"name", "Line " + std::to_string(id)},
      {"aes67", {{"sink_id", id}, {"source_id", id}, {"channels", channels}}},
      {"sip", {{"extension", "100" + std::to_string(id)}}}};
  return Config::parse_line(document);
}

}  // namespace

TEST_CASE(fake_daemon_reports_ptp_and_version) {
  auto daemon = DaemonClient::create(fake_daemon_config());
  CHECK(daemon != nullptr);
  CHECK(daemon->connected());

  std::string version;
  std::string error;
  CHECK(daemon->get_version(&version, &error));
  CHECK(!version.empty());

  PtpStatus ptp;
  CHECK(daemon->get_ptp_status(&ptp, &error));
  CHECK_EQ(ptp.status, std::string("locked"));
}

TEST_CASE(fake_daemon_stores_sinks_and_sources) {
  auto daemon = DaemonClient::create(fake_daemon_config());
  const LineConfig line = make_line(2, {4, 5});

  std::string error;
  const json source = DaemonClient::make_source(fake_daemon_config(), line);
  CHECK(daemon->put_source(line.aes67.source_id, source, &error));

  json sinks;
  json sources;
  CHECK(daemon->get_sources(&sources, &error));
  CHECK_EQ(sources.at("sources").size(), 1U);
  CHECK_EQ(sources.at("sources")[0].at("map"), json({4, 5}));

  json sink;
  CHECK(!DaemonClient::make_sink(fake_daemon_config(), line, "", &sink, &error));
  CHECK(!error.empty());  // an endpoint SDP is mandatory for a sink

  const std::string sdp =
      "v=0\ns=Endpoint\nm=audio 5004 RTP/AVP 98\na=rtpmap:98 L16/48000/2\n";
  CHECK(DaemonClient::make_sink(fake_daemon_config(), line, sdp, &sink, &error));
  CHECK(daemon->put_sink(line.aes67.sink_id, sink, &error));
  CHECK(daemon->get_sinks(&sinks, &error));
  CHECK_EQ(sinks.at("sinks").size(), 1U);
  CHECK_EQ(sinks.at("sinks")[0].at("sdp").get<std::string>(), sdp);

  SinkStatus status;
  CHECK(daemon->get_sink_status(line.aes67.sink_id, &status, &error));

  CHECK(daemon->delete_sink(line.aes67.sink_id, &error));
  CHECK(daemon->delete_source(line.aes67.source_id, &error));
  CHECK(daemon->get_sinks(&sinks, &error));
  CHECK_EQ(sinks.at("sinks").size(), 0U);
}

TEST_CASE(source_json_follows_the_daemon_schema) {
  const LineConfig line = make_line(1, {2});
  const json source = DaemonClient::make_source(fake_daemon_config(), line);
  // L24 is the default payload: Dante and most AES67 devices use it.
  CHECK_EQ(source.at("codec"), json("L24"));
  CHECK_EQ(source.at("map"), json({2}));
  CHECK_EQ(source.at("payload_type").get<int>(), 98);
  CHECK_EQ(source.at("ttl").get<int>(), 15);
  CHECK_EQ(source.at("name").get<std::string>(), std::string("Line 1"));
  CHECK(source.contains("enabled"));
  CHECK(!source.contains("map_extra"));

  // the payload is configurable per line (older gear still wants L16)
  LineConfig legacy = make_line(2, {3});
  legacy.aes67.codec = "L16";
  const json legacy_source = DaemonClient::make_source(fake_daemon_config(), legacy);
  CHECK_EQ(legacy_source.at("codec"), json("L16"));
}

TEST_CASE(daemon_browse_and_sdp_endpoints) {
  auto daemon = DaemonClient::create(fake_daemon_config());
  std::string error;
  json discovered;
  CHECK(daemon->browse_sources("all", &discovered, &error));
  CHECK(discovered.contains("remote_sources"));

  std::string sdp;
  CHECK(daemon->get_source_sdp(0, &sdp, &error));
  CHECK(sdp.find("m=audio") != std::string::npos);

  json config;
  CHECK(daemon->get_config(&config, &error));
  CHECK_EQ(config.at("sample_rate").get<int>(), 48000);
  CHECK(daemon->set_config(json{{"sample_rate", 48000}}, &error));
}
