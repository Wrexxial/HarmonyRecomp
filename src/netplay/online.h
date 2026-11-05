// harmonyrecomp netplay - online play through the HarmonyRecomp server.
//
// Without a VPN, players cannot reach each other's real addresses. Instead the
// server gives every player a virtual IPv4 address (10.66.x.y) and introduces
// players who join the same room. The game only ever sees virtual addresses;
// this module carries their UDP traffic over one socket:
//   * directly between the PCs when UDP hole punching works (most routers),
//   * otherwise relayed through the server.
// Rooms have a short code (e.g. K7Q2XM) to join with; public rooms are listed
// for everyone with the same game version.
//
// No SDK headers here, so it can be tested natively (see server/README.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "udp.h"

namespace harmony::netplay::online {

constexpr uint16_t kDefaultServerPort = 47660;

struct Config {
  std::string server;   // host name or IP, optionally ":port"
  uint16_t local_port = 0;  // our UDP port (0 = any)
  std::string build;    // game version, must match the room's
  std::string name;     // shown to other players
  bool force_relay = false;  // testing: never connect directly
};

using LogFn = void (*)(const std::string& message);
void SetLogger(LogFn fn);

// Connects to the server (waits up to ~2 s for its answer).
bool Start(const Config& config, std::string* error);
void Stop();
bool Connected();
uint32_t VirtualIp();  // 0 when not connected

// 10.66.0.0/16: addresses handed out by the server.
bool IsVirtual(uint32_t ip);

// UDP over the virtual network. Handlers run on the module's thread.
using Handler = std::function<void(uint32_t src_vip, uint16_t src_port, const uint8_t* data,
                                   size_t size)>;
void Listen(uint16_t port, Handler handler);
void Unlisten(uint16_t port);
bool Send(uint32_t dst_vip, uint16_t src_port, uint16_t dst_port, const void* data, size_t size);

// ---- Rooms ----------------------------------------------------------------------

// Opens (or updates) the room for the session we host; the code arrives later.
void OpenRoom(bool is_public, const std::string& name, int players, int max_players);
void CloseRoom();
std::string RoomCode();  // empty while no room is open
// Called (on the module's thread) when the server hands out a room code.
void SetRoomCodeListener(LogFn fn);

struct RoomInfo {
  std::string code;
  std::string name;
  int players = 0;
  int max_players = 0;
};
// Public rooms with our game version (blocks up to timeout_ms).
std::vector<RoomInfo> ListRooms(int timeout_ms);

// Connects us with everyone in the room (blocks up to timeout_ms for the
// server's answer; the direct connections follow in the background).
bool JoinRoom(const std::string& code, std::string* message, int timeout_ms);
std::string JoinedRoom();  // code of the last room joined, empty if none

struct PeerInfo {
  uint32_t vip = 0;
  std::string name;
  bool direct = false;   // false: relayed through the server (or still trying)
  bool reachable = false;
  int ping_ms = -1;      // round trip, -1 while unknown (or an older version)
};
std::vector<PeerInfo> Peers();

}  // namespace harmony::netplay::online
