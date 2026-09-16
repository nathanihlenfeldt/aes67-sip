// The intercom matrix: party lines mixed on the appliance.
//
// Everything here works on device channels and buffers, so it is testable
// without an audio device: feed it a capture block, read the playback block.

#include "matrix/intercom_matrix.hpp"

#include <algorithm>
#include <cmath>

#include "log.hpp"
#include "util.hpp"

namespace aes67sip {

namespace {

/** A member counts as arriving while its held peak is above this. */
constexpr double kArrivalThresholdDbfs = -60.0;

}  // namespace

IntercomMatrix::IntercomMatrix(unsigned sample_rate)
    : sample_rate_(sample_rate == 0 ? 48000 : sample_rate) {}

IntercomMatrix::~IntercomMatrix() = default;

bool IntercomMatrix::plan_from_config(const Config& config, MatrixPlan* plan,
                                      std::string* error) {
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
  for (const auto& line : config.party_lines) {
    MatrixLinePlan planned;
    planned.id = line.id;
    planned.name = line.name.empty() ? line.id : line.name;
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
    resolved_line.members.reserve(line.members.size());
    for (const auto& member : line.members) {
      const MatrixEndpointPlan* endpoint = nullptr;
      for (const auto& candidate : plan.endpoints) {
        if (candidate.id == member.endpoint_id) {
          endpoint = &candidate;
          break;
        }
      }
      if (endpoint == nullptr) {
        if (error != nullptr) {
          *error = "party line '" + line.id +
                   "' has a member for unknown endpoint '" + member.endpoint_id +
                   "'";
        }
        return false;
      }
      if (member.talk_channel >= 0 && static_cast<size_t>(member.talk_channel) >=
                                          endpoint->talk_channels.size()) {
        if (error != nullptr) {
          *error = "endpoint '" + endpoint->id + "' has no talk channel " +
                   std::to_string(member.talk_channel);
        }
        return false;
      }
      if (member.listen_channel >= 0 &&
          static_cast<size_t>(member.listen_channel) >=
              endpoint->listen_channels.size()) {
        if (error != nullptr) {
          *error = "endpoint '" + endpoint->id + "' has no listen channel " +
                   std::to_string(member.listen_channel);
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

  const double decay_db = kPeakDecayDbPerMs * 1000.0 * static_cast<double>(frames) /
                          static_cast<double>(sample_rate_);

  for (const auto& line : runtime->lines) {
    for (const auto& member : line.members) {
      if (member.listen_channel >= 0 && member.playback_channel < channels) {
        for (unsigned frame = 0; frame < frames; ++frame) {
          const size_t base = static_cast<size_t>(frame) * channels;
          double sum = 0.0;
          for (const auto& contributor : line.members) {
            if (contributor.talk_channel < 0 ||
                contributor.capture_channel >= channels) {
              continue;
            }
            // Mix-minus by construction: never the listener's own endpoint, and a
            // muted member contributes nothing without changing what it hears.
            if (contributor.endpoint_id == member.endpoint_id || contributor.mute) {
              continue;
            }
            sum +=
                static_cast<double>(capture[base + contributor.capture_channel]) *
                contributor.gain;
          }
          playback[base + member.playback_channel] += static_cast<float>(sum);
        }
      }

      if (member.talk_channel >= 0 && member.capture_channel < channels) {
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
    for (const auto& member : line.members) {
      MatrixMemberStatus member_status;
      member_status.endpoint = member.endpoint_id;
      member_status.endpoint_name = member.endpoint_name;
      member_status.contribution_db = member.contribution_db;
      member_status.level_dbfs = member.meter.value.load();
      member_status.arriving = member_status.level_dbfs > kArrivalThresholdDbfs;
      line_status.members.push_back(std::move(member_status));
    }
    result.push_back(std::move(line_status));
  }
  return result;
}

}  // namespace aes67sip
