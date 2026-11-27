#include "Hair/GroomBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <span>
#include <utility>

namespace br::hair {

namespace {

    constexpr float kFloatMax = std::numeric_limits<float>::max();
    constexpr float kPi = 3.14159265358979323846f;

    float DistanceSquared(Float3 a, Float3 b)
    {
        const Float3 d = a - b;
        return Dot(d, d);
    }

    float Component(Float3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

    // Uniform in [0, 1), a fixed function of its arguments (splitmix64 finalizer).
    float RandomUnit(std::uint32_t seed, std::uint32_t stream, std::uint32_t index)
    {
        std::uint64_t z = ((static_cast<std::uint64_t>(seed) << 32) | index) + 0x9E3779B97F4A7C15ull * (std::uint64_t{ stream } + 1u);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
    }

    // Uniform grid over a point set, for ball queries and nearest-neighbour searches. Roots lie on a surface, so
    // the resolution is chosen for a few points per occupied cell of a 2D sheet.
    class PointGrid {
    public:
        explicit PointGrid(std::span<const Float3> points)
            : m_points(points.begin(), points.end())
        {
            const std::size_t count = m_points.size();
            if (count == 0u) {
                return;
            }
            Float3 lo = m_points[0], hi = m_points[0];
            for (const Float3 point : m_points) {
                lo = Min(lo, point);
                hi = Max(hi, point);
            }
            m_min = lo;
            const Float3 extent = hi - lo;
            const float maxExtent = std::max({ extent.x, extent.y, extent.z });
            const float resolution = std::clamp(std::ceil(0.5f * std::sqrt(static_cast<float>(count))), 1.0f, 128.0f);
            m_cellSize = maxExtent > 0.0f ? maxExtent / resolution : 1.0f;
            for (int axis = 0; axis < 3; ++axis) {
                m_dims[axis] = static_cast<std::uint32_t>(std::min(resolution, std::floor(Component(extent, axis) / m_cellSize))) + 1u;
            }

            // Counting sort of the points by cell.
            const std::size_t cellCount = std::size_t{ m_dims[0] } * m_dims[1] * m_dims[2];
            m_cellStart.assign(cellCount + 1u, 0u);
            std::vector<std::uint32_t> cells(count);
            for (std::size_t i = 0; i < count; ++i) {
                const Float3 point = m_points[i];
                cells[i] = (CellCoord(point.z, 2) * m_dims[1] + CellCoord(point.y, 1)) * m_dims[0] + CellCoord(point.x, 0);
                ++m_cellStart[cells[i] + 1u];
            }
            for (std::size_t cell = 0; cell < cellCount; ++cell) {
                m_cellStart[cell + 1u] += m_cellStart[cell];
            }
            m_cellPoints.resize(count);
            std::vector<std::uint32_t> cursor(m_cellStart.begin(), m_cellStart.end() - 1);
            for (std::size_t i = 0; i < count; ++i) {
                m_cellPoints[cursor[cells[i]]++] = static_cast<std::uint32_t>(i);
            }
        }

        // Calls visit(index, distanceSquared) for every point within `radius` of `center`.
        template <typename Visit>
        void ForEachWithin(Float3 center, float radius, Visit&& visit) const
        {
            const float radiusSquared = radius * radius;
            const auto test = [&](std::uint32_t index) {
                const float distanceSquared = DistanceSquared(m_points[index], center);
                if (distanceSquared <= radiusSquared) {
                    visit(index, distanceSquared);
                }
            };

            std::uint32_t lo[3], hi[3];
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = CellCoord(Component(center, axis) - radius, axis);
                hi[axis] = CellCoord(Component(center, axis) + radius, axis);
            }
            const std::size_t cellsInRange = std::size_t{ hi[0] - lo[0] + 1u } * (hi[1] - lo[1] + 1u) * (hi[2] - lo[2] + 1u);
            if (cellsInRange >= m_points.size()) {
                // Walking that many mostly empty cells costs more than testing every point.
                for (std::uint32_t index = 0; index < m_points.size(); ++index) {
                    test(index);
                }
                return;
            }
            for (std::uint32_t z = lo[2]; z <= hi[2]; ++z) {
                for (std::uint32_t y = lo[1]; y <= hi[1]; ++y) {
                    // Cells of one row are consecutive, so the row's points are one run.
                    const std::size_t row = (std::size_t{ z } * m_dims[1] + y) * m_dims[0];
                    for (std::uint32_t i = m_cellStart[row + lo[0]]; i < m_cellStart[row + hi[0] + 1u]; ++i) {
                        test(m_cellPoints[i]);
                    }
                }
            }
        }

        struct Nearest {
            std::uint32_t count = 0u;
            std::uint32_t indices[3] = {};
            float distances[3] = {};
        };

        // The (up to) three points nearest to `center`, nearest first.
        Nearest FindNearest3(Float3 center) const
        {
            const std::uint32_t wanted = static_cast<std::uint32_t>(std::min<std::size_t>(3u, m_points.size()));
            float radius = m_cellSize;
            for (;;) {
                Nearest nearest;
                ForEachWithin(center, radius, [&nearest](std::uint32_t index, float distanceSquared) {
                    std::uint32_t slot = nearest.count;
                    if (slot == 3u) {
                        if (distanceSquared >= nearest.distances[2]) {
                            return;
                        }
                        slot = 2u;
                    } else {
                        ++nearest.count;
                    }
                    for (; slot > 0u && nearest.distances[slot - 1u] > distanceSquared; --slot) {
                        nearest.indices[slot] = nearest.indices[slot - 1u];
                        nearest.distances[slot] = nearest.distances[slot - 1u];
                    }
                    nearest.indices[slot] = index;
                    nearest.distances[slot] = distanceSquared;
                });
                // Every point within the radius was visited, so three hits are the three nearest overall.
                if (nearest.count == wanted) {
                    for (std::uint32_t i = 0; i < nearest.count; ++i) {
                        nearest.distances[i] = std::sqrt(nearest.distances[i]);
                    }
                    return nearest;
                }
                radius *= 2.0f;
            }
        }

    private:
        std::uint32_t CellCoord(float value, int axis) const
        {
            const float cell = std::floor((value - Component(m_min, axis)) / m_cellSize);
            const float last = static_cast<float>(m_dims[axis] - 1u);
            return !(cell > 0.0f) ? 0u : cell >= last ? m_dims[axis] - 1u : static_cast<std::uint32_t>(cell);
        }

        std::vector<Float3> m_points;
        Float3 m_min;
        float m_cellSize = 1.0f;
        std::uint32_t m_dims[3] = { 1u, 1u, 1u };
        std::vector<std::uint32_t> m_cellStart;
        std::vector<std::uint32_t> m_cellPoints;
    };

