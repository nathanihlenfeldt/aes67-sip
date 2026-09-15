#pragma once

#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace aes67sip {

enum class LogSeverity { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

/**
 * Minimal process wide logger.
 *
 * Messages are written to stderr and kept in a bounded in-memory ring buffer so
 * that the REST API can expose a log tail to the web UI (`GET /api/log`).
 * All methods are thread safe.
 */
class Log {
 public:
  static Log& instance();

  void set_severity(LogSeverity severity);
  LogSeverity severity() const;
  bool enabled(LogSeverity severity) const;

  void write(LogSeverity severity, const std::string& message);

  /** Last `lines` log lines, oldest first. */
  std::vector<std::string> tail(size_t lines) const;

  void clear();

 private:
  Log() = default;

  mutable std::mutex mutex_;
  LogSeverity severity_{LogSeverity::kInfo};
  std::vector<std::string> ring_;
  size_t capacity_{1000};
};

/** Formats arguments with operator<< into a single string. */
template <typename... Args>
std::string log_format(Args&&... args) {
  std::ostringstream out;
  (out << ... << args);
  return out.str();
}

}  // namespace aes67sip

#define AES67SIP_LOG(severity, ...)                                           \
  do {                                                                        \
    if (::aes67sip::Log::instance().enabled(severity)) {                      \
      ::aes67sip::Log::instance().write(severity,                             \
                                        ::aes67sip::log_format(__VA_ARGS__)); \
    }                                                                         \
  } while (0)

#define LOG_ERROR(...) AES67SIP_LOG(::aes67sip::LogSeverity::kError, __VA_ARGS__)
#define LOG_WARN(...) AES67SIP_LOG(::aes67sip::LogSeverity::kWarn, __VA_ARGS__)
#define LOG_INFO(...) AES67SIP_LOG(::aes67sip::LogSeverity::kInfo, __VA_ARGS__)
#define LOG_DEBUG(...) AES67SIP_LOG(::aes67sip::LogSeverity::kDebug, __VA_ARGS__)
