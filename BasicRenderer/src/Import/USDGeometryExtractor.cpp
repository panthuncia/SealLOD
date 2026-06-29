#include "Import/USDGeometryExtractor.h"

#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <optional>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec4f.h>

#include <DirectXMath.h>
#include <spdlog/spdlog.h>

#include "Import/CLodCacheLoader.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/ClusterLODTypes.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/DefaultCLodSettings.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTable.h"

using namespace pxr;

namespace {

constexpr size_t kMaxSkinInfluences = 8u;

// Interpolation

enum class InterpolationType {
	Constant,
	Uniform,
	Varying,
	Vertex,
	FaceVarying
};

static InterpolationType GetInterpolationType(const TfToken& tok) {
	if (tok == UsdGeomTokens->constant)    return InterpolationType::Constant;
	if (tok == UsdGeomTokens->uniform)     return InterpolationType::Uniform;
	if (tok == UsdGeomTokens->varying)     return InterpolationType::Varying;
	if (tok == UsdGeomTokens->vertex)      return InterpolationType::Vertex;
	if (tok == UsdGeomTokens->faceVarying) return InterpolationType::FaceVarying;
	return InterpolationType::Vertex;
}

// Triangulation

static std::vector<uint32_t> TriangulateIndices(
	VtArray<int> const& faceVertCounts,
	VtArray<int> const& faceVertIndices)
{
	std::vector<uint32_t> out;
	out.reserve(faceVertIndices.size());
	size_t offset = 0;
	for (int fvCount : faceVertCounts) {
		if (fvCount == 3) {
			out.push_back(faceVertIndices[offset + 0]);
			out.push_back(faceVertIndices[offset + 1]);
			out.push_back(faceVertIndices[offset + 2]);
		}
		else {
			for (int i = 1; i + 1 < fvCount; ++i) {
				out.push_back(faceVertIndices[offset + 0]);
				out.push_back(faceVertIndices[offset + i]);
				out.push_back(faceVertIndices[offset + i + 1]);
			}
		}
		offset += fvCount;
	}
	return out;
}

template<typename GfVecN>
static void FlattenVecArray(
	VtArray<GfVecN> const& src,
	std::vector<float>& dst,
	float scale = 1.0f)
{
	constexpr size_t N = GfVecN::dimension;
	dst.clear();
	dst.reserve(src.size() * N);
	for (auto const& v : src) {
		for (size_t i = 0; i < N; ++i)
			dst.push_back(float(v[i] * scale));
	}
}

static std::vector<float> ComputeFacetedNormals(
	const std::vector<float>& ctrlPos,
	const VtArray<int>& faceVertCounts,
	const VtArray<int>& faceVertIndices,
	bool reverseMeshWinding)
{
	std::vector<float> normals(faceVertCounts.size() * 3, 0.0f);

	size_t fvOffset = 0;
	for (size_t faceIndex = 0; faceIndex < faceVertCounts.size(); ++faceIndex) {
		const int fvCount = faceVertCounts[faceIndex];
		if (fvCount < 3) {
			fvOffset += fvCount;
			continue;
		}

		const size_t i0Fv = fvOffset;
		const size_t i1Fv = fvOffset + (reverseMeshWinding ? 2u : 1u);
		const size_t i2Fv = fvOffset + (reverseMeshWinding ? 1u : 2u);

		if (i2Fv >= faceVertIndices.size()) {
			spdlog::warn("Invalid face-vertex topology while computing faceted normals for face {}.", faceIndex);
			fvOffset += fvCount;
			continue;
		}

		const uint32_t i0 = static_cast<uint32_t>(faceVertIndices[i0Fv]);
		const uint32_t i1 = static_cast<uint32_t>(faceVertIndices[i1Fv]);
		const uint32_t i2 = static_cast<uint32_t>(faceVertIndices[i2Fv]);
		const size_t controlPointCount = ctrlPos.size() / 3;
		if (i0 >= controlPointCount) {
			spdlog::warn("Invalid position index {} while computing faceted normals.", i0);
			fvOffset += fvCount;
			continue;
		}
		if (i1 >= controlPointCount || i2 >= controlPointCount) {
			spdlog::warn("Invalid position index ({}, {}) while computing faceted normals.", i1, i2);
			fvOffset += fvCount;
			continue;
		}

		const GfVec3f p0(
			ctrlPos[i0 * 3 + 0],
			ctrlPos[i0 * 3 + 1],
			ctrlPos[i0 * 3 + 2]);
		const GfVec3f p1(
			ctrlPos[i1 * 3 + 0],
			ctrlPos[i1 * 3 + 1],
			ctrlPos[i1 * 3 + 2]);
		const GfVec3f p2(
			ctrlPos[i2 * 3 + 0],
			ctrlPos[i2 * 3 + 1],
			ctrlPos[i2 * 3 + 2]);

		GfVec3f faceNormal = GfCross(p1 - p0, p2 - p0);
		const float len2 = GfDot(faceNormal, faceNormal);
		if (len2 > 1e-20f) {
			faceNormal *= (1.0f / std::sqrt(len2));
			normals[faceIndex * 3 + 0] = faceNormal[0];
			normals[faceIndex * 3 + 1] = faceNormal[1];
			normals[faceIndex * 3 + 2] = faceNormal[2];
		}

		fvOffset += fvCount;
	}

	return normals;
}

struct ControlPointPositionKey
{
	uint32_t x;
	uint32_t y;
	uint32_t z;

	bool operator==(const ControlPointPositionKey&) const = default;
};

struct ControlPointPositionKeyHash
{
	size_t operator()(const ControlPointPositionKey& key) const noexcept
	{
		size_t h = static_cast<size_t>(key.x);
		h ^= static_cast<size_t>(key.y) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
		h ^= static_cast<size_t>(key.z) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
		return h;
	}
};

static uint32_t FloatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static ControlPointPositionKey MakeControlPointPositionKey(const std::vector<float>& ctrlPos, size_t index)
{
	const size_t base = index * 3u;
	return ControlPointPositionKey{
		FloatBits(ctrlPos[base + 0u]),
		FloatBits(ctrlPos[base + 1u]),
		FloatBits(ctrlPos[base + 2u])
	};
}

static std::vector<float> ComputeSmoothControlPointNormals(
	const std::vector<float>& ctrlPos,
	const VtArray<int>& faceVertCounts,
	const VtArray<int>& faceVertIndices,
	const std::vector<uint8_t>& useFace,
	const std::vector<uint8_t>& holedFaces,
	bool hasSubset,
	bool reverseMeshWinding)
{
	const size_t controlPointCount = ctrlPos.size() / 3u;
	std::vector<float> normals(controlPointCount * 3u, 0.0f);

	size_t fvOffset = 0;
	for (size_t faceIndex = 0; faceIndex < faceVertCounts.size(); ++faceIndex) {
		const int fvCount = faceVertCounts[faceIndex];
		if ((hasSubset && !useFace[faceIndex]) || holedFaces[faceIndex] || fvCount < 3) {
			fvOffset += fvCount;
			continue;
		}

		for (int i = 1; i + 1 < fvCount; ++i) {
			const size_t fv0 = fvOffset;
			const size_t fv1 = fvOffset + static_cast<size_t>(reverseMeshWinding ? (i + 1) : i);
			const size_t fv2 = fvOffset + static_cast<size_t>(reverseMeshWinding ? i : (i + 1));
			if (fv2 >= faceVertIndices.size()) {
				continue;
			}

			const uint32_t i0 = static_cast<uint32_t>(faceVertIndices[fv0]);
			const uint32_t i1 = static_cast<uint32_t>(faceVertIndices[fv1]);
			const uint32_t i2 = static_cast<uint32_t>(faceVertIndices[fv2]);
			if (i0 >= controlPointCount || i1 >= controlPointCount || i2 >= controlPointCount) {
				continue;
			}

			const GfVec3f p0(ctrlPos[i0 * 3u + 0u], ctrlPos[i0 * 3u + 1u], ctrlPos[i0 * 3u + 2u]);
			const GfVec3f p1(ctrlPos[i1 * 3u + 0u], ctrlPos[i1 * 3u + 1u], ctrlPos[i1 * 3u + 2u]);
			const GfVec3f p2(ctrlPos[i2 * 3u + 0u], ctrlPos[i2 * 3u + 1u], ctrlPos[i2 * 3u + 2u]);
			const GfVec3f faceNormal = GfCross(p1 - p0, p2 - p0);
			const float len2 = GfDot(faceNormal, faceNormal);
			if (len2 <= 1e-20f) {
				continue;
			}

			normals[i0 * 3u + 0u] += faceNormal[0];
			normals[i0 * 3u + 1u] += faceNormal[1];
			normals[i0 * 3u + 2u] += faceNormal[2];
			normals[i1 * 3u + 0u] += faceNormal[0];
			normals[i1 * 3u + 1u] += faceNormal[1];
			normals[i1 * 3u + 2u] += faceNormal[2];
			normals[i2 * 3u + 0u] += faceNormal[0];
			normals[i2 * 3u + 1u] += faceNormal[1];
			normals[i2 * 3u + 2u] += faceNormal[2];
		}

		fvOffset += fvCount;
	}

	std::unordered_map<ControlPointPositionKey, GfVec3f, ControlPointPositionKeyHash> coincidentNormalSums;
	coincidentNormalSums.reserve(controlPointCount);
	for (size_t index = 0; index < controlPointCount; ++index) {
		const GfVec3f n(normals[index * 3u + 0u], normals[index * 3u + 1u], normals[index * 3u + 2u]);
		auto [it, inserted] = coincidentNormalSums.try_emplace(
			MakeControlPointPositionKey(ctrlPos, index),
			0.0f,
			0.0f,
			0.0f);
		it->second += n;
	}

	for (size_t index = 0; index < controlPointCount; ++index) {
		GfVec3f n = coincidentNormalSums[MakeControlPointPositionKey(ctrlPos, index)];
		const float len2 = GfDot(n, n);
		if (len2 > 1e-20f) {
			n *= (1.0f / std::sqrt(len2));
			normals[index * 3u + 0u] = n[0];
			normals[index * 3u + 1u] = n[1];
			normals[index * 3u + 2u] = n[2];
		}
		else {
			normals[index * 3u + 1u] = 1.0f;
		}
	}

	return normals;
}

static UsdTimeCode GetUsdGeometrySampleTime(const UsdStageRefPtr& stage)
{
	if (stage && stage->HasAuthoredTimeCodeRange()) {
		return UsdTimeCode(stage->GetStartTimeCode());
	}

	return UsdTimeCode::Default();
}

static std::uint32_t TessellationSubdivisionForFactor(std::uint32_t tessellationFactor)
{
	if (tessellationFactor <= 1u) {
		return 1u;
	}

	const double root = std::sqrt(static_cast<double>(tessellationFactor));
	std::uint32_t subdivision = static_cast<std::uint32_t>(std::ceil(root));
	return std::max(1u, subdivision);
}

static std::uint32_t ReyesTessellationLookupIndex(std::uint32_t edge01Segments, std::uint32_t edge12Segments, std::uint32_t edge20Segments)
{
	constexpr uint32_t kLookupSize = 16u;
	constexpr uint32_t kLookupIndexBias =
		1u +
		kLookupSize +
		kLookupSize * kLookupSize;
	const uint32_t rawIndex =
		edge01Segments +
		edge12Segments * kLookupSize +
		edge20Segments * kLookupSize * kLookupSize;
	return rawIndex - kLookupIndexBias;
}

static DirectX::XMFLOAT3 DecodeReyesTessellationVertexBarycentrics(std::uint32_t packedVertex)
{
	constexpr float kCoordScale = 32768.0f;
	const float u = static_cast<float>(packedVertex & 0xFFFFu) / kCoordScale;
	const float v = static_cast<float>(packedVertex >> 16u) / kCoordScale;
	return DirectX::XMFLOAT3{ 1.0f - u - v, u, v };
}

static DirectX::XMUINT3 DecodeReyesTessellationTriangleIndices(std::uint32_t packedTriangle)
{
	return DirectX::XMUINT3{
		packedTriangle & 0xFFu,
		(packedTriangle >> 8u) & 0xFFu,
		(packedTriangle >> 16u) & 0xFFu
	};
}

struct TessellationPattern
{
	std::vector<DirectX::XMFLOAT3> vertices;
	std::vector<DirectX::XMUINT3> triangles;
	bool fromReyesTable{ false };
};

static std::uint32_t TriangularLatticeVertexIndex(std::uint32_t subdivision, std::uint32_t i, std::uint32_t j)
{
	return i * (subdivision + 1u) - (i * (i - 1u)) / 2u + j;
}

static std::shared_ptr<const TessellationPattern> BuildReyesTessellationPattern(std::uint32_t subdivision)
{
	const ReyesTessellationTableData& reyesTable = GetReyesTessellationTableData();
	const std::uint32_t configIndex = ReyesTessellationLookupIndex(subdivision, subdivision, subdivision);
	if (configIndex >= reyesTable.configs.size()) {
		return nullptr;
	}

	const CLodReyesTessTableConfigEntry& config = reyesTable.configs[configIndex];
	if (config.numVertices == 0u || config.numTriangles == 0u) {
		return nullptr;
	}

	auto pattern = std::make_shared<TessellationPattern>();
	pattern->fromReyesTable = true;
	pattern->vertices.reserve(config.numVertices);
	for (std::uint32_t i = 0; i < config.numVertices; ++i) {
		pattern->vertices.push_back(DecodeReyesTessellationVertexBarycentrics(reyesTable.vertices[config.firstVertex + i]));
	}
	pattern->triangles.reserve(config.numTriangles);
	for (std::uint32_t i = 0; i < config.numTriangles; ++i) {
		pattern->triangles.push_back(DecodeReyesTessellationTriangleIndices(reyesTable.triangles[config.firstTriangle + i]));
	}
	return pattern;
}

static std::shared_ptr<const TessellationPattern> BuildLatticeTessellationPattern(std::uint32_t subdivision)
{
	if (subdivision == 0u) {
		return nullptr;
	}

	const std::uint64_t vertexCount64 =
		(static_cast<std::uint64_t>(subdivision) + 1ull) *
		(static_cast<std::uint64_t>(subdivision) + 2ull) / 2ull;
	const std::uint64_t triangleCount64 = static_cast<std::uint64_t>(subdivision) * subdivision;
	if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
		triangleCount64 > std::numeric_limits<std::uint32_t>::max()) {
		return nullptr;
	}

