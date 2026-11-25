cmake_minimum_required(VERSION 3.16)
project(Firefly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 构建拆分契约（详见 docs/guides/build.md）
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

# ---------------- 公开头边界 ----------------
# 对外只暴露 include/：src/ 是构建内部布局，不属于消费方可见的 include 根。
# 测试与辅助程序确实需要内部头时显式链接 firefly_internal（测试特权），
# 这样"消费者只依赖 include/"是可检查的，而不是靠约定。
add_library(firefly_internal INTERFACE)
target_include_directories(firefly_internal INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

# ---------------- Core object 集合：不得出现 <nng/...> 或 NNG 符号 ----------------
add_library(firefly_core_objects OBJECT
    src/runtime/init.cc
    src/runtime/runtime.cc
    src/core/log.cc
    src/core/endpoint_id.cc
    src/core/writer_core.cc
    src/core/reader_core.cc
    src/core/service_core.cc
    src/core/client_core.cc
    src/core/node.cc)
target_include_directories(firefly_core_objects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(firefly_core_objects PUBLIC
    protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly_core_objects PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})

# ---------------- 后端 object：角色实现与默认注入分开 ----------------
# 角色 object 可被同一测试进程显式链接两次（内存 + NNG），默认注入 object
# 每个后端各一份，最终链接只能出现一份。
function(configure_firefly_object target)
  target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC Threads::Threads)
  target_compile_definitions(${target} PUBLIC
      FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
endfunction()

add_library(firefly_memory_role_objects OBJECT
    src/backends/memory/transport.cc
    src/backends/memory/registry.cc)
configure_firefly_object(firefly_memory_role_objects)

add_library(firefly_memory_injection_objects OBJECT
    src/backends/memory/injection.cc)
configure_firefly_object(firefly_memory_injection_objects)

if(FIREFLY_WITH_NNG)
  add_library(firefly_nng_role_objects OBJECT
      src/backends/nng/transport.cc
      src/backends/nng/registry.cc)
  configure_firefly_object(firefly_nng_role_objects)
  target_link_libraries(firefly_nng_role_objects PUBLIC
      nng::nng protobuf::libprotobuf)

  add_library(firefly_nng_injection_objects OBJECT
      src/backends/nng/injection.cc)
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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(firefly_core PUBLIC protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly_core PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
add_library(firefly::core ALIAS firefly_core)

if(FIREFLY_WITH_NNG)
  add_library(firefly_nng STATIC
      $<TARGET_OBJECTS:firefly_nng_role_objects>
      $<TARGET_OBJECTS:firefly_nng_injection_objects>)
  target_include_directories(firefly_nng PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(firefly PUBLIC protobuf::libprotobuf Threads::Threads)
target_compile_definitions(firefly PUBLIC
    FIREFLY_WITH_NNG=${FIREFLY_WITH_NNG_DEFINE})
if(FIREFLY_WITH_NNG)
  target_link_libraries(firefly PUBLIC nng::nng)
endif()
if(MSVC)
  target_compile_options(firefly PUBLIC /utf-8 /EHsc)
endif()

# 公开边界自检：对外 target 的 INTERFACE include 只能是 include/。若日后有人
# 把 src/ 加回 PUBLIC，配置阶段直接失败，避免"消费者悄悄依赖私有布局"。
function(firefly_assert_public_includes target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_public_includes ${target}
                      INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_dir IN LISTS _public_includes)
    string(FIND "${_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/src" _src_pos)
    if(NOT _src_pos EQUAL -1)
      message(FATAL_ERROR
          "${target} 对外暴露了 ${CMAKE_CURRENT_SOURCE_DIR}/src；"
          "内部头路径只能经 firefly_internal 提供给测试")
    endif()
  endforeach()
endfunction()

firefly_assert_public_includes(firefly)
firefly_assert_public_includes(firefly_core)
firefly_assert_public_includes(firefly_nng)

# BUILD_TESTING 由 CTest 定义（默认 ON）。作为 add_subdirectory 的被集成方时，
# 上层常常把 BUILD_TESTING 关掉：此时不应再要求 protoc、不应生成测试与示例，
# 只产出库。协议生成的 test_messages 也一并只在开启测试时构造。
include(CTest)

if(BUILD_TESTING)

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

# 测试特权：确需包含 core/、backends/ 内部头的用例显式声明，消费者拿不到。
function(use_firefly_internal target)
  target_link_libraries(${target} PRIVATE firefly_internal)
endfunction()

add_executable(firefly_header_check tests/header_check.cpp)
target_link_libraries(firefly_header_check PRIVATE firefly)
add_test(NAME header_check COMMAND firefly_header_check)

# 端到端测试：先由 protoc 生成测试消息代码
set(TEST_MESSAGES_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/test_messages.proto")
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
    COMMAND ${PROTOC_EXE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
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
  add_firefly_test(firefly_discovery_test tests/integration/discovery_test.cpp)
  use_firefly_internal(firefly_discovery_test)
endif()
add_firefly_test(firefly_e2e_test tests/integration/e2e_test.cpp)
add_firefly_test(firefly_reader_test tests/integration/reader_test.cpp)
add_firefly_test(firefly_client_test tests/integration/client_test.cpp)

# 阶段一验收：错误码 / Write 状态 / RPC 错误 / 空消息 / 旧 API
add_firefly_test(firefly_error_code_test tests/error_code_test.cpp)
use_firefly_internal(firefly_error_code_test)
add_firefly_test(firefly_write_status_test tests/write_status_test.cpp)
use_firefly_internal(firefly_write_status_test)
add_firefly_test(firefly_rpc_error_test tests/rpc_error_test.cpp)
use_firefly_internal(firefly_rpc_error_test)
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
add_firefly_test(firefly_fake_transport_test tests/contract/fake_transport_test.cpp)
use_firefly_internal(firefly_fake_transport_test)
add_firefly_test(firefly_creation_failure_test tests/contract/creation_failure_test.cpp)
use_firefly_internal(firefly_creation_failure_test)
add_firefly_test(firefly_clear_wait_test tests/clear_wait_test.cpp)
add_firefly_test(firefly_message_traits_test tests/message_traits_test.cpp)

# 审查回归 + 并发时序与发现记录契约
add_firefly_test(firefly_review_regression_test tests/review_regression_test.cpp)
add_firefly_test(firefly_concurrency_contract_test tests/contract/concurrency_contract_test.cpp)
add_firefly_test(firefly_registry_contract_test tests/contract/registry_contract_test.cpp)
use_firefly_internal(firefly_registry_contract_test)

# RPC 时序：排队到期、提交点取消、关停唤醒在途 I/O（可控 Registry + 计数
# Requester，固定时序而不依赖真实服务端的 sleep 竞猜）。
add_firefly_test(firefly_rpc_timing_test tests/contract/rpc_timing_test.cpp)
use_firefly_internal(firefly_rpc_timing_test)

# G1 回归：Follower 定期对账必须能移除漏掉注销广播的过期记录，且查询
# 失败时不得清空本地缓存。两个模式各自独立，进程内用临时端口，可并行。
if(FIREFLY_WITH_NNG)
  add_executable(firefly_follower_reconcile_test
      tests/contract/follower_reconcile_test.cpp)
  target_link_libraries(firefly_follower_reconcile_test
      PRIVATE firefly test_messages)
  use_firefly_internal(firefly_follower_reconcile_test)
  add_test(NAME follower_reconcile_delete
           COMMAND firefly_follower_reconcile_test delete)
  add_test(NAME follower_reconcile_query_fail
           COMMAND firefly_follower_reconcile_test query-fail)
  # P1 回归：广播字段转义（名称含 '|' 与 '\' 时不得切错位、不得让发现线程抛异常）。
  add_test(NAME follower_reconcile_escaped_name
           COMMAND firefly_follower_reconcile_test escaped-name)
endif()

# 共享后端契约：同一测试源分别用内存与 NNG 显式后端各跑一遍。
add_executable(firefly_backend_contract_test tests/contract/backend_contract_test.cpp)
target_link_libraries(firefly_backend_contract_test PRIVATE firefly test_messages)
use_firefly_internal(firefly_backend_contract_test)
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
  add_executable(firefly_interop_helper tests/integration/interop_helper.cpp)
  target_link_libraries(firefly_interop_helper PRIVATE firefly test_messages)
  use_firefly_internal(firefly_interop_helper)

  # 跨进程两组用例：每组独立发现地址，可并行运行，不共享 RESOURCE_LOCK。
  if(NOT Python3_Interpreter_FOUND)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()
  function(add_interop_pair name mode_a mode_b marker_a marker_b rep_port pub_port)
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_interop_pair.py
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
    concurrency_contract_test registry_contract_test)
if(FIREFLY_WITH_NNG)
  list(APPEND FIREFLY_PORT_LOCK_TESTS discovery_test)
endif()
set_tests_properties(${FIREFLY_PORT_LOCK_TESTS}
    PROPERTIES RESOURCE_LOCK firefly_registry_port)

# ---------------- 使用示例 ----------------
# 示例复用 tests/support 的 proto 消息，因此与测试一起构建；关掉 BUILD_TESTING
# 只得到库，这正是 add_subdirectory 集成方想要的形态。
add_executable(firefly_pubsub_example examples/pubsub_chat.cpp)
target_link_libraries(firefly_pubsub_example PRIVATE firefly test_messages)

add_executable(firefly_rpc_example examples/rpc_calculator.cpp)
target_link_libraries(firefly_rpc_example PRIVATE firefly test_messages)

endif()  # BUILD_TESTING

# ---------------- 安装与包配置 ----------------
# 三个 target 各自自包含（firefly 聚合 / firefly_core 无 NNG / firefly_nng 仅
# 后端），消费方按需链接其中一个，不要同时链接 firefly 与 firefly_core：两者
# 都包含 Core object，会重复定义符号。
set(FIREFLY_INSTALL_TARGETS firefly firefly_core)
if(FIREFLY_WITH_NNG)
  list(APPEND FIREFLY_INSTALL_TARGETS firefly_nng)
endif()

install(TARGETS ${FIREFLY_INSTALL_TARGETS}
        EXPORT FireflyTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include)
install(DIRECTORY include/ DESTINATION include)

install(EXPORT FireflyTargets
        FILE FireflyTargets.cmake
        NAMESPACE firefly::
        DESTINATION lib/cmake/firefly)

include(CMakePackageConfigHelpers)
set(FIREFLY_CONFIG_WITH_NNG ${FIREFLY_WITH_NNG})
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fireflyConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fireflyConfig.cmake"
    INSTALL_DESTINATION lib/cmake/firefly)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/fireflyConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/fireflyConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/fireflyConfigVersion.cmake"
        DESTINATION lib/cmake/firefly)
