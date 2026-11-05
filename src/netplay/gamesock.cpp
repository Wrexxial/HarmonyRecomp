// harmonyrecomp netplay - the game's own UDP traffic (VDP socket on port 1000).
//
// The SDK's versions of these imports are not usable for a real network game:
//  * WSARecvFrom always fails (no error code, wrong parameter list: fromlen is
//    missing, so the overlapped pointer is read from the wrong slot);
//  * WSAGetOverlappedResult is a stub;
//  * WSASendTo never completes its overlapped and ignores errors;
//  * XSocket::SendTo/RecvFrom swap the bytes of the IP address and port.
// The game posts one overlapped receive per socket, waits on its event and then
// calls WSAGetOverlappedResult - if the receive cannot be posted it reports a
// "network error" right after creating the session. These replacements work on
// the native socket directly and complete overlapped requests from a small
// poll thread.

#include "gamesock.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

// SDK headers first: xsocket.h declares enumerators named like the socket
// macros (AF_INET, ...), which must not be defined yet.
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include "netplay.h"
#include "online.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif


namespace harmony::netplay::gamesock {
namespace {

// The game binds its VDP socket to port 1000. Linux (also under Wine/Proton,
// Steam Deck) only lets root bind ports below 1024, so session creation failed
// there. Real (LAN/VPN) sockets therefore use port + kHostPortOffset on every
// system, and the game keeps seeing its own port numbers. The virtual network
// (online server) is not affected: it carries the game's port numbers itself.
constexpr uint16_t kHostPortOffset = 48000;
uint16_t HostPort(uint16_t game_port) {
  return game_port != 0 && game_port < 1024 ? uint16_t(game_port + kHostPortOffset) : game_port;
}
uint16_t GamePort(uint16_t host_port) {
  return host_port >= kHostPortOffset && host_port < kHostPortOffset + 1024
             ? uint16_t(host_port - kHostPortOffset)
             : host_port;
}
void ToHostPort(sockaddr_in* a) { a->sin_port = htons(HostPort(ntohs(a->sin_port))); }
void ToGamePort(sockaddr_in* a) { a->sin_port = htons(GamePort(ntohs(a->sin_port))); }


using rex::system::XThread;

#if defined(_WIN32)
using native_t = SOCKET;
using socklen_type = int;
#else
using native_t = int;
using socklen_type = socklen_t;
#endif

constexpr uint32_t kStatusPending = 0x103;     // STATUS_PENDING in overlapped.internal
constexpr uint32_t kStatusErrorTag = 0xC0DE0000;  // our failures: tag | WSA error

constexpr uint32_t kWsaIoIncomplete = 996;
constexpr uint32_t kWsaIoPending = 997;
constexpr uint32_t kWsaOperationAborted = 995;
constexpr uint32_t kWsaEFault = 10014;
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kWsaENetUnreach = 10051;
constexpr uint32_t kWsaEConnReset = 10054;
constexpr uint32_t kWsaEHostUnreach = 10065;

constexpr size_t kMaxDatagram = 65536;

struct GuestOverlapped {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  rex::be<uint32_t> offset;
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};

struct GuestWsaBuf {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf;
};

// Guest XSOCKADDR_IN: family (BE), then port and address in network order -
// exactly the bytes a native sockaddr_in holds, so they are copied raw.
struct GuestSockaddrIn {
  rex::be<uint16_t> family;
  uint8_t port[2];
  uint8_t addr[4];
  uint8_t zero[8];
};

template <typename T>
T* G(uint32_t guest_address) {
  return guest_address ? REX_KERNEL_MEMORY()->TranslateVirtual<T*>(guest_address) : nullptr;
}

// Guest-visible WSAGetLastError. Hooks always run on guest threads in the game;
// the check keeps the code usable from host threads (tests) too.
thread_local uint32_t t_host_last_error = 0;
void SetError(uint32_t error) {
  if (XThread::IsInThread()) {
    XThread::SetLastError(error);
  } else {
    t_host_last_error = error;
  }
}

native_t N(uint64_t handle) { return static_cast<native_t>(handle); }

uint32_t LastNativeError() {
#if defined(_WIN32)
  return static_cast<uint32_t>(WSAGetLastError());
#else
  switch (errno) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
      return kWsaEWouldBlock;
    case EMSGSIZE:
      return kWsaEMsgSize;
    case ECONNREFUSED:
    case ECONNRESET:
      return kWsaEConnReset;
    case ENETUNREACH:
      return kWsaENetUnreach;
    case EHOSTUNREACH:
      return kWsaEHostUnreach;
    case EBADF:
    case ENOTSOCK:
      return kWsaENotSock;
    default:
      return kWsaEFault;
  }
#endif
}

// ---- Socket table ---------------------------------------------------------------
//
// The game's sockets live in a table of our own instead of the SDK's kernel
// object table. A handle there can be released by unrelated code: the SDK caches
// the handle of an event inside the guest KEVENT, and a late ObDereferenceObject
// on freed event memory released whatever object had reused that handle number -
// the game socket. Sends then failed with WSAENOTSOCK (the game dropped the other
// player with 0x807A100A) and port 1000 stayed open for good.

struct GameSocket {
  uint64_t native = 0;
  int type = 0;      // 1 stream, 2 datagram
  int protocol = 0;  // 6 TCP, 17 UDP, 254 VDP
};

constexpr uint32_t kSocketHandleBase = 0x5C000004;

std::mutex g_table_mutex;
std::map<uint32_t, GameSocket> g_table;
uint32_t g_next_handle = kSocketHandleBase;

// ---- Online play (netplay/online.h) -------------------------------------------------
//
// Other players have virtual addresses (10.66.x.y). Datagrams the game sends
// there go through the online module; datagrams from them are queued here per
// socket and handed to the game's receives before the native socket's.

struct VirtualDatagram {
  uint32_t src_ip = 0;
  uint16_t src_port = 0;
  std::vector<uint8_t> data;
};

std::mutex g_inbox_mutex;
std::map<uint32_t, std::deque<VirtualDatagram>> g_inbox;  // socket handle -> datagrams
std::map<uint32_t, uint16_t> g_virtual_ports;             // socket handle -> bound port

bool InboxPeek(uint32_t socket, size_t* size) {
  std::lock_guard lock(g_inbox_mutex);
  auto it = g_inbox.find(socket);
  if (it == g_inbox.end() || it->second.empty()) return false;
  if (size) *size = it->second.front().data.size();
  return true;
}

bool InboxPop(uint32_t socket, VirtualDatagram* out) {
  std::lock_guard lock(g_inbox_mutex);
  auto it = g_inbox.find(socket);
  if (it == g_inbox.end() || it->second.empty()) return false;
  *out = std::move(it->second.front());
  it->second.pop_front();
  return true;
}

void ListenVirtual(uint32_t socket, uint16_t port);
void UnlistenVirtual(uint32_t socket);

bool NativeFor(uint32_t socket_handle, uint64_t* native) {
  std::lock_guard lock(g_table_mutex);
  auto it = g_table.find(socket_handle);
  if (it == g_table.end()) return false;
  *native = it->second.native;
  return true;
}

void CloseNative(uint64_t native) {
#if defined(_WIN32)
  closesocket(N(native));
#else
  close(N(native));
#endif
}

bool Readable(native_t s) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{0, 0};
  return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

void SignalEvent(uint32_t event_handle, bool set) {
  if (!event_handle) return;
  if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event_handle)) {
    if (set) {
      ev->Set(0, false);
    } else {
      ev->Reset();
    }
  }
}

