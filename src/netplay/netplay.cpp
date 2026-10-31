// harmonyrecomp netplay - settings, identity and startup.

#include "netplay.h"

#include "gamesock.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "../notice.h"
#include "lobby.h"
#include "online.h"
#include "udp.h"

REXCVAR_DEFINE_BOOL(netplay_enabled, false, "Netplay",
                    "Online co-op over LAN/VPN: pretend to be signed in to Xbox Live and find "
                    "other players' lobbies on the local network");
REXCVAR_DEFINE_STRING(netplay_name, "", "Netplay",
                      "Gamertag shown to other players (max 15 characters; empty = profile name)");
REXCVAR_DEFINE_STRING(netplay_ip, "", "Netplay",
                      "IPv4 address to advertise, e.g. your Radmin VPN/ZeroTier/Tailscale address "
                      "(empty = pick automatically)");
REXCVAR_DEFINE_STRING(netplay_peers, "", "Netplay",
                      "Comma separated addresses (ip or ip:port) of other players to ask for "
                      "lobbies; needed when broadcasts do not reach them (e.g. Tailscale)");
REXCVAR_DEFINE_UINT32(netplay_port, 47658, "Netplay", "UDP port of the lobby service");
REXCVAR_DEFINE_UINT32(netplay_remote_port, 47658, "Netplay",
                      "UDP lobby port of the other players (only differs when testing two "
                      "instances on one PC)");
REXCVAR_DEFINE_UINT32(netplay_search_ms, 1000, "Netplay",
                      "How long a lobby search waits for answers, in milliseconds");
// The HarmonyRecomp community server (server/hrnp_server.py on a VPS).
REXCVAR_DEFINE_STRING(netplay_server, "87.106.10.23", "Netplay",
                      "Online server (host or host:port) for room codes and play without a VPN; "
                      "empty = LAN/VPN only");
REXCVAR_DEFINE_BOOL(netplay_public_rooms, true, "Netplay",
                    "Online: list the rooms you host publicly, and find public rooms in the "
                    "game's search");

