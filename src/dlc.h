// harmonyrecomp - DLC packages the player provides (see README, "DLC").
#pragma once

#include <string>
#include <vector>

namespace rex {
class Runtime;
}

namespace harmony {

// Installs the DLC packages found in the dlc/ folder into the runtime's content
// folder, where the game's content enumeration finds them. Already installed
// packages are skipped. Call once the game executable is loaded (its title ID
// decides the content folder), before the game's main thread runs.
void InstallDlcPackages(rex::Runtime* runtime);

// DLC this build was made with (dlc_packages.txt next to the exe), as the
// game's package numbers ("package15"): the same for the English and the
// Japanese package of a DLC. Older builds: the package file names.
std::vector<std::string> BuiltDlcPackages();

// Readable name of a DLC id from BuiltDlcPackages() ("Simon (8-bit)").
std::string DlcName(const std::string& id);

}  // namespace harmony
