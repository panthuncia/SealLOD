#include "Hair/HairRuntime.h"

#include "HairGpuBuffers.h"

#include "Hair/AlembicGroomReader.h"
#include "Hair/GroomBuilder.h"
#include "Hair/GroomFile.h"
#include "Hair/HairFileReader.h"

#include <BasicRenderer/Assets/Geometry/MeshInstance.h>
#include <BasicRenderer/Diagnostics/ReadbackCaptures.h>
#include <BasicRenderer/Extensions/SceneEntityRenderState.h>
#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Scene/Animation/Skeleton.h>
#include <BasicRenderer/Scene/Components.h>
#include <BasicRenderer/Scene/Scene.h>

#include <ORGModuleServices/BufferImage.h>

#include <DirectXMath.h>
#include <flecs.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

namespace br::hair {

namespace {

using namespace DirectX;

constexpr std::size_t kUploadChunkBytes = 4u * 1024u * 1024u;
constexpr std::uint64_t kSelfCheckFrameInterval = 4u;
constexpr std::uint64_t kNoFrame = ~0ull;

struct Asset {
    BuiltGroom groom;
    std::string source;
    std::uint32_t clusterBase = 0u;
    std::uint32_t strandBase = 0u;
    std::uint32_t guideBase = 0u;
    std::uint32_t pointBase = 0u;
    std::vector<HairGuidePointRange> guidePointRanges; // by rank: where a guide's points are in an instance's guide state
    std::uint32_t guidePointCount = 0u;                // the guide state rows an instance of this groom needs
    // Simulation LOD: the level each guide is simulated from (0 for level 0, level + random offset above), ascending,
    // and the points of the guides up to and including each.
    std::vector<float> guideThresholds;
    std::vector<std::uint32_t> guideThresholdPoints;
    // The guide points simulated at simulation level `level`.
    std::uint32_t SimulatedPoints(float level) const
    {
        if (!(level > 0.0f)) return 0u;
        const auto count = std::ranges::upper_bound(guideThresholds, level) - guideThresholds.begin();
        return count == 0 ? 0u : guideThresholdPoints[static_cast<std::size_t>(count) - 1u];
    }
    // The radius of a sphere about the middle of the groom's bounds that holds every cluster's bounding sphere, in rest
    // space: what the select pass tests cluster by cluster, as one sphere.
    float cullRadius = 0.0f;
    bool hasSkinBindings = false; // built with a scalp mesh: the roots carry real joints and weights
    bool uploaded = false;
};

struct Instance {
    HairInstanceInfo info;
    std::uint64_t stableSceneID = 0u;
    std::uint32_t attachBone = HAIR_INVALID_INDEX;
    std::shared_ptr<Skeleton> skeleton; // the entity's pose instance, in Bone and PerRoot mode
    // Its rows of guide state while it holds any (HairGuideRowStats): simulated, or keeping the shape it stopped with.
    std::uint32_t guideRowBase = HAIR_INVALID_INDEX;
    std::uint32_t guideRowCount = 0u;
    std::vector<HairCollider> colliders;
    bool collidersUploaded = false;
    bool simResetRequested = false;            // by ResetSimulation or by turning the simulation on
    // Simulation LOD (PlanSimLevels).
    bool simActive = false;        // simulated: its guide state is live
    float simLevel = 0.0f;         // sFrac
    float simTarget = 0.0f;        // what sFrac moves toward
    float simForceLevel = -1.0f;   // >= 0: SetInstanceSimLevel's
    bool simMoving = false;        // sFrac is on its way to simTarget
    float simCoverage = 0.0f;
    float simViewExtent = 0.0f;    // OnScreenExtent of its bounds this frame
    bool simInView = false;        // a view shows part of it: it is to be simulated at the level its size there asks for
    bool simUnseen = false;        // not simulated this frame only because no view shows it (HAIR_SIM_FLAG_UNSEEN)
    bool simPlanned = false;       // PlanSimLevels looked at it this frame: its entity is drawn
    bool simStarved = false;       // in view and to be simulated, but there were no guide rows for it: it follows rigidly
    bool simUnsettled = false;     // out of view, would settle unseen, but there were no free guide rows for it
    // Its guide rows were taken back with its shape (or its unseen settle) lost: it is not settled out of view again,
    // and starts from its rigid pose when a view shows it. Cleared by that start, and when it is too small to simulate.
    bool simEvicted = false;
    double simLastSimulated = 0.0; // State::guideClock when it was last simulated: how old a kept shape is (guideKeepSeconds)
    double simOutOfViewSeconds = 0.0; // simulated time since a view last showed it
    double simSettledSeconds = 0.0;   // simulated time since it last started from its rigid pose
    // Its guide rows hold the shape an earlier simulated frame left: it starts again from that and not from the rigid
    // pose. Only while it holds rows: taking them back clears this.
    bool simStateValid = false;
    double simOffSeconds = 0.0;    // simulated time spent at level 0 while active: the guides' blend back to the rigid pose
    // The highest sFrac of this stretch of simulated time and of the one before, each as long as a demotion takes: a
    // guide the level no longer asks for is still simulated that long, so these say which levels can have such a guide.
    float simHighLevel = 0.0f;
    float simHighLevelBefore = 0.0f;
    double simHighSeconds = 0.0;
    std::uint64_t simulatedFrame = kNoFrame;   // the last frame its guides were simulated in
    HairSimMetrics simMetrics;                 // the newest readback
    HairInstanceGPU gpu{};
    bool gpuValid = false; // `gpu` is what the instance buffer holds
    // Self-check only: the skin matrices (inverse bind * pose) the previous frame's dispatch read. One matrix in Bone
    // mode, the whole palette in PerRoot mode.
    std::vector<XMFLOAT4X4> previousSkin;
    std::uint64_t previousSkinFrame = kNoFrame;
};

// One root of a self-check: where its row is and what the CPU expects there.
struct ExpectedRoot {
    std::uint32_t instance = 0u;
    std::uint32_t row = 0u;
    bool skinned = false;
    bool hasPrevious = false;
    bool unknown = false; // the CPU could not tell what the dispatch read
    HairRootFrame current{};
    HairRootFrame previous{};
};

// Contiguous row ranges out of one buffer: first fit, merged when freed, never moved.
struct RowAllocator {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> free; // offset and count, ascending, no two adjacent
    std::uint32_t capacity = 0u;
    std::uint32_t used = 0u;
    std::uint32_t highWater = 0u;

    void Reset(std::uint32_t rows)
    {
        capacity = rows;
        used = 0u;
        free.clear();
        if (rows != 0u) free.emplace_back(0u, rows);
    }
    std::optional<std::uint32_t> Allocate(std::uint32_t count)
    {
        if (count == 0u) return 0u;
        for (auto range = free.begin(); range != free.end(); ++range) {
            if (range->second < count) continue;
            const std::uint32_t offset = range->first;
            range->first += count;
            range->second -= count;
            if (range->second == 0u) free.erase(range);
            used += count;
            highWater = (std::max)(highWater, used);
            return offset;
        }
        return std::nullopt;
    }
    void Free(std::uint32_t offset, std::uint32_t count)
    {
        if (count == 0u) return;
        auto range = free.insert(std::ranges::lower_bound(free, offset, {}, &std::pair<std::uint32_t, std::uint32_t>::first),
            { offset, count });
        if (range + 1 != free.end() && range->first + range->second == (range + 1)->first) {
            range->second += (range + 1)->second;
            range = free.erase(range + 1) - 1;
        }
        if (range != free.begin() && (range - 1)->first + (range - 1)->second == range->first) {
            (range - 1)->second += range->second;
            free.erase(range);
        }
        used -= count;
    }
    std::uint32_t LargestFree() const
    {
        std::uint32_t largest = 0u;
        for (const auto& range : free) largest = (std::max)(largest, range.second);
        return largest;
    }
};

// The pooled buffers, for HairStats::bufferUse.
enum BufferId : std::size_t {
    kBufferClusters, kBufferAggregates, kBufferStrands, kBufferRootBindings, kBufferGuideBindings, kBufferGuideStrandIndices,
    kBufferGuidePointRanges, kBufferPoints, kBufferInstances, kBufferColliders, kBufferFrameState, kBufferSimFrame,
    kBufferCounters, kBufferSimMetrics, kBufferSimGroups, kBufferGuidePoints, kBufferVisibleClusters, kBufferPoolVertices,
    kBufferPoolSegmentIds, kBufferRootCheckList, kBufferRootCheck, kBufferProbe, kBufferCount
};

std::string LowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// Yuksel .hair files are Z-up and in their own units: onto +Y up (as the USD importer rotates Z-up stages) and metres,
// then to where the caller wants the file's origin.
void ConvertHairFileUnits(GroomSource& source, float unitScale, bool zUp, Float3 offset)
{
    for (auto& position : source.positions) {
        if (zUp) position = { position.x, position.z, -position.y };
        position = position * unitScale + offset;
    }
    for (auto& width : source.widths) width *= unitScale;
}

std::shared_ptr<org::Buffer> MakeBuffer(std::uint32_t rows, std::size_t stride, bool unorderedAccess, const char* name)
{
    auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(rows, static_cast<std::uint32_t>(stride), unorderedAccess);
    // Persistent contents: uploaded once, or written in one frame and read in later ones.
    buffer->SetAllowAlias(false);
    buffer->Materialize();
    buffer->SetName(name);
    return buffer;
}

void UploadRows(const HairUpload& upload, const std::shared_ptr<org::Buffer>& buffer, std::size_t firstRow, const void* rows,
    std::size_t rowCount, std::size_t stride)
{
    const auto* bytes = static_cast<const std::byte*>(rows);
    const std::size_t total = rowCount * stride;
    for (std::size_t done = 0u; done < total;) {
        const std::size_t chunk = (std::min)(kUploadChunkBytes, total - done);
        upload(buffer, firstRow * stride + done, bytes + done, chunk);
        done += chunk;
    }
}

XMVECTOR Load(const Float3& value) { return XMVectorSet(value.x, value.y, value.z, 0.0f); }

Float3 Store(FXMVECTOR value) { return { XMVectorGetX(value), XMVectorGetY(value), XMVectorGetZ(value) }; }

// hairRootFrame.hlsli's HairMakeRootFrame.
HairRootFrame MakeRootFrame(const HairRootBinding& binding, CXMMATRIX skin, CXMMATRIX model, std::uint64_t frameNumber)
{
    HairRootFrame frame{};
    frame.position = Store(XMVector3Transform(XMVector3Transform(Load(binding.restPosition), skin), model));
    frame.writtenFrame = static_cast<std::uint32_t>(frameNumber);
    frame.normal = Store(XMVector3Normalize(XMVector3TransformNormal(XMVector3TransformNormal(Load(binding.restNormal), skin), model)));
    frame.tangent = Store(XMVector3Normalize(XMVector3TransformNormal(XMVector3TransformNormal(Load(binding.restTangent), skin), model)));
    return frame;
}

// hairRootFrame.hlsli's HairLoadRootSkin, over a palette of skin matrices. `palette` holds the attach bone alone in Bone mode.
XMMATRIX RootSkin(HairAttachMode mode, const HairRootBinding& binding, const std::vector<XMFLOAT4X4>& palette)
{
    if (mode == HairAttachMode::Bone) {
        return palette.empty() ? XMMatrixIdentity() : XMLoadFloat4x4(&palette.front());
    }
    XMMATRIX sum = XMMatrixSet(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    float weightSum = 0.0f;
    for (std::uint32_t i = 0u; i < 4u; ++i) {
        const float weight = binding.weights[i];
        if (weight > 0.0f && binding.joints[i] < palette.size()) {
            sum += XMLoadFloat4x4(&palette[binding.joints[i]]) * weight;
            weightSum += weight;
        }
    }
    return weightSum > 1.0e-8f ? sum * (1.0f / weightSum) : XMMatrixIdentity();
}

float Distance(const Float3& a, const Float3& b) { return Length(a - b); }

// Simulation LOD: an instance that is not simulated starts when its coverage asks for at least this level, and the
// level of one that is sets off toward what its coverage asks for only once that is this far away (it then goes all
// the way). Together they keep a groom at a steady distance from flickering between states.
constexpr float kSimLevelStart = 0.25f;
constexpr float kSimLevelDeadband = 0.02f;

// The simulation level a coverage asks for, for a groom of `levels` guide levels (HairSimSettings).
float SimLevelForCoverage(const HairSimSettings& sim, float coverage, float levels)
{
    if (levels <= 0.0f || !(coverage > sim.lodOffCoverage)) return 0.0f;
    if (coverage >= sim.lodFullCoverage) return levels;
    if (coverage >= sim.lodLevel0Coverage) {
        return 1.0f + (levels - 1.0f) * std::log(coverage / sim.lodLevel0Coverage) / std::log(sim.lodFullCoverage / sim.lodLevel0Coverage);
    }
    return std::log(coverage / sim.lodOffCoverage) / std::log(sim.lodLevel0Coverage / sim.lodOffCoverage);
}

// The select pass's frustum test grows a cluster's sphere by HAIR_CULL_RADIUS_MARGIN. The CPU's test of an instance's
// sphere, which decides whether the instance is simulated, grows it by this instead: the camera and the bones the CPU
// sees can be a frame from the ones the frame draws with, and an instance has to be simulated before a strand of it is
// drawn.
constexpr float kSimViewRadiusMargin = 0.5f;
// OnScreenExtent of a sphere that the view is inside of, or that reaches behind it.
constexpr float kWholeView = 1.0e9f;

// The one place that answers whether a view needs an instance's hair, and how much of it: the size on screen, over the
// view's height, of the part of the sphere (centre and radius in world space) that is inside a view. That is the
// diameter of its projection times the smaller of the shares of its width and of its height that are on screen; a
// sphere wholly in view gives its coverage, as PlanSimLevels computes it. 0: no view shows any of it, by the test the
// select pass gives a cluster (the six view-space planes). Hair is drawn in the main view only, so there is one view
// here; a second one that shows hair is the larger of the two answers.
float OnScreenExtent(const HairFrameInputs& inputs, const Float3& center, float radius)
{
    const HairView& view = inputs.mainView;
    const XMVECTOR viewCenter = XMVector3Transform(XMVectorSetW(Load(center), 1.0f),
        XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(view.view.data())));
    for (const auto& plane : view.planes) {
        const float distance = plane[0] * XMVectorGetX(viewCenter) + plane[1] * XMVectorGetY(viewCenter) +
            plane[2] * XMVectorGetZ(viewCenter) + plane[3];
        if (distance < -radius) return 0.0f;
    }
    const float depth = -XMVectorGetZ(viewCenter);
    if (depth <= radius) return kWholeView;
    // The projection's interval on each screen axis, in units of half the view: the share of it inside [-1, 1].
    const auto share = [&](float viewCoordinate, float projectionScale) {
        const float middle = viewCoordinate * projectionScale / depth;
        const float half = radius * projectionScale / depth;
        return half > 0.0f ? std::clamp(((std::min)(middle + half, 1.0f) - (std::max)(middle - half, -1.0f)) / (2.0f * half), 0.0f, 1.0f) : 0.0f;
    };
    const float visible =
        (std::min)(share(XMVectorGetX(viewCenter), view.projectionScaleX), share(XMVectorGetY(viewCenter), view.projectionScaleY));
    // A sphere the planes let through has some of it on screen, however little.
    return (std::max)(radius * view.projectionScaleY / depth * visible, 1.0e-6f);
}

// Copies bytes out of a paged buffer image. False when the range is not in the image.
bool ReadImage(const org::services::BufferImage& image, std::size_t offset, void* out, std::size_t byteCount)
{
    if (offset > image.ByteSize() || byteCount > image.ByteSize() - offset) return false;
    constexpr std::size_t pageBytes = org::services::BufferImage::PageBytes;
    auto* destination = static_cast<std::byte*>(out);
    while (byteCount != 0u) {
        const std::size_t pageIndex = offset / pageBytes;
        const std::size_t inPage = offset % pageBytes;
        const std::size_t count = (std::min)(byteCount, pageBytes - inPage);
        const auto& pages = image.Pages();
        const auto* page = pageIndex < pages.size() ? pages[pageIndex].get() : nullptr;
        if (page && page->size() >= inPage + count) {
            std::memcpy(destination, page->data() + inPage, count);
        } else {
            std::memset(destination, 0, count); // the image is zero where nothing was written
        }
        destination += count;
        offset += count;
        byteCount -= count;
    }
    return true;
}

// The model and previous model matrix of a PerObjectCB row, which begins with them.
struct ObjectMatrices {
    XMFLOAT4X4 model;
    XMFLOAT4X4 previousModel;
};

// Writes RGBA8 or BGRA8 rows, top row first, as a 32-bit top-down .bmp. False when the file cannot be written.
bool WriteBmp(const std::filesystem::path& path, const std::byte* pixels, std::uint32_t rowPitch, std::uint32_t width,
    std::uint32_t height, bool rgba)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::uint32_t imageBytes = width * height * 4u;
    const std::uint32_t headerBytes = 14u + 40u;
    std::vector<std::uint8_t> header(headerBytes, 0u);
    const auto put32 = [&header](std::size_t at, std::uint32_t value) { std::memcpy(header.data() + at, &value, sizeof(value)); };
    const auto put16 = [&header](std::size_t at, std::uint16_t value) { std::memcpy(header.data() + at, &value, sizeof(value)); };
    header[0] = 'B';
    header[1] = 'M';
    put32(2u, headerBytes + imageBytes);
    put32(10u, headerBytes);
    put32(14u, 40u);
    put32(18u, width);
    put32(22u, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height))); // negative: rows run top to bottom
    put16(26u, 1u);
    put16(28u, 32u);
    put32(34u, imageBytes);
    file.write(reinterpret_cast<const char*>(header.data()), headerBytes);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4u);
    for (std::uint32_t y = 0u; y < height; ++y) {
        std::memcpy(row.data(), pixels + static_cast<std::size_t>(y) * rowPitch, row.size());
        for (std::uint32_t x = 0u; x < width; ++x) {
            if (rgba) std::swap(row[x * 4u], row[x * 4u + 2u]); // the file is BGRA
            row[x * 4u + 3u] = 255u;
        }
        file.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    return static_cast<bool>(file);
}

