#include "Import/MaterialUvRemapper.h"

#include "Mesh/VertexFlags.h"
#include "Mesh/VertexLayout.h"

#include <DirectXMath.h>
#include <Eigen/Core>
#include <Eigen/Sparse>
#include <Eigen/SparseQR>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <queue>
#include <string>
#include <unordered_map>

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

namespace br::import {
namespace {

constexpr std::uint32_t kAlgorithmVersion = 1u;
constexpr float kMinTriangleArea = 1.0e-8f;
constexpr float kMinUvArea = 1.0e-10f;
constexpr float kMaxFlipFraction = 0.005f;
constexpr double kAnchorWeight = 1024.0;

struct PositionKey {
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t z = 0;

	bool operator==(const PositionKey&) const = default;
};

struct PositionKeyHash {
	std::size_t operator()(const PositionKey& key) const noexcept
	{
		std::size_t h = static_cast<std::size_t>(key.x);
		h ^= static_cast<std::size_t>(key.y) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
		h ^= static_cast<std::size_t>(key.z) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
		return h;
	}
};

struct Constraint {
	std::uint32_t neighbor = 0;
	XMFLOAT2 delta{ 0.0f, 0.0f };
	float weight = 1.0f;
};

struct TriangleRecord {
	std::uint32_t vertex[3] = {};
	std::uint32_t weld[3] = {};
	float oldUvArea = 0.0f;
	float area3d = 0.0f;
};

std::uint32_t FloatBits(float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

PositionKey MakePositionKey(const XMFLOAT3& p)
{
	return { FloatBits(p.x), FloatBits(p.y), FloatBits(p.z) };
}

XMFLOAT3 ReadPosition(const std::vector<std::byte>& vertices, std::size_t stride, std::uint32_t index)
{
	XMFLOAT3 p{};
	std::memcpy(&p, vertices.data() + static_cast<std::size_t>(index) * stride + MeshVertexLayout::PositionOffset, sizeof(p));
	return p;
}

XMFLOAT3 ReadNormal(const std::vector<std::byte>& vertices, std::size_t stride, std::uint32_t index)
{
	XMFLOAT3 n{ 0.0f, 0.0f, 1.0f };
	std::memcpy(&n, vertices.data() + static_cast<std::size_t>(index) * stride + MeshVertexLayout::NormalOffset, sizeof(n));
	return n;
}

void WritePrimaryUv(std::vector<std::byte>& vertices, std::size_t stride, unsigned int vertexFlags, std::uint32_t index, const XMFLOAT2& uv)
{
	std::memcpy(
		vertices.data() + static_cast<std::size_t>(index) * stride + MeshVertexLayout::TexcoordOffset(vertexFlags),
		&uv,
		sizeof(uv));
}

void WriteTangent(std::vector<std::byte>& vertices, std::size_t stride, unsigned int vertexFlags, std::uint32_t index, const XMFLOAT4& tangent)
{
	std::memcpy(
		vertices.data() + static_cast<std::size_t>(index) * stride + MeshVertexLayout::TangentOffset(vertexFlags),
		&tangent,
		sizeof(tangent));
}

float Cross2(const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& c)
{
	const float ux = b.x - a.x;
	const float uy = b.y - a.y;
	const float vx = c.x - a.x;
	const float vy = c.y - a.y;
	return ux * vy - uy * vx;
}

XMFLOAT3 Sub3(const XMFLOAT3& a, const XMFLOAT3& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

XMFLOAT2 Sub2(const XMFLOAT2& a, const XMFLOAT2& b)
{
	return { a.x - b.x, a.y - b.y };
}

XMFLOAT3 Cross3(const XMFLOAT3& a, const XMFLOAT3& b)
{
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

float Dot3(const XMFLOAT3& a, const XMFLOAT3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Length3(const XMFLOAT3& v)
{
	return std::sqrt(std::max(0.0f, Dot3(v, v)));
}

float TriangleArea3D(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2)
{
	return 0.5f * Length3(Cross3(Sub3(p1, p0), Sub3(p2, p0)));
}

float UvAreaAbs(const XMFLOAT2& u0, const XMFLOAT2& u1, const XMFLOAT2& u2)
{
	return std::abs(Cross2(u0, u1, u2)) * 0.5f;
}

float Median(std::vector<float> values)
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

void AddConstraint(
	std::vector<std::vector<Constraint>>& constraints,
	std::uint32_t a,
	std::uint32_t b,
	XMFLOAT2 delta,
	float weight)
{
	if (a == b || weight <= 0.0f || !std::isfinite(delta.x) || !std::isfinite(delta.y)) {
		return;
	}

	constraints[a].push_back(Constraint{ b, delta, weight });
	constraints[b].push_back(Constraint{ a, XMFLOAT2{ -delta.x, -delta.y }, weight });
}

bool SolveLeastSquaresUv(
	const std::vector<std::vector<Constraint>>& constraints,
	const std::vector<XMFLOAT2>& initialUv,
	const std::vector<std::uint32_t>& anchors,
	std::vector<XMFLOAT2>& uv,
	std::string& errorMessage)
{
	using SparseMatrix = Eigen::SparseMatrix<double>;
	using Triplet = Eigen::Triplet<double>;

	const auto variableCount = static_cast<int>(constraints.size());
	if (variableCount <= 0) {
		errorMessage = "no welded vertices to solve";
		return false;
	}

	std::size_t directedConstraintCount = 0;
	for (const auto& vertexConstraints : constraints) {
		directedConstraintCount += vertexConstraints.size();
	}

	std::vector<std::uint8_t> hasConstraint(constraints.size(), 0);
	for (std::uint32_t vertexIndex = 0; vertexIndex < constraints.size(); ++vertexIndex) {
		if (!constraints[vertexIndex].empty()) {
			hasConstraint[vertexIndex] = 1;
		}
	}

	std::vector<std::uint8_t> isPinned(constraints.size(), 0);
	for (const std::uint32_t anchor : anchors) {
		if (anchor < isPinned.size()) {
			isPinned[anchor] = 1;
		}
	}
	for (std::uint32_t vertexIndex = 0; vertexIndex < constraints.size(); ++vertexIndex) {
		if (!hasConstraint[vertexIndex]) {
			isPinned[vertexIndex] = 1;
		}
	}

	std::size_t pinCount = 0;
	for (const std::uint8_t pinned : isPinned) {
		pinCount += pinned != 0u ? 1u : 0u;
	}

	const auto rowCount = static_cast<int>(directedConstraintCount + pinCount);
	if (rowCount < variableCount) {
		errorMessage = "underdetermined UV solve";
		return false;
	}

	std::vector<Triplet> triplets;
	triplets.reserve(directedConstraintCount * 2u + pinCount);
	Eigen::MatrixXd rhs(rowCount, 2);
	rhs.setZero();

	int row = 0;
	for (std::uint32_t vertexIndex = 0; vertexIndex < constraints.size(); ++vertexIndex) {
		for (const Constraint& constraint : constraints[vertexIndex]) {
			const double weight = static_cast<double>(constraint.weight);
			triplets.emplace_back(row, static_cast<int>(vertexIndex), -weight);
			triplets.emplace_back(row, static_cast<int>(constraint.neighbor), weight);
			rhs(row, 0) = static_cast<double>(constraint.delta.x) * weight;
			rhs(row, 1) = static_cast<double>(constraint.delta.y) * weight;
			++row;
		}
	}

	for (std::uint32_t vertexIndex = 0; vertexIndex < constraints.size(); ++vertexIndex) {
		if (!isPinned[vertexIndex]) {
			continue;
		}
		triplets.emplace_back(row, static_cast<int>(vertexIndex), kAnchorWeight);
		rhs(row, 0) = static_cast<double>(initialUv[vertexIndex].x) * kAnchorWeight;
		rhs(row, 1) = static_cast<double>(initialUv[vertexIndex].y) * kAnchorWeight;
		++row;
	}

	SparseMatrix system(rowCount, variableCount);
	system.setFromTriplets(triplets.begin(), triplets.end());
	system.makeCompressed();

	Eigen::SparseQR<SparseMatrix, Eigen::COLAMDOrdering<int>> solver;
	solver.compute(system);
	if (solver.info() != Eigen::Success) {
		errorMessage = "sparse QR factorization failed";
		return false;
	}

	const Eigen::MatrixXd solved = solver.solve(rhs);
	if (solver.info() != Eigen::Success || solved.rows() != variableCount || solved.cols() != 2) {
		errorMessage = "sparse QR solve failed";
		return false;
	}

	uv.resize(constraints.size());
	for (std::uint32_t vertexIndex = 0; vertexIndex < constraints.size(); ++vertexIndex) {
		const double u = solved(static_cast<int>(vertexIndex), 0);
		const double v = solved(static_cast<int>(vertexIndex), 1);
		if (!std::isfinite(u) || !std::isfinite(v)) {
			errorMessage = "sparse QR solve produced non-finite UVs";
			return false;
		}
		uv[vertexIndex] = XMFLOAT2{ static_cast<float>(u), static_cast<float>(v) };
	}

	return true;
}

void RecomputeTangents(
	std::vector<std::byte>& vertices,
	unsigned int vertexStrideBytes,
	unsigned int vertexFlags,
	const std::vector<std::uint32_t>& indices,
	const std::vector<MeshUvSetData>& uvSets,
	std::uint32_t uvSetIndex)
{
	if ((vertexFlags & VERTEX_TANGENTS) == 0u ||
		(vertexFlags & VERTEX_NORMALS) == 0u ||
		uvSetIndex >= uvSets.size() ||
		uvSets[uvSetIndex].values.size() != vertices.size() / vertexStrideBytes) {
		return;
	}

	const std::size_t vertexCount = vertices.size() / vertexStrideBytes;
	std::vector<XMFLOAT3> tangentSums(vertexCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });
	std::vector<XMFLOAT3> bitangentSums(vertexCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });

	for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
		const std::uint32_t i0 = indices[i + 0u];
		const std::uint32_t i1 = indices[i + 1u];
		const std::uint32_t i2 = indices[i + 2u];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
			continue;
		}

		const XMFLOAT3 p0 = ReadPosition(vertices, vertexStrideBytes, i0);
		const XMFLOAT3 p1 = ReadPosition(vertices, vertexStrideBytes, i1);
		const XMFLOAT3 p2 = ReadPosition(vertices, vertexStrideBytes, i2);
		const XMFLOAT2 uv0 = uvSets[uvSetIndex].values[i0];
		const XMFLOAT2 uv1 = uvSets[uvSetIndex].values[i1];
		const XMFLOAT2 uv2 = uvSets[uvSetIndex].values[i2];
		const XMFLOAT3 e1 = Sub3(p1, p0);
		const XMFLOAT3 e2 = Sub3(p2, p0);
		const XMFLOAT2 duv1 = Sub2(uv1, uv0);
		const XMFLOAT2 duv2 = Sub2(uv2, uv0);
		const float det = duv1.x * duv2.y - duv2.x * duv1.y;
		if (std::abs(det) < 1.0e-10f) {
			continue;
		}

		const float r = 1.0f / det;
		const XMFLOAT3 tangent{
			(e1.x * duv2.y - e2.x * duv1.y) * r,
			(e1.y * duv2.y - e2.y * duv1.y) * r,
			(e1.z * duv2.y - e2.z * duv1.y) * r
		};
		const XMFLOAT3 bitangent{
			(e2.x * duv1.x - e1.x * duv2.x) * r,
			(e2.y * duv1.x - e1.y * duv2.x) * r,
			(e2.z * duv1.x - e1.z * duv2.x) * r
		};

		for (const std::uint32_t index : { i0, i1, i2 }) {
			tangentSums[index].x += tangent.x;
			tangentSums[index].y += tangent.y;
			tangentSums[index].z += tangent.z;
			bitangentSums[index].x += bitangent.x;
			bitangentSums[index].y += bitangent.y;
			bitangentSums[index].z += bitangent.z;
		}
	}

	for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
		const XMFLOAT3 n = ReadNormal(vertices, vertexStrideBytes, vertexIndex);
		XMFLOAT3 t = tangentSums[vertexIndex];
		const float ndott = Dot3(n, t);
		t.x -= n.x * ndott;
		t.y -= n.y * ndott;
		t.z -= n.z * ndott;
		const float len = Length3(t);
		if (len <= 1.0e-8f) {
			t = std::abs(n.z) < 0.9f ? Cross3(XMFLOAT3{ 0.0f, 0.0f, 1.0f }, n) : Cross3(XMFLOAT3{ 1.0f, 0.0f, 0.0f }, n);
		}
		else {
			t.x /= len;
			t.y /= len;
			t.z /= len;
		}

		const XMFLOAT3 b = bitangentSums[vertexIndex];
		const XMFLOAT3 nCrossT = Cross3(n, t);
		const float sign = Dot3(nCrossT, b) < 0.0f ? -1.0f : 1.0f;
		WriteTangent(vertices, vertexStrideBytes, vertexFlags, vertexIndex, XMFLOAT4{ t.x, t.y, t.z, sign });
	}
}

} // namespace

