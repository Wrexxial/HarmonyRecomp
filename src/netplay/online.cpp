// harmonyrecomp netplay - online play through the HarmonyRecomp server.

#include "online.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace harmony::netplay::online {
namespace {

// ---- Wire format (see server/hrnp_server.py) --------------------------------------

constexpr uint32_t kServerMagic = 0x48525356;  // "HRSV"
constexpr uint8_t kServerVersion = 1;
constexpr uint32_t kPeerMagic = 0x48525032;    // "HRP2"

enum : uint8_t {
  kHello = 1, kPing, kRoomOpen, kRoomClose, kRoomList, kConnect, kRelay, kBye,
  kWelcome = 101, kPong, kRoomOpened, kRoomListResult, kConnected, kPeer, kRelayed, kError,
};
// kKeepalive carries the sender's clock (v0.6+); kKeepaliveAck echoes it
// for the ping shown in F6 > Online. Older versions ignore both extras.
enum : uint8_t { kPunch = 1, kPunchAck, kData, kKeepalive, kKeepaliveAck };

constexpr uint32_t kVirtualNet = 0x0A420000;  // 10.66.0.0/16
constexpr auto kPingInterval = std::chrono::seconds(10);
constexpr auto kPeerKeepalive = std::chrono::seconds(5);
constexpr auto kPunchInterval = std::chrono::milliseconds(200);
constexpr auto kPunchTime = std::chrono::seconds(6);
constexpr auto kPeerTimeout = std::chrono::seconds(20);

using Clock = std::chrono::steady_clock;

class Writer {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v >> 8)), u8(uint8_t(v)); }
  void u32(uint32_t v) { u16(uint16_t(v >> 16)), u16(uint16_t(v)); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)), u32(uint32_t(v)); }
  void str(const std::string& s) {
    std::string t = s.substr(0, 64);
    u8(uint8_t(t.size()));
    bytes(t.data(), t.size());
  }
  void ep(const udp::Endpoint& e) { u32(e.ip), u16(e.port); }
  void bytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  const std::vector<uint8_t>& data() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  uint8_t u8() { return need(1) ? p_[pos_++] : 0; }
  uint16_t u16() { uint16_t h = u8(); return uint16_t((h << 8) | u8()); }
  uint32_t u32() { uint32_t h = u16(); return (h << 16) | u16(); }
  uint64_t u64() { uint64_t h = u32(); return (h << 32) | u32(); }
  std::string str() {
    size_t n = u8();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(p_ + pos_), n);
    pos_ += n;
    return s;
  }
  udp::Endpoint ep() { udp::Endpoint e; e.ip = u32(); e.port = u16(); return e; }
  const uint8_t* rest(size_t* n) {
    *n = n_ - pos_;
    const uint8_t* p = p_ + pos_;
    pos_ = n_;
    return p;
  }

 private:
  bool need(size_t n) {
    if (!ok_ || pos_ + n > n_) return ok_ = false;
    return true;
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

Writer ServerMsg(uint8_t type) {
  Writer w;
  w.u32(kServerMagic);
  w.u8(kServerVersion);
  w.u8(type);
  return w;
}

Writer PeerMsg(uint8_t type) {
  Writer w;
  w.u32(kPeerMagic);
  w.u8(type);
  return w;
}

// ---- State --------------------------------------------------------------------------

struct Peer {
  uint32_t vip = 0;
  std::string name;
  std::vector<udp::Endpoint> candidates;  // public first, then local addresses
  std::optional<udp::Endpoint> direct;    // confirmed by a punch answer
  Clock::time_point punch_until{};
  Clock::time_point next_punch{};
  Clock::time_point last_heard{};
  Clock::time_point last_data{};    // last game packet
  Clock::time_point stalled_at{};   // game packets stopped (logged once)
  Clock::time_point next_keepalive{};
  uint64_t nonce = 0;
  int rtt_ms = -1;  // from the last answered keepalive, -1 = not known yet
};

// Game packets normally flow many times a second while a session runs; a gap
// this long is logged with the player and the path, so a dropped session can
// be traced to the player whose packets stopped first.
constexpr auto kStallLog = std::chrono::seconds(2);

struct State {
  std::mutex mutex;
  std::condition_variable cv;
  Config config;
  udp::Socket socket;
  udp::Endpoint server;
  std::thread thread;
  std::atomic<bool> running{false};
  uint64_t token = 0;
  uint32_t vip = 0;
  udp::Endpoint public_ep;
  Clock::time_point next_ping{};
  Clock::time_point last_server{};
  std::map<uint32_t, Peer> peers;
  std::map<uint16_t, Handler> handlers;
  // Rooms
  bool room_wanted = false;
  bool room_public = false;
  std::string room_name;
  int room_players = 0;
  int room_max = 0;
  std::string room_code;
  std::string joined;
  // Answers waited for by the blocking calls
  uint64_t list_seq = 0;
  std::vector<RoomInfo> list;
  uint64_t connect_seq = 0;
  bool connect_ok = false;
  std::string connect_message;
};

State& S() {
  static State s;
  return s;
}

LogFn g_log = nullptr;
LogFn g_code_listener = nullptr;
void Log(const std::string& m) {
  if (g_log) g_log(m);
}

std::string Ip(uint32_t ip) { return udp::ToString(ip); }

void ToServer(const Writer& w) { S().socket.SendTo(S().server, w.data().data(), w.data().size()); }

void SendHello() {
  auto& s = S();
  Writer w = ServerMsg(kHello);
  w.u64(s.token);
  w.str(s.config.build);
  w.str(s.config.name);
  std::vector<udp::Endpoint> local;
  for (const auto& itf : udp::ListInterfaces()) {
    if (local.size() < 8) local.push_back({itf.ip, s.socket.local_port()});
  }
  w.u8(uint8_t(local.size()));
  for (const auto& e : local) w.ep(e);
  ToServer(w);
}

void SendRoomOpen() {
  auto& s = S();  // caller holds the mutex
  Writer w = ServerMsg(kRoomOpen);
  w.u8(s.room_public ? 1 : 0);
  w.str(s.room_name);
  w.u8(uint8_t(std::clamp(s.room_players, 0, 255)));
  w.u8(uint8_t(std::clamp(s.room_max, 0, 255)));
  ToServer(w);
}

// Sends one peer frame to `vip`: directly if a path is confirmed, else through
// the server. Caller holds the mutex.
void SendFrame(Peer& peer, const Writer& frame) {
  auto& s = S();
  if (peer.direct && !s.config.force_relay) {
    s.socket.SendTo(*peer.direct, frame.data().data(), frame.data().size());
    return;
  }
  Writer w = ServerMsg(kRelay);
  w.u32(peer.vip);
  w.bytes(frame.data().data(), frame.data().size());
  ToServer(w);
}

void StartPunching(Peer& peer) {
  peer.punch_until = Clock::now() + kPunchTime;
  peer.next_punch = Clock::now();
}

void Punch(Peer& peer) {
  auto& s = S();
  Writer w = PeerMsg(kPunch);
  w.u32(s.vip);
  w.u32(peer.vip);
  w.u64(peer.nonce);
  for (const auto& c : peer.candidates) s.socket.SendTo(c, w.data().data(), w.data().size());
}

// ---- Receiving --------------------------------------------------------------------

void Deliver(uint32_t src_vip, Reader& r) {
  uint16_t src_port = r.u16();
  uint16_t dst_port = r.u16();
  size_t n = 0;
  const uint8_t* p = r.rest(&n);
  if (!r.ok()) return;
  Handler handler;
  {
    std::lock_guard lock(S().mutex);
    auto it = S().handlers.find(dst_port);
    if (it == S().handlers.end()) return;
    handler = it->second;
    auto peer = S().peers.find(src_vip);
    if (peer != S().peers.end()) {
      Peer& pr = peer->second;
      const auto now = Clock::now();
      if (pr.stalled_at != Clock::time_point{}) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - pr.last_data).count();
        Log("online: game data from " + pr.name + " again after " + std::to_string(ms) + " ms");
        pr.stalled_at = {};
      }
      pr.last_heard = pr.last_data = now;
    }
  }
  handler(src_vip, src_port, p, n);
}

