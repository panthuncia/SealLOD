#pragma once

#include "Hair/HairRuntime.h"

#include <BasicRenderer/Diagnostics/ControlCommandRegistry.h>

#include <memory>

namespace br::hair {

// Registers the hair control commands, for the control pipe and C# scenarios (docs/hair.md lists their fields):
// hair.load, hair.attach, hair.clear, hair.selfcheck and hair.stats.
void RegisterHairCommands(br::diagnostics::ControlCommandRegistry& commands, std::shared_ptr<HairRuntime> runtime);

} // namespace br::hair
