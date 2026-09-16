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

MatrixLinePlan line(const std::string& id, std::vector<MatrixMemberPlan> members,
                    bool claims_conference = false) {
  MatrixLinePlan plan;
  plan.id = id;
  plan.name = id;
  plan.members = std::move(members);
  plan.claims_conference = claims_conference;
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

/**
 * Two lines, one of which claims the conference, with a member on each:
 * "a" on line 1 (capture 0 / playback 0), "b" on line 2 (capture 1 / playback 1).
 */
MatrixPlan conference_plan(bool line_one_claims, bool line_two_claims) {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {1}, {1}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0)}, line_one_claims));
  plan.lines.push_back(line("pl2", {member("b", 0, 0)}, line_two_claims));
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

TEST_CASE(matrix_feeds_the_conference_to_the_lines_that_claim_it) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(conference_plan(true, false), &error));

  // The conference says something and the site is silent.
  std::vector<float> conference(kFrames, 0.25F);
  matrix.push_conference_audio(conference.data(), conference.size());

  std::vector<float> capture(kChannels * kFrames, 0.0F);
  std::vector<float> playback(kChannels * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), kChannels, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * kChannels + 0], 0.25, 1e-4);  // line 1 claims it
    CHECK_NEAR(playback[frame * kChannels + 1], 0.0, 1e-6);   // line 2 does not
  }
}

TEST_CASE(matrix_sends_the_members_of_claiming_lines_to_the_conference) {
  // "a" sits on the claiming line and talks at -6 dB; "b" sits on a line that does
  // not claim the conference, so only "a" may reach it.
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {1}, {1}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0, -6.0206)}, true));
  plan.lines.push_back(line("pl2", {member("b", 0, 0)}, false));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(2 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 2 + 0] = 0.5F;  // "a"
    capture[frame * 2 + 1] = 0.5F;  // "b", which the conference must not hear
  }
  std::vector<float> playback(2 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  std::vector<float> to_conference(kFrames, 40.0F);
  CHECK_EQ(matrix.pull_conference_audio(to_conference.data(), to_conference.size()),
           static_cast<size_t>(kFrames));
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(to_conference[frame], 0.25, 1e-3);  // "a" at its level, and no "b"
  }
}

TEST_CASE(matrix_never_echoes_the_conference_to_itself) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(conference_plan(true, false), &error));

  // The conference talks and "a" talks: the conference hears only the site.
  std::vector<float> conference(kFrames, 0.5F);
  matrix.push_conference_audio(conference.data(), conference.size());
  std::vector<float> capture(2 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 2 + 0] = 0.5F;  // "a"
  }
  std::vector<float> playback(2 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  std::vector<float> to_conference(kFrames, 40.0F);
  matrix.pull_conference_audio(to_conference.data(), to_conference.size());
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(to_conference[frame], 0.5, 1e-4);  // "a" only, never its own 0.5
  }
}

TEST_CASE(matrix_ignores_the_conference_when_no_line_claims_it) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(conference_plan(false, false), &error));

  std::vector<float> conference(kFrames, 0.5F);
  matrix.push_conference_audio(conference.data(), conference.size());
  std::vector<float> capture(2 * kFrames, 0.0F);
  std::vector<float> playback(2 * kFrames, 1.0F);  // stale audio everywhere
  matrix.process(capture.data(), playback.data(), 2, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.0, 1e-6);  // nobody hears the conference
    CHECK_NEAR(playback[frame * 2 + 1], 0.0, 1e-6);
  }

  // Nothing is sent to the conference either: its leg reads silence.
  std::vector<float> to_conference(kFrames, 40.0F);
  CHECK_EQ(matrix.pull_conference_audio(to_conference.data(), to_conference.size()),
           0U);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(to_conference[frame], 0.0, 1e-6);
  }
}

TEST_CASE(matrix_mixes_the_conference_up_to_one_block_capacity) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(conference_plan(true, false), &error));

  // A block larger than the conference side's scratch is mixed up to that capacity
  // and no further: the bound is deliberate, and the rest stays queued rather than
  // being dropped.
  constexpr unsigned kBig = 5000;
  std::vector<float> conference(kBig, 0.25F);
  matrix.push_conference_audio(conference.data(), conference.size());

  std::vector<float> capture(2 * kBig, 0.0F);
  std::vector<float> playback(2 * kBig, 0.0F);
  matrix.process(capture.data(), playback.data(), 2, kBig);

  for (unsigned frame = 0; frame < 4096; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.25, 1e-4);
  }
  for (unsigned frame = 4096; frame < kBig; ++frame) {
    CHECK_NEAR(playback[frame * 2 + 0], 0.0, 1e-6);
  }
}