    // Greedy farthest-point ordering of the roots: each next strand is the one farthest from all strands already
    // ordered, so every prefix covers the roots evenly. Flagged strands are ordered first, among themselves the
    // same way. Returns rank -> source strand.
    //
    // Adding a point can only lower the distance of points closer to it than the current largest distance, so each
    // step updates a ball of that radius; the ball shrinks as the ordering fills in, which keeps the whole thing
    // near O(n log n).
    std::vector<std::uint32_t> RankStrands(std::span<const Float3> roots, std::span<const std::uint8_t> guideFlags)
    {
        const std::uint32_t count = static_cast<std::uint32_t>(roots.size());
        const PointGrid grid(roots);
        std::vector<float> distanceSquared(count, kFloatMax); // to the nearest ordered root

        // Max-heaps on distance with lazily lowered keys; ties go to the lower strand index.
        using Entry = std::pair<float, std::uint32_t>;
        const auto lower = [](const Entry& a, const Entry& b) { return a.first != b.first ? a.first < b.first : a.second > b.second; };
        using Heap = std::priority_queue<Entry, std::vector<Entry>, decltype(lower)>;
        Heap heaps[2] = { Heap(lower), Heap(lower) }; // 0: flagged guides, 1: the rest
        for (std::uint32_t strand = 0; strand < count; ++strand) {
            const bool flagged = !guideFlags.empty() && guideFlags[strand] != 0u;
            heaps[flagged ? 0 : 1].push({ kFloatMax, strand });
        }

        std::vector<std::uint32_t> order;
        order.reserve(count);
        while (order.size() < count) {
            float largest = 0.0f;
            for (Heap& heap : heaps) {
                // Bring the top up to date: a stale key is replaced by the current distance and re-sorted.
                while (!heap.empty() && heap.top().first > distanceSquared[heap.top().second]) {
                    const std::uint32_t strand = heap.top().second;
                    heap.pop();
                    heap.push({ distanceSquared[strand], strand });
                }
                if (!heap.empty()) {
                    largest = std::max(largest, heap.top().first);
                }
            }
            Heap& heap = heaps[0].empty() ? heaps[1] : heaps[0];
            const std::uint32_t strand = heap.top().second;
            heap.pop();
            order.push_back(strand);

            // 1.001 covers the rounding of the square root.
            grid.ForEachWithin(roots[strand], std::sqrt(largest) * 1.001f, [&](std::uint32_t other, float d) {
                distanceSquared[other] = std::min(distanceSquared[other], d);
            });
        }
        return order;
    }

    // Splits `strands` in half along the widest feature axis until every part fits a cluster. Appends the end
    // offset of each cluster to `clusterEnds`; `strands` is reordered so clusters are contiguous.
    void SplitIntoClusters(std::span<std::uint32_t> strands,
                           std::uint32_t offset,
                           const std::vector<std::array<float, 6>>& features,
                           std::vector<std::uint32_t>& clusterEnds)
    {
        if (strands.size() <= HAIR_MAX_STRANDS_PER_CLUSTER) {
            clusterEnds.push_back(offset + static_cast<std::uint32_t>(strands.size()));
            return;
        }
        int widestAxis = 0;
        float widest = -1.0f;
        for (int axis = 0; axis < 6; ++axis) {
            float lo = kFloatMax, hi = -kFloatMax;
            for (const std::uint32_t strand : strands) {
                lo = std::min(lo, features[strand][axis]);
                hi = std::max(hi, features[strand][axis]);
            }
            if (hi - lo > widest) {
                widest = hi - lo;
                widestAxis = axis;
            }
        }
        const std::size_t half = strands.size() / 2u;
        std::nth_element(strands.begin(), strands.begin() + half, strands.end(), [&](std::uint32_t a, std::uint32_t b) {
            const float fa = features[a][widestAxis], fb = features[b][widestAxis];
            return fa != fb ? fa < fb : a < b;
        });
        SplitIntoClusters(strands.first(half), offset, features, clusterEnds);
        SplitIntoClusters(strands.subspan(half), offset + static_cast<std::uint32_t>(half), features, clusterEnds);
    }

