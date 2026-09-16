// The intercom matrix: party lines mixed on the appliance.
//
// Everything here works on device channels and buffers, so it is testable
// without an audio device: feed it a capture block, read the playback block.

#include "matrix/intercom_matrix.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

/** A member counts as arriving while its held peak is above this. */
constexpr double kArrivalThresholdDbfs = -60.0;

/**
 * How a claimant of a device channel is described in a refusal.  The index is
 * quoted as the configuration writes it (`talk_channels[0]`), so it cannot be
 * misread as the device channel named in the same sentence.
 */
std::string describe_claimant(const MatrixEndpointPlan& endpoint, bool talk_channel,
                              size_t index) {
  return "endpoint '" + endpoint.id + "' (" + endpoint.name + ") " +
         (talk_channel ? "talk_channels" : "listen_channels") + "[" +
         std::to_string(index) + "]";
}

/** "party line 'pl1' member 2" - where the binding a refusal is about sits. */
std::string describe_binding(const MatrixLinePlan& line, size_t index) {
  return "party line '" + line.id + "' member " + std::to_string(index + 1);
}

/**
 * Refuses two `subject` sharing an id.  Ids are the keys everything else uses -
 * member bindings name endpoints by id, the status view keys lines by id - so a
 * duplicate makes a configuration ambiguous rather than merely untidy.
 *
 * The wording of every refusal here and in the helpers below is a contract with
 * the web UI's commissioning page, which shows a refusal against the entry it
 * names (`webui/src/commissioning.js`, `locateRefusal`): changing a message means
 * changing that matcher, and the tests on both sides pin it.
 */
bool refuse_duplicate_ids(const std::vector<std::string>& ids,
                          const std::string& subject, std::string* error) {
  std::set<std::string> seen;
  for (const auto& id : ids) {
    if (!seen.insert(id).second) {
      if (error != nullptr) {
        *error = "two " + subject + " share the id '" + id +
                 "' - bindings and the status view name them by id, so it has to "
                 "be unique";
      }
      return false;
    }
  }
  return true;
}

/**
 * Refuses two party lines that are told apart only by case or spacing: their
 * names are labels a human reads in the commissioning view and matches in a
 * vendor's routing grid, so "PL 1" and "pl 1" are one line, not two.
 */
bool refuse_duplicate_names(const MatrixPlan& plan, std::string* error) {
  std::set<std::string> names;
  for (const auto& line : plan.lines) {
    if (!names.insert(to_lower(trim(line.name))).second) {
      if (error != nullptr) {
        *error = "two party lines are called '" + line.name +
                 "' - line names are how a person tells them apart, so they have "
                 "to differ";
      }
      return false;
    }
  }
  return true;
}

/**
 * Refuses a device channel two endpoints claim in the same direction.  Capture
 * and playback are separate directions on the device, so a channel may well be
 * one endpoint's talk channel and another's listen channel - that is how the
 * reference site packs sixteen beltpacks into 32 channels - but within one
 * direction a channel has exactly one claimant.
 */
bool refuse_claimed_twice(const MatrixPlan& plan, std::string* error) {
  for (const bool talk_channel : {true, false}) {
    std::map<unsigned, std::string> claimants;
    for (const auto& endpoint : plan.endpoints) {
      const std::vector<unsigned>& channels =
          talk_channel ? endpoint.talk_channels : endpoint.listen_channels;
      for (size_t index = 0; index < channels.size(); ++index) {
        const auto it = claimants.find(channels[index]);
        const std::string who = describe_claimant(endpoint, talk_channel, index);
        if (it != claimants.end()) {
          if (error != nullptr) {
            *error = "device channel " + std::to_string(channels[index]) +
                     " is claimed twice: " + it->second + " and " + who;
          }
          return false;
        }
        claimants[channels[index]] = who;
      }
    }
  }
  return true;
}

}  // namespace

bool validate_configuration(const Config& config, std::string* error) {
  MatrixPlan plan;
  return IntercomMatrix::plan_from_config(config, &plan, error);
}