	auto pattern = std::make_shared<TessellationPattern>();
	pattern->vertices.reserve(static_cast<size_t>(vertexCount64));
	pattern->triangles.reserve(static_cast<size_t>(triangleCount64));

	const float invSubdivision = 1.0f / static_cast<float>(subdivision);
	for (std::uint32_t i = 0; i <= subdivision; ++i) {
		for (std::uint32_t j = 0; j <= subdivision - i; ++j) {
			const float wb = static_cast<float>(i) * invSubdivision;
			const float wc = static_cast<float>(j) * invSubdivision;
			pattern->vertices.push_back(DirectX::XMFLOAT3{ 1.0f - wb - wc, wb, wc });
		}
	}

	for (std::uint32_t i = 0; i < subdivision; ++i) {
		for (std::uint32_t j = 0; j < subdivision - i; ++j) {
			pattern->triangles.push_back(DirectX::XMUINT3{
				TriangularLatticeVertexIndex(subdivision, i, j),
				TriangularLatticeVertexIndex(subdivision, i + 1u, j),
				TriangularLatticeVertexIndex(subdivision, i, j + 1u)
			});
			if (j + 1u < subdivision - i) {
				pattern->triangles.push_back(DirectX::XMUINT3{
					TriangularLatticeVertexIndex(subdivision, i + 1u, j),
					TriangularLatticeVertexIndex(subdivision, i + 1u, j + 1u),
					TriangularLatticeVertexIndex(subdivision, i, j + 1u)
				});
			}
		}
	}

	return pattern;
}

static std::shared_ptr<const TessellationPattern> GetTessellationPattern(std::uint32_t subdivision)
{
	static std::mutex cacheMutex;
	static std::unordered_map<std::uint32_t, std::shared_ptr<const TessellationPattern>> cache;

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (auto it = cache.find(subdivision); it != cache.end()) {
		return it->second;
	}

	auto pattern = subdivision <= 11u
		? BuildReyesTessellationPattern(subdivision)
		: BuildLatticeTessellationPattern(subdivision);
	if (!pattern) {
		pattern = BuildLatticeTessellationPattern(subdivision);
	}
	if (pattern) {
		cache.emplace(subdivision, pattern);
	}
	return pattern;
}

static void InterpolateFloatTuple(
	std::byte* dst,
	const std::byte* a,
	const std::byte* b,
	const std::byte* c,
	size_t offset,
	size_t components,
	float wa,
	float wb,
	float wc)
{
	auto* out = reinterpret_cast<float*>(dst + offset);
	const auto* av = reinterpret_cast<const float*>(a + offset);
	const auto* bv = reinterpret_cast<const float*>(b + offset);
	const auto* cv = reinterpret_cast<const float*>(c + offset);
	for (size_t component = 0; component < components; ++component) {
		out[component] = av[component] * wa + bv[component] * wb + cv[component] * wc;
	}
}

static void NormalizeFloat3(std::byte* dst, size_t offset)
{
	auto* value = reinterpret_cast<float*>(dst + offset);
	const float len2 = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
	if (len2 > 1e-20f) {
		const float invLen = 1.0f / std::sqrt(len2);
		value[0] *= invLen;
		value[1] *= invLen;
		value[2] *= invLen;
	}
}

static const std::byte* NearestBarycentricVertex(
	const std::byte* a,
	const std::byte* b,
	const std::byte* c,
	float wa,
	float wb,
	float wc)
{
	if (wb >= wa && wb >= wc) {
		return b;
	}
	if (wc >= wa && wc >= wb) {
		return c;
	}
	return a;
}

static void WriteInterpolatedVertex(
	std::vector<std::byte>& out,
	const std::byte* a,
	const std::byte* b,
	const std::byte* c,
	size_t vertexSize,
	unsigned int vertexFlags,
	float wa,
	float wb,
	float wc)
{
	const std::byte* nearest = NearestBarycentricVertex(a, b, c, wa, wb, wc);
	const size_t dstOffset = out.size();
	out.insert(out.end(), nearest, nearest + vertexSize);
	std::byte* dst = out.data() + dstOffset;

	InterpolateFloatTuple(dst, a, b, c, 0, 3, wa, wb, wc);
	if ((vertexFlags & VertexFlags::VERTEX_NORMALS) != 0) {
		InterpolateFloatTuple(dst, a, b, c, MeshVertexLayout::NormalOffset, 3, wa, wb, wc);
		NormalizeFloat3(dst, MeshVertexLayout::NormalOffset);
	}
	if ((vertexFlags & VertexFlags::VERTEX_TANGENTS) != 0) {
		const size_t tangentOffset = MeshVertexLayout::TangentOffset(vertexFlags);
		InterpolateFloatTuple(dst, a, b, c, tangentOffset, 3, wa, wb, wc);
		NormalizeFloat3(dst, tangentOffset);
		reinterpret_cast<float*>(dst + tangentOffset)[3] =
			reinterpret_cast<const float*>(nearest + tangentOffset)[3];
	}
	if ((vertexFlags & VertexFlags::VERTEX_TEXCOORDS) != 0) {
		InterpolateFloatTuple(dst, a, b, c, MeshVertexLayout::TexcoordOffset(vertexFlags), 2, wa, wb, wc);
	}
	if ((vertexFlags & VertexFlags::VERTEX_COLORS) != 0) {
		InterpolateFloatTuple(dst, a, b, c, MeshVertexLayout::ColorOffset(vertexFlags), 3, wa, wb, wc);
	}
}

static void WriteInterpolatedSkinningVertex(
	std::vector<std::byte>& out,
	const std::byte* a,
	const std::byte* b,
	const std::byte* c,
	size_t skinningVertexSize,
	float wa,
	float wb,
	float wc)
{
	const std::byte* nearest = NearestBarycentricVertex(a, b, c, wa, wb, wc);
	const size_t dstOffset = out.size();
	out.insert(out.end(), nearest, nearest + skinningVertexSize);
	std::byte* dst = out.data() + dstOffset;
	InterpolateFloatTuple(dst, a, b, c, 0, 3, wa, wb, wc);
	InterpolateFloatTuple(dst, a, b, c, sizeof(DirectX::XMFLOAT3), 3, wa, wb, wc);
	NormalizeFloat3(dst, sizeof(DirectX::XMFLOAT3));
}