const char* ModeName(HairAttachMode mode)
{
    switch (mode) {
    case HairAttachMode::Bone: return "bone";
    case HairAttachMode::PerRoot: return "per-root skin";
    default: return "rigid";
    }
}

} // namespace

struct HairRuntime::State {
    mutable std::mutex mutex;
    HairCapacities capacities;
    std::vector<Asset> assets;
    std::vector<Instance> instances;
    std::uint32_t usedClusters = 0u;
    std::uint32_t usedStrands = 0u;
    std::uint32_t usedGuides = 0u;
    std::uint32_t usedPoints = 0u;
    std::uint32_t instanceStrands = 0u; // strands summed over instances
    std::uint64_t generation = 0u; // bumped by Clear: readbacks asked for before it are dropped

    // Guide state, lent to instances (HairGuideRowStats says by what rules).
    RowAllocator guideRows;
    HairGuideRowStats guideStats; // the running counts; Stats fills in the rest
    double guideClock = 0.0;      // simulated seconds, never reset: when an instance was last simulated
    std::array<HairBufferUse, kBufferCount> bufferUse;
    std::uint32_t lastSimGroups = 0u;
    std::uint32_t lastRootChecks = 0u;

    std::shared_ptr<HairGpuBuffers> buffers;
    std::uint64_t bufferBytes = 0u;
    HairFramePlan plan;
    // The instances the visible-cluster list on the GPU was selected for. A frozen LOD keeps the list while they match.
    std::vector<HairInstanceDispatch> selectedInstances;
    bool selectionValid = false;
    HairLodSettings lod;
    HairDebugView debugView = HairDebugView::Off;
    br::render::SceneMutationSubscription sceneHook;

    std::uint64_t lastFrame = kNoFrame;
    std::uint64_t frames = 0u;
    std::uint64_t evaluateDispatches = 0u;
    std::uint64_t assetBytesUploaded = 0u;
    std::uint64_t instanceBytesUploaded = 0u;

    bool counterReadbackWanted = false;
    bool continuousCounters = false;
    bool counterReadbackInFlight = false;
    HairGpuCounters gpuCounters;
    std::deque<HairGpuCounters> counterHistory; // continuous counters only, oldest first
    HairDebugCapture debugCapture;
    bool debugCaptureRequested = false; // asked for, not yet taken by the overlay pass
    HairDebugCapture probe;
    bool probeRequested = false; // asked for, not yet planned by UpdateFrame
    struct DumpRequest {
        std::string buffer;
        std::string path;
        std::uint32_t rows = 0u;
    };
    std::vector<DumpRequest> dumpRequests; // asked for, not yet read back
    std::uint32_t dumpsInFlight = 0u;
    std::uint32_t dumpsWritten = 0u;
    std::uint32_t dumpsFailed = 0u;

    HairSelfCheckStatus selfCheck;
    bool selfCheckInFlight = false;
    std::uint64_t selfCheckFrame = kNoFrame; // the last frame a check was asked for

    HairSimSettings sim;
    double simAccumulator = 0.0; // frame time not yet simulated
    HairSimStatus simStatus;     // the counters; its settings and metrics are filled when asked for
    bool simReadbackWanted = false;
    bool simReadbackInFlight = false;

    void ResolvePendingInstances(Scene& scene);
    void CreateBuffers();
    void EnsureVisibilityTarget(std::uint32_t width, std::uint32_t height);
    void UploadAsset(Asset& asset, const HairUpload& upload);
    void RequestCounterReadback(const std::shared_ptr<State>& self, const HairFrameInputs& inputs, bool selected);
    void RequestSelfCheck(const std::shared_ptr<State>& self, std::uint64_t frameNumber, std::vector<ExpectedRoot> expected);
    void RequestProbe(const std::shared_ptr<State>& self);
    void RequestSimMetricsReadback(const std::shared_ptr<State>& self, std::uint64_t frameNumber);
    void RequestDumps(const std::shared_ptr<State>& self, std::uint64_t frameNumber);
    // The buffer DumpBuffer calls `name`, with its row size; null for an unknown name or before the buffers exist.
    org::Buffer* DumpTarget(const std::string& name, std::size_t& stride) const;
    HairSimStatus DescribeSim();
    std::uint32_t PlanSimSteps(float deltaSeconds);
    void PlanSimLevels(const HairFrameInputs& inputs, float simulatedSeconds);
    std::uint32_t GuideRowBudget() const
    {
        return sim.guidePointBudget != 0u ? (std::min)(sim.guidePointBudget, guideRows.capacity) : guideRows.capacity;
    }
    bool AcquireGuideRows(Instance& instance, bool inView);
    void ReleaseGuideRows(Instance& instance);
    void EvictGuideRows(Instance& instance, bool byAge);
    Instance* PickGuideRowVictim(bool includeInView, std::uint64_t* evictableRows = nullptr);
    void Use(BufferId id, std::uint64_t rows)
    {
        bufferUse[id].usedRows = rows;
        bufferUse[id].highWaterRows = (std::max)(bufferUse[id].highWaterRows, rows);
    }
    void RefreshBufferUse();
};

// Gives an instance its guide rows, if it has none: free ones, within the budget. An instance in view may take them from
// instances that are not, the smallest on screen first. False when there are none to be had; nothing is evicted in vain.
bool HairRuntime::State::AcquireGuideRows(Instance& instance, bool inView)
{
    if (instance.guideRowBase != HAIR_INVALID_INDEX) return true;
    const std::uint32_t rows = assets[instance.info.asset].guidePointCount;
    const std::uint32_t budget = GuideRowBudget();
    if (rows > budget) return false;
    const auto take = [&] {
        if (guideRows.used > budget - rows) return false;
        const auto base = guideRows.Allocate(rows);
        if (!base) return false;
        instance.guideRowBase = *base;
        instance.guideRowCount = rows;
        // New rows hold nothing of this instance: its next simulated frame starts every guide again, also when it was
        // simulated in the frame before and lost its rows in this one.
        instance.simulatedFrame = kNoFrame;
        ++guideStats.allocations;
        return true;
    };
    if (take()) return true;
    if (!inView) return false;
    std::uint64_t evictable = 0u;
    PickGuideRowVictim(false, &evictable);
    if (guideRows.used - evictable > budget - rows) return false;
    while (Instance* victim = PickGuideRowVictim(false)) {
        EvictGuideRows(*victim, false);
        if (take()) return true;
    }
    return false; // fragmentation: the rows were there, in pieces
}

// Gives an instance's rows back when nothing is lost by it: no shape is kept in them.
void HairRuntime::State::ReleaseGuideRows(Instance& instance)
{
    instance.simStateValid = false;
    if (instance.guideRowBase == HAIR_INVALID_INDEX) return;
    guideRows.Free(instance.guideRowBase, instance.guideRowCount);
    instance.guideRowBase = HAIR_INVALID_INDEX;
    instance.guideRowCount = 0u;
    ++guideStats.releases;
}

// Takes an instance's rows back with what they hold: its kept shape, or the simulation it was running.
void HairRuntime::State::EvictGuideRows(Instance& instance, bool byAge)
{
    if (instance.guideRowBase == HAIR_INVALID_INDEX) return;
    guideRows.Free(instance.guideRowBase, instance.guideRowCount);
    instance.guideRowBase = HAIR_INVALID_INDEX;
    instance.guideRowCount = 0u;
    instance.simActive = false;
    instance.simStateValid = false;
    instance.simEvicted = true;
    ++guideStats.evictions;
    ++(byAge ? guideStats.evictionsByAge : guideStats.evictionsForRows);
}

// The holder of guide rows to take them from next: of the instances no view shows, the one that would be smallest on
// screen (its coverage), whether it keeps a shape or is being simulated out of view; one whose entity is not drawn
// before any. Else, when asked, the smallest instance in view. `evictableRows` gets the rows of every holder out of view.
//
// Smallest first, and not the one simulated longest ago: with more grooms near enough to simulate than there are rows
// for, a camera that turns in place comes to them in turn, and the one simulated longest ago is the one it shows next.
// Every entry is then a start from the rigid pose, each of which is simulated for lodSettleSeconds out of view, and
// those starts evict each other. By size the same turn keeps the shapes of the large grooms and takes the small ones'.
Instance* HairRuntime::State::PickGuideRowVictim(bool includeInView, std::uint64_t* evictableRows)
{
    Instance* best = nullptr;
    bool bestInView = false;
    float bestCoverage = 0.0f;
    for (auto& instance : instances) {
        if (instance.guideRowBase == HAIR_INVALID_INDEX) continue;
        const bool inView = instance.simPlanned && instance.simInView;
        if (!inView && evictableRows) *evictableRows += instance.guideRowCount;
        if (inView && !includeInView) continue;
        const float coverage = instance.simPlanned ? instance.simCoverage : -1.0f;
        if (!best || (bestInView && !inView) || (bestInView == inView && coverage < bestCoverage)) {
            best = &instance;
            bestInView = inView;
            bestCoverage = coverage;
        }
    }
    return best;
}