void CompleteOverlapped(uint32_t overlapped, uint32_t status, uint32_t bytes) {
  auto* ov = G<GuestOverlapped>(overlapped);
  if (!ov) return;
  ov->internal_high = bytes;
  std::atomic_thread_fence(std::memory_order_release);
  ov->internal = status;
  SignalEvent(ov->event_handle, true);
}

void WriteFrom(uint32_t from_ptr, uint32_t fromlen_ptr, const sockaddr_in& from) {
  if (auto* out = G<GuestSockaddrIn>(from_ptr)) {
    out->family = 2;  // AF_INET
    std::memcpy(out->port, &from.sin_port, 2);
    std::memcpy(out->addr, &from.sin_addr, 4);
    std::memset(out->zero, 0, sizeof(out->zero));
  }
  if (auto* len = G<rex::be<uint32_t>>(fromlen_ptr)) {
    *len = static_cast<uint32_t>(sizeof(GuestSockaddrIn));
  }
}

bool ReadTo(uint32_t to_ptr, sockaddr_in* to) {
  auto* in = G<GuestSockaddrIn>(to_ptr);
  if (!in) return false;
  std::memset(to, 0, sizeof(*to));
  to->sin_family = AF_INET;
  std::memcpy(&to->sin_port, in->port, 2);
  std::memcpy(&to->sin_addr, in->addr, 4);
  return true;
}

