// 仅校验头文件在依赖（protobuf）存在时可被正确解析，
// 不实例化尚未实现的 detail 工厂，故可正常编译链接。
#include "firefly/firefly.h"

#include "firefly/common/types.h"
#include "firefly/init.h"
#include "firefly/message/message_traits.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/node.h"
#include "firefly/node/reader.h"
#include "firefly/node/reader_base.h"
#include "firefly/node/writer.h"
#include "firefly/node/writer_base.h"
#include "firefly/service/client.h"
#include "firefly/service/client_base.h"
#include "firefly/service/service.h"
#include "firefly/service/service_base.h"

int main() {
  firefly::QosProfile qos;
  firefly::ChannelConfig cfg;
  firefly::ReaderConfig rcfg;
  firefly::RoleAttributes attr;

  static_assert(firefly::message::HasSerializer<google::protobuf::Message>::value,
                "protobuf base type must be serializable");
  static_assert(!firefly::message::HasSerializer<int>::value,
                "non-protobuf type must not be serializable");

  const char* req_suffix = firefly::SRV_CHANNEL_REQ_SUFFIX;
  const char* res_suffix = firefly::SRV_CHANNEL_RES_SUFFIX;
  (void)cfg;
  (void)rcfg;
  (void)attr;
  (void)qos;

  return (req_suffix != nullptr && res_suffix != nullptr) ? 0 : 1;
}