bool IntercomMatrix::validate_plan(const MatrixPlan& plan, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  for (const auto& line : plan.lines) {
    for (size_t index = 0; index < line.members.size(); ++index) {
      const MatrixMemberPlan& member = line.members[index];
      const MatrixEndpointPlan* endpoint = nullptr;
      for (const auto& candidate : plan.endpoints) {
        if (candidate.id == member.endpoint_id) {
          endpoint = &candidate;
          break;
        }
      }
      if (endpoint == nullptr) {
        if (error != nullptr) {
          *error = describe_binding(line, index) + " binds endpoint '" +
                   member.endpoint_id + "', which no endpoint declares";
        }
        return false;
      }
      // A channel is an index into the endpoint's *own* declared channels, so a
      // member may leave a direction unbound (-1) but may not reach past what its
      // endpoint carries.
      if (member.talk_channel >= 0 && static_cast<size_t>(member.talk_channel) >=
                                          endpoint->talk_channels.size()) {
        if (error != nullptr) {
          *error = describe_binding(line, index) + " binds talk_channel " +
                   std::to_string(member.talk_channel) + " of endpoint '" +
                   endpoint->id + "' (" + endpoint->name + "), which declares " +
                   std::to_string(endpoint->talk_channels.size()) +
                   " talk channel(s)";
        }
        return false;
      }
      if (member.listen_channel >= 0 &&
          static_cast<size_t>(member.listen_channel) >=
              endpoint->listen_channels.size()) {
        if (error != nullptr) {
          *error = describe_binding(line, index) + " binds listen_channel " +
                   std::to_string(member.listen_channel) + " of endpoint '" +
                   endpoint->id + "' (" + endpoint->name + "), which declares " +
                   std::to_string(endpoint->listen_channels.size()) +
                   " listen channel(s)";
        }
        return false;
      }
    }
  }
  return true;
}

IntercomMatrix::IntercomMatrix(unsigned sample_rate)
    : sample_rate_(sample_rate == 0 ? 48000 : sample_rate),
      conference_incoming_block_(kMaxConferenceBlockFrames, 0.0F),
      conference_outgoing_block_(kMaxConferenceBlockFrames, 0.0F) {}

IntercomMatrix::~IntercomMatrix() = default;

unsigned MatrixChannelUse::device_channels() const {
  return std::max(capture, playback);
}

MatrixChannelUse IntercomMatrix::channel_use(const MatrixPlan& plan) {
  MatrixChannelUse use;
  for (const auto& endpoint : plan.endpoints) {
    for (const unsigned channel : endpoint.talk_channels) {
      use.capture = std::max(use.capture, channel + 1);
    }
    for (const unsigned channel : endpoint.listen_channels) {
      use.playback = std::max(use.playback, channel + 1);
    }
  }
  return use;
}

