#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/ring.hpp"
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
  /** Whether this line claims the conference (see `PartyLineConfig`). */
  bool claims_conference{false};
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

/**
 * The device channels the declared endpoint shapes claim.
 *
 * One declared channel is one device channel, and the two directions are
 * independent, so the width the device has to be opened at is the highest channel
 * index the shapes reach in either direction, plus one.  Nothing here counts
 * endpoints or assumes a shape: halving the declared shapes halves this, and a
 * site that declares four channels needs a four channel device whatever the
 * endpoints are called.
 */
struct MatrixChannelUse {
  unsigned capture{0};   // talk channels: device capture channels
  unsigned playback{0};  // listen channels: device playback channels

  /** The device width that covers both directions. */
  unsigned device_channels() const;
};

struct MatrixMemberStatus {
  std::string endpoint;
  std::string endpoint_name;
  /** The endpoint's own channel indices, or -1 where that direction is unbound. */
  int talk_channel{-1};
  int listen_channel{-1};
  double contribution_db{0.0};
  double level_dbfs{kSilenceDbfs};
  bool arriving{false};
};

struct MatrixLineStatus {
  std::string id;
  std::string name;
  /** True when this line's members are on the off-site conference call. */
  bool claims_conference{false};
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

  /**
   * Resolves a configuration into a plan, without touching the running routing.
   *
   * Refuses the configuration - false, with the declared and the available
   * totals in `error` - when the declared endpoint shapes need a wider device
   * than `audio.channels` opens, rather than routing the channels that happen to
   * fit.  A channel outside the opened device is a member that silently never
   * arrives, which is the failure this is here to prevent.
   */
  static bool plan_from_config(const Config& config, MatrixPlan* plan,
                               std::string* error);

  /** The device channels a plan's declared shapes claim. */
  static MatrixChannelUse channel_use(const MatrixPlan& plan);

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

  /**
   * Queues what the conference says: the audio arriving on the SIP leg.  The
   * matrix knows nothing about SIP — it only knows that one participant, the
   * conference, has an incoming side that somebody else feeds.
   */
  void push_conference_audio(const float* source, size_t frames);

  /**
   * Fills what the conference hears: the sum of the members of the lines that
   * claim it, each at its contribution level.  Writes what it can and pads with
   * silence, like a member's mix.
   */
  size_t pull_conference_audio(float* destination, size_t frames);

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

  /**
   * One distinct contribution a line receives: an endpoint's talk channel, once,
   * however many member entries bind it.  A beltpack that talks on two channels
   * into the same line is heard once per channel; one whose mic is bound twice is
   * still heard once.
   */
  struct Contribution {
    std::string endpoint_id;
    unsigned capture_channel{0};
    float gain{1.0F};
    bool mute{false};
  };

  /** One distinct mix a line feeds: an endpoint's listen channel, once. */
  struct Listener {
    std::string endpoint_id;
    unsigned playback_channel{0};
  };

  struct RuntimeLine {
    std::string id;
    std::string name;
    bool claims_conference{false};
    std::vector<RuntimeMember> members;  // as configured, for the status view
    std::vector<Contribution> contributions;
    std::vector<Listener> listeners;
  };

  struct Runtime {
    std::vector<RuntimeLine> lines;
    std::vector<unsigned> owned_playback;  // channels the matrix writes
  };

  std::shared_ptr<const Runtime> current_runtime() const;

  /**
   * The largest block the conference side mixes in one go.  The scratch buffers
   * are sized once, in the constructor, so the audio thread never allocates; a
   * block larger than this is simply mixed up to the capacity (periods are 48
   * frames in practice, so this is a bound, not a limit anyone meets).
   */
  static constexpr unsigned kMaxConferenceBlockFrames = 4096;

  unsigned sample_rate_{48000};
  mutable std::mutex runtime_mutex_;
  std::shared_ptr<const Runtime> runtime_;

  /** What the conference says, and what it hears; filled and drained by its leg. */
  SpscRing conference_incoming_{8192};
  SpscRing conference_outgoing_{8192};
  std::vector<float> conference_incoming_block_;
  std::vector<float> conference_outgoing_block_;
};

}  // namespace aes67sip