    // Smallest k with 2^k >= value, at most HAIR_MAX_VERTEX_LEVEL.
    std::uint32_t CeilLog2(float value)
    {
        std::uint32_t k = 0u;
        while (k < HAIR_MAX_VERTEX_LEVEL && static_cast<float>(1u << k) < value) {
            ++k;
        }
        return k;
    }

    // The vertex level one strand asks for: one segment per maxSegmentAngle of turning, but no more segments than
    // the source polyline has.
    std::uint32_t StrandVertexLevel(const GroomSource& source, std::uint32_t strand, float maxSegmentAngle)
    {
        const std::uint32_t begin = source.StrandPointOffset(strand);
        const std::uint32_t pointCount = source.StrandPointCount(strand);
        float turning = 0.0f;
        Float3 previous{};
        bool hasPrevious = false;
        for (std::uint32_t i = 0; i + 1u < pointCount; ++i) {
            const Float3 segment = source.positions[begin + i + 1u] - source.positions[begin + i];
            if (!(Length(segment) > 0.0f)) {
                continue;
            }
            const Float3 direction = Normalize(segment);
            if (hasPrevious) {
                turning += std::acos(std::clamp(Dot(previous, direction), -1.0f, 1.0f));
            }
            previous = direction;
            hasPrevious = true;
        }
        return std::min(CeilLog2(turning / maxSegmentAngle), CeilLog2(static_cast<float>(pointCount - 1u)));
    }

    // Unit direction a strand leaves its root in: the first segment with any length, +Y for a zero-length strand.
    Float3 StrandRootDirection(const GroomSource& source, std::uint32_t strand)
    {
        const std::uint32_t begin = source.StrandPointOffset(strand);
        for (std::uint32_t i = 1; i < source.StrandPointCount(strand); ++i) {
            const Float3 direction = Normalize(source.positions[begin + i] - source.positions[begin]);
            if (Dot(direction, direction) > 0.0f) {
                return direction;
            }
        }
        return { 0.0f, 1.0f, 0.0f };
    }

    // Keeps the four largest of the triangle's barycentric-weighted vertex influences.
    void InterpolateSkin(const ScalpMesh& scalp, std::uint32_t triangle, float u, float v, HairRootBinding& binding)
    {
        const float barycentrics[3] = { 1.0f - u - v, u, v };
        std::uint32_t joints[12];
        float weights[12];
        std::uint32_t count = 0u;
        for (std::uint32_t corner = 0; corner < 3u; ++corner) {
            const std::uint32_t vertex = scalp.indices[triangle * 3u + corner];
            for (std::uint32_t slot = 0; slot < 4u; ++slot) {
                const float weight = scalp.weights[vertex][slot] * barycentrics[corner];
                if (!(weight > 0.0f)) {
                    continue;
                }
                const std::uint32_t joint = scalp.joints[vertex][slot];
                std::uint32_t entry = 0u;
                while (entry < count && joints[entry] != joint) {
                    ++entry;
                }
                if (entry == count) {
                    joints[count] = joint;
                    weights[count++] = 0.0f;
                }
                weights[entry] += weight;
            }
        }
        float total = 0.0f;
        for (std::uint32_t slot = 0; slot < 4u && count > 0u; ++slot) {
            const std::uint32_t best = static_cast<std::uint32_t>(std::max_element(weights, weights + count) - weights);
            if (!(weights[best] > 0.0f)) {
                break;
            }
            binding.joints[slot] = joints[best];
            binding.weights[slot] = weights[best];
            total += weights[best];
            weights[best] = 0.0f;
        }
        if (!(total > 0.0f)) {
            binding.joints[0] = 0u;
            binding.weights[0] = 1.0f;
            return;
        }
        for (float& weight : binding.weights) {
            weight /= total;
        }
    }

    // Per-triangle bounding spheres, so the nearest-triangle scan can skip most triangles with one comparison.
    struct ScalpTriangleBounds {
        std::vector<Float3> centers;
        std::vector<float> radii;

        explicit ScalpTriangleBounds(const ScalpMesh& scalp)
        {
            const std::size_t triangleCount = scalp.indices.size() / 3u;
            centers.resize(triangleCount);
            radii.resize(triangleCount);
            for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                const Float3 a = scalp.positions[scalp.indices[triangle * 3u]];
                const Float3 b = scalp.positions[scalp.indices[triangle * 3u + 1u]];
                const Float3 c = scalp.positions[scalp.indices[triangle * 3u + 2u]];
                const Float3 center = (a + b + c) * (1.0f / 3.0f);
                centers[triangle] = center;
                radii[triangle] = std::sqrt(std::max({ DistanceSquared(a, center), DistanceSquared(b, center), DistanceSquared(c, center) }));
            }
        }
    };

