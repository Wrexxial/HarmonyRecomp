// harmonyrecomp - short messages for the player.

#include "notice.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>

namespace harmony::notice {
namespace {

constexpr double kShowSeconds = 8.0;
constexpr size_t kMaxShown = 4;

using Clock = std::chrono::steady_clock;

struct Message {
  std::string text;
  Clock::time_point shown_at;
  double seconds = kShowSeconds;
};

class NoticeDialog;

std::mutex g_mutex;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_app_context = nullptr;
NoticeDialog* g_dialog = nullptr;  // UI thread only
std::deque<Message> g_messages;    // guarded by g_mutex

// Display-only window at the bottom centre. Closes (and deletes) itself once
// every message has expired.
class NoticeDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit NoticeDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool WantsMouse() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::deque<Message> messages;
    {
      std::lock_guard lock(g_mutex);
      const auto now = Clock::now();
      for (auto it = g_messages.begin(); it != g_messages.end();) {
        if (std::chrono::duration<double>(now - it->shown_at).count() > it->seconds) {
          it = g_messages.erase(it);
        } else {
          ++it;
        }
      }
      messages = g_messages;
    }
    if (messages.empty()) {
      Close();
      return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##harmony_notice", nullptr, kFlags)) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
      for (const auto& m : messages) {
        ImGui::TextUnformatted(m.text.c_str());
      }
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }

  void OnClose() override {
    if (g_dialog == this) {
      g_dialog = nullptr;
    }
  }
};

// UI thread: make sure the dialog exists while there are messages.
void ShowOnUIThread() {
  if (!g_drawer || g_dialog) {
    return;
  }
  g_dialog = new NoticeDialog(g_drawer);
}

}  // namespace

void Init(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context) {
  std::lock_guard lock(g_mutex);
  g_drawer = drawer;
  g_app_context = app_context;
}

void Shutdown() {
  {
    std::lock_guard lock(g_mutex);
    g_drawer = nullptr;
    g_app_context = nullptr;
    g_messages.clear();
  }
  delete g_dialog;
  g_dialog = nullptr;
}

void Post(std::string message, double seconds) {
  REXLOG_INFO("[notice] {}", message);
  rex::ui::WindowedAppContext* app_context = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (!g_drawer || !g_app_context) {
      return;
    }
    g_messages.push_back({std::move(message), Clock::now(), seconds});
    while (g_messages.size() > kMaxShown) {
      g_messages.pop_front();
    }
    app_context = g_app_context;
  }
  app_context->CallInUIThread(&ShowOnUIThread);
}

}  // namespace harmony::notice