// A frame from another player, direct (`from` set) or relayed.
void HandlePeerFrame(const uint8_t* data, size_t size, std::optional<udp::Endpoint> from,
                     uint32_t relayed_from) {
  auto& s = S();
  Reader r(data, size);
  if (r.u32() != kPeerMagic) return;
  uint8_t type = r.u8();
  uint32_t src_vip = r.u32();
  if (!r.ok() || (relayed_from && relayed_from != src_vip)) return;
  switch (type) {
    case kPunch:
    case kPunchAck: {
      uint32_t dst_vip = r.u32();
      uint64_t nonce = r.u64();
      if (!r.ok() || dst_vip != s.vip || !from) return;
      std::lock_guard lock(s.mutex);
      auto it = s.peers.find(src_vip);
      if (it == s.peers.end()) return;
      Peer& peer = it->second;
      peer.last_heard = Clock::now();
      if (type == kPunch) {
        Writer w = PeerMsg(kPunchAck);
        w.u32(s.vip);
        w.u32(src_vip);
        w.u64(nonce);
        s.socket.SendTo(*from, w.data().data(), w.data().size());
      }
      if (!peer.direct && !s.config.force_relay) {
        peer.direct = *from;
        peer.next_keepalive = Clock::now() + kPeerKeepalive;
        Log("direct connection to " + peer.name + " (" + Ip(peer.vip) + ") via " +
            udp::ToString(*from));
      }
      return;
    }
    case kData:
      Deliver(src_vip, r);
      return;
    case kKeepalive: {
      uint64_t sent = r.u64();  // missing from older versions
      std::lock_guard lock(s.mutex);
      auto it = s.peers.find(src_vip);
      if (it == s.peers.end()) return;
      it->second.last_heard = Clock::now();
      if (r.ok()) {
        Writer w = PeerMsg(kKeepaliveAck);
        w.u32(s.vip);
        w.u64(sent);
        SendFrame(it->second, w);
      }
      return;
    }
    case kKeepaliveAck: {
      uint64_t sent = r.u64();
      if (!r.ok()) return;
      std::lock_guard lock(s.mutex);
      auto it = s.peers.find(src_vip);
      if (it == s.peers.end()) return;
      const auto now = Clock::now();
      it->second.last_heard = now;
      const uint64_t now_us = uint64_t(
          std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
      if (now_us >= sent) it->second.rtt_ms = int((now_us - sent) / 1000);
      return;
    }
  }
}

void HandleServer(const uint8_t* data, size_t size) {
  auto& s = S();
  Reader r(data, size);
  if (r.u32() != kServerMagic) return;
  if (r.u8() != kServerVersion) return;
  uint8_t type = r.u8();
  {
    std::lock_guard lock(s.mutex);
    s.last_server = Clock::now();
  }
  switch (type) {
    case kWelcome: {
      uint32_t vip = r.u32();
      udp::Endpoint pub = r.ep();
      if (!r.ok()) return;
      std::lock_guard lock(s.mutex);
      if (s.vip != vip) {
        Log("online: connected, virtual address " + Ip(vip) + ", public " + udp::ToString(pub));
      }
      s.vip = vip;
      s.public_ep = pub;
      if (s.room_wanted) SendRoomOpen();
      s.cv.notify_all();
      return;
    }
    case kRoomOpened: {
      std::string code = r.str();
      if (!r.ok()) return;
      bool fresh = false;
      {
        std::lock_guard lock(s.mutex);
        if (s.room_wanted && code != s.room_code) {
          s.room_code = code;
          fresh = true;
          Log("online: room code " + code + (s.room_public ? " (public)" : " (private)"));
        }
      }
      if (fresh && g_code_listener) g_code_listener(code);
      return;
    }
    case kRoomListResult: {
      std::vector<RoomInfo> rooms(r.u8());
      for (auto& room : rooms) {
        room.code = r.str();
        room.name = r.str();
        room.players = r.u8();
        room.max_players = r.u8();
      }
      if (!r.ok()) return;
      std::lock_guard lock(s.mutex);
      s.list = std::move(rooms);
      ++s.list_seq;
      s.cv.notify_all();
      return;
    }
    case kConnected: {
      bool ok = r.u8() != 0;
      std::string message = r.str();
      r.u32();  // host vip
      if (!r.ok()) return;
      std::lock_guard lock(s.mutex);
      s.connect_ok = ok;
      s.connect_message = message;
      ++s.connect_seq;
      s.cv.notify_all();
      return;
    }
    case kPeer: {
      Peer peer;
      peer.vip = r.u32();
      peer.name = r.str();
      peer.candidates.push_back(r.ep());
      uint8_t n = r.u8();
      for (uint8_t i = 0; i < n; ++i) peer.candidates.push_back(r.ep());
      if (!r.ok() || peer.vip == s.vip) return;
      std::lock_guard lock(s.mutex);
      auto& existing = s.peers[peer.vip];
      bool fresh = existing.vip == 0;
      existing.vip = peer.vip;
      existing.name = peer.name;
      existing.candidates = peer.candidates;
      existing.last_heard = Clock::now();
      if (!existing.nonce) existing.nonce = (uint64_t(std::random_device{}()) << 32) | std::random_device{}();
      if (!existing.direct) StartPunching(existing);
      if (fresh) Log("online: player " + peer.name + " (" + Ip(peer.vip) + ")");
      return;
    }
    case kRelayed: {
      uint32_t src = r.u32();
      size_t n = 0;
      const uint8_t* p = r.rest(&n);
      if (!r.ok()) return;
      HandlePeerFrame(p, n, std::nullopt, src);
      return;
    }
    case kError: {
      std::string message = r.str();
      Log("online: server says: " + message);
      if (message.find("HELLO") != std::string::npos) SendHello();
      return;
    }
    default:
      return;
  }
}

void Tick() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  auto now = Clock::now();
  if (now >= s.next_ping) {
    s.next_ping = now + kPingInterval;
    // HELLO doubles as keepalive: it also refreshes our public address if the
    // router changed the port.
    SendHello();
  }
  for (auto it = s.peers.begin(); it != s.peers.end();) {
    Peer& peer = it->second;
    if (peer.last_data != Clock::time_point{} && peer.stalled_at == Clock::time_point{} &&
        now - peer.last_data > kStallLog && now - peer.last_data < kStallLog * 2) {
      peer.stalled_at = now;
      const auto heard =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - peer.last_heard).count();
      Log("online: no game data from " + peer.name + " for 2 s (" +
          (peer.direct ? "direct" : "relay") + ", last packet of any kind " +
          std::to_string(heard) + " ms ago)");
    }
    if (now - peer.last_heard > kPeerTimeout && now > peer.punch_until) {
      Log("online: lost " + peer.name + " (" + Ip(peer.vip) + ")");
      it = s.peers.erase(it);
      continue;
    }
    if (!peer.direct && now < peer.punch_until && now >= peer.next_punch) {
      peer.next_punch = now + kPunchInterval;
      Punch(peer);
      if (now + kPunchInterval >= peer.punch_until && !s.config.force_relay) {
        Log("online: no direct connection to " + peer.name + ", using the relay");
      }
    }
    if (now >= peer.next_keepalive) {
      peer.next_keepalive = now + kPeerKeepalive;
      Writer w = PeerMsg(kKeepalive);
      w.u32(s.vip);
      w.u64(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch()).count()));
      SendFrame(peer, w);
    }
    ++it;
  }
}