    void BindRootToScalp(const ScalpMesh& scalp, const ScalpTriangleBounds& bounds, Float3 root, Float3 strandDirection, HairRootBinding& binding)
    {
        float bestDistanceSquared = kFloatMax;
        float bestDistance = kFloatMax;
        std::uint32_t bestTriangle = 0u;
        float bestU = 0.0f, bestV = 0.0f;
        for (std::uint32_t triangle = 0; triangle < bounds.centers.size(); ++triangle) {
            // No point of the triangle is nearer than its bounding sphere.
            const float reach = bestDistance + bounds.radii[triangle];
            if (DistanceSquared(root, bounds.centers[triangle]) >= reach * reach) {
                continue;
            }
            float u, v;
            const Float3 closest = ClosestPointOnTriangle(root, scalp.positions[scalp.indices[triangle * 3u]],
                                                          scalp.positions[scalp.indices[triangle * 3u + 1u]],
                                                          scalp.positions[scalp.indices[triangle * 3u + 2u]], u, v);
            const float distanceSquared = DistanceSquared(root, closest);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestDistance = std::sqrt(distanceSquared);
                bestTriangle = triangle;
                bestU = u;
                bestV = v;
            }
        }

        binding.triangleIndex = bestTriangle;
        binding.barycentricU = bestU;
        binding.barycentricV = bestV;
        InterpolateSkin(scalp, bestTriangle, bestU, bestV, binding);

        const Float3 a = scalp.positions[scalp.indices[bestTriangle * 3u]];
        const Float3 edge = scalp.positions[scalp.indices[bestTriangle * 3u + 1u]] - a;
        Float3 normal = Normalize(Cross(edge, scalp.positions[scalp.indices[bestTriangle * 3u + 2u]] - a));
        Float3 tangent = Normalize(edge - normal * Dot(edge, normal));
        if (!(Dot(normal, normal) > 0.0f) || !(Dot(tangent, tangent) > 0.0f)) {
            // Zero-area triangle: fall back to the frame a scalp-less root gets.
            Float3 bitangent;
            normal = strandDirection;
            BuildTangentFrame(normal, tangent, bitangent);
        }
        binding.restNormal = normal;
        binding.restTangent = tangent;
    }

    // Position on a resampled strand at parameter t in [0,1], by vertex index.
    Float3 SampleStrand(std::span<const Float3> strandPoints, float t)
    {
        const float scaled = t * static_cast<float>(strandPoints.size() - 1u);
        const std::size_t index = std::min(static_cast<std::size_t>(scaled), strandPoints.size() - 2u);
        return Lerp(strandPoints[index], strandPoints[index + 1u], scaled - static_cast<float>(index));
    }

    // Centreline through the strand means at evenly spaced parameters, and at each control point the 2D PCA of the
    // strands' offsets in the plane across the centreline.
    HairClusterAggregate BuildAggregate(std::span<const Float3> clusterPoints, std::uint32_t strandCount, std::uint32_t pointsPerStrand)
    {
        HairClusterAggregate aggregate{};
        aggregate.strandCount = strandCount;

        constexpr std::uint32_t kControlPoints = HAIR_AGGREGATE_CONTROL_POINT_COUNT;
        std::vector<Float3> samples(std::size_t{ strandCount } * kControlPoints);
        for (std::uint32_t control = 0; control < kControlPoints; ++control) {
            const float t = static_cast<float>(control) / static_cast<float>(kControlPoints - 1u);
            double sum[3] = {};
            for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
                const Float3 sample = SampleStrand(clusterPoints.subspan(std::size_t{ strand } * pointsPerStrand, pointsPerStrand), t);
                samples[std::size_t{ strand } * kControlPoints + control] = sample;
                sum[0] += sample.x;
                sum[1] += sample.y;
                sum[2] += sample.z;
            }
            aggregate.controlPoints[control].position = { static_cast<float>(sum[0] / strandCount), static_cast<float>(sum[1] / strandCount),
                                                          static_cast<float>(sum[2] / strandCount) };
        }

        double areaSum = 0.0;
        for (std::uint32_t control = 0; control < kControlPoints; ++control) {
            HairAggregateControlPoint& point = aggregate.controlPoints[control];
            const Float3 next = aggregate.controlPoints[std::min(control + 1u, kControlPoints - 1u)].position;
            const Float3 previous = aggregate.controlPoints[control == 0u ? 0u : control - 1u].position;
            Float3 direction = Normalize(next - previous);
            if (!(Dot(direction, direction) > 0.0f)) {
                direction = { 0.0f, 1.0f, 0.0f };
            }
            Float3 planeU, planeV;
            BuildTangentFrame(direction, planeU, planeV);

            double uu = 0.0, uv = 0.0, vv = 0.0;
            for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
                const Float3 offset = samples[std::size_t{ strand } * kControlPoints + control] - point.position;
                const double u = Dot(offset, planeU), v = Dot(offset, planeV);
                uu += u * u;
                uv += u * v;
                vv += v * v;
            }
            uu /= strandCount;
            uv /= strandCount;
            vv /= strandCount;

            // Eigen-decomposition of the 2x2 covariance. Two standard deviations is the radius of a uniformly
            // filled disc with that variance.
            const double mean = 0.5 * (uu + vv);
            const double spread = std::sqrt(0.25 * (uu - vv) * (uu - vv) + uv * uv);
            const float semiMajor = 2.0f * static_cast<float>(std::sqrt(std::max(0.0, mean + spread)));
            const float semiMinor = 2.0f * static_cast<float>(std::sqrt(std::max(0.0, mean - spread)));
            const float angle = 0.5f * static_cast<float>(std::atan2(2.0 * uv, uu - vv));
            const float c = std::cos(angle), s = std::sin(angle);
            point.axisMajor = (planeU * c + planeV * s) * semiMajor;
            point.axisMinor = (planeV * c - planeU * s) * semiMinor;
            areaSum += static_cast<double>(kPi) * semiMajor * semiMinor;
        }
        const double meanArea = areaSum / kControlPoints;
        aggregate.density = meanArea > 0.0 ? static_cast<float>(strandCount / meanArea) : 0.0f;
        return aggregate;
    }

    std::uint32_t QuantizeUnorm16(float value)
    {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 65535.0f) + 0.5f);
    }

    std::string CheckInputs(const GroomSource& source, const ScalpMesh* scalp, const GroomBuildSettings& settings)
    {
        if (std::string problem = ValidateGroomSource(source); !problem.empty()) {
            return problem;
        }
        if (source.StrandCount() == 0u) {
            return "groom has no strands";
        }
        for (std::size_t point = 0; point < source.positions.size(); ++point) {
            const Float3 position = source.positions[point];
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || !std::isfinite(source.widths[point])) {
                return "point " + std::to_string(point) + " has a non-finite position or width";
            }
        }
        if (settings.minVertexLevel > settings.maxVertexLevel || settings.maxVertexLevel > HAIR_MAX_VERTEX_LEVEL) {
            return "vertex levels must satisfy min <= max <= " + std::to_string(HAIR_MAX_VERTEX_LEVEL);
        }
        if (!(settings.maxSegmentAngleRadians > 0.0f)) {
            return "maxSegmentAngleRadians must be positive";
        }
        if (settings.maxGuideCount == 0u || settings.guideLevelBase == 0u) {
            return "maxGuideCount and guideLevelBase must be at least 1";
        }
        if (scalp != nullptr) {
            if (scalp->indices.empty() || scalp->indices.size() % 3u != 0u) {
                return "scalp mesh indices are not a triangle list";
            }
            if (scalp->joints.size() != scalp->positions.size() || scalp->weights.size() != scalp->positions.size()) {
                return "scalp mesh needs joints and weights for every vertex";
            }
            for (const std::uint32_t index : scalp->indices) {
                if (index >= scalp->positions.size()) {
                    return "scalp mesh index is out of range";
                }
            }
        }
        return {};
    }

} // namespace