static void TessellateExtractedTriangles(
	std::unique_ptr<std::vector<std::byte>>& rawData,
	std::optional<std::unique_ptr<std::vector<std::byte>>>& skinningData,
	unsigned int vertexSize,
	unsigned int skinningVertexSize,
	std::vector<UINT32>& indices,
	unsigned int vertexFlags,
	std::vector<MeshUvSetData>& uvSets,
	std::uint32_t tessellationFactor,
	const std::string& primName)
{
	const std::uint32_t subdivision = TessellationSubdivisionForFactor(tessellationFactor);
	if (subdivision <= 1u || !rawData || vertexSize == 0u || indices.size() < 3u) {
		return;
	}

	const std::uint64_t inputTriangleCount = indices.size() / 3u;
	const std::shared_ptr<const TessellationPattern> pattern = GetTessellationPattern(subdivision);
	if (!pattern || pattern->vertices.empty() || pattern->triangles.empty()) {
		spdlog::warn(
			"Mesh '{}' tessellation factor {} subdivision {} could not build a tessellation pattern; skipping tessellation.",
			primName,
			tessellationFactor,
			subdivision);
		return;
	}

	const std::uint64_t outputTriangleCount = inputTriangleCount * static_cast<std::uint64_t>(pattern->triangles.size());
	const std::uint64_t outputVertexCount = inputTriangleCount * static_cast<std::uint64_t>(pattern->vertices.size());
	if (outputVertexCount > std::numeric_limits<UINT32>::max() ||
		outputTriangleCount > (std::numeric_limits<std::uint64_t>::max() / 3ull)) {
		spdlog::warn(
			"Mesh '{}' tessellation factor {} subdivision {} would produce {} vertices and {} triangles; skipping tessellation.",
			primName,
			tessellationFactor,
			subdivision,
			outputVertexCount,
			outputTriangleCount);
		return;
	}

	const std::uint64_t outputIndexCount = outputTriangleCount * 3ull;
	if (outputIndexCount > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
		outputVertexCount > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() / std::max<std::size_t>(1u, vertexSize))) {
		spdlog::warn(
			"Mesh '{}' tessellation factor {} subdivision {} would produce too many indices for this process; skipping tessellation.",
			primName,
			tessellationFactor,
			subdivision);
		return;
	}

	std::vector<std::byte> tessellatedVertices;
	std::vector<UINT32> tessellatedIndices;
	tessellatedVertices.reserve(static_cast<size_t>(outputVertexCount) * static_cast<size_t>(vertexSize));
	tessellatedIndices.reserve(static_cast<size_t>(outputIndexCount));

	const bool hasSkinning = skinningData && *skinningData && skinningVertexSize != 0u;
	std::vector<std::byte> tessellatedSkinningVertices;
	if (hasSkinning) {
		tessellatedSkinningVertices.reserve(static_cast<size_t>(outputVertexCount) * static_cast<size_t>(skinningVertexSize));
	}

	std::vector<MeshUvSetData> tessellatedUvSets;
	tessellatedUvSets.resize(uvSets.size());
	for (size_t uvSetIndex = 0; uvSetIndex < uvSets.size(); ++uvSetIndex) {
		tessellatedUvSets[uvSetIndex].name = uvSets[uvSetIndex].name;
		tessellatedUvSets[uvSetIndex].values.reserve(static_cast<size_t>(outputVertexCount));
	}

	const auto emitVertex = [&](UINT32 ia, UINT32 ib, UINT32 ic, float wa, float wb, float wc) {
		const std::byte* a = rawData->data() + static_cast<size_t>(ia) * vertexSize;
		const std::byte* b = rawData->data() + static_cast<size_t>(ib) * vertexSize;
		const std::byte* c = rawData->data() + static_cast<size_t>(ic) * vertexSize;
		WriteInterpolatedVertex(tessellatedVertices, a, b, c, vertexSize, vertexFlags, wa, wb, wc);
		if (hasSkinning) {
			const std::byte* sa = (*skinningData)->data() + static_cast<size_t>(ia) * skinningVertexSize;
			const std::byte* sb = (*skinningData)->data() + static_cast<size_t>(ib) * skinningVertexSize;
			const std::byte* sc = (*skinningData)->data() + static_cast<size_t>(ic) * skinningVertexSize;
			WriteInterpolatedSkinningVertex(tessellatedSkinningVertices, sa, sb, sc, skinningVertexSize, wa, wb, wc);
		}
		for (size_t uvSetIndex = 0; uvSetIndex < uvSets.size(); ++uvSetIndex) {
			const auto sampleUv = [&](UINT32 index) {
				return index < uvSets[uvSetIndex].values.size()
					? uvSets[uvSetIndex].values[index]
					: DirectX::XMFLOAT2{ 0.0f, 0.0f };
			};
			const DirectX::XMFLOAT2 uva = sampleUv(ia);
			const DirectX::XMFLOAT2 uvb = sampleUv(ib);
			const DirectX::XMFLOAT2 uvc = sampleUv(ic);
			tessellatedUvSets[uvSetIndex].values.push_back(DirectX::XMFLOAT2{
				uva.x * wa + uvb.x * wb + uvc.x * wc,
				uva.y * wa + uvb.y * wb + uvc.y * wc });
		}
	};

	for (size_t tri = 0; tri + 2u < indices.size(); tri += 3u) {
		const UINT32 ia = indices[tri + 0u];
		const UINT32 ib = indices[tri + 1u];
		const UINT32 ic = indices[tri + 2u];
		const auto baseVertex = static_cast<UINT32>(tessellatedVertices.size() / vertexSize);
		for (const DirectX::XMFLOAT3& bary : pattern->vertices) {
			emitVertex(ia, ib, ic, bary.x, bary.y, bary.z);
		}
		for (const DirectX::XMUINT3& local : pattern->triangles) {
			tessellatedIndices.push_back(baseVertex + local.x);
			tessellatedIndices.push_back(baseVertex + local.y);
			tessellatedIndices.push_back(baseVertex + local.z);
		}
	}

	*rawData = std::move(tessellatedVertices);
	indices = std::move(tessellatedIndices);
	if (hasSkinning) {
		**skinningData = std::move(tessellatedSkinningVertices);
	}
	uvSets = std::move(tessellatedUvSets);

	spdlog::debug(
		"Mesh '{}' tessellated: requested_factor={} subdivision={} source={} triangles {} -> {} vertices_per_source_triangle={}",
		primName,
		tessellationFactor,
		subdivision,
		pattern->fromReyesTable ? "reyes-table" : "lattice",
		inputTriangleCount,
		outputTriangleCount,
		pattern->vertices.size());
}

static bool HasMultipleAuthoredTimeSamples(const UsdAttribute& attr)
{
	if (!attr) {
		return false;
	}

	std::vector<double> timeSamples;
	if (!attr.GetTimeSamples(&timeSamples)) {
		return false;
	}

	return timeSamples.size() > 1;
}

// LoadGeom
// Extracts raw vertex + index arrays from a UsdGeomMesh (optionally
// limited to one or more subsets).  Handles face-varying expansion, optional
// normals, texture coordinates, and skinning vertex streams.

