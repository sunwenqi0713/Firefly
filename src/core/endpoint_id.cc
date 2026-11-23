#include "core/interfaces.h"

namespace firefly {
namespace detail {

std::shared_ptr<const EndpointId> MakeEndpointId(const std::string& url) {
  return std::shared_ptr<const EndpointId>(new EndpointId(url));
}

std::string EndpointId::DebugString() const {
  return url_.empty() ? std::string("<empty-endpoint>") : url_;
}

}  // namespace detail
}  // namespace firefly
