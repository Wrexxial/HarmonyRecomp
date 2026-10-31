// harmonyrecomp - short messages for the player, shown at the bottom of the
// screen for a few seconds (e.g. why an online lobby was left out).
//
// Post() may be called from any thread. Nothing is shown before Init() or
// after Shutdown().

#pragma once

#include <string>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace harmony::notice {

void Init(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context);
void Shutdown();  // on the UI thread, before the drawer goes away
void Post(std::string message);

}  // namespace harmony::notice