bool IntercomMatrix::plan_from_config(const Config& config, MatrixPlan* plan,
                                      std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (plan == nullptr) {
    if (error != nullptr) {
      *error = "internal error: matrix plan pointer is null";
    }
    return false;
  }
  MatrixPlan resolved;
  for (const auto& endpoint : config.endpoints) {
    MatrixEndpointPlan planned;
    planned.id = endpoint.id;
    planned.name = endpoint.name.empty() ? endpoint.id : endpoint.name;
    planned.talk_channels = endpoint.talk_channels;
    planned.listen_channels = endpoint.listen_channels;
    resolved.endpoints.push_back(std::move(planned));
  }
  std::vector<std::string> endpoint_ids;
  for (const auto& endpoint : resolved.endpoints) {
    endpoint_ids.push_back(endpoint.id);
  }
  if (!refuse_duplicate_ids(endpoint_ids, "endpoints", error)) {
    return false;
  }

  for (const auto& line : config.party_lines) {
    MatrixLinePlan planned;
    planned.id = line.id;
    planned.name = line.name.empty() ? line.id : line.name;
    planned.claims_conference = line.claims_conference;
    for (const auto& member : line.members) {
      MatrixMemberPlan planned_member;
      planned_member.endpoint_id = member.endpoint;
      planned_member.talk_channel = member.talk_channel;
      planned_member.listen_channel = member.listen_channel;
      planned_member.contribution_db = member.contribution_db;
      planned_member.mute = member.mute;
      planned.members.push_back(std::move(planned_member));
    }
    resolved.lines.push_back(std::move(planned));
  }
  std::vector<std::string> line_ids;
  for (const auto& line : resolved.lines) {
    line_ids.push_back(line.id);
  }
  if (!refuse_duplicate_ids(line_ids, "party lines", error) ||
      !refuse_duplicate_names(resolved, error)) {
    return false;
  }

  // The bindings, before anything is applied: a member that names an endpoint
  // nobody declared, or a channel its endpoint does not carry, is exactly the
  // typo this refusal exists for.
  if (!validate_plan(resolved, error) || !refuse_claimed_twice(resolved, error)) {
    return false;
  }

  // The declared shapes decide how wide the device has to be.  Refusing here -
  // with both totals - is what stops a configuration that asks for more channels
  // than the device carries from being routed down to the ones that happen to
  // fit, which would leave the rest of the site silently unheard.
  const MatrixChannelUse channels_used = channel_use(resolved);
  const unsigned available = config.audio.channels;
  if (channels_used.device_channels() > available) {
    if (error != nullptr) {
      std::ostringstream message;
      message << "the declared endpoint shapes need "
              << channels_used.device_channels() << " device channels ("
              << channels_used.capture << " capture, " << channels_used.playback
              << " playback) but audio.channels opens " << available
              << " - raise audio.channels or declare fewer channels";
      *error = message.str();
    }
    return false;
  }

  *plan = std::move(resolved);
  return true;
}

bool IntercomMatrix::configure(const MatrixPlan& plan, std::string* error) {
  if (plan.lines.empty()) {
    // No party lines means no routing: nothing is mixed, nothing is owned, and
    // the device buffers are left exactly as they were.  That is also how a site
    // that has never used the matrix behaves.
    clear();
    LOG_INFO("matrix: no party lines configured");
    return true;
  }

  // A plan built in code reaches here without going through `plan_from_config`,
  // so the bindings are checked again rather than indexed on trust.
  if (!validate_plan(plan, error)) {
    return false;
  }

  auto runtime = std::make_shared<Runtime>();

  // Every declared listen channel belongs to the matrix, whether or not a party
  // line routes to it: a channel nobody mixes is driven silent rather than left
  // holding whatever the device had in it.  Declaring the shape is what claims the
  // channels.
  for (const auto& endpoint : plan.endpoints) {
    for (const unsigned channel : endpoint.listen_channels) {
      if (std::find(runtime->owned_playback.begin(), runtime->owned_playback.end(),
                    channel) == runtime->owned_playback.end()) {
        runtime->owned_playback.push_back(channel);
      }
    }
  }

  for (const auto& line : plan.lines) {
    RuntimeLine resolved_line;
    resolved_line.id = line.id;
    resolved_line.name = line.name;
    resolved_line.claims_conference = line.claims_conference;
    resolved_line.members.reserve(line.members.size());
    for (const auto& member : line.members) {
      const MatrixEndpointPlan* endpoint = nullptr;
      for (const auto& candidate : plan.endpoints) {
        if (candidate.id == member.endpoint_id) {
          endpoint = &candidate;
          break;
        }
      }
      // `validate_plan` has already refused a plan without this endpoint, so this
      // is a guard rather than a check.
      if (endpoint == nullptr) {
        if (error != nullptr) {
          *error = "party line '" + line.id +
                   "' has a member for unknown endpoint '" + member.endpoint_id +
                   "'";
        }
        return false;
      }

      // Built in place: the level meter is an atomic, so the element is neither
      // copyable nor movable and the reserve() above keeps the buffer stable.
      resolved_line.members.emplace_back();
      RuntimeMember& resolved = resolved_line.members.back();
      resolved.endpoint_id = endpoint->id;
      resolved.endpoint_name = endpoint->name;
      resolved.contribution_db = member.contribution_db;
      resolved.gain = static_cast<float>(db_to_linear(member.contribution_db));
      resolved.mute = member.mute;
      if (member.talk_channel >= 0) {
        resolved.talk_channel = member.talk_channel;
        resolved.capture_channel =
            endpoint->talk_channels[static_cast<size_t>(member.talk_channel)];
      }
      if (member.listen_channel >= 0) {
        resolved.listen_channel = member.listen_channel;
        resolved.playback_channel =
            endpoint->listen_channels[static_cast<size_t>(member.listen_channel)];
      }

      // A line feeds each distinct listen channel once and receives each distinct
      // talk channel once, however many member entries happen to bind them: an
      // endpoint heard through the same channel twice would otherwise be summed
      // twice, which is a misconfiguration waiting to be measured as +6 dB.
      if (resolved.talk_channel >= 0 &&
          std::none_of(resolved_line.contributions.begin(),
                       resolved_line.contributions.end(),
                       [&resolved](const Contribution& existing) {
                         return existing.capture_channel ==
                                    resolved.capture_channel &&
                                existing.endpoint_id == resolved.endpoint_id;
                       })) {
        Contribution contribution;
        contribution.endpoint_id = resolved.endpoint_id;
        contribution.capture_channel = resolved.capture_channel;
        contribution.gain = resolved.gain;
        contribution.mute = resolved.mute;
        resolved_line.contributions.push_back(contribution);
      }
      if (resolved.listen_channel >= 0 &&
          std::none_of(
              resolved_line.listeners.begin(), resolved_line.listeners.end(),
              [&resolved](const Listener& existing) {
                return existing.playback_channel == resolved.playback_channel &&
                       existing.endpoint_id == resolved.endpoint_id;
              })) {
        Listener listener;
        listener.endpoint_id = resolved.endpoint_id;
        listener.playback_channel = resolved.playback_channel;
        resolved_line.listeners.push_back(listener);
      }
    }
    runtime->lines.push_back(std::move(resolved_line));
  }

  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_ = std::move(runtime);
  }
  LOG_INFO("matrix: ", plan.lines.size(), " party line(s) configured");
  return true;
}

