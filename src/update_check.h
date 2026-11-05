// harmonyrecomp - "a new version is available" check.
//
// A few seconds after the start, asks GitHub for the newest release once and,
// if it is newer than this build, shows a notice at the bottom of the screen
// and a line in the F6 menu. Online play needs everyone on the same version,
// so players should learn about updates without visiting GitHub.
// Own builds ("dev") and check_for_updates = false skip the check.

#pragma once

#include <string>

namespace harmony {

// Release of this build ("v0.5.0", "dev", ...).
const char* BuildVersion();

void StartUpdateCheck();

// How to update on this system ("Run the new setup from github.com/...").
std::string UpdateHowTo();

// Newer release found by the check, empty if none (yet).
std::string AvailableUpdate();

}  // namespace harmony
