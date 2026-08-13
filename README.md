# Firefly

参考 Apollo Cyber RT 设计的一套 C++ 通信接口库（当前为**接口设计阶段**，未实现传输细节）：

- 发布 / 订阅：`Writer` / `Reader`（回调模式 + Observe 模式）
- 服务 / 客户端：`Service` / `Client`（同步 + 异步请求）

消息类型依赖 **protobuf**（`google::protobuf::Message` 派生类）。

## 目录结构

```
firefly/
  firefly.h             # 总入口：#include "firefly/firefly.h"
  init.h                # Init/OK/IsShutdown/WaitForShutdown/Clear
  common/types.h        # ReturnCode/Relation/QosProfile/RoleAttributes 等
  message/message_traits.h  # protobuf 序列化 trait 约束
  node/                 # Node / Writer / Reader / 配置
  service/              # Service / Client
tests/
  header_check.cpp      # 头文件解析校验（不实例化 detail 工厂）
vcpkg.json              # 依赖清单（protobuf）
CMakePresets.json       # 构建预设（指向 D:\vcpkg toolchain）
```

## 依赖

使用 [vcpkg](https://github.com/microsoft/vcpkg)（本机位于 `D:\vcpkg`）进行 manifest 模式依赖管理。

```text
protobuf
```

首次构建时 vcpkg 会自动下载并编译 protobuf 及其依赖（abseil 等）。

## 构建与测试

需要：CMake ≥ 3.16、Visual Studio 2022（含 C++ 桌面开发）、vcpkg。

### 一键脚本（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1          # 配置+构建+测试
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Action configure
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Action build
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Action test
```

脚本会自动加载 MSVC `vcvars64.bat` 环境（MSVC 编译器依赖该环境，普通 PowerShell 无法直接识别）。

### 手动（需在 VS Developer Command Prompt 中）

```powershell
# 1. 配置（会自动通过 vcpkg 安装 protobuf）
cmake --preset default

# 2. 编译
cmake --build --preset default

# 3. 运行测试（头文件解析校验）
ctest --preset default
```

`CMakePresets.json` 中的预设已把 toolchain 指向 `D:/vcpkg/scripts/buildsystems/vcpkg.cmake`，
triplet 为 `x64-windows`，并显式指定了 vcpkg 自带的 ninja。如需自定义，可复制预设并修改。

### 手动指定 vcpkg（未使用预设时）

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DCMAKE_BUILD_TYPE=Debug
```

## 网络 / 代理说明

protobuf 及 abseil 源码从 GitHub 下载。若网络无法直连 GitHub，请先启动代理，并通过代理访问 vcpkg：

```powershell
$env:HTTP_PROXY  = "http://<proxy-host>:<port>"
$env:HTTPS_PROXY = "http://<proxy-host>:<port>"
cmake --preset default
```

注意：若代理为 SOCKS5，应使用 `socks5://host:port` 格式，并确认端口/协议与代理实际一致。

## 状态

当前 `detail::*Handle` 工厂函数仅声明未实现，接口层可编译（头文件校验通过），
但实例化模板使用需等待实现层完成。