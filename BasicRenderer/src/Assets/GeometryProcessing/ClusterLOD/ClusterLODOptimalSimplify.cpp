#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODOptimalSimplify.h"

// The collapse loop below is derived from meshoptimizer 1.3 (ThirdParty/meshoptimizer1.3/src/simplifier.cpp,
// meshopt_simplifyEdge), Copyright (C) 2016-2026 Arseny Kapoulkine, MIT License (see that file and clusterlod.h). The upstream
// function is untouched: this is a separate copy that only BasicRenderer's stage 4 path calls.
//
// Unchanged from upstream: edge adjacency, position remap and wedges, vertex classification (kinds, loops), rescaling, the
// position/edge/attribute/fold quadrics, which edges are candidates (pickEdgeCollapses), the counting sort, per-pass collapse
// locking and goals, how a collapse remaps wedges (complex/seam/manifold), quadric accumulation, loop remapping and index
// compaction. Divergences, each marked "stage 4:" where it happens:
//  1. Ranking evaluates replacement positions, not only the endpoints: for a collapse whose target may move (manifold or
//     complex targets anywhere; border or seam pairs along their edge), the position minimizing the merged position quadric with
//     the incident wedges' attribute quadrics eliminated (Hoppe's new quadric metric, as upstream's solvePositions does after
//     the loop), regularized towards the edge midpoint and offered only within clampScale edge lengths of the edge, and the
//     optimum on the edge segment. Each candidate is costed at its own position by what it adds: the source patch's position
//     quadric error there (or the target patch's increase, if larger), plus the merged wedges' attribute quadric errors with
//     their optimal values (moved) or the target's kept values (endpoint), less the target wedges' current errors. For an
//     endpoint placement that equals upstream's cost up to rounding.
//  2. Validation uses the solved position. Every surviving face around both endpoints (the target moves too) must not rotate to
//     or past flipCos in this collapse (upstream's ~75 degree test, applied with the moved target), and must not turn beyond
//     sheetCos from a persistent reference: the area-weighted orientation of the source surface it represents (given per input
//     triangle, measured against the original by the builder), into which a removed face's reference is merged by the faces
//     that absorb it. A face already beyond sheetCos may still change if it does not get worse (inherited, not new).
//  3. A collapse whose best placement fails validation may use its second-best placement (an endpoint when the best moved) if
//     that is within the pass's error goal.
//  4. An accepted collapse writes its position into every wedge of the target immediately, and after the pass the moved
//     wedges' attributes are solved from their merged attribute quadrics at that position (upstream's solveAttributes rule).
//     There is no solve after the loop.
//  5. No sparse remap (the buffers are private), no pruning, no debug output.

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include <meshoptimizer.h>
#include <ThirdParty/meshoptimizer/clusterlod.h>
#include <tracy/Tracy.hpp>

namespace clod_detail {

	namespace optimal {

		struct EdgeAdjacency
		{
			struct Edge
			{
				unsigned int next;
				unsigned int prev;
			};

			std::vector<unsigned int> offsets;
			std::vector<Edge> data;
		};

		static void updateEdgeAdjacency(EdgeAdjacency& adjacency, const unsigned int* indices, size_t index_count, size_t vertex_count, const unsigned int* remap)
		{
			size_t face_count = index_count / 3;
			adjacency.offsets.assign(vertex_count + 1, 0);
			adjacency.data.resize(index_count);
			unsigned int* offsets = adjacency.offsets.data() + 1;
			EdgeAdjacency::Edge* data = adjacency.data.data();

			for (size_t i = 0; i < index_count; ++i)
			{
				unsigned int v = remap ? remap[indices[i]] : indices[i];
				offsets[v]++;
			}

			unsigned int offset = 0;
			for (size_t i = 0; i < vertex_count; ++i)
			{
				unsigned int count = offsets[i];
				offsets[i] = offset;
				offset += count;
			}

			for (size_t i = 0; i < face_count; ++i)
			{
				unsigned int a = indices[i * 3 + 0], b = indices[i * 3 + 1], c = indices[i * 3 + 2];
				if (remap)
				{
					a = remap[a];
					b = remap[b];
					c = remap[c];
				}

				data[offsets[a]].next = b;
				data[offsets[a]].prev = c;
				offsets[a]++;

				data[offsets[b]].next = c;
				data[offsets[b]].prev = a;
				offsets[b]++;

				data[offsets[c]].next = a;
				data[offsets[c]].prev = b;
				offsets[c]++;
			}

			adjacency.offsets[0] = 0;
		}

		static size_t hashBuckets2(size_t count)
		{
			size_t buckets = 1;
			while (buckets < count + count / 4)
				buckets *= 2;
			return buckets;
		}

		static void buildPositionRemap(unsigned int* remap, unsigned int* wedge, const float* vertex_positions, size_t vertex_count)
		{
			const auto hash = [&](unsigned int index) {
				const unsigned int* key = reinterpret_cast<const unsigned int*>(vertex_positions + index * 3);
				unsigned int x = key[0], y = key[1], z = key[2];
				x = (x == 0x80000000) ? 0 : x;
				y = (y == 0x80000000) ? 0 : y;
				z = (z == 0x80000000) ? 0 : z;
				x ^= x >> 17;
				y ^= y >> 17;
				z ^= z >> 17;
				return size_t((x * 73856093) ^ (y * 19349663) ^ (z * 83492791));
			};
			const auto equal = [&](unsigned int lhs, unsigned int rhs) {
				const float* lv = vertex_positions + lhs * 3;
				const float* rv = vertex_positions + rhs * 3;
				return lv[0] == rv[0] && lv[1] == rv[1] && lv[2] == rv[2];
			};

			size_t table_size = hashBuckets2(vertex_count);
			std::vector<unsigned int> table(table_size, ~0u);
			const size_t hashmod = table_size - 1;

			for (size_t i = 0; i < vertex_count; ++i)
			{
				unsigned int index = unsigned(i);
				size_t bucket = hash(index) & hashmod;
				unsigned int* entry = nullptr;
				for (size_t probe = 0; probe <= hashmod; ++probe)
				{
					unsigned int& item = table[bucket];
					if (item == ~0u || equal(item, index))
					{
						entry = &item;
						break;
					}
					bucket = (bucket + probe + 1) & hashmod;
				}
				assert(entry);
				if (*entry == ~0u)
					*entry = index;
				remap[index] = *entry;
			}

			for (size_t i = 0; i < vertex_count; ++i)
				wedge[i] = unsigned(i);

			for (size_t i = 0; i < vertex_count; ++i)
				if (remap[i] != i)
				{
					unsigned int r = remap[i];
					wedge[i] = wedge[r];
					wedge[r] = unsigned(i);
				}
		}

		enum VertexKind
		{
			Kind_Manifold,
			Kind_Border,
			Kind_Seam,
			Kind_Complex,
			Kind_Fringe,
			Kind_Locked,

			Kind_Count
		};

		const unsigned char kCanCollapse[Kind_Count][Kind_Count] = {
			{1, 1, 1, 1, 1, 1},
			{0, 1, 0, 0, 1, 1},
			{0, 0, 1, 0, 0, 1},
			{0, 0, 0, 1, 1, 1},
			{0, 0, 0, 0, 1, 0},
			{0, 0, 0, 0, 0, 0},
		};

		const unsigned char kHasOpposite[Kind_Count][Kind_Count] = {
			{1, 1, 1, 1, 1, 1},
			{1, 0, 1, 0, 0, 0},
			{1, 1, 1, 0, 0, 1},
			{1, 0, 0, 0, 0, 0},
			{1, 0, 0, 0, 0, 0},
			{1, 0, 1, 0, 0, 0},
		};

		static bool hasEdge(const EdgeAdjacency& adjacency, unsigned int a, unsigned int b)
		{
			unsigned int count = adjacency.offsets[a + 1] - adjacency.offsets[a];
			const EdgeAdjacency::Edge* edges = adjacency.data.data() + adjacency.offsets[a];
			for (size_t i = 0; i < count; ++i)
				if (edges[i].next == b)
					return true;
			return false;
		}

		static bool hasEdge(const EdgeAdjacency& adjacency, unsigned int a, unsigned int b, const unsigned int* remap, const unsigned int* wedge)
		{
			unsigned int v = a;
			do
			{
				unsigned int count = adjacency.offsets[v + 1] - adjacency.offsets[v];
				const EdgeAdjacency::Edge* edges = adjacency.data.data() + adjacency.offsets[v];
				for (size_t i = 0; i < count; ++i)
					if (remap[edges[i].next] == remap[b])
						return true;
				v = wedge[v];
			} while (v != a);
			return false;
		}

