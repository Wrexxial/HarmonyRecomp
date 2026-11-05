// harmonyrecomp - Online tab of the F6 settings menu.

#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <sstream>
#include <string_view>

#include <imgui.h>
#include <rex/cvar.h>

#include "netplay/netplay.h"
#include "netplay/online.h"
#include "netplay/udp.h"
#include "settings_tabs.h"

namespace harmony {
namespace {

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
void Set(const char* name, std::string_view value) { rex::cvar::SetFlagByName(name, value); }
bool IsTrue(const std::string& v) { return v == "true" || v == "1"; }

std::string Trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}

void CopyTo(char* dst, size_t size, const std::string& src) {
  std::snprintf(dst, size, "%s", src.c_str());
}

void RestartHint(bool changed) {
  if (changed) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "restart");
  }
}

void Help(const char* text) {
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", text);
  }
}

bool ValidAddress(const std::string& text) {
  return netplay::udp::Parse(text, 1).has_value();
}

}  // namespace

struct OnlineTab::Async {
  std::mutex mutex;
  bool busy = false;
  std::string status;
  bool status_ok = true;
  bool have_list = false;
  std::vector<netplay::online::RoomInfo> rooms;
};

OnlineTab::OnlineTab()
    : start_enabled_(Get("netplay_enabled")),
      start_name_(Get("netplay_name")),
      start_ip_(Get("netplay_ip")),
      start_peers_(Get("netplay_peers")) {
  CopyTo(name_, sizeof(name_), start_name_);
  start_server_ = Get("netplay_server");
  CopyTo(server_, sizeof(server_), start_server_);
  async_ = std::make_shared<Async>();
  CopyTo(ip_, sizeof(ip_), start_ip_);
  LoadPeers();
  for (const auto& itf : netplay::udp::ListInterfaces()) {
    adapters_.push_back({itf.name, netplay::udp::ToString(itf.ip)});
  }
}

void OnlineTab::LoadPeers() {
  peers_.clear();
  std::stringstream list(Get("netplay_peers"));
  std::string item;
  while (std::getline(list, item, ',')) {
    item = Trim(item);
    if (!item.empty()) {
      peers_.push_back(item);
    }
  }
}

void OnlineTab::StorePeers() {
  std::string joined;
  for (const auto& peer : peers_) {
    if (!joined.empty()) {
      joined += ",";
    }
    joined += peer;
  }
  Set("netplay_peers", joined);
}

void OnlineTab::Draw() {
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);

  // Current session.
  if (netplay::Enabled() && IsTrue(start_enabled_)) {
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Online co-op is on");
    ImGui::Text("Playing as %s, others reach you at %s", netplay::Gamertag().c_str(),
                netplay::udp::ToString(netplay::LocalIp()).c_str());
  } else {
    ImGui::TextDisabled("Online co-op is off in this session");
  }
  DrawRooms();
  ImGui::Separator();
  ImGui::TextDisabled("Settings below apply after a restart");

  bool enabled = IsTrue(Get("netplay_enabled"));
  if (ImGui::Checkbox("Online co-op", &enabled)) {
    Set("netplay_enabled", enabled ? "true" : "false");
  }
  Help("Signs you in to an emulated Xbox Live so the game's online menus work.\n"
       "Players find each other on the same network or a virtual LAN\n"
       "(Radmin VPN, ZeroTier, Tailscale, ...).");
  RestartHint(IsTrue(Get("netplay_enabled")) != IsTrue(start_enabled_));

  if (ImGui::InputTextWithHint("Player name", "profile name", name_, 16)) {
    Set("netplay_name", Trim(name_));
  }
  Help("Gamertag the other players see (up to 15 characters).\nEmpty: your profile's name.");
  RestartHint(Get("netplay_name") != start_name_);

  if (ImGui::InputTextWithHint("Online server", "none: LAN/VPN only", server_, sizeof(server_))) {
    Set("netplay_server", Trim(server_));
  }
  Help("HarmonyRecomp online server (host or host:port). With it you play\n"
       "without a VPN: room codes, public rooms, direct connections or a relay.");
  RestartHint(Get("netplay_server") != start_server_);
  bool public_rooms = IsTrue(Get("netplay_public_rooms"));
  if (ImGui::Checkbox("Public rooms", &public_rooms)) {
    Set("netplay_public_rooms", public_rooms ? "true" : "false");
  }
  Help("On: rooms you host are listed for everyone with the same game version,\n"
       "and the game's search also finds other players' public rooms.\n"
       "Off: only players with your room code can join.");

  ImGui::Spacing();
  ImGui::TextDisabled("LAN / VPN");

  // Address to advertise.
  if (ImGui::InputTextWithHint("Your address", "automatic", ip_, sizeof(ip_))) {
    Set("netplay_ip", Trim(ip_));
  }
  Help("The IPv4 address other players reach you on, e.g. your Radmin VPN,\n"
       "ZeroTier or Tailscale address. Empty: picked automatically.");
  const std::string ip = Trim(ip_);
  if (!ip.empty() && !ValidAddress(ip)) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "invalid");
  }
  RestartHint(Get("netplay_ip") != start_ip_);
  if (!adapters_.empty() && ImGui::TreeNode("Network adapters on this PC")) {
    if (ImGui::SmallButton("Automatic")) {
      ip_[0] = '\0';
      Set("netplay_ip", "");
    }
    for (const auto& adapter : adapters_) {
      ImGui::PushID(adapter.ip.c_str());
      if (ImGui::SmallButton("Use")) {
        CopyTo(ip_, sizeof(ip_), adapter.ip);
        Set("netplay_ip", adapter.ip);
      }
      ImGui::SameLine();
      ImGui::Text("%s  %s", adapter.ip.c_str(), adapter.name.c_str());
      ImGui::PopID();
    }
    ImGui::TreePop();
  }

  // Other players to ask directly.
  ImGui::Spacing();
  ImGui::Text("Other players' addresses");
  Help("Only needed when lobby searches do not find the others on their own\n"
       "(broadcasts blocked, e.g. Tailscale). One address per player,\n"
       "optionally with :port.");
  RestartHint(Get("netplay_peers") != start_peers_);
  int remove = -1;
  for (size_t i = 0; i < peers_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("Remove")) {
      remove = static_cast<int>(i);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(peers_[i].c_str());
    if (!ValidAddress(peers_[i])) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "invalid");
    }
    ImGui::PopID();
  }
  if (remove >= 0) {
    peers_.erase(peers_.begin() + remove);
    StorePeers();
  }
  const bool submitted = ImGui::InputTextWithHint("##new_peer", "e.g. 26.12.34.56", new_peer_,
                                                  sizeof(new_peer_),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const std::string candidate = Trim(new_peer_);
  ImGui::BeginDisabled(candidate.empty() || !ValidAddress(candidate));
  if ((ImGui::Button("Add") || submitted) && !candidate.empty() && ValidAddress(candidate)) {
    peers_.push_back(candidate);
    StorePeers();
    new_peer_[0] = '\0';
  }
  ImGui::EndDisabled();

  ImGui::PopTextWrapPos();
}

