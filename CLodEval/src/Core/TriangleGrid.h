#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include <DirectXMath.h>

namespace clodeval {

struct Vec {
	double x = 0.0, y = 0.0, z = 0.0;
};
inline Vec ToVec(const DirectX::XMFLOAT3& value) { return { value.x, value.y, value.z }; }
inline Vec operator-(const Vec& a, const Vec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec operator+(const Vec& a, const Vec& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec operator*(const Vec& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline double Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec Cross(const Vec& a, const Vec& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

// The closest point on triangle abc to p, as barycentric weights of a, b and c (Ericson, Real-Time Collision Detection
// 5.1.5).
std::array<double, 3> ClosestPointBarycentric(const Vec& p, const Vec& a, const Vec& b, const Vec& c);
double SquaredDistanceToTriangle(const Vec& p, const Vec& a, const Vec& b, const Vec& c);

// A uniform grid over triangles. Nearest-surface queries grow outward in shells of cells and stop once no unvisited cell
// can hold anything closer.
class TriangleGrid {
public:
	explicit TriangleGrid(std::vector<std::array<DirectX::XMFLOAT3, 3>> triangles);

	bool Empty() const { return m_triangles.empty(); }
	const std::array<DirectX::XMFLOAT3, 3>& Triangle(uint32_t index) const { return m_triangles[index]; }

	// Distance to the nearest triangle.
	double Nearest(const Vec& point) const;

	struct Hit {
		uint32_t index = std::numeric_limits<uint32_t>::max();
		double distanceSquared = std::numeric_limits<double>::max();
		std::array<double, 3> weights{};  // barycentric weights of the closest point on the triangle
		bool Found() const { return index != std::numeric_limits<uint32_t>::max(); }
	};
	// The nearest triangle for which accept(index) holds, no farther than maxDistance; not Found() if there is none.
	template <typename Predicate>
	Hit NearestIf(const Vec& point, double maxDistance, Predicate&& accept) const
	{
		Hit hit;
		if (m_triangles.empty()) {
			return hit;
		}
		const double maxSquared = maxDistance * maxDistance;
		hit.distanceSquared = maxSquared;
		const auto [cx, cy, cz] = CellOf(point);
		const int maxRing = std::max({ m_counts[0], m_counts[1], m_counts[2] }) + 1;
		for (int ring = 0; ring <= maxRing; ++ring) {
			// Everything in this ring or beyond is at least (ring - 1) cells from the point, as in Nearest. A point outside
			// the grid is farther than that from every cell, so the bound still holds.
			const double reach = static_cast<double>(ring - 1) * m_cell;
			if (reach > 0.0 && reach * reach > hit.distanceSquared) {
				break;
			}
			for (int z = cz - ring; z <= cz + ring; ++z) {
				if (z < 0 || z >= m_counts[2]) {
					continue;
				}
				for (int y = cy - ring; y <= cy + ring; ++y) {
					if (y < 0 || y >= m_counts[1]) {
						continue;
					}
					const bool onShellYZ = std::abs(z - cz) == ring || std::abs(y - cy) == ring;
					const int step = onShellYZ || ring == 0 ? 1 : 2 * ring;
					for (int x = cx - ring; x <= cx + ring; x += step) {
						if (x < 0 || x >= m_counts[0]) {
							continue;
						}
						const size_t cell = Index(x, y, z);
						for (uint32_t entry = m_cellStart[cell]; entry < m_cellStart[cell + 1u]; ++entry) {
							const uint32_t index = m_cellTriangles[entry];
							if (!accept(index)) {
								continue;
							}
							const auto& triangle = m_triangles[index];
							const Vec a = ToVec(triangle[0]), b = ToVec(triangle[1]), c = ToVec(triangle[2]);
							const auto weights = ClosestPointBarycentric(point, a, b, c);
							const Vec offset = point - (a * weights[0] + b * weights[1] + c * weights[2]);
							const double distanceSquared = Dot(offset, offset);
							if (distanceSquared < hit.distanceSquared || (!hit.Found() && distanceSquared <= maxSquared)) {
								hit.index = index;
								hit.distanceSquared = distanceSquared;
								hit.weights = weights;
							}
						}
					}
				}
			}
		}
		if (!hit.Found()) {
			hit.distanceSquared = std::numeric_limits<double>::max();
		}
		return hit;
	}

	// Calls visit(index) for every triangle in a cell the box of half-width radius around point touches; a triangle in
	// several of those cells is visited once per cell.
	template <typename Visitor>
	void ForEachNear(const Vec& point, double radius, Visitor&& visit) const
	{
		const auto [x0, y0, z0] = CellOf(point - Vec{ radius, radius, radius });
		const auto [x1, y1, z1] = CellOf(point + Vec{ radius, radius, radius });
		for (int z = z0; z <= z1; ++z) {
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					const size_t cell = Index(x, y, z);
					for (uint32_t entry = m_cellStart[cell]; entry < m_cellStart[cell + 1u]; ++entry) {
						visit(m_cellTriangles[entry]);
					}
				}
			}
		}
	}

private:
	size_t CellCount() const { return static_cast<size_t>(m_counts[0]) * m_counts[1] * m_counts[2]; }
	size_t Index(int x, int y, int z) const { return (static_cast<size_t>(z) * m_counts[1] + y) * m_counts[0] + x; }
	std::array<int, 3> CellOf(const Vec& point) const;

	std::vector<std::array<DirectX::XMFLOAT3, 3>> m_triangles;
	Vec m_min;
	double m_cell = 1.0;
	std::array<int, 3> m_counts{ 1, 1, 1 };
	std::vector<uint32_t> m_cellStart;
	std::vector<uint32_t> m_cellTriangles;
};

}