		static void classifyVertices(unsigned char* result, unsigned int* loop, unsigned int* loopback, size_t vertex_count, const EdgeAdjacency& adjacency, const unsigned int* remap, const unsigned int* wedge, const unsigned char* vertex_lock, unsigned int options)
		{
			memset(loop, -1, vertex_count * sizeof(unsigned int));
			memset(loopback, -1, vertex_count * sizeof(unsigned int));

			unsigned int* openinc = loopback;
			unsigned int* openout = loop;

			for (size_t i = 0; i < vertex_count; ++i)
			{
				unsigned int vertex = unsigned(i);
				unsigned int count = adjacency.offsets[vertex + 1] - adjacency.offsets[vertex];
				const EdgeAdjacency::Edge* edges = adjacency.data.data() + adjacency.offsets[vertex];

				for (size_t j = 0; j < count; ++j)
				{
					unsigned int target = edges[j].next;
					if (target == vertex)
					{
						openinc[vertex] = openout[vertex] = vertex;
					}
					else if (!hasEdge(adjacency, target, vertex))
					{
						openinc[target] = (openinc[target] == ~0u) ? vertex : target;
						openout[vertex] = (openout[vertex] == ~0u) ? target : vertex;
					}
				}
			}

			for (size_t i = 0; i < vertex_count; ++i)
			{
				if (remap[i] == i)
				{
					if (wedge[i] == i)
					{
						unsigned int openi = openinc[i], openo = openout[i];
						if (openi == ~0u && openo == ~0u)
							result[i] = Kind_Manifold;
						else if (openi != ~0u && openo != ~0u && remap[openi] == remap[openo] && openi != i)
							result[i] = Kind_Seam;
						else if (openi != i && openo != i)
							result[i] = Kind_Border;
						else
							result[i] = Kind_Locked;
					}
					else if (wedge[wedge[i]] == i)
					{
						unsigned int w = wedge[i];
						unsigned int openiv = openinc[i], openov = openout[i];
						unsigned int openiw = openinc[w], openow = openout[w];

						if (openiv != ~0u && openiv != i && openov != ~0u && openov != i &&
						    openiw != ~0u && openiw != w && openow != ~0u && openow != w &&
						    remap[openiv] == remap[openow] && remap[openov] == remap[openiw] && remap[openiv] != remap[openov])
							result[i] = Kind_Seam;
						else
							result[i] = Kind_Locked;
					}
					else
					{
						result[i] = Kind_Locked;
					}
				}
				else
				{
					assert(remap[i] < i);
					result[i] = result[remap[i]];
				}
			}

			if (options & meshopt_SimplifyPermissive)
				for (size_t i = 0; i < vertex_count; ++i)
					if (result[i] == Kind_Seam || result[i] == Kind_Locked)
					{
						if (remap[i] != i)
						{
							result[i] = result[remap[i]];
							continue;
						}

						unsigned int v = unsigned(i);
						bool protect = false;
						do
						{
							protect |= vertex_lock && (vertex_lock[v] & meshopt_SimplifyVertex_Protect) != 0;
							v = wedge[v];
						} while (v != i);

						bool border = false;
						do
						{
							const EdgeAdjacency::Edge* edges = &adjacency.data[adjacency.offsets[v]];
							size_t count = adjacency.offsets[v + 1] - adjacency.offsets[v];
							for (size_t j = 0; j < count; ++j)
								border |= !hasEdge(adjacency, edges[j].next, v, remap, wedge);
							v = wedge[v];
						} while (v != i);

						result[i] = protect ? result[i] : (unsigned char)(border ? Kind_Fringe : Kind_Complex);
					}

			if (vertex_lock)
			{
				for (size_t i = 0; i < vertex_count; ++i)
					if (vertex_lock[i] & meshopt_SimplifyVertex_Lock)
						result[remap[i]] = Kind_Locked;

				for (size_t i = 0; i < vertex_count; ++i)
					if (result[remap[i]] == Kind_Locked)
						result[i] = Kind_Locked;
			}

			if (options & meshopt_SimplifyLockBorder)
				for (size_t i = 0; i < vertex_count; ++i)
					if (result[i] == Kind_Border || result[i] == Kind_Fringe)
						result[i] = Kind_Locked;
		}

		struct Vector3
		{
			float x, y, z;
		};

		static const size_t kMaxAttributes = 32;

		struct Quadric
		{
			float a00, a11, a22;
			float a10, a20, a21;
			float b0, b1, b2, c;
			float w;
		};

		struct QuadricGrad
		{
			float gx, gy, gz, gw;
		};

		static float normalize(Vector3& v)
		{
			float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
			if (length > 0)
			{
				v.x /= length;
				v.y /= length;
				v.z /= length;
			}
			return length;
		}

		static void quadricAdd(Quadric& Q, const Quadric& R)
		{
			Q.a00 += R.a00;
			Q.a11 += R.a11;
			Q.a22 += R.a22;
			Q.a10 += R.a10;
			Q.a20 += R.a20;
			Q.a21 += R.a21;
			Q.b0 += R.b0;
			Q.b1 += R.b1;
			Q.b2 += R.b2;
			Q.c += R.c;
			Q.w += R.w;
		}

		static void quadricAdd(QuadricGrad& G, const QuadricGrad& R)
		{
			G.gx += R.gx;
			G.gy += R.gy;
			G.gz += R.gz;
			G.gw += R.gw;
		}

		static void quadricAdd(QuadricGrad* G, const QuadricGrad* R, size_t attribute_count)
		{
			for (size_t k = 0; k < attribute_count; ++k)
			{
				G[k].gx += R[k].gx;
				G[k].gy += R[k].gy;
				G[k].gz += R[k].gz;
				G[k].gw += R[k].gw;
			}
		}

		static float quadricEval(const Quadric& Q, const Vector3& v)
		{
			float rx = (Q.b0 + Q.a10 * v.y) * 2.f;
			float ry = (Q.b1 + Q.a21 * v.z) * 2.f;
			float rz = (Q.b2 + Q.a20 * v.x) * 2.f;

			rx += Q.a00 * v.x;
			ry += Q.a11 * v.y;
			rz += Q.a22 * v.z;

			return Q.c + rx * v.x + ry * v.y + rz * v.z;
		}

		static float quadricError(const Quadric& Q, const Vector3& v)
		{
			float r = quadricEval(Q, v);
			float s = Q.w == 0.f ? 0.f : 1.f / Q.w;
			return fabsf(r) * s;
		}

		static float quadricError(const Quadric& Q, const QuadricGrad* G, size_t attribute_count, const Vector3& v, const float* va, bool clamped)
		{
			float r = quadricEval(Q, v);
			for (size_t k = 0; k < attribute_count; ++k)
			{
				float a = va[k];
				float g = v.x * G[k].gx + v.y * G[k].gy + v.z * G[k].gz + G[k].gw;
				r += a * (a * Q.w - 2 * g);
			}
			float e = fabsf(r);
			return clamped ? (e < Q.w ? e : Q.w) : e;
		}

		static void quadricFromPlane(Quadric& Q, float a, float b, float c, float d, float w)
		{
			float aw = a * w;
			float bw = b * w;
			float cw = c * w;
			float dw = d * w;

			Q.a00 = a * aw;
			Q.a11 = b * bw;
			Q.a22 = c * cw;
			Q.a10 = a * bw;
			Q.a20 = a * cw;
			Q.a21 = b * cw;
			Q.b0 = a * dw;
			Q.b1 = b * dw;
			Q.b2 = c * dw;
			Q.c = d * dw;
			Q.w = w;
		}

		static void quadricFromPoint(Quadric& Q, float x, float y, float z, float w)
		{
			Q.a00 = Q.a11 = Q.a22 = w;
			Q.a10 = Q.a20 = Q.a21 = 0;
			Q.b0 = -x * w;
			Q.b1 = -y * w;
			Q.b2 = -z * w;
			Q.c = (x * x + y * y + z * z) * w;
			Q.w = w;
		}

		static void quadricFromTriangle(Quadric& Q, const Vector3& p0, const Vector3& p1, const Vector3& p2, float weight)
		{
			Vector3 p10 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
			Vector3 p20 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
			Vector3 normal = {p10.y * p20.z - p10.z * p20.y, p10.z * p20.x - p10.x * p20.z, p10.x * p20.y - p10.y * p20.x};
			float area = normalize(normal);
			float distance = normal.x * p0.x + normal.y * p0.y + normal.z * p0.z;
			quadricFromPlane(Q, normal.x, normal.y, normal.z, -distance, sqrtf(area) * weight);
		}

		static void quadricFromTriangleEdge(Quadric& Q, const Vector3& p0, const Vector3& p1, const Vector3& p2, float weight)
		{
			Vector3 p10 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
			float lengthsq = p10.x * p10.x + p10.y * p10.y + p10.z * p10.z;
			float length = sqrtf(lengthsq);
			Vector3 p20 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
			float p20p = p20.x * p10.x + p20.y * p10.y + p20.z * p10.z;
			Vector3 perp = {p20.x * lengthsq - p10.x * p20p, p20.y * lengthsq - p10.y * p20p, p20.z * lengthsq - p10.z * p20p};
			normalize(perp);
			float distance = perp.x * p0.x + perp.y * p0.y + perp.z * p0.z;
			quadricFromPlane(Q, perp.x, perp.y, perp.z, -distance, length * weight);
		}

		static void quadricFromAttributes(Quadric& Q, QuadricGrad* G, const Vector3& p0, const Vector3& p1, const Vector3& p2, const float* va0, const float* va1, const float* va2, size_t attribute_count)
		{
			Vector3 p10 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
			Vector3 p20 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
			Vector3 normal = {p10.y * p20.z - p10.z * p20.y, p10.z * p20.x - p10.x * p20.z, p10.x * p20.y - p10.y * p20.x};
			float area = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z) * 0.5f;
			float w = area;

			const Vector3& v0 = p10;
			const Vector3& v1 = p20;
			float d00 = v0.x * v0.x + v0.y * v0.y + v0.z * v0.z;
			float d01 = v0.x * v1.x + v0.y * v1.y + v0.z * v1.z;
			float d11 = v1.x * v1.x + v1.y * v1.y + v1.z * v1.z;
			float denom = d00 * d11 - d01 * d01;
			float denomr = denom == 0 ? 0.f : 1.f / denom;

