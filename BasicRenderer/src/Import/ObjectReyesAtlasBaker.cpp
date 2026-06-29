#include "Import/ObjectReyesAtlasBaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#include <DirectXMath.h>
#include <xatlas.h>

#include "Mesh/VertexLayout.h"

namespace br::import {
namespace {

struct PositionKey {
	std::array<std::uint32_t, 3> bits{};

	bool operator==(const PositionKey& rhs) const noexcept {
		return bits == rhs.bits;
	}
};

struct PositionKeyHash {
	std::size_t operator()(const PositionKey& key) const noexcept {
		std::size_t h = 1469598103934665603ull;
		for (std::uint32_t value : key.bits) {
			h ^= static_cast<std::size_t>(value);
			h *= 1099511628211ull;
		}
		return h;
	}
};

struct TriangleKey {
	std::array<std::uint32_t, 3> indices{};

	bool operator==(const TriangleKey& rhs) const noexcept {
		return indices == rhs.indices;
	}
};

struct TriangleKeyHash {
	std::size_t operator()(const TriangleKey& key) const noexcept {
		std::size_t h = 1469598103934665603ull;
		for (std::uint32_t value : key.indices) {
			h ^= static_cast<std::size_t>(value);
			h *= 1099511628211ull;
		}
		return h;
	}
};

DirectX::XMFLOAT3 LoadFloat3(const std::byte* bytes, std::size_t offset)
{
	DirectX::XMFLOAT3 v{};
	std::memcpy(&v, bytes + offset, sizeof(v));
	return v;
}

DirectX::XMFLOAT2 LoadFloat2(const std::byte* bytes, std::size_t offset)
{
	DirectX::XMFLOAT2 v{};
	std::memcpy(&v, bytes + offset, sizeof(v));
	return v;
}

bool IsFinite(const DirectX::XMFLOAT2& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y);
}

bool IsFinite(const DirectX::XMFLOAT3& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

TriangleKey MakeTriangleKey(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
	return TriangleKey{ { a, b, c } };
}

bool TryFindTriangleMaterial(
	const std::unordered_map<TriangleKey, std::uint32_t, TriangleKeyHash>& lookup,
	std::uint32_t a,
	std::uint32_t b,
	std::uint32_t c,
	std::uint32_t& outMaterial)
{
	const TriangleKey variants[6] = {
		MakeTriangleKey(a, b, c),
		MakeTriangleKey(b, c, a),
		MakeTriangleKey(c, a, b),
		MakeTriangleKey(a, c, b),
		MakeTriangleKey(c, b, a),
		MakeTriangleKey(b, a, c)
	};
	for (const TriangleKey& key : variants) {
		if (const auto it = lookup.find(key); it != lookup.end()) {
			outMaterial = it->second;
			return true;
		}
	}
	return false;
}

PositionKey MakePositionKey(const std::byte* vertex)
{
	PositionKey key{};
	std::memcpy(key.bits.data(), vertex + MeshVertexLayout::PositionOffset, sizeof(float) * 3u);
	return key;
}

DirectX::XMFLOAT3 NormalizeOrFallback(const DirectX::XMFLOAT3& n)
{
	const float lenSq = n.x * n.x + n.y * n.y + n.z * n.z;
	if (lenSq <= 1.0e-20f || !std::isfinite(lenSq)) {
		return { 0.0f, 0.0f, 1.0f };
	}
	const float invLen = 1.0f / std::sqrt(lenSq);
	return { n.x * invLen, n.y * invLen, n.z * invLen };
}

void AddNormal(DirectX::XMFLOAT3& dst, const DirectX::XMFLOAT3& n)
{
	dst.x += n.x;
	dst.y += n.y;
	dst.z += n.z;
}

void RecomputeExactPositionWeldedNormals(
	std::vector<std::byte>& vertices,
	std::uint32_t vertexSize,
	std::span<const std::uint32_t> indices)
{
	if (vertexSize < MeshVertexLayout::NormalOffset + sizeof(DirectX::XMFLOAT3) || vertices.empty()) {
		return;
	}

	const std::size_t vertexCount = vertices.size() / vertexSize;
	std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> weldLookup;
	std::vector<std::uint32_t> vertexToWeld(vertexCount, 0u);
	std::vector<DirectX::XMFLOAT3> weldedNormals;
	for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
		const PositionKey key = MakePositionKey(vertices.data() + vertexIndex * vertexSize);
		auto [it, inserted] = weldLookup.emplace(key, static_cast<std::uint32_t>(weldedNormals.size()));
		if (inserted) {
			weldedNormals.push_back({ 0.0f, 0.0f, 0.0f });
		}
		vertexToWeld[vertexIndex] = it->second;
	}

	for (std::size_t index = 0; index + 2u < indices.size(); index += 3u) {
		const std::uint32_t i0 = indices[index + 0u];
		const std::uint32_t i1 = indices[index + 1u];
		const std::uint32_t i2 = indices[index + 2u];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
			continue;
		}
		const auto p0 = LoadFloat3(vertices.data() + static_cast<std::size_t>(i0) * vertexSize, MeshVertexLayout::PositionOffset);
		const auto p1 = LoadFloat3(vertices.data() + static_cast<std::size_t>(i1) * vertexSize, MeshVertexLayout::PositionOffset);
		const auto p2 = LoadFloat3(vertices.data() + static_cast<std::size_t>(i2) * vertexSize, MeshVertexLayout::PositionOffset);
		const DirectX::XMFLOAT3 e1{ p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
		const DirectX::XMFLOAT3 e2{ p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
		const DirectX::XMFLOAT3 n{
			e1.y * e2.z - e1.z * e2.y,
			e1.z * e2.x - e1.x * e2.z,
			e1.x * e2.y - e1.y * e2.x
		};
		AddNormal(weldedNormals[vertexToWeld[i0]], n);
		AddNormal(weldedNormals[vertexToWeld[i1]], n);
		AddNormal(weldedNormals[vertexToWeld[i2]], n);
	}

	for (DirectX::XMFLOAT3& normal : weldedNormals) {
		normal = NormalizeOrFallback(normal);
	}
	for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
		const DirectX::XMFLOAT3 n = weldedNormals[vertexToWeld[vertexIndex]];
		std::memcpy(vertices.data() + vertexIndex * vertexSize + MeshVertexLayout::NormalOffset, &n, sizeof(n));
	}
}

} // namespace

