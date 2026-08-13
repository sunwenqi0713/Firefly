#include "firefly/discovery.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/common/log.h"
#include "firefly/runtime.h"

namespace firefly {
namespace discovery {

const char* KindToString(Kind k) {
  switch (k) {
    case Kind::kWriter:
      return "writer";
    case Kind::kReader:
      return "reader";
    case Kind::kService:
      return "service";
  }
  return "unknown";
}

namespace {

constexpr char kRegistryRepUrl[] = "tcp://127.0.0.1:37221";
constexpr char kRegistryPubUrl[] = "tcp://127.0.0.1:37222";
constexpr int kPollTimeoutMs = 200;
constexpr int kReqTimeoutMs = 500;

struct Sub {
  int id;
  Kind kind;
  std::string name;
  std::function<void(const Event&)> cb;
};

}  // namespace

// 嵌套类定义（私有，仅在实现中使用）
struct Registry::Impl {
  bool primary = false;
  std::atomic<bool> running_{false};
  std::mutex start_mutex_;

  // 主实例
  nng_socket rep_sock_{0};
  nng_socket pub_sock_{0};
  // 从实例
  nng_socket req_sock_{0};
  nng_socket sub_sock_{0};

  std::mutex data_mutex_;
  std::map<std::string, std::set<std::string>> table_;

  std::mutex sub_mutex_;
  std::vector<Sub> subs_;
  int next_sub_id_ = 1;

  std::mutex req_mutex_;