			float gx1 = (d11 * v0.x - d01 * v1.x) * denomr;
			float gx2 = (d00 * v1.x - d01 * v0.x) * denomr;
			float gy1 = (d11 * v0.y - d01 * v1.y) * denomr;
			float gy2 = (d00 * v1.y - d01 * v0.y) * denomr;
			float gz1 = (d11 * v0.z - d01 * v1.z) * denomr;
			float gz2 = (d00 * v1.z - d01 * v0.z) * denomr;

			memset(&Q, 0, sizeof(Quadric));
			Q.w = w;

			for (size_t k = 0; k < attribute_count; ++k)
			{
				float a0 = va0[k], a1 = va1[k], a2 = va2[k];
				float gx = gx1 * (a1 - a0) + gx2 * (a2 - a0);
				float gy = gy1 * (a1 - a0) + gy2 * (a2 - a0);
				float gz = gz1 * (a1 - a0) + gz2 * (a2 - a0);
				float gw = a0 - p0.x * gx - p0.y * gy - p0.z * gz;

				Q.a00 += w * (gx * gx);
				Q.a11 += w * (gy * gy);
				Q.a22 += w * (gz * gz);
				Q.a10 += w * (gy * gx);
				Q.a20 += w * (gz * gx);
				Q.a21 += w * (gz * gy);
				Q.b0 += w * (gx * gw);
				Q.b1 += w * (gy * gw);
				Q.b2 += w * (gz * gw);
				Q.c += w * (gw * gw);

				G[k].gx = w * gx;
				G[k].gy = w * gy;
				G[k].gz = w * gz;
				G[k].gw = w * gw;
			}
		}

		static void quadricVolumeGradient(QuadricGrad& G, const Vector3& p0, const Vector3& p1, const Vector3& p2)
		{
			Vector3 p10 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
			Vector3 p20 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
			Vector3 normal = {p10.y * p20.z - p10.z * p20.y, p10.z * p20.x - p10.x * p20.z, p10.x * p20.y - p10.y * p20.x};
			float area = normalize(normal) * 0.5f;

			G.gx = normal.x * area;
			G.gy = normal.y * area;
			G.gz = normal.z * area;
			G.gw = (-p0.x * normal.x - p0.y * normal.y - p0.z * normal.z) * area;
		}

		static bool quadricSolve(Vector3& p, const Quadric& Q, const QuadricGrad& GV)
		{
			float a00 = Q.a00, a11 = Q.a11, a22 = Q.a22;
			float a10 = Q.a10, a20 = Q.a20, a21 = Q.a21;
			float x0 = -Q.b0, x1 = -Q.b1, x2 = -Q.b2;

			float eps = 1e-6f * Q.w;

			float d0 = a00;
			float l10 = a10 / d0;
			float l20 = a20 / d0;

			float d1 = a11 - a10 * l10;
			float dl21 = a21 - a20 * l10;
			float l21 = dl21 / d1;

			float d2 = a22 - a20 * l20 - dl21 * l21;

			float y0 = x0;
			float y1 = x1 - l10 * y0;
			float y2 = x2 - l20 * y0 - l21 * y1;

			float z0 = y0 / d0;
			float z1 = y1 / d1;
			float z2 = y2 / d2;

			float a30 = GV.gx, a31 = GV.gy, a32 = GV.gz;
			float x3 = -GV.gw;

			float l30 = a30 / d0;
			float dl31 = a31 - a30 * l10;
			float l31 = dl31 / d1;
			float dl32 = a32 - a30 * l20 - dl31 * l21;
			float l32 = dl32 / d2;
			float d3 = 0.f - a30 * l30 - dl31 * l31 - dl32 * l32;

			float y3 = x3 - l30 * y0 - l31 * y1 - l32 * y2;
			float z3 = fabsf(d3) > eps ? y3 / d3 : 0.f;

			float lambda = z3;
			float pz = z2 - l32 * lambda;
			float py = z1 - l21 * pz - l31 * lambda;
			float px = z0 - l10 * py - l20 * pz - l30 * lambda;

			p.x = px;
			p.y = py;
			p.z = pz;

			return fabsf(d0) > eps && fabsf(d1) > eps && fabsf(d2) > eps;
		}

		static void quadricReduceAttributes(Quadric& Q, const Quadric& A, const QuadricGrad* G, size_t attribute_count)
		{
			Q.a00 += A.a00 * Q.w;
			Q.a11 += A.a11 * Q.w;
			Q.a22 += A.a22 * Q.w;
			Q.a10 += A.a10 * Q.w;
			Q.a20 += A.a20 * Q.w;
			Q.a21 += A.a21 * Q.w;
			Q.b0 += A.b0 * Q.w;
			Q.b1 += A.b1 * Q.w;
			Q.b2 += A.b2 * Q.w;

			float iaw = A.w == 0 ? 0.f : Q.w / A.w;

			for (size_t k = 0; k < attribute_count; ++k)
			{
				const QuadricGrad& g = G[k];

				Q.a00 -= (g.gx * g.gx) * iaw;
				Q.a11 -= (g.gy * g.gy) * iaw;
				Q.a22 -= (g.gz * g.gz) * iaw;
				Q.a10 -= (g.gx * g.gy) * iaw;
				Q.a20 -= (g.gx * g.gz) * iaw;
				Q.a21 -= (g.gy * g.gz) * iaw;

				Q.b0 -= (g.gx * g.gw) * iaw;
				Q.b1 -= (g.gy * g.gw) * iaw;
				Q.b2 -= (g.gz * g.gw) * iaw;
			}
		}