void Loop() {
  auto& s = S();
  std::vector<uint8_t> buffer(2048);
  while (s.running) {
    udp::Endpoint from;
    int n = s.socket.RecvFrom(buffer.data(), buffer.size(), &from, 20);
    if (n > 0) {
      if (from == s.server) {
        HandleServer(buffer.data(), size_t(n));
      } else {
        HandlePeerFrame(buffer.data(), size_t(n), from, 0);
      }
    }
    Tick();
  }
}

}  // namespace

void SetLogger(LogFn fn) { g_log = fn; }
void SetRoomCodeListener(LogFn fn) { g_code_listener = fn; }

bool IsVirtual(uint32_t ip) { return (ip & 0xFFFF0000u) == kVirtualNet; }

bool Start(const Config& config, std::string* error) {
  auto& s = S();
  if (s.running) return true;
  std::string host = config.server;
  uint16_t port = kDefaultServerPort;
  if (auto colon = host.rfind(':'); colon != std::string::npos) {
    port = uint16_t(std::atoi(host.c_str() + colon + 1));
    host.resize(colon);
  }
  auto ip = udp::Resolve(host);
  if (!ip) {
    if (error) *error = "cannot resolve server '" + host + "'";
    return false;
  }
  if (!s.socket.Open(config.local_port, error)) return false;
  {
    std::lock_guard lock(s.mutex);
    s.config = config;
    s.server = {*ip, port};
    std::random_device rd;
    s.token = (uint64_t(rd()) << 32) | rd();
    s.vip = 0;
    s.peers.clear();
    s.next_ping = Clock::now();
  }
  s.running = true;
  s.thread = std::thread(Loop);
  std::unique_lock lock(s.mutex);
  if (!s.cv.wait_for(lock, std::chrono::seconds(3), [&] { return s.vip != 0; })) {
    if (error) *error = "no answer from server " + udp::ToString(s.server);
    lock.unlock();
    Stop();
    return false;
  }
  return true;
}