  std::thread rep_thread_;
  std::thread sub_thread_;
};

namespace {

bool KindFromString(const std::string& s, Kind* out) {
  if (s == "writer") {
    *out = Kind::kWriter;
  } else if (s == "reader") {
    *out = Kind::kReader;
  } else if (s == "service") {
    *out = Kind::kService;
  } else {
    return false;
  }
  return true;
}

inline std::string Key(Kind kind, const std::string& name) {
  return std::string(KindToString(kind)) + "\t" + name;
}

void CloseSocket(nng_socket& s) {
  if (s.id != 0) {
    nng_close(s);
    s.id = 0;
  }
}

// 阻塞接收一条消息并转为字符串；超时/关闭返回空串
std::string RecvToString(nng_socket s) {
  void* data = nullptr;
  size_t size = 0;
  int r = nng_recv(s, &data, &size, NNG_FLAG_ALLOC);
  if (r != 0) {
    if (r != NNG_ETIMEDOUT && r != NNG_ECLOSED) {
      FY_WARN("nng_recv: %s", nng_strerror(r));
    }
    return "";
  }
  std::string out(static_cast<char*>(data), size);
  nng_free(data, size);
  return out;
}

// 发送一条消息
bool SendRaw(nng_socket s, const std::string& msg) {
  int r = nng_send(s, const_cast<char*>(msg.data()), msg.size(), 0);
  if (r != 0) {
    FY_WARN("nng_send: %s", nng_strerror(r));
    return false;
  }
  return true;
}

// 通知本地匹配的订阅者
void NotifyLocal(Registry::Impl& impl, const Event& ev) {
  std::lock_guard<std::mutex> lock(impl.sub_mutex_);
  for (const auto& sub : impl.subs_) {
    if (sub.kind == ev.kind && sub.name == ev.name && sub.cb) {
      try {
        sub.cb(ev);
      } catch (...) {
        FY_ERROR("registry: subscriber callback threw");
      }
    }
  }
}

// 主实例：广播给其他进程
void Broadcast(Registry::Impl& impl, const Event& ev) {
  if (impl.pub_sock_.id == 0) return;
  std::string note = std::string(ev.added ? "A|" : "R|") + KindToString(ev.kind) +
                     "|" + ev.name + "|" + ev.url;
  SendRaw(impl.pub_sock_, note);
}

// 主实例：执行一条命令并返回响应文本
std::string Execute(Registry::Impl& impl, const std::string& req) {
  std::istringstream in(req);
  std::string cmd, kind, name, url;
  std::getline(in, cmd, '\t');

  if (cmd == "REG" || cmd == "UNREG") {
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    std::getline(in, url, '\t');
    Kind k;
    if (!KindFromString(kind, &k)) return "ERR bad kind\n";
    if (name.empty()) return "ERR bad name\n";

    Event ev{k, name, url, cmd == "REG"};
    {
      std::lock_guard<std::mutex> lock(impl.data_mutex_);
      auto key = Key(k, name);
      if (ev.added) {
        impl.table_[key].insert(url);
      } else {
        auto it = impl.table_.find(key);
        if (it != impl.table_.end()) {
          it->second.erase(url);
          if (it->second.empty()) impl.table_.erase(it);
        }
      }
    }
    NotifyLocal(impl, ev);   // 本进程直接派发
    Broadcast(impl, ev);     // 跨进程广播
    return "OK\n";
  }
  if (cmd == "LIST" || cmd == "HAS") {
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    Kind k;
    if (!KindFromString(kind, &k)) return "ERR bad kind\n";
    std::vector<std::string> urls;
    bool has = false;
    {
      std::lock_guard<std::mutex> lock(impl.data_mutex_);
      auto it = impl.table_.find(Key(k, name));
      if (it != impl.table_.end()) {
        if (cmd == "LIST") urls.assign(it->second.begin(), it->second.end());
        has = !it->second.empty();
      }
    }
    if (cmd == "LIST") {
      std::ostringstream out;
      out << "OK " << urls.size() << "\n";
      for (const auto& u : urls) out << u << "\n";
      return out.str();
    }
    return has ? "OK 1\n" : "OK 0\n";
  }
  return "ERR unknown cmd\n";
}

// 主实例：REP 服务线程
void RunRepThread(Registry::Impl& impl) {
  FY_INFO("registry: primary on %s", kRegistryRepUrl);
  while (impl.running_.load() &&
         !runtime::ShutdownManager::instance().IsShutdown()) {
    std::string req = RecvToString(impl.rep_sock_);
    if (req.empty()) continue;
    std::string resp = Execute(impl, req);
    SendRaw(impl.rep_sock_, resp);
  }
  FY_INFO("registry: rep loop exit");
}

// 从实例：订阅广播
void RunSubLoop(Registry::Impl& impl) {
  FY_INFO("registry: follower on %s", kRegistryRepUrl);
  while (impl.running_.load() &&
         !runtime::ShutdownManager::instance().IsShutdown()) {
    std::string line = RecvToString(impl.sub_sock_);
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string tag, kind, name, url;
    std::getline(in, tag, '|');
    std::getline(in, kind, '|');
    std::getline(in, name, '|');
    std::getline(in, url, '|');
    Kind k;
    if (!KindFromString(kind, &k)) continue;
    NotifyLocal(impl, Event{k, name, url, tag == "A"});
  }
  FY_INFO("registry: follower loop exit");
}

// 从实例：REQ 向主实例发命令
std::string RemoteRequest(Registry::Impl& impl, const std::string& line) {
  std::lock_guard<std::mutex> lock(impl.req_mutex_);
  if (impl.req_sock_.id == 0) return "";
  if (!SendRaw(impl.req_sock_, line)) return "";
  return RecvToString(impl.req_sock_);
}

}  // namespace anonymous

// ---------------- 公有接口 ----------------

Registry::Registry() : impl_(new Impl) {}
Registry::~Registry() { Stop(); }

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

bool Registry::Start() {
  std::lock_guard<std::mutex> lock(impl_->start_mutex_);
  if (impl_->running_.load()) return true;

  // 1) 尝试成为主注册中心
  int r = nng_rep0_open(&impl_->rep_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_rep0_open: %s", nng_strerror(r));
    return false;
  }
  nng_setopt_ms(impl_->rep_sock_, NNG_OPT_RECVTIMEO, kPollTimeoutMs);
  r = nng_listen(impl_->rep_sock_, kRegistryRepUrl, nullptr, 0);
  if (r == 0) {
    r = nng_pub0_open(&impl_->pub_sock_);
    if (r != 0) {
      FY_ERROR("registry: nng_pub0_open: %s", nng_strerror(r));
      CloseSocket(impl_->rep_sock_);
      return false;
    }
    r = nng_listen(impl_->pub_sock_, kRegistryPubUrl, nullptr, 0);
    if (r != 0) {
      FY_ERROR("registry: pub listen %s: %s", kRegistryPubUrl, nng_strerror(r));
      CloseSocket(impl_->rep_sock_);
      CloseSocket(impl_->pub_sock_);
      return false;
    }
    impl_->primary = true;
    impl_->rep_thread_ = std::thread(&RunRepThread, std::ref(*impl_));
    impl_->running_.store(true);
    FY_INFO("registry: acting as primary");
    return true;
  }

  // 端口被占用：作为从实例连接既有主注册中心
  if (r != NNG_EADDRINUSE) {
    FY_ERROR("registry: rep listen %s: %s", kRegistryRepUrl, nng_strerror(r));
    CloseSocket(impl_->rep_sock_);
    return false;
  }
  CloseSocket(impl_->rep_sock_);

  r = nng_req0_open(&impl_->req_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_req0_open: %s", nng_strerror(r));
    return false;
  }
  nng_setopt_ms(impl_->req_sock_, NNG_OPT_SENDTIMEO, kReqTimeoutMs);
  nng_setopt_ms(impl_->req_sock_, NNG_OPT_RECVTIMEO, kReqTimeoutMs);
  r = nng_dial(impl_->req_sock_, kRegistryRepUrl, nullptr, 0);
  if (r != 0) {
    FY_ERROR("registry: req dial %s: %s", kRegistryRepUrl, nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    return false;
  }

  r = nng_sub0_open(&impl_->sub_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_sub0_open: %s", nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    return false;
  }
  nng_setopt(impl_->sub_sock_, NNG_OPT_SUB_SUBSCRIBE, "", 0);
  nng_setopt_ms(impl_->sub_sock_, NNG_OPT_RECVTIMEO, kPollTimeoutMs);
  r = nng_dial(impl_->sub_sock_, kRegistryPubUrl, nullptr, 0);
  if (r != 0) {
    FY_ERROR("registry: sub dial %s: %s", kRegistryPubUrl, nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    CloseSocket(impl_->sub_sock_);
    return false;
  }

  impl_->primary = false;
  impl_->sub_thread_ = std::thread(&RunSubLoop, std::ref(*impl_));
  impl_->running_.store(true);
  FY_INFO("registry: acting as follower");
  return true;
}

void Registry::Stop() {
  if (!impl_->running_.load()) return;
  impl_->running_.store(false);
  // recv 内部带轮询超时，join 会在 ~200ms 内返回
  if (impl_->rep_thread_.joinable() &&
      impl_->rep_thread_.get_id() != std::this_thread::get_id()) {
    impl_->rep_thread_.join();
  }
  if (impl_->sub_thread_.joinable() &&
      impl_->sub_thread_.get_id() != std::this_thread::get_id()) {
    impl_->sub_thread_.join();
  }
  CloseSocket(impl_->rep_sock_);
  CloseSocket(impl_->pub_sock_);
  CloseSocket(impl_->req_sock_);
  CloseSocket(impl_->sub_sock_);
}

bool Registry::Register(Kind kind, const std::string& name,
                        const std::string& url) {
  if (!Start()) return false;
  std::string line =
      "REG\t" + std::string(KindToString(kind)) + "\t" + name + "\t" + url;
  if (impl_->primary) return Execute(*impl_, line) == "OK\n";
  return RemoteRequest(*impl_, line) == "OK\n";
}

bool Registry::Unregister(Kind kind, const std::string& name,
                          const std::string& url) {
  if (!impl_->running_.load()) return true;
  std::string line =
      "UNREG\t" + std::string(KindToString(kind)) + "\t" + name + "\t" + url;
  if (impl_->primary) return Execute(*impl_, line) == "OK\n";
  return RemoteRequest(*impl_, line) == "OK\n";
}

std::vector<std::string> Registry::List(Kind kind, const std::string& name) {
  std::vector<std::string> urls;
  if (!Start()) return urls;
  std::string line = "LIST\t" + std::string(KindToString(kind)) + "\t" + name;
  std::string resp =
      impl_->primary ? Execute(*impl_, line) : RemoteRequest(*impl_, line);
  std::istringstream in(resp);
  std::string status;
  int count = 0;
  in >> status >> count;
  if (status != "OK") return urls;
  std::string u;
  for (int i = 0; i < count && std::getline(in, u, '\n');) {
    if (!u.empty()) {
      urls.push_back(u);
      ++i;
    }
  }
  return urls;
}

bool Registry::Has(Kind kind, const std::string& name) {
  if (!Start()) return false;
  std::string line = "HAS\t" + std::string(KindToString(kind)) + "\t" + name;
  std::string resp =
      impl_->primary ? Execute(*impl_, line) : RemoteRequest(*impl_, line);
  return resp == "OK 1\n";
}

int Registry::Subscribe(Kind kind, const std::string& name,
                        std::function<void(const Event&)>&& cb) {
  if (!Start()) return 0;
  std::lock_guard<std::mutex> lock(impl_->sub_mutex_);
  int id = impl_->next_sub_id_++;
  impl_->subs_.push_back(Sub{id, kind, name, std::move(cb)});
  return id;
}

void Registry::Unsubscribe(int id) {
  std::lock_guard<std::mutex> lock(impl_->sub_mutex_);
  auto& subs = impl_->subs_;
  subs.erase(std::remove_if(subs.begin(), subs.end(),
                            [id](const Sub& s) { return s.id == id; }),
             subs.end());
}

}  // namespace discovery
}  // namespace firefly
