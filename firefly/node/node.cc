#include "firefly/node/node.h"

#include <memory>
#include <string>
#include <utility>

#include "firefly/common/log.h"

namespace firefly {

// Node 的内部实现占位（后续阶段可承载拓扑注册等元信息）。
struct Node::Impl {};

std::unique_ptr<Node> CreateNode(const std::string& node_name,
                                 const std::string& name_space) {
  return std::unique_ptr<Node>(new Node(node_name, name_space));
}

Node::Node(const std::string& node_name, const std::string& name_space)
    : node_name_(node_name), name_space_(name_space), impl_(new Impl) {
  FY_INFO("node[%s] created", node_name_.c_str());
}

Node::~Node() {
  for (auto& kv : readers_) {
    kv.second->Shutdown();
  }
  FY_INFO("node[%s] destroyed", node_name_.c_str());
}

bool Node::DeleteReader(const std::string& channel_name) {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  auto it = readers_.find(channel_name);
  if (it == readers_.end()) {
    return false;  // 不存在
  }
  it->second->Shutdown();
  readers_.erase(it);
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