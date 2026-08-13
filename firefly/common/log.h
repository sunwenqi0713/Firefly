#ifndef FIREFLY_COMMON_LOG_H_
#define FIREFLY_COMMON_LOG_H_

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace firefly {
namespace internal {

// 极简日志：带时间戳输出到 stderr，线程安全。
inline void LogWrite(const char* tag, const char* file, int line,
                     const char* fmt, ...) {
  static std::mutex g_log_mutex;
  std::lock_guard<std::mutex> lock(g_log_mutex);

  char ts[32] = {0};
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

  char msg[1024] = {0};
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  std::fprintf(stderr, "%s [%s] %s:%d | %s\n", ts, tag, file, line, msg);
  std::fflush(stderr);
}

}  // namespace internal
}  // namespace firefly

// 供 nngpp 异常打印的辅助函数
namespace firefly {
namespace internal {
inline const char* ErrToString(const char* who, int err) {
  (void)who;
  (void)err;
  return nullptr;
}
}  // namespace internal
}  // namespace firefly

#define FY_INFO(fmt, ...)                                    \
  ::firefly::internal::LogWrite("INFO", __FILE__, __LINE__,\
                                fmt, ##__VA_ARGS__)

#define FY_WARN(fmt, ...)                                   \
  ::firefly::internal::LogWrite("WARN", __FILE__, __LINE__, \
                                fmt, ##__VA_ARGS__)

#define FY_ERROR(fmt, ...)                                    \
  ::firefly::internal::LogWrite("ERROR", __FILE__, __LINE__,  \
                                fmt, ##__VA_ARGS__)

#endif  // FIREFLY_COMMON_LOG_H_