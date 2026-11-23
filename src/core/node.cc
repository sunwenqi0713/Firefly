#include "firefly/node/node.h"

#include <memory>
#include <string>
#include <utility>

#include "firefly/common/log.h"
#include "core/runtime.h"

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
  // 锁内标记"正在删除"，但保留 readers_ 表项：回调内并发 GetReader/Observe
  // 仍能正常拿到对象；同名 Reader 在 Shutdown 完成前一律 kDuplicate。
  std::shared_ptr<ReaderBase> victim;
  {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    auto it = readers_.find(channel_name);
    if (it == readers_.end() || readers_deleting_.count(channel_name) != 0) {
      return false;  // 不存在或已在删除中
    }
    victim = it->second;
    readers_deleting_.insert(channel_name);
  }
  // 节点锁外 Shutdown：join 收线程并等待在途回调，回调可能需要 readers_mutex_。
  victim->Shutdown();
  // Shutdown 完成（线程已退出、注册已注销）后才释放名称。
  {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    readers_deleting_.erase(channel_name);
    auto it = readers_.find(channel_name);
    if (it != readers_.end() && it->second == victim) readers_.erase(it);
  }
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