cmake_minimum_required(VERSION 3.16)
project(Firefly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 第 4 批构建拆分契约（详见 docs/build.md）
#   - firefly_core_objects：四 Core + 运行时 + 四角色接口 + IRegistry，零 NNG。
#   - firefly_memory_objects / firefly_nng_objects：两个互斥的默认注入后端。
#   - firefly_core：Core object + 内存后端，自包含且从不链接 NNG。
#   - firefly_nng：仅 NNG 后端 object（默认 ON 时构建）。
#   - firefly::firefly：Core object + 当前后端 object；ON=NNG，OFF=内存。
# FIREFLY_WITH_NNG=OFF 时不调用 find_package(nng)，不编译任何 NNG 源文件。
# ---------------------------------------------------------------------------
option(FIREFLY_WITH_NNG "Build the NNG transport and discovery backend" ON)

find_package(protobuf CONFIG REQUIRED)
find_package(Threads REQUIRED)       # 提供 std::thread；Linux 上等价于 -pthread
if(FIREFLY_WITH_NNG)
  find_package(nng CONFIG REQUIRED)  # 仅 NNG 后端需要
  message(STATUS "Firefly: NNG backend ENABLED")
  set(FIREFLY_WITH_NNG_DEFINE 1)
else()
  message(STATUS "Firefly: NNG backend DISABLED (in-memory backend selected)")
  set(FIREFLY_WITH_NNG_DEFINE 0)
endif()

# ---------------- Core object 集合：不得出现 <nng/...> 或 NNG 符号 ----------------
add_library(firefly_core_objects OBJECT
    firefly/init.cc
    firefly/runtime.cc
    firefly/common/log.cc
    firefly/detail/endpoint_id.cc
    firefly/detail/writer_core.cc
    firefly/detail/reader_core.cc
    firefly/detail/service_core.cc
    firefly/detail/client_core.cc
    firefly/node/node.cc)
target_include_directories(firefly_core_objects PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(firefly_core_objects PUBLIC
    protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly_core_objects PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})

# ---------------- 后端 object：角色实现与默认注入分开 ----------------
# 角色 object 可被同一测试进程显式链接两次（内存 + NNG），默认注入 object
# 每个后端各一份，最终链接只能出现一份。
function(configure_firefly_object target)
  target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
  target_link_libraries(${target} PUBLIC Threads::Threads)
  target_compile_definitions(${target} PUBLIC
      FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
endfunction()

add_library(firefly_memory_role_objects OBJECT
    firefly/detail/memory_transport.cc
    firefly/detail/memory_registry.cc)
configure_firefly_object(firefly_memory_role_objects)

add_library(firefly_memory_injection_objects OBJECT
    firefly/detail/memory_injection.cc)
configure_firefly_object(firefly_memory_injection_objects)

if(FIREFLY_WITH_NNG)
  add_library(firefly_nng_role_objects OBJECT
      firefly/detail/transport_nng.cc
      firefly/detail/discovery.cc)
  configure_firefly_object(firefly_nng_role_objects)
  target_link_libraries(firefly_nng_role_objects PUBLIC
      nng::nng protobuf::libprotobuf)

  add_library(firefly_nng_injection_objects OBJECT
      firefly/detail/nng_injection.cc)
  configure_firefly_object(firefly_nng_injection_objects)
  target_link_libraries(firefly_nng_injection_objects PUBLIC nng::nng)
endif()

# 源码/头文件均以 UTF-8 编写，避免 MSVC 按 GBK 解析中文注释
if(MSVC)
  set(FIREFLY_ALL_OBJECT_LIBS
      firefly_core_objects
      firefly_memory_role_objects
      firefly_memory_injection_objects)
  if(FIREFLY_WITH_NNG)
    list(APPEND FIREFLY_ALL_OBJECT_LIBS
         firefly_nng_role_objects firefly_nng_injection_objects)
  endif()
  foreach(objlib IN LISTS FIREFLY_ALL_OBJECT_LIBS)
    target_compile_options(${objlib} PUBLIC /utf-8 /EHsc)
  endforeach()
endif()

# ---------------- 可独立检查的 target ----------------
add_library(firefly_core STATIC
    $<TARGET_OBJECTS:firefly_core_objects>
    $<TARGET_OBJECTS:firefly_memory_role_objects>
    $<TARGET_OBJECTS:firefly_memory_injection_objects>)
target_include_directories(firefly_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(firefly_core PUBLIC protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly_core PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
add_library(firefly::core ALIAS firefly_core)

if(FIREFLY_WITH_NNG)
  add_library(firefly_nng STATIC
      $<TARGET_OBJECTS:firefly_nng_role_objects>
      $<TARGET_OBJECTS:firefly_nng_injection_objects>)
  target_include_directories(firefly_nng PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
  target_link_libraries(firefly_nng PUBLIC
      nng::nng protobuf::libprotobuf Threads::Threads)
  add_library(firefly::nng ALIAS firefly_nng)
  set(FIREFLY_BACKEND_OBJECTS
      $<TARGET_OBJECTS:firefly_nng_role_objects>
      $<TARGET_OBJECTS:firefly_nng_injection_objects>)
else()
  set(FIREFLY_BACKEND_OBJECTS
      $<TARGET_OBJECTS:firefly_memory_role_objects>
      $<TARGET_OBJECTS:firefly_memory_injection_objects>)
endif()

# ---------------- 默认聚合 target：对外接口与阶段三一致 ----------------
add_library(firefly STATIC
    $<TARGET_OBJECTS:firefly_core_objects>
    ${FIREFLY_BACKEND_OBJECTS})
add_library(firefly::firefly ALIAS firefly)
target_include_directories(firefly PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(firefly PUBLIC protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
if(FIREFLY_WITH_NNG)
  target_link_libraries(firefly PUBLIC nng::nng)
endif()
if(MSVC)
  target_compile_options(firefly PUBLIC /utf-8 /EHsc)
endif()

enable_testing()

# 零泄漏脚本化验收：公共头不得 include nng/*_impl.h/discovery
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME no_leak_check
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_leak.py
                   ${CMAKE_CURRENT_SOURCE_DIR})
endif()

function(add_firefly_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE firefly test_messages)
  string(REGEX REPLACE "^firefly_" "" testname "${name}")
  add_test(NAME ${testname} COMMAND ${name})
endfunction()

add_executable(firefly_header_check tests/header_check.cpp)
target_link_libraries(firefly_header_check PRIVATE firefly)
add_test(NAME header_check COMMAND firefly_header_check)

# 端到端测试：先由 protoc 生成测试消息代码
set(TEST_MESSAGES_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_messages.proto")
set(TEST_MESSAGES_OUT "${CMAKE_CURRENT_BINARY_DIR}/tests/gen")
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

# 发现中心 / Reader / Service / Client 单元测试
if(FIREFLY_WITH_NNG)
  add_firefly_test(firefly_discovery_test tests/discovery_test.cpp)
endif()
add_firefly_test(firefly_e2e_test tests/e2e_test.cpp)
add_firefly_test(firefly_reader_test tests/reader_test.cpp)
add_firefly_test(firefly_client_test tests/client_test.cpp)

# 阶段一验收：错误码 / Write 状态 / RPC 错误 / 空消息 / 旧 API
add_firefly_test(firefly_error_code_test tests/error_code_test.cpp)
add_firefly_test(firefly_write_status_test tests/write_status_test.cpp)
add_firefly_test(firefly_rpc_error_test tests/rpc_error_test.cpp)
add_firefly_test(firefly_empty_message_test tests/empty_message_test.cpp)
add_firefly_test(firefly_deprecated_compat_test tests/deprecated_compat_test.cpp)

# 阶段二验收：关停 / 取消 / 查重 / 端点生命周期 / 回调契约
add_firefly_test(firefly_shutdown_test tests/shutdown_test.cpp)
add_firefly_test(firefly_request_cancel_test tests/request_cancel_test.cpp)
add_firefly_test(firefly_duplicate_test tests/duplicate_test.cpp)
add_firefly_test(firefly_endpoint_lifecycle_test tests/endpoint_lifecycle_test.cpp)
add_firefly_test(firefly_callback_contract_test tests/callback_contract_test.cpp)

# 阶段三验收：上下文回调 / 统计 / fake 注入 / Clear 等待 / schema
add_firefly_test(firefly_callback_context_test tests/callback_context_test.cpp)
add_firefly_test(firefly_stats_test tests/stats_test.cpp)
add_firefly_test(firefly_fake_transport_test tests/fake_transport_test.cpp)
add_firefly_test(firefly_creation_failure_test tests/creation_failure_test.cpp)
add_firefly_test(firefly_clear_wait_test tests/clear_wait_test.cpp)
add_firefly_test(firefly_message_traits_test tests/message_traits_test.cpp)

# 审查回归 + 第 1/3 批行为契约
add_firefly_test(firefly_review_regression_test tests/review_regression_test.cpp)
add_firefly_test(firefly_batch1_contract_test tests/batch1_contract_test.cpp)
add_firefly_test(firefly_batch3_contract_test tests/batch3_contract_test.cpp)

# 第 4 批共享后端契约：同一测试源分别用内存与 NNG 显式后端各跑一遍。
add_executable(firefly_backend_contract_test tests/backend_contract_test.cpp)
target_link_libraries(firefly_backend_contract_test PRIVATE firefly test_messages)
if(FIREFLY_WITH_NNG)
  # 默认聚合库含 NNG 注入；这里补上内存角色 object（不含注入）避免符号重复。
  target_link_libraries(firefly_backend_contract_test PRIVATE
      firefly_memory_role_objects)
  add_test(NAME backend_contract_memory
           COMMAND firefly_backend_contract_test memory)
  add_test(NAME backend_contract_nng
           COMMAND firefly_backend_contract_test nng)
  set_tests_properties(backend_contract_nng PROPERTIES
      RESOURCE_LOCK firefly_registry_port
      ENVIRONMENT "FIREFLY_DISCOVERY_REP_URL=tcp://127.0.0.1:37401;FIREFLY_DISCOVERY_PUB_URL=tcp://127.0.0.1:37402")
else()
  add_test(NAME backend_contract_memory
           COMMAND firefly_backend_contract_test memory)
endif()

# 旧版 raw-NNG 进程与新版公开 API 进程的线兼容辅助程序（仅 NNG 后端）
if(FIREFLY_WITH_NNG)
  add_executable(firefly_interop_helper tests/interop_helper.cpp)
  target_link_libraries(firefly_interop_helper PRIVATE firefly test_messages)

  # 跨进程两组用例：每组独立发现地址，可并行运行，不共享 RESOURCE_LOCK。
  if(NOT Python3_Interpreter_FOUND)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()
  function(add_interop_pair name mode_a mode_b marker_a marker_b rep_port pub_port)
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_interop_pair.py
                     $<TARGET_FILE:firefly_interop_helper>
                     ${mode_a} ${mode_b} ${marker_a} ${marker_b} ${ARGN})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT
        "FIREFLY_DISCOVERY_REP_URL=tcp://127.0.0.1:${rep_port};FIREFLY_DISCOVERY_PUB_URL=tcp://127.0.0.1:${pub_port}")
  endfunction()
  # mode_b 先启动并打印 READY 后才启动 mode_a；mode_b 因而稳定成为发现主实例。
  add_interop_pair(cross_pubsub_legacy_to_new
      legacy_pub new_sub LEGACY_PUB_OK NEW_SUB_OK 37501 37502 NEW_SUB_READY)
  add_interop_pair(cross_pubsub_new_to_legacy
      legacy_sub new_pub LEGACY_SUB_OK NEW_PUB_OK 37503 37504 NEW_PUB_READY)
  add_interop_pair(cross_rpc_legacy_to_new
      new_req legacy_rep NEW_REQ_OK LEGACY_REP_OK 37505 37506 LEGACY_REP_READY)
  add_interop_pair(cross_rpc_new_to_legacy
      legacy_req new_rep LEGACY_REQ_OK NEW_REP_OK 37507 37508 NEW_REP_READY)
  # 三项 P1 回归：旧注册协议、Follower 初始快照、Writer 先启 Reader 后启。
  add_interop_pair(cross_registry_legacy_follower_new_primary
      legacy_reg_client new_reg_primary LEGACY_REG_OK NEW_REG_PRIMARY_OK
      37509 37510 NEW_REG_PRIMARY_READY)
  add_interop_pair(cross_registry_new_follower_legacy_primary
      new_reg_follower legacy_reg_primary NEW_REG_FOLLOWER_OK LEGACY_REG_PRIMARY_OK
      37511 37512 LEGACY_REG_PRIMARY_READY)
  add_interop_pair(cross_pubsub_writer_first_reader_late
      new_sub_late new_pub_first NEW_SUB_LATE_OK NEW_PUB_FIRST_OK
      37513 37514 NEW_PUB_FIRST_READY)
endif()

# 端口隔离：所有依赖注册中心的测试共享同一 RESOURCE_LOCK。
set(FIREFLY_PORT_LOCK_TESTS
    e2e_test reader_test client_test
    error_code_test write_status_test rpc_error_test empty_message_test
    deprecated_compat_test shutdown_test request_cancel_test duplicate_test
    endpoint_lifecycle_test callback_contract_test callback_context_test
    stats_test fake_transport_test creation_failure_test clear_wait_test review_regression_test
    batch1_contract_test batch3_contract_test)
if(FIREFLY_WITH_NNG)
  list(APPEND FIREFLY_PORT_LOCK_TESTS discovery_test)
endif()
set_tests_properties(${FIREFLY_PORT_LOCK_TESTS}
    PROPERTIES RESOURCE_LOCK firefly_registry_port)

# ---------------- 使用示例 ----------------
add_executable(firefly_pubsub_example examples/pubsub_chat.cpp)
target_link_libraries(firefly_pubsub_example PRIVATE firefly test_messages)

add_executable(firefly_rpc_example examples/rpc_calculator.cpp)
target_link_libraries(firefly_rpc_example PRIVATE firefly test_messages)
