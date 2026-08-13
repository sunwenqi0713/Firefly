#ifndef FIREFLY_NODE_NODE_H_
#define FIREFLY_NODE_NODE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "firefly/common/types.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/reader.h"
#include "firefly/node/reader_base.h"
#include "firefly/node/writer.h"
#include "firefly/service/client.h"
#include "firefly/service/service.h"

namespace firefly {

class Node;

// 创建节点（节点是通信的载体：读写者、服务/客户端都在节点上创建）
std::unique_ptr<Node> CreateNode(const std::string& node_name,
                                 const std::string& name_space = "");

/**
 * @class Node
 * @brief 通信的基本单元。一个模块通过 Node 定义读写者与/或服务客户端。
 * @warning 拓扑对象（节点、读写者、服务等）名称不允许重复。
 */
class Node {
 public:
  friend std::unique_ptr<Node> CreateNode(const std::string&, const std::string&);

  // 定义于 node.cc（实现阶段补充），以保证唯一指针 impl_ 在完整类型处析构
  virtual ~Node();

  const std::string& Name() const { return node_name_; }
  const std::string& NameSpace() const { return name_space_; }

  // ---------------- 发布 / 订阅 ----------------

  // 创建发布者（按通道名，使用默认 QoS）
  template <typename MessageT>
  std::shared_ptr<Writer<MessageT>> CreateWriter(const std::string& channel_name);

  // 创建发布者（按通道配置，可指定 QoS）
  template <typename MessageT>
  std::shared_ptr<Writer<MessageT>> CreateWriter(const ChannelConfig& config);

  // 创建订阅者（按通道名，使用默认配置；可传回调）
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> CreateReader(
      const std::string& channel_name,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  // 创建订阅者（按 ReaderConfig 配置）
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> CreateReader(
      const ReaderConfig& config,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  // 删除订阅者（同一节点重复删除会失败）
  bool DeleteReader(const std::string& channel_name);

  // 获取已创建的 Reader，不存在或类型不符返回 nullptr
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> GetReader(const std::string& channel_name);

  // 把节点下所有 Reader 的新消息拉取到已观察缓存
  void Observe();
  // 清空节点下所有 Reader 的缓存数据
  void ClearData();

  // ---------------- 服务 / 客户端 ----------------

  // 创建服务（service_callback 处理请求并填充响应）
  template <typename Request, typename Response>
  std::shared_ptr<Service<Request, Response>> CreateService(
      const std::string& service_name,
      const typename Service<Request, Response>::ServiceCallback& service_callback);

  // 创建指向指定服务的客户端
  template <typename Request, typename Response>
  std::shared_ptr<Client<Request, Response>> CreateClient(
      const std::string& service_name);

 private:
  Node(const std::string& node_name, const std::string& name_space = "");

  std::string node_name_;
  std::string name_space_;

  std::mutex readers_mutex_;
  std::map<std::string, std::shared_ptr<ReaderBase>> readers_;

  // 内部实现（拓扑注册、服务发现等），实现阶段补充
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

template <typename MessageT>
std::shared_ptr<Writer<MessageT>> Node::CreateWriter(
    const std::string& channel_name) {
  ChannelConfig config;
  config.channel_name = channel_name;
  return CreateWriter<MessageT>(config);
}

template <typename MessageT>
std::shared_ptr<Writer<MessageT>> Node::CreateWriter(const ChannelConfig& config) {
  auto writer = std::make_shared<Writer<MessageT>>(config);
  if (writer == nullptr || !writer->Init()) {
    return nullptr;
  }
  return writer;
}

template <typename MessageT>
std::shared_ptr<Reader<MessageT>> Node::CreateReader(
    const std::string& channel_name,
    const CallbackFunc<MessageT>& reader_func) {
  ReaderConfig config;
  config.channel_name = channel_name;
  return CreateReader<MessageT>(config, reader_func);
}

template <typename MessageT>
std::shared_ptr<Reader<MessageT>> Node::CreateReader(
    const ReaderConfig& config, const CallbackFunc<MessageT>& reader_func) {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  if (readers_.find(config.channel_name) != readers_.end()) {
    return nullptr;  // 同一节点不允许对同一通道创建多个 Reader
  }
  auto reader = std::make_shared<Reader<MessageT>>(config, reader_func);
  if (reader == nullptr || !reader->Init()) {
    return nullptr;
  }
  readers_.emplace(config.channel_name, reader);
  return reader;
}

template <typename MessageT>
std::shared_ptr<Reader<MessageT>> Node::GetReader(
    const std::string& channel_name) {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  auto it = readers_.find(channel_name);
  if (it != readers_.end()) {
    return std::dynamic_pointer_cast<Reader<MessageT>>(it->second);
  }
  return nullptr;
}

template <typename Request, typename Response>
std::shared_ptr<Service<Request, Response>> Node::CreateService(
    const std::string& service_name,
    const typename Service<Request, Response>::ServiceCallback& service_callback) {
  auto service = std::make_shared<Service<Request, Response>>(
      node_name_, service_name, service_callback);
  if (service == nullptr || !service->Init()) {
    return nullptr;
  }
  return service;
}

template <typename Request, typename Response>
std::shared_ptr<Client<Request, Response>> Node::CreateClient(
    const std::string& service_name) {
  auto client =
      std::make_shared<Client<Request, Response>>(node_name_, service_name);
  if (client == nullptr || !client->Init()) {
    return nullptr;
  }
  return client;
}

}  // namespace firefly

#endif  // FIREFLY_NODE_NODE_H_