void ResampleStrand(const GroomSource& source,
                    std::uint32_t strand,
                    std::uint32_t vertexLevel,
                    std::vector<Float3>& positions,
                    std::vector<float>& widths)
{
    const std::uint32_t begin = source.StrandPointOffset(strand);
    const std::uint32_t sourceCount = source.StrandPointCount(strand);
    const std::uint32_t segmentCount = 1u << vertexLevel;
    positions.resize(segmentCount + 1u);
    widths.resize(segmentCount + 1u);

    double totalLength = 0.0;
    for (std::uint32_t i = 0; i + 1u < sourceCount; ++i) {
        totalLength += Length(source.positions[begin + i + 1u] - source.positions[begin + i]);
    }

    // Walk the source polyline once; `segment` is the source segment holding the current target arc length.
    std::uint32_t segment = 0u;
    double segmentStart = 0.0;
    double segmentLength = Length(source.positions[begin + 1u] - source.positions[begin]);
    for (std::uint32_t i = 1; i < segmentCount; ++i) {
        const double target = totalLength * i / segmentCount;
        while (segment + 2u < sourceCount && segmentStart + segmentLength < target) {
            segmentStart += segmentLength;
            ++segment;
            segmentLength = Length(source.positions[begin + segment + 1u] - source.positions[begin + segment]);
        }
        const float t = segmentLength > 0.0 ? static_cast<float>(std::clamp((target - segmentStart) / segmentLength, 0.0, 1.0)) : 0.0f;
        positions[i] = Lerp(source.positions[begin + segment], source.positions[begin + segment + 1u], t);
        widths[i] = source.widths[begin + segment] + (source.widths[begin + segment + 1u] - source.widths[begin + segment]) * t;
    }
    positions.front() = source.positions[begin];
    widths.front() = source.widths[begin];
    positions.back() = source.positions[begin + sourceCount - 1u];
    widths.back() = source.widths[begin + sourceCount - 1u];
}