TEST_CASE(matrix_status_reports_which_lines_claim_the_conference) {
  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(conference_plan(true, false), &error));

  const auto status = matrix.status();
  CHECK_EQ(status.size(), 2U);
  CHECK_EQ(status[0].claims_conference, true);
  CHECK_EQ(status[1].claims_conference, false);
}

TEST_CASE(matrix_binds_each_channel_pair_independently) {
  MatrixPlan plan;
  // A two pair endpoint: pair 0 on one line, pair 1 on another, so it talks on
  // one line and hears two different mixes on its two listen channels.
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {3}, {3}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("dual", 0, 0)}));
  plan.lines.push_back(line("pl2", {member("b", 0, 0), member("dual", 1, 1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  // "a" talks: only the dual endpoint's pair 0 hears it, on its own channel.
  std::vector<float> capture(4 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 4 + 0] = 0.5F;
  }
  std::vector<float> playback(4 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 4, kFrames);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 4 + 1], 0.5, 1e-4);  // pair 0 hears line 1
    CHECK_NEAR(playback[frame * 4 + 2], 0.0, 1e-6);  // pair 1 hears nothing yet
  }

  // "b" talks: the same endpoint hears it on the *other* channel, and line 1's
  // mix has not leaked into it.
  std::vector<float> second(4 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    second[frame * 4 + 3] = 0.5F;
  }
  std::vector<float> playback2(4 * kFrames, 0.0F);
  matrix.process(second.data(), playback2.data(), 4, kFrames);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback2[frame * 4 + 2], 0.5, 1e-4);  // pair 1 hears line 2
    CHECK_NEAR(playback2[frame * 4 + 1], 0.0, 1e-6);  // pair 0 is silent
    CHECK_NEAR(playback2[frame * 4 + 0], 0.0, 1e-6);  // "a" does not hear "b"
  }
}

TEST_CASE(matrix_counts_an_endpoint_on_one_line_once_and_never_itself) {
  MatrixPlan plan;
  // The same endpoint joins the same line through both pairs: it must still hear
  // the other members once, and none of its own contributions.
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  plan.lines.push_back(
      line("pl1", {member("a", 0, 0), member("dual", 0, 0), member("dual", 1, 1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(3 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 3 + 0] = 0.5F;  // "a" talks
    capture[frame * 3 + 1] = 0.5F;  // the dual endpoint talks on pair 0
  }
  std::vector<float> playback(3 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 3, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 3 + 1], 0.5, 1e-4);  // pair 0 hears "a" once
    CHECK_NEAR(playback[frame * 3 + 2], 0.5, 1e-4);  // pair 1 hears "a" once too
  }
}

TEST_CASE(matrix_drives_an_unbound_listen_channel_of_a_bound_endpoint_silent) {
  MatrixPlan plan;
  // Only pair 0 is bound; pair 1's listen channel is declared but unbound, so it
  // must come out silent rather than replaying what the buffer held.
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("dual", 0, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(3 * kFrames, 0.0F);
  std::vector<float> playback(3 * kFrames, 1.0F);  // stale audio everywhere
  matrix.process(capture.data(), playback.data(), 3, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 3 + 1], 0.0, 1e-6);  // pair 0 hears nobody
    CHECK_NEAR(playback[frame * 3 + 2], 0.0, 1e-6);  // pair 1 is unbound: silence
  }
}

TEST_CASE(matrix_ignores_an_unbound_talk_channel) {
  MatrixPlan plan;
  // "listener" is declared with two talk channels but bound as listen-only: its
  // capture channels must contribute nothing to anybody.
  plan.endpoints.push_back(endpoint("a", {0}, {2}));
  plan.endpoints.push_back(endpoint("listener", {1, 3}, {1}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("listener", -1, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(4 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 4 + 0] = 0.5F;  // "a" talks
    capture[frame * 4 + 1] =
        1.0F;  // the listener's own capture channels carry audio
    capture[frame * 4 + 3] = 1.0F;
  }
  std::vector<float> playback(4 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 4, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 4 + 2], 0.0,
               1e-6);  // "a" does not hear the unbound talk
    CHECK_NEAR(playback[frame * 4 + 1], 0.5, 1e-4);  // the listener hears "a"
  }
}

