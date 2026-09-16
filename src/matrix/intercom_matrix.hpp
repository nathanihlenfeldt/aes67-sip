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

/**
 * What a line's members are doing, as one derived fact rather than three counts
 * each consumer computes for itself: the self-test, the API and the UI all read
 * this, so "who has gone silent" cannot be answered two different ways.
 *
 * A member with no talk channel bound cannot be heard on this line at all (it may
 * still listen), which is a configuration fact and not silence: a line where
 * nobody can talk is the one state that is genuinely broken, while a line whose
 * members are merely quiet is not.
 */
struct MatrixLineSummary {
  unsigned members{0};
  unsigned arriving{0};  // bound, and audio is reaching the appliance
  unsigned silent{0};    // bound, but nothing is arriving
  unsigned unbound{0};   // no talk channel bound: cannot be heard here
  std::vector<std::string> arriving_names;
  std::vector<std::string> silent_names;
  std::vector<std::string> unbound_names;

  /** True when at least one member can be heard on this line. */
  bool can_be_heard() const { return members > unbound; }
  /** True when the line can be heard and nobody is talking on it right now. */
  bool quiet() const { return can_be_heard() && arriving == 0; }

  /**
   * The one word a report or a UI shows, so neither has to re-derive it: `active`
   * (someone is being heard), `quiet` (the line works, nobody is talking) or
   * `cannot_be_heard` (no member has a talk channel bound, so the line carries
   * nothing - the one state that is broken rather than silent).
   *
   * A line whose members are all silent is `quiet`, and the appliance does not
   * claim more than it knows: with only per-member level meters, a quiet room and
   * every cable on that line being pulled look the same, which is why the names of
   * the silent members matter more than the verdict.
   */
  const char* state() const {
    if (!can_be_heard()) {
      return "cannot_be_heard";
    }
    return arriving > 0 ? "active" : "quiet";
  }
};

struct MatrixLineStatus {
  std::string id;
  std::string name;
  /** True when this line's members are on the off-site conference call. */
  bool claims_conference{false};
  std::vector<MatrixMemberStatus> members;
  MatrixLineSummary summary;
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
   * This is also the whole-configuration refusal for the intercom side: a
   * configuration that could not work is refused as a whole, with a reason a
   * commissioning engineer can act on, rather than partly applied.  It refuses,
   * in this order:
   *
   *  - two endpoints sharing an id, or two party lines sharing an id or a name
   *    (a binding names an endpoint by id, and the status view keys lines by id
   *    and shows their names, so both have to be unambiguous);
   *  - a member binding that does not resolve, or a channel outside the shape its
   *    endpoint declared (see `validate_plan`);
   *  - two endpoints claiming one device channel in the same direction, naming
   *    the channel and both claimants;
   *  - declared endpoint shapes wider than `audio.channels` opens, with the
   *    declared and the available totals.
   *
   * Nothing is applied from a configuration that fails any of them.
   */
  static bool plan_from_config(const Config& config, MatrixPlan* plan,
                               std::string* error);

  /**
   * Refuses a plan whose bindings do not resolve: a member naming an endpoint
   * that does not exist, or a talk/listen channel outside the shape that endpoint
   * declared.  `configure()` checks the plan it is handed with this too, so a
   * plan built in code cannot index past a declared shape.
   *
   * The two device-level rules - one claimant per channel, and the declared
   * shapes fitting the device - are configuration-level and live in
   * `plan_from_config`: `configure()` never opens the device, it only routes the
   * channels a validated configuration gave it.
   */
  static bool validate_plan(const MatrixPlan& plan, std::string* error);

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

/**
 * True when the configuration could work: the conference leg is diallable and uses
 * a declared account, no configured line takes the conference leg's reserved id,
 * and (see `IntercomMatrix::plan_from_config`) no duplicate endpoint or party-line
 * ids or names, no device channel claimed twice, every member binding in range and
 * resolvable, and the declared shapes inside the device the configuration opens.
 *
 * The matrix plan is resolved into `plan` when one is given, so an apply path can
 * validate and resolve in one step; pass null to validate only (what an editor
 * asking "would this be accepted?" wants).  Every caller that applies or edits
 * configuration goes through this one function, which is what keeps a hand-edited
 * file and a web edit refused the same way.
 */
bool validate_configuration(const Config& config, MatrixPlan* plan,
                            std::string* error);

}  // namespace aes67sip