void HairRuntime::State::RefreshBufferUse()
{
    const auto instanceCount = static_cast<std::uint64_t>(instances.size());
    Use(kBufferClusters, usedClusters);
    Use(kBufferAggregates, usedClusters);
    Use(kBufferStrands, usedStrands);
    Use(kBufferRootBindings, usedStrands);
    Use(kBufferGuideBindings, usedGuides);
    Use(kBufferGuideStrandIndices, usedGuides);
    Use(kBufferGuidePointRanges, usedGuides);
    Use(kBufferPoints, usedPoints);
    Use(kBufferInstances, instanceCount);
    Use(kBufferColliders, instanceCount * HAIR_MAX_COLLIDERS_PER_INSTANCE);
    Use(kBufferSimMetrics, sim.enabled ? instanceCount * HAIR_SIM_METRIC_COUNT : 0u);
    Use(kBufferFrameState, 1u);
    Use(kBufferSimFrame, 1u);
    Use(kBufferCounters, HAIR_COUNTER_COUNT);
    Use(kBufferSimGroups, lastSimGroups);
    Use(kBufferRootCheckList, lastRootChecks);
    Use(kBufferRootCheck, lastRootChecks);
    bufferUse[kBufferGuidePoints].usedRows = guideRows.used;
    bufferUse[kBufferGuidePoints].highWaterRows = guideRows.highWater;
    // The visible-cluster list and the strand pool are filled on the GPU: the counter readback sets theirs. The probe
    // buffer's is set when a probe is written.
}

HairRuntime::HairRuntime(HairCapacities capacities)
    : m_state(std::make_shared<State>())
{
    m_state->capacities = capacities;
    m_state->guideRows.Reset(capacities.guidePoints);
}

HairRuntime::~HairRuntime() { Shutdown(); }

void HairRuntime::Initialize(Renderer& renderer)
{
    // The hook runs inside Renderer::Update, where no asynchronous scene update is touching the scene.
    m_state->sceneHook = renderer.AddSceneMutationHook([state = std::weak_ptr<State>(m_state)](Scene& scene) {
        if (const auto locked = state.lock()) {
            std::scoped_lock lock(locked->mutex);
            locked->ResolvePendingInstances(scene);
        }
    });
}

void HairRuntime::Shutdown()
{
    m_state->sceneHook.Reset();
    std::scoped_lock lock(m_state->mutex);
    ++m_state->generation;
    m_state->plan = HairFramePlan{};
    m_state->selectionValid = false;
    m_state->instances.clear();
    m_state->assets.clear();
    m_state->buffers.reset();
}

std::optional<std::uint32_t> HairRuntime::LoadGroom(const HairGroomRequest& request, std::string& error)
{
    Asset asset;
    if (request.path.empty()) {
        asset.source = "procedural";
        const GroomSource source = GenerateGroomOnSphereCap(request.procedural);
        if (!BuildGroom(source, nullptr, request.build, asset.groom, &error)) return std::nullopt;
    } else {
        const std::filesystem::path path(request.path);
        const std::string extension = LowerExtension(path);
        const float unitScale = request.unitScale > 0.0f ? request.unitScale : extension == ".hair" ? 0.003f : 0.01f;
        asset.source = request.path;
        if (extension == ".groom") {
            if (!LoadGroomFile(path, asset.groom, &error)) return std::nullopt;
        } else if (extension == ".hair") {
            GroomSource source;
            if (!LoadHairFile(path, source, &error)) return std::nullopt;
            ConvertHairFileUnits(source, unitScale, request.zUp.value_or(true), request.offset);
            if (!BuildGroom(source, nullptr, request.build, asset.groom, &error)) return std::nullopt;
        } else if (extension == ".abc") {
            AlembicGroomImportSettings settings;
            settings.unitScale = unitScale;
            settings.upAxis = request.zUp.value_or(false) ? GroomUpAxis::Z : GroomUpAxis::Y;
            GroomSource source;
            if (!LoadAlembicGroom(path, settings, source, &error)) return std::nullopt;
            for (auto& position : source.positions) position = position + request.offset;
            if (!BuildGroom(source, nullptr, request.build, asset.groom, &error)) return std::nullopt;
        } else {
            error = "unknown groom source '" + request.path + "': expected .groom, .hair or .abc";
            return std::nullopt;
        }
    }

    const BuiltGroom& groom = asset.groom;
    const auto clusters = static_cast<std::uint32_t>(groom.clusters.size());
    const auto strands = static_cast<std::uint32_t>(groom.strands.size());
    const auto guides = groom.GuideCount();
    const auto points = static_cast<std::uint32_t>(groom.points.size());
    asset.hasSkinBindings = std::ranges::any_of(groom.rootBindings,
        [](const HairRootBinding& binding) { return binding.triangleIndex != HAIR_INVALID_INDEX; });
    // A guide keeps its cluster's stored vertex level, so guides differ in point count.
    asset.guidePointRanges.reserve(guides);
    for (std::uint32_t rank = 0u; rank < guides; ++rank) {
        const HairStrandRecord& strand = groom.strands[groom.guideStrandIndices[rank]];
        const std::uint32_t count = HairStrandPointCount(groom.clusters[strand.clusterIndex].vertexLevel);
        asset.guidePointRanges.push_back({ asset.guidePointCount, count });
        asset.guidePointCount += count;
    }
    const Float3 boundsMiddle = (groom.boundsMin + groom.boundsMax) * 0.5f;
    for (const HairClusterHeader& cluster : groom.clusters) {
        asset.cullRadius = (std::max)(asset.cullRadius, Distance(cluster.boundsCenter, boundsMiddle) + cluster.boundsRadius);
    }
    if (groom.GuideLevelCount() > HAIR_MAX_GUIDE_LEVELS) {
        error = "groom '" + asset.source + "' has " + std::to_string(groom.GuideLevelCount()) + " guide levels; the runtime takes " +
            std::to_string(HAIR_MAX_GUIDE_LEVELS);
        return std::nullopt;
    }
    {
        std::vector<std::pair<float, std::uint32_t>> thresholds; // level a guide is simulated from, its points
        for (std::uint32_t rank = 0u; rank < guides; ++rank) {
            const HairGuideBinding& binding = groom.guideBindings[rank];
            thresholds.emplace_back(binding.level == 0u ? 0.0f : static_cast<float>(binding.level) + binding.randomOffset,
                asset.guidePointRanges[rank].count);
        }
        std::ranges::sort(thresholds);
        std::uint32_t points = 0u;
        for (const auto& [threshold, count] : thresholds) {
            asset.guideThresholds.push_back(threshold);
            asset.guideThresholdPoints.push_back(points += count);
        }
    }

    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    const auto fits = [&](const char* what, std::uint32_t needed, std::uint32_t used, std::uint32_t capacity) {
        if (needed <= capacity - used) return true;
        error = "groom '" + asset.source + "' needs " + std::to_string(needed) + " " + what + " but " + std::to_string(used) +
            " of " + std::to_string(capacity) + " are in use (HairCapacities)";
        return false;
    };
    if (!fits("clusters", clusters, state.usedClusters, state.capacities.clusters) ||
        !fits("strands", strands, state.usedStrands, state.capacities.strands) ||
        !fits("guides", guides, state.usedGuides, state.capacities.guides) ||
        !fits("points", points, state.usedPoints, state.capacities.points)) {
        spdlog::error("Hair: {}", error);
        return std::nullopt;
    }
    asset.clusterBase = state.usedClusters;
    asset.strandBase = state.usedStrands;
    asset.guideBase = state.usedGuides;
    asset.pointBase = state.usedPoints;
    state.usedClusters += clusters;
    state.usedStrands += strands;
    state.usedGuides += guides;
    state.usedPoints += points;
    const auto id = static_cast<std::uint32_t>(state.assets.size());
    spdlog::info("Hair: loaded groom {} from '{}': {} strands, {} clusters, {} guides, {} points{}", id, asset.source, strands,
        clusters, guides, points, asset.hasSkinBindings ? ", with skin bindings" : "");
    state.assets.push_back(std::move(asset));
    return id;
}

std::optional<std::uint32_t> HairRuntime::AttachGroom(std::uint32_t asset, std::string entityName, std::string boneName,
    const HairMaterial& material, std::string& error)
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    if (asset >= state.assets.size()) {
        error = "no groom asset " + std::to_string(asset);
        return std::nullopt;
    }
    if (entityName.empty()) {
        error = "an instance needs an entity name";
        return std::nullopt;
    }
    // An instance takes nothing but its row: no root frames are stored, and guide state is lent while it is simulated.
    if (state.instances.size() >= state.capacities.instances) {
        error = "all " + std::to_string(state.capacities.instances) + " hair instances are in use (HairCapacities)";
        spdlog::error("Hair: {}", error);
        return std::nullopt;
    }
    Instance instance;
    instance.info.id = static_cast<std::uint32_t>(state.instances.size());
    instance.info.asset = asset;
    instance.info.entity = std::move(entityName);
    instance.info.bone = std::move(boneName);
    instance.info.material = material;
    instance.info.strands = static_cast<std::uint32_t>(state.assets[asset].groom.strands.size());
    instance.info.guideRows = state.assets[asset].guidePointCount;
    state.instanceStrands += instance.info.strands;
    state.instances.push_back(std::move(instance));
    return state.instances.back().info.id;
}

void HairRuntime::Clear()
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    ++state.generation;
    state.plan = HairFramePlan{};
    state.selectionValid = false;
    state.instances.clear();
    state.assets.clear();
    state.usedClusters = state.usedStrands = state.usedGuides = state.usedPoints = state.instanceStrands = 0u;
    state.guideRows.Reset(state.capacities.guidePoints);
}

void HairRuntime::SetSelfCheck(bool enabled, std::uint32_t rootsPerInstance)
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    state.selfCheck = HairSelfCheckStatus{};
    state.selfCheck.enabled = enabled;
    state.selfCheck.rootsPerInstance = enabled ? (std::max)(1u, rootsPerInstance) : 0u;
    for (auto& instance : state.instances) instance.previousSkinFrame = kNoFrame;
}

HairStats HairRuntime::Stats()
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    HairStats stats;
    stats.assets = static_cast<std::uint32_t>(state.assets.size());
    stats.instances = static_cast<std::uint32_t>(state.instances.size());
    HairGuideRowStats& guideRows = stats.guideRows;
    guideRows = state.guideStats;
    guideRows.capacityRows = state.guideRows.capacity;
    guideRows.budgetRows = state.GuideRowBudget();
    guideRows.rowsInUse = state.guideRows.used;
    guideRows.highWaterRows = state.guideRows.highWater;
    guideRows.largestFreeRows = state.guideRows.LargestFree();
    for (const auto& instance : state.instances) {
        stats.attachedInstances += instance.info.state == HairInstanceState::Attached ? 1u : 0u;
        stats.failedInstances += instance.info.state == HairInstanceState::Failed ? 1u : 0u;
        stats.activeInstances += instance.info.active ? 1u : 0u;
        stats.instanceInfo.push_back(instance.info);
        stats.instanceInfo.back().guideRowBase = instance.guideRowBase;
        guideRows.starvedInstances += instance.simStarved ? 1u : 0u;
        guideRows.unsettledInstances += instance.simUnsettled ? 1u : 0u;
        if (instance.guideRowBase == HAIR_INVALID_INDEX) continue;
        ++guideRows.holders;
        if (instance.simActive && instance.simPlanned) {
            ++guideRows.activeHolders;
            guideRows.rowsActive += instance.guideRowCount;
        }
    }
    for (const auto& asset : state.assets) stats.pendingUploadAssets += asset.uploaded ? 0u : 1u;
    stats.clusters = state.usedClusters;
    stats.strands = state.usedStrands;
    stats.guides = state.usedGuides;
    stats.points = state.usedPoints;
    stats.instanceStrands = state.instanceStrands;
    stats.guidePoints = state.guideRows.used;
    stats.capacities = state.capacities;
    stats.buffersCreated = state.buffers != nullptr;
    stats.bufferBytes = state.bufferBytes;
    if (state.buffers) {
        state.RefreshBufferUse();
        stats.bufferUse.assign(state.bufferUse.begin(), state.bufferUse.end());
    }
    stats.assetBytesUploaded = state.assetBytesUploaded;
    stats.instanceBytesUploaded = state.instanceBytesUploaded;
    stats.frames = state.frames;
    stats.evaluateDispatches = state.evaluateDispatches;
    stats.gpuCounters = state.gpuCounters;
    stats.continuousCounters = state.continuousCounters;
    stats.debugCapture = state.debugCapture;
    stats.probe = state.probe;
    stats.dumpsPending = static_cast<std::uint32_t>(state.dumpRequests.size()) + state.dumpsInFlight;
    stats.dumpsWritten = state.dumpsWritten;
    stats.dumpsFailed = state.dumpsFailed;
    stats.gpuCounterHistory.assign(state.counterHistory.begin(), state.counterHistory.end());
    stats.lod = state.lod;
    stats.debugView = state.debugView;
    if (state.buffers) {
        stats.visibilityWidth = state.buffers->visibilityWidth;
        stats.visibilityHeight = state.buffers->visibilityHeight;
    }
    stats.selfCheck = state.selfCheck;
    stats.sim = state.DescribeSim();
    state.counterReadbackWanted = true;
    return stats;
}

HairSimStatus HairRuntime::State::DescribeSim()
{
    HairSimStatus status = simStatus;
    status.settings = sim;
    status.guidePoints = guideRows.used;
    for (const auto& instance : instances) {
        if (instance.simMetrics.valid) status.metrics.push_back(instance.simMetrics);
    }
    simReadbackWanted = true;
    return status;
}

