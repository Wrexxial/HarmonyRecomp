// harmonyrecomp - save data: where it is and a one-click backup (F6 > Saves).
#pragma once

#include <filesystem>
#include <string>

namespace harmony::saves {

// Called once at startup with the runtime's user data folder
// (Documents\harmonyrecomp on Windows).
void SetUserDataRoot(const std::filesystem::path& root);

// Folder with the game's save data (may not exist before the first save).
std::filesystem::path SaveFolder();
// Backups go to <user data>/save-backups/<date time>/.
std::filesystem::path BackupFolder();

// Copies the save folder to a new dated backup folder. Returns a message for
// the menu ("backed up 3 files to ...", or what went wrong).
std::string BackupNow();

// Number of backups made so far.
int BackupCount();

// Opens a folder in the file manager (Explorer, or xdg-open on Linux).
void OpenFolder(const std::filesystem::path& folder);

}  // namespace harmony::saves
