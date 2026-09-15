#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "log.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace aes67sip {

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

double monotonic_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string format_duration(int64_t seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  const int64_t hours = seconds / 3600;
  const int64_t minutes = (seconds % 3600) / 60;
  const int64_t secs = seconds % 60;
  char buffer[32];
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld",
                  static_cast<long long>(hours), static_cast<long long>(minutes),
                  static_cast<long long>(secs));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld",
                  static_cast<long long>(minutes), static_cast<long long>(secs));
  }
  return std::string(buffer);
}

double linear_to_dbfs(double amplitude) {
  if (amplitude <= 1e-9) {
    return -std::numeric_limits<double>::infinity();
  }
  return 20.0 * std::log10(amplitude);
}

double db_to_linear(double db) {
  return std::pow(10.0, db / 20.0);
}

bool json_has(const json& object, const std::string& key) {
  return object.is_object() && object.find(key) != object.end();
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> result;
  std::string current;
  for (const char c : value) {
    if (c == delimiter) {
      result.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  result.push_back(current);
  return result;
}

std::string join(const std::vector<std::string>& values,
                 const std::string& delimiter) {
  std::string result;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      result += delimiter;
    }
    result += values[i];
  }
  return result;
}

std::string to_lower(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

std::optional<double> parse_double(const std::string& value) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (trimmed == "-inf" || trimmed == "-Infinity") {
    return -std::numeric_limits<double>::infinity();
  }
  try {
    size_t consumed = 0;
    const double result = std::stod(trimmed, &consumed);
    if (consumed != trimmed.size()) {
      return std::nullopt;
    }
    return result;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

json dbfs_to_json(double dbfs) {
  if (!std::isfinite(dbfs)) {
    return nullptr;
  }
  return json(std::round(dbfs * 10.0) / 10.0);
}

void set_realtime_thread_priority(int priority) {
#if defined(__linux__)
  sched_param param{};
  param.sched_priority = priority;
  const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
  if (rc != 0) {
    LOG_DEBUG("cannot set SCHED_FIFO priority ", priority, ": ", std::strerror(rc),
              " (run with CAP_SYS_NICE for glitch free audio)");
  }
#elif defined(__APPLE__)
  // macOS has no SCHED_FIFO for unprivileged processes; raise the QoS class
  // instead so the audio thread is not throttled during local development.
  const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  (void)rc;
  (void)priority;
#else
  (void)priority;
#endif
}

void set_thread_name(const std::string& name) {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#elif defined(__APPLE__)
  pthread_setname_np(name.substr(0, 63).c_str());
#else
  (void)name;
#endif
}

}  // namespace aes67sip