void HairRuntime::SetSimSettings(const HairSimSettings& settings)
{
    HairSimSettings clamped = settings;
    clamped.damping = (std::max)(settings.damping, 0.0f);
    clamped.stretchCompliance = (std::max)(settings.stretchCompliance, 0.0f);
    clamped.bendCompliance = (std::max)(settings.bendCompliance, 0.0f);
    clamped.shapeCompliance = (std::max)(settings.shapeCompliance, 0.0f);
    clamped.stepSeconds = std::clamp(settings.stepSeconds, 1.0e-3f, 0.1f);
    clamped.substeps = std::clamp(settings.substeps, 1u, 64u);
    clamped.maxStepsPerFrame = std::clamp(settings.maxStepsPerFrame, 1u, 16u);
    clamped.gustAmplitude = (std::max)(settings.gustAmplitude, 0.0f);
    clamped.friction = std::clamp(settings.friction, 0.0f, 1.0f);
    clamped.collisionMargin = (std::max)(settings.collisionMargin, 0.0f);
    clamped.teleportDistance = (std::max)(settings.teleportDistance, 1.0e-3f);
    clamped.frameSeconds = std::clamp(settings.frameSeconds, 0.0f, 1.0f);
    clamped.lodFullCoverage = (std::max)(settings.lodFullCoverage, 1.0e-4f);
    clamped.lodLevel0Coverage = std::clamp(settings.lodLevel0Coverage, 1.0e-5f, clamped.lodFullCoverage * 0.999f);
    clamped.lodOffCoverage = std::clamp(settings.lodOffCoverage, 1.0e-6f, clamped.lodLevel0Coverage * 0.999f);
    clamped.lodRate = (std::max)(settings.lodRate, 0.0f);
    clamped.lodForceLevel = settings.lodForceLevel < 0.0f ? -1.0f : (std::min)(settings.lodForceLevel, static_cast<float>(HAIR_MAX_GUIDE_LEVELS));
    clamped.promoteSeconds = std::clamp(settings.promoteSeconds, 0.0f, 10.0f);
    clamped.demoteSeconds = std::clamp(settings.demoteSeconds, 0.0f, 10.0f);
    clamped.lodViewGraceSeconds = std::clamp(settings.lodViewGraceSeconds, 0.0f, 60.0f);
    clamped.lodSettleSeconds = std::clamp(settings.lodSettleSeconds, 0.0f, 60.0f);
    clamped.guideKeepSeconds = (std::max)(settings.guideKeepSeconds, 0.0f);
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    if (clamped.enabled && !state.sim.enabled) {
        // A fresh start: the guides go to their rest pose in the first simulated frame of each instance.
        state.simAccumulator = 0.0;
        state.simStatus = HairSimStatus{};
        for (auto& instance : state.instances) instance.simMetrics = HairSimMetrics{};
    }
    state.sim = clamped;
}

HairSimSettings HairRuntime::SimSettings() const
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->sim;
}

void HairRuntime::ResetSimulation()
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    state.simAccumulator = 0.0;
    state.simStatus = HairSimStatus{};
    for (auto& instance : state.instances) {
        instance.simResetRequested = true;
        instance.simStateValid = false; // an instance that is not simulated gives its rows back in the next frame
        instance.simEvicted = false;
    }
}

bool HairRuntime::SetInstanceSimLevel(std::uint32_t instance, float level, std::string& error)
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    if (instance >= state.instances.size()) {
        error = "no hair instance " + std::to_string(instance);
        return false;
    }
    state.instances[instance].simForceLevel = level < 0.0f ? -1.0f : level;
    return true;
}

bool HairRuntime::SetColliders(std::uint32_t instance, const std::vector<HairCollider>& colliders, std::string& error)
{
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    if (instance >= state.instances.size()) {
        error = "no hair instance " + std::to_string(instance);
        return false;
    }
    if (colliders.size() > HAIR_MAX_COLLIDERS_PER_INSTANCE) {
        error = "an instance takes at most " + std::to_string(HAIR_MAX_COLLIDERS_PER_INSTANCE) + " colliders";
        return false;
    }
    state.instances[instance].colliders = colliders;
    state.instances[instance].collidersUploaded = false;
    return true;
}

HairSimStatus HairRuntime::SimStatus()
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->DescribeSim();
}

// The fixed steps this frame takes: the frame's time goes into an accumulator and whole steps come out of it. A frame
// that would need more than maxStepsPerFrame takes that many and the rest of its time is dropped, so a long frame
// costs a bounded amount and the simulation runs slow instead.
std::uint32_t HairRuntime::State::PlanSimSteps(float deltaSeconds)
{
    const double step = sim.stepSeconds;
    simAccumulator += (std::max)(deltaSeconds, 0.0f);
    // The tolerance keeps a frame time that is one step, give or take rounding, at exactly one step a frame.
    auto steps = static_cast<std::uint64_t>((simAccumulator + step * 1.0e-3) / step);
    if (steps > sim.maxStepsPerFrame) {
        simStatus.droppedSeconds += simAccumulator - sim.maxStepsPerFrame * step;
        ++simStatus.cappedFrames;
        steps = sim.maxStepsPerFrame;
        simAccumulator = 0.0;
    } else {
        simAccumulator = (std::max)(simAccumulator - static_cast<double>(steps) * step, 0.0);
    }
    return static_cast<std::uint32_t>(steps);
}

// Simulation LOD: each drawn instance's simulation level for this frame, and whether it is simulated at all. An instance
// is simulated while a view shows part of it (OnScreenExtent), at the level its size on screen asks for: its coverage,
// or the part of that inside the view when less is, though never less than level 0 alone. Those levels are scaled down
// together when they pass the point budget, and an instance's level moves toward its own at a limited rate.
//
// An instance whose coverage has brought its level down to 0 stays simulated for as long as its guides take to blend
// back onto the rigid pose, then stops. One that no view shows stops as it is, once it has been out of view for
// lodViewGraceSeconds, and its guide rows keep its shape: when a view shows it again it goes on from that
// (HAIR_SIM_FLAG_KEEP_SHAPE), in the frame it enters. An instance with no such shape that would be simulated in view is
// started out of view too, at level 0 alone, and stops like any other once it has had lodSettleSeconds: it settles
// before a view can turn to it.
//
// A simulated instance holds guide rows, and this is where they are lent and taken back (HairGuideRowStats has the
// rules). An instance in view that cannot get any is not simulated (simStarved): it follows its roots rigidly.
void HairRuntime::State::PlanSimLevels(const HairFrameInputs& inputs, float simulatedSeconds)
{
    guideClock += simulatedSeconds;
    std::vector<Instance*> drawn;
    for (auto& instance : instances) {
        instance.simUnseen = false;
        instance.simPlanned = false;
        instance.simUnsettled = false;
        if (instance.info.state != HairInstanceState::Attached) continue;
        const auto entity = br::extensions::FindSceneEntityRenderState(instance.stableSceneID);
        if (!entity || entity->perObjectBufferIndex >= inputs.objects.rowCount) continue;
        const Asset& asset = assets[instance.info.asset];
        const BuiltGroom& groom = asset.groom;
        const auto levels = static_cast<float>(groom.GuideLevelCount());
        // Without the entity's matrices on the CPU neither its coverage nor where it is on screen is known: every guide
        // is simulated. So is a groom whose roots follow their own joints: its bounds on the CPU do not follow them.
        instance.simCoverage = 0.0f;
        instance.simViewExtent = kWholeView;
        instance.simInView = true;
        instance.simTarget = levels;
        ObjectMatrices matrices{};
        if (instance.info.mode != HairAttachMode::PerRoot && inputs.objects.image && inputs.objects.rowStride >= sizeof(ObjectMatrices) &&
            ReadImage(*inputs.objects.image, static_cast<std::size_t>(entity->perObjectBufferIndex) * inputs.objects.rowStride,
                &matrices, sizeof(matrices))) {
            const XMMATRIX model = XMLoadFloat4x4(&matrices.model);
            XMMATRIX toWorld = model;
            if (instance.info.mode == HairAttachMode::Bone && instance.skeleton && entity->skinningInstanceSlot != HAIR_INVALID_INDEX) {
                const auto pose = instance.skeleton->GetBoneMatrices();
                const auto inverseBind = instance.skeleton->GetInverseBindMatrices();
                if (instance.attachBone < (std::min)(pose.size(), inverseBind.size())) {
                    toWorld = XMMatrixMultiply(XMMatrixMultiply(inverseBind[instance.attachBone], pose[instance.attachBone]), model);
                }
            }
            const Float3 center = Store(XMVector3Transform(XMVectorSetW(Load((groom.boundsMin + groom.boundsMax) * 0.5f), 1.0f), toWorld));
            const float scale = XMVectorGetX(XMVector3Length(model.r[0]));
            const float radius = 0.5f * Length(groom.boundsMax - groom.boundsMin) * scale;
            instance.simCoverage = radius * inputs.mainView.projectionScaleY / (std::max)(Distance(center, inputs.cameraPosition), 1.0e-3f);
            instance.simViewExtent =
                OnScreenExtent(inputs, center, (asset.cullRadius * (1.0f + kSimViewRadiusMargin) + HAIR_CULL_WIDTH_MARGIN) * scale);
            instance.simInView = instance.simViewExtent > 0.0f;
            const float byCoverage = SimLevelForCoverage(sim, instance.simCoverage, levels);
            if (!instance.simInView) {
                instance.simTarget = (std::min)(byCoverage, 1.0f); // what it settles at, unseen
            } else if (byCoverage >= 1.0f) {
                instance.simTarget =
                    (std::max)(SimLevelForCoverage(sim, (std::min)(instance.simCoverage, instance.simViewExtent), levels), 1.0f);
            } else {
                instance.simTarget = byCoverage;
            }
        }
        const float force = instance.simForceLevel >= 0.0f ? instance.simForceLevel : sim.lodForceLevel;
        if (force >= 0.0f) {
            // A forced level is for testing: it holds wherever the instance is.
            instance.simTarget = (std::min)(force, levels);
            instance.simInView = true;
        }
        instance.simPlanned = true;
        drawn.push_back(&instance);
    }
    for (auto& instance : instances) instance.simStarved = instance.simStarved && instance.simPlanned;

    if (sim.lodPointBudget != 0u) {
        const auto points = [&](float scale) {
            std::uint64_t sum = 0u;
            for (const Instance* instance : drawn) sum += assets[instance->info.asset].SimulatedPoints(instance->simTarget * scale);
            return sum;
        };
        if (points(1.0f) > sim.lodPointBudget) {
            float fits = 0.0f;
            float over = 1.0f;
            for (int step = 0; step < 24; ++step) {
                const float middle = 0.5f * (fits + over);
                (points(middle) > sim.lodPointBudget ? over : fits) = middle;
            }
            // Never below level 0 alone: the budget does not turn an instance off.
            for (Instance* instance : drawn) instance->simTarget = (std::max)(instance->simTarget * fits, (std::min)(instance->simTarget, 1.0f));
        }
    }

    // A budget that fell below what is lent out: kept shapes go, then instances simulated out of view, then the
    // smallest instances in view, which then follow rigidly.
    while (guideRows.used > GuideRowBudget()) {
        Instance* victim = PickGuideRowVictim(true);
        if (!victim) break;
        EvictGuideRows(*victim, false);
    }

    // Rows go first to the instances a view shows, the largest on screen first.
    std::ranges::stable_sort(drawn, [](const Instance* a, const Instance* b) {
        return a->simInView != b->simInView ? a->simInView : a->simCoverage > b->simCoverage;
    });
    for (Instance* instance : drawn) {
        const bool forced = instance->simForceLevel >= 0.0f || sim.lodForceLevel >= 0.0f;
        instance->simOutOfViewSeconds = instance->simInView ? 0.0 : instance->simOutOfViewSeconds + simulatedSeconds;
        const bool wasStarved = std::exchange(instance->simStarved, false);
        if (!instance->simActive) {
            if (instance->simTarget <= 0.0f || (!forced && instance->simTarget < kSimLevelStart)) {
                // Too small on screen to be simulated, in view or out of it: a kept shape has no use.
                ReleaseGuideRows(*instance);
                instance->simEvicted = false;
                continue;
            }
            if (!instance->simInView) {
                // Out of view it starts only to settle, when its rows hold no shape to go on from; and not again once
                // that was taken from it. It takes free rows only.
                if (instance->simStateValid || !(sim.lodSettleSeconds > 0.0f) || instance->simEvicted) {
                    instance->simUnseen = true;
                    continue;
                }
                if (!AcquireGuideRows(*instance, false)) {
                    instance->simUnseen = instance->simUnsettled = true;
                    continue;
                }
            } else if (!AcquireGuideRows(*instance, true)) {
                // No rows to be had: it follows its roots rigidly, and asks again every frame.
                instance->simStarved = true;
                guideStats.allocationFailures += wasStarved ? 0u : 1u;
                continue;
            }
            instance->simEvicted = false;
            instance->simActive = true;
            instance->simMoving = false;
            instance->simOffSeconds = 0.0;
            if (instance->simStateValid) {
                // It goes on from the shape it was left in, with the guides that were simulated then: at the level it
                // had, from which the level moves like any other.
                instance->simLevel = (std::min)(instance->simTarget, instance->simLevel);
            } else {
                // It starts with every guide on its rigid pose, which is where its strands are: at its level at once.
                instance->simLevel = instance->simTarget;
                instance->simSettledSeconds = 0.0;
            }
        } else if (!instance->simInView && instance->simOutOfViewSeconds >= sim.lodViewGraceSeconds &&
            instance->simSettledSeconds >= sim.lodSettleSeconds) {
            // No view needs it: it stops as it is. A level of 0 was on its way to the rigid pose, and starts from there.
            instance->simActive = false;
            instance->simStateValid = instance->simLevel > 0.0f;
            instance->simUnseen = instance->simTarget >= kSimLevelStart;
            if (!instance->simStateValid) ReleaseGuideRows(*instance);
            continue;
        } else if (forced) {
            instance->simLevel = instance->simTarget;
        } else {
            const float difference = instance->simTarget - instance->simLevel;
            instance->simMoving = difference != 0.0f &&
                (instance->simMoving || instance->simTarget <= 0.0f || std::abs(difference) > kSimLevelDeadband);
            if (instance->simMoving) {
                const float most = sim.lodRate * simulatedSeconds;
                instance->simLevel = (std::max)(instance->simLevel + std::clamp(difference, -most, most), 0.0f);
            }
        }
        instance->simSettledSeconds += simulatedSeconds;
        if (instance->simLevel > 0.0f) {
            instance->simOffSeconds = 0.0;
            continue;
        }
        instance->simOffSeconds += simulatedSeconds;
        if (instance->simOffSeconds >= sim.demoteSeconds + 2.0 * sim.stepSeconds) {
            // Its guides are on the rigid pose: there is no shape to keep.
            instance->simActive = false;
            ReleaseGuideRows(*instance);
        }
    }

    // Kept shapes: one that was reset away goes back at once, and one older than guideKeepSeconds when that is set.
    for (auto& instance : instances) {
        if (instance.guideRowBase == HAIR_INVALID_INDEX || (instance.simActive && instance.simPlanned)) continue;
        if (!instance.simActive && !instance.simStateValid) {
            ReleaseGuideRows(instance);
        } else if (sim.guideKeepSeconds > 0.0f && guideClock - instance.simLastSimulated >= sim.guideKeepSeconds) {
            EvictGuideRows(instance, true);
        }
    }
}

