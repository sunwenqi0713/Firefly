#ifndef FIREFLY_COMMON_LOG_H_
#define FIREFLY_COMMON_LOG_H_

// 极简日志设施（阶段三：sink 可替换）。
// 默认 sink 仍为 fprintf(stderr)，线程安全；用户可调用 SetLogSink 注入自定义
// sink（例如转发到业务日志系统）。sink 不得回调 Firefly 日志（会自锁）。
// LogWrite 的协议：调用方给出 tag/file/line/格式化串，实现负责时间戳、格式化
// 与串行派发；默认输出 [HH:MM:SS] [TAG] file:line | message。

#include <functional>

namespace firefly {

enum class LogLevel { kInfo, kWarn, kError };
using LogSink = std::function<void(LogLevel level, const char* message)>;

// 注入自定义 sink；传 nullptr 恢复默认 stderr 输出。线程安全（@threadsafe）。
void SetLogSink(LogSink sink);

namespace internal {

void LogWrite(const char* tag, const char* file, int line, const char* fmt, ...);

// 供 nngpp 异常打印的辅助函数（保留旧签名）
inline const char* ErrToString(const char* who, int err) {
  (void)who;
  (void)err;
  return nullptr;
}

}  // namespace internal
}  // namespace firefly

#define FY_INFO(fmt, ...)                                    \
  ::firefly::internal::LogWrite("INFO", __FILE__, __LINE__,  \
                                fmt, ##__VA_ARGS__)

#define FY_WARN(fmt, ...)                                    \
  ::firefly::internal::LogWrite("WARN", __FILE__, __LINE__,  \
                                fmt, ##__VA_ARGS__)

#define FY_ERROR(fmt, ...)                                   \
  ::firefly::internal::LogWrite("ERROR", __FILE__, __LINE__, \
                                fmt, ##__VA_ARGS__)

#endif  // FIREFLY_COMMON_LOG_H_
