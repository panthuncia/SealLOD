#pragma once

#include "Hair/GroomSource.h"

#include <cstdint>
#include <span>

namespace br::hair {

enum class ProceduralHairStyle {
    Straight,
    Wavy,  // planar sine around the growth direction
    Curly, // helix around the growth direction
};

// Test-fixture grooms. Lengths are in the caller's units; the defaults are metres on a head-sized sphere.
// Strand i depends only on (seed, i), so the same seed with a larger strand count extends the same groom.
struct ProceduralGroomSettings {
    std::uint32_t strandCount = 1000u;
    std::uint32_t pointsPerStrand = 17u; // at least 2
    ProceduralHairStyle style = ProceduralHairStyle::Straight;
    std::uint32_t seed = 1u;

    float strandLength = 0.25f;
    float lengthVariation = 0.2f; // each strand is shortened by up to this fraction
    float rootWidth = 0.00008f;
    float tipWidth = 0.00002f;
    float waveAmplitude = 0.01f; // wave height or curl radius
    float waveCount = 4.0f;      // waves or curl turns along the strand

    // Gravity droop. 0 keeps the strands straight along the surface normal. Above 0 the growth direction turns toward
    // droopDirection along the strand, by about `droop` radians' worth of pull over its length: around 3, a strand that
    // starts sideways ends up hanging. A drooping strand stays outside the sphere (GenerateGroomOnSphereCap) or on the
    // outer side of its root's face (GenerateGroomOnTriangles).
    float droop = 0.0f;
    Float3 droopDirection{ 0.0f, -1.0f, 0.0f };

    // Sphere cap (GenerateGroomOnSphereCap only): roots are uniform on the part of the sphere within capAngle of
    // capAxis. The default is the upper hemisphere.
    Float3 capCenter{ 0.0f, 0.0f, 0.0f };
    Float3 capAxis{ 0.0f, 1.0f, 0.0f };
    float capRadius = 0.1f;
    float capAngleRadians = 1.57079632679f;
};

// Strands grow along the sphere normal at their root, and droop from there when settings.droop is set.
GroomSource GenerateGroomOnSphereCap(const ProceduralGroomSettings& settings);

// Roots are scattered uniformly by area over a triangle list and strands grow along the face normal
// (counter-clockwise winding). Returns an empty groom when the indices are not a valid triangle list over
// `positions` or the mesh has no area.
GroomSource GenerateGroomOnTriangles(const ProceduralGroomSettings& settings,
                                     std::span<const Float3> positions,
                                     std::span<const std::uint32_t> indices);

} // namespace br::hair
