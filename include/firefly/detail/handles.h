#ifndef FIREFLY_DETAIL_HANDLES_H_
#define FIREFLY_DETAIL_HANDLES_H_

/// @file handles.h
/// @brief 端点内部实现句柄的抽象基类。
///
/// 用于 pimpl 类型擦除：公共模板只持有基类指针，具体实现由后端定义在本文件
/// 之外。业务代码不应依赖本文件中的任何类型。

namespace firefly {
namespace detail {

/// @brief 发布端点内部实现的抽象基类。
struct WriterHandle {
  virtual ~WriterHandle() = default;
};

/// @brief 订阅端点内部实现的抽象基类。
struct ReaderHandle {
  virtual ~ReaderHandle() = default;
};

/// @brief 服务端点内部实现的抽象基类。
struct ServiceHandle {
  virtual ~ServiceHandle() = default;
};

/// @brief 客户端端点内部实现的抽象基类。
struct ClientHandle {
  virtual ~ClientHandle() = default;
};

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_HANDLES_H_