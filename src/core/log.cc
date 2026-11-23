#include "firefly/common/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>

namespace firefly {
namespace {

std::mutex g_log_mutex;
LogSink g_sink;

LogLevel LevelFromTag(const char* tag) {
  if (tag != nullptr && std::string(tag) == "ERROR") return LogLevel::kError;
  if (tag != nullptr && std::string(tag) == "WARN") return LogLevel::kWarn;
  return LogLevel::kInfo;
}

}  // namespace

void SetLogSink(LogSink sink) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_sink = std::move(sink);
}

namespace internal {

void LogWrite(const char* tag, const char* file, int line, const char* fmt, ...) {
  char msg[1024] = {0};
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char ts[32] = {0};
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

  std::string line_text = std::string(ts) + " [" + (tag ? tag : "LOG") + "] " +
                          (file ? file : "?") + ":" + std::to_string(line) +
                          " | " + msg;

  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_sink) {
    g_sink(LevelFromTag(tag), line_text.c_str());
  } else {
    std::fprintf(stderr, "%s\n", line_text.c_str());
    std::fflush(stderr);
  }
}

}  // namespace internal
}  // namespace firefly
