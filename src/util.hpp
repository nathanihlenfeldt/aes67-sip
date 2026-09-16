#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aes67sip {

using json = nlohmann::json;

/** Milliseconds since the unix epoch. */
int64_t now_ms();

/** Monotonic seconds, used for measuring intervals. */
double monotonic_seconds();

/** "mm:ss" / "hh:mm:ss". */
std::string format_duration(int64_t seconds);

/** dBFS from linear RMS amplitude; returns -inf for silence. */
double linear_to_dbfs(double amplitude);

/** Linear gain from a dB value. */
double db_to_linear(double db);

/** Level of a silent channel in dBFS: the floor every meter holds at. */
constexpr double kSilenceDbfs = -1000.0;

/** Peak hold decay for level meters, in dB per millisecond. */
constexpr double kPeakDecayDbPerMs = 0.5;

/**
 * Peak hold with a fixed decay, so the meters stay readable in the UI.  It never
 * decays below `kSilenceDbfs`: silence is the floor, not an ever smaller number.
 */
double hold_peak(double previous, double current, double decay_db);

/** JSON helper: value of `key` if present and of a convertible type, else `def`. */
template <typename T>
T json_get(const json& object, const char* key, const T& def) {
  if (!object.is_object()) {
    return def;
  }
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) {
    return def;
  }
  try {
    return it->get<T>();
  } catch (const json::exception&) {
    return def;
  }
}

/** JSON helper for nested lookups: json_get_path(j, {"sip", "dial_target"}, "") */
template <typename T>
T json_get_path(const json& object, const std::vector<std::string>& path,
                const T& def) {
  const json* current = &object;
  for (const auto& key : path) {
    if (!current->is_object()) {
      return def;
    }
    const auto it = current->find(key);
    if (it == current->end()) {
      return def;
    }
    current = &(*it);
  }
  if (current->is_null()) {
    return def;
  }
  try {
    return current->get<T>();
  } catch (const json::exception&) {
    return def;
  }
}

/** True if `object` has `key` (even when the value is null). */
bool json_has(const json& object, const std::string& key);

/** Trims ASCII whitespace. */
std::string trim(const std::string& value);

/** Splits on a single delimiter, keeping empty fields. */
std::vector<std::string> split(const std::string& value, char delimiter);

/** Joins values with a delimiter. */
std::string join(const std::vector<std::string>& values,
                 const std::string& delimiter);

/** Lower cases an ASCII string. */
std::string to_lower(const std::string& value);

/** Parses "3.5" / "3" / "-inf" into a double; returns nullopt on failure. */
std::optional<double> parse_double(const std::string& value);

/** Serialises a dBFS value, mapping -inf to JSON null. */
json dbfs_to_json(double dbfs);

/**
 * Best effort real time scheduling for the audio thread.  Requires
 * CAP_SYS_NICE or root on Linux; failures are not fatal and only logged.
 */
void set_realtime_thread_priority(int priority);

/** Sets the current thread name (truncated to the platform limit). */
void set_thread_name(const std::string& name);

}  // namespace aes67sip
