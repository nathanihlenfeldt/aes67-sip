#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>

namespace aes67sip {

namespace {

const char* severity_name(LogSeverity severity) {
  switch (severity) {
    case LogSeverity::kError:
      return "error";
    case LogSeverity::kWarn:
      return "warn";
    case LogSeverity::kInfo:
      return "info";
    case LogSeverity::kDebug:
      return "debug";
  }
  return "?";
}

std::string timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t tt = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
      << std::setw(3) << ms.count();
  return out.str();
}

}  // namespace

Log& Log::instance() {
  static Log logger;
  return logger;
}

void Log::set_severity(LogSeverity severity) {
  std::lock_guard<std::mutex> lock(mutex_);
  severity_ = severity;
}

LogSeverity Log::severity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return severity_;
}

bool Log::enabled(LogSeverity severity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(severity) <= static_cast<int>(severity_);
}

void Log::write(LogSeverity severity, const std::string& message) {
  const std::string line =
      timestamp() + " [" + severity_name(severity) + "] " + message;
  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<int>(severity) > static_cast<int>(severity_)) {
    return;
  }
  std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
  if (ring_.size() >= capacity_) {
    ring_.erase(ring_.begin(), ring_.begin() + (ring_.size() - capacity_ + 1));
  }
  ring_.push_back(line);
}

std::vector<std::string> Log::tail(size_t lines) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lines == 0 || lines >= ring_.size()) {
    return ring_;
  }
  return std::vector<std::string>(ring_.end() - static_cast<long>(lines),
                                  ring_.end());
}

void Log::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  ring_.clear();
}

}  // namespace aes67sip