		static void fillFaceQuadrics(Quadric* vertex_quadrics, QuadricGrad* volume_gradients, const unsigned int* indices, size_t index_count, const Vector3* vertex_positions, const unsigned int* remap)
		{
			for (size_t i = 0; i < index_count; i += 3)
			{
				unsigned int i0 = indices[i + 0];
				unsigned int i1 = indices[i + 1];
				unsigned int i2 = indices[i + 2];

				Quadric Q;
				quadricFromTriangle(Q, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2], 1.f);

				quadricAdd(vertex_quadrics[remap[i0]], Q);
				quadricAdd(vertex_quadrics[remap[i1]], Q);
				quadricAdd(vertex_quadrics[remap[i2]], Q);

				if (volume_gradients)
				{
					QuadricGrad GV;
					quadricVolumeGradient(GV, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2]);

					quadricAdd(volume_gradients[remap[i0]], GV);
					quadricAdd(volume_gradients[remap[i1]], GV);
					quadricAdd(volume_gradients[remap[i2]], GV);
				}
			}
		}

		static void fillVertexQuadrics(Quadric* vertex_quadrics, const Vector3* vertex_positions, size_t vertex_count, const unsigned int* remap, const unsigned char* vertex_lock, unsigned int options)
		{
			float factor = (options & meshopt_SimplifyRegularizeLight) ? 1e-2f : ((options & meshopt_SimplifyRegularize) ? 1e-1f : 1e-7f);

			for (size_t i = 0; i < vertex_count; ++i)
			{
				if (remap[i] != i)
					continue;

				bool priority = vertex_lock && (vertex_lock[i] & meshopt_SimplifyVertex_Priority) != 0;

				const Vector3& p = vertex_positions[i];
				float w = vertex_quadrics[i].w * (priority ? 1.0f : factor);

				Quadric Q;
				quadricFromPoint(Q, p.x, p.y, p.z, w);

				quadricAdd(vertex_quadrics[i], Q);
			}
		}

		static void fillEdgeQuadrics(Quadric* vertex_quadrics, const unsigned int* indices, size_t index_count, const Vector3* vertex_positions, const unsigned int* remap, const unsigned char* vertex_kind, const unsigned int* loop, const unsigned int* loopback)
		{
			for (size_t i = 0; i < index_count; i += 3)
			{
				static const int next[4] = {1, 2, 0, 1};

				for (int e = 0; e < 3; ++e)
				{
					unsigned int i0 = indices[i + e];
					unsigned int i1 = indices[i + next[e]];

					unsigned char k0 = vertex_kind[i0];
					unsigned char k1 = vertex_kind[i1];

					if (k0 == Kind_Manifold || k1 == Kind_Manifold)
						continue;

					if (k0 != Kind_Border && k0 != Kind_Seam && k0 != Kind_Fringe && k1 != Kind_Border && k1 != Kind_Seam && k1 != Kind_Fringe)
						continue;

					if ((k0 == Kind_Border || k0 == Kind_Seam) && loop[i0] != i1)
						continue;

					if ((k1 == Kind_Border || k1 == Kind_Seam) && loopback[i1] != i0)
						continue;

					if ((k0 == Kind_Fringe || k1 == Kind_Fringe) && (k0 == Kind_Complex || k1 == Kind_Complex))
						continue;

					unsigned int i2 = indices[i + next[e + 1]];

					const float kEdgeWeightSeam = 0.5f;
					const float kEdgeWeightBorder = 10.f;

					float edgeWeight = (k0 == Kind_Seam || k1 == Kind_Seam) ? kEdgeWeightSeam : kEdgeWeightBorder;

					Quadric Q;
					quadricFromTriangleEdge(Q, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2], edgeWeight);

					Quadric QT;
					quadricFromTriangle(QT, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2], edgeWeight);

					QT.w = 0;
					quadricAdd(Q, QT);

					quadricAdd(vertex_quadrics[remap[i0]], Q);
					quadricAdd(vertex_quadrics[remap[i1]], Q);
				}
			}
		}

		static unsigned int oppositeVertex(const EdgeAdjacency& adjacency, unsigned int a, unsigned int b, const unsigned int* remap, const unsigned int* wedge)
		{
			unsigned int v = a;
			unsigned int r = ~0u;

			do
			{
				unsigned int count = adjacency.offsets[v + 1] - adjacency.offsets[v];
				const EdgeAdjacency::Edge* edges = adjacency.data.data() + adjacency.offsets[v];

				for (size_t i = 0; i < count; ++i)
					if (remap[edges[i].next] == remap[b])
						r = (r == ~0u) ? edges[i].prev : a;

				v = wedge[v];
			} while (v != a);

			return r == a ? ~0u : r;
		}

		static void fillFoldQuadrics(Quadric* vertex_quadrics, const EdgeAdjacency& adjacency, const Vector3* vertex_positions, size_t vertex_count, const unsigned int* remap, const unsigned int* wedge)
		{
			for (size_t i0 = 0; i0 < vertex_count; ++i0)
				for (unsigned int i = adjacency.offsets[i0]; i < adjacency.offsets[i0 + 1]; ++i)
				{
					unsigned int i1 = adjacency.data[i].next;
					unsigned int i2 = adjacency.data[i].prev;

					if (remap[i1] > remap[i0])
						continue;

					unsigned int i3 = oppositeVertex(adjacency, i1, unsigned(i0), remap, wedge);

					if (i3 == ~0u)
						continue;

					Vector3 p10 = {vertex_positions[i1].x - vertex_positions[i0].x, vertex_positions[i1].y - vertex_positions[i0].y, vertex_positions[i1].z - vertex_positions[i0].z};
					Vector3 p20 = {vertex_positions[i2].x - vertex_positions[i0].x, vertex_positions[i2].y - vertex_positions[i0].y, vertex_positions[i2].z - vertex_positions[i0].z};
					Vector3 p30 = {vertex_positions[i3].x - vertex_positions[i0].x, vertex_positions[i3].y - vertex_positions[i0].y, vertex_positions[i3].z - vertex_positions[i0].z};

					Vector3 normal = {p10.y * p20.z - p10.z * p20.y, p10.z * p20.x - p10.x * p20.z, p10.x * p20.y - p10.y * p20.x};
					Vector3 opposite = {p30.y * p10.z - p30.z * p10.y, p30.z * p10.x - p30.x * p10.z, p30.x * p10.y - p30.y * p10.x};
					float nl = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
					float ol = sqrtf(opposite.x * opposite.x + opposite.y * opposite.y + opposite.z * opposite.z);

					if (normal.x * opposite.x + normal.y * opposite.y + normal.z * opposite.z >= -0.85f * nl * ol)
						continue;

					Quadric Q, QO;
					quadricFromTriangleEdge(Q, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2], 1.f);
					quadricFromTriangleEdge(QO, vertex_positions[i0], vertex_positions[i1], vertex_positions[i3], 1.f);
					quadricAdd(Q, QO);

					quadricAdd(vertex_quadrics[remap[i0]], Q);
					quadricAdd(vertex_quadrics[remap[i1]], Q);
				}
		}

		static void fillAttributeQuadrics(Quadric* attribute_quadrics, QuadricGrad* attribute_gradients, const unsigned int* indices, size_t index_count, const Vector3* vertex_positions, const float* vertex_attributes, size_t attribute_count)
		{
			for (size_t i = 0; i < index_count; i += 3)
			{
				unsigned int i0 = indices[i + 0];
				unsigned int i1 = indices[i + 1];
				unsigned int i2 = indices[i + 2];

				Quadric QA;
				QuadricGrad G[kMaxAttributes];
				quadricFromAttributes(QA, G, vertex_positions[i0], vertex_positions[i1], vertex_positions[i2], &vertex_attributes[i0 * attribute_count], &vertex_attributes[i1 * attribute_count], &vertex_attributes[i2 * attribute_count], attribute_count);

				quadricAdd(attribute_quadrics[i0], QA);
				quadricAdd(attribute_quadrics[i1], QA);
				quadricAdd(attribute_quadrics[i2], QA);

				quadricAdd(&attribute_gradients[i0 * attribute_count], G, attribute_count);
				quadricAdd(&attribute_gradients[i1 * attribute_count], G, attribute_count);
				quadricAdd(&attribute_gradients[i2 * attribute_count], G, attribute_count);
			}
		}

		static unsigned int getComplexTarget(unsigned int v, unsigned int target, const unsigned int* remap, const unsigned int* loop, const unsigned int* loopback)
		{
			unsigned int r = remap[target];
			if (loop[v] != ~0u && remap[loop[v]] == r)
				return loop[v];
			else if (loopback[v] != ~0u && remap[loopback[v]] == r)
				return loopback[v];
			else
				return target;
		}

		static size_t boundEdgeCollapses(const EdgeAdjacency& adjacency, size_t vertex_count, size_t index_count, const unsigned char* vertex_kind)
		{
			size_t dual_count = 0;
			for (size_t i = 0; i < vertex_count; ++i)
			{
				unsigned char k = vertex_kind[i];
				unsigned int e = adjacency.offsets[i + 1] - adjacency.offsets[i];
				dual_count += (k == Kind_Manifold || k == Kind_Seam) ? e : 0;
			}
			return (index_count - dual_count / 2) + 3;
		}

		// stage 4: a placement for a collapse. move: 0 the target stays (upstream), 1 free solve, 2 on the edge's line.
		struct Placement
		{
			unsigned int v0;
			unsigned int v1;
			Vector3 p;
			float error;
			unsigned char move;
		};

		// stage 4: upstream's Collapse plus its best and second-best placements (sorted by the best).
		struct Collapse
		{
			unsigned int v0;
			unsigned int v1;
			unsigned int bidi;
			float error;
			Placement placements[2];
			unsigned char placement_count;
		};

		static size_t pickEdgeCollapses(Collapse* collapses, size_t collapse_capacity, const unsigned int* indices, size_t index_count, const unsigned int* remap, const unsigned char* vertex_kind, const unsigned int* loop, const unsigned int* loopback)
		{
			size_t collapse_count = 0;

			for (size_t i = 0; i < index_count; i += 3)
			{
				static const int next[3] = {1, 2, 0};

				if (collapse_count + 3 > collapse_capacity)
					break;

				for (int e = 0; e < 3; ++e)
				{
					unsigned int i0 = indices[i + e];
					unsigned int i1 = indices[i + next[e]];

					if (remap[i0] == remap[i1])
						continue;

					unsigned char k0 = vertex_kind[i0];
					unsigned char k1 = vertex_kind[i1];

					if (!(kCanCollapse[k0][k1] | kCanCollapse[k1][k0]))
						continue;

					if (kHasOpposite[k0][k1] && remap[i1] > remap[i0])
						continue;

					if ((k0 == Kind_Border || k0 == Kind_Seam) && k1 != Kind_Manifold && loop[i0] != i1)
						continue;
					if ((k1 == Kind_Border || k1 == Kind_Seam) && k0 != Kind_Manifold && loopback[i1] != i0)
						continue;

					Collapse c = {};
					if (kCanCollapse[k0][k1] & kCanCollapse[k1][k0])
					{
						c.v0 = i0;
						c.v1 = i1;
						c.bidi = 1;
					}
					else
					{
						c.v0 = kCanCollapse[k0][k1] ? i0 : i1;
						c.v1 = kCanCollapse[k0][k1] ? i1 : i0;
						c.bidi = 0;
					}
					collapses[collapse_count++] = c;
				}
			}

			return collapse_count;
		}

		static void sortEdgeCollapses(unsigned int* sort_order, const Collapse* collapses, size_t collapse_count)
		{
			const unsigned int sort_bits = 12;
			const unsigned int sort_bins = 2048 + 512;

			std::vector<unsigned int> histogram(sort_bins, 0);

			const auto keyOf = [&](const Collapse& c) {
				unsigned int error;
				memcpy(&error, &c.error, sizeof(error));
				unsigned int key = (error << 1) >> (32 - sort_bits);
				return key < sort_bins ? key : sort_bins - 1;
			};

			for (size_t i = 0; i < collapse_count; ++i)
				histogram[keyOf(collapses[i])]++;

			size_t histogram_sum = 0;
			for (size_t i = 0; i < sort_bins; ++i)
			{
				size_t count = histogram[i];
				histogram[i] = unsigned(histogram_sum);
				histogram_sum += count;
			}

			for (size_t i = 0; i < collapse_count; ++i)
				sort_order[histogram[keyOf(collapses[i])]++] = unsigned(i);
		}

		static void remapEdgeLoops(unsigned int* loop, size_t vertex_count, const unsigned int* collapse_remap)
		{
			for (size_t i = 0; i < vertex_count; ++i)
			{
				if (loop[i] != ~0u)
				{
					unsigned int l = loop[i];
					unsigned int r = collapse_remap[l];

					if (i == r)
						loop[i] = (loop[l] != ~0u) ? collapse_remap[loop[l]] : ~0u;
					else
						loop[i] = r;
				}
			}
		}

		inline Vector3 sub(const Vector3& a, const Vector3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline Vector3 cross(const Vector3& a, const Vector3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline float dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline Vector3 lerp(const Vector3& a, const Vector3& b, float t) { return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; }
		inline bool finite(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

		// stage 4: the wedge attribute quadrics a collapse merges. Each target wedge of the collapse with the source wedges remapped
		// onto it (as performEdgeCollapses remaps them); with all, every wedge of the target, since a moved target moves them all.
		struct MergedWedge
		{
			unsigned int target;
			bool received;
			Quadric A;
			QuadricGrad G[kMaxAttributes];
		};

		struct State
		{
			size_t vertex_count = 0;
			size_t attribute_count = 0;
			const unsigned int* remap = nullptr;
			const unsigned int* wedge = nullptr;
			const unsigned int* loop = nullptr;
			const unsigned int* loopback = nullptr;
			const unsigned char* kind = nullptr;
			Vector3* positions = nullptr;
			float* attributes = nullptr;
			Quadric* quadrics = nullptr;
			QuadricGrad* volume = nullptr;
			Quadric* attribute_quadrics = nullptr;
			QuadricGrad* attribute_gradients = nullptr;
			bool clamped = false;
		};

		// Source wedges of a collapse i0 -> i1 and their targets, as performEdgeCollapses remaps them.
		static void collapsePairs(const State& s, unsigned int i0, unsigned int i1, std::vector<std::pair<unsigned int, unsigned int> >& pairs)
		{
			pairs.clear();
			const unsigned char kind = s.kind[i0];
			if (kind == Kind_Complex || kind == Kind_Fringe)
			{
				pairs.push_back({ i0, i1 });
				for (unsigned int v = s.wedge[i0]; v != i0; v = s.wedge[v])
					pairs.push_back({ v, getComplexTarget(v, i1, s.remap, s.loop, s.loopback) });
			}
			else if (kind == Kind_Seam)
			{
				unsigned int s0 = s.wedge[i0];
				unsigned int s1 = s.loop[i0] == i1 ? s.loopback[s0] : s.loop[s0];
				s1 = (s1 != ~0u) ? s1 : s.wedge[i1];
				pairs.push_back({ i0, i1 });
				if (s0 != i0)
					pairs.push_back({ s0, s1 });
			}
			else
			{
				pairs.push_back({ i0, i1 });
			}
		}

		static void mergeWedges(const State& s, unsigned int i0, unsigned int i1, bool all, const std::vector<std::pair<unsigned int, unsigned int> >& pairs, std::vector<MergedWedge>& merged)
		{
			const size_t K = s.attribute_count;
			merged.clear();
			const auto find = [&](unsigned int target) -> MergedWedge& {
				for (MergedWedge& m : merged)
					if (m.target == target)
						return m;
				merged.emplace_back();
				MergedWedge& m = merged.back();
				m.target = target;
				m.received = false;
				m.A = s.attribute_quadrics[target];
				memcpy(m.G, &s.attribute_gradients[size_t(target) * K], sizeof(QuadricGrad) * K);
				return m;
			};
			for (const auto& pair : pairs)
			{
				MergedWedge& m = find(pair.second);
				m.received = true;
				quadricAdd(m.A, s.attribute_quadrics[pair.first]);
				quadricAdd(m.G, &s.attribute_gradients[size_t(pair.first) * K], K);
			}
			if (all)
			{
				unsigned int v = i1;
				do
				{
					find(v);
					v = s.wedge[v];
				} while (v != i1);
			}
		}

		// The attribute value minimizing a wedge's quadric at p (upstream's solveAttributes).
		inline float optimalAttribute(const MergedWedge& m, size_t k, const Vector3& p)
		{
			const QuadricGrad& G = m.G[k];
			const float iw = m.A.w == 0 ? 0.f : 1.f / m.A.w;
			return (G.gx * p.x + G.gy * p.y + G.gz * p.z + G.gw) * iw;
		}

		// stage 4: the cost of a placement (see the divergences at the top): what the collapse adds to the target's error. Position:
		// the source patch's quadric error at p, or the target patch's increase over its current error if that is larger; attributes:
		// each merged wedge's quadric error at p (with its optimal values when the target moves, else the target's kept values) less
		// the target wedge's current error. At the target's own position this is upstream's cost (rankEdgeCollapses), up to rounding.
		static float placementCost(const State& s, unsigned int i0, unsigned int i1, const Vector3& p, bool moved, const std::vector<MergedWedge>& merged)
		{
			const unsigned int r0 = s.remap[i0], r1 = s.remap[i1];
			const Vector3& current = s.positions[i1];
			const float e0 = quadricError(s.quadrics[r0], p);
			const float e1 = moved ? quadricError(s.quadrics[r1], p) - quadricError(s.quadrics[r1], current) : 0.f;
			float error = e0 > e1 ? e0 : e1;
			const size_t K = s.attribute_count;
			if (K)
			{
				float values[kMaxAttributes];
				for (const MergedWedge& m : merged)
				{
					if (!m.received && !moved)
						continue;
					const float* kept = &s.attributes[size_t(m.target) * K];
					const float* a = kept;
					if (moved)
					{
						for (size_t k = 0; k < K; ++k)
							values[k] = optimalAttribute(m, k, p);
						a = values;
					}
					error += quadricError(m.A, m.G, K, p, a, s.clamped);
					error -= quadricError(s.attribute_quadrics[m.target], &s.attribute_gradients[size_t(m.target) * K], K, current, kept, s.clamped);
				}
			}
			return error > 0.f ? error : 0.f;
		}

		// stage 4: may the target of i0 -> i1 move, and how: 1 anywhere (manifold or complex target), 2 along the edge (a border or
		// a proper seam pair collapsing along its own loop), 0 not at all.
		static int moveMode(const State& s, unsigned int i0, unsigned int i1)
		{
			const unsigned char k0 = s.kind[i0], k1 = s.kind[i1];
			if (k1 == Kind_Manifold || (k1 == Kind_Complex && (k0 == Kind_Manifold || k0 == Kind_Complex)))
				return 1;
			if (k0 == Kind_Border && k1 == Kind_Border && s.wedge[i0] == i0 && s.wedge[i1] == i1 && (s.loop[i0] == i1 || s.loopback[i0] == i1))
				return 2;
			if (k0 == Kind_Seam && k1 == Kind_Seam)
			{
				const unsigned int s0 = s.wedge[i0];
				if (s0 == i0 || s.wedge[s0] != i0)
					return 0;
				const unsigned int s1 = s.loop[i0] == i1 ? s.loopback[s0] : s.loop[s0];
				if (s1 == ~0u || s1 != s.wedge[i1] || s.wedge[s1] != i1 || !(s.loop[i0] == i1 || s.loopback[i0] == i1))
					return 0;
				return 2;
			}
			return 0;
		}

		// stage 4: ranks a collapse by its best placement and keeps the second best.
		static void rankCollapse(const State& s, const OptimalSimplifyParams& params, Collapse& c, std::vector<std::pair<unsigned int, unsigned int> >& pairs, std::vector<MergedWedge>& merged, OptimalSimplifyStats& stats)
		{
			Placement candidates[4];
			int count = 0;

			const unsigned int i0 = c.v0, i1 = c.v1;
			const Vector3 p0 = s.positions[i0], p1 = s.positions[i1];

			// the endpoints (upstream's placements)
			collapsePairs(s, i0, i1, pairs);
			mergeWedges(s, i0, i1, false, pairs, merged);
			candidates[count++] = { i0, i1, p1, placementCost(s, i0, i1, p1, false, merged), 0 };
			if (c.bidi)
			{
				collapsePairs(s, i1, i0, pairs);
				mergeWedges(s, i1, i0, false, pairs, merged);
				candidates[count++] = { i1, i0, p0, placementCost(s, i1, i0, p0, false, merged), 0 };
			}

			// which placements params.moves allows: boundary (border/seam) edges along the edge; interior targets along the edge
			// and/or freely
			const int mode = moveMode(s, i0, i1);
			const bool line = mode == 2 ? (params.moves & OptimalMove_BoundaryLine) != 0 : (mode == 1 && (params.moves & OptimalMove_InteriorLine) != 0);
			const bool freeSolve = mode == 1 && (params.moves & OptimalMove_InteriorFree) != 0;
			if (line || freeSolve)
			{
				collapsePairs(s, i0, i1, pairs);
				mergeWedges(s, i0, i1, true, pairs, merged);

				const unsigned int r0 = s.remap[i0], r1 = s.remap[i1];
				const Vector3 edge = sub(p1, p0);
				const float edge_length_sq = dot(edge, edge);

				// the merged position quadric with the wedges' attribute quadrics eliminated (their optimal values substituted),
				// regularized towards the edge midpoint so flat, attribute-free regions have a unique optimum
				Quadric Q = s.quadrics[r0];
				quadricAdd(Q, s.quadrics[r1]);
				const Vector3 mid = lerp(p0, p1, 0.5f);
				Quadric R;
				quadricFromPoint(R, mid.x, mid.y, mid.z, Q.w * 1e-4f);
				quadricAdd(Q, R);
				QuadricGrad GV = {};
				if (s.attribute_count)
				{
					for (const MergedWedge& m : merged)
						quadricReduceAttributes(Q, m.A, m.G, s.attribute_count);
					if (s.volume)
					{
						GV = s.volume[r0];
						quadricAdd(GV, s.volume[r1]);
					}
				}

				// the optimum on the edge segment: the reduced quadric is a parabola along it
				if (line)
				{
					const float e0 = quadricEval(Q, p0), eh = quadricEval(Q, mid), e1 = quadricEval(Q, p1);
					const float a = 2.f * e1 - 4.f * eh + 2.f * e0;
					const float b = e1 - e0 - a;
					if (a > 0.f)
					{
						const float t = -b / (2.f * a);
						if (t > 0.02f && t < 0.98f)
						{
							const Vector3 p = lerp(p0, p1, t);
							if (finite(p))
								candidates[count++] = { i0, i1, p, placementCost(s, i0, i1, p, true, merged), 2 };
						}
					}
				}

				if (freeSolve)
				{
					Vector3 p;
					if (!quadricSolve(p, Q, GV) || !finite(p))
					{
						stats.solveFailed++;
					}
					else
					{
						// distance from the edge segment, in edge lengths
						const Vector3 d0 = sub(p, p0);
						float t = edge_length_sq > 0.f ? dot(d0, edge) / edge_length_sq : 0.f;
						t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
						const Vector3 off = sub(p, lerp(p0, p1, t));
						if (dot(off, off) > params.clampScale * params.clampScale * edge_length_sq)
							stats.solveClamped++;
						else
							candidates[count++] = { i0, i1, p, placementCost(s, i0, i1, p, true, merged), 1 };
					}
				}
			}

			// best, then the second best (an endpoint when the best moves)
			int best = 0;
			for (int i = 1; i < count; ++i)
				if (candidates[i].error < candidates[best].error)
					best = i;
			int second = -1;
			for (int i = 0; i < count; ++i)
			{
				if (i == best || (candidates[best].move != 0 && candidates[i].move != 0))
					continue;
				if (second < 0 || candidates[i].error < candidates[second].error)
					second = i;
			}

			c.placements[0] = candidates[best];
			c.placement_count = 1;
			if (second >= 0)
				c.placements[c.placement_count++] = candidates[second];
			c.error = candidates[best].error;
		}

		// Per pass: the triangles around each position (by remap root), for neighbourhood checks.
		struct TriangleLists
		{
			std::vector<unsigned int> offsets;
			std::vector<unsigned int> triangles;

			void build(const unsigned int* indices, size_t index_count, const unsigned int* remap, size_t vertex_count)
			{
				offsets.assign(vertex_count + 1, 0);
				for (size_t i = 0; i < index_count; ++i)
					offsets[remap[indices[i]] + 1]++;
				for (size_t v = 0; v < vertex_count; ++v)
					offsets[v + 1] += offsets[v];
				triangles.resize(index_count);
				std::vector<unsigned int> cursor(offsets.begin(), offsets.end() - 1);
				for (size_t i = 0; i < index_count; ++i)
					triangles[cursor[remap[indices[i]]]++] = unsigned(i / 3);
			}
		};

		struct Neighbourhood
		{
			struct Survivor
			{
				unsigned int triangle;
				unsigned int before[3]; // position roots now
				unsigned int after[3];  // after the collapse
				Vector3 incoming;       // reference merged in from removed faces
			};
			struct Removed
			{
				unsigned int triangle;
				unsigned int keep; // the corner that survives (root), or ~0u
			};
			std::vector<Survivor> survivors;
			std::vector<Removed> removed;
		};

		enum CheckResult
		{
			Check_Ok = 0,
			Check_Flip = 1,
			Check_Sheet = 2,
			Check_Vanish = 3,
		};

		// stage 4: validates a placement against the current (in-pass) geometry: rotation per collapse, and orientation against the
		// persistent source-sheet references after the removed faces' references are merged into the faces absorbing them.
		static CheckResult checkPlacement(const State& s, const OptimalSimplifyParams& params, const Placement& placement, const unsigned int* result, const TriangleLists& lists,
		    const unsigned int* collapse_remap, const std::vector<Vector3>& reference, Neighbourhood& n)
		{
			const unsigned int r0 = s.remap[placement.v0], r1 = s.remap[placement.v1];
			n.survivors.clear();
			n.removed.clear();

			const auto visit = [&](unsigned int root, bool skip_r0) {
				for (unsigned int i = lists.offsets[root]; i < lists.offsets[root + 1]; ++i)
				{
					const unsigned int t = lists.triangles[i];
					unsigned int q[3];
					for (int j = 0; j < 3; ++j)
						q[j] = s.remap[collapse_remap[result[t * 3 + j]]];
					if (q[0] == q[1] || q[1] == q[2] || q[0] == q[2])
						continue; // already collapsed this pass
					if (skip_r0 && (q[0] == r0 || q[1] == r0 || q[2] == r0))
						continue; // visited from r0
					unsigned int a[3];
					for (int j = 0; j < 3; ++j)
						a[j] = q[j] == r0 ? r1 : q[j];
					if (a[0] == a[1] || a[1] == a[2] || a[0] == a[2])
					{
						Neighbourhood::Removed rem = { t, ~0u };
						for (int j = 0; j < 3; ++j)
							if (a[j] != r1)
								rem.keep = a[j];
						n.removed.push_back(rem);
						continue;
					}
					Neighbourhood::Survivor sur;
					sur.triangle = t;
					memcpy(sur.before, q, sizeof(q));
					memcpy(sur.after, a, sizeof(a));
					sur.incoming = { 0.f, 0.f, 0.f };
					n.survivors.push_back(sur);
				}
			};
			visit(r0, false);
			visit(r1, true);

			// a collapse that leaves nothing around it deletes the rest of a component: its quadric error at a moved position (an
			// edge midpoint, say) says nothing about the surface that disappears, so only the endpoints may do that
			if (placement.move != 0 && n.survivors.empty())
				return Check_Vanish;

			// removed faces' references go to the survivors sharing their surviving corner (the faces that absorb them), else to all
			for (const Neighbourhood::Removed& rem : n.removed)
			{
				const Vector3& R = reference[rem.triangle];
				size_t receivers = 0;
				for (const Neighbourhood::Survivor& sur : n.survivors)
					receivers += (sur.after[0] == rem.keep || sur.after[1] == rem.keep || sur.after[2] == rem.keep) ? 1 : 0;
				const bool all = receivers == 0;
				if (all)
					receivers = n.survivors.size();
				if (receivers == 0)
					continue;
				const float share = 1.f / float(receivers);
				for (Neighbourhood::Survivor& sur : n.survivors)
					if (all || sur.after[0] == rem.keep || sur.after[1] == rem.keep || sur.after[2] == rem.keep)
					{
						sur.incoming.x += R.x * share;
						sur.incoming.y += R.y * share;
						sur.incoming.z += R.z * share;
					}
			}

			const auto at = [&](unsigned int root, bool after) -> Vector3 { return (after && root == r1) ? placement.p : s.positions[root]; };
			CheckResult outcome = Check_Ok;
			for (const Neighbourhood::Survivor& sur : n.survivors)
			{
				const Vector3 o0 = at(sur.before[0], false), o1 = at(sur.before[1], false), o2 = at(sur.before[2], false);
				const Vector3 a0 = at(sur.after[0], true), a1 = at(sur.after[1], true), a2 = at(sur.after[2], true);
				const Vector3 nold = cross(sub(o1, o0), sub(o2, o0));
				const Vector3 nnew = cross(sub(a1, a0), sub(a2, a0));
				const float lold = dot(nold, nold), lnew = dot(nnew, nnew);

				// upstream's test (hasTriangleFlip), with the moved target; a degenerate result fails it
				if (dot(nold, nnew) <= params.flipCos * sqrtf(lold * lnew))
					return Check_Flip;

				const Vector3& R0 = reference[sur.triangle];
				const Vector3 R1 = { R0.x + sur.incoming.x, R0.y + sur.incoming.y, R0.z + sur.incoming.z };
				const float lr0 = dot(R0, R0), lr1 = dot(R1, R1);
				if (!(lr1 > 0.f))
					continue;
				const float cos_new = dot(nnew, R1) / sqrtf(lnew * lr1);
				if (cos_new > params.sheetCos)
					continue;
				// inherited, not new: a face already this far from its reference may change if it does not get worse
				const float cos_old = (lold > 0.f && lr0 > 0.f) ? dot(nold, R0) / sqrtf(lold * lr0) : -1.f;
				if (cos_new < cos_old - 1e-4f)
					outcome = Check_Sheet;
			}
			return outcome;
		}

		static size_t remapIndexBuffer(unsigned int* indices, size_t index_count, const unsigned int* collapse_remap, const unsigned int* remap, std::vector<Vector3>& reference)
		{
			size_t write = 0;
			for (size_t i = 0; i < index_count; i += 3)
			{
				unsigned int v0 = collapse_remap[indices[i + 0]];
				unsigned int v1 = collapse_remap[indices[i + 1]];
				unsigned int v2 = collapse_remap[indices[i + 2]];

				unsigned int r0 = remap[v0];
				unsigned int r1 = remap[v1];
				unsigned int r2 = remap[v2];

				if (r0 != r1 && r0 != r2 && r1 != r2)
				{
					indices[write + 0] = v0;
					indices[write + 1] = v1;
					indices[write + 2] = v2;
					// stage 4: the persistent references follow their triangles
					reference[write / 3] = reference[i / 3];
					write += 3;
				}
			}
			return write;
		}

	}

	size_t SimplifyOptimal(
		unsigned int* indices, size_t index_count, float* positions_data, size_t vertex_count,
		float* attributes_data, size_t attributes_stride, const float* attribute_weights, size_t attribute_count,
		const unsigned char* vertex_lock, const float* triangle_reference, size_t target_index_count, unsigned int options,
		const OptimalSimplifyParams& params, float* out_error, OptimalSimplifyStats* out_stats)
	{
		using namespace optimal;
		ZoneScopedN("ClusterLODOptimalSimplify::SimplifyOptimal");
		assert(index_count % 3 == 0);
		assert(attribute_count <= kMaxAttributes);

		OptimalSimplifyStats stats;
		std::vector<unsigned int> result_storage(indices, indices + index_count);
		unsigned int* result = result_storage.data();

		EdgeAdjacency adjacency;
		updateEdgeAdjacency(adjacency, result, index_count, vertex_count, nullptr);

		std::vector<unsigned int> remap(vertex_count), wedge(vertex_count);
		buildPositionRemap(remap.data(), wedge.data(), positions_data, vertex_count);

		std::vector<unsigned char> vertex_kind(vertex_count);
		std::vector<unsigned int> loop(vertex_count), loopback(vertex_count);
		classifyVertices(vertex_kind.data(), loop.data(), loopback.data(), vertex_count, adjacency, remap.data(), wedge.data(), vertex_lock, options);

		// rescale positions into the unit cube, as upstream's rescalePositions
		std::vector<Vector3> vertex_positions(vertex_count);
		float minv[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, maxv[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (size_t i = 0; i < vertex_count; ++i)
			for (int j = 0; j < 3; ++j)
			{
				minv[j] = std::min(minv[j], positions_data[i * 3 + j]);
				maxv[j] = std::max(maxv[j], positions_data[i * 3 + j]);
			}
		float vertex_scale = 0.f;
		for (int j = 0; j < 3; ++j)
			vertex_scale = std::max(vertex_scale, maxv[j] - minv[j]);
		const float inverse_scale = vertex_scale == 0.f ? 0.f : 1.f / vertex_scale;
		for (size_t i = 0; i < vertex_count; ++i)
		{
			vertex_positions[i].x = (positions_data[i * 3 + 0] - minv[0]) * inverse_scale;
			vertex_positions[i].y = (positions_data[i * 3 + 1] - minv[1]) * inverse_scale;
			vertex_positions[i].z = (positions_data[i * 3 + 2] - minv[2]) * inverse_scale;
		}

		// attributes with weight > 0, scaled by their weights, as upstream's rescaleAttributes
		unsigned int attribute_remap[kMaxAttributes];
		size_t attributes_used = 0;
		for (size_t i = 0; i < attribute_count; ++i)
			if (attribute_weights[i] > 0)
				attribute_remap[attributes_used++] = unsigned(i);
		attribute_count = attributes_used;
		std::vector<float> vertex_attributes(vertex_count * attribute_count);
		for (size_t i = 0; i < vertex_count; ++i)
			for (size_t k = 0; k < attribute_count; ++k)
				vertex_attributes[i * attribute_count + k] = attributes_data[i * attributes_stride + attribute_remap[k]] * attribute_weights[attribute_remap[k]];

		std::vector<Quadric> vertex_quadrics(vertex_count, Quadric{});
		std::vector<Quadric> attribute_quadrics(attribute_count ? vertex_count : 0, Quadric{});
		std::vector<QuadricGrad> attribute_gradients(vertex_count * attribute_count, QuadricGrad{});
		std::vector<QuadricGrad> volume_gradients(attribute_count ? vertex_count : 0, QuadricGrad{});

		fillFaceQuadrics(vertex_quadrics.data(), attribute_count ? volume_gradients.data() : nullptr, result, index_count, vertex_positions.data(), remap.data());
		fillVertexQuadrics(vertex_quadrics.data(), vertex_positions.data(), vertex_count, remap.data(), vertex_lock, options);
		fillEdgeQuadrics(vertex_quadrics.data(), result, index_count, vertex_positions.data(), remap.data(), vertex_kind.data(), loop.data(), loopback.data());
		if ((options & meshopt_SimplifyPreserveFolds) || params.preserveFolds)
			fillFoldQuadrics(vertex_quadrics.data(), adjacency, vertex_positions.data(), vertex_count, remap.data(), wedge.data());
		if (attribute_count)
			fillAttributeQuadrics(attribute_quadrics.data(), attribute_gradients.data(), result, index_count, vertex_positions.data(), vertex_attributes.data(), attribute_count);

		// stage 4: persistent source-sheet references, per triangle
		std::vector<Vector3> reference(index_count / 3);
		for (size_t t = 0; t < index_count / 3; ++t)
		{
			if (triangle_reference)
			{
				reference[t] = { triangle_reference[t * 3 + 0], triangle_reference[t * 3 + 1], triangle_reference[t * 3 + 2] };
			}
			else
			{
				const Vector3& a = vertex_positions[result[t * 3 + 0]];
				reference[t] = cross(sub(vertex_positions[result[t * 3 + 1]], a), sub(vertex_positions[result[t * 3 + 2]], a));
			}
		}

		State state;
		state.vertex_count = vertex_count;
		state.attribute_count = attribute_count;
		state.remap = remap.data();
		state.wedge = wedge.data();
		state.loop = loop.data();
		state.loopback = loopback.data();
		state.kind = vertex_kind.data();
		state.positions = vertex_positions.data();
		state.attributes = attribute_count ? vertex_attributes.data() : nullptr;
		state.quadrics = vertex_quadrics.data();
		state.volume = attribute_count ? volume_gradients.data() : nullptr;
		state.attribute_quadrics = attribute_count ? attribute_quadrics.data() : nullptr;
		state.attribute_gradients = attribute_count ? attribute_gradients.data() : nullptr;
		state.clamped = (options & meshopt_SimplifyErrorClamped) != 0;

		const size_t collapse_capacity = boundEdgeCollapses(adjacency, vertex_count, index_count, vertex_kind.data());
		std::vector<Collapse> edge_collapses(collapse_capacity);
		std::vector<unsigned int> collapse_order(collapse_capacity);
		std::vector<unsigned int> collapse_remap(vertex_count);
		std::vector<unsigned char> collapse_locked(vertex_count);
		std::vector<unsigned char> moved(vertex_count, 0);
		std::vector<unsigned char> moved_pass(vertex_count, 0);
		std::vector<unsigned int> moved_roots;

		std::vector<std::pair<unsigned int, unsigned int> > pairs;
		std::vector<MergedWedge> merged;
		TriangleLists lists;
		Neighbourhood neighbourhood;

		size_t result_count = index_count;
		float result_error = 0;

		const float error_scale = (options & meshopt_SimplifyErrorAbsolute) ? vertex_scale : 1.f;

		while (result_count > target_index_count)
		{
			// stage 4: the triangles around each position replace upstream's per-pass adjacency (used only for the flip test)
			lists.build(result, result_count, remap.data(), vertex_count);

			const size_t edge_collapse_count = pickEdgeCollapses(edge_collapses.data(), collapse_capacity, result, result_count, remap.data(), vertex_kind.data(), loop.data(), loopback.data());
			if (edge_collapse_count == 0)
				break;

			for (size_t i = 0; i < edge_collapse_count; ++i)
				rankCollapse(state, params, edge_collapses[i], pairs, merged, stats);

			sortEdgeCollapses(collapse_order.data(), edge_collapses.data(), edge_collapse_count);

			const size_t triangle_collapse_goal = (result_count - target_index_count) / 3;

			for (size_t i = 0; i < vertex_count; ++i)
				collapse_remap[i] = unsigned(i);
			memset(collapse_locked.data(), 0, vertex_count);
			moved_roots.clear();

			// performEdgeCollapses, with placements
			size_t edge_collapses_done = 0;
			size_t triangle_collapses = 0;
			size_t edge_collapse_goal = triangle_collapse_goal / 2;
			for (size_t i = 0; i < edge_collapse_count; ++i)
			{
				const Collapse& c = edge_collapses[collapse_order[i]];

				if (triangle_collapses >= triangle_collapse_goal)
					break;

				const float error_goal = edge_collapse_goal < edge_collapse_count ? 1.5f * edge_collapses[collapse_order[edge_collapse_goal]].error : FLT_MAX;
				if (c.error > error_goal && c.error > result_error && triangle_collapses > triangle_collapse_goal / 6)
					break;

				const unsigned int r0 = remap[c.v0];
				const unsigned int r1 = remap[c.v1];
				if (collapse_locked[r0] | collapse_locked[r1])
					continue;

				const Placement* chosen = nullptr;
				for (unsigned char k = 0; k < c.placement_count && !chosen; ++k)
				{
					const Placement& placement = c.placements[k];
					if (k > 0 && placement.error > error_goal)
						break;
					const CheckResult check = checkPlacement(state, params, placement, result, lists, collapse_remap.data(), reference, neighbourhood);
					if (check == Check_Ok)
					{
						chosen = &placement;
						stats.fallbacks += k > 0 ? 1 : 0;
					}
					else
					{
						(check == Check_Flip ? stats.rejectedFlip : (check == Check_Sheet ? stats.rejectedSheet : stats.rejectedVanish))++;
					}
				}
				if (!chosen)
				{
					edge_collapse_goal++;
					continue;
				}

				const unsigned int i0 = chosen->v0;
				const unsigned int i1 = chosen->v1;
				const unsigned char kind = vertex_kind[i0];

				if (kind == Kind_Complex || kind == Kind_Fringe)
				{
					collapse_remap[i0] = i1;
					for (unsigned int v = wedge[i0]; v != i0; v = wedge[v])
						collapse_remap[v] = getComplexTarget(v, i1, remap.data(), loop.data(), loopback.data());
				}
				else if (kind == Kind_Seam)
				{
					unsigned int s0 = wedge[i0];
					unsigned int s1 = loop[i0] == i1 ? loopback[s0] : loop[s0];
					s1 = (s1 != ~0u) ? s1 : wedge[i1];
					collapse_remap[i0] = i1;
					collapse_remap[s0] = s1;
				}
				else
				{
					collapse_remap[i0] = i1;
				}

				collapse_locked[r0] = 1;
				collapse_locked[r1] = 1;

				// stage 4: the removed faces' references now belong to the faces that absorbed them (checkPlacement left the
				// accepted placement's neighbourhood in neighbourhood)
				for (const Neighbourhood::Survivor& sur : neighbourhood.survivors)
				{
					Vector3& R = reference[sur.triangle];
					R.x += sur.incoming.x;
					R.y += sur.incoming.y;
					R.z += sur.incoming.z;
				}
				for (const Neighbourhood::Removed& rem : neighbourhood.removed)
					reference[rem.triangle] = { 0.f, 0.f, 0.f };

				// stage 4: the target takes its placement; every wedge of it moves
				if (chosen->move != 0)
				{
					unsigned int v = i1;
					do
					{
						vertex_positions[v] = chosen->p;
						moved[v] = 1;
						v = wedge[v];
					} while (v != i1);
					if (!moved_pass[r1])
					{
						moved_pass[r1] = 1;
						moved_roots.push_back(r1);
					}
					(chosen->move == 1 ? stats.movedFree : stats.movedLine)++;
				}

				triangle_collapses += (kind == Kind_Border) ? 1 : 2;
				edge_collapses_done++;
				stats.collapses++;
				result_error = result_error < chosen->error ? chosen->error : result_error;
			}

			if (edge_collapses_done == 0)
				break;

			// updateQuadrics (upstream), without its vertex error (only pruning reads it)
			for (size_t i = 0; i < vertex_count; ++i)
			{
				if (collapse_remap[i] == i)
					continue;

				unsigned int i0 = unsigned(i);
				unsigned int i1 = collapse_remap[i];
				unsigned int r0 = remap[i0];
				unsigned int r1 = remap[i1];

				if (i0 == r0)
				{
					quadricAdd(vertex_quadrics[r1], vertex_quadrics[r0]);
					if (attribute_count)
						quadricAdd(volume_gradients[r1], volume_gradients[r0]);
				}

				if (attribute_count)
				{
					quadricAdd(attribute_quadrics[i1], attribute_quadrics[i0]);
					quadricAdd(&attribute_gradients[i1 * attribute_count], &attribute_gradients[i0 * attribute_count], attribute_count);
				}
			}

			// stage 4: the moved targets' wedge attributes, solved at their new position (upstream's solveAttributes rule: a complex
			// vertex keeps attributes its wedges shared, from the heaviest wedge)
			if (attribute_count)
				for (unsigned int r : moved_roots)
				{
					const Vector3& p = vertex_positions[r];
					for (size_t k = 0; k < attribute_count; ++k)
					{
						unsigned int shared = ~0u;
						if (vertex_kind[r] == Kind_Complex || vertex_kind[r] == Kind_Fringe)
						{
							shared = r;
							for (unsigned int v = wedge[r]; v != r; v = wedge[v])
								if (vertex_attributes[v * attribute_count + k] != vertex_attributes[r * attribute_count + k])
									shared = ~0u;
								else if (shared != ~0u && attribute_quadrics[v].w > attribute_quadrics[shared].w)
									shared = v;
						}
						unsigned int v = r;
						do
						{
							const unsigned int source = (shared == ~0u) ? v : shared;
							const Quadric& A = attribute_quadrics[source];
							const QuadricGrad& G = attribute_gradients[source * attribute_count + k];
							const float iw = A.w == 0 ? 0.f : 1.f / A.w;
							vertex_attributes[v * attribute_count + k] = (G.gx * p.x + G.gy * p.y + G.gz * p.z + G.gw) * iw;
							v = wedge[v];
						} while (v != r);
					}
				}
			for (unsigned int r : moved_roots)
				moved_pass[r] = 0;

			remapEdgeLoops(loop.data(), vertex_count, collapse_remap.data());
			remapEdgeLoops(loopback.data(), vertex_count, collapse_remap.data());

			result_count = remapIndexBuffer(result, result_count, collapse_remap.data(), remap.data(), reference);
		}

		// write moved vertices back (unmoved ones keep their exact bits)
		for (size_t i = 0; i < vertex_count; ++i)
		{
			if (!moved[i] || vertex_kind[i] == Kind_Locked || (vertex_lock && (vertex_lock[i] & meshopt_SimplifyVertex_Lock)))
				continue;
			const Vector3& p = vertex_positions[i];
			positions_data[i * 3 + 0] = p.x * vertex_scale + minv[0];
			positions_data[i * 3 + 1] = p.y * vertex_scale + minv[1];
			positions_data[i * 3 + 2] = p.z * vertex_scale + minv[2];
			for (size_t k = 0; k < attribute_count; ++k)
			{
				const unsigned int rk = attribute_remap[k];
				attributes_data[i * attributes_stride + rk] = vertex_attributes[i * attribute_count + k] / attribute_weights[rk];
			}
		}

		memcpy(indices, result, result_count * sizeof(unsigned int));
		if (out_error)
			*out_error = sqrtf(result_error) * error_scale;
		if (out_stats)
		{
			out_stats->collapses += stats.collapses;
			out_stats->movedFree += stats.movedFree;
			out_stats->movedLine += stats.movedLine;
			out_stats->fallbacks += stats.fallbacks;
			out_stats->rejectedFlip += stats.rejectedFlip;
			out_stats->rejectedSheet += stats.rejectedSheet;
			out_stats->solveFailed += stats.solveFailed;
			out_stats->solveClamped += stats.solveClamped;
			out_stats->rejectedVanish += stats.rejectedVanish;
		}
		return result_count;
	}

	namespace {

		// Reference samples per input triangle (barycentric), each standing for a quarter of its area, as the validator samples.
		constexpr float kReferenceSamples[4][3] = {
			{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f },
			{ 4.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 4.0f / 6.0f, 1.0f / 6.0f },
			{ 1.0f / 6.0f, 1.0f / 6.0f, 4.0f / 6.0f },
		};

	}

	OptimalSimplifier::OptimalSimplifier(const SimplifyValidator& original, const OptimalSimplifyParams& params)
		: m_original(original)
		, m_params(params)
	{
	}

	size_t OptimalSimplifier::Simplify(const clodMesh&, clodOptimalRequest& request) const
	{
		ZoneScopedN("ClusterLODOptimalSimplify::Simplify");
		m_calls.fetch_add(1, std::memory_order_relaxed);

		// The source-sheet reference of every input triangle: the original's face orientation under it, sampled where the
		// validator samples and weighted by area. A group's candidates share their input, so clusterlod keeps it across targets.
		if (request.triangle_reference != nullptr && !request.triangle_reference_ready)
		{
			const auto start = std::chrono::steady_clock::now();
			uint64_t samples = 0, unmatched = 0;
			for (size_t t = 0; t < request.index_count / 3u; ++t)
			{
				const float* p[3];
				for (int j = 0; j < 3; ++j)
				{
					p[j] = &request.positions[static_cast<size_t>(request.indices[t * 3u + j]) * 3u];
				}
				const float ab[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
				const float ac[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
				float face[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
				const float twiceArea = std::sqrt(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]);
				float* out = &request.triangle_reference[t * 3u];
				out[0] = out[1] = out[2] = 0.0f;
				if (!(twiceArea > 0.0f))
				{
					continue;
				}
				for (float& axis : face)
				{
					axis /= twiceArea;
				}
				const float quarter = 0.125f * twiceArea;
				for (const auto& sample : kReferenceSamples)
				{
					float point[3];
					for (int axis = 0; axis < 3; ++axis)
					{
						point[axis] = p[0][axis] * sample[0] + p[1][axis] * sample[1] + p[2][axis] * sample[2];
					}
					float normal[3];
					++samples;
					if (!m_original.OriginalFaceNormalAt(point, face, normal))
					{
						++unmatched;
						std::memcpy(normal, face, sizeof(normal));
					}
					for (int axis = 0; axis < 3; ++axis)
					{
						out[axis] += normal[axis] * quarter;
					}
				}
			}
			request.triangle_reference_ready = 1;
			m_referenceSamples.fetch_add(samples, std::memory_order_relaxed);
			m_referenceUnmatched.fetch_add(unmatched, std::memory_order_relaxed);
			m_referenceNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
		}

		const auto start = std::chrono::steady_clock::now();
		OptimalSimplifyStats stats;
		const size_t count = SimplifyOptimal(request.indices, request.index_count, request.positions, request.vertex_count,
			request.attributes, request.attributes_stride / sizeof(float), request.attribute_weights, request.attribute_count,
			request.locks, request.triangle_reference, request.target_index_count, request.options, m_params, &request.error, &stats);
		m_loopNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);

		const uint64_t values[9] = { stats.collapses, stats.movedFree, stats.movedLine, stats.fallbacks, stats.rejectedFlip, stats.rejectedSheet, stats.solveFailed, stats.solveClamped, stats.rejectedVanish };
		for (int i = 0; i < 9; ++i)
		{
			m_counters[i].fetch_add(values[i], std::memory_order_relaxed);
		}
		return count;
	}

	OptimalSimplifier::Stats OptimalSimplifier::GetStats() const
	{
		Stats stats;
		stats.loop.collapses = m_counters[0].load();
		stats.loop.movedFree = m_counters[1].load();
		stats.loop.movedLine = m_counters[2].load();
		stats.loop.fallbacks = m_counters[3].load();
		stats.loop.rejectedFlip = m_counters[4].load();
		stats.loop.rejectedSheet = m_counters[5].load();
		stats.loop.solveFailed = m_counters[6].load();
		stats.loop.solveClamped = m_counters[7].load();
		stats.loop.rejectedVanish = m_counters[8].load();
		stats.calls = m_calls.load();
		stats.referenceSamples = m_referenceSamples.load();
		stats.referenceUnmatched = m_referenceUnmatched.load();
		stats.referenceNs = m_referenceNs.load();
		stats.loopNs = m_loopNs.load();
		return stats;
	}

}