void Stop() {
  auto& s = S();
  if (!s.running) return;
  ToServer(ServerMsg(kBye));
  s.running = false;
  if (s.thread.joinable()) s.thread.join();
  s.socket.Close();
  std::lock_guard lock(s.mutex);
  s.vip = 0;
  s.peers.clear();
  s.room_code.clear();
  s.room_wanted = false;
  s.joined.clear();
}

bool Connected() {
  std::lock_guard lock(S().mutex);
  return S().running && S().vip != 0;
}

uint32_t VirtualIp() {
  std::lock_guard lock(S().mutex);
  return S().vip;
}

void Listen(uint16_t port, Handler handler) {
  std::lock_guard lock(S().mutex);
  S().handlers[port] = std::move(handler);
}

void Unlisten(uint16_t port) {
  std::lock_guard lock(S().mutex);
  S().handlers.erase(port);
}

bool Send(uint32_t dst_vip, uint16_t src_port, uint16_t dst_port, const void* data, size_t size) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  if (!s.running || !s.vip) return false;
  auto it = s.peers.find(dst_vip);
  if (it == s.peers.end()) return false;
  Writer w = PeerMsg(kData);
  w.u32(s.vip);
  w.u16(src_port);
  w.u16(dst_port);
  w.bytes(data, size);
  SendFrame(it->second, w);
  return true;
}

