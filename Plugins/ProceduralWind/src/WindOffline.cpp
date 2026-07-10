#include "ProceduralWind/WindOffline.h"

#include <DirectXPackedVector.h>
#include <lz4.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <numeric>

namespace br::wind {
namespace {

constexpr std::uint32_t kCacheLayerKindTerrainRelative = 1u;

template <class T>
void HashValue(std::uint64_t& hash, const T& value)
{
    const auto bytes = std::as_bytes(std::span{ &value, 1u });
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
}

std::uint64_t Checksum(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <class T>
bool Write(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return stream.good();
}

template <class T>
bool Read(std::ifstream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return stream.good();
}

struct Grid {
    float originX = 0.0f;
    float originY = 0.0f;
    float cellSize = 0.0f;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::vector<double> terrainHeight;
    std::vector<std::uint8_t> coverage;
};

double Edge(double ax, double ay, double bx, double by, double px, double py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

Grid Rasterize(WindTerrainMeshView mesh, float cellSize)
{
    Grid grid;
    grid.cellSize = cellSize;
    if (mesh.positions.empty() || mesh.indices.size() < 3u || !(cellSize > 0.0f)) return grid;

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (const auto& point : mesh.positions) {
        minX = std::min(minX, point.x); minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x); maxY = std::max(maxY, point.y);
    }
    grid.originX = std::floor(minX / cellSize) * cellSize - cellSize;
    grid.originY = std::floor(minY / cellSize) * cellSize - cellSize;
    grid.width = static_cast<std::uint32_t>(std::ceil((maxX - grid.originX) / cellSize)) + 2u;
    grid.height = static_cast<std::uint32_t>(std::ceil((maxY - grid.originY) / cellSize)) + 2u;
    const std::size_t count = static_cast<std::size_t>(grid.width) * grid.height;
    grid.terrainHeight.assign(count, -std::numeric_limits<double>::infinity());
    grid.coverage.assign(count, 0u);

    for (std::size_t triangle = 0; triangle + 2u < mesh.indices.size(); triangle += 3u) {
        const auto i0 = mesh.indices[triangle]; const auto i1 = mesh.indices[triangle + 1u]; const auto i2 = mesh.indices[triangle + 2u];
        if (i0 >= mesh.positions.size() || i1 >= mesh.positions.size() || i2 >= mesh.positions.size()) continue;
        const auto& a = mesh.positions[i0]; const auto& b = mesh.positions[i1]; const auto& c = mesh.positions[i2];
        const double area = Edge(a.x, a.y, b.x, b.y, c.x, c.y);
        if (std::abs(area) < 1.0e-12) continue;
        const int gx0 = std::max(0, static_cast<int>(std::floor((std::min({ a.x, b.x, c.x }) - grid.originX) / cellSize)));
        const int gy0 = std::max(0, static_cast<int>(std::floor((std::min({ a.y, b.y, c.y }) - grid.originY) / cellSize)));
        const int gx1 = std::min(static_cast<int>(grid.width) - 1, static_cast<int>(std::floor((std::max({ a.x, b.x, c.x }) - grid.originX) / cellSize)));
        const int gy1 = std::min(static_cast<int>(grid.height) - 1, static_cast<int>(std::floor((std::max({ a.y, b.y, c.y }) - grid.originY) / cellSize)));
        for (int y = gy0; y <= gy1; ++y) for (int x = gx0; x <= gx1; ++x) {
            const double px = grid.originX + (x + 0.5) * cellSize;
            const double py = grid.originY + (y + 0.5) * cellSize;
            const double w0 = Edge(b.x, b.y, c.x, c.y, px, py) / area;
            const double w1 = Edge(c.x, c.y, a.x, a.y, px, py) / area;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1.0e-7 || w1 < -1.0e-7 || w2 < -1.0e-7) continue;
            const auto index = static_cast<std::size_t>(y) * grid.width + x;
            grid.terrainHeight[index] = std::max(grid.terrainHeight[index], w0 * a.z + w1 * b.z + w2 * c.z);
            grid.coverage[index] = 255u;
        }
    }

    double mean = 0.0; std::size_t valid = 0u;
    for (std::size_t i = 0; i < count; ++i) if (grid.coverage[i]) { mean += grid.terrainHeight[i]; ++valid; }
    mean = valid ? mean / static_cast<double>(valid) : 0.0;
    for (std::size_t i = 0; i < count; ++i) if (!grid.coverage[i]) grid.terrainHeight[i] = mean;
    // Diffuse heights into uncovered padding/holes for stable coefficients while retaining the original coverage mask.
    for (std::uint32_t pass = 0; pass < 8u; ++pass) {
        auto next = grid.terrainHeight;
        for (std::uint32_t y = 1; y + 1u < grid.height; ++y) for (std::uint32_t x = 1; x + 1u < grid.width; ++x) {
            const auto i = static_cast<std::size_t>(y) * grid.width + x;
            if (!grid.coverage[i]) next[i] = 0.25 * (grid.terrainHeight[i - 1u] + grid.terrainHeight[i + 1u] + grid.terrainHeight[i - grid.width] + grid.terrainHeight[i + grid.width]);
        }
        grid.terrainHeight.swap(next);
    }
    return grid;
}

bool SolvePotential(const Grid& grid, double dx, double dy, const WindBakeSettings& settings, std::vector<double>& phi)
{
    const std::size_t count = static_cast<std::size_t>(grid.width) * grid.height;
    const double maxHeight = *std::max_element(grid.terrainHeight.begin(), grid.terrainHeight.end());
    const double lid = maxHeight + settings.atmosphericClearanceCells * grid.cellSize;
    const double minDepth = settings.minimumLayerDepthCells * grid.cellSize;
    std::vector<double> depth(count), diagonal(count, 1.0), rhs(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) depth[i] = std::max(minDepth, lid - grid.terrainHeight[i]);
    phi.resize(count);
    for (std::uint32_t y = 0; y < grid.height; ++y) for (std::uint32_t x = 0; x < grid.width; ++x) {
        const auto i = static_cast<std::size_t>(y) * grid.width + x;
        phi[i] = dx * (grid.originX + (x + 0.5) * grid.cellSize) + dy * (grid.originY + (y + 0.5) * grid.cellSize);
    }
    const auto boundary = [&](std::uint32_t x, std::uint32_t y) { return x == 0u || y == 0u || x + 1u == grid.width || y + 1u == grid.height; };
    const auto coefficient = [&](std::size_t a, std::size_t b) { return 2.0 * depth[a] * depth[b] / std::max(1.0e-12, depth[a] + depth[b]); };
    for (std::uint32_t y = 1; y + 1u < grid.height; ++y) for (std::uint32_t x = 1; x + 1u < grid.width; ++x) {
        const auto i = static_cast<std::size_t>(y) * grid.width + x;
        const std::array<std::size_t, 4> neighbours{ i - 1u, i + 1u, i - grid.width, i + grid.width };
        diagonal[i] = 0.0;
        for (const auto n : neighbours) {
            const double c = coefficient(i, n); diagonal[i] += c;
            const auto nx = static_cast<std::uint32_t>(n % grid.width); const auto ny = static_cast<std::uint32_t>(n / grid.width);
            if (boundary(nx, ny)) rhs[i] += c * phi[n];
        }
    }
    const auto apply = [&](const std::vector<double>& value, std::vector<double>& out) {
        std::fill(out.begin(), out.end(), 0.0);
        for (std::uint32_t y = 1; y + 1u < grid.height; ++y) for (std::uint32_t x = 1; x + 1u < grid.width; ++x) {
            const auto i = static_cast<std::size_t>(y) * grid.width + x;
            out[i] = diagonal[i] * value[i];
            const std::array<std::size_t, 4> neighbours{ i - 1u, i + 1u, i - grid.width, i + grid.width };
            for (const auto n : neighbours) {
                const auto nx = static_cast<std::uint32_t>(n % grid.width); const auto ny = static_cast<std::uint32_t>(n / grid.width);
                if (!boundary(nx, ny)) out[i] -= coefficient(i, n) * value[n];
            }
        }
    };
    std::vector<double> ap(count), r(count), z(count), p(count);
    apply(phi, ap);
    double rhsNorm2 = 0.0, residualNorm2 = 0.0, rz = 0.0;
    for (std::uint32_t y = 1; y + 1u < grid.height; ++y) for (std::uint32_t x = 1; x + 1u < grid.width; ++x) {
        const auto i = static_cast<std::size_t>(y) * grid.width + x;
        r[i] = rhs[i] - ap[i]; z[i] = r[i] / diagonal[i]; p[i] = z[i]; rz += r[i] * z[i]; residualNorm2 += r[i] * r[i]; rhsNorm2 += rhs[i] * rhs[i];
    }
    const double threshold = settings.relativeTolerance * settings.relativeTolerance * std::max(1.0, rhsNorm2);
    if (residualNorm2 <= threshold) return true;
    for (std::uint32_t iteration = 0; iteration < settings.maximumIterations; ++iteration) {
        apply(p, ap); double pap = 0.0;
        for (std::size_t i = 0; i < count; ++i) pap += p[i] * ap[i];
        if (std::abs(pap) < 1.0e-30) break;
        const double alpha = rz / pap; double residual2 = 0.0;
        for (std::size_t i = 0; i < count; ++i) { phi[i] += alpha * p[i]; r[i] -= alpha * ap[i]; residual2 += r[i] * r[i]; }
        if (residual2 <= threshold) return true;
        double nextRz = 0.0;
        for (std::size_t i = 0; i < count; ++i) { z[i] = diagonal[i] != 0.0 ? r[i] / diagonal[i] : 0.0; nextRz += r[i] * z[i]; }
        const double beta = nextRz / std::max(1.0e-30, rz);
        for (std::size_t i = 0; i < count; ++i) p[i] = z[i] + beta * p[i];
        rz = nextRz;
    }
    return false;
}

} // namespace

WindDirectionBracket ComputeDirectionBracket(float angleRadians, std::uint32_t directionCount)
{
    if (directionCount == 0u || !std::isfinite(angleRadians)) return {};
    const float tau = 2.0f * std::numbers::pi_v<float>;
    float wrapped = std::fmod(angleRadians, tau); if (wrapped < 0.0f) wrapped += tau;
    const float coordinate = wrapped * static_cast<float>(directionCount) / tau;
    const auto lower = static_cast<std::uint32_t>(std::floor(coordinate)) % directionCount;
    return { lower, (lower + 1u) % directionCount, coordinate - std::floor(coordinate) };
}

std::uint64_t ComputeWindBakeConfigHash(const WindBakeSettings& settings)
{
    std::uint64_t hash = 1469598103934665603ull;
    HashValue(hash, kWindCacheSchemaVersion); HashValue(hash, settings.cellSize); HashValue(hash, settings.directionCount);
    HashValue(hash, settings.atmosphericClearanceCells); HashValue(hash, settings.minimumLayerDepthCells);
    HashValue(hash, settings.maximumUpliftRatio); HashValue(hash, settings.maximumIterations); HashValue(hash, settings.relativeTolerance);
    return hash;
}

WindBakeResult BakeLevel0WindField(WindTerrainMeshView terrain, std::uint64_t sourceHash, const WindBakeSettings& settings)
{
    WindBakeResult result;
    if (settings.directionCount < 2u || settings.cellSize <= 0.0f) { result.error = "invalid wind bake settings"; return result; }
    Grid grid = Rasterize(terrain, settings.cellSize);
    if (grid.width < 3u || grid.height < 3u || std::none_of(grid.coverage.begin(), grid.coverage.end(), [](auto v) { return v != 0u; })) {
        result.error = "terrain did not cover any wind cells"; return result;
    }
    result.metadata.sourceHash = sourceHash; result.metadata.configHash = ComputeWindBakeConfigHash(settings);
    result.metadata.origin = { grid.originX, grid.originY, 0.0f }; result.metadata.cellSize = grid.cellSize;
    result.metadata.width = grid.width; result.metadata.height = grid.height; result.coverage = grid.coverage;
    const std::size_t cells = static_cast<std::size_t>(grid.width) * grid.height;
    result.directionRgba16f.reserve(settings.directionCount); result.metadata.directions.resize(settings.directionCount);
    bool allConverged = true;
    for (std::uint32_t direction = 0; direction < settings.directionCount; ++direction) {
        const double angle = 2.0 * std::numbers::pi * direction / settings.directionCount;
        const double dx = std::cos(angle), dy = std::sin(angle);
        std::vector<double> phi; allConverged = SolvePotential(grid, dx, dy, settings, phi) && allConverged;
        std::vector<double> vx(cells), vy(cells), vz(cells); double meanDownwind = 0.0; std::size_t valid = 0u;
        for (std::uint32_t y = 0; y < grid.height; ++y) for (std::uint32_t x = 0; x < grid.width; ++x) {
            const auto i = static_cast<std::size_t>(y) * grid.width + x;
            const auto xl = x ? x - 1u : x; const auto xr = std::min(x + 1u, grid.width - 1u);
            const auto yd = y ? y - 1u : y; const auto yu = std::min(y + 1u, grid.height - 1u);
            vx[i] = (phi[static_cast<std::size_t>(y) * grid.width + xr] - phi[static_cast<std::size_t>(y) * grid.width + xl]) / ((xr - xl) * grid.cellSize + 1.0e-12);
            vy[i] = (phi[static_cast<std::size_t>(yu) * grid.width + x] - phi[static_cast<std::size_t>(yd) * grid.width + x]) / ((yu - yd) * grid.cellSize + 1.0e-12);
            const double dhdx = (grid.terrainHeight[static_cast<std::size_t>(y) * grid.width + xr] - grid.terrainHeight[static_cast<std::size_t>(y) * grid.width + xl]) / ((xr - xl) * grid.cellSize + 1.0e-12);
            const double dhdy = (grid.terrainHeight[static_cast<std::size_t>(yu) * grid.width + x] - grid.terrainHeight[static_cast<std::size_t>(yd) * grid.width + x]) / ((yu - yd) * grid.cellSize + 1.0e-12);
            vz[i] = vx[i] * dhdx + vy[i] * dhdy;
            if (grid.coverage[i]) { meanDownwind += vx[i] * dx + vy[i] * dy; ++valid; }
        }
        const double scale = valid && std::abs(meanDownwind) > 1.0e-12 ? valid / meanDownwind : 1.0;
        std::vector<std::uint16_t> packed(cells * 4u);
        for (std::size_t i = 0; i < cells; ++i) {
            packed[i * 4u] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(vx[i] * scale));
            packed[i * 4u + 1u] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(vy[i] * scale));
            packed[i * 4u + 2u] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(
                std::clamp(vz[i] * scale, -static_cast<double>(settings.maximumUpliftRatio), static_cast<double>(settings.maximumUpliftRatio))));
            packed[i * 4u + 3u] = DirectX::PackedVector::XMConvertFloatToHalf(grid.coverage[i] ? 1.0f : 0.0f);
        }
        result.metadata.directions[direction].angleRadians = static_cast<float>(angle);
        result.directionRgba16f.push_back(std::move(packed));
    }
    result.converged = allConverged;
    if (!allConverged) result.error = "one or more potential solves did not reach tolerance";
    return result;
}

