#include "Core/TriangleGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace clodeval {

std::array<double, 3> ClosestPointBarycentric(const Vec& p, const Vec& a, const Vec& b, const Vec& c)
{
	const Vec ab = b - a, ac = c - a, ap = p - a;
	const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) {
		return { 1.0, 0.0, 0.0 };
	}
	const Vec bp = p - b;
	const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) {
		return { 0.0, 1.0, 0.0 };
	}
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		const double v = d1 / (d1 - d3);
		return { 1.0 - v, v, 0.0 };
	}
	const Vec cp = p - c;
	const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) {
		return { 0.0, 0.0, 1.0 };
	}
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		const double w = d2 / (d2 - d6);
		return { 1.0 - w, 0.0, w };
	}
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return { 0.0, 1.0 - w, w };
	}
	const double denominator = 1.0 / (va + vb + vc);
	const double v = vb * denominator, w = vc * denominator;
	return { 1.0 - v - w, v, w };
}

double SquaredDistanceToTriangle(const Vec& p, const Vec& a, const Vec& b, const Vec& c)
{
	const auto [u, v, w] = ClosestPointBarycentric(p, a, b, c);
	const Vec d = p - (a * u + b * v + c * w);
	return Dot(d, d);
}

TriangleGrid::TriangleGrid(std::vector<std::array<DirectX::XMFLOAT3, 3>> triangles)
	: m_triangles(std::move(triangles))
{
	using DirectX::XMFLOAT3;
	m_min = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
	Vec max{ -m_min.x, -m_min.y, -m_min.z };
	for (const auto& triangle : m_triangles) {
		for (const XMFLOAT3& vertex : triangle) {
			m_min = { std::min<double>(m_min.x, vertex.x), std::min<double>(m_min.y, vertex.y), std::min<double>(m_min.z, vertex.z) };
			max = { std::max<double>(max.x, vertex.x), std::max<double>(max.y, vertex.y), std::max<double>(max.z, vertex.z) };
		}
	}
	const Vec size = max - m_min;
	const double volume = std::max(size.x, 1.0e-6) * std::max(size.y, 1.0e-6) * std::max(size.z, 1.0e-6);
	m_cell = std::max(std::cbrt(volume / std::max<double>(static_cast<double>(m_triangles.size()), 1.0)), 1.0e-6);
	// Thin shapes would otherwise ask for too many cells.
	m_cell = std::max(m_cell, std::max({ size.x, size.y, size.z }) / 512.0);
	m_counts[0] = static_cast<int>(size.x / m_cell) + 1;
	m_counts[1] = static_cast<int>(size.y / m_cell) + 1;
	m_counts[2] = static_cast<int>(size.z / m_cell) + 1;

	std::vector<uint32_t> counts(CellCount() + 1u, 0u);
	const auto forEachCell = [&](const std::array<XMFLOAT3, 3>& triangle, auto&& visit) {
		Vec low = ToVec(triangle[0]), high = low;
		for (const XMFLOAT3& vertex : triangle) {
			low = { std::min<double>(low.x, vertex.x), std::min<double>(low.y, vertex.y), std::min<double>(low.z, vertex.z) };
			high = { std::max<double>(high.x, vertex.x), std::max<double>(high.y, vertex.y), std::max<double>(high.z, vertex.z) };
		}
		const auto [x0, y0, z0] = CellOf(low);
		const auto [x1, y1, z1] = CellOf(high);
		for (int z = z0; z <= z1; ++z) {
			for (int y = y0; y <= y1; ++y) {
				for (int x = x0; x <= x1; ++x) {
					visit(Index(x, y, z));
				}
			}
		}
	};
	for (const auto& triangle : m_triangles) {
		forEachCell(triangle, [&](size_t cell) { ++counts[cell + 1u]; });
	}
	for (size_t index = 1; index < counts.size(); ++index) {
		counts[index] += counts[index - 1u];
	}
	m_cellStart = counts;
	m_cellTriangles.resize(counts.back());
	for (uint32_t index = 0; index < m_triangles.size(); ++index) {
		forEachCell(m_triangles[index], [&](size_t cell) { m_cellTriangles[counts[cell]++] = index; });
	}
}

double TriangleGrid::Nearest(const Vec& point) const
{
	const auto [cx, cy, cz] = CellOf(point);
	double best = std::numeric_limits<double>::max();
	const int maxRing = std::max({ m_counts[0], m_counts[1], m_counts[2] }) + 1;
	for (int ring = 0; ring <= maxRing; ++ring) {
		// Everything in this ring or beyond is at least (ring - 1) cells away, plus whatever the query point sits
		// outside the grid by. Being conservative here only costs an extra ring.
		if (best != std::numeric_limits<double>::max()) {
			const double reach = static_cast<double>(ring - 1) * m_cell;
			if (reach > 0.0 && reach * reach > best) {
				break;
			}
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
						const auto& triangle = m_triangles[m_cellTriangles[entry]];
						best = std::min(best, SquaredDistanceToTriangle(point, ToVec(triangle[0]), ToVec(triangle[1]), ToVec(triangle[2])));
					}
				}
			}
		}
	}
	return std::sqrt(best);
}

std::array<int, 3> TriangleGrid::CellOf(const Vec& point) const
{
	const auto clampAxis = [&](double value, double origin, int count) {
		return std::clamp(static_cast<int>(std::floor((value - origin) / m_cell)), 0, count - 1);
	};
	return { clampAxis(point.x, m_min.x, m_counts[0]), clampAxis(point.y, m_min.y, m_counts[1]), clampAxis(point.z, m_min.z, m_counts[2]) };
}

}