ObjectReyesAtlasBakeResult BuildObjectReyesAtlasBakedHeightMesh(
	std::span<const ObjectReyesAtlasSourceMesh> meshes,
	const ObjectReyesAtlasBakeOptions& options,
	std::string_view debugName)
{
	ObjectReyesAtlasBakeResult result{};
	if (meshes.empty()) {
		result.error = "no source meshes";
		return result;
	}

	const auto fail = [&](std::string message) {
		result.success = false;
		result.error = std::move(message);
		return result;
	};

	const std::uint32_t vertexSize = meshes.front().vertexSize;
	const std::uint32_t vertexFlags = meshes.front().vertexFlags;
	const std::size_t uvSetCount = meshes.front().uvSets ? meshes.front().uvSets->size() : 0u;
	if (vertexSize < MeshVertexLayout::NormalOffset + sizeof(DirectX::XMFLOAT3)) {
		return fail("missing position/normal streams");
	}
	if (meshes.front().vertices == nullptr || meshes.front().indices == nullptr || meshes.front().uvSets == nullptr) {
		return fail("missing source mesh data");
	}

	std::size_t totalVertexCount = 0;
	std::size_t totalIndexCount = 0;
	for (const ObjectReyesAtlasSourceMesh& mesh : meshes) {
		if (mesh.vertices == nullptr || mesh.indices == nullptr || mesh.uvSets == nullptr) {
			return fail("missing source mesh data");
		}
		if (mesh.vertexSize != vertexSize || mesh.vertexFlags != vertexFlags || mesh.uvSets->size() != uvSetCount) {
			return fail("incompatible sibling vertex layouts");
		}
		if (mesh.vertices->size() % vertexSize != 0u || mesh.indices->size() % 3u != 0u) {
			return fail("malformed source mesh buffers");
		}
		for (const MeshUvSetData& uvSet : *mesh.uvSets) {
			if (uvSet.values.size() != mesh.vertices->size() / vertexSize) {
				return fail("source UV set vertex count mismatch");
			}
		}
		totalVertexCount += mesh.vertices->size() / vertexSize;
		totalIndexCount += mesh.indices->size();
	}
	if (totalVertexCount == 0u || totalIndexCount < 3u) {
		return fail("empty source geometry");
	}
	if (totalVertexCount > std::numeric_limits<std::uint32_t>::max()) {
		return fail("source geometry has too many vertices for xatlas UInt32 indices");
	}

	std::vector<std::byte> inputVertices;
	std::vector<std::uint32_t> inputIndices;
	std::vector<MeshUvSetData> inputUvSets = *meshes.front().uvSets;
	std::vector<std::uint32_t> inputTriangleMaterials;
	inputVertices.reserve(totalVertexCount * vertexSize);
	inputIndices.reserve(totalIndexCount);
	inputTriangleMaterials.reserve(totalIndexCount / 3u);
	for (MeshUvSetData& uvSet : inputUvSets) {
		uvSet.values.clear();
		uvSet.values.reserve(totalVertexCount);
	}

	std::uint32_t vertexBase = 0u;
	for (const ObjectReyesAtlasSourceMesh& mesh : meshes) {
		const std::size_t sourceVertexCount = mesh.vertices->size() / vertexSize;
		inputVertices.insert(inputVertices.end(), mesh.vertices->begin(), mesh.vertices->end());
		for (std::uint32_t index : *mesh.indices) {
			if (index >= sourceVertexCount) {
				return fail("source index out of range");
			}
			inputIndices.push_back(vertexBase + index);
		}
		for (std::size_t uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex) {
			inputUvSets[uvSetIndex].values.insert(
				inputUvSets[uvSetIndex].values.end(),
				(*mesh.uvSets)[uvSetIndex].values.begin(),
				(*mesh.uvSets)[uvSetIndex].values.end());
		}
		inputTriangleMaterials.insert(
			inputTriangleMaterials.end(),
			mesh.indices->size() / 3u,
			mesh.materialIndex);
		vertexBase += static_cast<std::uint32_t>(sourceVertexCount);
	}

	std::vector<DirectX::XMFLOAT3> positions(totalVertexCount);
	std::vector<DirectX::XMFLOAT3> normals(totalVertexCount);
	for (std::size_t vertexIndex = 0; vertexIndex < totalVertexCount; ++vertexIndex) {
		const std::byte* vertex = inputVertices.data() + vertexIndex * vertexSize;
		positions[vertexIndex] = LoadFloat3(vertex, MeshVertexLayout::PositionOffset);
		normals[vertexIndex] = LoadFloat3(vertex, MeshVertexLayout::NormalOffset);
		if (!IsFinite(positions[vertexIndex])) {
			return fail("non-finite source position");
		}
		if (!IsFinite(normals[vertexIndex])) {
			normals[vertexIndex] = { 0.0f, 0.0f, 1.0f };
		}
	}

	std::unordered_map<TriangleKey, std::uint32_t, TriangleKeyHash> sourceTriangleMaterials;
	sourceTriangleMaterials.reserve(inputIndices.size() / 3u);
	for (std::size_t triangleIndex = 0; triangleIndex + 2u < inputIndices.size() / 3u * 3u; triangleIndex += 3u) {
		const std::size_t faceIndex = triangleIndex / 3u;
		const std::uint32_t materialIndex = faceIndex < inputTriangleMaterials.size()
			? inputTriangleMaterials[faceIndex]
			: 0u;
		sourceTriangleMaterials.emplace(
			MakeTriangleKey(inputIndices[triangleIndex + 0u], inputIndices[triangleIndex + 1u], inputIndices[triangleIndex + 2u]),
			materialIndex);
	}

	xatlas::Atlas* atlas = xatlas::Create();
	xatlas::MeshDecl meshDecl{};
	meshDecl.vertexCount = static_cast<std::uint32_t>(positions.size());
	meshDecl.vertexPositionData = positions.data();
	meshDecl.vertexPositionStride = sizeof(DirectX::XMFLOAT3);
	meshDecl.vertexNormalData = normals.data();
	meshDecl.vertexNormalStride = sizeof(DirectX::XMFLOAT3);
	meshDecl.indexCount = static_cast<std::uint32_t>(inputIndices.size());
	meshDecl.indexData = inputIndices.data();
	meshDecl.indexFormat = xatlas::IndexFormat::UInt32;
	meshDecl.faceCount = meshDecl.indexCount / 3u;
	meshDecl.faceMaterialData = inputTriangleMaterials.data();
	meshDecl.epsilon = 1.0e-6f;

	const xatlas::AddMeshError addMeshError = xatlas::AddMesh(atlas, meshDecl, 1u);
	if (addMeshError != xatlas::AddMeshError::Success) {
		const char* errorText = xatlas::StringForEnum(addMeshError);
		std::string message = "xatlas AddMesh failed for '";
		message += debugName;
		message += "': ";
		message += errorText ? errorText : "<unknown>";
		xatlas::Destroy(atlas);
		return fail(std::move(message));
	}

	xatlas::ChartOptions chartOptions{};
	xatlas::PackOptions packOptions{};
	packOptions.bilinear = true;
	packOptions.blockAlign = true;
	packOptions.padding = options.paddingTexels;
	packOptions.maxChartSize = options.maxAtlasSize;
	packOptions.resolution = options.resolution;
	packOptions.texelsPerUnit = std::isfinite(options.texelsPerUnit) && options.texelsPerUnit > 0.0f
		? options.texelsPerUnit
		: 0.0f;
	xatlas::Generate(atlas, chartOptions, packOptions);

	if (atlas->meshCount != 1u || atlas->meshes == nullptr) {
		xatlas::Destroy(atlas);
		return fail("xatlas did not produce one output mesh");
	}
	const xatlas::Mesh& xaMesh = atlas->meshes[0];
	if (xaMesh.vertexCount == 0u || xaMesh.indexCount == 0u || xaMesh.vertexArray == nullptr || xaMesh.indexArray == nullptr) {
		xatlas::Destroy(atlas);
		return fail("xatlas produced empty output geometry");
	}
	if (atlas->width == 0u || atlas->height == 0u) {
		xatlas::Destroy(atlas);
		return fail("xatlas produced an empty atlas");
	}

	result.vertices.resize(static_cast<std::size_t>(xaMesh.vertexCount) * vertexSize);
	result.indices.assign(xaMesh.indexArray, xaMesh.indexArray + xaMesh.indexCount);
	result.uvSets = inputUvSets;
	for (MeshUvSetData& uvSet : result.uvSets) {
		uvSet.values.resize(xaMesh.vertexCount);
	}
	MeshUvSetData atlasUvSet{};
	atlasUvSet.name = "__object_reyes_atlas_height";
	atlasUvSet.values.resize(xaMesh.vertexCount);

	const float invAtlasWidth = 1.0f / static_cast<float>(atlas->width);
	const float invAtlasHeight = 1.0f / static_cast<float>(atlas->height);
	for (std::uint32_t vertexIndex = 0; vertexIndex < xaMesh.vertexCount; ++vertexIndex) {
		const xatlas::Vertex& xaVertex = xaMesh.vertexArray[vertexIndex];
		if (static_cast<std::size_t>(xaVertex.xref) >= totalVertexCount) {
			xatlas::Destroy(atlas);
			return fail("xatlas output vertex references invalid source vertex");
		}
		const std::size_t sourceVertexIndex = static_cast<std::size_t>(xaVertex.xref);
		std::memcpy(
			result.vertices.data() + static_cast<std::size_t>(vertexIndex) * vertexSize,
			inputVertices.data() + sourceVertexIndex * vertexSize,
			vertexSize);
		for (std::size_t uvSetIndex = 0; uvSetIndex < result.uvSets.size(); ++uvSetIndex) {
			result.uvSets[uvSetIndex].values[vertexIndex] = inputUvSets[uvSetIndex].values[sourceVertexIndex];
		}
		const DirectX::XMFLOAT2 uv{
			xaVertex.uv[0] * invAtlasWidth,
			xaVertex.uv[1] * invAtlasHeight
		};
		if (!IsFinite(uv)) {
			xatlas::Destroy(atlas);
			return fail("xatlas produced non-finite UVs");
		}
		atlasUvSet.values[vertexIndex] = uv;
	}

	result.atlasUvSetIndex = static_cast<std::uint32_t>(result.uvSets.size());
	result.uvSets.push_back(std::move(atlasUvSet));
	result.triangleMaterialIndices.resize(result.indices.size() / 3u, 0u);
	std::uint32_t xrefMaterialResolved = 0u;
	std::uint32_t xrefMaterialFallbacks = 0u;
	std::uint32_t orderedMaterialMismatches = 0u;
	for (std::size_t triangleIndex = 0; triangleIndex < result.triangleMaterialIndices.size(); ++triangleIndex) {
		const std::uint32_t i0 = result.indices[triangleIndex * 3u + 0u];
		const std::uint32_t i1 = result.indices[triangleIndex * 3u + 1u];
		const std::uint32_t i2 = result.indices[triangleIndex * 3u + 2u];
		std::uint32_t materialFromXref = 0u;
		const bool canResolveFromXref =
			i0 < xaMesh.vertexCount &&
			i1 < xaMesh.vertexCount &&
			i2 < xaMesh.vertexCount &&
			TryFindTriangleMaterial(
				sourceTriangleMaterials,
				xaMesh.vertexArray[i0].xref,
				xaMesh.vertexArray[i1].xref,
				xaMesh.vertexArray[i2].xref,
				materialFromXref);
		if (canResolveFromXref) {
			result.triangleMaterialIndices[triangleIndex] = materialFromXref;
			++xrefMaterialResolved;
			if (triangleIndex < inputTriangleMaterials.size() &&
				inputTriangleMaterials[triangleIndex] != materialFromXref) {
				++orderedMaterialMismatches;
			}
			continue;
		}
		if (triangleIndex < inputTriangleMaterials.size()) {
			result.triangleMaterialIndices[triangleIndex] = inputTriangleMaterials[triangleIndex];
		}
		++xrefMaterialFallbacks;
	}
	result.diagnostics = "xatlas triangle material mapping: xrefResolved=" + std::to_string(xrefMaterialResolved) +
		" fallback=" + std::to_string(xrefMaterialFallbacks) +
		" orderedMismatch=" + std::to_string(orderedMaterialMismatches);
	result.atlasWidth = atlas->width;
	result.atlasHeight = atlas->height;
	result.texelsPerUnit = atlas->texelsPerUnit > 0.0f ? atlas->texelsPerUnit : packOptions.texelsPerUnit;
	xatlas::Destroy(atlas);

	RecomputeExactPositionWeldedNormals(result.vertices, vertexSize, result.indices);
	result.success = true;
	return result;
}

} // namespace br::import