void OnlineTab::DrawRooms() {
  namespace online = netplay::online;
  if (!online::Connected()) {
    if (!Get("netplay_server").empty() && netplay::Enabled()) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Not connected to the online server");
    }
    return;
  }
  ImGui::Spacing();
  ImGui::Text("Online server: connected");

  // Room we host.
  const std::string code = online::RoomCode();
  if (!code.empty()) {
    ImGui::Text("Your room code:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", code.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(code.c_str());
  } else {
    ImGui::TextDisabled("Host a game in the online menu to get a room code.");
  }

  // Join by code / public rooms (requests in the background).
  auto async = async_;
  bool busy;
  {
    std::lock_guard lock(async->mutex);
    busy = async->busy;
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
  const bool submitted = ImGui::InputTextWithHint("##code", "room code", join_code_,
                                                  sizeof(join_code_),
                                                  ImGuiInputTextFlags_CharsUppercase |
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const std::string wanted = Trim(join_code_);
  ImGui::BeginDisabled(busy || wanted.empty());
  const bool join = ImGui::Button("Join a room") || (submitted && !wanted.empty() && !busy);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(busy);
  const bool refresh = ImGui::Button("List public rooms");
  ImGui::EndDisabled();

  auto run = [async](std::function<void()> work) {
    {
      std::lock_guard lock(async->mutex);
      async->busy = true;
    }
    std::thread([async, work = std::move(work)] {
      work();
      std::lock_guard lock(async->mutex);
      async->busy = false;
    }).detach();
  };
  auto join_room = [async](std::string room) {
    std::string message;
    const bool ok = online::JoinRoom(room, &message, 2000);
    std::lock_guard lock(async->mutex);
    async->status_ok = ok;
    async->status = ok ? "Connected to " + message + "'s room " + room +
                             ". Now search for a game in the game's online menu."
                       : "Could not join " + room + ": " + message;
  };
  if (join) run([join_room, wanted] { join_room(wanted); });
  if (refresh) {
    run([async] {
      auto rooms = online::ListRooms(2000);
      std::lock_guard lock(async->mutex);
      async->rooms = std::move(rooms);
      async->have_list = true;
      async->status_ok = true;
      async->status = async->rooms.empty() ? "No public rooms open right now." : "";
    });
  }

  std::vector<online::RoomInfo> rooms;
  std::string status;
  bool status_ok;
  bool have_list;
  {
    std::lock_guard lock(async->mutex);
    rooms = async->rooms;
    status = async->status;
    status_ok = async->status_ok;
    have_list = async->have_list;
  }
  if (busy) ImGui::TextDisabled("asking the server...");
  if (!status.empty()) {
    if (status_ok) {
      ImGui::TextWrapped("%s", status.c_str());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status.c_str());
    }
  }
  if (have_list && !rooms.empty()) {
    for (const auto& room : rooms) {
      ImGui::PushID(room.code.c_str());
      ImGui::BeginDisabled(busy);
      if (ImGui::SmallButton("Join")) {
        std::string c = room.code;
        run([join_room, c] { join_room(c); });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Text("%s  (%d/%d)  %s", room.name.c_str(), room.players, room.max_players,
                  room.code.c_str());
      ImGui::PopID();
    }
  }

  // Connected players.
  auto peers = online::Peers();
  if (!peers.empty()) {
    ImGui::TextDisabled("Players connected online:");
    for (const auto& p : peers) {
      const char* path = !p.reachable ? "not responding" : p.direct ? "direct" : "via relay";
      if (p.reachable && p.ping_ms >= 0) {
        // Green below 80 ms, yellow below 160 ms, red above.
        const ImVec4 colour = p.ping_ms < 80    ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                              : p.ping_ms < 160 ? ImVec4(0.95f, 0.85f, 0.3f, 1.0f)
                                                : ImVec4(0.95f, 0.4f, 0.35f, 1.0f);
        ImGui::Bullet();
        ImGui::Text("%s  %s", p.name.c_str(), path);
        ImGui::SameLine();
        ImGui::TextColored(colour, "%d ms", p.ping_ms);
      } else {
        ImGui::BulletText("%s  %s", p.name.c_str(), path);
      }
    }
  }
}

}  // namespace harmony