MaterialUvRemapResult RemapMaterialUvSet(
	std::vector<std::byte>& vertices,
	unsigned int vertexStrideBytes,
	unsigned int vertexFlags,
	const std::vector<std::uint32_t>& indices,
	std::vector<MeshUvSetData>& uvSets,
	const MaterialUvRemapRequest& request)
{
	MaterialUvRemapResult result{};
	result.attempted = true;
	result.uvSetIndex = request.uvSetIndex;

	if (kAlgorithmVersion == 0u) {
		result.message = "invalid remap algorithm version";
		return result;
	}

	if (vertexStrideBytes == 0u || vertices.empty() || indices.empty()) {
		result.message = "empty geometry";
		return result;
	}
	if ((vertexFlags & VERTEX_TEXCOORDS) == 0u) {
		result.message = "mesh has no primary texcoord stream";
		return result;
	}
	if (request.uvSetIndex >= uvSets.size()) {
		result.message = "requested UV set is missing";
		return result;
	}

	const std::size_t vertexCount = vertices.size() / static_cast<std::size_t>(vertexStrideBytes);
	if (uvSets[request.uvSetIndex].values.size() != vertexCount) {
		result.message = "requested UV set does not match vertex count";
		return result;
	}
	if (vertexCount < 3u || indices.size() < 3u) {
		result.message = "not enough geometry";
		return result;
	}

	std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> weldMap;
	weldMap.reserve(vertexCount);
	std::vector<std::uint32_t> vertexToWeld(vertexCount);
	std::vector<std::vector<std::uint32_t>> weldToVertices;
	std::vector<XMFLOAT2> initialUv;
	for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
		const PositionKey key = MakePositionKey(ReadPosition(vertices, vertexStrideBytes, vertexIndex));
		auto [it, inserted] = weldMap.try_emplace(key, static_cast<std::uint32_t>(weldToVertices.size()));
		const std::uint32_t weldIndex = it->second;
		if (inserted) {
			weldToVertices.push_back({});
			initialUv.push_back(XMFLOAT2{ 0.0f, 0.0f });
		}
		vertexToWeld[vertexIndex] = weldIndex;
		weldToVertices[weldIndex].push_back(vertexIndex);
		initialUv[weldIndex].x += uvSets[request.uvSetIndex].values[vertexIndex].x;
		initialUv[weldIndex].y += uvSets[request.uvSetIndex].values[vertexIndex].y;
	}
	for (std::uint32_t weldIndex = 0; weldIndex < weldToVertices.size(); ++weldIndex) {
		const float invCount = 1.0f / static_cast<float>(std::max<std::size_t>(1u, weldToVertices[weldIndex].size()));
		initialUv[weldIndex].x *= invCount;
		initialUv[weldIndex].y *= invCount;
	}

	std::vector<TriangleRecord> triangles;
	triangles.reserve(indices.size() / 3u);
	std::vector<std::vector<Constraint>> constraints(weldToVertices.size());
	std::vector<float> oldDensities;
	for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
		const std::uint32_t i0 = indices[i + 0u];
		const std::uint32_t i1 = indices[i + 1u];
		const std::uint32_t i2 = indices[i + 2u];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
			continue;
		}
		const std::uint32_t w0 = vertexToWeld[i0];
		const std::uint32_t w1 = vertexToWeld[i1];
		const std::uint32_t w2 = vertexToWeld[i2];
		if (w0 == w1 || w1 == w2 || w2 == w0) {
			continue;
		}

		const XMFLOAT3 p0 = ReadPosition(vertices, vertexStrideBytes, i0);
		const XMFLOAT3 p1 = ReadPosition(vertices, vertexStrideBytes, i1);
		const XMFLOAT3 p2 = ReadPosition(vertices, vertexStrideBytes, i2);
		const float area3d = TriangleArea3D(p0, p1, p2);
		if (area3d <= kMinTriangleArea) {
			continue;
		}

		const XMFLOAT2 uv0 = uvSets[request.uvSetIndex].values[i0];
		const XMFLOAT2 uv1 = uvSets[request.uvSetIndex].values[i1];
		const XMFLOAT2 uv2 = uvSets[request.uvSetIndex].values[i2];
		const float oldAreaSigned = Cross2(uv0, uv1, uv2) * 0.5f;
		const float oldArea = std::abs(oldAreaSigned);
		if (oldArea > kMinUvArea) {
			oldDensities.push_back(std::sqrt(oldArea / area3d));
		}

		const float weight = std::sqrt(area3d);
		AddConstraint(constraints, w0, w1, Sub2(uv1, uv0), weight);
		AddConstraint(constraints, w1, w2, Sub2(uv2, uv1), weight);
		AddConstraint(constraints, w2, w0, Sub2(uv0, uv2), weight);

		TriangleRecord tri{};
		tri.vertex[0] = i0;
		tri.vertex[1] = i1;
		tri.vertex[2] = i2;
		tri.weld[0] = w0;
		tri.weld[1] = w1;
		tri.weld[2] = w2;
		tri.oldUvArea = oldAreaSigned;
		tri.area3d = area3d;
		triangles.push_back(tri);
	}

	result.weldedVertexCount = static_cast<std::uint32_t>(weldToVertices.size());
	result.validTriangleCount = static_cast<std::uint32_t>(triangles.size());
	if (triangles.empty()) {
		result.message = "no valid triangles for remap";
		return result;
	}

	std::vector<int> component(weldToVertices.size(), -1);
	std::vector<std::uint32_t> anchors;
	for (std::uint32_t start = 0; start < weldToVertices.size(); ++start) {
		if (component[start] >= 0 || constraints[start].empty()) {
			continue;
		}
		const int componentIndex = static_cast<int>(anchors.size());
		anchors.push_back(start);
		std::queue<std::uint32_t> q;
		q.push(start);
		component[start] = componentIndex;
		while (!q.empty()) {
			const std::uint32_t v = q.front();
			q.pop();
			for (const Constraint& c : constraints[v]) {
				if (component[c.neighbor] < 0) {
					component[c.neighbor] = componentIndex;
					q.push(c.neighbor);
				}
			}
		}
	}
	result.componentCount = static_cast<std::uint32_t>(anchors.size());
	if (anchors.empty()) {
		result.message = "no connected remap components";
		return result;
	}

	std::vector<XMFLOAT2> uv;
	std::string solveError;
	if (!SolveLeastSquaresUv(constraints, initialUv, anchors, uv, solveError)) {
		result.message = solveError;
		return result;
	}

	std::vector<float> newDensities;
	for (const TriangleRecord& tri : triangles) {
		const float newArea = UvAreaAbs(uv[tri.weld[0]], uv[tri.weld[1]], uv[tri.weld[2]]);
		if (newArea > kMinUvArea) {
			newDensities.push_back(std::sqrt(newArea / tri.area3d));
		}
	}
	const float oldDensity = Median(std::move(oldDensities));
	const float newDensity = Median(std::move(newDensities));
	if (oldDensity > 0.0f && newDensity > 0.0f) {
		const float scale = oldDensity / newDensity;
		for (XMFLOAT2& value : uv) {
			value.x *= scale;
			value.y *= scale;
		}
	}

	std::vector<std::uint32_t> componentPositive(result.componentCount, 0u);
	std::vector<std::uint32_t> componentNegative(result.componentCount, 0u);
	std::uint32_t degenerateNewUvTriangles = 0;
	for (const TriangleRecord& tri : triangles) {
		const int componentIndex = component[tri.weld[0]];
		if (componentIndex < 0 || static_cast<std::uint32_t>(componentIndex) >= result.componentCount) {
			continue;
		}
		const float newArea = Cross2(uv[tri.weld[0]], uv[tri.weld[1]], uv[tri.weld[2]]) * 0.5f;
		if (std::abs(newArea) <= kMinUvArea) {
			++degenerateNewUvTriangles;
		}
		else if (newArea > 0.0f) {
			++componentPositive[static_cast<std::uint32_t>(componentIndex)];
		}
		else {
			++componentNegative[static_cast<std::uint32_t>(componentIndex)];
		}
	}

	std::uint32_t flipped = degenerateNewUvTriangles;
	for (std::uint32_t componentIndex = 0; componentIndex < result.componentCount; ++componentIndex) {
		flipped += std::min(componentPositive[componentIndex], componentNegative[componentIndex]);
	}
	result.flippedTriangleCount = flipped;
	if (static_cast<float>(flipped) > static_cast<float>(triangles.size()) * kMaxFlipFraction) {
		result.message =
			"generated UVs exceeded triangle foldover threshold (" +
			std::to_string(flipped) + "/" +
			std::to_string(triangles.size()) +
			", degenerate=" +
			std::to_string(degenerateNewUvTriangles) +
			")";
		return result;
	}

	for (const XMFLOAT2& value : uv) {
		if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
			result.message = "generated UVs contain non-finite values";
			return result;
		}
	}

	MeshUvSetData& targetUvSet = uvSets[request.uvSetIndex];
	for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
		targetUvSet.values[vertexIndex] = uv[vertexToWeld[vertexIndex]];
	}

	if (request.uvSetIndex == 0u) {
		for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
			WritePrimaryUv(vertices, vertexStrideBytes, vertexFlags, vertexIndex, targetUvSet.values[vertexIndex]);
		}
	}

	RecomputeTangents(vertices, vertexStrideBytes, vertexFlags, indices, uvSets, request.uvSetIndex);

	result.succeeded = true;
	result.message = "remap succeeded";
	return result;
}

} // namespace br::import