std::shared_ptr<const HairGpuBuffers> HairRuntime::GpuBuffers() const
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->buffers;
}

HairFramePlan HairRuntime::FramePlan() const
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->plan;
}

void HairRuntime::SetLodSettings(const HairLodSettings& settings)
{
    HairLodSettings clamped = settings;
    clamped.errorPixels = (std::max)(settings.errorPixels, 1.0e-3f);
    clamped.maxWidthScale = (std::max)(settings.maxWidthScale, 1.0f);
    clamped.minDetail = std::clamp(settings.minDetail, 1.0e-4f, 1.0f);
    clamped.forceDetail = std::clamp(settings.forceDetail, 0.0f, 1.0f);
    clamped.segmentPixels = (std::max)(settings.segmentPixels, 0.25f);
    clamped.detailExponent = std::clamp(settings.detailExponent, 1.0f, 3.0f);
    std::scoped_lock lock(m_state->mutex);
    m_state->lod = clamped;
}

HairLodSettings HairRuntime::LodSettings() const
{
    std::scoped_lock lock(m_state->mutex);
    return m_state->lod;
}

void HairRuntime::SetDebugView(HairDebugView view)
{
    std::scoped_lock lock(m_state->mutex);
    m_state->debugView = view;
}

bool HairRuntime::CaptureDebugView(const std::string& path, std::string& error)
{
    std::scoped_lock lock(m_state->mutex);
    if (m_state->debugCapture.state == HairDebugCapture::State::Pending) {
        error = "a capture to '" + m_state->debugCapture.path + "' is still pending";
        return false;
    }
    m_state->debugCapture = HairDebugCapture{};
    m_state->debugCapture.state = HairDebugCapture::State::Pending;
    m_state->debugCapture.path = path;
    m_state->debugCaptureRequested = true;
    return true;
}

bool HairRuntime::TakeDebugCaptureRequest(std::string& path)
{
    std::scoped_lock lock(m_state->mutex);
    if (!std::exchange(m_state->debugCaptureRequested, false)) return false;
    path = m_state->debugCapture.path;
    return true;
}

void HairRuntime::ReadDebugCapture(const std::string& path, org::Resource* presentationColor)
{
    const auto finish = [state = std::weak_ptr<State>(m_state), path](HairDebugCapture capture) {
        capture.path = path;
        capture.state = capture.error.empty() ? HairDebugCapture::State::Written : HairDebugCapture::State::Failed;
        if (capture.error.empty()) {
            spdlog::info("Hair: wrote the presented frame, {}x{}, to '{}'", capture.width, capture.height, path);
        } else {
            spdlog::error("Hair: debug view capture: {}", capture.error);
        }
        if (const auto locked = state.lock()) {
            std::scoped_lock lock(locked->mutex);
            locked->debugCapture = std::move(capture);
        }
    };
    auto* readbacks = br::GetReadbackCaptureService();
    if (!readbacks || !presentationColor) {
        HairDebugCapture capture;
        capture.error = readbacks ? "the frame has no presentation colour texture" : "the renderer has no readback service";
        finish(std::move(capture));
        return;
    }
    // After the graph: the texture as the frame's last pass left it.
    readbacks->RequestReadbackCaptureAfterGraph(presentationColor, {}, [finish, path](br::ReadbackCaptureResult&& result) {
        HairDebugCapture capture;
        capture.width = result.width;
        capture.height = result.height;
        const bool rgba = result.format == rhi::Format::R8G8B8A8_UNorm || result.format == rhi::Format::R8G8B8A8_UNorm_sRGB;
        const bool bgra = result.format == rhi::Format::B8G8R8A8_UNorm || result.format == rhi::Format::B8G8R8A8_UNorm_sRGB;
        if (result.layouts.empty() || result.width == 0u || result.height == 0u || (!rgba && !bgra)) {
            capture.error = "the presentation colour came back empty or in a format that is not 8-bit RGBA";
        } else {
            const auto& layout = result.layouts.front();
            const std::size_t needed = layout.offset + static_cast<std::size_t>(layout.rowPitch) * (result.height - 1u) +
                static_cast<std::size_t>(result.width) * 4u;
            if (result.data.size() < needed) {
                capture.error = "the readback is shorter than the image";
            } else if (!WriteBmp(path, result.data.data() + layout.offset, layout.rowPitch, result.width, result.height, rgba)) {
                capture.error = "cannot write '" + path + "'";
            }
        }
        finish(std::move(capture));
    });
}

bool HairRuntime::CaptureProbe(const std::string& path, std::string& error)
{
    std::scoped_lock lock(m_state->mutex);
    if (m_state->probe.state == HairDebugCapture::State::Pending) {
        error = "a probe to '" + m_state->probe.path + "' is still pending";
        return false;
    }
    m_state->probe = HairDebugCapture{};
    m_state->probe.state = HairDebugCapture::State::Pending;
    m_state->probe.path = path;
    m_state->probeRequested = true;
    return true;
}

org::Buffer* HairRuntime::State::DumpTarget(const std::string& name, std::size_t& stride) const
{
    if (name == "pool_vertices") {
        stride = sizeof(HairPoolVertex);
        return buffers ? buffers->poolVertices.get() : nullptr;
    }
    if (name == "visible_clusters") {
        stride = sizeof(HairVisibleCluster);
        return buffers ? buffers->visibleClusters.get() : nullptr;
    }
    if (name == "guide_points") {
        stride = sizeof(HairGuidePoint);
        return buffers ? buffers->guidePoints.get() : nullptr;
    }
    // The groom data and the instance rows, as uploaded: for working out from a frame's selection what it used.
    if (name == "instances") {
        stride = sizeof(HairInstanceGPU);
        return buffers ? buffers->instances.get() : nullptr;
    }
    if (name == "clusters") {
        stride = sizeof(HairClusterHeader);
        return buffers ? buffers->clusters.get() : nullptr;
    }
    if (name == "strands") {
        stride = sizeof(HairStrandRecord);
        return buffers ? buffers->strands.get() : nullptr;
    }
    if (name == "guide_bindings") {
        stride = sizeof(HairGuideBinding);
        return buffers ? buffers->guideBindings.get() : nullptr;
    }
    stride = 0u;
    return nullptr;
}

bool HairRuntime::DumpBuffer(const std::string& buffer, const std::string& path, std::uint32_t rows, std::string& error)
{
    std::scoped_lock lock(m_state->mutex);
    std::size_t stride = 0u;
    m_state->DumpTarget(buffer, stride);
    if (stride == 0u) {
        error = "unknown buffer '" + buffer +
            "': expected pool_vertices, visible_clusters, guide_points, instances, clusters, strands or guide_bindings";
        return false;
    }
    m_state->dumpRequests.push_back({ buffer, path, rows });
    return true;
}

// Reads every requested buffer back after this frame and writes each to its file.
void HairRuntime::State::RequestDumps(const std::shared_ptr<State>& self, std::uint64_t frameNumber)
{
    auto* readbacks = br::GetReadbackCaptureService();
    for (auto& request : std::exchange(dumpRequests, {})) {
        std::size_t stride = 0u;
        org::Buffer* buffer = DumpTarget(request.buffer, stride);
        if (!readbacks || !buffer) {
            ++dumpsFailed;
            continue;
        }
        ++dumpsInFlight;
        readbacks->RequestReadbackCaptureAfterGraph(buffer, {},
            [state = std::weak_ptr<State>(self), request, stride, frameNumber](br::ReadbackCaptureResult&& result) {
                std::size_t bytes = result.data.size();
                if (request.rows != 0u) bytes = (std::min)(bytes, static_cast<std::size_t>(request.rows) * stride);
                std::ofstream file(request.path, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(&frameNumber), sizeof(frameNumber));
                file.write(reinterpret_cast<const char*>(result.data.data()), static_cast<std::streamsize>(bytes));
                const bool written = bytes != 0u && static_cast<bool>(file);
                if (written) {
                    spdlog::info("Hair: wrote {} bytes of {} after frame {} to '{}'", bytes, request.buffer, frameNumber, request.path);
                } else {
                    spdlog::error("Hair: cannot write {} to '{}'", request.buffer, request.path);
                }
                if (const auto locked = state.lock()) {
                    std::scoped_lock lock(locked->mutex);
                    --locked->dumpsInFlight;
                    ++(written ? locked->dumpsWritten : locked->dumpsFailed);
                }
            });
    }
}

void HairRuntime::SetContinuousCounters(bool enabled)
{
    std::scoped_lock lock(m_state->mutex);
    m_state->continuousCounters = enabled;
    if (!enabled) m_state->counterHistory.clear();
}

// Scene thread, inside Renderer::Update. Finds each pending instance's entity by name.
void HairRuntime::State::ResolvePendingInstances(Scene& scene)
{
    for (auto& instance : instances) {
        if (instance.info.state != HairInstanceState::Pending) continue;
        const auto fail = [&](std::string message) {
            instance.info.state = HairInstanceState::Failed;
            instance.info.error = std::move(message);
            spdlog::error("Hair: instance {}: {}", instance.info.id, instance.info.error);
        };
        flecs::world world = scene.GetRoot().world();
        flecs::entity named;
        world.each([&](flecs::entity entity, const Components::Name& name) {
            if (!named.is_valid() && name.name == instance.info.entity) named = entity;
        });
        if (!named.is_valid()) {
            fail("no scene entity named '" + instance.info.entity + "'");
            continue;
        }
        flecs::entity target = named.has<Components::MeshInstances>() ? named : flecs::entity();
        if (!target.is_valid()) {
            named.children([&](flecs::entity child) {
                if (!target.is_valid() && child.has<Components::MeshInstances>()) target = child;
            });
        }
        const auto* stableID = target.is_valid() ? target.try_get<Components::StableSceneID>() : nullptr;
        if (!stableID) {
            fail("entity '" + instance.info.entity + "' has no renderable part");
            continue;
        }
        std::shared_ptr<Skeleton> skeleton;
        for (const auto& meshInstance : target.get<Components::MeshInstances>().meshInstances) {
            if (meshInstance && meshInstance->HasSkin()) {
                skeleton = meshInstance->GetSkin();
                break;
            }
        }
        instance.info.mode = HairAttachMode::Rigid;
        if (!instance.info.bone.empty()) {
            if (!skeleton) {
                fail("entity '" + instance.info.entity + "' has no skeleton to find bone '" + instance.info.bone + "' in");
                continue;
            }
            // The glTF importer names a bone "<node name>#node_<index>"; the node name alone is accepted.
            const auto names = skeleton->GetBoneNames();
            const std::string prefix = instance.info.bone + "#";
            const auto found = std::ranges::find_if(names, [&](const std::string& name) {
                return name == instance.info.bone || name.starts_with(prefix);
            });
            if (found == names.end()) {
                std::string known;
                for (std::size_t i = 0u; i < names.size() && i < 8u; ++i) known += (i ? ", '" : "'") + names[i] + "'";
                fail("the skeleton of '" + instance.info.entity + "' has no bone named '" + instance.info.bone + "' (its " +
                    std::to_string(names.size()) + " bones start with " + known + ")");
                continue;
            }
            instance.attachBone = static_cast<std::uint32_t>(found - names.begin());
            instance.info.mode = HairAttachMode::Bone;
        } else if (skeleton && assets[instance.info.asset].hasSkinBindings) {
            instance.info.mode = HairAttachMode::PerRoot;
        }
        if (instance.info.mode != HairAttachMode::Rigid) instance.skeleton = skeleton;
        instance.stableSceneID = stableID->value;
        instance.info.state = HairInstanceState::Attached;
        spdlog::info("Hair: instance {} attached groom {} to '{}' ({})", instance.info.id, instance.info.asset,
            instance.info.entity, ModeName(instance.info.mode));
    }
}

