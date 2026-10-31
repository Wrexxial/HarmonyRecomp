// harmonyrecomp netplay - minimal platform UDP layer.

#include "udp.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace harmony::netplay::udp {
namespace {

#ifdef _WIN32
using native_t = SOCKET;
constexpr native_t kInvalid = INVALID_SOCKET;
void CloseNative(native_t s) { closesocket(s); }
#else
using native_t = int;
constexpr native_t kInvalid = -1;
void CloseNative(native_t s) { close(s); }
#endif

native_t N(intptr_t h) { return static_cast<native_t>(h); }

sockaddr_in ToSockaddr(const Endpoint& ep) {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(ep.port);
  sa.sin_addr.s_addr = htonl(ep.ip);
  return sa;
}

}  // namespace

bool Startup() {
#ifdef _WIN32
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ok;
#else
  return true;
#endif
}

std::string ToString(uint32_t ip) {
  return std::to_string((ip >> 24) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

std::string ToString(const Endpoint& ep) { return ToString(ep.ip) + ":" + std::to_string(ep.port); }

std::optional<Endpoint> Parse(const std::string& raw, uint16_t default_port) {
  std::string text;
  for (char c : raw) {
    if (c != ' ' && c != '\t') text += c;
  }
  if (text.empty()) return std::nullopt;
  Endpoint ep;
  ep.port = default_port;
  auto colon = text.find(':');
  std::string host = text.substr(0, colon);
  if (colon != std::string::npos) {
    try {
      unsigned long p = std::stoul(text.substr(colon + 1));
      if (p == 0 || p > 65535) return std::nullopt;
      ep.port = static_cast<uint16_t>(p);
    } catch (...) {
      return std::nullopt;
    }
  }
  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return std::nullopt;
  ep.ip = ntohl(addr.s_addr);
  return ep;
}

std::vector<Interface> ListInterfaces() {
  std::vector<Interface> out;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  for (int attempt = 0; attempt < 3; ++attempt) {
    buffer.resize(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) continue;
    if (rc != NO_ERROR) return out;
    for (auto* a = addrs; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      std::string name;
      if (a->FriendlyName) {
        for (const wchar_t* p = a->FriendlyName; *p; ++p) {
          name += (*p < 0x80) ? static_cast<char>(*p) : '?';
        }
      }
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
        Interface itf;
        itf.name = name;
        itf.ip = ntohl(sin->sin_addr.s_addr);
        uint8_t prefix = u->OnLinkPrefixLength;
        itf.netmask = prefix == 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1));
        out.push_back(itf);
      }
    }
    break;
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (auto* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    Interface itf;
    itf.name = i->ifa_name ? i->ifa_name : "";
    itf.ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    itf.netmask =
        i->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr)
                       : 0xFFFFFF00u;
    out.push_back(itf);
  }
  freeifaddrs(list);
#endif
  return out;
}

std::string Hostname() {
  Startup();
  char name[256] = {};
  if (gethostname(name, sizeof(name) - 1) != 0) return {};
  return name;
}

std::optional<uint32_t> RouteSourceIp(uint32_t dest) {
  Startup();
  native_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) return std::nullopt;
  sockaddr_in to = ToSockaddr({dest, 9});
  std::optional<uint32_t> result;
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
        local.sin_addr.s_addr != 0) {
      result = ntohl(local.sin_addr.s_addr);
    }
  }
  CloseNative(s);
  return result;
}

std::optional<uint32_t> Resolve(const std::string& host) {
  Startup();
  if (auto ep = Parse(host, 0)) return ep->ip;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return std::nullopt;
  std::optional<uint32_t> ip;
  for (auto* ai = result; ai; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET) {
      ip = ntohl(reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr.s_addr);
      break;
    }
  }
  freeaddrinfo(result);
  return ip;
}

Socket::~Socket() { Close(); }

bool Socket::Open(uint16_t port, std::string* error) {
  Close();
  Startup();
  native_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalid) {
    if (error) *error = "socket() failed";
    return false;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
  // Without this, an ICMP "port unreachable" for an earlier send makes the
  // next recvfrom on this UDP socket fail with WSAECONNRESET.
  constexpr DWORD kSioUdpConnReset = _WSAIOW(IOC_VENDOR, 12);
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(s, kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
  sockaddr_in local = ToSockaddr({0, port});
  if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    if (error) *error = "bind() to UDP port " + std::to_string(port) + " failed (port in use?)";
    CloseNative(s);
    return false;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
  port_ = ntohs(bound.sin_port);
  handle_ = static_cast<intptr_t>(s);
  return true;
}

void Socket::Close() {
  if (handle_ != -1) {
    CloseNative(N(handle_));
    handle_ = -1;
  }
}

bool Socket::is_open() const { return handle_ != -1; }
uint16_t Socket::local_port() const { return port_; }

bool Socket::SendTo(const Endpoint& to, const void* data, size_t size) {
  if (handle_ == -1) return false;
  sockaddr_in sa = ToSockaddr(to);
  auto sent = sendto(N(handle_), static_cast<const char*>(data), static_cast<int>(size), 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
  return sent == static_cast<decltype(sent)>(size);
}

int Socket::RecvFrom(void* buffer, size_t size, Endpoint* from, int timeout_ms) {
  if (handle_ == -1) return -1;
#ifdef _WIN32
  fd_set set;
  FD_ZERO(&set);
  FD_SET(N(handle_), &set);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int ready = select(0, &set, nullptr, nullptr, &tv);
#else
  pollfd p{N(handle_), POLLIN, 0};
  int ready = poll(&p, 1, timeout_ms);
#endif
  if (ready <= 0) return ready == 0 ? 0 : -1;
  sockaddr_in sa{};
  socklen_t len = sizeof(sa);
  auto n = recvfrom(N(handle_), static_cast<char*>(buffer), static_cast<int>(size), 0,
                    reinterpret_cast<sockaddr*>(&sa), &len);
  if (n < 0) {
#ifdef _WIN32
    // An ICMP "port unreachable" from an earlier send surfaces here on
    // Windows; it is not an error for the listening socket.
    if (WSAGetLastError() == WSAECONNRESET) return 0;
#endif
    return -1;
  }
  if (from) {
    from->ip = ntohl(sa.sin_addr.s_addr);
    from->port = ntohs(sa.sin_port);
  }
  return static_cast<int>(n);
}

}  // namespace harmony::netplay::udp
