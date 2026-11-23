#ifndef FIREFLY_DETAIL_HANDLES_H_
#define FIREFLY_DETAIL_HANDLES_H_

namespace firefly {
namespace detail {

// 不透明实现句柄基类：具体实现由底层传输层持有（见 *_impl.h）。
// 仅用于 pimpl 类型擦除，接口层不同实现细节。
struct WriterHandle {
  virtual ~WriterHandle() = default;
};

struct ReaderHandle {
  virtual ~ReaderHandle() = default;
};

struct ServiceHandle {
  virtual ~ServiceHandle() = default;
};

struct ClientHandle {
  virtual ~ClientHandle() = default;
};

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_HANDLES_H_