namespace harmony::netplay {
namespace {

constexpr uint32_t kTitleId = 0x58410A7A;

std::mutex g_mutex;
std::atomic<uint32_t> g_local_ip{0x7F000001};
bool g_ip_pinned = false;
bool g_started = false;
std::string g_build;
std::vector<std::string> g_dlc;
uint64_t g_xuid = 0;

uint64_t Fnv1a64(const std::string& text) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : text) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

uint32_t Mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool InRange(uint32_t ip, uint32_t net, int prefix) {
  uint32_t mask = prefix == 0 ? 0 : ~((1u << (32 - prefix)) - 1);
  return (ip & mask) == (net & mask);
}

// Virtual LAN adapters are preferred: with a LAN and a VPN address, the VPN
// one is how internet friends reach us.
bool LooksLikeVirtualLan(const udp::Interface& itf) {
  std::string name = Lower(itf.name);
  for (const char* key : {"radmin", "zerotier", "tailscale", "hamachi", "wireguard"}) {
    if (name.find(key) != std::string::npos) return true;
  }
  return InRange(itf.ip, 0x1A000000, 8) ||   // 26.0.0.0/8 Radmin VPN
         InRange(itf.ip, 0x19000000, 8) ||   // 25.0.0.0/8 Hamachi
         InRange(itf.ip, 0x64400000, 10);    // 100.64.0.0/10 Tailscale
}

std::vector<udp::Endpoint> ParsePeers() {
  std::vector<udp::Endpoint> peers;
  std::stringstream list(REXCVAR_GET(netplay_peers));
  std::string item;
  while (std::getline(list, item, ',')) {
    if (item.find_first_not_of(" \t") == std::string::npos) continue;
    if (auto ep = udp::Parse(item, static_cast<uint16_t>(REXCVAR_GET(netplay_remote_port)))) {
      peers.push_back(*ep);
    } else {
      NETPLAY_WARN("ignoring invalid netplay_peers entry '{}'", item);
    }
  }
  return peers;
}

uint32_t ChooseLocalIp(const std::vector<udp::Endpoint>& peers) {
  std::string pinned = REXCVAR_GET(netplay_ip);
  if (!pinned.empty()) {
    if (auto ep = udp::Parse(pinned, 0)) {
      g_ip_pinned = true;
      NETPLAY_LOG("using netplay_ip {}", udp::ToString(ep->ip));
      return ep->ip;
    }
    NETPLAY_WARN("netplay_ip '{}' is not an IPv4 address, choosing automatically", pinned);
  }
  auto interfaces = udp::ListInterfaces();
  for (const auto& itf : interfaces) {
    NETPLAY_LOG("network adapter '{}' {}", itf.name, udp::ToString(itf.ip));
  }
  if (!peers.empty()) {
    if (auto ip = udp::RouteSourceIp(peers.front().ip)) {
      NETPLAY_LOG("using {} (route to first peer {})", udp::ToString(*ip),
                  udp::ToString(peers.front().ip));
      return *ip;
    }
  }
  for (const auto& itf : interfaces) {
    if (LooksLikeVirtualLan(itf)) {
      NETPLAY_LOG("using {} (virtual LAN adapter '{}')", udp::ToString(itf.ip), itf.name);
      return itf.ip;
    }
  }
  if (auto ip = udp::RouteSourceIp(0x08080808)) {
    NETPLAY_LOG("using {} (default route)", udp::ToString(*ip));
    return *ip;
  }
  if (!interfaces.empty()) return interfaces.front().ip;
  NETPLAY_WARN("no network adapter found, using 127.0.0.1");
  return 0x7F000001;
}

void LobbyLog(const std::string& message) { NETPLAY_LOG("{}", message); }
void LobbyNotice(const std::string& message) { harmony::notice::Post(message); }

// ---- Online play -------------------------------------------------------------------

bool VirtualSend(uint32_t ip, uint16_t port, const void* data, size_t size) {
  return online::Send(ip, lobby::kDefaultPort, port, data, size);
}

std::vector<uint32_t> VirtualPeers() {
  std::vector<uint32_t> vips;
  for (const auto& p : online::Peers()) vips.push_back(p.vip);
  return vips;
}

void OnHostAd(const lobby::SessionAd* ad) {
  if (!online::Connected()) return;
  if (!ad) {
    online::CloseRoom();
    return;
  }
  online::OpenRoom(REXCVAR_GET(netplay_public_rooms), ad->host_name,
                   static_cast<int>(ad->filled_public + ad->filled_private),
                   static_cast<int>(ad->max_public + ad->max_private));
}

void OnRoomCode(const std::string& code) {
  harmony::notice::Post("Online room code: " + code +
                        " (friends enter it in F6 > Online > Join a room)");
}

void StartOnline() {
  const std::string server = REXCVAR_GET(netplay_server);
  if (server.empty()) return;
  online::SetLogger(&LobbyLog);
  online::SetRoomCodeListener(&OnRoomCode);
  online::Config config;
  config.server = server;
  config.build = g_build;
  config.name = Gamertag();
  std::string error;
  if (!online::Start(config, &error)) {
    NETPLAY_WARN("online server {}: {}. Only LAN/VPN play is available.", server, error);
    harmony::notice::Post("Online server not reachable (" + error + "). LAN/VPN play only.");
    return;
  }
  // Other players see us at our virtual address from now on.
  g_local_ip = online::VirtualIp();
  g_ip_pinned = true;
  lobby::VirtualNet net;
  net.is_virtual = &online::IsVirtual;
  net.send = &VirtualSend;
  net.peers = &VirtualPeers;
  lobby::SetVirtualNet(net);
  lobby::SetHostObserver(&OnHostAd);
  online::Listen(lobby::kDefaultPort, [](uint32_t vip, uint16_t port, const uint8_t* data,
                                         size_t size) { lobby::Inject(data, size, {vip, port}); });
  NETPLAY_LOG("online via {}: our address {}", server, udp::ToString(g_local_ip.load()));
}

}  // namespace

bool Enabled() { return REXCVAR_GET(netplay_enabled); }

void SetBuildInfo(std::string build, std::vector<std::string> dlc) {
  std::lock_guard lock(g_mutex);
  g_build = std::move(build);
  g_dlc = std::move(dlc);
}

void Startup() {
  std::lock_guard lock(g_mutex);
  if (g_started || !Enabled()) return;
  g_started = true;

  auto peers = ParsePeers();
  g_local_ip = ChooseLocalIp(peers);

  std::string host = udp::Hostname();
  g_xuid = 0x0009000000000000ull | (Fnv1a64(Gamertag() + "|" + host) & 0x0000FFFFFFFFFFFFull);
  NETPLAY_LOG("signed in to (emulated) Live as '{}', online XUID {:016X}", Gamertag(), g_xuid);

  lobby::SetLogger(&LobbyLog);
  lobby::SetNoticeSink(&LobbyNotice);
  lobby::Config config;
  config.title_id = kTitleId;
  config.port = static_cast<uint16_t>(REXCVAR_GET(netplay_port));
  config.remote_port = static_cast<uint16_t>(REXCVAR_GET(netplay_remote_port));
  config.peers = peers;
  config.build = g_build;
  config.dlc = g_dlc;
  NETPLAY_LOG("game version {}, {} DLC package(s)", g_build, g_dlc.size());
  std::string error;
  if (!lobby::Start(config, &error)) {
    NETPLAY_WARN("lobby service could not start: {}. Hosting still works for players who "
                 "join by search from elsewhere only if this is fixed.",
                 error);
  }
  for (const auto& p : peers) NETPLAY_LOG("peer {}", udp::ToString(p));
  StartOnline();
}

