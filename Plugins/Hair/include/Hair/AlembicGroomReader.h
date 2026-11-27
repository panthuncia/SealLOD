#pragma once

#include "Hair/GroomSource.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace br::hair {

// Alembic files carry no up-axis metadata, so the caller says which axis the file was authored with.
enum class GroomUpAxis {
    Y, // the renderer's convention: positions are kept as they are
    Z, // rotated onto +Y up the way the USD importer corrects Z-up stages: (x, y, z) -> (x, z, -y)
};

struct AlembicGroomImportSettings {
    // File units to metres. The UE groom schema is in centimetres.
    float unitScale = 0.01f;
    GroomUpAxis upAxis = GroomUpAxis::Y;
};

struct AlembicGroomImportInfo {
    std::uint32_t curveObjectCount = 0u;   // ICurves objects found in the archive
    std::uint32_t skippedStrandCount = 0u; // curves with fewer than two points, left out of the groom
    int versionMajor = -1;                 // groom_version_major / groom_version_minor, -1 when the file has none
    int versionMinor = -1;
};

// Reads the curves of an Alembic (Ogawa) archive into `groom`, following the UE groom schema: every ICurves object
// is read at its first sample, moved by its parent transforms and appended, in archive traversal order.
//
// Positions and widths are converted to metres and to +Y up (see the settings). Widths come from groom_width, else
// the schema's own width property, else 1 cm. groom_guide, groom_group_id, groom_root_uv, groom_closest_guides and
// groom_guide_weights fill the optional GroomSource arrays; an array stays empty when no curves object authors its
// attribute, and curves objects without it get 0 (no guide, group 0, uv 0, no closest guides). An attribute may be
// authored constant, per curve or per point, on the curves object or on any parent transform.
//
// groom_closest_guides values are groom_id values when the file has groom_id, otherwise curve indices in archive
// order; either way they are turned into strand indices of `groom` (-1 for a guide that is not in it). They are kept
// only together with groom_guide_weights.
//
// Curves with fewer than two points are skipped and counted in `info`. Returns false and sets `error` (when given)
// for a missing or unreadable file, an archive without curves, an archive with no usable strand, or a malformed
// attribute; `groom` is left empty in that case.
bool LoadAlembicGroom(const std::filesystem::path& path,
                      const AlembicGroomImportSettings& settings,
                      GroomSource& groom,
                      std::string* error = nullptr,
                      AlembicGroomImportInfo* info = nullptr);

} // namespace br::hair
