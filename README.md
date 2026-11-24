# Firefly

Firefly 是一个 **C++17 消息通信库**，提供基于 Protocol Buffers 的类型化发布订阅、请求应答与端点发现。API 设计参考 Apollo Cyber RT，应用通过 `Node` 创建通信端点。

默认 **NNG 后端**支持同机跨进程通信，**内存后端**支持进程内通信（示例、测试、无 NNG 构建）。两种后端使用同一套应用 API，构建时选择。

| 能力 | API |
| --- | --- |
| 发布订阅 | `Writer<T>` 发布，`Reader<T>` 回调接收或从有界历史缓存读取 |
| 请求应答 | `Service<Req, Resp>` 处理，`Client<Req, Resp>` 提供同步与可取消的异步调用 |
| 端点发现 | 按通道名/服务名发现，`HasReader()`、`HasWriter()`、`WaitForService()` 查询 |
| 运行统计 | `Stats()` 报告收发计数、丢弃计数与缓存深度 |

## 快速开始

需要 **C++17 编译器、CMake 3.21+、Protobuf（含 `protoc`）**；默认 NNG 构建还需要 **NNG 与 Python 3**（仅跨进程测试）。以下假设已安装 [vcpkg](https://github.com/microsoft/vcpkg) 并设置 `VCPKG_ROOT`。若 CMake 找不到 `protoc`，将其加入 `PATH` 或传 `-DPROTOC_EXE=...`。

**Linux / macOS**

```bash
git clone https://github.com/sunwenqi0713/Firefly.git && cd Firefly
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/firefly_pubsub_example && ./build/firefly_rpc_example
```

**Windows**（VS Developer PowerShell，使用 Ninja）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`-DFIREFLY_WITH_NNG=OFF` 会排除 NNG 的查找、编译与链接，改用内存后端（仓库的 vcpkg manifest 仍含 NNG）。只有 Protobuf 的环境请使用独立依赖前缀。

## 集成到 C++ 项目

把 Firefly 放到应用的 `third_party/Firefly`，链接 `firefly::firefly`：

```cmake
add_subdirectory(third_party/Firefly)
add_executable(my_app main.cpp)
target_compile_features(my_app PRIVATE cxx_std_17)
target_link_libraries(my_app PRIVATE firefly::firefly)
```

下面的 `main.cpp` 用 Protobuf 自带的 `Int32Value` 做一次同进程 RPC，成功输出 `42`。业务消息用 `protoc` 从自己的 `.proto` 生成并加入应用 target；消息类型须派生自 `google::protobuf::Message`。

```cpp
#include <chrono>
#include <iostream>
#include <memory>

#include <google/protobuf/wrappers.pb.h>
#include <firefly/firefly.h>

int Run() {
  using Value = google::protobuf::Int32Value;
  using namespace std::chrono_literals;

  auto node = firefly::CreateNode("example");
  if (!node) return 1;

  auto service = node.value->CreateService<Value, Value>(
      "double", [](const std::shared_ptr<const Value>& request) {
        auto reply = std::make_shared<Value>();
        reply->set_value(request->value() * 2);
        return firefly::ServiceResult<Value>{firefly::ErrorCode::kOk, reply};
      });
  auto client = node.value->CreateClient<Value, Value>("double");
  if (!service || !client || !client.value->WaitForService(2s)) return 1;

  Value request;
  request.set_value(21);
  auto response = client.value->SendRequest(request, 2s);
  if (!response) return 1;
  std::cout << response->value() << '\n';
  return 0;
}

int main() {
  if (!firefly::Init("example")) return 1;
  const int status = Run();
  firefly::Clear();
  return status;
}
```

## 使用约定

- **发布状态。** `WriteStatus::kAccepted` 表示本地传输层接受了消息，**不代表对端收到**；默认 Pub/Sub 为尽力投递，需要确认时由业务定义应答。
- **请求取消。** 排队中的请求取消后跳过发送；一旦提交发送，取消只结束本地等待，服务端仍可能执行。
- **网络范围。** 发现地址与数据端点默认使用回环地址，适合同机跨进程。多组进程并行时用 `FIREFLY_DISCOVERY_REP_URL`、`FIREFLY_DISCOVERY_PUB_URL` 配置发现端口。

## 文档

| 文档 | 内容 |
| --- | --- |
| [文档索引](docs/README.md) | 全部文档与阅读路径 |
| [架构概览](docs/architecture/overview.md) | 分层、决策摘要、术语 |
| [行为契约](docs/architecture/contracts.md) | 生命周期、并发、错误码、送达语义 |
| [构建与验证](docs/guides/build.md) | 依赖、target、开关、一条命令验收 |
| [ADR 索引](docs/decisions/README.md) | 关键决策的理由与代价 |
| [已知差距](docs/architecture/known-gaps.md) | 限制清单与设计定位 |

## 反馈与贡献

请在 [GitHub Issues](https://github.com/sunwenqi0713/Firefly/issues) 提交问题，注明提交号、操作系统、编译器、后端配置与最小复现步骤。提交 PR 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

仓库目前尚未提供 `LICENSE` 文件。