void OpenRoom(bool is_public, const std::string& name, int players, int max_players) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  bool changed = !s.room_wanted || s.room_public != is_public || s.room_name != name ||
                 s.room_players != players || s.room_max != max_players;
  s.room_wanted = true;
  s.room_public = is_public;
  s.room_name = name;
  s.room_players = players;
  s.room_max = max_players;
  if (changed && s.running && s.vip) SendRoomOpen();
}

void CloseRoom() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  if (!s.room_wanted) return;
  s.room_wanted = false;
  if (!s.room_code.empty()) Log("online: room " + s.room_code + " closed");
  s.room_code.clear();
  if (s.running) ToServer(ServerMsg(kRoomClose));
}

std::string RoomCode() {
  std::lock_guard lock(S().mutex);
  return S().room_code;
}

std::vector<RoomInfo> ListRooms(int timeout_ms) {
  auto& s = S();
  std::unique_lock lock(s.mutex);
  if (!s.running || !s.vip) return {};
  uint64_t seq = s.list_seq;
  ToServer(ServerMsg(kRoomList));
  s.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return s.list_seq != seq; });
  return s.list_seq != seq ? s.list : std::vector<RoomInfo>{};
}

bool JoinRoom(const std::string& code, std::string* message, int timeout_ms) {
  auto& s = S();
  std::unique_lock lock(s.mutex);
  if (!s.running || !s.vip) {
    if (message) *message = "not connected to the server";
    return false;
  }
  uint64_t seq = s.connect_seq;
  Writer w = ServerMsg(kConnect);
  w.str(code);
  ToServer(w);
  if (!s.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [&] { return s.connect_seq != seq; })) {
    if (message) *message = "no answer from the server";
    return false;
  }
  if (message) *message = s.connect_message;
  if (s.connect_ok) s.joined = code;
  return s.connect_ok;
}

std::string JoinedRoom() {
  std::lock_guard lock(S().mutex);
  return S().joined;
}

std::vector<PeerInfo> Peers() {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<PeerInfo> out;
  auto now = Clock::now();
  for (const auto& [vip, peer] : s.peers) {
    out.push_back({vip, peer.name, peer.direct.has_value(), now - peer.last_heard < kPeerTimeout,
                   peer.rtt_ms});
  }
  return out;
}

}  // namespace harmony::netplay::online
