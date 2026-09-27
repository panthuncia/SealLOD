#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "VirtualGeometry/Reyes/Tessellation/ReyesTessellationPatternUtils.h"

enum class ReyesArrangementLineFamily : uint32_t
{
    ConstantW = 0u,
    ConstantU = 1u,
    ConstantV = 2u,
};

struct ReyesExactBarycentricPoint
{
    uint32_t wNumerator = 0u;
    uint32_t uNumerator = 0u;
    uint32_t vNumerator = 0u;
    uint32_t denominator = 1u;
};

struct ReyesPatternArrangementLine
{
    ReyesArrangementLineFamily family = ReyesArrangementLineFamily::ConstantW;
    uint32_t numerator = 0u;
    bool isBoundary = false;
    std::vector<uint32_t> vertexIndices;
};

struct ReyesPatternArrangementEdge
{
    uint32_t vertexIndex0 = 0u;
    uint32_t vertexIndex1 = 0u;
    ReyesArrangementLineFamily family = ReyesArrangementLineFamily::ConstantW;
    uint32_t lineIndex = 0u;
};

struct ReyesPatternArrangementFace
{
    std::vector<uint32_t> vertexIndices;
    double signedArea2 = 0.0;
    bool preferSteinerFan = false;
    bool preferBoundaryStartEar = false;
};

struct ReyesPatternArrangementTriangle
{
    uint32_t vertexIndex0 = 0u;
    uint32_t vertexIndex1 = 0u;
    uint32_t vertexIndex2 = 0u;
    uint32_t sourceFaceIndex = 0u;
};

struct ReyesPatternBoundaryArrangement
{
    ReyesCanonicalPatternKey key{};
    uint32_t commonDenominator = 1u;
    std::vector<ReyesExactBarycentricPoint> vertices;
    std::array<std::vector<uint32_t>, 3> boundaryVertexIndices;
    std::array<std::vector<uint32_t>, 3> exactLineIndices;
    std::vector<uint32_t> interiorVertexIndices;
    std::vector<ReyesPatternArrangementLine> lines;
    std::vector<ReyesPatternArrangementEdge> edges;
    std::vector<ReyesPatternArrangementFace> faces;
    std::vector<ReyesPatternArrangementTriangle> triangles;
};

ReyesPatternBoundaryArrangement BuildCanonicalReyesBoundaryArrangement(const ReyesCanonicalPatternKey& key);