bool BuildGroom(const GroomSource& source,
                const ScalpMesh* scalp,
                const GroomBuildSettings& settings,
                BuiltGroom& groom,
                std::string* error,
                GroomBuildInfo* info)
{
    groom = {};
    if (info != nullptr) {
        *info = {};
    }
    if (std::string problem = CheckInputs(source, scalp, settings); !problem.empty()) {
        if (error != nullptr) {
            *error = std::move(problem);
        }
        return false;
    }

    const std::uint32_t strandCount = source.StrandCount();
    std::vector<Float3> roots(strandCount);
    for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
        roots[strand] = source.positions[source.StrandPointOffset(strand)];
    }

    // Rank.
    const std::vector<std::uint32_t> order = RankStrands(roots, source.guideFlags); // rank -> source strand
    std::vector<std::uint32_t> rankOf(strandCount);                                 // source strand -> rank
    for (std::uint32_t rank = 0; rank < strandCount; ++rank) {
        rankOf[order[rank]] = rank;
    }

    // Clusters: by root and weighted tip offset, each sorted by rank.
    std::vector<std::array<float, 6>> features(strandCount);
    for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
        const Float3 tip = source.positions[source.StrandPointOffset(strand) + source.StrandPointCount(strand) - 1u];
        const Float3 shape = (tip - roots[strand]) * settings.clusterShapeWeight;
        features[strand] = { roots[strand].x, roots[strand].y, roots[strand].z, shape.x, shape.y, shape.z };
    }
    std::vector<std::uint32_t> sourceStrands(strandCount); // built strand -> source strand
    for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
        sourceStrands[strand] = strand;
    }
    std::vector<std::uint32_t> clusterEnds;
    SplitIntoClusters(sourceStrands, 0u, features, clusterEnds);
    const std::uint32_t clusterCount = static_cast<std::uint32_t>(clusterEnds.size());

    std::vector<std::uint32_t> builtIndexOf(strandCount); // source strand -> built strand
    for (std::uint32_t cluster = 0, begin = 0; cluster < clusterCount; begin = clusterEnds[cluster++]) {
        std::sort(sourceStrands.begin() + begin, sourceStrands.begin() + clusterEnds[cluster],
                  [&rankOf](std::uint32_t a, std::uint32_t b) { return rankOf[a] < rankOf[b]; });
    }
    for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
        builtIndexOf[sourceStrands[strand]] = strand;
    }

    // Guide-capable set and its levels.
    const std::uint32_t guideCount = std::min(settings.maxGuideCount, strandCount);
    groom.guideLevelOffsets.push_back(0u);
    for (std::uint64_t end = settings.guideLevelBase; groom.guideLevelOffsets.back() < guideCount; end *= 4u) {
        groom.guideLevelOffsets.push_back(static_cast<std::uint32_t>(std::min<std::uint64_t>(end, guideCount)));
    }
    std::vector<Float3> guideRoots(guideCount);
    groom.guideStrandIndices.resize(guideCount);
    for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
        guideRoots[guide] = roots[order[guide]];
        groom.guideStrandIndices[guide] = builtIndexOf[order[guide]];
    }

    // Writes up to three neighbours as indices (plus indexOffset) and normalized inverse-distance weights.
    const auto bindToNearest = [](const PointGrid& grid, Float3 position, std::uint32_t indexOffset, uint* indices, float* weights) {
        const PointGrid::Nearest nearest = grid.FindNearest3(position);
        float total = 0.0f;
        for (std::uint32_t i = 0; i < 3u; ++i) {
            indices[i] = i < nearest.count ? indexOffset + nearest.indices[i] : HAIR_INVALID_INDEX;
            weights[i] = i < nearest.count ? 1.0f / (nearest.distances[i] + 1.0e-6f) : 0.0f;
            total += weights[i];
        }
        for (std::uint32_t i = 0; i < 3u; ++i) {
            weights[i] /= total;
        }
    };

    groom.guideBindings.resize(guideCount);
    for (std::uint32_t level = 0; level < groom.GuideLevelCount(); ++level) {
        const std::uint32_t begin = groom.guideLevelOffsets[level], end = groom.guideLevelOffsets[level + 1u];
        if (level == 0u) {
            for (std::uint32_t guide = begin; guide < end; ++guide) {
                HairGuideBinding& binding = groom.guideBindings[guide];
                binding.level = 0u;
                binding.parentIndices[0] = binding.parentIndices[1] = binding.parentIndices[2] = HAIR_INVALID_INDEX;
                binding.randomOffset = RandomUnit(settings.seed, 0u, guide);
            }
            continue;
        }
        const std::uint32_t parentBegin = groom.guideLevelOffsets[level - 1u];
        const PointGrid parents(std::span<const Float3>(guideRoots).subspan(parentBegin, begin - parentBegin));
        for (std::uint32_t guide = begin; guide < end; ++guide) {
            HairGuideBinding& binding = groom.guideBindings[guide];
            binding.level = level;
            bindToNearest(parents, guideRoots[guide], parentBegin, binding.parentIndices, binding.parentWeights);
            binding.randomOffset = RandomUnit(settings.seed, 0u, guide);
        }
    }

    // Strand records, root bindings and resampled points, cluster by cluster.
    const PointGrid guideGrid(guideRoots);
    const ScalpTriangleBounds scalpBounds = scalp != nullptr ? ScalpTriangleBounds(*scalp) : ScalpTriangleBounds(ScalpMesh{});
    groom.clusters.resize(clusterCount);
    groom.aggregates.resize(clusterCount);
    groom.strands.resize(strandCount);
    groom.rootBindings.resize(strandCount);
    groom.boundsMin = { kFloatMax, kFloatMax, kFloatMax };
    groom.boundsMax = { -kFloatMax, -kFloatMax, -kFloatMax };

    std::vector<Float3> clusterPoints, strandPoints;
    std::vector<float> clusterWidths, strandWidths;
    for (std::uint32_t clusterIndex = 0, begin = 0; clusterIndex < clusterCount; begin = clusterEnds[clusterIndex++]) {
        const std::uint32_t end = clusterEnds[clusterIndex];
        HairClusterHeader& cluster = groom.clusters[clusterIndex];
        cluster.strandOffset = begin;
        cluster.strandCount = end - begin;
        cluster.randomOffset = RandomUnit(settings.seed, 1u, clusterIndex);
        cluster.parentIndex = HAIR_INVALID_INDEX;

        std::uint32_t vertexLevel = 0u;
        for (std::uint32_t strand = begin; strand < end; ++strand) {
            vertexLevel = std::max(vertexLevel, StrandVertexLevel(source, sourceStrands[strand], settings.maxSegmentAngleRadians));
        }
        cluster.vertexLevel = std::clamp(vertexLevel, settings.minVertexLevel, settings.maxVertexLevel);
        const std::uint32_t pointsPerStrand = HairStrandPointCount(cluster.vertexLevel);

        clusterPoints.clear();
        clusterWidths.clear();
        Float3 lo{ kFloatMax, kFloatMax, kFloatMax }, hi{ -kFloatMax, -kFloatMax, -kFloatMax };
        for (std::uint32_t strand = begin; strand < end; ++strand) {
            const std::uint32_t sourceStrand = sourceStrands[strand];
            ResampleStrand(source, sourceStrand, cluster.vertexLevel, strandPoints, strandWidths);
            clusterPoints.insert(clusterPoints.end(), strandPoints.begin(), strandPoints.end());
            clusterWidths.insert(clusterWidths.end(), strandWidths.begin(), strandWidths.end());
            for (const Float3 point : strandPoints) {
                lo = Min(lo, point);
                hi = Max(hi, point);
            }

            HairStrandRecord& record = groom.strands[strand];
            record.pointOffset = static_cast<std::uint32_t>(groom.points.size() + (clusterPoints.size() - pointsPerStrand));
            record.rank = rankOf[sourceStrand];
            record.width = *std::max_element(strandWidths.begin(), strandWidths.end());
            record.clusterIndex = clusterIndex;
            record.guideIndices[0] = record.guideIndices[1] = record.guideIndices[2] = HAIR_INVALID_INDEX;
            if (record.rank < guideCount) {
                record.guideIndices[0] = record.rank;
                record.guideWeights[0] = 1.0f;
            } else {
                // Authored guides, when every one of them is guide-capable and their weights are usable.
                std::uint32_t authored = 0u;
                float total = 0.0f;
                bool usable = !source.closestGuides.empty();
                for (std::uint32_t i = 0; usable && i < 3u; ++i) {
                    const std::int32_t guideStrand = source.closestGuides[sourceStrand][i];
                    const float weight = source.closestGuideWeights[sourceStrand][i];
                    if (guideStrand < 0) {
                        continue;
                    }
                    usable = rankOf[guideStrand] < guideCount && std::isfinite(weight) && weight >= 0.0f;
                    record.guideIndices[authored] = rankOf[guideStrand];
                    record.guideWeights[authored++] = weight;
                    total += weight;
                }
                if (usable && total > 0.0f) {
                    for (std::uint32_t i = 0; i < authored; ++i) {
                        record.guideWeights[i] /= total;
                    }
                } else {
                    bindToNearest(guideGrid, roots[sourceStrand], 0u, record.guideIndices, record.guideWeights);
                }
            }

            HairRootBinding& binding = groom.rootBindings[strand];
            binding.restPosition = roots[sourceStrand];
            const Float3 direction = StrandRootDirection(source, sourceStrand);
            if (scalp != nullptr) {
                BindRootToScalp(*scalp, scalpBounds, roots[sourceStrand], direction, binding);
            } else {
                Float3 bitangent;
                binding.joints[0] = 0u;
                binding.weights[0] = 1.0f;
                binding.triangleIndex = HAIR_INVALID_INDEX;
                binding.restNormal = direction;
                BuildTangentFrame(direction, binding.restTangent, bitangent);
            }
        }

        // Bounds and 16-bit quantization over the cluster's box.
        cluster.boundsCenter = (lo + hi) * 0.5f;
        cluster.boundsExtent = (hi - lo) * 0.5f;
        cluster.quantizationOrigin = lo;
        cluster.quantizationScale = (hi - lo) * (1.0f / 65535.0f);
        float radiusSquared = 0.0f;
        const Float3 scale = cluster.quantizationScale;
        const Float3 inverseScale{ scale.x > 0.0f ? 1.0f / scale.x : 0.0f, scale.y > 0.0f ? 1.0f / scale.y : 0.0f, scale.z > 0.0f ? 1.0f / scale.z : 0.0f };
        for (std::size_t i = 0; i < clusterPoints.size(); ++i) {
            const Float3 point = clusterPoints[i];
            const float strandWidth = groom.strands[begin + i / pointsPerStrand].width;
            const std::uint32_t x = QuantizeUnorm16((point.x - lo.x) * inverseScale.x);
            const std::uint32_t y = QuantizeUnorm16((point.y - lo.y) * inverseScale.y);
            const std::uint32_t z = QuantizeUnorm16((point.z - lo.z) * inverseScale.z);
            const std::uint32_t width = strandWidth > 0.0f ? QuantizeUnorm16(clusterWidths[i] / strandWidth * 65535.0f) : 0u;
            groom.points.push_back({ x | (y << 16u), z | (width << 16u) });
            // The sphere bounds the points as the GPU decodes them.
            radiusSquared = std::max(radiusSquared, DistanceSquared(DecodeHairPointPosition(cluster, groom.points.back()), cluster.boundsCenter));
        }
        cluster.boundsRadius = std::sqrt(radiusSquared);
        groom.boundsMin = Min(groom.boundsMin, lo);
        groom.boundsMax = Max(groom.boundsMax, hi);

        groom.aggregates[clusterIndex] = BuildAggregate(clusterPoints, cluster.strandCount, pointsPerStrand);
    }

    if (info != nullptr) {
        info->sourceStrandIndices = std::move(sourceStrands);
    }
    return true;
}