std::string Describe(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

// ---- Receive --------------------------------------------------------------------

struct RecvOp {
  uint32_t socket = 0;
  uint64_t native = 0;
  uint32_t buffers = 0;
  uint32_t count = 0;
  uint32_t flags_ptr = 0;
  uint32_t from_ptr = 0;
  uint32_t fromlen_ptr = 0;
  uint32_t overlapped = 0;
};

enum class RecvResult { kData, kWouldBlock, kError };

// Receives one datagram into the guest buffers. Datagrams the game cannot use
// (ICMP "port unreachable" echoes, oversize packets) are dropped, like the
// console's network stack does for VDP.
RecvResult TryRecv(const RecvOp& op, uint32_t* bytes, uint32_t* error) {
  const native_t s = N(op.native);
  thread_local std::vector<uint8_t> scratch(kMaxDatagram);
  auto* bufs = G<GuestWsaBuf>(op.buffers);
  size_t capacity = 0;
  for (uint32_t i = 0; bufs && i < op.count; ++i) capacity += bufs[i].len;
  capacity = std::min(capacity, kMaxDatagram);

  VirtualDatagram vd;
  while (InboxPop(op.socket, &vd)) {
    if (vd.data.size() > capacity) continue;  // larger than the game's buffer: drop
    size_t offset = 0;
    for (uint32_t i = 0; i < op.count && offset < vd.data.size(); ++i) {
      size_t chunk = std::min<size_t>(bufs[i].len, vd.data.size() - offset);
      if (auto* dst = G<uint8_t>(bufs[i].buf)) std::memcpy(dst, vd.data.data() + offset, chunk);
      offset += chunk;
    }
    sockaddr_in from{};
    from.sin_family = AF_INET;
    from.sin_port = htons(vd.src_port);
    from.sin_addr.s_addr = htonl(vd.src_ip);
    WriteFrom(op.from_ptr, op.fromlen_ptr, from);
    if (auto* flags = G<rex::be<uint32_t>>(op.flags_ptr)) *flags = 0u;
    *bytes = static_cast<uint32_t>(vd.data.size());
    return RecvResult::kData;
  }

  for (int attempt = 0; attempt < 16; ++attempt) {
    if (!Readable(s)) return RecvResult::kWouldBlock;
    sockaddr_in from{};
    socklen_type from_len = sizeof(from);
    auto n = recvfrom(s, reinterpret_cast<char*>(scratch.data()), static_cast<int>(kMaxDatagram), 0,
                      reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      uint32_t e = LastNativeError();
      if (e == kWsaEWouldBlock) return RecvResult::kWouldBlock;
      if (e == kWsaEConnReset || e == kWsaEMsgSize || e == kWsaENetUnreach ||
          e == kWsaEHostUnreach) {
        continue;  // drop and look for the next datagram
      }
      *error = e;
      return RecvResult::kError;
    }
    if (static_cast<size_t>(n) > capacity) {
      continue;  // larger than the game's buffer: drop
    }
    size_t offset = 0;
    for (uint32_t i = 0; i < op.count && offset < static_cast<size_t>(n); ++i) {
      size_t chunk = std::min<size_t>(bufs[i].len, static_cast<size_t>(n) - offset);
      if (auto* dst = G<uint8_t>(bufs[i].buf)) std::memcpy(dst, scratch.data() + offset, chunk);
      offset += chunk;
    }
    ToGamePort(&from);
    WriteFrom(op.from_ptr, op.fromlen_ptr, from);
    if (auto* flags = G<rex::be<uint32_t>>(op.flags_ptr)) *flags = 0u;
    *bytes = static_cast<uint32_t>(n);
    return RecvResult::kData;
  }
  return RecvResult::kWouldBlock;
}

std::mutex g_mutex;  // guards g_pending and every recvfrom on a game socket
std::condition_variable g_cv;
std::vector<RecvOp> g_pending;
std::thread g_pump;
std::atomic<bool> g_running{false};
std::once_flag g_pump_once;

std::atomic<uint32_t> g_recv_count{0};
std::atomic<uint32_t> g_send_count{0};

void NoteReceive(uint32_t bytes) {
  uint32_t n = ++g_recv_count;
  if (n == 1 || n == 100 || n % 10000 == 0) {
    NETPLAY_LOG("game traffic: {} packets received (last {} bytes)", n, bytes);
  }
}

void PumpThread() {
  while (g_running) {
    std::vector<native_t> watch;
    {
      std::unique_lock lock(g_mutex);
      if (g_pending.empty()) {
        g_cv.wait_for(lock, std::chrono::milliseconds(50));
        continue;
      }
      for (size_t i = 0; i < g_pending.size();) {
        const RecvOp& op = g_pending[i];
        uint32_t bytes = 0, error = 0;
        auto r = TryRecv(op, &bytes, &error);
        if (r == RecvResult::kWouldBlock) {
          watch.push_back(N(op.native));
          ++i;
          continue;
        }
        if (r == RecvResult::kData) {
          CompleteOverlapped(op.overlapped, 0, bytes);
          NoteReceive(bytes);
        } else {
          CompleteOverlapped(op.overlapped, kStatusErrorTag | (error & 0xFFFF), 0);
        }
        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
    if (watch.empty()) continue;
    // Wait for data without holding the lock (closesocket may need it).
    fd_set set;
    FD_ZERO(&set);
    int max_fd = 0;
    for (size_t i = 0; i < watch.size() && i < 60; ++i) {
      FD_SET(watch[i], &set);
      max_fd = std::max(max_fd, static_cast<int>(watch[i]));
    }
    timeval tv{0, 2000};
    if (select(max_fd + 1, &set, nullptr, nullptr, &tv) < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void EnsurePump() {
  std::call_once(g_pump_once, [] {
    g_running = true;
    g_pump = std::thread(PumpThread);
  });
}

void ListenVirtual(uint32_t socket, uint16_t port) {
  {
    std::lock_guard lock(g_inbox_mutex);
    g_virtual_ports[socket] = port;
    g_inbox[socket];
  }
  online::Listen(port, [socket](uint32_t src_vip, uint16_t src_port, const uint8_t* data,
                                size_t size) {
    {
      std::lock_guard lock(g_inbox_mutex);
      auto it = g_inbox.find(socket);
      if (it == g_inbox.end()) return;
      if (it->second.size() >= 256) it->second.pop_front();  // nobody reads: keep it bounded
      it->second.push_back({src_vip, src_port, std::vector<uint8_t>(data, data + size)});
    }
    NoteReceive(static_cast<uint32_t>(size));
    g_cv.notify_one();
  });
  NETPLAY_LOG("game socket {:08X} also reachable online on port {}", socket, port);
}

void UnlistenVirtual(uint32_t socket) {
  uint16_t port = 0;
  {
    std::lock_guard lock(g_inbox_mutex);
    auto it = g_virtual_ports.find(socket);
    if (it == g_virtual_ports.end()) return;
    port = it->second;
    g_virtual_ports.erase(it);
    g_inbox.erase(socket);
  }
  online::Unlisten(port);
}

// ---- Hooked imports ---------------------------------------------------------------

u32 WSARecvFrom(u32 caller, u32 socket, u32 buffers, u32 buffer_count, u32 bytes_ptr,
                u32 flags_ptr, u32 from_ptr, u32 fromlen_ptr, u32 overlapped,
                u32 completion_routine) {
  (void)caller;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  if (completion_routine) {
    static std::once_flag warned;
    std::call_once(warned, [] { NETPLAY_LOG("WSARecvFrom: completion routines are not supported"); });
  }
  RecvOp op{socket, native, buffers, buffer_count, flags_ptr, from_ptr, fromlen_ptr, overlapped};

  std::lock_guard lock(g_mutex);
  uint32_t bytes = 0, error = 0;
  switch (TryRecv(op, &bytes, &error)) {
    case RecvResult::kData:
      if (auto* out = G<rex::be<uint32_t>>(bytes_ptr)) *out = bytes;
      CompleteOverlapped(overlapped, 0, bytes);
      NoteReceive(bytes);
      return 0;
    case RecvResult::kError:
      SetError(error);
      return ~0u;
    case RecvResult::kWouldBlock:
      break;
  }
  if (!overlapped) {
    SetError(kWsaEWouldBlock);
    return ~0u;
  }
  if (auto* ov = G<GuestOverlapped>(overlapped)) {
    ov->internal_high = 0u;
    ov->internal = kStatusPending;
    SignalEvent(ov->event_handle, false);
  }
  EnsurePump();
  g_pending.push_back(op);
  g_cv.notify_one();
  SetError(kWsaIoPending);
  return ~0u;
}

u32 WSASendTo(u32 caller, u32 socket, u32 buffers, u32 buffer_count, u32 bytes_sent_ptr,
              u32 flags, u32 to_ptr, u32 to_len, u32 overlapped, u32 completion_routine) {
  (void)caller;
  (void)to_len;
  (void)flags;
  (void)completion_routine;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  std::vector<uint8_t> data;
  auto* bufs = G<GuestWsaBuf>(buffers);
  for (uint32_t i = 0; bufs && i < buffer_count; ++i) {
    const uint32_t len = bufs[i].len;
    if (auto* src = G<uint8_t>(bufs[i].buf)) data.insert(data.end(), src, src + len);
  }
  sockaddr_in to{};
  const bool has_to = ReadTo(to_ptr, &to);
  if (has_to && online::IsVirtual(ntohl(to.sin_addr.s_addr))) {
    uint16_t src_port = 0;
    {
      std::lock_guard lock(g_inbox_mutex);
      auto it = g_virtual_ports.find(socket);
      if (it != g_virtual_ports.end()) src_port = it->second;
    }
    if (!online::Send(ntohl(to.sin_addr.s_addr), src_port, ntohs(to.sin_port), data.data(),
                      data.size())) {
      static std::atomic<uint32_t> unknown{0};
      if (++unknown <= 5) NETPLAY_LOG("game send to {}: player not connected (yet)", Describe(to));
    }
    // Like UDP: a datagram to an unreachable player is simply lost.
    const auto n = static_cast<uint32_t>(data.size());
    if (auto* out = G<rex::be<uint32_t>>(bytes_sent_ptr)) *out = n;
    CompleteOverlapped(overlapped, 0, n);
    return 0;
  }
  if (has_to) ToHostPort(&to);
  auto n = has_to ? sendto(N(native), reinterpret_cast<const char*>(data.data()),
                           static_cast<int>(data.size()), 0,
                           reinterpret_cast<const sockaddr*>(&to), sizeof(to))
                  : send(N(native), reinterpret_cast<const char*>(data.data()),
                         static_cast<int>(data.size()), 0);
  if (n < 0) {
    uint32_t e = LastNativeError();
    static std::atomic<uint32_t> failures{0};
    if (++failures <= 5) {
      NETPLAY_LOG("game send to {} failed (error {})", has_to ? Describe(to) : "?", e);
    }
    SetError(e);
    return ~0u;
  }
  uint32_t count = ++g_send_count;
  if (has_to) {
    static std::mutex seen_mutex;
    static std::set<std::pair<uint32_t, uint16_t>> seen;
    std::lock_guard lock(seen_mutex);
    if (seen.insert({to.sin_addr.s_addr, to.sin_port}).second) {
      NETPLAY_LOG("game traffic: first packet to {} ({} bytes)", Describe(to), n);
    }
  }
  (void)count;
  if (auto* out = G<rex::be<uint32_t>>(bytes_sent_ptr)) *out = static_cast<uint32_t>(n);
  CompleteOverlapped(overlapped, 0, static_cast<uint32_t>(n));
  return 0;
}

u32 WSAGetOverlappedResult(u32 caller, u32 socket, u32 overlapped, u32 bytes_ptr, u32 wait,
                           u32 flags_ptr) {
  (void)caller;
  (void)socket;
  auto* ov = G<GuestOverlapped>(overlapped);
  if (!ov) {
    SetError(kWsaEFault);
    return 0;
  }
  uint32_t status = ov->internal;
  if (status == kStatusPending) {
    if (!wait) {
      SetError(kWsaIoIncomplete);
      return 0;
    }
    while ((status = ov->internal) == kStatusPending) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (status == 0) {
    if (auto* out = G<rex::be<uint32_t>>(bytes_ptr)) *out = static_cast<uint32_t>(ov->internal_high);
    if (auto* flags = G<rex::be<uint32_t>>(flags_ptr)) *flags = 0u;
    return 1;
  }
  uint32_t error = (status & 0xFFFF0000u) == kStatusErrorTag ? (status & 0xFFFF) : kWsaOperationAborted;
  SetError(error);
  return 0;
}

u32 RecvFrom(u32 caller, u32 socket, u32 buf_ptr, u32 buf_len, u32 flags, u32 from_ptr,
             u32 fromlen_ptr) {
  (void)caller;
  (void)flags;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  VirtualDatagram vd;
  if (InboxPop(socket, &vd)) {
    const size_t n = std::min<size_t>(vd.data.size(), buf_len);
    if (auto* out = G<uint8_t>(buf_ptr)) std::memcpy(out, vd.data.data(), n);
    sockaddr_in vfrom{};
    vfrom.sin_family = AF_INET;
    vfrom.sin_port = htons(vd.src_port);
    vfrom.sin_addr.s_addr = htonl(vd.src_ip);
    WriteFrom(from_ptr, fromlen_ptr, vfrom);
    return static_cast<u32>(n);
  }
  sockaddr_in from{};
  socklen_type from_len = sizeof(from);
  auto* dst = G<char>(buf_ptr);
  auto n = recvfrom(N(native), dst, static_cast<int>(buf_len), 0,
                    reinterpret_cast<sockaddr*>(&from), &from_len);
  if (n < 0) {
    SetError(LastNativeError());
    return ~0u;
  }
  ToGamePort(&from);
  WriteFrom(from_ptr, fromlen_ptr, from);
  return static_cast<u32>(n);
}

u32 Socket(u32 caller, u32 af, u32 type, u32 protocol) {
  (void)caller;
  if (af != 2) {
    SetError(10047);  // WSAEAFNOSUPPORT
    return ~0u;
  }
  int native_type = type == 1 ? SOCK_STREAM : SOCK_DGRAM;
  int native_protocol = 0;
  if (protocol == 6) native_protocol = IPPROTO_TCP;
  if (protocol == 17 || protocol == 254) native_protocol = IPPROTO_UDP;  // VDP rides on UDP
  auto s = ::socket(AF_INET, native_type, native_protocol);
#if defined(_WIN32)
  if (s == INVALID_SOCKET) {
#else
  if (s < 0) {
#endif
    SetError(LastNativeError());
    return ~0u;
  }
#if defined(_WIN32)
  if (native_type == SOCK_DGRAM) {
    // Without this, an ICMP "port unreachable" from a peer that is not running
    // yet makes the next receive fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr,
             nullptr);
  }
#endif
  uint32_t handle;
  {
    std::lock_guard lock(g_table_mutex);
    handle = g_next_handle;
    g_next_handle += 4;
    g_table[handle] = GameSocket{static_cast<uint64_t>(s), static_cast<int>(type),
                                 static_cast<int>(protocol)};
  }
  return handle;
}

u32 Bind(u32 caller, u32 socket, u32 name_ptr, u32 name_len) {
  (void)caller;
  (void)name_len;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  sockaddr_in addr{};
  if (!ReadTo(name_ptr, &addr)) {
    SetError(kWsaEFault);
    return ~0u;
  }
  const uint16_t game_port = ntohs(addr.sin_port);
  ToHostPort(&addr);
  if (::bind(N(native), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    uint32_t e = LastNativeError();
    if (Enabled()) {
      NETPLAY_LOG("game socket {:08X} could not bind port {} (game port {}, error {}: port in use?)",
                  socket, ntohs(addr.sin_port), game_port, e);
    }
    SetError(e);
    return ~0u;
  }
  // These ports must be allowed through the firewall (next to the lobby port)
  // for other players to reach this PC.
  if (Enabled()) {
    NETPLAY_LOG("game socket {:08X} bound to port {} (game port {})", socket, ntohs(addr.sin_port),
                game_port);
  }
  if (online::Connected()) {
    sockaddr_in bound{};
    socklen_type bound_len = sizeof(bound);
    uint16_t port = game_port;
    if (getsockname(N(native), reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
      port = GamePort(ntohs(bound.sin_port));
    }
    ListenVirtual(socket, port);
  }
  return 0;
}

u32 SetSockOpt(u32 caller, u32 socket, u32 level, u32 optname, u32 optval_ptr, u32 optlen) {
  (void)caller;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  int native_level = 0, native_name = 0;
  if (level == 0xFFFF) {
    native_level = SOL_SOCKET;
    switch (optname) {
      case 0x0004: native_name = SO_REUSEADDR; break;
      case 0x0020: native_name = SO_BROADCAST; break;
      case 0x1001: native_name = SO_SNDBUF; break;
      case 0x1002: native_name = SO_RCVBUF; break;
      default: break;  // 0x5801/0x5802 (Xbox socket security) and the rest: accepted
    }
  } else if (level == 6 && optname == 1) {
    native_level = IPPROTO_TCP;
    native_name = TCP_NODELAY;
  }
  if (!native_name || optlen < 4 || !optval_ptr) {
    static std::atomic<int> logged{0};
    if (native_name == 0 && optname != 0x5801 && optname != 0x5802 && logged++ < 10 && Enabled()) {
      NETPLAY_LOG("setsockopt level {:X} option {:X} ignored", level, optname);
    }
    return 0;
  }
  int value = static_cast<int>(static_cast<uint32_t>(*G<rex::be<uint32_t>>(optval_ptr)));
  if (setsockopt(N(native), native_level, native_name, reinterpret_cast<const char*>(&value),
                 sizeof(value)) != 0) {
    SetError(LastNativeError());
    return ~0u;
  }
  return 0;
}

u32 IoctlSocket(u32 caller, u32 socket, u32 cmd, u32 arg_ptr) {
  (void)caller;
  uint64_t native = 0;
  if (!NativeFor(socket, &native)) {
    SetError(kWsaENotSock);
    return ~0u;
  }
  auto* arg = G<rex::be<uint32_t>>(arg_ptr);
  constexpr u32 kFionbio = 0x8004667E;
  constexpr u32 kFionread = 0x4004667F;
  if (cmd == kFionbio) {
#if defined(_WIN32)
    u_long value = arg ? static_cast<uint32_t>(*arg) : 0;
    if (ioctlsocket(N(native), FIONBIO, &value) != 0) {
#else
    int value = arg ? static_cast<int>(static_cast<uint32_t>(*arg)) : 0;
    if (ioctl(N(native), FIONBIO, &value) != 0) {
#endif
      SetError(LastNativeError());
      return ~0u;
    }
    return 0;
  }
  if (cmd == kFionread) {
    size_t queued = 0;
    if (InboxPeek(socket, &queued)) {
      if (arg) *arg = static_cast<uint32_t>(queued);
      return 0;
    }
#if defined(_WIN32)
    u_long value = 0;
    if (ioctlsocket(N(native), FIONREAD, &value) != 0) {
#else
    int value = 0;
    if (ioctl(N(native), FIONREAD, &value) != 0) {
#endif
      SetError(LastNativeError());
      return ~0u;
    }
    if (arg) *arg = static_cast<uint32_t>(value);
    return 0;
  }
  if (Enabled()) NETPLAY_LOG("ioctlsocket {:08X} command {:08X} ignored", socket, cmd);
  return 0;
}

// Guest fd_set: be32 count, be32 handles[64].
struct GuestFdSet {
  rex::be<uint32_t> count;
  rex::be<uint32_t> handles[64];
};

u32 Select(u32 caller, u32 nfds, u32 read_ptr, u32 write_ptr, u32 except_ptr, u32 timeout_ptr) {
  (void)caller;
  (void)nfds;
  GuestFdSet* guest_sets[3] = {G<GuestFdSet>(read_ptr), G<GuestFdSet>(write_ptr),
                               G<GuestFdSet>(except_ptr)};
  fd_set native_sets[3];
  std::vector<std::pair<uint32_t, uint64_t>> members[3];
  int max_fd = 0;
  bool any = false;
  for (int k = 0; k < 3; ++k) {
    FD_ZERO(&native_sets[k]);
    if (!guest_sets[k]) continue;
    uint32_t count = std::min<uint32_t>(guest_sets[k]->count, 64);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t handle = guest_sets[k]->handles[i];
      uint64_t native = 0;
      if (!NativeFor(handle, &native)) {
        SetError(kWsaENotSock);
        return ~0u;
      }
      FD_SET(N(native), &native_sets[k]);
      max_fd = std::max(max_fd, static_cast<int>(N(native)));
      members[k].push_back({handle, native});
      any = true;
    }
  }
  timeval tv{};
  timeval* tvp = nullptr;
  if (auto* guest_tv = G<rex::be<uint32_t>>(timeout_ptr)) {
    tv.tv_sec = static_cast<int32_t>(static_cast<uint32_t>(guest_tv[0]));
    tv.tv_usec = static_cast<int32_t>(static_cast<uint32_t>(guest_tv[1]));
    tvp = &tv;
  }
  // Sockets with queued datagrams from online players are readable now.
  std::vector<uint32_t> virtual_ready;
  for (auto& [handle, native] : members[0]) {
    if (InboxPeek(handle, nullptr)) virtual_ready.push_back(handle);
  }
  if (!virtual_ready.empty()) {
    tv = timeval{0, 0};
    tvp = &tv;
  }
  int ret;
  if (!any) {
    // Windows rejects a select without sockets; behave like a plain wait.
    if (tvp) std::this_thread::sleep_for(std::chrono::seconds(tv.tv_sec) +
                                         std::chrono::microseconds(tv.tv_usec));
    ret = 0;
  } else {
    ret = ::select(max_fd + 1, guest_sets[0] ? &native_sets[0] : nullptr,
                   guest_sets[1] ? &native_sets[1] : nullptr,
                   guest_sets[2] ? &native_sets[2] : nullptr, tvp);
    if (ret < 0) {
      SetError(LastNativeError());
      return ~0u;
    }
  }
  int ready = 0;
  for (int k = 0; k < 3; ++k) {
    if (!guest_sets[k]) continue;
    uint32_t out = 0;
    for (auto& [handle, native] : members[k]) {
      bool set = FD_ISSET(N(native), &native_sets[k]) ||
                 (k == 0 && std::find(virtual_ready.begin(), virtual_ready.end(), handle) !=
                                virtual_ready.end());
      if (set) guest_sets[k]->handles[out++] = handle;
    }
    guest_sets[k]->count = out;
    ready += static_cast<int>(out);
  }
  (void)ret;
  return static_cast<u32>(ready);
}

u32 CloseSocket(u32 caller, u32 socket) {
  (void)caller;
  int aborted = 0;
  {
    std::lock_guard lock(g_mutex);  // no receive is running on it while we close
    for (size_t i = 0; i < g_pending.size();) {
      if (g_pending[i].socket == socket) {
        CompleteOverlapped(g_pending[i].overlapped, kStatusErrorTag | kWsaOperationAborted, 0);
        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        ++aborted;
      } else {
        ++i;
      }
    }
    GameSocket entry;
    {
      std::lock_guard table_lock(g_table_mutex);
      auto it = g_table.find(socket);
      if (it == g_table.end()) {
        SetError(kWsaENotSock);
        return ~0u;
      }
      entry = it->second;
      g_table.erase(it);
    }
    CloseNative(entry.native);
  }
  UnlistenVirtual(socket);
  if (Enabled()) {
    NETPLAY_LOG("game socket {:08X} closed{}", socket,
                aborted ? fmt::format(", {} pending receive(s) aborted", aborted) : "");
  }
  return 0;
}

}  // namespace

uint32_t HostLastError() { return t_host_last_error; }

void Shutdown() {
  if (!g_running.exchange(false)) return;
  g_cv.notify_all();
  if (g_pump.joinable()) g_pump.join();
  std::lock_guard lock(g_mutex);
  g_pending.clear();
}

}  // namespace harmony::netplay::gamesock

REX_HOOK(__imp__NetDll_socket, harmony::netplay::gamesock::Socket);
REX_HOOK(__imp__NetDll_bind, harmony::netplay::gamesock::Bind);
REX_HOOK(__imp__NetDll_setsockopt, harmony::netplay::gamesock::SetSockOpt);
REX_HOOK(__imp__NetDll_ioctlsocket, harmony::netplay::gamesock::IoctlSocket);
REX_HOOK(__imp__NetDll_select, harmony::netplay::gamesock::Select);
REX_HOOK(__imp__NetDll_closesocket, harmony::netplay::gamesock::CloseSocket);
REX_HOOK(__imp__NetDll_recvfrom, harmony::netplay::gamesock::RecvFrom);
REX_HOOK(__imp__NetDll_WSARecvFrom, harmony::netplay::gamesock::WSARecvFrom);
REX_HOOK(__imp__NetDll_WSASendTo, harmony::netplay::gamesock::WSASendTo);
REX_HOOK(__imp__NetDll_WSAGetOverlappedResult, harmony::netplay::gamesock::WSAGetOverlappedResult);