bool SaveWindCache(const std::filesystem::path& path, const WindBakeResult& bake, std::string* error)
{
    if (bake.directionRgba16f.size() != bake.metadata.directions.size()) { if (error) *error = "direction data mismatch"; return false; }
    std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); if (!stream) { if (error) *error = "could not create wind cache"; return false; }
    const auto directionCount = static_cast<std::uint32_t>(bake.metadata.directions.size());
    Write(stream, kWindCacheMagic); Write(stream, kWindCacheSchemaVersion); Write(stream, bake.metadata.sourceHash); Write(stream, bake.metadata.configHash);
    Write(stream, bake.metadata.origin); Write(stream, bake.metadata.cellSize); Write(stream, bake.metadata.width); Write(stream, bake.metadata.height);
    Write(stream, bake.metadata.layerCount); Write(stream, kCacheLayerKindTerrainRelative); Write(stream, directionCount);
    const auto directoryOffset = stream.tellp(); WindDirectionBlock empty{}; for (std::uint32_t i = 0; i < directionCount; ++i) Write(stream, empty);
    std::vector<WindDirectionBlock> directory(directionCount);
    for (std::uint32_t i = 0; i < directionCount; ++i) {
        const auto raw = std::as_bytes(std::span{ bake.directionRgba16f[i] });
        std::vector<char> compressed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(raw.size()))));
        const int compressedBytes = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()), compressed.data(), static_cast<int>(raw.size()), static_cast<int>(compressed.size()));
        if (compressedBytes <= 0) { if (error) *error = "LZ4 compression failed"; return false; }
        auto& block = directory[i]; block.angleRadians = bake.metadata.directions[i].angleRadians; block.fileOffset = static_cast<std::uint64_t>(stream.tellp());
        block.compressedBytes = static_cast<std::uint32_t>(compressedBytes); block.uncompressedBytes = static_cast<std::uint32_t>(raw.size()); block.checksum = Checksum(raw);
        stream.write(compressed.data(), compressedBytes);
    }
    const auto end = stream.tellp(); stream.seekp(directoryOffset); for (const auto& block : directory) Write(stream, block); stream.seekp(end);
    if (!stream.good()) { if (error) *error = "wind cache write failed"; return false; }
    return true;
}

