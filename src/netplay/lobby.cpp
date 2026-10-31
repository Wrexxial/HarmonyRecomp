// harmonyrecomp netplay - LAN lobby service.

#include "lobby.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

namespace harmony::netplay::lobby {
namespace {

constexpr uint32_t kMagic = 0x48524E50;  // "HRNP"
// 2: search requests and answers carry the game version and DLC list.
constexpr uint8_t kVersion = 2;
constexpr size_t kMaxDatagram = 1400;

enum class Type : uint8_t {
  kSearchRequest = 1,
  kSearchResponse = 2,
  kQosRequest = 3,
  kQosResponse = 4,
};

uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- Serialization (big-endian) --------------------------------------------

class Writer {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v) {
    u8(static_cast<uint8_t>(v >> 8));
    u8(static_cast<uint8_t>(v));
  }
  void u32(uint32_t v) {
    u16(static_cast<uint16_t>(v >> 16));
    u16(static_cast<uint16_t>(v));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void bytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  size_t size() const { return buf_.size(); }
  const std::vector<uint8_t>& data() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  uint8_t u8() {
    if (!need(1)) return 0;
    return p_[pos_++];
  }
  uint16_t u16() {
    uint16_t hi = u8();
    return static_cast<uint16_t>((hi << 8) | u8());
  }
  uint32_t u32() {
    uint32_t hi = u16();
    return (hi << 16) | u16();
  }
  uint64_t u64() {
    uint64_t hi = u32();
    return (hi << 32) | u32();
  }
  bool bytes(void* out, size_t n) {
    if (!need(n)) return false;
    std::copy_n(p_ + pos_, n, static_cast<uint8_t*>(out));
    pos_ += n;
    return true;
  }

 private:
  bool need(size_t n) {
    if (!ok_ || pos_ + n > n_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

struct Header {
  Type type{};
  uint32_t title_id = 0;
  uint64_t instance = 0;
  uint32_t request_id = 0;
};

void WriteHeader(Writer& w, const Header& h) {
  w.u32(kMagic);
  w.u8(kVersion);
  w.u8(static_cast<uint8_t>(h.type));
  w.u32(h.title_id);
  w.u64(h.instance);
  w.u32(h.request_id);
}

enum class HeaderResult { kOk, kOtherVersion, kInvalid };

HeaderResult ReadHeader(Reader& r, Header* h, uint8_t* version) {
  if (r.u32() != kMagic) return HeaderResult::kInvalid;
  *version = r.u8();
  if (!r.ok()) return HeaderResult::kInvalid;
  if (*version != kVersion) return HeaderResult::kOtherVersion;
  h->type = static_cast<Type>(r.u8());
  h->title_id = r.u32();
  h->instance = r.u64();
  h->request_id = r.u32();
  return r.ok() ? HeaderResult::kOk : HeaderResult::kInvalid;
}

struct BuildInfo {
  std::string build;
  std::vector<std::string> dlc;
};

void WriteString(Writer& w, const std::string& text) {
  std::string t = text.substr(0, 64);
  w.u8(static_cast<uint8_t>(t.size()));
  w.bytes(t.data(), t.size());
}

std::string ReadString(Reader& r) {
  std::string t(r.u8(), '\0');
  if (!t.empty()) r.bytes(t.data(), t.size());
  return t;
}

void WriteInfo(Writer& w, const BuildInfo& info) {
  WriteString(w, info.build);
  size_t count = std::min<size_t>(info.dlc.size(), 24);
  w.u8(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; ++i) WriteString(w, info.dlc[i]);
}

bool ReadInfo(Reader& r, BuildInfo* info) {
  info->build = ReadString(r);
  info->dlc.resize(r.u8());
  for (auto& d : info->dlc) d = ReadString(r);
  return r.ok();
}

void WriteAd(Writer& w, const SessionAd& ad) {
  w.u64(ad.id);
  w.bytes(ad.key, sizeof(ad.key));
  w.u32(ad.flags);
  w.u32(ad.max_public);
  w.u32(ad.max_private);
  w.u32(ad.filled_public);
  w.u32(ad.filled_private);
  w.u32(ad.state);
  std::string name = ad.host_name.substr(0, 32);
  w.u8(static_cast<uint8_t>(name.size()));
  w.bytes(name.data(), name.size());
  size_t ctx_count = std::min<size_t>(ad.contexts.size(), 32);
  w.u8(static_cast<uint8_t>(ctx_count));
  for (size_t i = 0; i < ctx_count; ++i) {
    w.u32(ad.contexts[i].id);
    w.u32(ad.contexts[i].value);
  }
  // Properties are optional extras: keep the datagram below kMaxDatagram.
  std::vector<const Property*> props;
  size_t budget = kMaxDatagram > w.size() + 64 ? kMaxDatagram - w.size() - 64 : 0;
  for (const auto& p : ad.properties) {
    size_t cost = 6 + p.value.size();
    if (props.size() >= 32 || p.value.size() > 0xFFFF || cost > budget) continue;
    budget -= cost;
    props.push_back(&p);
  }
  w.u8(static_cast<uint8_t>(props.size()));
  for (const auto* p : props) {
    w.u32(p->id);
    w.u16(static_cast<uint16_t>(p->value.size()));
    w.bytes(p->value.data(), p->value.size());
  }
}

bool ReadAd(Reader& r, SessionAd* ad) {
  ad->id = r.u64();
  r.bytes(ad->key, sizeof(ad->key));
  ad->flags = r.u32();
  ad->max_public = r.u32();
  ad->max_private = r.u32();
  ad->filled_public = r.u32();
  ad->filled_private = r.u32();
  ad->state = r.u32();
  uint8_t name_len = r.u8();
  ad->host_name.resize(name_len);
  if (name_len) r.bytes(ad->host_name.data(), name_len);
  uint8_t ctx_count = r.u8();
  ad->contexts.resize(ctx_count);
  for (auto& c : ad->contexts) {
    c.id = r.u32();
    c.value = r.u32();
  }
  uint8_t prop_count = r.u8();
  ad->properties.resize(prop_count);
  for (auto& p : ad->properties) {
    p.id = r.u32();
    uint16_t len = r.u16();
    p.value.resize(len);
    if (len) r.bytes(p.value.data(), len);
  }
  return r.ok();
}

// ---- Service state ----------------------------------------------------------

struct QosListen {
  bool enabled = false;
  std::vector<uint8_t> data;
};

struct Pending {
  std::mutex mutex;
  std::condition_variable cv;
  uint64_t sent_at = 0;
  std::vector<Found> found;
  std::optional<QosResult> qos;
  std::optional<uint64_t> qos_session;
};

struct State {
  std::mutex mutex;
  Config config;
  udp::Socket socket;
  std::thread thread;
  std::atomic<bool> running{false};
  uint64_t instance = 0;
  std::atomic<uint32_t> next_request{1};
  std::map<uint64_t, SessionAd> ads;
  std::map<uint64_t, QosListen> qos;
  std::unordered_map<uint32_t, std::shared_ptr<Pending>> pending;
  // Lobby service port per host IP, learned from search answers.
  std::unordered_map<uint32_t, uint16_t> host_ports;
  // Players running another netplay protocol: IP -> (last seen ms, version).
  std::unordered_map<uint32_t, std::pair<uint64_t, uint8_t>> other_protocol;
  std::set<uint32_t> logged_mismatch;  // IPs already reported in the log
  std::vector<Mismatch> last_mismatches;
  std::map<std::string, uint64_t> noticed;  // notice text -> last shown ms
};

State& S() {
  static State state;
  return state;
}

LogFn g_log = nullptr;
void Log(const std::string& message) {
  if (g_log) g_log(message);
}

LogFn g_notice = nullptr;
// The game searches again every few seconds: show the same notice at most once
// a minute.
void Notice(const std::string& message) {
  auto& s = S();
  {
    std::lock_guard lock(s.mutex);
    uint64_t now = NowMs();
    auto it = s.noticed.find(message);
    if (it != s.noticed.end() && now - it->second < 60000) return;
    s.noticed[message] = now;
  }
  if (g_notice) g_notice(message);
}

BuildInfo OurInfo() { return {S().config.build, S().config.dlc}; }

std::string BuildName(const std::string& build) { return build.empty() ? "unknown" : build; }

VirtualNet g_vnet;
HostObserver g_host_observer = nullptr;

void SendTo(const udp::Endpoint& to, const Writer& w) {
  if (g_vnet.is_virtual && g_vnet.is_virtual(to.ip)) {
    if (g_vnet.send) g_vnet.send(to.ip, to.port, w.data().data(), w.size());
    return;
  }
  S().socket.SendTo(to, w.data().data(), w.size());
}

void HandleDatagram(const uint8_t* data, size_t size, const udp::Endpoint& from) {
  auto& s = S();
  Reader r(data, size);
  Header h;
  uint8_t version = 0;
  switch (ReadHeader(r, &h, &version)) {
    case HeaderResult::kInvalid:
      return;
    case HeaderResult::kOtherVersion: {
      // Another HarmonyRecomp release with an incompatible lobby protocol:
      // cannot talk to it, but tell the player why it is not found.
      bool first = false;
      {
        std::lock_guard lock(s.mutex);
        first = s.logged_mismatch.insert(from.ip).second;
        s.other_protocol[from.ip] = {NowMs(), version};
      }
      if (first) {
        Log("player at " + udp::ToString(from.ip) + " uses netplay protocol " +
            std::to_string(version) + " (ours " + std::to_string(kVersion) +
            "): another HarmonyRecomp version, cannot play together");
      }
      return;
    }
    case HeaderResult::kOk:
      break;
  }
  if (h.title_id != s.config.title_id) return;
  if (h.instance == s.instance) return;  // our own broadcast

  switch (h.type) {
    case Type::kSearchRequest: {
      uint8_t flags = r.u8();
      uint64_t wanted = r.u64();
      BuildInfo searcher;
      if (!ReadInfo(r, &searcher)) return;
      if (searcher.build != s.config.build) {
        bool first = false;
        {
          std::lock_guard lock(s.mutex);
          first = s.logged_mismatch.insert(from.ip).second;
        }
        if (first) {
          Log("search from " + udp::ToString(from.ip) + " with game version " +
              BuildName(searcher.build) + " (ours " + BuildName(s.config.build) +
              "): they will not see our lobby");
        }
      }
      std::vector<SessionAd> ads;
      {
        std::lock_guard lock(s.mutex);
        for (const auto& [id, ad] : s.ads) {
          if (!(flags & 1) || id == wanted) ads.push_back(ad);
        }
      }
      for (const auto& ad : ads) {
        Writer w;
        WriteHeader(w, {Type::kSearchResponse, s.config.title_id, s.instance, h.request_id});
        WriteInfo(w, OurInfo());
        WriteAd(w, ad);
        SendTo(from, w);
      }
      return;
    }
    case Type::kQosRequest: {
      uint64_t session = r.u64();
      uint64_t echo = r.u64();
      if (!r.ok()) return;
      QosListen listen;
      {
        std::lock_guard lock(s.mutex);
        auto it = s.qos.find(session);
        if (it == s.qos.end()) return;  // not listening: no answer, like real XNet
        listen = it->second;
      }
      Writer w;
      WriteHeader(w, {Type::kQosResponse, s.config.title_id, s.instance, h.request_id});
      w.u64(session);
      w.u64(echo);
      w.u8(listen.enabled ? 1 : 0);
      size_t len = listen.enabled ? std::min<size_t>(listen.data.size(), 1024) : 0;
      w.u16(static_cast<uint16_t>(len));
      if (len) w.bytes(listen.data.data(), len);
      SendTo(from, w);
      return;
    }
    case Type::kSearchResponse:
    case Type::kQosResponse: {
      std::shared_ptr<Pending> pending;
      {
        std::lock_guard lock(s.mutex);
        auto it = s.pending.find(h.request_id);
        if (it == s.pending.end()) return;
        pending = it->second;
      }
      uint32_t rtt = static_cast<uint32_t>(NowMs() - pending->sent_at);
      if (h.type == Type::kSearchResponse) {
        Found f;
        BuildInfo info;
        if (!ReadInfo(r, &info) || !ReadAd(r, &f.ad)) return;
        f.build = std::move(info.build);
        f.dlc = std::move(info.dlc);
        f.from = from;
        f.rtt_ms = rtt;
        {
          std::lock_guard lock(s.mutex);
          s.host_ports[from.ip] = from.port;
        }
        std::lock_guard lock(pending->mutex);
        bool dup = std::any_of(pending->found.begin(), pending->found.end(),
                               [&](const Found& x) { return x.ad.id == f.ad.id; });
        if (!dup) pending->found.push_back(std::move(f));
      } else {
        uint64_t session = r.u64();
        r.u64();  // echo
        QosResult q;
        q.contacted = true;
        q.enabled = r.u8() != 0;
        uint16_t len = r.u16();
        q.data.resize(len);
        if (len) r.bytes(q.data.data(), len);
        if (!r.ok()) return;
        q.rtt_ms = std::max<uint32_t>(rtt, 1);
        std::lock_guard lock(pending->mutex);
        if (pending->qos_session && *pending->qos_session == session && !pending->qos) {
          pending->qos = std::move(q);
        }
      }
      pending->cv.notify_all();
      return;
    }
  }
}

void ServiceLoop() {
  auto& s = S();
  std::vector<uint8_t> buffer(2048);
  while (s.running) {
    udp::Endpoint from;
    int n = s.socket.RecvFrom(buffer.data(), buffer.size(), &from, 100);
    if (n > 0) HandleDatagram(buffer.data(), static_cast<size_t>(n), from);
  }
}

std::shared_ptr<Pending> NewPending(uint32_t* request_id) {
  auto& s = S();
  auto p = std::make_shared<Pending>();
  *request_id = s.next_request++;
  std::lock_guard lock(s.mutex);
  s.pending[*request_id] = p;
  return p;
}

void DropPending(uint32_t request_id) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  s.pending.erase(request_id);
}

std::vector<udp::Endpoint> SearchTargets() {
  auto& s = S();
  std::vector<udp::Endpoint> targets;
  auto add = [&](udp::Endpoint ep) {
    if (std::find(targets.begin(), targets.end(), ep) == targets.end()) targets.push_back(ep);
  };
  add({0xFFFFFFFFu, s.config.remote_port});
  for (const auto& itf : udp::ListInterfaces()) {
    if (itf.netmask && itf.netmask != 0xFFFFFFFFu) add({itf.broadcast(), s.config.remote_port});
  }
  for (const auto& peer : s.config.peers) add(peer);
  if (g_vnet.peers) {
    for (uint32_t vip : g_vnet.peers()) add({vip, kDefaultPort});
  }
  return targets;
}

}  // namespace

void SetLogger(LogFn fn) { g_log = fn; }
void SetVirtualNet(const VirtualNet& net) { g_vnet = net; }
void SetHostObserver(HostObserver fn) { g_host_observer = fn; }

void Inject(const uint8_t* data, size_t size, const udp::Endpoint& from) {
  if (S().running) HandleDatagram(data, size, from);
}
void SetNoticeSink(LogFn fn) { g_notice = fn; }

bool Start(const Config& config, std::string* error) {
  auto& s = S();
  if (s.running) return true;
  s.config = config;
  if (!s.socket.Open(config.port, error)) return false;
  std::random_device rd;
  s.instance = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ NowMs();
  s.running = true;
  s.thread = std::thread(ServiceLoop);
  Log("lobby service on UDP port " + std::to_string(s.socket.local_port()));
  return true;
}

void Stop() {
  auto& s = S();
  if (!s.running) return;
  s.running = false;
  if (s.thread.joinable()) s.thread.join();
  s.socket.Close();
}

bool Running() { return S().running; }
uint16_t BoundPort() { return S().socket.local_port(); }

void Advertise(const SessionAd& ad) {
  {
    std::lock_guard lock(S().mutex);
    S().ads[ad.id] = ad;
  }
  if (g_host_observer) g_host_observer(&ad);
}

void Withdraw(uint64_t session_id) {
  std::optional<SessionAd> remaining;
  {
    std::lock_guard lock(S().mutex);
    S().ads.erase(session_id);
    if (!S().ads.empty()) remaining = S().ads.begin()->second;
  }
  if (g_host_observer) g_host_observer(remaining ? &*remaining : nullptr);
}

void SetQosListen(uint64_t session_id, std::optional<bool> enabled,
                  const std::vector<uint8_t>* data) {
  std::lock_guard lock(S().mutex);
  auto& q = S().qos[session_id];
  if (enabled) q.enabled = *enabled;
  if (data) q.data = *data;
}

void ReleaseQosListen(uint64_t session_id) {
  std::lock_guard lock(S().mutex);
  S().qos.erase(session_id);
}

std::vector<Found> Search(int timeout_ms, std::optional<uint64_t> session_id) {
  auto& s = S();
  if (!s.running) return {};
  uint32_t request_id = 0;
  auto pending = NewPending(&request_id);
  Writer w;
  WriteHeader(w, {Type::kSearchRequest, s.config.title_id, s.instance, request_id});
  w.u8(session_id ? 1 : 0);
  w.u64(session_id.value_or(0));
  WriteInfo(w, OurInfo());
  auto targets = SearchTargets();
  pending->sent_at = NowMs();
  // Send twice: a single lost broadcast should not hide a lobby. Answers are
  // collected by the service thread for the whole timeout.
  for (int round = 0; round < 2; ++round) {
    for (const auto& t : targets) SendTo(t, w);
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms / 2));
  }
  DropPending(request_id);
  std::vector<Found> found;
  {
    std::lock_guard lock(pending->mutex);
    found = pending->found;
  }