TEST_CASE(matrix_binds_each_talk_channel_to_its_own_line) {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("b", {3}, {3}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("dual", 0, 0)}));
  plan.lines.push_back(line("pl2", {member("b", 0, 0), member("dual", 1, 1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  // The dual endpoint talks on pair 0: line 1 hears it, line 2 must not.
  std::vector<float> pair0(4 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    pair0[frame * 4 + 1] = 0.5F;
  }
  std::vector<float> playback(4 * kFrames, 0.0F);
  matrix.process(pair0.data(), playback.data(), 4, kFrames);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 4 + 0], 0.5, 1e-4);  // "a" on line 1 hears it
    CHECK_NEAR(playback[frame * 4 + 3], 0.0, 1e-6);  // "b" on line 2 does not
  }

  // And on pair 1: line 2 hears it, line 1 must not.
  std::vector<float> pair1(4 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    pair1[frame * 4 + 2] = 0.5F;
  }
  std::vector<float> playback2(4 * kFrames, 0.0F);
  matrix.process(pair1.data(), playback2.data(), 4, kFrames);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback2[frame * 4 + 3], 0.5, 1e-4);  // "b" on line 2 hears it
    CHECK_NEAR(playback2[frame * 4 + 0], 0.0, 1e-6);  // "a" on line 1 does not
  }
}

TEST_CASE(matrix_counts_a_member_bound_twice_on_one_line_once) {
  MatrixPlan plan;
  // One microphone bound to the same line through two listen channels: the other
  // members must hear it once, and it must hear the line on both its channels.
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("dual", {1}, {1, 2}));
  plan.lines.push_back(
      line("pl1", {member("a", 0, 0), member("dual", 0, 0), member("dual", 0, 1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(3 * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * 3 + 1] = 0.5F;  // the dual endpoint talks
  }
  std::vector<float> playback(3 * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), 3, kFrames);

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * 3 + 0], 0.5, 1e-4);  // "a" hears it once, not twice
    CHECK_NEAR(playback[frame * 3 + 1], 0.0, 1e-6);  // pair 0 hears no echo
    CHECK_NEAR(playback[frame * 3 + 2], 0.0, 1e-6);  // pair 1 hears no echo either
  }
}