bool LoadWindCacheMetadata(const std::filesystem::path& path, WindCacheMetadata& metadata, std::string* error)
{
    std::ifstream stream(path, std::ios::binary); if (!stream) { if (error) *error = "could not open wind cache"; return false; }
    std::uint32_t magic = 0, version = 0, layerKind = 0, directionCount = 0;
    if (!Read(stream, magic) || !Read(stream, version) || magic != kWindCacheMagic || version != kWindCacheSchemaVersion) { if (error) *error = "invalid wind cache header"; return false; }
    metadata = {}; metadata.schemaVersion = version;
    if (!Read(stream, metadata.sourceHash) || !Read(stream, metadata.configHash) || !Read(stream, metadata.origin) || !Read(stream, metadata.cellSize) ||
        !Read(stream, metadata.width) || !Read(stream, metadata.height) || !Read(stream, metadata.layerCount) || !Read(stream, layerKind) || !Read(stream, directionCount) ||
        layerKind != kCacheLayerKindTerrainRelative || metadata.layerCount != 1u || !std::isfinite(metadata.cellSize) || metadata.cellSize <= 0.0f ||
        metadata.width < 2u || metadata.height < 2u || metadata.width > 65535u || metadata.height > 65535u ||
        directionCount < 2u || directionCount > 1024u) { if (error) *error = "invalid wind cache metadata"; return false; }
    metadata.directions.resize(directionCount); for (auto& block : metadata.directions) if (!Read(stream, block)) { if (error) *error = "truncated wind cache directory"; return false; }
    const std::uint64_t expectedBytes = static_cast<std::uint64_t>(metadata.width) * metadata.height * 4u * sizeof(std::uint16_t);
    std::error_code ec;
    const auto fileBytes = std::filesystem::file_size(path, ec);
    for (const auto& block : metadata.directions) {
        if (!std::isfinite(block.angleRadians) || block.uncompressedBytes != expectedBytes || block.compressedBytes == 0u ||
            ec || block.fileOffset > fileBytes || block.compressedBytes > fileBytes - block.fileOffset) {
            if (error) *error = "invalid wind cache direction directory";
            return false;
        }
    }
    return true;
}

