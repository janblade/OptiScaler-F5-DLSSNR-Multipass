#pragma once

#include <string>

// Which XeFG and XeLL files are in use, for the log and the menu: the path and where it comes from (OptiScaler's
// folder, the game's, the override in the ini), the file version, whether its signature checks out, its SHA-256, and a
// second copy of the game's own when one is loaded too. Worked out once on a background thread (hashing and the
// signature check read the whole file); until then the menu lines are empty.
namespace XeModuleInfo
{
// Starts the background work once XeFGProxy and XeLLProxy have loaded their copies; later calls do nothing
void Describe();

// One short line for the menu, empty while it is being worked out or when that module is not loaded
std::string XeFGLine();
std::string XeLLLine();
} // namespace XeModuleInfo
