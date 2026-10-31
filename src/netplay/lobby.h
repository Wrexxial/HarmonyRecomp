// harmonyrecomp netplay - LAN lobby service.
//
// Replaces the Xbox Live matchmaking server with a small UDP protocol:
// hosts answer search and QoS requests for the sessions they advertise,
// searchers broadcast on every local subnet and also ask fixed peer
// addresses (for VPNs that do not forward broadcasts, e.g. Tailscale).
//
// No SDK headers here, so this part can be tested natively.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "udp.h"

namespace harmony::netplay::lobby {

constexpr uint16_t kDefaultPort = 47658;

struct Context {
  uint32_t id = 0;
  uint32_t value = 0;
};

// A session property as the game set it: id (type in the top nibble) and the
// raw big-endian value bytes (for strings/binary: the payload itself).
struct Property {
  uint32_t id = 0;
  std::vector<uint8_t> value;
};

// What a host advertises for one session.
struct SessionAd {
  uint64_t id = 0;  // XNKID read as a big-endian u64
  uint8_t key[16] = {};
  uint32_t flags = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint32_t filled_public = 0;
  uint32_t filled_private = 0;
  uint32_t state = 0;  // XSESSION_STATE
  std::string host_name;
  std::vector<Context> contexts;
  std::vector<Property> properties;
};

// A session found by Search(): the advertisement plus where it came from.
struct Found {
  SessionAd ad;
  udp::Endpoint from;  // host's lobby service address (IP the host is reachable on)
  uint32_t rtt_ms = 0;
  std::string build;              // host's game version, e.g. "2.04"
  std::vector<std::string> dlc;   // host's DLC packages
};

// A host the last Search() found but left out, because playing together would
// desync (other game version or HarmonyRecomp netplay protocol).
struct Mismatch {
  uint32_t ip = 0;
  std::string host_name;
  std::string reason;
};

struct QosResult {
  bool contacted = false;
  bool enabled = false;
  uint32_t rtt_ms = 0;
  std::vector<uint8_t> data;
};

struct Config {
  uint16_t port = kDefaultPort;         // our own lobby port
  uint16_t remote_port = kDefaultPort;  // lobby port of other players (broadcast target)
  std::vector<udp::Endpoint> peers;     // extra unicast targets for searches
  uint32_t title_id = 0;
  // Compared between players: sessions of hosts with another build are not
  // offered (other game version = other code and data = desync).
  std::string build;
  std::vector<std::string> dlc;
};

using LogFn = void (*)(const std::string& message);
void SetLogger(LogFn fn);
// Short messages for the player (shown in the game), e.g. why a lobby was
// left out of a search.
void SetNoticeSink(LogFn fn);

bool Start(const Config& config, std::string* error);
void Stop();
bool Running();
uint16_t BoundPort();

void Advertise(const SessionAd& ad);  // insert or replace by ad.id
void Withdraw(uint64_t session_id);

// XNetQosListen state for a session we host. `data` == nullptr keeps the
// current data.
void SetQosListen(uint64_t session_id, std::optional<bool> enabled,
                  const std::vector<uint8_t>* data);
void ReleaseQosListen(uint64_t session_id);

// Blocks for `timeout_ms` collecting answers. With `session_id`, only that
// session is asked for (XSessionSearchByID).
// ---- Online play (netplay/online.h) ----------------------------------------------
// Players reached through the online server have virtual addresses. The lobby
// talks to them through these callbacks (on the virtual network the lobby
// always uses kDefaultPort).
struct VirtualNet {
  bool (*is_virtual)(uint32_t ip) = nullptr;
  bool (*send)(uint32_t ip, uint16_t port, const void* data, size_t size) = nullptr;
  std::vector<uint32_t> (*peers)() = nullptr;  // searched in addition to the LAN
};
void SetVirtualNet(const VirtualNet& net);
// A lobby datagram that arrived over the virtual network.
void Inject(const uint8_t* data, size_t size, const udp::Endpoint& from);

// Called with the session we host when it is advertised, nullptr when we no
// longer advertise any (for online rooms).
using HostObserver = void (*)(const SessionAd* ad);
void SetHostObserver(HostObserver fn);

// Hosts with another build are left out (see LastMismatches()).
std::vector<Found> Search(int timeout_ms, std::optional<uint64_t> session_id = std::nullopt);
std::vector<Mismatch> LastMismatches();

// Probes the host at `ip` for its QoS data of `session_id`.
QosResult Qos(uint32_t ip, uint64_t session_id, int timeout_ms);

}  // namespace harmony::netplay::lobby
