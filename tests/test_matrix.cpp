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
