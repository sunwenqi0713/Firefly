#ifndef FIREFLY_COMMON_LOG_H_
#define FIREFLY_COMMON_LOG_H_

/// @file log.h
/// @brief 极简日志设施与 FY_INFO / FY_WARN / FY_ERROR 宏。
///
/// 默认输出到 stderr，格式为 `[HH:MM:SS] [TAG] file:line | message`，写入
/// 由内部串行化，多线程可直接调用。可通过 SetLogSink() 转发到业务日志系统。

#include <functional>

namespace firefly {

/// @brief 日志级别。
enum class LogLevel {
  kInfo,   ///< 常规流程信息。
  kWarn,   ///< 可恢复的异常，功能仍可用。
  kError   ///< 失败或不可恢复的异常。
};

/// @brief 自定义日志出口的签名。
/// @note 回调在写日志的线程上执行，必须快速返回且不得调用任何 Firefly 日志
///       宏或 SetLogSink()，否则会自锁。
using LogSink = std::function<void(LogLevel level, const char* message)>;

/// @brief 注入自定义日志出口。
/// @param[in] sink 出口函数；传 nullptr 恢复默认的 stderr 输出。
/// @par 线程安全
/// 可与正在进行的日志写入并发调用；替换只影响之后的日志。
void SetLogSink(LogSink sink);

namespace internal {

/// @brief 日志写入原语：格式化并派发到当前出口。
/// @param[in] tag  级别标签，如 `"INFO"`。
/// @param[in] file 源文件名，通常由宏传入 `__FILE__`。
/// @param[in] line 源码行号，通常由宏传入 `__LINE__`。
/// @param[in] fmt  printf 风格格式串。
/// @note 由 FY_INFO / FY_WARN / FY_ERROR 调用，业务代码不应直接调用。
void LogWrite(const char* tag, const char* file, int line, const char* fmt, ...);

/// @brief 兼容旧调用方的占位实现。
/// @return 恒返回 nullptr。
/// @deprecated 不提供任何信息，改用 FY_ERROR 直接记录错误码。
inline const char* ErrToString(const char* who, int err) {
  (void)who;
  (void)err;
  return nullptr;
}

}  // namespace internal
}  // namespace firefly

/// @brief 记录一条 INFO 级日志。
/// @param[in] fmt   printf 风格格式串。
/// @param[in] ...   与格式串匹配的参数。
/// @note 参数只求值一次；宏展开为一次函数调用，可安全用于 if 语句体。
#define FY_INFO(fmt, ...)                                    \
  ::firefly::internal::LogWrite("INFO", __FILE__, __LINE__,  \
                                fmt, ##__VA_ARGS__)

/// @brief 记录一条 WARN 级日志。
/// @param[in] fmt   printf 风格格式串。
/// @param[in] ...   与格式串匹配的参数。
/// @note 参数只求值一次；宏展开为一次函数调用，可安全用于 if 语句体。
#define FY_WARN(fmt, ...)                                    \
  ::firefly::internal::LogWrite("WARN", __FILE__, __LINE__,  \
                                fmt, ##__VA_ARGS__)

/// @brief 记录一条 ERROR 级日志。
/// @param[in] fmt   printf 风格格式串。
/// @param[in] ...   与格式串匹配的参数。
/// @note 参数只求值一次；宏展开为一次函数调用，可安全用于 if 语句体。
#define FY_ERROR(fmt, ...)                                   \
  ::firefly::internal::LogWrite("ERROR", __FILE__, __LINE__, \
                                fmt, ##__VA_ARGS__)

#endif  // FIREFLY_COMMON_LOG_H_