bool LoadWindDirection(const std::filesystem::path& path, const WindCacheMetadata& metadata, std::uint32_t directionIndex, std::vector<std::uint16_t>& rgba16f, std::string* error)
{
    if (directionIndex >= metadata.directions.size()) { if (error) *error = "wind direction out of range"; return false; }
    const auto& block = metadata.directions[directionIndex]; std::ifstream stream(path, std::ios::binary); if (!stream) return false;
    stream.seekg(static_cast<std::streamoff>(block.fileOffset)); std::vector<char> compressed(block.compressedBytes); stream.read(compressed.data(), compressed.size());
    if (!stream.good()) { if (error) *error = "truncated wind direction block"; return false; }
    std::vector<std::byte> raw(block.uncompressedBytes); const int decoded = LZ4_decompress_safe(compressed.data(), reinterpret_cast<char*>(raw.data()), static_cast<int>(compressed.size()), static_cast<int>(raw.size()));
    if (decoded != static_cast<int>(raw.size()) || Checksum(raw) != block.checksum || raw.size() % sizeof(std::uint16_t) != 0u) { if (error) *error = "invalid wind direction block"; return false; }
    rgba16f.resize(raw.size() / sizeof(std::uint16_t)); std::memcpy(rgba16f.data(), raw.data(), raw.size()); return true;
}

bool IsWindCacheCurrent(const std::filesystem::path& path, std::uint64_t sourceHash, const WindBakeSettings& settings)
{
    WindCacheMetadata metadata; return LoadWindCacheMetadata(path, metadata) && metadata.sourceHash == sourceHash && metadata.configHash == ComputeWindBakeConfigHash(settings);
}

} // namespace br::wind
