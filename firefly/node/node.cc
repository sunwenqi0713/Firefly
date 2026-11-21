#include "firefly/node/node.h"

#include <memory>
#include <string>
#include <utility>

#include "firefly/common/log.h"
#include "firefly/runtime.h"

namespace firefly {

// Node 的内部实现占位（后续阶段可承载拓扑注册等元信息）。
struct Node::Impl {};

Result<std::unique_ptr<Node>> CreateNode(const std::string& node_name,
                                         const std::string& name_space) {
  return CreateNode(node_name, name_space, nullptr);
}

Result<std::unique_ptr<Node>> CreateNode(
    const std::string& node_name, const std::string& name_space,
    const std::shared_ptr<detail::InjectionContext>& ctx) {
  using R = Result<std::unique_ptr<Node>>;
  if (detail::ValidateNodeName(node_name) != ErrorCode::kOk) {
    return R{nullptr, ErrorCode::kInvalidName,
             "CreateNode[node='" + node_name + "']: invalid node name"};
  }
  // 进程内查重：复用 ShutdownManager 单例的节点名注册表（阶段二任务4）。
  if (!runtime::ShutdownManager::instance().RegisterNode(node_name)) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateNode[node='" + node_name +
                 "']: a Node with this name already exists in this process"};
  }
  return R{std::unique_ptr<Node>(new Node(node_name, name_space, ctx)),
           ErrorCode::kOk, ""};
}

Node::Node(const std::string& node_name, const std::string& name_space,
           const std::shared_ptr<detail::InjectionContext>& ctx)
    : node_name_(node_name), name_space_(name_space), ctx_(ctx), impl_(new Impl) {
  FY_INFO("node[%s] created", node_name_.c_str());
}

Node::~Node() {
  // Node 所有权契约（阶段二，选方案 b）：Node 析构只 Shutdown 由它持有的
  // readers_；Writer/Service/Client 由外部 shared_ptr 独立持有，Node 仅保存
  // weak_ptr 供查重/DeleteXxx 定位，析构时不关闭它们。DeleteXxx 是唯一的
  // 显式销毁途径。端点需在 Node 之前或之后由外部自行 Shutdown/释放。
  for (auto& kv : readers_) {
    kv.second->Shutdown();
  }
  runtime::ShutdownManager::instance().UnregisterNode(node_name_);
  FY_INFO("node[%s] destroyed", node_name_.c_str());
}

bool Node::DeleteReader(const std::string& channel_name) {
  // 先在锁内摘出指针并从表移除，再在锁外 Shutdown：
  // Shutdown 会 join 收线程并等待在途回调，回调内可能调用 GetReader/Observe
  // 需要 readers_mutex_；若持锁等待将自锁死锁（审查 P1）。
  std::shared_ptr<ReaderBase> victim;
  {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    auto it = readers_.find(channel_name);
    if (it == readers_.end()) {
      return false;  // 不存在
    }
    victim = it->second;
    readers_.erase(it);
  }
  victim->Shutdown();
  return true;
}

void Node::Observe() {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  for (auto& kv : readers_) {
    kv.second->Observe();
  }
}

void Node::ClearData() {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  for (auto& kv : readers_) {
    kv.second->ClearData();
  }
}

}  // namespace firefly