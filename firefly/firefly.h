#ifndef FIREFLY_FIREFLY_H_
#define FIREFLY_FIREFLY_H_

#include <memory>
#include <string>

#include "firefly/init.h"
#include "firefly/node/node.h"

/*
 * 典型用法：
 *
 *   #include "firefly/firefly.h"
 *
 *   int main(int argc, char* argv[]) {
 *     firefly::Init(argv[0]);
 *     auto node = firefly::CreateNode("talker");
 *
 *     // 发布订阅
 *     auto writer = node->CreateWriter<Chatter>("channel/chatter");
 *     writer->Write(msg);
 *
 *     auto reader = node->CreateReader<Chatter>("channel/chatter",
 *         [](const std::shared_ptr<Chatter>& msg) { ... });
 *
 *     // 服务 / 客户端
 *     auto service = node->CreateService<Req, Resp>("calc",
 *         [](const std::shared_ptr<Req>& req, std::shared_ptr<Resp>& resp) { ... });
 *     auto client = node->CreateClient<Req, Resp>("calc");
 *     auto resp = client->SendRequest(req);
 *
 *     firefly::WaitForShutdown();
 *     firefly::Clear();
 *     return 0;
 *   }
 */

#endif  // FIREFLY_FIREFLY_H_