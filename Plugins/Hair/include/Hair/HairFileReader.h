#pragma once

#include "Hair/GroomSource.h"

#include <filesystem>
#include <string>

namespace br::hair {

// Reads a Cem Yuksel .hair file (https://www.cemyuksel.com/research/hairmodels/) into `groom`.
// Positions and thickness are kept in the file's units and axes. Per-point colour is kept when the file has it; the
// header's default colour and all transparency are dropped. Returns false and sets `error` (when given) for files
// that cannot be opened, are truncated, or are malformed; `groom` is left empty in that case.
bool LoadHairFile(const std::filesystem::path& path, GroomSource& groom, std::string* error = nullptr);

} // namespace br::hair
