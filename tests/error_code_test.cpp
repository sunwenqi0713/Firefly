// error_code_test.cpp：每个 ErrorCode 至少一条触发路径 + message 非空校验。
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::Chatter;

static int Check(bool cond, int line) {
  if (!cond) return line;
  return 0;
}

int main() {
  // kNotReady：Init 之前调用 Create 必须失败且带诊断信息
  {
    auto node = firefly::CreateNode("pre_init_node");
    auto r = node->CreateWriter<Chatter>("ch_not_ready");
    if (int e = Check(!r && r.code == ErrorCode::kNotReady && !r.message.empty(), 10)) return e;
  }

  firefly::Init("error_code_test");
  auto node = firefly::CreateNode("error_code_node");
  if (!node) return 20;

  // kOk
  auto ok = node->CreateWriter<Chatter>("ch_ok");
  if (int e = Check(static_cast<bool>(ok) && ok.code == ErrorCode::kOk, 30)) return e;

  // kInvalidName：空名 / 服务后缀保留字
  auto empty_name = node->CreateWriter<Chatter>("");
  if (int e = Check(!empty_name && empty_name.code == ErrorCode::kInvalidName &&
                        !empty_name.message.empty(), 40)) return e;
  auto reserved = node->CreateWriter<Chatter>("foo__SRV__REQUEST");
  if (int e = Check(!reserved && reserved.code == ErrorCode::kInvalidName, 41)) return e;

  // kInvalidConfig：depth==0 / pending_queue_size==0
  firefly::ChannelConfig bad_cfg;
  bad_cfg.channel_name = "ch_bad_qos";
  bad_cfg.qos_profile.depth = 0;
  auto bad_qos = node->CreateWriter<Chatter>(bad_cfg);
  if (int e = Check(!bad_qos && bad_qos.code == ErrorCode::kInvalidConfig, 50)) return e;

  firefly::ReaderConfig bad_rcfg;
  bad_rcfg.channel_name = "ch_bad_rqos";
  bad_rcfg.pending_queue_size = 0;
  auto bad_rqos = node->CreateReader<Chatter>(bad_rcfg);
  if (int e = Check(!bad_rqos && bad_rqos.code == ErrorCode::kInvalidConfig, 51)) return e;

  // kDuplicate：同节点同通道重复 Reader
  auto reader1 = node->CreateReader<Chatter>("ch_dup");
  auto reader2 = node->CreateReader<Chatter>("ch_dup");
  if (int e = Check(!reader2 && reader2.code == ErrorCode::kDuplicate &&
                        reader2.message.find("ch_dup") != std::string::npos, 60)) return e;

  // kNoSubscriber：Write 到无订阅者通道
  auto writer_r = node->CreateWriter<Chatter>("ch_no_sub");
  if (int e = Check(static_cast<bool>(writer_r), 70)) return e;
  auto writer = writer_r.value;
  Chatter msg;
  msg.set_seq(1);
  if (int e = Check(writer->Write(msg) == firefly::WriteStatus::kNoSubscriber, 71)) return e;

  // kQueueFull：协议层错误映射（真实 EAGAIN 需构造拥塞，本阶段验证映射函数）
  if (int e = Check(firefly::detail::MapNngSendError(NNG_EAGAIN) ==
                        firefly::WriteStatus::kQueueFull, 80)) return e;

  // kTransport：未 Init 的 Writer 写操作走传输错误分支
  firefly::Writer<Chatter> uninitialized(firefly::ChannelConfig{});
  if (int e = Check(uninitialized.Write(msg) == firefly::WriteStatus::kTransportError, 90)) return e;

  // kSerialization / kTimeout / kServiceNotFound / kServiceFailed / kTypeMismatch：
  // 端到端触发路径见 rpc_error_test.cpp；此处验证 Result/ServiceResult 的码值与
  // 可读名在接口层一致（这五个码的构造与判定是公共契约的一部分）。
  firefly::Result<std::shared_ptr<Chatter>> r{
      nullptr, ErrorCode::kSerialization, "Serialize[msg='x']: empty payload rejected"};
  if (int e = Check(!r && r.code == ErrorCode::kSerialization &&
                        std::string(firefly::ErrorCodeToString(r.code)) == "kSerialization",
                    100)) return e;
  if (int e = Check(r.value_or(nullptr) == nullptr && !r.message.empty(), 101)) return e;

  firefly::Result<int> t{0, ErrorCode::kTimeout, "SendRequest[service='s']: kTimeout"};
  if (int e = Check(!t && t.value_or(42) == 42 && !t.message.empty(), 110)) return e;

  firefly::Result<int> nf{0, ErrorCode::kServiceNotFound, "SendRequest[service='s']: not found"};
  if (int e = Check(!nf && std::string(firefly::ErrorCodeToString(nf.code)) ==
                                "kServiceNotFound", 120)) return e;

  firefly::Result<int> sf{0, ErrorCode::kServiceFailed, "SendRequest[service='s']: kServiceFailed"};
  if (int e = Check(!sf && std::string(firefly::ErrorCodeToString(sf.code)) ==
                                "kServiceFailed", 130)) return e;

  // kTypeMismatch：当前 GetReader 对类型不符返回 nullptr 而不返回码（已知缺口），
  // 这里验证该码本身的 Result 语义，缺口记入契约清单。
  firefly::Result<int> tm{0, ErrorCode::kTypeMismatch, "GetReader: type mismatch"};
  if (int e = Check(!tm && std::string(firefly::ErrorCodeToString(tm.code)) ==
                                "kTypeMismatch", 140)) return e;

  firefly::Clear();
  return 0;
}
