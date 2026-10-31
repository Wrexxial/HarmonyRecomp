// harmonyrecomp netplay - settings, identity and startup.
//
// Netplay emulates just enough of Xbox Live for Harmony of Despair's online
// co-op: sign-in, XNet addressing, QoS and matchmaking sessions. Instead of a
// Live server, players find each other on the local network (or a virtual LAN
// such as Radmin VPN / ZeroTier / Tailscale) through the lobby service.
//
// Every player is identified by the IPv4 address others reach them on: the
// XNADDR a machine reports for itself and the one others derive from its IP
// are byte-identical, so XNADDR <-> IN_ADDR translation needs no server.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "guest.h"

#define NETPLAY_LOG(...) REXLOG_INFO("[netplay] {}", fmt::format(__VA_ARGS__))
#define NETPLAY_WARN(...) REXLOG_WARN("[netplay] {}", fmt::format(__VA_ARGS__))

namespace harmony::netplay {

// netplay_enabled in harmonyrecomp.toml. Off: the SDK's offline behaviour.
bool Enabled();

// Game version ("1.04" / "2.04") and DLC packages of this build, compared with
// other players in the lobby. Call before Startup().
void SetBuildInfo(std::string build, std::vector<std::string> dlc);

// Reads the settings, picks the local address and starts the lobby service.
// Called once the runtime is set up; does nothing when netplay is off.
void Startup();
void Shutdown();

// Before the game's lobby search: with the online server, connect to the hosts
// of public rooms so the search finds them.
void PrepareSearch();

// IPv4 (host byte order) this machine advertises to other players.
uint32_t LocalIp();
// After finding a host at `ip`: switch to the local address that reaches it,
// unless netplay_ip pins one (keeps a VPN address when a LAN one exists too).
void NoteRemoteHost(uint32_t ip);

// Online XUID / gamertag of the local player (user index 0).
uint64_t OnlineXuid();
std::string Gamertag();

// Machine id and XNADDR derived purely from an IP, so both sides agree.
uint64_t MachineId(uint32_t ip);
void FillXnAddr(uint32_t ip, guest::XNADDR* out);
uint32_t IpFromXnAddr(const guest::XNADDR& addr);

uint32_t SearchTimeoutMs();

uint64_t ReadXnkid(const guest::XNKID& id);
void WriteXnkid(uint64_t value, guest::XNKID* id);

}  // namespace harmony::netplay
