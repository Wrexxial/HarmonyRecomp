// harmonyrecomp - Online and Controls tabs of the F6 settings menu.

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace harmony {

// Online co-op settings (netplay_* in harmonyrecomp.toml), read when the game
// starts, and the live online rooms (room code, join by code, public rooms).
class OnlineTab {
 public:
  OnlineTab();
  void Draw();

 private:
  void LoadPeers();
  void StorePeers();
  void DrawRooms();

  // Server requests run in the background; the result lands here.
  struct Async;
  std::shared_ptr<Async> async_;
  char join_code_[16] = {};
  std::string start_server_;
  char server_[128] = {};

  std::string start_enabled_;
  std::string start_name_;
  std::string start_ip_;
  std::string start_peers_;
  char name_[32] = {};
  char ip_[64] = {};
  char new_peer_[64] = {};
  std::vector<std::string> peers_;
  struct Adapter {
    std::string name;
    std::string ip;
  };
  std::vector<Adapter> adapters_;
};

// Keyboard/mouse bindings (keybind_*, mnk_*) and controller remapping
// (remap_*). Both apply immediately.
class ControlsTab {
 public:
  void Draw();

 private:
  void DrawKeyboard();
  void DrawController();
  // True if a key or button was bound this frame.
  bool PollCapture();

  // keybind_* setting waiting for a key press, empty if none.
  std::string capturing_;
  bool capture_armed_ = false;  // mouse released since the capture started
  std::chrono::steady_clock::time_point capture_start_;
};

}  // namespace harmony
