#ifndef FIREFLY_FIREFLY_H_
#define FIREFLY_FIREFLY_H_

#include <memory>
#include <string>

#include "firefly/init.h"
#include "firefly/node/node.h"

/// @file firefly.h
/// @brief 库的单一入口头文件，包含初始化接口与节点定义。

#include <memory>
#include <string>

#include "firefly/init.h"
#include "firefly/node/node.h"

/*
 * 典型用法：所有工厂都返回 Result，取 value 前先判断 bool。
 *
 *   #include "firefly/firefly.h"
 *
 *   int main(int argc, char* argv[]) {
 *     firefly::Init(argv[0]);
 *     auto node_r = firefly::CreateNode("talker");
 *     if (!node_r) return 1;
 *     auto node = std::move(node_r.value);
 *
 *     // 发布订阅
 *     auto writer_r = node->CreateWriter<Chatter>("channel/chatter");
 *     if (!writer_r) return 2;
 *     auto writer = writer_r.value;
 *     writer->Write(msg);  // 返回 WriteStatus，kAccepted 不等于对端已收到
 *
 *     auto reader_r = node->CreateReader<Chatter>("channel/chatter",
 *         [](const firefly::MessageContext&,
 *            const std::shared_ptr<const Chatter>& msg) {
 *           return firefly::CallbackResult::kOk;  // 或 kDropped
 *         });
 *
 *     // 服务 / 客户端
 *     auto service_r = node->CreateService<Req, Resp>("calc",
 *         [](const std::shared_ptr<const Req>& req) {
 *           auto resp = std::make_shared<Resp>();
 *           return firefly::ServiceResult<Resp>{firefly::ErrorCode::kOk, resp};
 *         });
 *     auto client_r = node->CreateClient<Req, Resp>("calc");
 *     auto resp = client_r.value->SendRequest(req);  // resp.code 区分失败原因
 *
 *     firefly::Shutdown();               // 主动触发关停，不级联关闭端点
 *     firefly::WaitForShutdownFor(std::chrono::seconds(2));
 *     firefly::Clear();
 *     return 0;
 *   }
 */

#endif  // FIREFLY_FIREFLY_H_