TEST_CASE(matrix_status_reports_per_channel_bindings) {
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {0}));
  plan.endpoints.push_back(endpoint("dual", {1, 2}, {1, 2}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("dual", 0, 0)}));
  plan.lines.push_back(line("pl2", {member("dual", 1, 1)}));
  plan.lines.push_back(line("pl3", {member("a", -1, -1)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  const auto status = matrix.status();
  CHECK_EQ(status.size(), 3U);

  // Members carry the channels they bound, not just the endpoint that owns them.
  CHECK_EQ(status[0].members[0].endpoint, std::string("a"));
  CHECK_EQ(status[0].members[0].talk_channel, 0);
  CHECK_EQ(status[0].members[0].listen_channel, 0);
  CHECK_EQ(status[0].members[1].endpoint, std::string("dual"));
  CHECK_EQ(status[0].members[1].talk_channel, 0);
  CHECK_EQ(status[0].members[1].listen_channel, 0);

  // The same endpoint appears on the other line through its second pair.
  CHECK_EQ(status[1].members[0].endpoint, std::string("dual"));
  CHECK_EQ(status[1].members[0].talk_channel, 1);
  CHECK_EQ(status[1].members[0].listen_channel, 1);

  // An endpoint bound to neither direction reports both channels as absent.
  CHECK_EQ(status[2].members[0].talk_channel, -1);
  CHECK_EQ(status[2].members[0].listen_channel, -1);
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

// ---------------------------------------------------------------------------
// scale: eight party lines, 32 channels, every member in both directions
// ---------------------------------------------------------------------------

namespace {

/**
 * The reference site's shape, as configuration rather than as a plan: every
 * endpoint declares two talk and two listen channels, packed into the device
 * channels in order, and each of its two pairs is a member of a different party
 * line.  Sixteen endpoints therefore total 32 channels per direction, eight
 * lines have four members each, and every declared channel is live - which is
 * what the device has to be opened at, decided from these declarations.
 */
Config scale_config(unsigned endpoint_count) {
  Config config = Config::from_json(json::object());
  config.audio.channels = 2 * endpoint_count;
  for (unsigned i = 0; i < endpoint_count; ++i) {
    EndpointConfig endpoint;
    endpoint.id = "pack-" + std::to_string(i + 1);
    endpoint.name = "Beltpack " + std::to_string(i + 1);
    endpoint.talk_channels = {2 * i, 2 * i + 1};
    endpoint.listen_channels = {2 * i, 2 * i + 1};
    config.endpoints.push_back(endpoint);
  }
  const unsigned line_count = endpoint_count / 2;
  for (unsigned index = 0; index < line_count; ++index) {
    PartyLineConfig line;
    line.id = "pl" + std::to_string(index + 1);
    line.name = "PL " + std::to_string(index + 1);
    config.party_lines.push_back(line);
  }
  for (unsigned i = 0; i < endpoint_count; ++i) {
    for (unsigned pair = 0; pair < 2; ++pair) {
      const unsigned line_index = (i + pair) % line_count;
      PartyLineMemberConfig member;
      member.endpoint = "pack-" + std::to_string(i + 1);
      member.talk_channel = static_cast<int>(pair);
      member.listen_channel = static_cast<int>(pair);
      // Each line has its own contribution level, so a listen channel carrying
      // another line's mix cannot pass by accident.
      member.contribution_db = -3.0 * static_cast<double>(line_index);
      config.party_lines[line_index].members.push_back(member);
    }
  }
  return config;
}

const MatrixEndpointPlan* endpoint_plan(const MatrixPlan& plan,
                                        const std::string& id) {
  for (const auto& endpoint : plan.endpoints) {
    if (endpoint.id == id) {
      return &endpoint;
    }
  }
  return nullptr;
}

/** The device channel a member's listen channel maps onto. */
unsigned listen_device_channel(const MatrixPlan& plan,
                               const MatrixMemberPlan& member) {
  return endpoint_plan(plan, member.endpoint_id)
      ->listen_channels[static_cast<size_t>(member.listen_channel)];
}

}  // namespace

TEST_CASE(matrix_channel_use_follows_the_declared_shapes) {
  // Sixteen endpoints of two talk and two listen channels total 32 channels per
  // direction: the width the device needs is declared, never assumed.
  const Config full_config = scale_config(16);
  MatrixPlan full;
  std::string error;
  CHECK(IntercomMatrix::plan_from_config(full_config, &full, &error));
  const MatrixChannelUse full_use = IntercomMatrix::channel_use(full);
  CHECK_EQ(full_use.capture, 32U);
  CHECK_EQ(full_use.playback, 32U);
  CHECK_EQ(full_use.device_channels(), 32U);

  // Halving the endpoints halves the channels used - no fixed count anywhere.
  const Config half_config = scale_config(8);
  MatrixPlan half;
  CHECK(IntercomMatrix::plan_from_config(half_config, &half, &error));
  const MatrixChannelUse half_use = IntercomMatrix::channel_use(half);
  CHECK_EQ(half_use.capture, 16U);
  CHECK_EQ(half_use.playback, 16U);
  CHECK_EQ(half_use.device_channels(), 16U);

  // The highest declared index is what sets the width, in either direction.
  MatrixPlan sparse;
  sparse.endpoints.push_back(endpoint("single", {3}, {3}));
  sparse.endpoints.push_back(endpoint("console", {4, 5, 6, 7}, {4, 5, 6, 7}));
  const MatrixChannelUse sparse_use = IntercomMatrix::channel_use(sparse);
  CHECK_EQ(sparse_use.capture, 8U);
  CHECK_EQ(sparse_use.playback, 8U);
}

TEST_CASE(matrix_refuses_declared_channels_beyond_the_device) {
  // The shapes declare 32 channels per direction; the device opens 16.
  Config config = scale_config(16);
  config.audio.channels = 16;

  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  // both totals, in the roles they play
  CHECK(error.find("32 device channels") != std::string::npos);       // declared
  CHECK(error.find("audio.channels opens 16") != std::string::npos);  // available

  // The same configuration is accepted once the device is wide enough: the
  // refusal is about capacity, not about the configuration being wrong.
  config.audio.channels = 32;
  CHECK(IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK(error.empty());
}

TEST_CASE(matrix_mixes_eight_lines_across_every_endpoint) {
  constexpr unsigned kEndpoints = 16;
  constexpr unsigned kChannels = 32;
  constexpr unsigned kFrames = 8;

  Config config = scale_config(kEndpoints);
  MatrixPlan plan;
  std::string error;
  CHECK(IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK_EQ(plan.endpoints.size(), kEndpoints);
  CHECK_EQ(plan.lines.size(), 8U);

  IntercomMatrix matrix;
  CHECK(matrix.configure(plan, &error));

  // Every member's audio arrives at the appliance: all 32 talk channels carry
  // audio, one per member of the model.
  std::vector<float> capture(kChannels * kFrames, 0.0F);
  for (unsigned channel = 0; channel < kChannels; ++channel) {
    for (unsigned frame = 0; frame < kFrames; ++frame) {
      capture[frame * kChannels + channel] = 0.25F;
    }
  }
  std::vector<float> playback(kChannels * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), kChannels, kFrames);

  // Each listen channel is then every *other* member of the line it is bound to,
  // at that line's contribution level, and nothing else: a mix that included the
  // listening endpoint's own audio, or leaked another line's, would read
  // differently.  Both directions of every member are checked.
  unsigned checked = 0;
  for (size_t index = 0; index < plan.lines.size(); ++index) {
    const MatrixLinePlan& line = plan.lines[index];
    CHECK_EQ(line.members.size(),
             kChannels / static_cast<unsigned>(plan.lines.size()));
    const double expected = 0.25 * db_to_linear(-3.0 * static_cast<double>(index)) *
                            static_cast<double>(line.members.size() - 1);
    for (const auto& member : line.members) {
      const unsigned channel = listen_device_channel(plan, member);
      for (unsigned frame = 0; frame < kFrames; ++frame) {
        CHECK_NEAR(playback[frame * kChannels + channel], expected, 1e-3);
      }
      ++checked;
    }
  }
  CHECK_EQ(checked, kChannels);

  // The status view reports the same eight lines with every member heard from -
  // the contract the commissioning UI and these tests share.
  const std::vector<MatrixLineStatus> status = matrix.status();
  CHECK_EQ(status.size(), 8U);
  unsigned members = 0;
  for (const auto& line : status) {
    CHECK_EQ(line.members.size(), 4U);
    for (const auto& member : line.members) {
      ++members;
      CHECK(member.arriving);  // its audio reached the appliance
    }
  }
  CHECK_EQ(members, kChannels);
}

// ---------------------------------------------------------------------------
// validation: a configuration that could not work is refused as a whole
// ---------------------------------------------------------------------------

namespace {

/**
 * A configuration that works: two endpoints of two channels each, packed into
 * four device channels, both members of one party line.
 */
Config valid_matrix_config() {
  Config config = Config::from_json(json::object());
  config.audio.channels = 4;
  EndpointConfig first;
  first.id = "a";
  first.name = "Camera 1";
  first.talk_channels = {0, 1};
  first.listen_channels = {0, 1};
  EndpointConfig second;
  second.id = "b";
  second.name = "Camera 2";
  second.talk_channels = {2, 3};
  second.listen_channels = {2, 3};
  config.endpoints = {first, second};

  PartyLineConfig line;
  line.id = "cameras";
  line.name = "Cameras";
  PartyLineMemberConfig first_member;
  first_member.endpoint = "a";
  first_member.talk_channel = 0;
  first_member.listen_channel = 0;
  PartyLineMemberConfig second_member;
  second_member.endpoint = "b";
  second_member.talk_channel = 0;
  second_member.listen_channel = 0;
  line.members = {first_member, second_member};
  config.party_lines = {line};
  return config;
}

}  // namespace

TEST_CASE(matrix_refuses_two_party_lines_with_the_same_name) {
  Config config = valid_matrix_config();
  PartyLineConfig second = config.party_lines[0];
  second.id = "stage";  // a distinct id: the *name* is the duplicate here
  second.name = "Cameras";
  config.party_lines.push_back(second);

  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK(error.find("Cameras") != std::string::npos);

  // Case and stray spacing are how two "different" names get typed, so they do
  // not make a second line either.
  config.party_lines[1].name = " cameras ";
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK(error.find("cameras") != std::string::npos);
}

TEST_CASE(matrix_refuses_two_lines_or_endpoints_sharing_an_id) {
  // Two endpoints with one id: a binding names an endpoint by id, so the member
  // that says "a" would mean both of them.
  Config endpoints = valid_matrix_config();
  EndpointConfig duplicate = endpoints.endpoints[0];
  duplicate.name = "Camera 1 again";
  endpoints.endpoints.push_back(duplicate);
  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(endpoints, &plan, &error));
  CHECK(error.find("endpoints") != std::string::npos);
  CHECK(error.find("'a'") != std::string::npos);

  // Two party lines with one id: the status view keys a line by it.
  Config lines = valid_matrix_config();
  lines.party_lines.push_back(lines.party_lines[0]);
  CHECK(!IntercomMatrix::plan_from_config(lines, &plan, &error));
  CHECK(error.find("party lines") != std::string::npos);
  CHECK(error.find("cameras") != std::string::npos);
}

TEST_CASE(matrix_refuses_a_device_channel_two_endpoints_claim) {
  Config config = valid_matrix_config();
  // "b" now talks on capture channel 1, which "a" already talks on.
  config.endpoints[1].talk_channels = {1, 3};

  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK(error.find("device channel 1") != std::string::npos);
  CHECK(error.find("Camera 1") != std::string::npos);  // first claimant
  CHECK(error.find("Camera 2") != std::string::npos);  // and the second
  // The claimants are named as the configuration writes them, so the two numbers
  // in the sentence (device channel, array position) cannot be confused.
  CHECK(error.find("talk_channels[0]") != std::string::npos);

  // The listen direction is checked too, and named as the listen channel it is.
  Config listen = valid_matrix_config();
  listen.endpoints[1].listen_channels = {1, 3};
  CHECK(!IntercomMatrix::plan_from_config(listen, &plan, &error));
  CHECK(error.find("device channel 1") != std::string::npos);
  CHECK(error.find("listen_channels[0]") != std::string::npos);

  // Capture and playback are separate directions of the device, so a channel may
  // be one endpoint's talk channel and another's listen channel: that is how the
  // reference site packs sixteen beltpacks into 32 channels.
  Config crossed = valid_matrix_config();
  crossed.endpoints[0].listen_channels = {2, 3};
  crossed.endpoints[1].listen_channels = {0, 1};
  CHECK(IntercomMatrix::plan_from_config(crossed, &plan, &error));
  CHECK(error.empty());
}

TEST_CASE(matrix_refuses_a_member_channel_outside_its_endpoints_shape) {
  // "a" declares two talk channels, so talk_channel 2 does not exist.
  Config config = valid_matrix_config();
  config.party_lines[0].members[0].talk_channel = 2;
  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  // the whole refusal, so a passing check cannot be an unrelated member failure
  CHECK(error.find(
            "party line 'cameras' member 1 binds talk_channel 2 of endpoint 'a' "
            "(Camera 1), which declares 2 talk channel(s)") != std::string::npos);

  Config listen = valid_matrix_config();
  listen.party_lines[0].members[1].listen_channel = 2;
  CHECK(!IntercomMatrix::plan_from_config(listen, &plan, &error));
  CHECK(
      error.find("party line 'cameras' member 2 binds listen_channel 2 of endpoint "
                 "'b' (Camera 2), which declares 2 listen channel(s)") !=
      std::string::npos);
}

TEST_CASE(matrix_refuses_a_binding_to_an_endpoint_that_does_not_exist) {
  Config config = valid_matrix_config();
  config.party_lines[0].members[1].endpoint = "ghost";

  MatrixPlan plan;
  std::string error;
  CHECK(!IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK(error.find("party line 'cameras' member 2 binds endpoint 'ghost', which no "
                   "endpoint declares") != std::string::npos);

  // The same refusal protects the routing itself: a plan built in code is checked
  // where it is indexed, not trusted.
  MatrixPlan hand_built;
  hand_built.endpoints.push_back(endpoint("a", {0}, {0}));
  hand_built.lines.push_back(line("pl1", {member("ghost", 0, 0)}));
  CHECK(!IntercomMatrix::validate_plan(hand_built, &error));
  CHECK(error.find("ghost") != std::string::npos);
  IntercomMatrix matrix;
  CHECK(!matrix.configure(hand_built, &error));
  CHECK(matrix.empty());

  MatrixPlan too_far;
  too_far.endpoints.push_back(endpoint("a", {0}, {0}));
  too_far.lines.push_back(line("pl1", {member("a", 1, 0)}));
  CHECK(!IntercomMatrix::validate_plan(too_far, &error));
  CHECK(error.find("talk_channel 1") != std::string::npos);
}

TEST_CASE(matrix_refuses_a_conference_leg_that_could_not_be_dialled) {
  Config config = valid_matrix_config();
  config.conference.enabled = true;

  std::string error;
  // Enabled with nowhere to dial: refused where the configuration is applied.
  CHECK(!validate_configuration(config, nullptr, &error));
  CHECK(error.find("conference leg is enabled but has no target") !=
        std::string::npos);

  config.conference.target = "sip:conf@pbx.example.com";
  CHECK(validate_configuration(config, nullptr, &error));
  CHECK(error.empty());

  // ...and with an account nobody declares, which would register nowhere.
  config.conference.account = "ghost";
  CHECK(!validate_configuration(config, nullptr, &error));
  CHECK(error.find("account 'ghost'") != std::string::npos);

  // The reserved id belongs to the leg: a configured line may not take it.
  config.conference.enabled = false;
  config.lines[0].id = kConferenceLineId;
  CHECK(!validate_configuration(config, nullptr, &error));
  CHECK(error.find("reserved for the conference leg") != std::string::npos);
}

TEST_CASE(matrix_accepts_a_valid_configuration_unchanged) {
  Config config = valid_matrix_config();
  config.party_lines[0].claims_conference = true;

  std::string error;
  CHECK(validate_configuration(config, nullptr, &error));
  CHECK(error.empty());

  // "Loads unchanged" means what was declared is what the plan carries.
  MatrixPlan plan;
  CHECK(IntercomMatrix::plan_from_config(config, &plan, &error));
  CHECK_EQ(plan.endpoints.size(), 2U);
  CHECK_EQ(plan.endpoints[0].id, std::string("a"));
  CHECK_EQ(plan.endpoints[1].talk_channels, std::vector<unsigned>({2, 3}));
  CHECK_EQ(plan.lines.size(), 1U);
  CHECK_EQ(plan.lines[0].id, std::string("cameras"));
  CHECK(plan.lines[0].claims_conference);
  CHECK_EQ(plan.lines[0].members.size(), 2U);
  CHECK_EQ(plan.lines[0].members[1].endpoint_id, std::string("b"));
  CHECK_EQ(plan.lines[0].members[1].talk_channel, 0);
  CHECK_EQ(plan.lines[0].members[1].listen_channel, 0);

  // A configuration with no party lines at all is valid, as it always was,
  // including one that declares endpoints nobody has routed yet.
  Config none = Config::from_json(json::object());
  none.endpoints = config.endpoints;
  CHECK(validate_configuration(none, nullptr, &error));
  CHECK(error.empty());
  MatrixPlan empty_plan;
  CHECK(IntercomMatrix::plan_from_config(none, &empty_plan, &error));
  CHECK(empty_plan.empty());
}

TEST_CASE(matrix_summary_names_who_is_arriving_silent_and_unbound) {
  // One line, three members: one talking, one bound but quiet, one with no talk
  // channel bound at all.  The three are different things and the summary says so.
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("talker", {0}, {0}));
  plan.endpoints.push_back(endpoint("quiet", {1}, {1}));
  plan.endpoints.push_back(endpoint("listener", {2}, {2}));
  plan.lines.push_back(line("pl1", {member("talker", 0, 0), member("quiet", 0, 0),
                                    member("listener", -1, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  const std::vector<float> capture(kChannels * kFrames, 0.0F);
  std::vector<float> capture_block = capture;
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture_block[frame * kChannels + 0] = 0.5F;  // only "talker" talks
  }
  std::vector<float> playback(kChannels * kFrames, 0.0F);
  matrix.process(capture_block.data(), playback.data(), kChannels, kFrames);

  MatrixLineStatus status = matrix.status()[0];
  CHECK_EQ(status.summary.members, 3U);
  CHECK_EQ(status.summary.arriving, 1U);
  CHECK_EQ(status.summary.silent, 1U);
  CHECK_EQ(status.summary.unbound, 1U);
  // Named by their endpoint names, which is what a person searches for.
  CHECK_EQ(status.summary.arriving_names, std::vector<std::string>({"talker"}));
  CHECK_EQ(status.summary.silent_names, std::vector<std::string>({"quiet"}));
  CHECK_EQ(status.summary.unbound_names, std::vector<std::string>({"listener"}));
  CHECK(status.summary.can_be_heard());  // two members can talk
  CHECK(!status.summary.quiet());        // and one of them is talking
  CHECK_EQ(std::string(status.summary.state()), std::string("active"));

  // A member that stops talking is reported within a bounded time: the meter holds
  // its peak and decays 0.5 dB per millisecond, so an 8-frame block decays about
  // 0.083 dB.  A 0.5 peak sits at -6 dBFS and the arrival threshold is -60, so 54
  // dB takes about 650 blocks - a little over 100 ms of audio.  Asserted in blocks,
  // never in wall clock, so the bound is exact.
  std::vector<float> quiet_playback(kChannels * kFrames, 0.0F);
  for (unsigned block = 0; block < 700; ++block) {
    matrix.process(capture.data(), quiet_playback.data(), kChannels, kFrames);
  }
  status = matrix.status()[0];
  CHECK_EQ(status.summary.arriving, 0U);
  CHECK_EQ(status.summary.silent, 2U);
  CHECK_EQ(status.summary.silent_names,
           std::vector<std::string>({"talker", "quiet"}));
  // Two members can still talk, so a silent line is not a broken one: it is
  // quiet, and says so rather than claiming health or failure.
  CHECK(status.summary.can_be_heard());
  CHECK(status.summary.quiet());
  CHECK_EQ(std::string(status.summary.state()), std::string("quiet"));

  // A line nobody can talk on is the broken case, and it is a configuration fact
  // rather than silence.
  MatrixPlan mute_only;
  mute_only.endpoints.push_back(endpoint("listener", {0}, {0}));
  mute_only.lines.push_back(line("pl2", {member("listener", -1, 0)}));
  IntercomMatrix listeners;
  CHECK(listeners.configure(mute_only, &error));
  const MatrixLineStatus listen_only = listeners.status()[0];
  CHECK_EQ(listen_only.summary.members, 1U);
  CHECK_EQ(listen_only.summary.unbound, 1U);
  CHECK(!listen_only.summary.can_be_heard());
  CHECK(!listen_only.summary.quiet());
  CHECK_EQ(std::string(listen_only.summary.state()),
           std::string("cannot_be_heard"));
}

TEST_CASE(matrix_line_summary_reports_a_dead_member_without_taking_the_line_down) {
  // "a" and "b" share a line; only "a" is heard from.  The line keeps mixing - "b"
  // still hears "a" - and the summary says who has gone quiet.
  MatrixPlan plan;
  plan.endpoints.push_back(endpoint("a", {0}, {1}));
  plan.endpoints.push_back(endpoint("b", {1}, {0}));
  plan.lines.push_back(line("pl1", {member("a", 0, 0), member("b", 0, 0)}));

  IntercomMatrix matrix;
  std::string error;
  CHECK(matrix.configure(plan, &error));

  std::vector<float> capture(kChannels * kFrames, 0.0F);
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    capture[frame * kChannels + 0] = 0.5F;  // "a" alone
  }
  std::vector<float> playback(kChannels * kFrames, 0.0F);
  matrix.process(capture.data(), playback.data(), kChannels, kFrames);

  const MatrixLineStatus status = matrix.status()[0];
  CHECK_EQ(status.summary.members, 2U);
  CHECK_EQ(status.summary.arriving, 1U);
  CHECK_EQ(status.summary.arriving_names, std::vector<std::string>({"a"}));
  CHECK_EQ(status.summary.silent, 1U);
  CHECK_EQ(status.summary.silent_names, std::vector<std::string>({"b"}));
  CHECK(status.summary.can_be_heard());
  CHECK(!status.summary.quiet());

  // The dead member did not take the line down: "b" hears "a" on the channel it
  // listens on.
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    CHECK_NEAR(playback[frame * kChannels + 0], 0.5, 1e-4);
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