std::string ValidateBuiltGroom(const BuiltGroom& groom)
{
    const std::size_t strandCount = groom.strands.size();
    const std::size_t guideCount = groom.guideBindings.size();
    if (groom.aggregates.size() != groom.clusters.size()) return "aggregate count does not match cluster count";
    if (groom.rootBindings.size() != strandCount) return "root binding count does not match strand count";
    if (groom.guideStrandIndices.size() != guideCount) return "guide strand index count does not match guide count";
    if (guideCount > strandCount) return "more guides than strands";

    // Guide levels: offsets start at 0, end at the guide count and every level is non-empty.
    const std::uint32_t levelCount = groom.GuideLevelCount();
    if (guideCount == 0u ? !groom.guideLevelOffsets.empty() : levelCount == 0u) return "guide level offsets do not match guide count";
    if (levelCount != 0u && (groom.guideLevelOffsets.front() != 0u || groom.guideLevelOffsets.back() != guideCount)) {
        return "guide level offsets do not span the guides";
    }
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const std::uint32_t begin = groom.guideLevelOffsets[level], end = groom.guideLevelOffsets[level + 1u];
        if (end <= begin || end > guideCount) return "guide level " + std::to_string(level) + " is empty or out of range";
        for (std::uint32_t guide = begin; guide < end; ++guide) {
            const HairGuideBinding& binding = groom.guideBindings[guide];
            if (binding.level != level) return "guide " + std::to_string(guide) + " is in the wrong level";
            for (const std::uint32_t parent : binding.parentIndices) {
                if (parent == HAIR_INVALID_INDEX) continue;
                if (level == 0u || parent < groom.guideLevelOffsets[level - 1u] || parent >= begin) {
                    return "guide " + std::to_string(guide) + " has a parent outside the level below";
                }
            }
            const std::uint32_t strand = groom.guideStrandIndices[guide];
            if (strand >= strandCount || groom.strands[strand].rank != guide) {
                return "guide " + std::to_string(guide) + " does not map to the strand of that rank";
            }
        }
    }

    // Clusters partition the strands in order; strands fit the point array.
    std::size_t nextStrand = 0u;
    for (std::size_t clusterIndex = 0; clusterIndex < groom.clusters.size(); ++clusterIndex) {
        const HairClusterHeader& cluster = groom.clusters[clusterIndex];
        const std::string name = "cluster " + std::to_string(clusterIndex);
        if (cluster.strandCount == 0u || cluster.strandCount > HAIR_MAX_STRANDS_PER_CLUSTER) return name + " has an invalid strand count";
        if (cluster.strandOffset != nextStrand || cluster.strandCount > strandCount - nextStrand) return name + " does not continue the strand array";
        if (cluster.vertexLevel > HAIR_MAX_VERTEX_LEVEL) return name + " has an invalid vertex level";
        if (groom.aggregates[clusterIndex].strandCount != cluster.strandCount) return name + " disagrees with its aggregate";
        const std::uint32_t pointsPerStrand = HairStrandPointCount(cluster.vertexLevel);
        for (std::size_t strand = nextStrand; strand < nextStrand + cluster.strandCount; ++strand) {
            const HairStrandRecord& record = groom.strands[strand];
            if (record.clusterIndex != clusterIndex) return "strand " + std::to_string(strand) + " names the wrong cluster";
            if (record.rank >= strandCount) return "strand " + std::to_string(strand) + " has an out-of-range rank";
            if (record.pointOffset > groom.points.size() || groom.points.size() - record.pointOffset < pointsPerStrand) {
                return "strand " + std::to_string(strand) + " runs past the point array";
            }
            for (const std::uint32_t guide : record.guideIndices) {
                if (guide != HAIR_INVALID_INDEX && guide >= guideCount) {
                    return "strand " + std::to_string(strand) + " has a guide outside the guide-capable set";
                }
            }
        }
        nextStrand += cluster.strandCount;
    }
    if (nextStrand != strandCount) return "clusters do not cover every strand";
    return {};
}

} // namespace br::hair
