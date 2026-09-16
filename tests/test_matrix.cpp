// The intercom matrix: party lines mixed on the appliance.
//
// These tests drive the matrix with synthetic buffers, so the mixing maths is
// verified without an audio device, a daemon or a SIP engine.  Each endpoint
// declares its own shape, which is what keeps the tests honest about the model
// being endpoint-agnostic rather than shaped like one vendor's beltpack.

#include <string>
#include <vector>

#include "matrix/intercom_matrix.hpp"
#include "test_framework.hpp"

using namespace aes67sip;

namespace {

MatrixEndpointPlan endpoint(const std::string& id, std::vector<unsigned> talk,
                            std::vector<unsigned> listen) {
  MatrixEndpointPlan plan;
  plan.id = id;
  plan.name = id;
  plan.talk_channels = std::move(talk);
  plan.listen_channels = std::move(listen);
  return plan;
}

MatrixMemberPlan member(const std::string& endpoint_id, int talk, int listen,
                        double contribution_db = 0.0, bool mute = false) {
  MatrixMemberPlan plan;
  plan.endpoint_id = endpoint_id;
  plan.talk_channel = talk;
  plan.listen_channel = listen;
  plan.contribution_db = contribution_db;
  plan.mute = mute;
  return plan;
}

MatrixLinePlan line(const std::string& id, std::vector<MatrixMemberPlan> members) {
  MatrixLinePlan plan;
  plan.id = id;
  plan.name = id;
  plan.members = std::move(members);
  return plan;
}

/**
 * Two endpoints on one party line, each with a single talk and listen channel:
 * capture 0 / playback 0 for "a", capture 1 / playback 1 for "b".
 */
MatrixPlan two_endpoint_line() {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {1}, {1}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("b", 0, 0)}));
  return plan;
}

constexpr unsigned kChannels = 2;
constexpr unsigned kFrames = 8;

std::vector<float> capture_with(unsigned channel, float value) {
  std::vector<float> capture(kChannels * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * kChannels + channel] = value;
  }
  return capture;
}

}  // namespace

TEST_CASE(matrix_mixes_the_line_and_excludes_each_speaker) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(two_endpoint_line(), &error));

  std::vector<float> capture = capture_with(0, 0.5F);
  std::vector<float> playback(kChannels * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), kChannels, kFrames);

  // "b" hears "a" on its own listen channel...
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * kChannels + 1], 0.5, 1e-4);
  }
  // ...and "a" never hears itself.
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * kChannels + 0], 0.0, 1e-6);
  }
}

TEST_CASE(matrix_keeps_lines_separate) {
  MatrixPlan plan = two_endpoint_line();
  // A third endpoint on its own line: it must hear nothing of "pl1".
  plan.endpoints.push_back(endpoint("c", {2}, {2}));
  plan.lines.push_back(line("pl2", {member("c", 0, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture = capture_with(0, 0.5F);  // only "a" talks
  std::vector<float> playback(3 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 3, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 3 + 2], 0.0, 1e-6);
  }
}

TEST_CASE(matrix_serves_differently_shaped_endpoints_through_the_same_core) {
  MatrixPlan plan;
  // A single pair endpoint and a two pair one: the matrix must not care which.
  plan.endpoints.push_back(endpoint("single", {0}, {0}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  // The dual endpoint joins on its *second* pair, so the shape — not a fixed
  // channel — decides where its audio comes from and goes to.
  plan.lines.push_back(line("pl1", {member("single", 0, 0), member("dual", 1, 1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(3 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 3 + 2] = 0.5F;  // the dual endpoint talks on its second channel
  }
  std::vector<float> playback(3 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 3, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 3 + 0], 0.5, 1e-4);  // the single endpoint hears it
    CHECK_NEAR(playback[frame * 3 + 2], 0.0, 1e-6);  // the dual one hears no echo
  }
}

TEST_CASE(matrix_status_reports_membership_levels_and_arrival) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(two_endpoint_line(), &error));

  // Before any audio: the line and its members exist, and nobody is arriving.
  const auto initial = matrix.status();
  CHECK_EQ(initial.size(), 1U);
  CHECK_EQ(initial[0].id, std::string("pl1"));
  CHECK_EQ(initial[0].members.size(), 2U);
  CHECK_EQ(initial[0].members[0].endpoint, std::string("a"));
  CHECK_EQ(initial[0].members[1].endpoint, std::string("b"));
  CHECK_EQ(initial[0].members[0].arriving, false);

  std::vector<float> capture = capture_with(0, 0.5F);  // only "a" talks
  std::vector<float> playback(2 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  const auto after = matrix.status();
  CHECK_EQ(after[0].members[0].arriving, true);
  CHECK_NEAR(after[0].members[0].level_dbfs, -6.0,
             1.0);  // half scale is about -6 dBFS
  CHECK_EQ(after[0].members[1].arriving, false);
}

TEST_CASE(matrix_without_lines_mixes_nothing) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(MatrixPlan{}, &error));
  CHECK(matrix.empty());
  CHECK_EQ(matrix.status().size(), 0U);

  std::vector<float> capture = capture_with(0, 0.5F);
  std::vector<float> playback(2 * kFrames, 0.25F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  // No routing: the matrix must not touch the device buffers at all.
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.25, 1e-6);
    CHECK_NEAR(playback[frame * 2 + 1], 0.25, 1e-6);
  }
}

TEST_CASE(matrix_drives_an_unrouted_listen_channel_silent) {
  MatrixPlan plan;
  // "unrouted" is declared with a listen channel but is on no party line at all:
  // the matrix owns the channel it was given and must drive it silent rather than
  // leave whatever the device buffer held.
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("unrouted", {1}, {1}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(2 * kFrames, 0.0F);
  std::vector<float> playback(2 * kFrames, 1.0F);  // stale audio from before
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.0, 1e-6);  // "a" hears nobody
    CHECK_NEAR(playback[frame * 2 + 1], 0.0,
               1e-6);  // the unrouted one hears nothing
  }
}

TEST_CASE(matrix_mutes_a_contribution_without_changing_what_that_member_hears) {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {1}, {1}));
  // "b" is muted: nobody hears it, but it still hears everybody else.
  plan.lines.push_back(
      line("pl1", {member("a", 0, 0), member("b", 0, 0, 0.0, true)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(2 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 2 + 0] = 0.5F;  // "a" talks
    capture[frame * 2 + 1] = 0.5F;  // "b" talks too, but is muted
  }
  std::vector<float> playback(2 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.0,
               1e-6);  // "a" does not hear the muted "b"
    CHECK_NEAR(playback[frame * 2 + 1], 0.5, 1e-4);  // "b" still hears "a"
  }
}

TEST_CASE(matrix_applies_the_contribution_level) {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {1}, {1}));
  // "b" contributes at -6 dB, so "a" hears it at roughly half the level.
  plan.lines.push_back(
      line("pl1", {member("a", 0, 0), member("b", 0, 0, -6.0206)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture = capture_with(1, 0.5F);  // "b" talks
  std::vector<float> playback(2 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.25, 1e-3);  // "a" hears "b" attenuated
    CHECK_NEAR(playback[frame * 2 + 1], 0.0,
               1e-6);  // "b" still hears nothing of itself
  }
}