void HairRuntime::State::CreateBuffers()
{
    auto created = std::make_shared<HairGpuBuffers>();
    std::uint64_t bytes = 0u;
    const auto make = [&](BufferId id, std::uint32_t rows, std::size_t stride, bool unorderedAccess, const char* name) {
        bytes += static_cast<std::uint64_t>(rows) * stride;
        bufferUse[id] = HairBufferUse{ name, static_cast<std::uint32_t>(stride), rows, 0u, 0u };
        return MakeBuffer(rows, stride, unorderedAccess, name);
    };
    created->clusters = make(kBufferClusters, capacities.clusters, sizeof(HairClusterHeader), false, "Hair.Clusters");
    created->aggregates = make(kBufferAggregates, capacities.clusters, sizeof(HairClusterAggregate), false, "Hair.ClusterAggregates");
    created->strands = make(kBufferStrands, capacities.strands, sizeof(HairStrandRecord), false, "Hair.Strands");
    created->rootBindings = make(kBufferRootBindings, capacities.strands, sizeof(HairRootBinding), false, "Hair.RootBindings");
    created->guideBindings = make(kBufferGuideBindings, capacities.guides, sizeof(HairGuideBinding), false, "Hair.GuideBindings");
    created->guideStrandIndices =
        make(kBufferGuideStrandIndices, capacities.guides, sizeof(std::uint32_t), false, "Hair.GuideStrandIndices");
    created->points = make(kBufferPoints, capacities.points, sizeof(HairPackedPoint), false, "Hair.Points");
    created->instances = make(kBufferInstances, capacities.instances, sizeof(HairInstanceGPU), false, "Hair.Instances");
    created->frameState = make(kBufferFrameState, 1u, sizeof(HairFrameGPU), false, "Hair.FrameState");
    created->rootCheckList = make(kBufferRootCheckList, HAIR_ROOT_CHECK_CAPACITY, sizeof(HairRootCheckSample), false, "Hair.RootCheckList");
    created->rootCheck = make(kBufferRootCheck, HAIR_ROOT_CHECK_CAPACITY, sizeof(HairRootFramePair), true, "Hair.RootCheck");
    created->counters = make(kBufferCounters, HAIR_COUNTER_COUNT, sizeof(std::uint32_t), true, "Hair.Counters");
    created->visibleClusters =
        make(kBufferVisibleClusters, capacities.visibleClusters, sizeof(HairVisibleCluster), true, "Hair.VisibleClusters");
    created->poolVertices = make(kBufferPoolVertices, capacities.poolVertices, sizeof(HairPoolVertex), true, "Hair.PoolVertices");
    created->poolSegmentIds = make(kBufferPoolSegmentIds, capacities.poolVertices, sizeof(std::uint32_t), true, "Hair.PoolSegmentIds");
    created->probe = make(kBufferProbe, HAIR_PROBE_HEADER_WORDS + HAIR_PROBE_ROW_CAPACITY * HAIR_PROBE_ROW_WORDS,
        sizeof(std::uint32_t), true, "Hair.Probe");
    created->guidePointRanges = make(kBufferGuidePointRanges, capacities.guides, sizeof(HairGuidePointRange), false, "Hair.GuidePointRanges");
    created->guidePoints = make(kBufferGuidePoints, capacities.guidePoints, sizeof(HairGuidePoint), true, "Hair.GuidePoints");
    created->colliders = make(kBufferColliders, capacities.instances * HAIR_MAX_COLLIDERS_PER_INSTANCE, sizeof(HairColliderGPU), false,
        "Hair.Colliders");
    created->simFrame = make(kBufferSimFrame, 1u, sizeof(HairSimFrameGPU), false, "Hair.SimFrame");
    created->simMetrics =
        make(kBufferSimMetrics, capacities.instances * HAIR_SIM_METRIC_COUNT, sizeof(std::uint32_t), true, "Hair.SimMetrics");
    created->simGroups = make(kBufferSimGroups, capacities.simGroups, sizeof(HairSimGroup), false, "Hair.SimGroups");
    buffers = std::move(created);
    bufferBytes = bytes;
    spdlog::info("Hair: created the pooled GPU buffers, {:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// The hair visibility target is as large as the opaque visibility buffer it is tested against. At a new size the whole
// buffer set is replaced by a copy holding the new target, so the passes declare again.
void HairRuntime::State::EnsureVisibilityTarget(std::uint32_t width, std::uint32_t height)
{
    if (width == 0u || height == 0u || (buffers->visibility && buffers->visibilityWidth == width && buffers->visibilityHeight == height)) {
        return;
    }
    org::TextureDescription description;
    description.channels = 2;
    description.format = rhi::Format::R32G32_UInt; // read and written as one uint64_t, as the opaque visibility buffer
    description.hasSRV = description.hasUAV = description.hasNonShaderVisibleUAV = true;
    description.imageDimensions.emplace_back(width, height, 0, 0);
    auto replaced = std::make_shared<HairGpuBuffers>(*buffers);
    replaced->visibility = org::PixelBuffer::CreateShared(description);
    replaced->visibility->SetName("Hair.Visibility");
    description.channels = 1;
    description.format = rhi::Format::R32_UInt;
    replaced->coverage = org::PixelBuffer::CreateShared(description);
    replaced->coverage->SetName("Hair.Coverage");
    replaced->visibilityWidth = width;
    replaced->visibilityHeight = height;
    buffers = std::move(replaced);
    spdlog::info("Hair: made the hair visibility target, {}x{}", width, height);
}

void HairRuntime::State::UploadAsset(Asset& asset, const HairUpload& upload)
{
    const BuiltGroom& groom = asset.groom;
    std::uint64_t bytes = 0u;
    const auto rows = [&](const std::shared_ptr<org::Buffer>& buffer, std::uint32_t firstRow, const auto& array) {
        if (array.empty()) return;
        UploadRows(upload, buffer, firstRow, array.data(), array.size(), sizeof(array[0]));
        bytes += array.size() * sizeof(array[0]);
    };
    rows(buffers->clusters, asset.clusterBase, groom.clusters);
    rows(buffers->aggregates, asset.clusterBase, groom.aggregates);
    rows(buffers->strands, asset.strandBase, groom.strands);
    rows(buffers->rootBindings, asset.strandBase, groom.rootBindings);
    rows(buffers->guideBindings, asset.guideBase, groom.guideBindings);
    rows(buffers->guideStrandIndices, asset.guideBase, groom.guideStrandIndices);
    rows(buffers->guidePointRanges, asset.guideBase, asset.guidePointRanges);
    rows(buffers->points, asset.pointBase, groom.points);
    asset.uploaded = true;
    assetBytesUploaded += bytes;
    spdlog::info("Hair: uploaded groom '{}', {} bytes", asset.source, bytes);
}

void HairRuntime::State::RequestCounterReadback(const std::shared_ptr<State>& self, const HairFrameInputs& inputs, bool selected)
{
    auto* readbacks = br::GetReadbackCaptureService();
    if (!readbacks) return;
    counterReadbackWanted = false;
    counterReadbackInFlight = true;
    HairGpuCounters request;
    request.frame = inputs.frameNumber;
    request.cpuEvaluateDispatches = evaluateDispatches;
    request.cameraPosition = inputs.cameraPosition;
    request.selected = selected;
    readbacks->RequestReadbackCaptureAfterGraph(buffers->counters.get(), {},
        [state = std::weak_ptr<State>(self), request, visibleCapacity = capacities.visibleClusters,
            poolCapacity = capacities.poolVertices](br::ReadbackCaptureResult&& result) {
            const auto locked = state.lock();
            if (!locked) return;
            std::scoped_lock lock(locked->mutex);
            locked->counterReadbackInFlight = false;
            std::uint32_t rows[HAIR_COUNTER_COUNT];
            if (result.data.size() < sizeof(rows)) return;
            std::memcpy(rows, result.data.data(), sizeof(rows));
            HairGpuCounters counters = request;
            counters.valid = true;
            counters.evaluateDispatches = rows[HAIR_COUNTER_EVALUATE_DISPATCHES];
            counters.staleGuidesDrawn = rows[HAIR_COUNTER_STALE_GUIDES_DRAWN];
            counters.unseenStrandsDrawn = rows[HAIR_COUNTER_UNSEEN_STRANDS_DRAWN];
            counters.clustersTested = rows[HAIR_COUNTER_CLUSTERS_TESTED];
            // The two allocators run past their capacities when clusters are dropped.
            counters.clustersVisible = (std::min)(rows[HAIR_COUNTER_CLUSTERS_VISIBLE], visibleCapacity);
            for (std::uint32_t word = 0u; word < HAIR_COUNTER_INSTANCE_MASK_WORDS; ++word) {
                counters.instancesVisible += static_cast<std::uint32_t>(std::popcount(rows[HAIR_COUNTER_INSTANCE_MASK_FIRST + word]));
            }
            counters.strandsSelected = rows[HAIR_COUNTER_STRANDS_SELECTED];
            counters.segmentsSelected = rows[HAIR_COUNTER_SEGMENTS_SELECTED];
            counters.poolVertices = (std::min)(rows[HAIR_COUNTER_POOL_VERTICES], poolCapacity);
            counters.overflowClusters = rows[HAIR_COUNTER_OVERFLOW_CLUSTERS];
            counters.pixelsCovered = rows[HAIR_COUNTER_PIXELS_COVERED];
            counters.coverageSum = static_cast<float>(rows[HAIR_COUNTER_COVERAGE_SUM]) / HAIR_COVERAGE_COUNTER_UNIT;
            counters.segmentsDrawn = rows[HAIR_COUNTER_SEGMENTS_DRAWN];
            counters.pixelsWritten = rows[HAIR_COUNTER_PIXELS_WRITTEN];
            counters.pixelsOccluded = rows[HAIR_COUNTER_PIXELS_OCCLUDED];
            counters.segmentsCapped = rows[HAIR_COUNTER_SEGMENTS_CAPPED];
            counters.pixelsResolved = rows[HAIR_COUNTER_PIXELS_RESOLVED];
            locked->Use(kBufferVisibleClusters, counters.clustersVisible);
            locked->Use(kBufferPoolVertices, counters.poolVertices);
            locked->Use(kBufferPoolSegmentIds, counters.poolVertices);
            // Readbacks of different frames can be in flight together: keep the newest, and the history in frame order.
            if (!locked->gpuCounters.valid || counters.frame >= locked->gpuCounters.frame) locked->gpuCounters = counters;
            if (!locked->continuousCounters) return;
            auto& history = locked->counterHistory;
            history.insert(std::ranges::upper_bound(history, counters.frame, {}, &HairGpuCounters::frame), counters);
            while (history.size() > kHairCounterHistoryFrames) history.pop_front();
        });
}

// Reads the simulation's metrics back after the frame that wrote them.
void HairRuntime::State::RequestSimMetricsReadback(const std::shared_ptr<State>& self, std::uint64_t frameNumber)
{
    auto* readbacks = br::GetReadbackCaptureService();
    if (!readbacks) return;
    simReadbackWanted = false;
    simReadbackInFlight = true;
    // What the frame's readback cannot tell: each instance's colliders and its simulation level in this frame.
    std::vector<std::pair<std::uint32_t, HairSimMetrics>> measured;
    for (const auto& dispatch : plan.instances) {
        const Instance& instance = instances[dispatch.instanceIndex];
        HairSimMetrics known;
        known.hasColliders = !instance.colliders.empty();
        known.simLevel = dispatch.simulated != 0u ? instance.simLevel : 0.0f;
        known.targetLevel = instance.simTarget;
        known.coverage = instance.simCoverage;
        known.viewExtent = instance.simViewExtent;
        known.inView = instance.simViewExtent > 0.0f;
        known.holdsRows = instance.guideRowBase != HAIR_INVALID_INDEX;
        known.starved = instance.simStarved;
        known.levelCount = dispatch.guideLevelCount;
        measured.emplace_back(dispatch.instanceIndex, known);
    }
    readbacks->RequestReadbackCaptureAfterGraph(buffers->simMetrics.get(), {},
        [state = std::weak_ptr<State>(self), frameNumber, requestGeneration = generation, steps = simStatus.steps,
            measured = std::move(measured)](br::ReadbackCaptureResult&& result) {
            const auto locked = state.lock();
            if (!locked) return;
            std::scoped_lock lock(locked->mutex);
            locked->simReadbackInFlight = false;
            if (requestGeneration != locked->generation) return;
            const auto asFloat = [](std::uint32_t bits) {
                float value;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            };
            for (const auto& [index, known] : measured) {
                std::uint32_t rows[HAIR_SIM_METRIC_COUNT];
                const std::size_t offset = static_cast<std::size_t>(index) * sizeof(rows);
                if (index >= locked->instances.size() || offset + sizeof(rows) > result.data.size()) continue;
                std::memcpy(rows, result.data.data() + offset, sizeof(rows));
                HairSimMetrics metrics = known;
                const bool hasColliders = known.hasColliders;
                metrics.valid = true;
                metrics.instance = index;
                metrics.frame = frameNumber;
                metrics.steps = steps;
                metrics.guides = rows[HAIR_SIM_METRIC_GUIDES];
                const float guides = static_cast<float>((std::max)(metrics.guides, 1u));
                metrics.stretchMax = asFloat(rows[HAIR_SIM_METRIC_STRETCH_MAX]);
                metrics.stretchMean = static_cast<float>(rows[HAIR_SIM_METRIC_STRETCH_SUM]) * HAIR_SIM_METRIC_STRETCH_UNIT / guides;
                metrics.stretchedGuides = rows[HAIR_SIM_METRIC_STRETCHED_GUIDES];
                metrics.speedMax = asFloat(rows[HAIR_SIM_METRIC_SPEED_MAX]);
                metrics.speedMean = static_cast<float>(rows[HAIR_SIM_METRIC_SPEED_SUM]) * HAIR_SIM_METRIC_SPEED_UNIT / guides;
                metrics.deviationMean = static_cast<float>(rows[HAIR_SIM_METRIC_DEVIATION_SUM]) * HAIR_SIM_METRIC_DEVIATION_UNIT / guides;
                metrics.hasColliders = hasColliders && rows[HAIR_SIM_METRIC_COLLIDER_DISTANCE] != 0xFFFFFFFFu;
                if (metrics.hasColliders) {
                    metrics.colliderDistance = static_cast<float>(static_cast<std::int32_t>(
                        rows[HAIR_SIM_METRIC_COLLIDER_DISTANCE] - HAIR_SIM_METRIC_DISTANCE_BIAS)) * HAIR_SIM_METRIC_DISTANCE_UNIT;
                }
                metrics.guideRoots = rows[HAIR_SIM_METRIC_GUIDE_ROOTS];
                metrics.guideRootError = asFloat(rows[HAIR_SIM_METRIC_GUIDE_ROOT_ERROR]);
                metrics.strandRoots = rows[HAIR_SIM_METRIC_STRAND_ROOTS];
                metrics.strandRootError = asFloat(rows[HAIR_SIM_METRIC_STRAND_ROOT_ERROR]);
                metrics.resetGuides = rows[HAIR_SIM_METRIC_RESET_GUIDES];
                metrics.resumedGuides = rows[HAIR_SIM_METRIC_RESUMED_GUIDES];
                metrics.staleGuides = rows[HAIR_SIM_METRIC_STALE_GUIDES];
                metrics.rootShift = asFloat(rows[HAIR_SIM_METRIC_ROOT_SHIFT]);
                for (std::uint32_t level = 0u; level < HAIR_MAX_GUIDE_LEVELS; ++level) {
                    metrics.levelGuides[level] = rows[HAIR_SIM_METRIC_LEVEL_GUIDES + level];
                }
                metrics.simulatedPoints = rows[HAIR_SIM_METRIC_SIMULATED_POINTS];
                metrics.evaluatedGuides = rows[HAIR_SIM_METRIC_EVALUATED_GUIDES];
                metrics.blendingGuides = rows[HAIR_SIM_METRIC_BLENDING_GUIDES];
                metrics.promotedGuides = rows[HAIR_SIM_METRIC_PROMOTED_GUIDES];
                metrics.demotedGuides = rows[HAIR_SIM_METRIC_DEMOTED_GUIDES];
                metrics.transitionMotion = asFloat(rows[HAIR_SIM_METRIC_TRANSITION_MOTION]);
                metrics.steadyMotion = asFloat(rows[HAIR_SIM_METRIC_STEADY_MOTION]);
                auto& kept = locked->instances[index].simMetrics;
                if (!kept.valid || metrics.frame >= kept.frame) kept = metrics;
            }
        });
}

// Reads the probe buffer back after the frame whose probe pass fills it, and writes its header and rows to the file.
void HairRuntime::State::RequestProbe(const std::shared_ptr<State>& self)
{
    const auto finish = [state = std::weak_ptr<State>(self), path = probe.path](std::string error, std::uint32_t rows) {
        if (error.empty()) {
            spdlog::info("Hair: wrote {} probe rows to '{}'", rows, path);
        } else {
            spdlog::error("Hair: probe: {}", error);
        }
        if (const auto locked = state.lock()) {
            std::scoped_lock lock(locked->mutex);
            locked->probe.state = error.empty() ? HairDebugCapture::State::Written : HairDebugCapture::State::Failed;
            locked->probe.error = std::move(error);
            locked->probe.width = rows;
        }
    };
    auto* readbacks = br::GetReadbackCaptureService();
    if (!readbacks) {
        finish("the renderer has no readback service", 0u);
        return;
    }
    readbacks->RequestReadbackCaptureAfterGraph(buffers->probe.get(), {},
        [finish, state = std::weak_ptr<State>(self), path = probe.path](br::ReadbackCaptureResult&& result) {
        if (result.data.size() < HAIR_PROBE_HEADER_WORDS * sizeof(std::uint32_t)) {
            finish("the readback is shorter than the probe header", 0u);
            return;
        }
        std::uint32_t found = 0u;
        std::memcpy(&found, result.data.data(), sizeof(found));
        const std::uint32_t rows = (std::min)(found, HAIR_PROBE_ROW_CAPACITY);
        if (const auto locked = state.lock()) {
            std::scoped_lock lock(locked->mutex);
            locked->Use(kBufferProbe, HAIR_PROBE_HEADER_WORDS + static_cast<std::uint64_t>(rows) * HAIR_PROBE_ROW_WORDS);
        }
        const std::size_t bytes = (HAIR_PROBE_HEADER_WORDS + static_cast<std::size_t>(rows) * HAIR_PROBE_ROW_WORDS) * sizeof(std::uint32_t);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (bytes <= result.data.size()) file.write(reinterpret_cast<const char*>(result.data.data()), static_cast<std::streamsize>(bytes));
        finish(bytes <= result.data.size() && file ? std::string() : "cannot write '" + path + "'", rows);
    });
}

void HairRuntime::State::RequestSelfCheck(const std::shared_ptr<State>& self, std::uint64_t frameNumber,
    std::vector<ExpectedRoot> expected)
{
    auto* readbacks = br::GetReadbackCaptureService();
    if (!readbacks || expected.empty()) return;
    selfCheckInFlight = true;
    selfCheckFrame = frameNumber;
    readbacks->RequestReadbackCaptureAfterGraph(buffers->rootCheck.get(), {},
        [state = std::weak_ptr<State>(self), frameNumber, requestGeneration = generation,
            expected = std::move(expected)](br::ReadbackCaptureResult&& result) {
            const auto locked = state.lock();
            if (!locked) return;
            std::scoped_lock lock(locked->mutex);
            locked->selfCheckInFlight = false;
            auto& status = locked->selfCheck;
            if (!status.enabled || requestGeneration != locked->generation) return;

            status.frame = frameNumber;
            status.instances = status.skinnedInstances = status.staleInstances = status.roots = status.previousRoots = 0u;
            status.positionError = status.directionError = status.previousPositionError = status.previousDirectionError = 0.0f;
            status.motion = status.skinnedMotion = 0.0f;
            // Row i of the check buffer is root i of the list the frame's root-check pass was given. A row it did not
            // write in the checked frame says nothing; an instance with no row written was not compared.
            std::uint32_t lastInstance = HAIR_INVALID_INDEX;
            bool compared = true;
            for (const auto& root : expected) {
                const std::size_t offset = static_cast<std::size_t>(root.row) * sizeof(HairRootFramePair);
                if (offset + sizeof(HairRootFramePair) > result.data.size()) continue;
                HairRootFramePair pair;
                std::memcpy(&pair, result.data.data() + offset, sizeof(pair));
                if (root.instance != lastInstance) {
                    status.staleInstances += compared ? 0u : 1u;
                    lastInstance = root.instance;
                    compared = false;
                }
                if (root.unknown || pair.current.writtenFrame != static_cast<std::uint32_t>(frameNumber)) continue;
                if (!compared) {
                    compared = true;
                    ++status.instances;
                    status.skinnedInstances += root.skinned ? 1u : 0u;
                }
                ++status.roots;
                float& motion = root.skinned ? status.skinnedMotion : status.motion;
                motion = (std::max)(motion, Distance(pair.current.position, pair.previous.position));
                status.positionError = (std::max)(status.positionError, Distance(pair.current.position, root.current.position));
                status.directionError = (std::max)({ status.directionError, Distance(pair.current.normal, root.current.normal),
                    Distance(pair.current.tangent, root.current.tangent) });
                if (!root.hasPrevious) continue;
                ++status.previousRoots;
                status.previousPositionError =
                    (std::max)(status.previousPositionError, Distance(pair.previous.position, root.previous.position));
                status.previousDirectionError = (std::max)({ status.previousDirectionError,
                    Distance(pair.previous.normal, root.previous.normal), Distance(pair.previous.tangent, root.previous.tangent) });
            }
            status.staleInstances += compared ? 0u : 1u;
            ++status.completedChecks;
            status.totalStaleInstances += status.staleInstances;
            status.largestMotion = (std::max)(status.largestMotion, status.motion);
            status.largestSkinnedMotion = (std::max)(status.largestSkinnedMotion, status.skinnedMotion);
            status.worstPositionError = (std::max)({ status.worstPositionError, status.positionError, status.previousPositionError });
            status.worstDirectionError =
                (std::max)({ status.worstDirectionError, status.directionError, status.previousDirectionError });
            spdlog::info("Hair self-check: frame {}: {} instances ({} skinned, {} stale), {} roots ({} with a previous frame): "
                "largest position difference {:.3e} m (previous {:.3e}), largest direction difference {:.3e} (previous {:.3e}), "
                "moved {:.3e} m (skinned {:.3e})",
                frameNumber, status.instances, status.skinnedInstances, status.staleInstances, status.roots, status.previousRoots,
                status.positionError, status.previousPositionError, status.directionError, status.previousDirectionError,
                status.motion, status.skinnedMotion);
        });
}

// Render thread, from the root-binding pass's Update: after the frame's scene synchronization and animation upload.
void HairRuntime::UpdateFrame(const HairFrameInputs& inputs, const HairUpload& upload)
{
    const std::uint64_t frameNumber = inputs.frameNumber;
    const HairFrameObjects& objects = inputs.objects;
    auto& state = *m_state;
    std::scoped_lock lock(state.mutex);
    if (state.lastFrame == frameNumber) return;
    state.lastFrame = frameNumber;
    if (!state.buffers) {
        if (state.assets.empty()) return; // no GPU memory until a groom is loaded
        state.CreateBuffers();
    }
    state.EnsureVisibilityTarget(inputs.renderWidth, inputs.renderHeight);
    for (auto& asset : state.assets) {
        if (!asset.uploaded) state.UploadAsset(asset, upload);
    }

    const bool checking = state.selfCheck.enabled && !state.selfCheckInFlight &&
        (state.selfCheckFrame == kNoFrame || frameNumber >= state.selfCheckFrame + kSelfCheckFrameInterval);
    std::vector<ExpectedRoot> expected;
    std::vector<HairRootCheckSample> checkList; // the roots the root-check pass writes this frame: `expected` rows name them

    // The simulation's steps for this frame, the same for every instance.
    const bool simulating = state.sim.enabled;
    const std::uint32_t simSteps =
        simulating ? state.PlanSimSteps(state.sim.frameSeconds > 0.0f ? state.sim.frameSeconds : inputs.deltaSeconds) : 0u;
    const double simTimeBefore = state.simStatus.simulatedSeconds;
    if (simulating) {
        state.simStatus.steps += simSteps;
        state.simStatus.simulatedSeconds += static_cast<double>(simSteps) * state.sim.stepSeconds;
        state.simStatus.lastFrameSteps = simSteps;
        state.PlanSimLevels(inputs, static_cast<float>(simSteps) * state.sim.stepSeconds);
    } else {
        // Nothing is simulated: no instance holds guide rows.
        for (auto& instance : state.instances) {
            instance.simActive = instance.simUnseen = instance.simStarved = instance.simUnsettled = instance.simEvicted = false;
            state.ReleaseGuideRows(instance);
        }
    }

    HairFramePlan plan;
    std::uint64_t planClusters = 0u;
    std::uint64_t planPoints = 0u;
    for (std::uint32_t index = 0u; index < state.instances.size(); ++index) {
        auto& instance = state.instances[index];
        instance.info.active = false;
        if (instance.info.state != HairInstanceState::Attached) continue;
        const auto entity = br::extensions::FindSceneEntityRenderState(instance.stableSceneID);
        // Not drawn (yet, or any more), or this frame's per-object buffer does not have the entity's row yet.
        if (!entity || entity->perObjectBufferIndex >= objects.rowCount) continue;
        const Asset& asset = state.assets[instance.info.asset];
        const BuiltGroom& groom = asset.groom;
        const bool skinned = instance.skeleton && entity->skinningInstanceSlot != HAIR_INVALID_INDEX;

        HairInstanceGPU gpu{};
        gpu.clusterBase = asset.clusterBase;
        gpu.clusterCount = static_cast<std::uint32_t>(groom.clusters.size());
        gpu.strandBase = asset.strandBase;
        gpu.strandCount = static_cast<std::uint32_t>(groom.strands.size());
        gpu.guideBase = asset.guideBase;
        gpu.guideCount = groom.GuideCount();
        gpu.pointBase = asset.pointBase;
        gpu.pointCount = static_cast<std::uint32_t>(groom.points.size());
        gpu.perObjectIndex = entity->perObjectBufferIndex;
        gpu.skinningInstanceSlot = skinned ? entity->skinningInstanceSlot : HAIR_INVALID_INDEX;
        gpu.attachBone = instance.info.mode == HairAttachMode::Bone ? instance.attachBone : HAIR_INVALID_INDEX;
        gpu.baseColor = instance.info.material.baseColor;
        gpu.roughness = instance.info.material.roughness;
        gpu.guidePointBase = instance.guideRowBase;
        gpu.colliderBase = index * HAIR_MAX_COLLIDERS_PER_INSTANCE;
        gpu.colliderCount = static_cast<std::uint32_t>(instance.colliders.size());
        // Simulated only with guide rows to simulate in (PlanSimLevels saw to it).
        if (instance.simActive && instance.guideRowBase == HAIR_INVALID_INDEX) instance.simActive = false;
        if (instance.simActive) {
            // The guide state is only live while it was simulated in the frame before. After a gap the guides start
            // again: from the shape the rows still hold when there is one, and from the rigid pose otherwise.
            const bool reset = instance.simResetRequested || instance.simulatedFrame == kNoFrame ||
                instance.simulatedFrame + 1u != frameNumber;
            const bool keepShape = reset && !instance.simResetRequested && instance.simStateValid;
            if (reset && !keepShape) instance.simSettledSeconds = 0.0;
            instance.simStateValid = true;
            gpu.simFlags = HAIR_SIM_FLAG_ENABLED | (reset ? HAIR_SIM_FLAG_RESET : 0u) | (keepShape ? HAIR_SIM_FLAG_KEEP_SHAPE : 0u);
            gpu.simLevel = instance.simLevel;
            // Which guides the simulate pass looks at: the levels up to the highest the level has asked for lately.
            const double highWindow = state.sim.demoteSeconds + 4.0 * state.sim.stepSeconds;
            instance.simHighSeconds += static_cast<double>(simSteps) * state.sim.stepSeconds;
            if (reset) {
                instance.simHighLevel = instance.simHighLevelBefore = instance.simLevel;
                instance.simHighSeconds = 0.0;
            } else if (instance.simHighSeconds >= highWindow) {
                instance.simHighLevelBefore = instance.simHighLevel;
                instance.simHighLevel = instance.simLevel;
                instance.simHighSeconds = 0.0;
            } else {
                instance.simHighLevel = (std::max)(instance.simHighLevel, instance.simLevel);
            }
            instance.simResetRequested = false;
            instance.simulatedFrame = frameNumber;
            instance.simLastSimulated = state.guideClock;
        } else {
            instance.simulatedFrame = kNoFrame;
            gpu.simFlags = instance.simUnseen ? HAIR_SIM_FLAG_UNSEEN : 0u;
        }
        if (!instance.collidersUploaded) {
            HairColliderGPU rows[HAIR_MAX_COLLIDERS_PER_INSTANCE] = {};
            for (std::size_t collider = 0u; collider < instance.colliders.size(); ++collider) {
                rows[collider].a = instance.colliders[collider].a;
                rows[collider].b = instance.colliders[collider].b;
                rows[collider].radius = instance.colliders[collider].radius;
            }
            upload(state.buffers->colliders, gpu.colliderBase * sizeof(HairColliderGPU), rows, sizeof(rows));
            instance.collidersUploaded = true;
        }
        if (!instance.gpuValid || std::memcmp(&gpu, &instance.gpu, sizeof(gpu)) != 0) {
            upload(state.buffers->instances, index * sizeof(HairInstanceGPU), &gpu, sizeof(gpu));
            state.instanceBytesUploaded += sizeof(gpu);
            instance.gpu = gpu;
            instance.gpuValid = true;
        }
        instance.info.active = true;
        HairInstanceDispatch dispatch{ index, gpu.clusterCount, gpu.guideCount };
        dispatch.simulated = instance.simActive ? 1u : 0u;
        dispatch.guideLevelCount = groom.GuideLevelCount();
        if (instance.simActive) {
            // A guide of level k is simulated from sFrac k + its offset, so levels 0 to floor(sFrac) can hold one. In the
            // frame an instance starts or is reset the pass writes every guide's state.
            const float high = (std::max)(instance.simHighLevel, instance.simHighLevelBefore);
            const auto levels = (std::min)(dispatch.guideLevelCount, static_cast<std::uint32_t>(high) + 1u);
            dispatch.simulateGuides =
                (gpu.simFlags & HAIR_SIM_FLAG_RESET) != 0u ? gpu.guideCount : high > 0.0f ? groom.guideLevelOffsets[levels] : 0u;
        }
        std::ranges::copy(groom.guideLevelOffsets, dispatch.guideLevelOffsets.begin());
        plan.instances.push_back(dispatch);
        plan.simulate = plan.simulate || instance.simActive;
        planClusters += gpu.clusterCount;
        planPoints += gpu.pointCount;

        if (!state.selfCheck.enabled) continue;
        // The skin matrices this frame's dispatch reads: what SkeletonManager uploaded this frame (inverse bind * pose).
        std::vector<XMFLOAT4X4> skin;
        if (skinned) {
            const auto pose = instance.skeleton->GetBoneMatrices();
            const auto inverseBind = instance.skeleton->GetInverseBindMatrices();
            const std::size_t boneCount = (std::min)(pose.size(), inverseBind.size());
            const std::size_t first = instance.info.mode == HairAttachMode::Bone ? instance.attachBone : 0u;
            const std::size_t last = instance.info.mode == HairAttachMode::Bone ? first + 1u : boneCount;
            for (std::size_t bone = first; bone < last && bone < boneCount; ++bone) {
                XMStoreFloat4x4(&skin.emplace_back(), XMMatrixMultiply(inverseBind[bone], pose[bone]));
            }
        }
        if (checking) {
            const bool hasPrevious = !skinned || (instance.previousSkinFrame + 1u == frameNumber &&
                instance.previousSkin.size() == skin.size());
            // The matrices the dispatch reads: the entity's row of the per-object buffer this frame binds.
            ObjectMatrices matrices{};
            const bool haveMatrices = objects.image && objects.rowStride >= sizeof(ObjectMatrices) &&
                ReadImage(*objects.image, static_cast<std::size_t>(entity->perObjectBufferIndex) * objects.rowStride,
                    &matrices, sizeof(matrices));
            const XMMATRIX model = XMLoadFloat4x4(&matrices.model);
            const XMMATRIX previousModel = XMLoadFloat4x4(&matrices.previousModel);
            // Without them one root with no expectation marks the instance as not compared.
            const std::uint32_t count = haveMatrices ? (std::min)(state.selfCheck.rootsPerInstance, gpu.strandCount) : 0u;
            if (!haveMatrices || checkList.size() + count > HAIR_ROOT_CHECK_CAPACITY) {
                ExpectedRoot unknown;
                unknown.instance = index;
                unknown.unknown = true;
                expected.push_back(unknown);
            }
            for (std::uint32_t sample = 0u; sample < count && checkList.size() + count <= HAIR_ROOT_CHECK_CAPACITY + sample; ++sample) {
                const auto root = static_cast<std::uint32_t>(static_cast<std::uint64_t>(sample) * gpu.strandCount / count);
                const HairRootBinding& binding = groom.rootBindings[root];
                ExpectedRoot expectedRoot;
                expectedRoot.instance = index;
                expectedRoot.row = static_cast<std::uint32_t>(checkList.size());
                checkList.push_back({ index, root });
                expectedRoot.skinned = skinned;
                expectedRoot.hasPrevious = hasPrevious;
                const XMMATRIX currentSkin = skinned ? RootSkin(instance.info.mode, binding, skin) : XMMatrixIdentity();
                expectedRoot.current = MakeRootFrame(binding, currentSkin, model, frameNumber);
                if (hasPrevious) {
                    const XMMATRIX previousSkin =
                        skinned ? RootSkin(instance.info.mode, binding, instance.previousSkin) : XMMatrixIdentity();
                    expectedRoot.previous = MakeRootFrame(binding, previousSkin, previousModel, frameNumber);
                }
                expected.push_back(expectedRoot);
            }
        }
        instance.previousSkin = std::move(skin);
        instance.previousSkinFrame = skinned ? frameNumber : kNoFrame;
    }

    ++state.frames;

    // A frozen LOD keeps the visible-cluster list of the last selection while it was made for these same instances.
    plan.select = !plan.instances.empty() &&
        !(state.lod.freeze && state.selectionValid && state.selectedInstances == plan.instances);
    if (plan.select) state.selectedInstances = plan.instances;
    state.selectionValid = !plan.instances.empty();
    // The GPU decides how many clusters and vertices a frame draws. These are the most it can.
    plan.evaluateGroups = static_cast<std::uint32_t>((std::min<std::uint64_t>)(planClusters, state.capacities.visibleClusters));
    plan.rasterRows = static_cast<std::uint32_t>((std::min<std::uint64_t>)(planPoints, state.capacities.poolVertices));
    plan.debugView = state.debugView;
    plan.probe = !plan.instances.empty() && std::exchange(state.probeRequested, false);
    if (!checkList.empty()) {
        upload(state.buffers->rootCheckList, 0u, checkList.data(), checkList.size() * sizeof(HairRootCheckSample));
        plan.rootCheckCount = static_cast<std::uint32_t>(checkList.size());
        state.lastRootChecks = plan.rootCheckCount;
    }
    if (plan.simulate) {
        // The frame's simulation thread groups: the simulate pass's, then the guide-level pass's level by level.
        std::vector<HairSimGroup> groups;
        const auto append = [&groups](std::uint32_t instance, std::uint32_t first, std::uint32_t end) {
            for (; first < end; first += 64u) groups.push_back({ instance, first, (std::min)(64u, end - first), 0u });
        };
        for (const auto& dispatch : plan.instances) {
            if (dispatch.simulated != 0u) append(dispatch.instanceIndex, 0u, dispatch.simulateGuides);
        }
        plan.simulateGroups = static_cast<std::uint32_t>(groups.size());
        for (std::uint32_t level = 0u; level < HAIR_MAX_GUIDE_LEVELS; ++level) {
            plan.levelGroupFirst[level] = static_cast<std::uint32_t>(groups.size());
            for (const auto& dispatch : plan.instances) {
                if (dispatch.simulated == 0u || level >= dispatch.guideLevelCount) continue;
                append(dispatch.instanceIndex, dispatch.guideLevelOffsets[level], dispatch.guideLevelOffsets[level + 1u]);
            }
        }
        plan.levelGroupFirst[HAIR_MAX_GUIDE_LEVELS] = static_cast<std::uint32_t>(groups.size());
        if (groups.size() > state.capacities.simGroups) {
            spdlog::error("Hair: {} simulation thread groups do not fit the {} of HairCapacities::simGroups; not simulating this frame",
                groups.size(), state.capacities.simGroups);
            plan.simulateGroups = 0u;
            plan.levelGroupFirst.fill(0u);
        } else if (!groups.empty()) {
            upload(state.buffers->simGroups, 0u, groups.data(), groups.size() * sizeof(HairSimGroup));
        }
        state.lastSimGroups = plan.levelGroupFirst[HAIR_MAX_GUIDE_LEVELS];
    } else {
        state.lastSimGroups = 0u;
    }
    state.Use(kBufferSimGroups, state.lastSimGroups);
    // The evaluate pass's dispatch, as it records it: one over the most clusters the frame can show.
    state.evaluateDispatches += plan.evaluateGroups != 0u ? 1u : 0u;
    state.plan = plan;

    HairSimFrameGPU simFrame{};
    simFrame.stepCount = simSteps;
    simFrame.substeps = state.sim.substeps;
    simFrame.stepSeconds = state.sim.stepSeconds;
    simFrame.time = static_cast<float>(simTimeBefore);
    simFrame.gravity = state.sim.gravity;
    simFrame.damping = state.sim.damping;
    simFrame.wind = state.sim.wind;
    simFrame.gustAmplitude = state.sim.gustAmplitude;
    simFrame.gustFrequency = state.sim.gustFrequency;
    simFrame.stretchCompliance = state.sim.stretchCompliance;
    simFrame.bendCompliance = state.sim.bendCompliance;
    simFrame.shapeCompliance = state.sim.shapeCompliance;
    simFrame.friction = state.sim.friction;
    simFrame.collisionMargin = state.sim.collisionMargin;
    simFrame.teleportDistance = state.sim.teleportDistance;
    simFrame.promoteSeconds = state.sim.promoteSeconds;
    simFrame.demoteSeconds = state.sim.demoteSeconds;
    simFrame.frameNumber = static_cast<std::uint32_t>(frameNumber);
    upload(state.buffers->simFrame, 0u, &simFrame, sizeof(simFrame));
    if (simulating) {
        // The frame's metrics start from nothing measured; the smallest collider distance starts at all ones.
        std::vector<std::uint32_t> metrics(state.instances.size() * HAIR_SIM_METRIC_COUNT, 0u);
        for (std::size_t index = 0u; index < state.instances.size(); ++index) {
            metrics[index * HAIR_SIM_METRIC_COUNT + HAIR_SIM_METRIC_COLLIDER_DISTANCE] = 0xFFFFFFFFu;
        }
        upload(state.buffers->simMetrics, 0u, metrics.data(), metrics.size() * sizeof(std::uint32_t));
    }

    HairFrameGPU frame{};
    frame.frameNumber = static_cast<std::uint32_t>(frameNumber);
    frame.lodErrorPixels = state.lod.errorPixels;
    frame.maxWidthScale = state.lod.maxWidthScale;
    frame.minDetail = state.lod.minDetail;
    frame.forceDetail = state.lod.forceDetail;
    frame.segmentPixels = state.lod.segmentPixels;
    frame.visibleClusterCapacity = state.capacities.visibleClusters;
    frame.poolVertexCapacity = state.capacities.poolVertices;
    frame.coverageMode = static_cast<std::uint32_t>(state.lod.coverage);
    frame.coverageAccumulate = state.lod.accumulateCoverage || state.debugView == HairDebugView::Coverage ? 1u : 0u;
    frame.detailExponent = state.lod.detailExponent;
    upload(state.buffers->frameState, 0u, &frame, sizeof(frame));
    // The frame's counters start at zero. A frozen selection keeps its counters with its list.
    static constexpr std::uint32_t zeros[HAIR_COUNTER_COUNT] = {};
    const std::uint32_t firstReset = plan.select || plan.instances.empty() ? HAIR_COUNTER_SELECTION_FIRST : HAIR_COUNTER_RASTER_FIRST;
    // The visible-instance bits are the selection's too, and sit after the raster counters.
    const std::uint32_t resetEnd = firstReset == HAIR_COUNTER_SELECTION_FIRST ? HAIR_COUNTER_COUNT : HAIR_COUNTER_INSTANCE_MASK_FIRST;
    upload(state.buffers->counters, firstReset * sizeof(std::uint32_t), zeros, (resetEnd - firstReset) * sizeof(std::uint32_t));
    upload(state.buffers->counters, HAIR_COUNTER_FRAME_FIRST * sizeof(std::uint32_t), zeros,
        (HAIR_COUNTER_SELECTION_FIRST - HAIR_COUNTER_FRAME_FIRST) * sizeof(std::uint32_t));

    if (plan.instances.empty()) return;
    if (plan.probe) {
        // The probe pass counts its rows from zero.
        upload(state.buffers->probe, 0u, zeros, sizeof(std::uint32_t));
        state.RequestProbe(m_state);
    }
    if (state.continuousCounters || (state.counterReadbackWanted && !state.counterReadbackInFlight)) {
        state.RequestCounterReadback(m_state, inputs, plan.select);
    }
    if (checking) state.RequestSelfCheck(m_state, frameNumber, std::move(expected));
    if (!state.dumpRequests.empty()) state.RequestDumps(m_state, frameNumber);
    // Also while no instance is simulated: the metrics then say so.
    if (simulating && (state.continuousCounters || (state.simReadbackWanted && !state.simReadbackInFlight))) {
        state.RequestSimMetricsReadback(m_state, frameNumber);
    }
}

} // namespace br::hair