void IntercomMatrix::clear() {
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  runtime_.reset();
  // Drop whatever the conference's leg had queued: it belongs to the routing
  // that has just been replaced.
  conference_incoming_.reset();
  conference_outgoing_.reset();
}

bool IntercomMatrix::empty() const {
  return current_runtime() == nullptr;
}

std::shared_ptr<const IntercomMatrix::Runtime> IntercomMatrix::current_runtime()
    const {
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  return runtime_;
}

void IntercomMatrix::process(const float* capture, float* playback,
                             unsigned channels, unsigned frames) {
  if (capture == nullptr || playback == nullptr || channels == 0 || frames == 0) {
    return;
  }
  const auto runtime = current_runtime();
  if (runtime == nullptr) {
    return;
  }

  // Own the mix channels first: a mix is a sum, and a channel nobody mixes has
  // to go silent rather than hold whatever was in the buffer before.
  for (const unsigned channel : runtime->owned_playback) {
    if (channel >= channels) {
      continue;
    }
    for (unsigned frame = 0; frame < frames; ++frame) {
      playback[static_cast<size_t>(frame) * channels + channel] = 0.0F;
    }
  }

  // What the conference says, drained once per block: every listener on a line
  // that claims it reads the same samples, and a leg that sent nothing leaves
  // silence rather than whatever the buffer held.
  const unsigned conference_frames = std::min(frames, kMaxConferenceBlockFrames);
  const size_t conference_read = conference_incoming_.read(
      conference_incoming_block_.data(), conference_frames);
  std::fill(conference_incoming_block_.data() + conference_read,
            conference_incoming_block_.data() + conference_frames, 0.0F);

  const double decay_db = kPeakDecayDbPerMs * 1000.0 * static_cast<double>(frames) /
                          static_cast<double>(sample_rate_);

  bool conference_claimed = false;
  for (const auto& line : runtime->lines) {
    // The mixes: one per distinct listen channel this line feeds.
    for (const auto& listener : line.listeners) {
      if (listener.playback_channel >= channels) {
        continue;
      }
      for (unsigned frame = 0; frame < frames; ++frame) {
        const size_t base = static_cast<size_t>(frame) * channels;
        double sum = 0.0;
        for (const auto& contributor : line.contributions) {
          if (contributor.mute || contributor.capture_channel >= channels) {
            continue;
          }
          // Mix-minus by construction: never the listener's own endpoint, and a
          // muted member contributes nothing without changing what it hears.
          if (contributor.endpoint_id == listener.endpoint_id) {
            continue;
          }
          sum += static_cast<double>(capture[base + contributor.capture_channel]) *
                 contributor.gain;
        }
        playback[base + listener.playback_channel] += static_cast<float>(sum);
      }
    }

    // A line that claims the conference: its members hear what arrived on the SIP
    // leg.  A line that does not claim it is untouched by it.
    if (line.claims_conference) {
      conference_claimed = true;
      for (const auto& listener : line.listeners) {
        if (listener.playback_channel >= channels) {
          continue;
        }
        for (unsigned frame = 0; frame < conference_frames; ++frame) {
          playback[static_cast<size_t>(frame) * channels +
                   listener.playback_channel] += conference_incoming_block_[frame];
        }
      }
    }

    // The meters: per configured binding, so the status view can name the channel
    // that went quiet.
    for (const auto& member : line.members) {
      if (member.talk_channel < 0 || member.capture_channel >= channels) {
        continue;
      }
      double peak = 0.0;
      for (unsigned frame = 0; frame < frames; ++frame) {
        peak = std::max(peak, std::fabs(static_cast<double>(
                                  capture[static_cast<size_t>(frame) * channels +
                                          member.capture_channel])));
      }
      member.meter.value.store(
          hold_peak(member.meter.value.load(), linear_to_dbfs(peak), decay_db));
    }
  }

  // What the conference hears: the members of the lines that claim it, each at its
  // contribution level.  Its own contribution is never in there — mix-minus
  // applies to the conference like any other participant, so it cannot be echoed.
  if (conference_claimed && conference_frames > 0) {
    for (unsigned frame = 0; frame < conference_frames; ++frame) {
      const size_t base = static_cast<size_t>(frame) * channels;
      double sum = 0.0;
      for (const auto& line : runtime->lines) {
        if (!line.claims_conference) {
          continue;
        }
        for (const auto& contributor : line.contributions) {
          if (contributor.mute || contributor.capture_channel >= channels) {
            continue;
          }
          sum += static_cast<double>(capture[base + contributor.capture_channel]) *
                 contributor.gain;
        }
      }
      conference_outgoing_block_[frame] = static_cast<float>(sum);
    }
    conference_outgoing_.write(conference_outgoing_block_.data(),
                               conference_frames);
  }
}