  // Leave out hosts we cannot play with, and say why.
  std::vector<Found> playable;
  std::vector<Mismatch> mismatches;
  for (auto& f : found) {
    if (f.build == s.config.build) {
      playable.push_back(std::move(f));
      continue;
    }
    mismatches.push_back({f.from.ip, f.ad.host_name,
                          "game version " + BuildName(f.build) + ", you have " +
                              BuildName(s.config.build)});
  }
  {
    std::lock_guard lock(s.mutex);
    for (const auto& [ip, seen] : s.other_protocol) {
      if (seen.first >= pending->sent_at) {
        mismatches.push_back({ip, "", "another HarmonyRecomp version (netplay protocol " +
                                          std::to_string(seen.second) + ", yours " +
                                          std::to_string(kVersion) + ")"});
      }
    }
    s.last_mismatches = mismatches;
  }
  for (const auto& m : mismatches) {
    std::string who = m.host_name.empty() ? udp::ToString(m.ip) : m.host_name;
    Log("left out lobby of " + who + " at " + udp::ToString(m.ip) + ": " + m.reason);
    Notice("Lobby of " + who + " hidden: " + m.reason + ". Everyone needs the same version.");
  }
  // Same version but different DLC: playable, the host's stage may be missing.
  for (const auto& f : playable) {
    size_t missing = 0;
    for (const auto& d : f.dlc) {
      if (std::find(s.config.dlc.begin(), s.config.dlc.end(), d) == s.config.dlc.end()) ++missing;
    }
    if (missing) {
      Notice(f.ad.host_name + " has " + std::to_string(missing) +
             " DLC package(s) you don't have; their DLC stages and characters may not work.");
    }
  }
  return playable;
}

std::vector<Mismatch> LastMismatches() {
  std::lock_guard lock(S().mutex);
  return S().last_mismatches;
}

QosResult Qos(uint32_t ip, uint64_t session_id, int timeout_ms) {
  auto& s = S();
  if (!s.running) return {};
  uint16_t port = s.config.remote_port;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.host_ports.find(ip);
    if (it != s.host_ports.end()) port = it->second;
  }
  uint32_t request_id = 0;
  auto pending = NewPending(&request_id);
  pending->qos_session = session_id;
  Writer w;
  WriteHeader(w, {Type::kQosRequest, s.config.title_id, s.instance, request_id});
  w.u64(session_id);
  w.u64(NowMs());
  pending->sent_at = NowMs();
  const int tries = 3;
  for (int i = 0; i < tries; ++i) {
    SendTo({ip, port}, w);
    std::unique_lock lock(pending->mutex);
    if (pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms / tries),
                             [&] { return pending->qos.has_value(); })) {
      break;
    }
  }
  DropPending(request_id);
  std::lock_guard lock(pending->mutex);
  return pending->qos.value_or(QosResult{});
}

}  // namespace harmony::netplay::lobby
