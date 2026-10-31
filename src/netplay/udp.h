// harmonyrecomp netplay - minimal platform UDP layer.
//
// Kept free of SDK headers so Winsock can be included without clashing with
// anything the SDK pulls in. IPv4 addresses are host byte order uint32_t
// (10.0.0.1 == 0x0A000001) everywhere in this API.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harmony::netplay::udp {

// Initializes the socket library (WSAStartup on Windows). Safe to call often.
bool Startup();

struct Endpoint {
  uint32_t ip = 0;     // host byte order
  uint16_t port = 0;   // host byte order
  bool operator==(const Endpoint&) const = default;
};

std::string ToString(uint32_t ip);
std::string ToString(const Endpoint& ep);
// Parses "a.b.c.d" or "a.b.c.d:port"; missing port -> default_port.
std::optional<Endpoint> Parse(const std::string& text, uint16_t default_port);

struct Interface {
  std::string name;   // adapter friendly name where available
  uint32_t ip = 0;
  uint32_t netmask = 0;
  uint32_t broadcast() const { return (ip & netmask) | ~netmask; }
};
// Up, non-loopback IPv4 interfaces.
std::vector<Interface> ListInterfaces();

// This computer's host name (empty if unavailable).
std::string Hostname();

// IPv4 address of a host name or dotted address (DNS lookup; blocks).
std::optional<uint32_t> Resolve(const std::string& host);

// Local source address the OS would use to reach `dest` (no packet is sent).
std::optional<uint32_t> RouteSourceIp(uint32_t dest);

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  // Binds a UDP socket on 0.0.0.0:port (0 = any) with broadcast enabled.
  bool Open(uint16_t port, std::string* error = nullptr);
  void Close();
  bool is_open() const;
  uint16_t local_port() const;

  bool SendTo(const Endpoint& to, const void* data, size_t size);
  // Waits up to timeout_ms; returns bytes received, 0 on timeout, -1 on error.
  int RecvFrom(void* buffer, size_t size, Endpoint* from, int timeout_ms);

 private:
  intptr_t handle_ = -1;
  uint16_t port_ = 0;
};

}  // namespace harmony::netplay::udp