std::vector<MatrixLineStatus> IntercomMatrix::status() const {
  std::vector<MatrixLineStatus> result;
  const auto runtime = current_runtime();
  if (runtime == nullptr) {
    return result;
  }
  for (const auto& line : runtime->lines) {
    MatrixLineStatus line_status;
    line_status.id = line.id;
    line_status.name = line.name;
    line_status.claims_conference = line.claims_conference;
    for (const auto& member : line.members) {
      MatrixMemberStatus member_status;
      member_status.endpoint = member.endpoint_id;
      member_status.endpoint_name = member.endpoint_name;
      member_status.talk_channel = member.talk_channel;
      member_status.listen_channel = member.listen_channel;
      member_status.contribution_db = member.contribution_db;
      member_status.level_dbfs = member.meter.value.load();
      member_status.arriving = member_status.level_dbfs > kArrivalThresholdDbfs;
      line_status.members.push_back(std::move(member_status));
    }
    result.push_back(std::move(line_status));
  }
  return result;
}

void IntercomMatrix::push_conference_audio(const float* source, size_t frames) {
  if (source == nullptr || frames == 0) {
    return;
  }
  conference_incoming_.write(source, frames);
}

size_t IntercomMatrix::pull_conference_audio(float* destination, size_t frames) {
  if (destination == nullptr || frames == 0) {
    return 0;
  }
  const size_t read = conference_outgoing_.read(destination, frames);
  std::fill(destination + read, destination + frames, 0.0F);
  return read;
}

}  // namespace aes67sip
