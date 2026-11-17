cmake_minimum_required(VERSION 3.16)
project(Firefly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 依赖：protobuf + nng（vcpkg manifest 模式，见 vcpkg.json）
find_package(protobuf CONFIG REQUIRED)
find_package(nng CONFIG REQUIRED)    # 网络传输层
find_package(Threads REQUIRED)       # 提供 std::thread；Linux 上等价于 -pthread

# 静态库：接口 + 运行时 + 注册中心实现
add_library(firefly STATIC
    firefly/init.cc
    firefly/runtime.cc
    firefly/common/log.cc
    firefly/detail/endpoint_id.cc
    firefly/detail/discovery.cc
    firefly/detail/transport_nng.cc
    firefly/detail/writer_core.cc
    firefly/detail/reader_core.cc
    firefly/detail/service_core.cc
    firefly/detail/client_core.cc
    firefly/node/node.cc)
add_library(firefly::firefly ALIAS firefly)
target_include_directories(firefly PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(firefly PUBLIC
    protobuf::libprotobuf
    nng::nng
    Threads::Threads)

# 源码/头文件均以 UTF-8 编写，避免 MSVC 按 GBK 解析中文注释
if(MSVC)
  target_compile_options(firefly PUBLIC /utf-8 /EHsc)
endif()

# 头文件解析校验（不实例化尚未实现的 detail 工厂）
enable_testing()

# 零泄漏脚本化验收（阶段三）：公共头不得 include nng/*_impl.h/discovery
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME no_leak_check
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_leak.py
                   ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_executable(firefly_header_check tests/header_check.cpp)
target_link_libraries(firefly_header_check PRIVATE firefly)
add_test(NAME header_check COMMAND firefly_header_check)

# 注册中心单元测试（主实例：注册/列表/订阅派发）
add_executable(firefly_discovery_test tests/discovery_test.cpp)
target_link_libraries(firefly_discovery_test PRIVATE firefly)
add_test(NAME discovery_test COMMAND firefly_discovery_test)

# 端到端测试：先由 protoc 生成测试消息代码
set(TEST_MESSAGES_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_messages.proto")
set(TEST_MESSAGES_OUT "${CMAKE_CURRENT_BINARY_DIR}/tests/gen")
find_package(protobuf CONFIG REQUIRED)
find_program(PROTOC_EXE protoc HINTS
    "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows/tools/protobuf")
if(NOT PROTOC_EXE)
  find_program(PROTOC_EXE NAMES protoc REQUIRED)
endif()

add_custom_command(
    OUTPUT ${TEST_MESSAGES_OUT}/test_messages.pb.cc
           ${TEST_MESSAGES_OUT}/test_messages.pb.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_MESSAGES_OUT}
    COMMAND ${PROTOC_EXE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests
            --cpp_out=${TEST_MESSAGES_OUT} ${TEST_MESSAGES_PROTO}
    DEPENDS ${TEST_MESSAGES_PROTO}
    COMMENT "Protoc generating test_messages"
    VERBATIM)

add_library(test_messages STATIC
    ${TEST_MESSAGES_OUT}/test_messages.pb.cc)
target_include_directories(test_messages PUBLIC ${TEST_MESSAGES_OUT})
target_link_libraries(test_messages PUBLIC protobuf::libprotobuf)

add_executable(firefly_e2e_test tests/e2e_test.cpp)
target_link_libraries(firefly_e2e_test PRIVATE firefly test_messages)
add_test(NAME e2e_test COMMAND firefly_e2e_test)

# Reader API 单元测试（Observe/回调/缓存深度/在线关系）
add_executable(firefly_reader_test tests/reader_test.cpp)
target_link_libraries(firefly_reader_test PRIVATE firefly test_messages)
add_test(NAME reader_test COMMAND firefly_reader_test)

# Service/Client 单元测试（就绪等待/同步/异步/超时失败路径）
add_executable(firefly_client_test tests/client_test.cpp)
target_link_libraries(firefly_client_test PRIVATE firefly test_messages)
add_test(NAME client_test COMMAND firefly_client_test)

# 阶段一验收：错误码 / Write 状态 / RPC 错误可传递 / 空消息契约
add_executable(firefly_error_code_test tests/error_code_test.cpp)
target_link_libraries(firefly_error_code_test PRIVATE firefly test_messages)
add_test(NAME error_code_test COMMAND firefly_error_code_test)

add_executable(firefly_write_status_test tests/write_status_test.cpp)
target_link_libraries(firefly_write_status_test PRIVATE firefly test_messages)
add_test(NAME write_status_test COMMAND firefly_write_status_test)

add_executable(firefly_rpc_error_test tests/rpc_error_test.cpp)
target_link_libraries(firefly_rpc_error_test PRIVATE firefly test_messages)
add_test(NAME rpc_error_test COMMAND firefly_rpc_error_test)

add_executable(firefly_empty_message_test tests/empty_message_test.cpp)
target_link_libraries(firefly_empty_message_test PRIVATE firefly test_messages)
add_test(NAME empty_message_test COMMAND firefly_empty_message_test)

# 旧 API 灰度：deprecated 路径仍可编译
add_executable(firefly_deprecated_compat_test tests/deprecated_compat_test.cpp)
target_link_libraries(firefly_deprecated_compat_test PRIVATE firefly test_messages)
add_test(NAME deprecated_compat_test COMMAND firefly_deprecated_compat_test)

# 阶段二验收：关停 / 取消 / 查重 / 端点生命周期 / 回调契约
add_executable(firefly_shutdown_test tests/shutdown_test.cpp)
target_link_libraries(firefly_shutdown_test PRIVATE firefly test_messages)
add_test(NAME shutdown_test COMMAND firefly_shutdown_test)

add_executable(firefly_request_cancel_test tests/request_cancel_test.cpp)
target_link_libraries(firefly_request_cancel_test PRIVATE firefly test_messages)
add_test(NAME request_cancel_test COMMAND firefly_request_cancel_test)

add_executable(firefly_duplicate_test tests/duplicate_test.cpp)
target_link_libraries(firefly_duplicate_test PRIVATE firefly test_messages)
add_test(NAME duplicate_test COMMAND firefly_duplicate_test)

add_executable(firefly_endpoint_lifecycle_test tests/endpoint_lifecycle_test.cpp)
target_link_libraries(firefly_endpoint_lifecycle_test PRIVATE firefly test_messages)
add_test(NAME endpoint_lifecycle_test COMMAND firefly_endpoint_lifecycle_test)

add_executable(firefly_callback_contract_test tests/callback_contract_test.cpp)
target_link_libraries(firefly_callback_contract_test PRIVATE firefly test_messages)
add_test(NAME callback_contract_test COMMAND firefly_callback_contract_test)

# 阶段三验收：上下文回调 / 实时统计 / fake 注入 / Clear 等待 / schema 演进
add_executable(firefly_callback_context_test tests/callback_context_test.cpp)
target_link_libraries(firefly_callback_context_test PRIVATE firefly test_messages)
add_test(NAME callback_context_test COMMAND firefly_callback_context_test)

add_executable(firefly_stats_test tests/stats_test.cpp)
target_link_libraries(firefly_stats_test PRIVATE firefly test_messages)
add_test(NAME stats_test COMMAND firefly_stats_test)

add_executable(firefly_fake_transport_test tests/fake_transport_test.cpp)
target_link_libraries(firefly_fake_transport_test PRIVATE firefly test_messages)
add_test(NAME fake_transport_test COMMAND firefly_fake_transport_test)

add_executable(firefly_clear_wait_test tests/clear_wait_test.cpp)
target_link_libraries(firefly_clear_wait_test PRIVATE firefly test_messages)
add_test(NAME clear_wait_test COMMAND firefly_clear_wait_test)

add_executable(firefly_message_traits_test tests/message_traits_test.cpp)
target_link_libraries(firefly_message_traits_test PRIVATE firefly test_messages)
add_test(NAME message_traits_test COMMAND firefly_message_traits_test)

# ---------------- 使用示例 ----------------

# 发布 / 订阅示例：回调 + Observe 两种订阅模式
add_executable(firefly_pubsub_example examples/pubsub_chat.cpp)
target_link_libraries(firefly_pubsub_example PRIVATE firefly test_messages)

# 服务 / 客户端示例：同步 / 异步请求
add_executable(firefly_rpc_example examples/rpc_calculator.cpp)
target_link_libraries(firefly_rpc_example PRIVATE firefly test_messages)
