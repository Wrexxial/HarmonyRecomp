// harmonyrecomp - Online tab of the F6 settings menu.

#include <cstdio>
#include <sstream>
#include <string_view>

#include <imgui.h>
#include <rex/cvar.h>

#include "netplay/netplay.h"
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

OnlineTab::OnlineTab()
    : start_enabled_(Get("netplay_enabled")),
      start_name_(Get("netplay_name")),
      start_ip_(Get("netplay_ip")),
      start_peers_(Get("netplay_peers")) {
  CopyTo(name_, sizeof(name_), start_name_);
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
  ImGui::TextDisabled("Changes here apply after a restart");
  ImGui::Separator();

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

}  // namespace harmony