static void LoadGeom(
	std::unique_ptr<std::vector<std::byte>>& rawData,
	std::optional<std::unique_ptr<std::vector<std::byte>>>& skinningData,
	unsigned int& vertexSize,
	unsigned int& skinningVertexSize,
	std::vector<UINT32>& indices,
	unsigned int& vertexFlags,
    std::vector<MeshUvSetData>& uvSets,
	const UsdGeomMesh& mesh,
	const std::vector<UsdGeomSubset>& subsets,
	UsdTimeCode geomTimeCode,
	double metersPerUnit,
	const std::vector<std::string>& requiredUvSetNames,
	const std::optional<UsdSkelSkinningQuery>& skinQ,
	const VtTokenArray& skelJointOrderRaw,
	const VtTokenArray& skelJointOrderMapped,
	const USDGeometryExtractor::ExtractOptions& options)
{
	rawData = std::make_unique<std::vector<std::byte>>();
	skinningData.reset();
	indices.clear();
	vertexFlags = 0;
    uvSets.clear();

	// If we have a skeleton, build joint mappings
	std::unordered_map<unsigned int, unsigned int> jointMapping;
	for (unsigned int i = 0; i < skelJointOrderMapped.size(); i++) {
		std::string jointName = skelJointOrderMapped[i].GetString();
		auto it = std::find_if(skelJointOrderRaw.begin(), skelJointOrderRaw.end(),
			[&jointName](const pxr::TfToken& token) {
				return token.GetString() == jointName;
			});
		if (it != skelJointOrderRaw.end()) {
			unsigned int rawIndex = static_cast<unsigned int>(std::distance(skelJointOrderRaw.begin(), it));
			jointMapping[i] = rawIndex;
		}
		else {
			spdlog::error("Joint {} not found in raw joint order.", jointName);
			throw std::runtime_error("Invalid joint name in mapped joint order");
		}
	}

	// positions
	VtArray<GfVec3f> usdPts;
	mesh.GetPointsAttr().Get(&usdPts, geomTimeCode);

	std::vector<float> ctrlPos;
	FlattenVecArray<GfVec3f>(usdPts, ctrlPos, static_cast<float>(metersPerUnit));

	// control mesh topology
	VtArray<int> faceVertCounts, faceVertIndices;
	mesh.GetFaceVertexCountsAttr().Get(&faceVertCounts, geomTimeCode);
	mesh.GetFaceVertexIndicesAttr().Get(&faceVertIndices, geomTimeCode);

	TfToken orientation = UsdGeomTokens->rightHanded;
	mesh.GetOrientationAttr().Get(&orientation);
	const bool reverseMeshWinding = (orientation == UsdGeomTokens->leftHanded);

	TfToken subdivisionScheme = UsdGeomTokens->catmullClark;
	mesh.GetSubdivisionSchemeAttr().Get(&subdivisionScheme);
	const bool isPolygonalMesh = (subdivisionScheme == UsdGeomTokens->none);
	const bool previewSubdiv = !isPolygonalMesh;
	const bool previewTopology =
		HasMultipleAuthoredTimeSamples(mesh.GetFaceVertexCountsAttr()) ||
		HasMultipleAuthoredTimeSamples(mesh.GetFaceVertexIndicesAttr()) ||
		HasMultipleAuthoredTimeSamples(mesh.GetHoleIndicesAttr());

	skinningVertexSize = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3)
		+ sizeof(uint32_t) * kMaxSkinInfluences + sizeof(float) * kMaxSkinInfluences;

	// subset face mask
	std::vector<uint8_t> useFace(faceVertCounts.size(), 0);
	const bool limitToSubsetFaces = !subsets.empty();
	for (const UsdGeomSubset& subset : subsets) {
		VtArray<int> subsetFaceIndices;
		subset.GetIndicesAttr().Get(&subsetFaceIndices);
		for (int fi : subsetFaceIndices)
			if (fi >= 0 && (size_t)fi < useFace.size()) useFace[fi] = 1;
	}

	std::string primName = mesh.GetPrim().GetName().GetString();
	UsdGeomPrimvarsAPI primvarsAPI(mesh);
	if (previewSubdiv) {
		spdlog::info(
			"Mesh '{}' uses subdivision scheme '{}' at sample time {}; rendering control cage as a static preview mesh.",
			primName,
			subdivisionScheme.GetString(),
			geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
	}
	if (previewTopology) {
		spdlog::info(
			"Mesh '{}' has time-varying topology or hole data; freezing extraction to sample time {} for static preview rendering.",
			primName,
			geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
	}

	VtArray<int> holeIndices;
	if (mesh.GetHoleIndicesAttr()) {
		mesh.GetHoleIndicesAttr().Get(&holeIndices, geomTimeCode);
	}

	std::vector<uint8_t> holedFaces(faceVertCounts.size(), 0);
	for (int holeFaceIndex : holeIndices) {
		if (holeFaceIndex >= 0 && static_cast<size_t>(holeFaceIndex) < holedFaces.size()) {
			holedFaces[holeFaceIndex] = 1;
		}
		else {
			spdlog::warn(
				"Mesh '{}' authored hole face index {} outside the face range {}; ignoring it for preview extraction.",
				primName,
				holeFaceIndex,
				faceVertCounts.size());
		}
	}

	// Count output corners
	size_t cornerCount = 0;
	for (size_t faceIndex = 0; faceIndex < faceVertCounts.size(); ++faceIndex) {
		if ((limitToSubsetFaces && !useFace[faceIndex]) || holedFaces[faceIndex]) continue;
		const int fvCount = faceVertCounts[faceIndex];
		if (fvCount == 3)
			cornerCount += 3;
		else if (fvCount > 3)
			cornerCount += static_cast<size_t>(fvCount - 2) * 3;
	}

	if (cornerCount > 0 && ctrlPos.empty()) {
		spdlog::warn(
			"Mesh '{}' has topology at geometry sample time {} but no readable positions; skipping mesh.",
			primName,
			geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
		return;
	}

	// normals
	bool gotNormals = false;
	InterpolationType normInterp = InterpolationType::Vertex;
	std::vector<float> rawNormals;

	UsdGeomPrimvar normalPrimvar = primvarsAPI.GetPrimvar(TfToken("normals"));
	if (normalPrimvar) {
		VtArray<GfVec3f> usdPrimvarNormals;
		if (normalPrimvar.ComputeFlattened(&usdPrimvarNormals, geomTimeCode)) {
			FlattenVecArray<GfVec3f>(usdPrimvarNormals, rawNormals, 1.0f);
			normInterp = GetInterpolationType(normalPrimvar.GetInterpolation());
			gotNormals = true;
			vertexFlags |= VertexFlags::VERTEX_NORMALS;
		}
		else {
			spdlog::warn(
				"Mesh '{}' authored primvars:normals but it could not be flattened at geometry sample time {}; falling back to legacy normals attribute if present.",
				primName,
				geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
		}
	}

	if (!gotNormals) {
		VtArray<GfVec3f> usdNormals;
		if (mesh.GetNormalsAttr().Get(&usdNormals, geomTimeCode)) {
			FlattenVecArray<GfVec3f>(usdNormals, rawNormals, 1.0f);
			normInterp = GetInterpolationType(mesh.GetNormalsInterpolation());
			gotNormals = true;
			vertexFlags |= VertexFlags::VERTEX_NORMALS;
		}
	}

	if (!gotNormals && (isPolygonalMesh || previewSubdiv || previewTopology)) {
		rawNormals = ComputeFacetedNormals(ctrlPos, faceVertCounts, faceVertIndices, reverseMeshWinding);
		if (!rawNormals.empty()) {
			gotNormals = true;
			normInterp = InterpolationType::Uniform;
			vertexFlags |= VertexFlags::VERTEX_NORMALS;
			if (previewSubdiv) {
				spdlog::info("Generated faceted preview normals for subdivision control cage '{}'.", primName);
			}
			else if (previewTopology) {
				spdlog::info("Generated faceted preview normals for frozen topology mesh '{}'.", primName);
			}
			else {
				spdlog::info("Generated faceted normals for polygon mesh '{}' because no normals attribute was authored.", primName);
			}
		}
	}

	if (isPolygonalMesh || previewSubdiv || previewTopology) {
		if (options.brniflyModelSpaceNormals) {
			rawNormals = ComputeFacetedNormals(ctrlPos, faceVertCounts, faceVertIndices, reverseMeshWinding);
			if (!rawNormals.empty()) {
				gotNormals = true;
				normInterp = InterpolationType::Uniform;
				vertexFlags |= VertexFlags::VERTEX_NORMALS;
				spdlog::info("Using faceted CLod source normals for model-space normal map mesh '{}'.", primName);
			}
		}
		else {
			auto smoothNormals = ComputeSmoothControlPointNormals(
				ctrlPos,
				faceVertCounts,
				faceVertIndices,
				useFace,
				holedFaces,
				limitToSubsetFaces,
				reverseMeshWinding);
			if (!smoothNormals.empty()) {
				rawNormals = std::move(smoothNormals);
				gotNormals = true;
				normInterp = InterpolationType::Vertex;
				vertexFlags |= VertexFlags::VERTEX_NORMALS;
				spdlog::info("Using calculated smooth CLod source normals for mesh '{}'.", primName);
			}
		}
	}

	bool gotColors = false;
	InterpolationType colorInterp = InterpolationType::Vertex;
	std::vector<float> rawColors;
	UsdGeomPrimvar displayColorPrimvar = primvarsAPI.FindPrimvarWithInheritance(TfToken("displayColor"));
	if (displayColorPrimvar) {
		VtArray<GfVec3f> usdColors;
		if (displayColorPrimvar.ComputeFlattened(&usdColors, geomTimeCode)) {
			FlattenVecArray<GfVec3f>(usdColors, rawColors, 1.0f);
			colorInterp = GetInterpolationType(displayColorPrimvar.GetInterpolation());
			gotColors = true;
			vertexFlags |= VertexFlags::VERTEX_COLORS;
		}
		else {
			spdlog::debug(
				"Mesh '{}' authored primvars:displayColor but it could not be flattened at geometry sample time {}; ignoring vertex colors for preview extraction.",
				primName,
				geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
		}
	}

	bool gotOpacity = false;
	InterpolationType opacityInterp = InterpolationType::Vertex;
	std::vector<float> rawOpacities;
	UsdGeomPrimvar displayOpacityPrimvar = primvarsAPI.FindPrimvarWithInheritance(TfToken("displayOpacity"));
	if (displayOpacityPrimvar) {
		VtArray<float> usdOpacities;
		if (displayOpacityPrimvar.ComputeFlattened(&usdOpacities, geomTimeCode)) {
			rawOpacities.assign(usdOpacities.begin(), usdOpacities.end());
			opacityInterp = GetInterpolationType(displayOpacityPrimvar.GetInterpolation());
			gotOpacity = true;
		}
		else {
			spdlog::debug(
				"Mesh '{}' authored primvars:displayOpacity but it could not be flattened at geometry sample time {}; ignoring vertex opacity for preview extraction.",
				primName,
				geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
		}
	}

	bool gotTangents = false;
	InterpolationType tangentInterp = InterpolationType::Vertex;
	std::vector<float> rawTangents;
	{
		UsdGeomPrimvar tangentPrimvar = primvarsAPI.GetPrimvar(TfToken("brnifly:tangents"));
		if (tangentPrimvar) {
			VtArray<GfVec4f> usdTangents;
			if (tangentPrimvar.ComputeFlattened(&usdTangents, geomTimeCode)) {
				rawTangents.reserve(usdTangents.size() * 4u);
				for (const GfVec4f& tangent : usdTangents) {
					rawTangents.push_back(tangent[0]);
					rawTangents.push_back(tangent[1]);
					rawTangents.push_back(tangent[2]);
					rawTangents.push_back(tangent[3]);
				}
				tangentInterp = GetInterpolationType(tangentPrimvar.GetInterpolation());
				gotTangents = true;
				vertexFlags |= VertexFlags::VERTEX_TANGENTS;
			}
			else {
				spdlog::warn(
					"Mesh '{}' authored primvars:brnifly:tangents but it could not be flattened at geometry sample time {}; falling back to derivative tangent basis.",
					primName,
					geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
			}
		}
	}

    std::vector<std::string> uvSetNames;
    for (const std::string& requiredUvSetName : requiredUvSetNames) {
        if (!requiredUvSetName.empty()) {
            uvSetNames.push_back(requiredUvSetName);
        }
    }
    if (std::find(uvSetNames.begin(), uvSetNames.end(), "st") == uvSetNames.end()) {
        uvSetNames.push_back("st");
    }
    {
        std::set<std::string> remainingNames;
        for (const UsdGeomPrimvar& primvar : primvarsAPI.GetPrimvars()) {
            if (!primvar) {
                continue;
            }

            const auto typeName = primvar.GetTypeName();
            if (typeName != SdfValueTypeNames->TexCoord2fArray &&
                typeName != SdfValueTypeNames->Float2Array) {
                continue;
            }

            const std::string primvarName = primvar.GetPrimvarName().GetString();
            if (primvarName == "st") {
                continue;
            }

            remainingNames.insert(primvarName);
        }

        for (const std::string& remainingName : remainingNames) {
            if (std::find(uvSetNames.begin(), uvSetNames.end(), remainingName) == uvSetNames.end()) {
                uvSetNames.push_back(remainingName);
            }
        }
    }

    struct UvSetBuildData {
        MeshUvSetData uvSet;
        bool available = false;
        InterpolationType interpolation = InterpolationType::Vertex;
        std::vector<float> rawData;
    };

    std::vector<UvSetBuildData> uvSetBuildData;
    uvSetBuildData.reserve(uvSetNames.size());
    for (const std::string& uvSetName : uvSetNames) {
        UvSetBuildData uvData;
        uvData.uvSet.name = uvSetName;

        UsdAttribute tcAttr = mesh.GetPrim().GetAttribute(TfToken("primvars:" + uvSetName));
        UsdGeomPrimvar uvPrim(tcAttr);
        VtArray<GfVec2f> usdTC;
        uvData.available = (uvPrim && uvPrim.ComputeFlattened(&usdTC, geomTimeCode));
        uvData.interpolation = uvData.available ? GetInterpolationType(uvPrim.GetInterpolation()) : InterpolationType::Vertex;
        if (uvData.available) {
            uvData.rawData.reserve(usdTC.size() * 2);
            for (auto const& uv : usdTC) {
                uvData.rawData.push_back(float(uv[0]));
                uvData.rawData.push_back(1.0f - float(uv[1]));
            }
        }

        uvSetBuildData.push_back(std::move(uvData));
    }

    size_t primaryUvSetIndex = uvSetBuildData.size();
    for (size_t uvSetIndex = 0; uvSetIndex < uvSetBuildData.size(); ++uvSetIndex) {
        if (uvSetBuildData[uvSetIndex].available) {
            primaryUvSetIndex = uvSetIndex;
            vertexFlags |= VertexFlags::VERTEX_TEXCOORDS;
            break;
        }
    }

	vertexSize = MeshVertexLayout::VertexSize(vertexFlags);

	// skinning
	UsdSkelBindingAPI bindAPI(mesh.GetPrim());
	UsdSkelSkeleton skel = bindAPI.GetInheritedSkeleton();
	std::vector<uint32_t> rawJoints;
	std::vector<float>    rawWeights;
	InterpolationType jointInterp = InterpolationType::Vertex,
		weightInterp = InterpolationType::Vertex;

	if (skinQ) {
		VtIntArray   jointIndices;
		VtFloatArray jointWeights;
		skinQ.value().ComputeVaryingJointInfluences(
			usdPts.size(), &jointIndices, &jointWeights);

		unsigned int influencesPerPoint = skinQ.value().GetNumInfluencesPerComponent();
		unsigned short maxInfluencesPerJoint = static_cast<unsigned short>(kMaxSkinInfluences);
		rawJoints.reserve(usdPts.size() * maxInfluencesPerJoint);
		rawWeights.reserve(usdPts.size() * maxInfluencesPerJoint);

		size_t cursor = 0;
		for (size_t pt = 0; pt < usdPts.size(); ++pt) {
			for (unsigned int slot = 0; slot < maxInfluencesPerJoint; ++slot) {
				if (slot < influencesPerPoint) {
					if (cursor < jointIndices.size()) {
						rawJoints.push_back((uint32_t)jointMapping[jointIndices[cursor]]);
						rawWeights.push_back(jointWeights[cursor]);
						++cursor;
					}
					else {
						rawJoints.push_back(0u);
						rawWeights.push_back(0.0f);
					}
				}
				else {
					rawJoints.push_back(0u);
					rawWeights.push_back(0.0f);
				}
			}
			if (maxInfluencesPerJoint < influencesPerPoint)
				cursor += influencesPerPoint - maxInfluencesPerJoint;
		}

		jointInterp = GetInterpolationType(
			bindAPI.GetJointIndicesPrimvar().GetInterpolation());
		weightInterp = GetInterpolationType(
			bindAPI.GetJointWeightsPrimvar().GetInterpolation());

		vertexFlags |= VertexFlags::VERTEX_SKINNED;
	}
	else {
		UsdGeomPrimvar jointIndexPrimvar = primvarsAPI.GetPrimvar(TfToken("brnifly:jointIndices"));
		UsdGeomPrimvar jointWeightPrimvar = primvarsAPI.GetPrimvar(TfToken("brnifly:jointWeights"));
		if (jointIndexPrimvar && jointWeightPrimvar) {
			VtArray<int> jointIndices;
			VtArray<float> jointWeights;
			if (jointIndexPrimvar.ComputeFlattened(&jointIndices, geomTimeCode) &&
				jointWeightPrimvar.ComputeFlattened(&jointWeights, geomTimeCode)) {
				unsigned int indexElementSize = (std::max)(1, jointIndexPrimvar.GetElementSize());
				unsigned int weightElementSize = (std::max)(1, jointWeightPrimvar.GetElementSize());
				if (indexElementSize == 1 && weightElementSize == 1 &&
					jointIndices.size() == usdPts.size() * kMaxSkinInfluences &&
					jointWeights.size() == usdPts.size() * kMaxSkinInfluences) {
					indexElementSize = kMaxSkinInfluences;
					weightElementSize = kMaxSkinInfluences;
				}
				const unsigned int tupleCount = static_cast<unsigned int>((std::min)(
					jointIndices.size() / indexElementSize,
					jointWeights.size() / weightElementSize));
				const unsigned int maxInfluencesPerJoint = static_cast<unsigned int>(kMaxSkinInfluences);
				rawJoints.reserve(static_cast<size_t>(tupleCount) * maxInfluencesPerJoint);
				rawWeights.reserve(static_cast<size_t>(tupleCount) * maxInfluencesPerJoint);

				for (unsigned int tuple = 0; tuple < tupleCount; ++tuple) {
					for (unsigned int slot = 0; slot < maxInfluencesPerJoint; ++slot) {
						if (slot < indexElementSize && slot < weightElementSize) {
							rawJoints.push_back(static_cast<uint32_t>(jointIndices[static_cast<size_t>(tuple) * indexElementSize + slot]));
							rawWeights.push_back(jointWeights[static_cast<size_t>(tuple) * weightElementSize + slot]);
						}
						else {
							rawJoints.push_back(0u);
							rawWeights.push_back(0.0f);
						}
					}
				}

				jointInterp = GetInterpolationType(jointIndexPrimvar.GetInterpolation());
				weightInterp = GetInterpolationType(jointWeightPrimvar.GetInterpolation());
				vertexFlags |= VertexFlags::VERTEX_SKINNED;
			}
			else {
				spdlog::warn(
					"Mesh '{}' has BRNifly skinning primvars, but they could not be flattened at geometry sample time {}; importing as rigid geometry.",
					primName,
					geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
			}
		}
	}

	// allocate output buffers
	rawData->resize(cornerCount * vertexSize);
	indices.reserve(cornerCount);

	const bool hasSkinning = !rawJoints.empty();
	if (hasSkinning) {
		skinningData = std::make_unique<std::vector<std::byte>>(cornerCount * skinningVertexSize);
	}

	// tuple-copy helpers
	auto tupleBase = [](InterpolationType interp, size_t faceIndex,
		size_t fvIndex, uint32_t vertIndex, size_t numComponents) -> size_t
	{
		switch (interp) {
		case InterpolationType::Constant:   return 0;
		case InterpolationType::Uniform:    return faceIndex * numComponents;
		case InterpolationType::Vertex:
		case InterpolationType::Varying:    return static_cast<size_t>(vertIndex) * numComponents;
		case InterpolationType::FaceVarying:return fvIndex * numComponents;
		}
		return 0;
	};

	auto copyTupleFloat = [&](std::byte* dst, const std::vector<float>& raw,
		size_t numComponents, InterpolationType interp,
		size_t faceIndex, size_t fvIndex, uint32_t vertIndex,
		bool& warned, const char* attributeName)
	{
		const size_t base = tupleBase(interp, faceIndex, fvIndex, vertIndex, numComponents);
		if (base + numComponents > raw.size()) {
			std::memset(dst, 0, numComponents * sizeof(float));
			if (!warned) {
				spdlog::warn(
					"Mesh '{}' sampled '{}' tuple data out of range at geometry sample time {}; zero-filling missing values.",
					primName,
					attributeName,
					geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
				warned = true;
			}
			return;
		}
		std::memcpy(dst, raw.data() + base, numComponents * sizeof(float));
	};

	auto sampleScalar = [&](const std::vector<float>& raw,
		InterpolationType interp,
		size_t faceIndex,
		size_t fvIndex,
		uint32_t vertIndex,
		float defaultValue,
		bool& warned,
		const char* attributeName) -> float
	{
		const size_t base = tupleBase(interp, faceIndex, fvIndex, vertIndex, 1);
		if (base >= raw.size()) {
			if (!warned) {
				spdlog::warn(
					"Mesh '{}' sampled '{}' scalar data out of range at geometry sample time {}; using default value {}.",
					primName,
					attributeName,
					geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue(),
					defaultValue);
				warned = true;
			}
			return defaultValue;
		}
		return raw[base];
	};

	auto copyTupleUInt = [&](std::byte* dst, const std::vector<uint32_t>& raw,
		size_t numComponents, InterpolationType interp,
		size_t faceIndex, size_t fvIndex, uint32_t vertIndex,
		bool& warned, const char* attributeName)
	{
		const size_t base = tupleBase(interp, faceIndex, fvIndex, vertIndex, numComponents);
		if (base + numComponents > raw.size()) {
			std::memset(dst, 0, numComponents * sizeof(uint32_t));
			if (!warned) {
				spdlog::warn(
					"Mesh '{}' sampled '{}' tuple data out of range at geometry sample time {}; zero-filling missing values.",
					primName,
					attributeName,
					geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
				warned = true;
			}
			return;
		}
		std::memcpy(dst, raw.data() + base, numComponents * sizeof(uint32_t));
	};

	// emit vertices
	const DirectX::XMFLOAT3 defaultNormal{ 0.0f, 0.0f, 0.0f };
	bool warnedInvalidPositionIndex = false;
	bool warnedInvalidFaceVertexIndex = false;
	bool warnedNormalTupleRange = false;
	bool warnedTangentTupleRange = false;
	bool warnedUvTupleRange = false;
	bool warnedColorTupleRange = false;
	bool warnedOpacityTupleRange = false;
	bool warnedJointTupleRange = false;
	bool warnedWeightTupleRange = false;
	size_t alphaRejectedTriangles = 0;

	size_t fvOffset = 0;
	size_t outVertex = 0;
	for (size_t f = 0; f < faceVertCounts.size(); ++f) {
		int fc = faceVertCounts[f];
		if ((!limitToSubsetFaces || useFace[f]) && !holedFaces[f]) {
			for (int i = 1; i + 1 < fc; ++i) {
				int cornerIdxs[3] = { 0, reverseMeshWinding ? (i + 1) : i, reverseMeshWinding ? i : (i + 1) };
				uint32_t triVertIdxs[3] = {};
				bool validTriangle = true;
				for (int c = 0; c < 3; ++c) {
					const size_t fvIndex = fvOffset + static_cast<size_t>(cornerIdxs[c]);
					if (fvIndex >= faceVertIndices.size()) {
						if (!warnedInvalidFaceVertexIndex) {
							spdlog::warn(
								"Mesh '{}' has face-vertex index {} out of range for topology buffer size {}; skipping malformed triangle.",
								primName,
								fvIndex,
								faceVertIndices.size());
							warnedInvalidFaceVertexIndex = true;
						}
						validTriangle = false;
						break;
					}

					const uint32_t vertIdx = static_cast<uint32_t>(faceVertIndices[fvIndex]);
					if (static_cast<size_t>(vertIdx) >= ctrlPos.size() / 3) {
						if (!warnedInvalidPositionIndex) {
							spdlog::warn(
								"Mesh '{}' has position index {} out of range for {} control points at geometry sample time {}; skipping malformed triangle.",
								primName,
								vertIdx,
								ctrlPos.size() / 3,
								geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());
							warnedInvalidPositionIndex = true;
						}
						validTriangle = false;
						break;
					}
					triVertIdxs[c] = vertIdx;
				}

				if (!validTriangle) {
					continue;
				}

				if (options.vertexAlphaCutoff.has_value() && gotOpacity) {
					bool allBelowCutoff = true;
					for (int c = 0; c < 3; ++c) {
						const size_t fvIndex = fvOffset + static_cast<size_t>(cornerIdxs[c]);
						const uint32_t vertIdx = triVertIdxs[c];
						const float opacity = sampleScalar(rawOpacities, opacityInterp, f, fvIndex, vertIdx, 1.0f, warnedOpacityTupleRange, "displayOpacity");
						if (opacity >= options.vertexAlphaCutoff.value()) {
							allBelowCutoff = false;
							break;
						}
					}

					if (allBelowCutoff) {
						++alphaRejectedTriangles;
						continue;
					}
				}

				for (int c = 0; c < 3; ++c) {
					size_t fvIndex = fvOffset + cornerIdxs[c];
					uint32_t vertIdx = triVertIdxs[c];

					const size_t outBase = outVertex * vertexSize;
					std::byte* outPtr = rawData->data() + outBase;

					const size_t posBase = static_cast<size_t>(vertIdx) * 3;
					std::memcpy(outPtr, ctrlPos.data() + posBase, sizeof(DirectX::XMFLOAT3));

					if (gotNormals)
						copyTupleFloat(outPtr + MeshVertexLayout::NormalOffset, rawNormals, 3, normInterp, f, fvIndex, vertIdx, warnedNormalTupleRange, "normals");
					else
						std::memcpy(outPtr + MeshVertexLayout::NormalOffset, &defaultNormal, sizeof(defaultNormal));

					if (gotTangents) {
						copyTupleFloat(outPtr + MeshVertexLayout::TangentOffset(vertexFlags), rawTangents, 4, tangentInterp, f, fvIndex, vertIdx, warnedTangentTupleRange, "brnifly:tangents");
					}

                    if (primaryUvSetIndex < uvSetBuildData.size()) {
                        DirectX::XMFLOAT2 packedUv = { 0.0f, 0.0f };
                        copyTupleFloat(reinterpret_cast<std::byte*>(&packedUv), uvSetBuildData[primaryUvSetIndex].rawData, 2, uvSetBuildData[primaryUvSetIndex].interpolation, f, fvIndex, vertIdx, warnedUvTupleRange, uvSetBuildData[primaryUvSetIndex].uvSet.name.c_str());
                        std::memcpy(outPtr + MeshVertexLayout::TexcoordOffset(vertexFlags), &packedUv, sizeof(packedUv));
                    }

					if (gotColors) {
						copyTupleFloat(outPtr + MeshVertexLayout::ColorOffset(vertexFlags), rawColors, 3, colorInterp, f, fvIndex, vertIdx, warnedColorTupleRange, "displayColor");
					}

					if (hasSkinning) {
						std::byte* skinPtr = skinningData.value()->data() + outVertex * skinningVertexSize;
						std::memcpy(skinPtr, outPtr, sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3));

						const size_t jointsOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
						const size_t weightsOffset = jointsOffset + sizeof(uint32_t) * kMaxSkinInfluences;
						copyTupleUInt(skinPtr + jointsOffset, rawJoints, kMaxSkinInfluences, jointInterp, f, fvIndex, vertIdx, warnedJointTupleRange, "jointIndices");
						copyTupleFloat(skinPtr + weightsOffset, rawWeights, kMaxSkinInfluences, weightInterp, f, fvIndex, vertIdx, warnedWeightTupleRange, "jointWeights");
					}

					indices.push_back(static_cast<UINT32>(outVertex));
					outVertex++;
                    for (size_t uvSetIndex = 0; uvSetIndex < uvSetBuildData.size(); ++uvSetIndex) {
                        DirectX::XMFLOAT2 uvValue = { 0.0f, 0.0f };
                        if (uvSetBuildData[uvSetIndex].available) {
                            copyTupleFloat(reinterpret_cast<std::byte*>(&uvValue), uvSetBuildData[uvSetIndex].rawData, 2, uvSetBuildData[uvSetIndex].interpolation, f, fvIndex, vertIdx, warnedUvTupleRange, uvSetBuildData[uvSetIndex].uvSet.name.c_str());
                        }
                        if (uvSets.size() <= uvSetIndex) {
                            uvSets.resize(uvSetIndex + 1u);
                            uvSets[uvSetIndex].name = uvSetBuildData[uvSetIndex].uvSet.name;
                        }
                        uvSets[uvSetIndex].values.push_back(uvValue);
                    }
				}
			}
		}
		fvOffset += fc;
	}

	rawData->resize(outVertex * static_cast<size_t>(vertexSize));
	if (hasSkinning) {
		skinningData.value()->resize(outVertex * static_cast<size_t>(skinningVertexSize));
	}
	if (alphaRejectedTriangles > 0) {
		spdlog::info(
			"Mesh '{}' rejected {} triangle(s) using primvars:displayOpacity and alpha cutoff {}.",
			primName,
			alphaRejectedTriangles,
			options.vertexAlphaCutoff.value());
	}
}

}

// Public API

namespace USDGeometryExtractor {

namespace {
	struct AtomicBenchmarkStats {
		std::atomic<std::uint64_t> submeshes{ 0 };
		std::atomic<std::uint64_t> clodCacheHits{ 0 };
		std::atomic<std::uint64_t> clodCacheMisses{ 0 };
		std::atomic<std::uint64_t> loadGeomMs{ 0 };
		std::atomic<std::uint64_t> clodBuildMs{ 0 };
		std::atomic<std::uint64_t> clodSaveMs{ 0 };
		std::atomic<std::uint64_t> clodReloadMs{ 0 };
	};

	AtomicBenchmarkStats g_benchmarkStats;

	br::import::RenderablePrototypeGeometry BuildPrototypeGeometry(
		const std::vector<std::byte>& rawData,
		unsigned int vertexSize,
		unsigned int vertexFlags,
		const std::vector<UINT32>& indices)
	{
		br::import::RenderablePrototypeGeometry geometry;
		geometry.vertexFlags = vertexFlags;
		geometry.indices.assign(indices.begin(), indices.end());
		if (vertexSize == 0u) {
			return geometry;
		}

		const size_t vertexCount = rawData.size() / static_cast<size_t>(vertexSize);
		geometry.vertices.reserve(vertexCount);
		for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
			const std::byte* vertexBytes = rawData.data() + vertexIndex * static_cast<size_t>(vertexSize);
			br::import::RenderablePrototypeVertex vertex{};
			std::memcpy(std::addressof(vertex.position), vertexBytes + MeshVertexLayout::PositionOffset, sizeof(vertex.position));
			if ((vertexFlags & VertexFlags::VERTEX_NORMALS) != 0u) {
				std::memcpy(std::addressof(vertex.normal), vertexBytes + MeshVertexLayout::NormalOffset, sizeof(vertex.normal));
			}
			if ((vertexFlags & VertexFlags::VERTEX_TEXCOORDS) != 0u) {
				std::memcpy(std::addressof(vertex.uv), vertexBytes + MeshVertexLayout::TexcoordOffset(vertexFlags), sizeof(vertex.uv));
			}
			if ((vertexFlags & VertexFlags::VERTEX_TANGENTS) != 0u) {
				std::memcpy(std::addressof(vertex.tangent), vertexBytes + MeshVertexLayout::TangentOffset(vertexFlags), sizeof(vertex.tangent));
			}
			if ((vertexFlags & VertexFlags::VERTEX_COLORS) != 0u) {
				DirectX::XMFLOAT3 color{};
				std::memcpy(std::addressof(color), vertexBytes + MeshVertexLayout::ColorOffset(vertexFlags), sizeof(color));
				vertex.color = DirectX::XMFLOAT4{ color.x, color.y, color.z, 1.0f };
			}
			geometry.vertices.push_back(vertex);
		}
		return geometry;
	}

	float MedianPositive(std::vector<float> values)
	{
		values.erase(
			std::remove_if(values.begin(), values.end(), [](float v) { return !std::isfinite(v) || v <= 0.0f; }),
			values.end());
		if (values.empty()) {
			return 0.0f;
		}
		const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2u);
		std::nth_element(values.begin(), middle, values.end());
		return *middle;
	}

	float EstimateObjectSurfaceTexelDensity(
		const std::vector<std::byte>& vertices,
		unsigned int vertexStrideBytes,
		const std::vector<UINT32>& indices,
		const std::vector<MeshUvSetData>& uvSets)
	{
		if (vertexStrideBytes == 0u || uvSets.empty()) {
			return 1.0f;
		}

		const std::size_t vertexCount = vertices.size() / static_cast<std::size_t>(vertexStrideBytes);
		if (uvSets.front().values.size() < vertexCount) {
			return 1.0f;
		}

		auto readPosition = [&](std::uint32_t index) {
			DirectX::XMFLOAT3 position{};
			std::memcpy(
				std::addressof(position),
				vertices.data() + static_cast<std::size_t>(index) * vertexStrideBytes + MeshVertexLayout::PositionOffset,
				sizeof(position));
			return position;
		};
		auto sub3 = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
			return DirectX::XMFLOAT3{ a.x - b.x, a.y - b.y, a.z - b.z };
		};
		auto lengthCross = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
			const float x = a.y * b.z - a.z * b.y;
			const float y = a.z * b.x - a.x * b.z;
			const float z = a.x * b.y - a.y * b.x;
			return std::sqrt(x * x + y * y + z * z);
		};

		std::vector<float> densities;
		densities.reserve(indices.size() / 3u);
		for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
			const std::uint32_t i0 = indices[i + 0u];
			const std::uint32_t i1 = indices[i + 1u];
			const std::uint32_t i2 = indices[i + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
				continue;
			}

			const DirectX::XMFLOAT3 p0 = readPosition(i0);
			const DirectX::XMFLOAT3 p1 = readPosition(i1);
			const DirectX::XMFLOAT3 p2 = readPosition(i2);
			const float objectArea = 0.5f * lengthCross(sub3(p1, p0), sub3(p2, p0));
			if (!std::isfinite(objectArea) || objectArea <= 1.0e-12f) {
				continue;
			}

			const DirectX::XMFLOAT2 uv0 = uvSets.front().values[i0];
			const DirectX::XMFLOAT2 uv1 = uvSets.front().values[i1];
			const DirectX::XMFLOAT2 uv2 = uvSets.front().values[i2];
			const float du1 = uv1.x - uv0.x;
			const float dv1 = uv1.y - uv0.y;
			const float du2 = uv2.x - uv0.x;
			const float dv2 = uv2.y - uv0.y;
			const float uvArea = 0.5f * std::abs(du1 * dv2 - dv1 * du2);
			if (!std::isfinite(uvArea) || uvArea <= 1.0e-12f) {
				continue;
			}

			densities.push_back(std::sqrt(uvArea / objectArea));
		}

		const float density = MedianPositive(std::move(densities));
		return density > 0.0f ? density : 1.0f;
	}

	std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
	}

	void AddMs(std::atomic<std::uint64_t>& target, std::chrono::steady_clock::time_point begin)
	{
		target.fetch_add(ElapsedMs(begin, std::chrono::steady_clock::now()), std::memory_order_relaxed);
	}
}

void ResetBenchmarkStats()
{
	g_benchmarkStats.submeshes.store(0, std::memory_order_relaxed);
	g_benchmarkStats.clodCacheHits.store(0, std::memory_order_relaxed);
	g_benchmarkStats.clodCacheMisses.store(0, std::memory_order_relaxed);
	g_benchmarkStats.loadGeomMs.store(0, std::memory_order_relaxed);
	g_benchmarkStats.clodBuildMs.store(0, std::memory_order_relaxed);
	g_benchmarkStats.clodSaveMs.store(0, std::memory_order_relaxed);
	g_benchmarkStats.clodReloadMs.store(0, std::memory_order_relaxed);
}

BenchmarkStats GetBenchmarkStats()
{
	BenchmarkStats stats;
	stats.submeshes = g_benchmarkStats.submeshes.load(std::memory_order_relaxed);
	stats.clodCacheHits = g_benchmarkStats.clodCacheHits.load(std::memory_order_relaxed);
	stats.clodCacheMisses = g_benchmarkStats.clodCacheMisses.load(std::memory_order_relaxed);
	stats.loadGeomMs = g_benchmarkStats.loadGeomMs.load(std::memory_order_relaxed);
	stats.clodBuildMs = g_benchmarkStats.clodBuildMs.load(std::memory_order_relaxed);
	stats.clodSaveMs = g_benchmarkStats.clodSaveMs.load(std::memory_order_relaxed);
	stats.clodReloadMs = g_benchmarkStats.clodReloadMs.load(std::memory_order_relaxed);
	return stats;
}

std::optional<UsdSkelSkinningQuery> GetSkinningQuery(
	const UsdGeomMesh& mesh,
	const UsdSkelCache& skelCache)
{
	UsdSkelBindingAPI bindAPI(mesh.GetPrim());
	UsdSkelSkeleton skel = bindAPI.GetInheritedSkeleton();
	if (!skel)
		return std::nullopt;

	skelCache.Populate(UsdSkelRoot(skel.GetPrim()), UsdPrimDefaultPredicate);
	VtTokenArray skelJoints, meshJoints;
	skel.GetJointsAttr().Get(&skelJoints);
	bindAPI.GetJointsAttr().Get(&meshJoints);

	UsdSkelSkinningQuery skinQ(
		mesh.GetPrim(),
		skelJoints,
		meshJoints,
		bindAPI.GetJointIndicesAttr(),
		bindAPI.GetJointWeightsAttr(),
		bindAPI.GetSkinningMethodAttr(),
		bindAPI.GetGeomBindTransformAttr(),
		bindAPI.GetJointsAttr(),
		bindAPI.GetBlendShapesAttr(),
		bindAPI.GetBlendShapeTargetsRel());
	return skinQ;
}

MeshPreprocessResult ExtractSubMeshGroup(
	const UsdGeomMesh& mesh,
	const std::vector<UsdGeomSubset>& subsets,
	const UsdStageRefPtr& stage,
	UsdTimeCode geomTimeCode,
	double metersPerUnit,
	const std::vector<std::string>& requiredUvSetNames,
	const std::optional<UsdSkelSkinningQuery>& skinQ,
	const VtTokenArray& skelJointOrderRaw,
	const VtTokenArray& skelJointOrderMapped,
	bool doubleSidedVoxelSourceNormals,
	const std::string& sourceIdentifierOverride,
	std::uint32_t tessellationFactor,
	const ExtractOptions& options)
{
	std::string subsetName;
	if (!subsets.empty()) {
		for (const UsdGeomSubset& subset : subsets) {
			if (!subsetName.empty()) {
				subsetName += "+";
			}
			subsetName += subset.GetPrim().GetName().GetString();
		}
	}

	auto cacheIdentity = CLodCacheLoader::BuildIdentity(mesh, stage, subsetName, geomTimeCode, sourceIdentifierOverride);
	if (subsets.size() > 1u) {
		cacheIdentity.sourceIdentifier += "#combined_usd_subsets=" + std::to_string(subsets.size());
	}
	if (tessellationFactor > 1u) {
		cacheIdentity.sourceIdentifier += "#usd_tessellation_factor=" + std::to_string(tessellationFactor);
	}
	if (options.vertexAlphaCutoff.has_value()) {
		cacheIdentity.sourceIdentifier += "#usd_vertex_alpha_cutoff=" + std::to_string(options.vertexAlphaCutoff.value());
	}
	if (options.brniflyVertexAlpha) {
		cacheIdentity.sourceIdentifier += "#brnifly_vertex_alpha=1";
	}
	if (!options.brniflyZBufferWrite) {
		cacheIdentity.sourceIdentifier += "#brnifly_zbuffer_write=0";
	}
	if (options.brniflyDecal) {
		cacheIdentity.sourceIdentifier += "#brnifly_decal=1";
	}
	if (options.brniflyDynamicDecal) {
		cacheIdentity.sourceIdentifier += "#brnifly_dynamic_decal=1";
	}
	if (options.brniflyModelSpaceNormals) {
		cacheIdentity.sourceIdentifier += "#brnifly_model_space_normals=1";
	}
	if (options.objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::None) {
		cacheIdentity.sourceIdentifier += "#object_surface_sampling=" +
			std::to_string(static_cast<std::uint32_t>(options.objectSurfaceSamplingMode));
		cacheIdentity.sourceIdentifier += "#object_surface_sampling_config=" + options.objectSurfaceSamplingConfigHash;
	}
	if (options.objectSurfaceUseTriplanarProjection) {
		cacheIdentity.sourceIdentifier += "#object_surface_triplanar_projection=1";
		cacheIdentity.sourceIdentifier += "#object_surface_sampling_config=" + options.objectSurfaceSamplingConfigHash;
	}
	if (options.objectSurfaceUseTripleTapStochastic) {
		cacheIdentity.sourceIdentifier += "#object_surface_triple_tap_stochastic=1";
		cacheIdentity.sourceIdentifier += "#object_surface_sampling_config=" + options.objectSurfaceSamplingConfigHash;
	}
	cacheIdentity.doubleSidedVoxelSourceNormals = doubleSidedVoxelSourceNormals;
	spdlog::debug("    ExtractSubMesh: prim='{}' subset='{}' source='{}'",
		cacheIdentity.primPath, subsetName, cacheIdentity.sourceIdentifier);
	spdlog::debug("    Geometry sample time for prim='{}' is {}",
		cacheIdentity.primPath,
		geomTimeCode.IsDefault() ? -1.0 : geomTimeCode.GetValue());

	g_benchmarkStats.submeshes.fetch_add(1, std::memory_order_relaxed);
	const auto cacheLoadBegin = std::chrono::steady_clock::now();
	std::optional<ClusterLODPrebuiltData> prebuiltData;
	prebuiltData = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);
	AddMs(g_benchmarkStats.clodReloadMs, cacheLoadBegin);
	if (prebuiltData.has_value()) {
		g_benchmarkStats.clodCacheHits.fetch_add(1, std::memory_order_relaxed);
	}
	else {
		g_benchmarkStats.clodCacheMisses.fetch_add(1, std::memory_order_relaxed);
	}
	if (prebuiltData.has_value())
		spdlog::debug("    Cache HIT for prim='{}' subset='{}'", cacheIdentity.primPath, subsetName);
	else
		spdlog::debug("    Cache MISS for prim='{}' subset='{}' — will build", cacheIdentity.primPath, subsetName);

	// Load raw geometry
	std::unique_ptr<std::vector<std::byte>> rawData;
	std::optional<std::unique_ptr<std::vector<std::byte>>> skinningData;
	unsigned int vertexSize = 0;
	unsigned int skinningVertexSize = 0;
	std::vector<UINT32> indices;
	unsigned int vertexFlags = 0;
    std::vector<MeshUvSetData> uvSets;
	const auto loadGeomBegin = std::chrono::steady_clock::now();
	LoadGeom(rawData, skinningData, vertexSize, skinningVertexSize,
		indices, vertexFlags, uvSets, mesh, subsets, geomTimeCode, metersPerUnit, requiredUvSetNames,
		skinQ, skelJointOrderRaw, skelJointOrderMapped, options);
	TessellateExtractedTriangles(
		rawData,
		skinningData,
		vertexSize,
		skinningVertexSize,
		indices,
		vertexFlags,
		uvSets,
		tessellationFactor,
		mesh.GetPrim().GetPath().GetString());
	AddMs(g_benchmarkStats.loadGeomMs, loadGeomBegin);

	ObjectSurfaceSamplingMode objectSurfaceSamplingMode = options.objectSurfaceSamplingMode;
	float objectSurfaceTexelDensity = rawData
		? EstimateObjectSurfaceTexelDensity(*rawData, vertexSize, indices, uvSets)
		: 1.0f;
	if (!std::isfinite(objectSurfaceTexelDensity) || objectSurfaceTexelDensity <= 0.0f) {
		objectSurfaceTexelDensity = 1.0f;
		if (objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::TriplanarStochastic ||
			objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::AtlasBakedHeight) {
			spdlog::warn(
				"Object Reyes surface sampling for prim='{}' subset='{}' could not estimate texel density; using 1.0.",
				cacheIdentity.primPath,
				subsetName);
		}
	}
	if (objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::TriplanarStochastic) {
		spdlog::info(
			"Object Reyes tri-planar stochastic sampling enabled for prim='{}' subset='{}' density={}.",
			cacheIdentity.primPath,
			subsetName,
			objectSurfaceTexelDensity);
	}
	else if (objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::AtlasBakedHeight) {
		spdlog::info(
			"Object Reyes atlas-baked height sampling enabled for prim='{}' subset='{}' density={} triplanarProjection={} tripleTapStochastic={}.",
			cacheIdentity.primPath,
			subsetName,
			objectSurfaceTexelDensity,
			options.objectSurfaceUseTriplanarProjection,
			options.objectSurfaceUseTripleTapStochastic);
	}

	const size_t loadedVertCount = rawData ? (rawData->size() / static_cast<size_t>(vertexSize > 0 ? vertexSize : 1)) : 0;
	spdlog::info("    LoadGeom done: {} verts, {} indices, vertexSize={}, flags=0x{:X}",
		loadedVertCount, indices.size(), vertexSize, vertexFlags);
	auto prototypeGeometry = rawData
		? BuildPrototypeGeometry(*rawData, vertexSize, vertexFlags, indices)
		: br::import::RenderablePrototypeGeometry{};

	// Populate MeshIngestBuilder
	ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings();
	builderSettings.doubleSidedVoxelSourceNormals = doubleSidedVoxelSourceNormals;
	MeshIngestBuilder ingest(vertexSize,
		(skinningData && *skinningData) ? skinningVertexSize : 0,
		vertexFlags, builderSettings);
    ingest.SetUvSets(std::move(uvSets));

	const size_t vertexCount = rawData->size() / static_cast<size_t>(vertexSize);
	ingest.ReserveVertices(vertexCount);
	for (size_t v = 0; v < vertexCount; ++v) {
		const std::byte* vb = rawData->data() + v * static_cast<size_t>(vertexSize);
		ingest.AppendVertexBytes(vb, vertexSize);
	}

	if (skinningData && *skinningData) {
		const size_t skinVertCount = (*skinningData)->size() / static_cast<size_t>(skinningVertexSize);
		ingest.ReserveVertices(skinVertCount);
		for (size_t v = 0; v < skinVertCount; ++v) {
			const std::byte* sb = (*skinningData)->data() + v * static_cast<size_t>(skinningVertexSize);
			ingest.AppendSkinningVertexBytes(sb, skinningVertexSize);
		}
	}

	ingest.ReserveIndices(indices.size());
	ingest.AppendIndices(indices.data(), indices.size());

	// Build CLod cache if needed
	if (!prebuiltData.has_value()) {
		spdlog::info("    Building CLod artifacts...");
		const auto clodBuildBegin = std::chrono::steady_clock::now();
		ClusterLODPrebuildArtifacts artifacts = ingest.BuildClusterLODArtifacts();
		AddMs(g_benchmarkStats.clodBuildMs, clodBuildBegin);
		ClusterLODPrebuiltData savedPrebuiltData;
		spdlog::info("    CLod artifacts built: {} groups, {} nodes",
			artifacts.prebuiltData.groups.size(), artifacts.prebuiltData.nodes.size());

		spdlog::info("    Saving cache to disk...");
		const auto clodSaveBegin = std::chrono::steady_clock::now();
		if (CLodCacheLoader::SavePrebuiltLocked(cacheIdentity, artifacts.prebuiltData,
			artifacts.cacheBuildData.AsPayload(), &savedPrebuiltData))
		{
			AddMs(g_benchmarkStats.clodSaveMs, clodSaveBegin);
			spdlog::info("    Cache SAVED successfully.");
			const auto clodReloadBegin = std::chrono::steady_clock::now();
			auto diskBackedPrebuilt = CLodCacheLoader::TryLoadPrebuilt(cacheIdentity);
			AddMs(g_benchmarkStats.clodReloadMs, clodReloadBegin);
			if (diskBackedPrebuilt.has_value())
				prebuiltData = std::move(diskBackedPrebuilt);
			else {
				spdlog::warn("    Cache reload missed immediately after save for prim='{}' subset='{}'; using saved disk metadata directly.",
					cacheIdentity.primPath,
					subsetName);
				prebuiltData = std::move(savedPrebuiltData);
			}
		}
		else {
			spdlog::warn("    Cache save FAILED — using in-memory artifacts only.");
			prebuiltData = std::move(artifacts.prebuiltData);
		}
	}

	TfToken subdivisionScheme = UsdGeomTokens->catmullClark;
	mesh.GetSubdivisionSchemeAttr().Get(&subdivisionScheme);
	const bool previewSubdiv = (subdivisionScheme != UsdGeomTokens->none);
	const bool previewTopology =
		HasMultipleAuthoredTimeSamples(mesh.GetFaceVertexCountsAttr()) ||
		HasMultipleAuthoredTimeSamples(mesh.GetFaceVertexIndicesAttr()) ||
		HasMultipleAuthoredTimeSamples(mesh.GetHoleIndicesAttr());

	MeshPreprocessResult result(
		std::move(ingest),
		std::move(cacheIdentity),
		std::move(prebuiltData),
		previewSubdiv || previewTopology,
		std::move(prototypeGeometry));
	result.geometricDisplacementOptIn = options.geometricDisplacementOptIn;
	result.objectSurfaceSamplingMode = objectSurfaceSamplingMode;
	result.objectSurfaceUseTriplanarProjection = options.objectSurfaceUseTriplanarProjection;
	result.objectSurfaceUseTripleTapStochastic = options.objectSurfaceUseTripleTapStochastic;
	result.objectSurfaceTexelDensity = objectSurfaceTexelDensity;
	return result;
}

MeshPreprocessResult ExtractSubMesh(
	const UsdGeomMesh& mesh,
	const std::optional<UsdGeomSubset>& subset,
	const UsdStageRefPtr& stage,
	UsdTimeCode geomTimeCode,
	double metersPerUnit,
	const std::vector<std::string>& requiredUvSetNames,
	const std::optional<UsdSkelSkinningQuery>& skinQ,
	const VtTokenArray& skelJointOrderRaw,
	const VtTokenArray& skelJointOrderMapped,
	bool doubleSidedVoxelSourceNormals,
	const std::string& sourceIdentifierOverride,
	std::uint32_t tessellationFactor,
	const ExtractOptions& options)
{
	std::vector<UsdGeomSubset> subsets;
	if (subset) {
		subsets.push_back(*subset);
	}
	return ExtractSubMeshGroup(
		mesh,
		subsets,
		stage,
		geomTimeCode,
		metersPerUnit,
		requiredUvSetNames,
		skinQ,
		skelJointOrderRaw,
		skelJointOrderMapped,
		doubleSidedVoxelSourceNormals,
		sourceIdentifierOverride,
		tessellationFactor,
		options);
}

StageExtractionResult ExtractAllFromStage(
	const UsdStageRefPtr& stage,
	const std::string& sourceIdentifier,
	std::uint32_t tessellationFactor) {
	StageExtractionResult result;

	if (!stage) {
		spdlog::error("  USD stage extraction received a null stage.");
		return result;
	}
	spdlog::info("  USD stage ready for extraction. sourceIdentifier='{}'", sourceIdentifier);

	double metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
	spdlog::info("  metersPerUnit = {}", metersPerUnit);

	UsdSkelCache skelCache;

	struct MeshWorkItem {
		UsdGeomMesh mesh;
		std::optional<UsdSkelSkinningQuery> skinQ;
		VtTokenArray skelJointOrderRaw;
		VtTokenArray skelJointOrderMapped;
		std::vector<UsdGeomSubset> subsets;
		std::string primPath;
		bool doubleSided = false;
	};

	size_t totalPrims = 0;
	std::vector<MeshWorkItem> meshWorkItems;
	const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);
	auto primRange = UsdPrimRange(stage->GetPseudoRoot());
	for (auto primIt = primRange.begin(); primIt != primRange.end(); ++primIt) {
		++totalPrims;
		UsdGeomMesh mesh(*primIt);
		if (!mesh)
			continue;

		bool doubleSided = false;
		UsdGeomGprim gprim(mesh.GetPrim());
		if (gprim) {
			gprim.GetDoubleSidedAttr().Get(&doubleSided, geomTimeCode);
		}

		// Attempt skinning query TODO: CLod skinning
		auto skinQ = GetSkinningQuery(mesh, skelCache);
		VtTokenArray skelJointOrderRaw, skelJointOrderMapped;

		if (skinQ) {
			UsdSkelBindingAPI bindAPI(mesh.GetPrim());
			UsdSkelSkeleton skel = bindAPI.GetInheritedSkeleton();
			if (skel) {
				skelCache.Populate(UsdSkelRoot(skel.GetPrim()), UsdPrimDefaultPredicate);
				auto skelQuery = skelCache.GetSkelQuery(skel);
				skelJointOrderRaw = skelQuery.GetJointOrder();

				auto& mapper = skinQ->GetJointMapper();
				if (mapper && !mapper->IsIdentity())
					mapper->Remap(skelJointOrderRaw, &skelJointOrderMapped);
				else
					skelJointOrderMapped = skelJointOrderRaw;
			}
		}

		// Determine subsets
		UsdShadeMaterialBindingAPI bindAPI(mesh);
		auto subsets = bindAPI.GetMaterialBindSubsets();
		spdlog::info("    {} material subset(s) for mesh '{}'",
			subsets.size(), mesh.GetPrim().GetPath().GetString());

		meshWorkItems.push_back(MeshWorkItem{
			.mesh = mesh,
			.skinQ = std::move(skinQ),
			.skelJointOrderRaw = std::move(skelJointOrderRaw),
			.skelJointOrderMapped = std::move(skelJointOrderMapped),
			.subsets = std::move(subsets),
			.primPath = mesh.GetPrim().GetPath().GetString(),
			.doubleSided = doubleSided
			});
	}

	spdlog::info("  Stage has {} total prims.", totalPrims);
	result.meshesProcessed = meshWorkItems.size();

	const std::vector<std::string> requiredUvSetNames = { "st" };
	std::mutex resultMutex;

	TaskSchedulerManager::GetInstance().ParallelFor("USDGeometryExtractor::PreprocessMeshes", meshWorkItems.size(), [&](size_t meshIndex) {
		const MeshWorkItem& workItem = meshWorkItems[meshIndex];
		spdlog::info("  Found mesh #{}: '{}'", meshIndex + 1, workItem.primPath);

		if (workItem.subsets.empty()) {
			MeshPreprocessResult submesh = ExtractSubMesh(workItem.mesh, std::nullopt, stage, geomTimeCode, metersPerUnit,
				requiredUvSetNames, workItem.skinQ, workItem.skelJointOrderRaw, workItem.skelJointOrderMapped,
				workItem.doubleSided, sourceIdentifier, tessellationFactor);
			std::lock_guard lock(resultMutex);
			result.submeshes.push_back(std::move(submesh));
		}
		else {
			TaskSchedulerManager::GetInstance().ParallelFor("USDGeometryExtractor::PreprocessSubsets", workItem.subsets.size(), [&](size_t subsetIndex) {
				MeshPreprocessResult submesh = ExtractSubMesh(workItem.mesh, std::make_optional(workItem.subsets[subsetIndex]), stage, geomTimeCode, metersPerUnit,
					requiredUvSetNames, workItem.skinQ, workItem.skelJointOrderRaw, workItem.skelJointOrderMapped,
					workItem.doubleSided, sourceIdentifier, tessellationFactor);
				std::lock_guard lock(resultMutex);
				result.submeshes.push_back(std::move(submesh));
				});
		}
		});

	for (const MeshWorkItem& workItem : meshWorkItems) {
		result.submeshesProcessed += workItem.subsets.empty() ? 1 : workItem.subsets.size();
	}
	result.cachesBuilt = result.submeshesProcessed;

	return result;
}

StageExtractionResult ExtractAll(const std::string& filePath) {
	StageExtractionResult result;

	spdlog::info("  USD ExtractAll: opening stage '{}'", filePath);
	UsdStageRefPtr stage = UsdStage::Open(filePath);
	if (!stage) {
		spdlog::error("  USD stage open FAILED for '{}'", filePath);
		return result;
	}
	spdlog::info("  USD stage opened successfully.");

	return ExtractAllFromStage(stage);
}

} // namespace USDGeometryExtractor
