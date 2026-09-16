#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"

namespace aes67sip {

/** One member's place on a party line, before device channels are resolved. */
struct MatrixMemberPlan {
  std::string endpoint_id;
  int talk_channel{-1};    // index into the endpoint's declared talk channels
  int listen_channel{-1};  // index into the endpoint's declared listen channels
  double contribution_db{0.0};
  bool mute{false};  // stops this member being heard, never what it hears
};

struct MatrixLinePlan {
  std::string id;
  std::string name;
  std::vector<MatrixMemberPlan> members;
};

/**
 * An endpoint's declared shape: how many talk channels it sends and how many
 * listen channels it receives.  The device channels are the ones those channels
 * map onto, and nothing in the matrix assumes any particular shape.
 */
struct MatrixEndpointPlan {
  std::string id;
  std::string name;
  std::vector<unsigned> talk_channels;    // device capture channels
  std::vector<unsigned> listen_channels;  // device playback channels
};

/** The routing the audio thread runs. */
struct MatrixPlan {
  std::vector<MatrixEndpointPlan> endpoints;
  std::vector<MatrixLinePlan> lines;

  bool empty() const { return lines.empty(); }
};

struct MatrixMemberStatus {
  std::string endpoint;
  std::string endpoint_name;
  double contribution_db{0.0};
  double level_dbfs{-1000.0};
  bool arriving{false};
};

struct MatrixLineStatus {
  std::string id;
  std::string name;
  std::vector<MatrixMemberStatus> members;
};

/**
 * The intercom matrix: the party lines the appliance runs.
 *
 * Each party line is a set of members; a member contributes one of its
 * endpoint's talk channels to the line, and hears the line on one of its listen
 * channels.  Every mix excludes the listening endpoint's own contributions, so
 * feedback is impossible by construction rather than discouraged by convention.
 *
 * The matrix owns no streams and knows nothing about SIP: it reads the captured
 * device channels and writes the mixes to the playback channels, inside the one
 * real-time thread that owns the audio device.
 */
class IntercomMatrix {
 public:
  /** `sample_rate` is the device rate, used to pace the level meters' decay. */
  explicit IntercomMatrix(unsigned sample_rate = 48000);
  ~IntercomMatrix();

  IntercomMatrix(const IntercomMatrix&) = delete;
  IntercomMatrix& operator=(const IntercomMatrix&) = delete;

  /** Resolves a configuration into a plan, without touching the running routing. */
  static bool plan_from_config(const Config& config, MatrixPlan* plan,
                               std::string* error);

  /** Replaces the running routing. Call from any control thread. */
  bool configure(const MatrixPlan& plan, std::string* error);

  /** Drops every party line: nothing is mixed and no playback channel is written.
   */
  void clear();

  /** True while no party line is configured. */
  bool empty() const;

  /**
   * One audio block.  Reads contributions from `capture`, writes each member's
   * mix to its listen channel in `playback`, and drives every listen channel the
   * declared endpoints own — whether or not a line routes to it — rather than
   * leaving stale audio in the device buffer.  Never allocates and never blocks.
   */
  void process(const float* capture, float* playback, unsigned channels,
               unsigned frames);

  /** Membership and levels, in configuration order. */
  std::vector<MatrixLineStatus> status() const;

 private:
  /**
   * A peak meter the audio thread updates and the status thread reads.
   *
   * Hand-written copy and move because a container element has to be movable and
   * `std::atomic` is not: both transfer the value rather than the atomic.
   */
  struct LevelMeter {
    LevelMeter() = default;
    LevelMeter(const LevelMeter& other) : value(other.value.load()) {}
    LevelMeter& operator=(const LevelMeter& other) {
      value.store(other.value.load());
      return *this;
    }
    LevelMeter(LevelMeter&& other) noexcept : value(other.value.load()) {}
    LevelMeter& operator=(LevelMeter&& other) noexcept {
      value.store(other.value.load());
      return *this;
    }

    mutable std::atomic<double> value{kSilenceDbfs};
  };

  struct RuntimeMember {
    std::string endpoint_id;
    std::string endpoint_name;
    int talk_channel{-1};
    int listen_channel{-1};
    unsigned capture_channel{0};
    unsigned playback_channel{0};
    float gain{1.0F};
    double contribution_db{0.0};
    bool mute{false};
    LevelMeter meter;
  };

  struct RuntimeLine {
    std::string id;
    std::string name;
    std::vector<RuntimeMember> members;
  };

  struct Runtime {
    std::vector<RuntimeLine> lines;
    std::vector<unsigned> owned_playback;  // channels the matrix writes
  };

  std::shared_ptr<const Runtime> current_runtime() const;

  unsigned sample_rate_{48000};
  mutable std::mutex runtime_mutex_;
  std::shared_ptr<const Runtime> runtime_;
};

}  // namespace aes67sip