void PrepareSearch() {
  if (!online::Connected() || !REXCVAR_GET(netplay_public_rooms)) return;
  // Connect with the hosts of public rooms, so the search below reaches them
  // (through the relay at first, directly once the connection is up).
  auto rooms = online::ListRooms(700);
  size_t joined = 0;
  for (const auto& room : rooms) {
    if (joined >= 6) break;
    std::string message;
    if (online::JoinRoom(room.code, &message, 700)) ++joined;
  }
  if (!rooms.empty()) NETPLAY_LOG("online: {} public room(s), connected to {}", rooms.size(), joined);
}

void Shutdown() {
  online::Stop();
  gamesock::Shutdown();
  std::lock_guard lock(g_mutex);
  if (!g_started) return;
  lobby::Stop();
  g_started = false;
}

uint32_t LocalIp() { return g_local_ip; }

void NoteRemoteHost(uint32_t ip) {
  if (g_ip_pinned) return;
  auto route = udp::RouteSourceIp(ip);
  if (route && *route != g_local_ip) {
    NETPLAY_LOG("switching advertised address {} -> {} (reaches host {})",
                udp::ToString(g_local_ip), udp::ToString(*route), udp::ToString(ip));
    g_local_ip = *route;
  }
}

uint64_t OnlineXuid() {
  if (!g_xuid) {
    // Before Startup (or with netplay off): stable fallback, never zero.
    return 0x0009000000000000ull | (Fnv1a64(Gamertag()) & 0x0000FFFFFFFFFFFFull);
  }
  return g_xuid;
}

std::string Gamertag() {
  std::string name = REXCVAR_GET(netplay_name);
  if (name.empty()) {
    if (auto* ks = REX_KERNEL_STATE()) {
      if (const auto& profile = ks->user_profile()) name = profile->name();
    }
  }
  if (name.empty()) name = "Player";
  if (name.size() > 15) name.resize(15);
  return name;
}

uint64_t MachineId(uint32_t ip) { return 0xFA00000000000000ull | (uint64_t(Mix32(ip)) << 16) | (ip & 0xFFFF); }

void FillXnAddr(uint32_t ip, guest::XNADDR* out) {
  std::memset(out, 0, sizeof(*out));
  out->ina = ip;
  out->ina_online = ip;
  out->port_online = 3074;
  uint32_t h = Mix32(ip ^ 0x48524E50);
  // Locally administered unicast MAC, never zero (RakNet-style code relies on it).
  out->ab_enet[0] = 0x02;
  out->ab_enet[1] = static_cast<uint8_t>(h >> 24);
  out->ab_enet[2] = static_cast<uint8_t>(h >> 16);
  out->ab_enet[3] = static_cast<uint8_t>(ip >> 16);
  out->ab_enet[4] = static_cast<uint8_t>(ip >> 8);
  out->ab_enet[5] = static_cast<uint8_t>(ip);
  out->ab_online.ina = ip;
  out->ab_online.security_parameter_index = h | 1;
  out->ab_online.xbox_id = MachineId(ip);
  out->ab_online.platform_type = 0;
}

uint32_t IpFromXnAddr(const guest::XNADDR& addr) {
  uint32_t ip = addr.ina;
  return ip ? ip : static_cast<uint32_t>(addr.ina_online);
}

uint32_t SearchTimeoutMs() { return std::clamp<uint32_t>(REXCVAR_GET(netplay_search_ms), 200, 10000); }

uint64_t ReadXnkid(const guest::XNKID& id) {
  uint64_t v = 0;
  for (uint8_t b : id.ab) v = (v << 8) | b;
  return v;
}

void WriteXnkid(uint64_t value, guest::XNKID* id) {
  for (int i = 7; i >= 0; --i) {
    id->ab[i] = static_cast<uint8_t>(value);
    value >>= 8;
  }
}

}  // namespace harmony::netplay
