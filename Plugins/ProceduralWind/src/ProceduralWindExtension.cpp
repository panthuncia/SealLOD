#include "ProceduralWind/ProceduralWindExtension.h"
#include "ProceduralWind/DynamicWindGPU.h"

#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include <BasicRenderer/Extensions/PipelineAccess.h>
#include <BasicRenderer/Extensions/RenderDeviceAccess.h>
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/IndirectCommand.h>
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "Render/MemoryIntrospectionAPI.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include <BasicRenderer/Streaming/PoseState.h>
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "RenderPasses/Base/RenderPass.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "Resources/PixelBuffer.h"
#include "Render/Runtime/IReadbackService.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

#include <fmt/format.h>
#include <tracy/Tracy.hpp>
#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <unordered_set>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace br::wind {
namespace {

constexpr std::uint32_t kThreadsPerGroup = 64u;
constexpr std::uint32_t kMaximumWindLodVariants = 16u;
constexpr std::uint32_t kLatePhaseBit = 0x80000000u;
constexpr std::uint32_t kDepthDescriptorMask = 0x7fffffffu;
constexpr std::uint32_t kWindBoneFlagTrunk = 1u << 0u;

std::uint32_t TransientBoneCapacity()
{
	return std::clamp(
		br::extensions::ReadUnsignedSetting(
			ProceduralWindTransientBoneCapacitySettingName),
		1024u,
		1048576u);
}

std::filesystem::path WindTelemetryPath()
{
	if (const char* benchmarkOutput = std::getenv("SARP_SCENE_OUT"); benchmarkOutput && *benchmarkOutput)
		return std::filesystem::path(benchmarkOutput).concat(".wind.log");
	return {};
}

void EmitWindTelemetry(const std::string& line)
{
	spdlog::info("{}", line);
	const auto path = WindTelemetryPath();
	if (path.empty()) return;
	static std::mutex mutex;
	const std::scoped_lock lock(mutex);
	std::ofstream output(path, std::ios::app);
	if (output) output << line << '\n';
}

struct WindBoneGPU {
    std::uint32_t skinningSlot = 0u;
    std::uint32_t jointIndex = 0u;
    std::uint32_t parentEntry = kInvalidSimulationGroup;
    std::uint32_t simulationGroup = kInvalidSimulationGroup;
    std::uint32_t phaseSeed = 0u;
    std::uint32_t flags = 0u;
    float influence = 0.0f;
    float meanBend = 0.0f;
    float parallelAmplitude = 0.0f;
    float perpendicularRatio = 0.0f;
    float torsionRatio = 0.0f;
    float frequencyScale = 1.0f;
    float maximumAngle = 0.0f;
    float gustAttenuation = 0.0f;
    std::array<float, 2> pad0{};
    std::array<float, 3> frequencies{};
    float pad1 = 0.0f;
    std::array<float, 3> weights{};
    float pad2 = 0.0f;
    std::array<float, 3> branchAxis{ 0.0f, 0.0f, 1.0f };
    float pad3 = 0.0f;
    std::array<float, 3> branchTangent{ 1.0f, 0.0f, 0.0f };
    float pad4 = 0.0f;
    DirectX::XMFLOAT4X4 bindGlobal{};
    DirectX::XMFLOAT4X4 inverseBind{};
};

struct WindRootConstants {
    std::uint32_t boneEntries = 0u;
    std::uint32_t scratchForward = 0u;
    std::uint32_t scratchInverse = 0u;
    std::uint32_t skinningInfo = 0u;
    std::uint32_t forwardSkin = 0u;
    std::uint32_t inverseSkin = 0u;
    std::uint32_t inverseBind = 0u;
    std::uint32_t boneCount = 0u;
    float elapsedSeconds = 0.0f;
    float windX = 1.0f;
    float windY = 0.0f;
    float strength = 0.0f;
    float gustStrength = 0.0f;
    std::uint32_t fieldSlice0 = 0u;
    std::uint32_t fieldSlice1 = 0u;
    std::uint32_t fieldDimensions = 0u;
    float fieldCellSize = 0.0f;
    float fieldOriginX = 0.0f;
    float fieldOriginY = 0.0f;
    float fieldInterpolation = 0.0f;
    std::uint32_t fieldValid = 0u;
};

struct WindTypeGPU {
    std::uint32_t firstBone = 0u;
    std::uint32_t boneCount = 0u;
    std::uint32_t sourceSkinningSlot = 0u;
    std::uint32_t bucketBase = 0u;
    std::uint32_t bucketCapacity = 0u;
    std::uint32_t diagnosticsDescriptor = 0u;
    std::uint32_t activeEntriesDescriptor = 0u;
    std::uint32_t transformCount = 0u;
    std::uint32_t deferredEntriesDescriptor = 0u;
    std::uint32_t processedTypeCountsDescriptor = 0u;
	std::uint32_t remapDescriptor = 0u;
	std::uint32_t remapOffset = 0u;
	std::uint32_t sourceBoneCount = 0u;
	std::uint32_t lodLevel = 0u;
	std::uint32_t baseTypeLookupDescriptor = 0u;
	std::uint32_t baseTypeLookupCount = 0u;
	std::uint32_t variantCount = 0u;
	float normalizedQuality = 0.0f;
	float collapseError = 0.0f;
	float qualityBias = 1.0f;
};

struct WindActiveInstanceGPU {
    std::uint32_t instanceTransformIndex = 0u;
    std::uint32_t stableSceneId = 0u;
    std::uint32_t transformOffsetMatrices = 0u;
    std::uint32_t inverseSkinOffsetMatrices = 0u;
	float screenFraction = 0.0f;
	std::uint32_t priorityKey = 0u;
	float windWeight = 1.0f;
	std::uint32_t pad0 = 0u;
};

struct DynamicWindVisibleSkeletonGPU {
	std::uint32_t instanceTransformIndex = 0u;
	std::uint32_t transientSkinningSlot = 0xFFFFFFFFu;
	std::uint32_t stableSceneId = 0u;
	std::uint32_t typeId = 0u;
	std::uint32_t skeletonLod = 0u;
	std::uint32_t priorityKey = 0u;
};

struct WindIndirectCommand {
    std::uint32_t typeId = 0u;
    std::uint32_t pad0 = 0u;
    std::uint32_t pad1 = 0u;
    D3D12_DISPATCH_ARGUMENTS dispatch{};
};

struct WindAllocationRecordGPU {
	std::uint32_t processedCount = 0u;
	std::uint32_t previousAcceptedCount = 0u;
	std::uint32_t acceptedCount = 0u;
	std::uint32_t typeMatrixBase = 0u;
	std::uint32_t baseTypeId = 0u;
};

struct WindTransientConstants {
    std::uint32_t types, bones, activeInstances, typeCounters;
    std::uint32_t counters, placements, indirectCommands, skinningInfo;
    std::uint32_t forwardSkin, inverseSkin, inverseBind, placementCount;
    std::uint32_t typeCount, transformBase, inverseBase, matrixCapacity;
    std::uint32_t cameraIndex, phaseAndDepthDescriptor, fieldSlice0, fieldSlice1;
    std::uint32_t fieldDimensions;
    float fieldCellSize, fieldOriginX, fieldOriginY;
    float fieldInterpolation, elapsedSeconds, windX, windY;
    float strength, gustStrength;
	std::array<float, 6> qualityCurveScreen;
	std::array<float, 6> qualityCurveValue;
	float staticCutoff;
	float lodHysteresis;
	std::int32_t forcedLod;
	float capacityTarget;
	float lateReserve;
	std::uint32_t allocationRecords;
};

static_assert(sizeof(WindBoneGPU) == 256u);
static_assert(sizeof(WindTypeGPU) == 80u);
static_assert(sizeof(WindActiveInstanceGPU) == 32u);
static_assert(sizeof(WindRootConstants) % sizeof(std::uint32_t) == 0u);

struct WindSharedResources {
    explicit WindSharedResources(std::shared_ptr<ProceduralWindRuntime> runtimeIn, org::runtime::IReadbackService* readbackServiceIn)
        : runtime(std::move(runtimeIn))
		, readbackService(readbackServiceIn)
        , fieldSlices{ DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice0"),
                       DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice1") }
		, frameState(DynamicStructuredBuffer<DynamicWindFrameGPU>::CreateShared(1u, "ProceduralWind.FrameState"))
        , boneEntries(DynamicStructuredBuffer<WindBoneGPU>::CreateShared(1u, "ProceduralWind.BoneEntries"))
		, boneRemaps(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.BoneRemaps"))
		, baseTypeLookup(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.BaseTypeBySkeletonSlot"))
        , windTypes(DynamicStructuredBuffer<WindTypeGPU>::CreateShared(1u, "ProceduralWind.Types"))
        , activeInstances(DynamicStructuredBuffer<WindActiveInstanceGPU>::CreateShared(1u, "ProceduralWind.ActiveInstances", true))
        , typeCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.TypeCounters", true))
        , processedTypeCounts(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.ProcessedTypeCounts", true))
        , deferredEntries(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.DeferredEntries", true))
        , allocationCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(15u, "ProceduralWind.AllocationCounters", true))
		, diagnostics(DynamicStructuredBuffer<std::uint32_t>::CreateShared(112u, "ProceduralWind.Diagnostics", true))
		, indirectCommands(DynamicStructuredBuffer<WindIndirectCommand>::CreateShared(1u, "ProceduralWind.IndirectCommands", true))
		, allocationRecords(DynamicStructuredBuffer<WindAllocationRecordGPU>::CreateShared(1u, "ProceduralWind.AllocationRecords", true))
		, visibleSkeletons(DynamicStructuredBuffer<DynamicWindVisibleSkeletonGPU>::CreateShared(1u, "ProceduralWind.VisibleSkeletons", true))
		, visibleSkeletonCounter(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.VisibleSkeletonCounter", true))
		, visibleSkeletonMembership(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.VisibleSkeletonMembership", true))
    {
		const auto tagResource = [](const auto& resource) {
			org::memory::SetResourceUsageHint(*resource, "Procedural wind");
		};
		tagResource(fieldSlices[0]);
		tagResource(fieldSlices[1]);
		tagResource(frameState);
		tagResource(boneEntries);
		tagResource(boneRemaps);
		tagResource(baseTypeLookup);
		tagResource(windTypes);
		tagResource(activeInstances);
		tagResource(typeCounters);
		tagResource(processedTypeCounts);
		tagResource(deferredEntries);
		tagResource(allocationCounters);
		tagResource(diagnostics);
		tagResource(indirectCommands);
		tagResource(allocationRecords);
		tagResource(visibleSkeletons);
		tagResource(visibleSkeletonCounter);
		tagResource(visibleSkeletonMembership);
		static std::once_flag resetTelemetry;
		std::call_once(resetTelemetry, [] {
			const auto path = WindTelemetryPath();
			if (!path.empty()) std::ofstream(path, std::ios::trunc);
		});
	}

    void RequestTelemetryReadback()
    {
        if (!readbackService || registeredTypeCount == 0u || residentPlacementCount == 0u || elapsedSeconds < nextTelemetrySeconds) return;
        nextTelemetrySeconds = elapsedSeconds + 2.0f;
        readbackService->RequestReadbackCaptureAfterGraph(allocationCounters.get(), {},
            [](org::ReadbackCaptureResult&& result) {
                if (result.data.size() < 15u * sizeof(std::uint32_t)) return;
                std::array<std::uint32_t, 15> c{};
                std::memcpy(c.data(), result.data.data(), (std::min)(result.data.size(), sizeof(c)));
				PublishCLodTelemetrySnapshot(g_dynamicWindVisibility, DynamicWindVisibilitySnapshot{
					.phase1Accepted = c[9],
					.phase2Accepted = c[13],
					.deferred = c[10],
					.capacityRejects = c[2],
					.bucketOverflow = c[3],
					.deferredOverflow = c[14],
				});
				EmitWindTelemetry(fmt::format(
					"ProceduralWind GPU telemetry: allocatedBones={} commands={} capacityRejects={} bucketOverflow={} allocatedAssemblies={} livePlacements={} stalePlacements={} frustumRejected={} distanceRejected={} visibleBucketed={} deferred={} deferredWritten={} lateOccluded={} lateAccepted={} deferredOverflow={}.",
					c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14]));
            }, org::QueueKind::Copy);
        const float currentScale = displacementScale;
        const float currentStrength = state.strength;
        readbackService->RequestReadbackCaptureAfterGraph(diagnostics.get(), {},
            [currentScale, currentStrength](org::ReadbackCaptureResult&& result) {
				if (result.data.size() < 112u * sizeof(std::uint32_t)) return;
				std::array<std::uint32_t, 112> d{};
                std::memcpy(d.data(), result.data.data(), (std::min)(result.data.size(), sizeof(d)));
                const auto asFloat = [&d](std::size_t index) {
                    float value = 0.0f;
                    std::memcpy(&value, &d[index], sizeof(value));
                    return value;
                };
				EmitWindTelemetry(fmt::format(
                    "ProceduralWind simulation telemetry: settingScale={:.3f} runtimeStrength={:.3f} effectiveScale={:.3f} "
                    "windGroupBones={} movedBones={} matrixWrites={} finiteWrites={} nonFiniteWrites={} "
                    "maxForcing={:.6f} maxBendRadians={:.6f} maxTorsionRadians={:.6f} "
                    "maxJointDisplacementOS={:.6f} maxSkinMatrixDelta={:.6f} maxBindIdentityError={:.6f} "
                    "maxBindOriginMappingError={:.6f}.",
                    currentScale, currentStrength, currentScale * currentStrength,
                    d[14], d[15], d[21], d[12], d[13],
					asFloat(16), asFloat(17), asFloat(18), asFloat(19), asFloat(20), asFloat(22), asFloat(23)));
				std::ostringstream desired, actual, simulated, animatedInstances;
				for (std::size_t lod = 0; lod < 16u; ++lod) {
					if (lod != 0u) { desired << ','; actual << ','; simulated << ','; animatedInstances << ','; }
					desired << d[40u + lod]; actual << d[24u + lod]; simulated << d[72u + lod];
					animatedInstances << d[96u + lod];
				}
				EmitWindTelemetry(fmt::format(
					"ProceduralWind skeleton LOD telemetry: desired=[{},static={}] actual=[{}] simulatedWrites=[{}] animatedInstances=[{}] "
					"transitions={} staticFallbacks={} duplicateAllocations={} duplicateStableId={} duplicateLods={}->{} "
					"duplicateTransform={} missingVariant={} invalidSourceJoint={} invalidParent={} invalidPaletteWrites={} "
					"maxDriverDepth={} maxPaletteOffset={} transformBase={} matrixCapacity={} sampledPaletteIndex={} "
					"sampledInstanceOffset={} sampledType={}.",
					desired.str(), d[56], actual.str(), simulated.str(), animatedInstances.str(), d[57], d[58], d[59], d[60],
					d[61] & 0xffffu, d[61] >> 16u, d[62], d[63], d[64], d[65], d[67], d[88], d[89],
					d[66], d[68], d[69], d[70], d[71]));
				EmitWindTelemetry(fmt::format(
					"ProceduralWind radius telemetry: fullStrengthPlacements={} fadingPlacements={}.",
					d[90], d[91]));
            }, org::QueueKind::Copy);
    }

    void UpdateFieldPair()
    {
        const auto pair = runtime->SnapshotResidentPair();
        if (!pair.valid || (pair.revision == fieldRevision && fieldReady)) return;
        bool ready = true;
        for (std::size_t slice = 0; slice < 2u; ++slice) {
            std::vector<std::uint32_t> packed((pair.rgba16f[slice].size() + 1u) / 2u, 0u);
            for (std::size_t i = 0; i < pair.rgba16f[slice].size(); ++i)
                packed[i / 2u] |= static_cast<std::uint32_t>(pair.rgba16f[slice][i]) << ((i & 1u) * 16u);
            fieldSlices[slice]->ReplaceData(std::move(packed));
            ready = ready && fieldSlices[slice]->ResidentCapacity() >= (pair.rgba16f[slice].size() + 1u) / 2u;
        }
        fieldRevision = pair.revision;
        fieldReady = ready;
        residentPair = ready ? pair : ResidentWindPair{};
        TracyPlot("ProceduralWind.ResidentDirectionLower", static_cast<std::int64_t>(pair.bracket.lower));
        TracyPlot("ProceduralWind.ResidentDirectionUpper", static_cast<std::int64_t>(pair.bracket.upper));
    }

	void AdvanceAndPublishFrame(float deltaTime, const ProceduralWindFrameSettings& settings)
	{
		previousElapsedSeconds = elapsedSeconds;
		elapsedSeconds += (std::max)(0.0f, deltaTime);
		state = runtime->SnapshotWindState();
		displacementScale = std::clamp(
			settings.displacementScale,
			0.0f,
			100.0f);
		const auto& resident = residentPair;
		DynamicWindFrameGPU frame{};
		frame.fieldSlice0 = fieldSlices[0]->GetSRVInfo(0).slot.index;
		frame.fieldSlice1 = fieldSlices[1]->GetSRVInfo(0).slot.index;
		frame.fieldDimensions =
			(resident.metadata.width & 0xffffu) |
			((resident.metadata.height & 0xffffu) << 16u);
		frame.fieldValid = fieldReady && resident.valid ? 1u : 0u;
		frame.fieldCellSize = resident.metadata.cellSize;
		frame.fieldOriginX = resident.metadata.origin.x;
		frame.fieldOriginY = resident.metadata.origin.y;
		frame.fieldInterpolation = resident.bracket.interpolation;
		frame.currentTime = elapsedSeconds;
		frame.previousTime = previousElapsedSeconds;
		frame.deltaTime = (std::max)(0.0f, deltaTime);
		frame.strength = state.strength;
		frame.windX = state.directionToWS.x;
		frame.windY = state.directionToWS.y;
		frame.gustStrength = state.gustStrength;
		frame.treeDisplacementScale = displacementScale;
		frameState->ReplaceData({ frame });
	}

    // Layout of one wind base skeleton: every LOD variant's type row and bone
    // entries with block-relative offsets and no instance slot. Blocks depend only
    // on the skeleton and the profile revision, so streaming instances in and out
    // concatenates cached blocks instead of re-deriving every bone.
    struct WindTypeBlock {
        std::vector<WindTypeGPU> types;          // firstBone / remapOffset relative to the block
        std::vector<WindBoneGPU> bones;          // parentEntry relative to the block
        std::vector<std::uint32_t> remaps;
        std::string layoutSummary;
    };

    std::shared_ptr<const WindTypeBlock> BuildTypeBlock(const Skeleton& typeSkeleton) const
    {
        auto block = std::make_shared<WindTypeBlock>();
        const auto& authoredWind = typeSkeleton.GetDynamicWindMetadata();
        const auto profile = runtime->ResolveProfile(typeSkeleton.GetWindProfileIdentity());
        const auto lodVariants = typeSkeleton.GetSkeletonLodVariants();
        const std::uint32_t sourceBoneCount = typeSkeleton.GetBoneCount();
        const std::uint32_t availableVariants = lodVariants.empty() ? 1u : (std::min)(kMaximumWindLodVariants, static_cast<std::uint32_t>(lodVariants.size()));
        const auto baseGroups = typeSkeleton.GetWindSimulationGroupIndices();
        const auto baseParents = typeSkeleton.GetParentIndices();
        const auto baseInvariants = typeSkeleton.GetWindBoneInvariants();
        const auto baseBindGlobals = typeSkeleton.GetBindGlobalMatrices();
        const auto baseInverseBinds = typeSkeleton.GetInverseBindMatrices();
        const auto boneNames = typeSkeleton.GetBoneNames();
        // Phase seeds depend on (profile identity, chain-origin bone name) only.
        std::vector<std::uint32_t> phaseSeedByJoint(sourceBoneCount, 0u);
        for (std::uint32_t sourceJoint = 0u; sourceJoint < sourceBoneCount; ++sourceJoint) {
            std::uint32_t phaseJoint = sourceJoint;
            if (authoredWind.enabled && sourceJoint < authoredWind.bones.size()) {
                const std::uint32_t chainOrigin = authoredWind.bones[sourceJoint].chainOriginBoneIndex;
                if (chainOrigin != 0xFFFFFFFFu && chainOrigin < sourceBoneCount) phaseJoint = chainOrigin;
            }
            // Type slots are runtime registration details and must not affect
            // animation. A NIF may expose a different local skin palette per mesh;
            // profile + chain-origin name keeps matching drivers phase-aligned
            // across those palettes while remaining deterministic across runs.
            std::uint32_t stablePhase = 2166136261u;
            const auto hashPhaseText = [&stablePhase](std::string_view text) {
                for (const unsigned char ch : text) {
                    stablePhase ^= static_cast<std::uint32_t>(std::tolower(ch));
                    stablePhase *= 16777619u;
                }
            };
            hashPhaseText(typeSkeleton.GetWindProfileIdentity());
            if (phaseJoint < boneNames.size()) hashPhaseText(boneNames[phaseJoint]);
            else stablePhase ^= phaseJoint * 2891336453u + 277803737u;
            phaseSeedByJoint[sourceJoint] = stablePhase;
        }
        std::ostringstream layoutSummary;
        layoutSummary << " source=" << sourceBoneCount << " lods=[";
        for (std::uint32_t lod = 0u; lod < availableVariants; ++lod) {
            const SkeletonLodVariant* variant = lodVariants.empty() ? nullptr : &lodVariants[lod];
            const std::uint32_t boneCount = variant ? static_cast<std::uint32_t>(variant->lodToBaseBone.size()) : sourceBoneCount;
            if (lod != 0u) layoutSummary << ',';
            layoutSummary << boneCount;
            if (variant) layoutSummary << "@q" << fmt::format("{:.3f}", variant->normalizedQuality)
                << "/e" << fmt::format("{:.3f}", variant->collapseError);
            if (variant) {
                std::uint32_t invalidRemaps = 0u;
                std::uint32_t invalidParents = 0u;
                for (const auto mapped : variant->baseToLodBone) invalidRemaps += mapped >= boneCount ? 1u : 0u;
                for (std::uint32_t compact = 0u; compact < variant->parentIndices.size(); ++compact) {
                    const auto parent = variant->parentIndices[compact];
                    invalidParents += parent >= 0 && static_cast<std::uint32_t>(parent) >= compact ? 1u : 0u;
                }
                const bool aligned = variant->level == lod && variant->baseToLodBone.size() == sourceBoneCount &&
                    variant->parentIndices.size() == boneCount && variant->inverseBindMatrices.size() == boneCount &&
                    variant->bindGlobalMatrices.size() == boneCount && variant->windSimulationGroupIndices.size() == boneCount;
                if (!aligned || invalidRemaps != 0u || invalidParents != 0u) {
                    spdlog::error(
                        "ProceduralWind invalid skeleton LOD: lod={} sourceBones={} compactBones={} aligned={} invalidRemaps={} invalidParents={}.",
                        lod, sourceBoneCount, boneCount, aligned, invalidRemaps, invalidParents);
                }
            }
            WindTypeGPU type{};
            type.firstBone = static_cast<std::uint32_t>(block->bones.size());
            type.boneCount = boneCount;
            type.remapOffset = static_cast<std::uint32_t>(block->remaps.size());
            type.sourceBoneCount = sourceBoneCount;
            type.lodLevel = lod;
            type.variantCount = availableVariants;
            type.normalizedQuality = variant ? variant->normalizedQuality : 1.0f;
            type.collapseError = variant ? variant->collapseError : 0.0f;
            type.qualityBias = authoredWind.skeletonLodQualityBias;
            if (variant) block->remaps.insert(block->remaps.end(), variant->baseToLodBone.begin(), variant->baseToLodBone.end());
            else for (std::uint32_t bone = 0; bone < sourceBoneCount; ++bone) block->remaps.push_back(bone);
            const auto groups = variant ? std::span<const std::uint32_t>(variant->windSimulationGroupIndices) : baseGroups;
            const auto parents = variant ? std::span<const std::int32_t>(variant->parentIndices) : baseParents;
            const auto invariants = variant ? std::span<const SkeletonWindBoneInvariant>(variant->windBoneInvariants) : baseInvariants;
            for (std::uint32_t joint = 0u; joint < boneCount; ++joint) {
                WindBoneGPU entry{};
                const std::uint32_t sourceJoint = variant ? variant->lodToBaseBone[joint] : joint;
                entry.jointIndex = sourceJoint;
                entry.phaseSeed = sourceJoint < phaseSeedByJoint.size() ? phaseSeedByJoint[sourceJoint] : 0u;
                entry.simulationGroup = joint < groups.size() ? groups[joint] : kInvalidSimulationGroup;
                if (joint < parents.size() && parents[joint] >= 0)
                    entry.parentEntry = type.firstBone + static_cast<std::uint32_t>(parents[joint]);
                std::uint32_t profileGroup = entry.simulationGroup;
                if (entry.simulationGroup < authoredWind.groups.size() && authoredWind.groups[entry.simulationGroup].profileGroupId != 0xFFFFFFFFu)
                    profileGroup = authoredWind.groups[entry.simulationGroup].profileGroupId;
                const auto found = std::ranges::find(profile.groups, profileGroup, &WindSimulationGroupProfile::id);
                if (found != profile.groups.end()) {
                    if (found->isTrunk) entry.flags |= kWindBoneFlagTrunk;
                    entry.influence = found->influence;
                    entry.meanBend = found->meanBendRadians;
                    entry.parallelAmplitude = found->parallelAmplitudeRadians;
                    entry.perpendicularRatio = found->perpendicularRatio;
                    entry.torsionRatio = found->torsionRatio;
                    entry.frequencyScale = found->frequencyScale;
                    entry.maximumAngle = found->maximumAngleRadians;
                }
                if (authoredWind.enabled && entry.simulationGroup < authoredWind.groups.size()) {
                    const auto& authoredGroup = authoredWind.groups[entry.simulationGroup];
                    if ((authoredGroup.flags & DynamicWindMetadata::GroupFlagTrunk) != 0u)
                        entry.flags |= kWindBoneFlagTrunk;
                    entry.influence = authoredGroup.influence;
                    if ((authoredGroup.flags & DynamicWindMetadata::GroupFlagDualInfluence) != 0u &&
                        sourceJoint < authoredWind.bones.size()) {
                        const auto& bone = authoredWind.bones[sourceJoint];
                        const float denominator = static_cast<float>(bone.chainBoneCount > 1u ? bone.chainBoneCount - 1u : 1u);
                        const float chainT = std::clamp(
                            static_cast<float>(bone.indexInBoneChain) / denominator + authoredGroup.shiftTop,
                            0.0f,
                            1.0f);
                        entry.influence = std::lerp(authoredGroup.minInfluence, authoredGroup.maxInfluence, chainT);
                    }
                }
                entry.gustAttenuation = std::clamp(authoredWind.gustAttenuation, 0.0f, 1.0f);
                entry.frequencies = profile.harmonicFrequenciesHz;
                entry.weights = profile.harmonicWeights;
                if (joint < invariants.size()) {
                    const auto& invariant = invariants[joint];
                    entry.flags |= invariant.flags;
                    entry.branchAxis = { invariant.branchAxis.x, invariant.branchAxis.y, invariant.branchAxis.z };
                    entry.branchTangent = { invariant.branchTangent.x, invariant.branchTangent.y, invariant.branchTangent.z };
                }
                const DirectX::XMMATRIX inverseBind = variant
                    ? DirectX::XMLoadFloat4x4(&variant->inverseBindMatrices[joint])
                    : (joint < baseInverseBinds.size() ? baseInverseBinds[joint] : DirectX::XMMatrixIdentity());
                const DirectX::XMMATRIX bindGlobal = variant
                    ? DirectX::XMLoadFloat4x4(&variant->bindGlobalMatrices[joint])
                    : (joint < baseBindGlobals.size() ? baseBindGlobals[joint] : DirectX::XMMatrixInverse(nullptr, inverseBind));
                // Collapsed drivers are not phase-aligned and rotate around different pivots in
                // the source hierarchy. Summing their count as one angular response produces
                // extreme local rotations (especially in the three-bone tier), so retain a
                // modest compensation while keeping the compact driver stable.
                const float responseScale = std::clamp(
                    variant && joint < variant->windResponseScales.size() ? variant->windResponseScales[joint] : 1.0f,
                    1.0f,
                    2.0f);
                entry.meanBend *= responseScale;
                entry.parallelAmplitude *= responseScale;
                entry.maximumAngle *= responseScale;
                DirectX::XMStoreFloat4x4(&entry.inverseBind, inverseBind);
                DirectX::XMStoreFloat4x4(&entry.bindGlobal, bindGlobal);
                block->bones.push_back(entry);
            }
            block->types.push_back(type);
        }
        layoutSummary << ']';
        block->layoutSummary = layoutSummary.str();
        return block;
    }

    // Concatenates cached blocks for the current instance set and uploads the
    // bone/remap/lookup tables. Type rows are kept in lastTypes for bucket
    // assignment, which runs independently whenever placement counts move.
    void RebuildLayout(const br::render::PublishedPoseState& publishedPoses, std::uint64_t profileRevision)
    {
        BT_ZONE_SCOPE("ProceduralWind::UpdateTypes::Layout");
        if (typeBlockCacheProfileRevision != profileRevision) {
            typeBlockCache.clear();
            typeBlockCacheProfileRevision = profileRevision;
        }
        std::vector<WindBoneGPU> next;
        std::vector<std::uint32_t> nextRemaps;
        std::vector<WindTypeGPU> types;
        std::string layoutSummary;
        std::uint32_t registeredTypes = 0u;
        const auto& activeInstancesView = publishedPoses.activeInstances;
        std::uint32_t lookupCount = 0u;
        for (const auto& instance : activeInstancesView) if (instance.baseSkeleton && instance.baseSkeleton->HasWindSimulationGroups()) {
            lookupCount = (std::max)(lookupCount, instance.instanceSlot + 1u);
        }
        std::vector<std::uint32_t> sourceSlotToBaseType((std::max)(1u, lookupCount), 0xFFFFFFFFu);
        std::unordered_map<const Skeleton*, std::uint32_t> firstVariantByBaseSkeleton;
        std::unordered_set<const Skeleton*> liveSkeletons;
        for (const auto& instance : activeInstancesView) {
            if (!instance.baseSkeleton || !instance.baseSkeleton->HasWindSimulationGroups()) continue;
            const Skeleton* typeSkeleton = instance.baseSkeleton.get();
            if (!typeSkeleton) continue;
            if (const auto existing = firstVariantByBaseSkeleton.find(typeSkeleton);
                existing != firstVariantByBaseSkeleton.end()) {
                sourceSlotToBaseType[instance.instanceSlot] = existing->second;
                continue;
            }
            liveSkeletons.insert(typeSkeleton);
            auto& cached = typeBlockCache[typeSkeleton];
            if (!cached) cached = BuildTypeBlock(*typeSkeleton);
            const auto& block = *cached;
            const std::uint32_t firstVariant = static_cast<std::uint32_t>(types.size());
            firstVariantByBaseSkeleton.emplace(typeSkeleton, firstVariant);
            sourceSlotToBaseType[instance.instanceSlot] = firstVariant;
            const std::uint32_t boneBase = static_cast<std::uint32_t>(next.size());
            const std::uint32_t remapBase = static_cast<std::uint32_t>(nextRemaps.size());
            layoutSummary += " slot=" + std::to_string(instance.instanceSlot) + block.layoutSummary;
            for (auto type : block.types) {
                type.firstBone += boneBase;
                type.remapOffset += remapBase;
                type.sourceSkinningSlot = instance.instanceSlot;
                types.push_back(type);
                ++registeredTypes;
            }
            const std::size_t boneStart = next.size();
            next.insert(next.end(), block.bones.begin(), block.bones.end());
            for (std::size_t index = boneStart; index < next.size(); ++index) {
                auto& entry = next[index];
                entry.skinningSlot = instance.instanceSlot;
                if (entry.parentEntry != kInvalidSimulationGroup) entry.parentEntry += boneBase;
            }
            nextRemaps.insert(nextRemaps.end(), block.remaps.begin(), block.remaps.end());
        }
        // Drop blocks for skeletons that left the scene so the cache follows residency.
        for (auto it = typeBlockCache.begin(); it != typeBlockCache.end();) {
            if (liveSkeletons.contains(it->first)) ++it;
            else it = typeBlockCache.erase(it);
        }
        lastTypes = std::move(types);
        lastSourceSlotToBaseType = sourceSlotToBaseType;
        lastLookupCount = lookupCount;
        lastRegisteredTypes = registeredTypes;
        lastRequestedBoneCount = static_cast<std::uint32_t>(next.size());
        boneEntries->ReplaceData(std::move(next));
        boneRemaps->ReplaceData(std::move(nextRemaps));
        baseTypeLookup->ReplaceData(std::move(sourceSlotToBaseType));
        if (layoutSummary != lastLoggedLayoutSummary) {
            lastLoggedLayoutSummary = layoutSummary;
            EmitWindTelemetry(fmt::format("ProceduralWind skeleton LOD layouts:{}", lastLoggedLayoutSummary));
        }
    }

    void UpdateTypes(const org::UpdateExecutionContext& context)
    {
        const auto* update = context.hostData ? context.hostData->Get<UpdateContext>() : nullptr;
        if (!update || !update->windPaletteService) {
            activeBoneCount = 0u;
            return;
        }
        const auto& publishedPoses = update->poses;
        if (!publishedPoses) {
            activeBoneCount = 0u;
            return;
        }
        const auto publishedObjects = update->publishedRendererState
            ? update->publishedRendererState->drawRecords.payload
                .Get<br::render::PublishedObjectBufferState>()
            : nullptr;
        if (!publishedObjects) {
            activeBoneCount = 0u;
            return;
        }
        const auto placementVersion = publishedObjects->FindVersion(
            br::render::kObjectSkinnedPlacementVariant);
        const auto activePlacementVersion = publishedObjects->FindVersion(
            br::render::kObjectActiveSkinnedPlacementVariant);
        if (!placementVersion || !activePlacementVersion) {
            activeBoneCount = 0u;
            return;
        }
        const auto transformCount = publishedObjects->residentTransformCount;
        residentTransformCount = transformCount;
        skinnedPlacements = placementVersion->resource;
        activeSkinnedPlacements = activePlacementVersion->resource;
        activeSkinnedPlacementResidentSize = static_cast<std::uint32_t>(
            activePlacementVersion->elementCount);
        const auto& activePlacementEntries = publishedObjects->activePlacementEntries;
        const std::uint32_t placementCount = activePlacementEntries
            ? static_cast<std::uint32_t>(activePlacementEntries->size()) : 0u;
        const std::uint32_t placementCapacity = (std::max)(
            1u, activeSkinnedPlacementResidentSize);
        const std::uint64_t instanceRevision = publishedPoses->activeInstanceRevision;
        const std::uint64_t profileRevision = runtime->ProfileRevision();
        // The bone layout depends on the set of wind skeletons and the profiles;
        // placement counts only move bucket capacities. The two are updated
        // independently so streaming placements never re-derive bones.
        const bool layoutChanged =
            instanceRevision != lastActiveInstanceRevision ||
            profileRevision != lastProfileRevision;
        const bool bucketsChanged = layoutChanged ||
            transformCount != lastStructuralTransformCount ||
            placementCapacity != lastStructuralPlacementCapacity ||
            placementCount != lastStructuralPlacementCount ||
            activeSkinnedPlacements.get() != lastActivePlacementsBuffer;
        TracyPlot("ProceduralWind.StructuralRebuild", static_cast<std::int64_t>(layoutChanged ? 1 : 0));
        if (registeredTypeCount != 0u) {
            // SkeletonManager flips the current/previous transient palette halves in
            // BeginFrame. Keep this value copy current even when the expensive type
            // layout is unchanged, or compact LODs address the wrong half every other
            // frame and visibly alternate between current and stale animation poses.
            transientRegion = update->windPaletteService->ReserveTransientWindRegion(TransientBoneCapacity());
            update->windPaletteService->EnsureTransientWindInstanceSlots(transformCount);
        }
        if (!bucketsChanged) {
            AdvanceAndPublishFrame(context.deltaTime, update->proceduralWind);
            RequestTelemetryReadback();
            TracyPlot("ProceduralWind.SimulatedBones", static_cast<std::int64_t>(activeBoneCount));
            return;
        }
        lastActiveInstanceRevision = instanceRevision;
        lastProfileRevision = profileRevision;
        lastStructuralTransformCount = transformCount;
        lastStructuralPlacementCapacity = placementCapacity;
        lastStructuralPlacementCount = placementCount;
        lastActivePlacementsBuffer = activeSkinnedPlacements.get();
        if (layoutChanged) RebuildLayout(*publishedPoses, profileRevision);

        BT_ZONE_SCOPE("ProceduralWind::UpdateTypes::Buckets");
        std::vector<WindTypeGPU> types = lastTypes;
        const auto& sourceSlotToBaseType = lastSourceSlotToBaseType;
        std::vector<std::uint32_t> placementCountByFirstVariant(types.size(), 0u);
        if (activeSkinnedPlacements) {
            const auto& placementRecords = publishedObjects->placementRecords;
            const auto& activeEntries = publishedObjects->activePlacementEntries;
            if (placementRecords && activeEntries) for (const auto& activeEntry : *activeEntries) {
                if (activeEntry.drawRecordIndex >= placementRecords->size()) {
                    continue;
                }
                const auto& placement = (*placementRecords)[activeEntry.drawRecordIndex];
                if (placement.generation != activeEntry.generation ||
                    placement.skinningTypeSlot >= sourceSlotToBaseType.size()) {
                    continue;
                }
                const auto firstVariant = sourceSlotToBaseType[placement.skinningTypeSlot];
                if (firstVariant < placementCountByFirstVariant.size()) {
                    ++placementCountByFirstVariant[firstVariant];
                }
            }
        }
        std::uint64_t totalBucketCapacity = 0u;
        const std::uint32_t remapDescriptor = boneRemaps->GetSRVInfo(0).slot.index;
        const std::uint32_t baseTypeLookupDescriptor = baseTypeLookup->GetSRVInfo(0).slot.index;
        for (auto& type : types) {
            const auto firstVariant = type.sourceSkinningSlot < sourceSlotToBaseType.size()
                ? sourceSlotToBaseType[type.sourceSkinningSlot]
                : 0xFFFFFFFFu;
            const auto bucketCapacity = firstVariant < placementCountByFirstVariant.size()
                ? (std::max)(1u, placementCountByFirstVariant[firstVariant])
                : 1u;
            type.bucketBase = static_cast<std::uint32_t>(totalBucketCapacity);
            type.bucketCapacity = bucketCapacity;
            totalBucketCapacity += bucketCapacity;
            type.diagnosticsDescriptor = diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
            type.activeEntriesDescriptor = activeSkinnedPlacements ? activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u;
            type.transformCount = transformCount;
            type.deferredEntriesDescriptor = deferredEntries->GetUAVShaderVisibleInfo(0).slot.index;
            type.processedTypeCountsDescriptor = processedTypeCounts->GetUAVShaderVisibleInfo(0).slot.index;
            type.remapDescriptor = remapDescriptor;
            type.baseTypeLookupDescriptor = baseTypeLookupDescriptor;
            type.baseTypeLookupCount = lastLookupCount;
        }
        const auto boundedBucketCapacity = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            totalBucketCapacity,
            std::numeric_limits<std::uint32_t>::max()));
        const std::uint32_t registeredTypes = lastRegisteredTypes;
        windTypes->ReplaceData(std::move(types));
        typeCount = windTypes->Size();
        registeredTypeCount = registeredTypes;
        if (registeredTypes != 0u) {
            transientRegion = update->windPaletteService->ReserveTransientWindRegion(TransientBoneCapacity());
            update->windPaletteService->EnsureTransientWindInstanceSlots(transformCount);
            residentPlacementCount = activeSkinnedPlacementResidentSize;
        }
        else {
            residentPlacementCount = 0u;
        }
        typeCounters->EnsureSize(std::max(1u, typeCount));
        processedTypeCounts->EnsureSize(std::max(1u, typeCount));
        deferredEntries->EnsureSize(std::max(1u, residentPlacementCount));
        indirectCommands->EnsureSize(std::max(1u, typeCount));
        allocationRecords->EnsureSize(std::max(1u, typeCount));
        activeInstances->EnsureSize((std::max)(1u, boundedBucketCapacity));
        visibleSkeletons->EnsureSize((std::max)(1u, residentPlacementCount));
        visibleSkeletonMembership->EnsureSize((std::max)(1u, residentTransformCount));
        activeBoneCount = std::min(lastRequestedBoneCount, boneEntries->ResidentCapacity());
        TracyPlot("ProceduralWind.CandidatePlacements", static_cast<std::int64_t>(residentPlacementCount));
        TracyPlot("ProceduralWind.RegisteredTypes", static_cast<std::int64_t>(registeredTypes));
        if (registeredTypes != lastLoggedRegisteredTypes || residentPlacementCount != lastLoggedPlacementCount) {
            EmitWindTelemetry(fmt::format(
                "ProceduralWind transient: registeredVariants={} typeSlots={} typeBones={} activePlacementEntries={} matrixCapacity={} bucketEntries={} legacyBucketEntries={} distance=[{},{}]",
                registeredTypes, typeCount, activeBoneCount, residentPlacementCount,
                transientRegion.capacityMatrices, boundedBucketCapacity,
                static_cast<std::uint64_t>(typeCount) * placementCapacity,
                update->proceduralWind.innerRadius,
                update->proceduralWind.outerRadius));
            lastLoggedRegisteredTypes = registeredTypes;
            lastLoggedPlacementCount = residentPlacementCount;
        }
        AdvanceAndPublishFrame(context.deltaTime, update->proceduralWind);
        RequestTelemetryReadback();
        TracyPlot("ProceduralWind.SimulatedBones", static_cast<std::int64_t>(activeBoneCount));
    }

    std::unordered_map<const Skeleton*, std::shared_ptr<const WindTypeBlock>> typeBlockCache;
    std::uint64_t typeBlockCacheProfileRevision = ~0ull;
    std::vector<WindTypeGPU> lastTypes;
    std::vector<std::uint32_t> lastSourceSlotToBaseType;
    std::uint32_t lastLookupCount = 0u;
    std::uint32_t lastRegisteredTypes = 0u;
    std::uint32_t lastRequestedBoneCount = 0u;
    std::shared_ptr<ProceduralWindRuntime> runtime;
	org::runtime::IReadbackService* readbackService = nullptr;
    std::array<std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>>, 2> fieldSlices;
	std::shared_ptr<DynamicStructuredBuffer<DynamicWindFrameGPU>> frameState;
    std::shared_ptr<DynamicStructuredBuffer<WindBoneGPU>> boneEntries;
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> boneRemaps;
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> baseTypeLookup;
    std::vector<std::uint32_t> firstBoneByTypeSlot;
    std::uint64_t lastBoneLayoutRevision = 0u;
    std::shared_ptr<DynamicStructuredBuffer<WindTypeGPU>> windTypes;
    std::shared_ptr<DynamicStructuredBuffer<WindActiveInstanceGPU>> activeInstances;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> typeCounters;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> processedTypeCounts;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> deferredEntries;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> allocationCounters;
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> diagnostics;
    std::shared_ptr<org::GloballyIndexedResource> skinnedPlacements;
    std::shared_ptr<org::GloballyIndexedResource> activeSkinnedPlacements;
    std::shared_ptr<DynamicStructuredBuffer<WindIndirectCommand>> indirectCommands;
	std::shared_ptr<DynamicStructuredBuffer<WindAllocationRecordGPU>> allocationRecords;
	std::shared_ptr<DynamicStructuredBuffer<DynamicWindVisibleSkeletonGPU>> visibleSkeletons;
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> visibleSkeletonCounter;
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> visibleSkeletonMembership;
    std::uint64_t fieldRevision = 0u;
    bool fieldReady = false;
    std::uint32_t activeBoneCount = 0u;
    std::uint32_t typeCount = 0u;
    std::uint32_t residentPlacementCount = 0u;
    std::uint32_t activeSkinnedPlacementResidentSize = 0u;
    std::uint32_t residentTransformCount = 0u;
    std::uint32_t lastLoggedRegisteredTypes = ~0u;
    std::uint32_t lastLoggedPlacementCount = ~0u;
	std::uint32_t registeredTypeCount = 0u;
	std::uint64_t lastActiveInstanceRevision = ~0ull;
	std::uint64_t lastProfileRevision = ~0ull;
	std::uint32_t lastStructuralTransformCount = ~0u;
	std::uint32_t lastStructuralPlacementCapacity = ~0u;
	std::uint32_t lastStructuralPlacementCount = ~0u;
	const org::GloballyIndexedResource* lastActivePlacementsBuffer = nullptr;
	std::string lastLoggedLayoutSummary;
    float nextTelemetrySeconds = 2.0f;
    float elapsedSeconds = 0.0f;
	float previousElapsedSeconds = 0.0f;
    WindState state{};
    float displacementScale = 1.0f;
    ResidentWindPair residentPair{};
    br::render::TransientWindRegion transientRegion{};
};

void BindAndDispatch(org::PassExecutionContext& executionContext, const org::PipelineState& pso, const WindRootConstants& constants)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& commandList = executionContext.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(br::extensions::GetComputePipelineLayout());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
    commandList.Dispatch((constants.boneCount + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);
}

struct PreparedWindResidency {};

class WindResidencyPass final : public org::TypedRenderGraphPass<WindResidencyPass, PreparedWindResidency> {
public:
    explicit WindResidencyPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources)) {}
    void Declare(org::PassBuilder& builder) {
        builder.WithShaderResource(m_resources->fieldSlices[0], m_resources->fieldSlices[1])
            .PreferQueue(org::QueueKind::Compute);
    }
    void Initialize() {}
    void Update(const org::UpdateExecutionContext&) override { m_resources->UpdateFieldPair(); }
    PreparedWindResidency Prepare(const org::PassPrepareContext&) { return {}; }
    static void Record(const PreparedWindResidency&, org::PassRecordContext&) {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
};

void PrepareTransient(org::PassExecutionContext& context, const org::PipelineState& pso, const WindTransientConstants& constants)
{
    auto* renderContext = context.hostData->Get<RenderContext>();
    auto& commandList = context.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(br::extensions::GetComputePipelineLayout());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
}

const ProceduralWindFrameSettings& WindFrameSettings(
    const org::PassPrepareContext& preparation)
{
    static const ProceduralWindFrameSettings defaults{};
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    return context ? context->proceduralWind : defaults;
}

WindTransientConstants MakeTransientConstants(WindSharedResources& resources, org::GloballyIndexedResource* skinInfo,
    org::GloballyIndexedResource* forward, org::GloballyIndexedResource* inverse, org::GloballyIndexedResource* inverseBind,
    const ProceduralWindFrameSettings& settings)
{
    WindTransientConstants c{};
    c.types = resources.windTypes->GetSRVInfo(0).slot.index;
    c.bones = resources.boneEntries->GetSRVInfo(0).slot.index;
    c.activeInstances = resources.activeInstances->GetUAVShaderVisibleInfo(0).slot.index;
    c.typeCounters = resources.typeCounters->GetUAVShaderVisibleInfo(0).slot.index;
    c.counters = resources.allocationCounters->GetUAVShaderVisibleInfo(0).slot.index;
    c.placements = resources.skinnedPlacements ? resources.skinnedPlacements->GetSRVInfo(0).slot.index : 0u;
    c.indirectCommands = resources.indirectCommands->GetUAVShaderVisibleInfo(0).slot.index;
    c.skinningInfo = skinInfo->GetUAVShaderVisibleInfo(0).slot.index;
    c.forwardSkin = forward->GetUAVShaderVisibleInfo(0).slot.index;
    c.inverseSkin = inverse->GetUAVShaderVisibleInfo(0).slot.index;
    c.inverseBind = inverseBind->GetSRVInfo(0).slot.index;
	c.allocationRecords = resources.allocationRecords->GetUAVShaderVisibleInfo(0).slot.index;
    c.placementCount = resources.residentPlacementCount;
    c.typeCount = resources.typeCount;
    c.transformBase = resources.transientRegion.transformBaseMatrices;
    c.inverseBase = resources.transientRegion.inverseSkinBaseMatrices;
    c.matrixCapacity = resources.transientRegion.capacityMatrices;
    c.phaseAndDepthDescriptor = 0u;
    c.fieldSlice0 = resources.fieldSlices[0]->GetSRVInfo(0).slot.index;
    c.fieldSlice1 = resources.fieldSlices[1]->GetSRVInfo(0).slot.index;
    const auto& resident = resources.residentPair;
    c.fieldDimensions = (resident.metadata.width & 0xffffu) | ((resident.metadata.height & 0xffffu) << 16u);
    c.fieldCellSize = resident.metadata.cellSize;
    c.fieldOriginX = resident.metadata.origin.x;
    c.fieldOriginY = resident.metadata.origin.y;
    c.fieldInterpolation = resident.bracket.interpolation;
    c.elapsedSeconds = resources.elapsedSeconds;
    c.windX = resources.state.directionToWS.x;
    c.windY = resources.state.directionToWS.y;
    c.strength = resources.state.strength * resources.displacementScale;
    c.gustStrength = resources.state.gustStrength;
	const auto& curve = settings.skeletonLodQualityCurve;
	const std::array<float, 12> defaults{ 0.50f, 1.00f, 0.25f, 0.70f, 0.10f, 0.48f, 0.04f, 0.28f, 0.015f, 0.10f, 0.005f, 0.00f };
	for (std::size_t i = 0; i < c.qualityCurveScreen.size(); ++i) {
		c.qualityCurveScreen[i] = (std::max)(0.0f, curve.size() > i * 2u ? curve[i * 2u] : defaults[i * 2u]);
		c.qualityCurveValue[i] = std::clamp(curve.size() > i * 2u + 1u ? curve[i * 2u + 1u] : defaults[i * 2u + 1u], 0.0f, 1.0f);
	}
	c.staticCutoff = (std::max)(0.0f,
		settings.skeletonLodStaticCutoff);
	c.lodHysteresis = std::clamp(
		settings.skeletonLodHysteresis, 0.0f, 0.49f);
	c.forcedLod = std::clamp(
		settings.forcedSkeletonLod, -1, 15);
	c.capacityTarget = std::clamp(
		settings.skeletonLodCapacityTarget, 0.1f, 1.0f);
	c.lateReserve = std::clamp(
		settings.skeletonLodLateReserve, 0.0f, 0.5f);
    return c;
}

void SetVisibleSkeletonConstants(WindTransientConstants& constants, const WindSharedResources& resources)
{
	constants.qualityCurveScreen[0] = std::bit_cast<float>(
		resources.visibleSkeletons->GetUAVShaderVisibleInfo(0).slot.index);
	constants.qualityCurveScreen[1] = std::bit_cast<float>(
		resources.visibleSkeletonCounter->GetUAVShaderVisibleInfo(0).slot.index);
	constants.qualityCurveScreen[2] = std::bit_cast<float>(
		resources.visibleSkeletonMembership->GetUAVShaderVisibleInfo(0).slot.index);
	constants.qualityCurveScreen[3] = std::bit_cast<float>(resources.residentPlacementCount);
}

void SetActivationPhaseAndDepth(
    WindTransientConstants& constants,
    const RenderContext* renderContext,
    bool latePhase)
{
    constants.phaseAndDepthDescriptor = latePhase ? kLatePhaseBit : 0u;
    if (!renderContext ||
        !renderContext->proceduralWind.occlusionCullingEnabled)
        return;

    const auto view = std::ranges::find(renderContext->Views(),
        renderContext->primaryViewID, &PreparedViewFrameData::id);
    if (view == renderContext->Views().end()) return;
    const auto depthMap = latePhase
        ? view->linearDepthMap
        : (view->depthHistory ? view->depthHistory.resource : nullptr);
    if (!depthMap || depthMap->GetNumSRVSlices() == 0u) return;

    std::uint32_t slice = view->depthBufferArrayIndex >= 0
        ? static_cast<std::uint32_t>(view->depthBufferArrayIndex)
        : 0u;
    slice = (std::min)(slice, depthMap->GetNumSRVSlices() - 1u);
    constants.phaseAndDepthDescriptor |= depthMap->GetSRVInfo(0, slice).slot.index & kDepthDescriptorMask;
}

void SetActivationPhaseAndDepth(
    WindTransientConstants& constants,
    const UpdateContext* context,
    bool latePhase)
{
    constants.phaseAndDepthDescriptor = latePhase ? kLatePhaseBit : 0u;
    if (!context ||
        !context->proceduralWind.occlusionCullingEnabled)
        return;
    const auto view = std::ranges::find(context->Views(),
        context->primaryViewID, &PreparedViewFrameData::id);
    if (view == context->Views().end()) return;
    const auto depthMap = latePhase
        ? view->linearDepthMap
        : (view->depthHistory ? view->depthHistory.resource : nullptr);
    if (!depthMap || depthMap->GetNumSRVSlices() == 0u) return;
    std::uint32_t slice = view->depthBufferArrayIndex >= 0
        ? static_cast<std::uint32_t>(view->depthBufferArrayIndex) : 0u;
    slice = (std::min)(slice, depthMap->GetNumSRVSlices() - 1u);
    constants.phaseAndDepthDescriptor |= depthMap->GetSRVInfo(0, slice).slot.index & kDepthDescriptorMask;
}

class WindResetPass final : public org::TypedRenderGraphPass<WindResetPass, br::render::PreparedComputeDispatch> {
public:
    explicit WindResetPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = br::extensions::MakeComputePipeline(br::extensions::GetComputePipelineLayout(),
            L"SARPShaders/ProceduralWind.hlsl", L"ResetWindTransientCS", {}, "ProceduralWind.ResetTransient");
    }
    void Declare(org::PassBuilder& builder)
    {
		builder.WithShaderResource(m_resources->windTypes,
			m_resources->baseTypeLookup,
            Builtin::SkinnedAssemblyPlacements, Builtin::ActiveSkinnedAssemblyPlacements,
            Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->typeCounters, m_resources->allocationCounters,
                m_resources->processedTypeCounts, m_resources->deferredEntries,
                m_resources->indirectCommands, m_resources->activeInstances, m_resources->diagnostics,
				m_resources->visibleSkeletonCounter, m_resources->visibleSkeletonMembership,
                Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Initialize() {}
    void Update(const org::UpdateExecutionContext& context) override { m_resources->UpdateTypes(context); }
    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        br::render::PreparedComputeDispatch data{};
        if (!m_resources->transientRegion.valid) return data;
        auto constants = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices),
            WindFrameSettings(preparation));
        constants.bones = m_resources->diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
        constants.placementCount = m_resources->residentTransformCount;
        constants.allocationRecords = m_resources->processedTypeCounts->GetUAVShaderVisibleInfo(0).slot.index;
        SetVisibleSkeletonConstants(constants, *m_resources);
        data.layout = br::extensions::GetComputePipelineLayout();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        static_assert(sizeof(constants) <= sizeof(data.constants));
        std::memcpy(data.constants.data(), &constants, sizeof(constants));
        data.groupsX = ((std::max)({m_resources->residentTransformCount, constants.typeCount, 64u}) + 63u) / 64u;
        return data;
    }
    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }
private:
    std::shared_ptr<WindSharedResources> m_resources;
    org::PipelineState m_pso;
};

class WindActivatePass final : public org::TypedRenderGraphPass<WindActivatePass, br::render::PreparedComputeDispatch> {
public:
    WindActivatePass(std::shared_ptr<WindSharedResources> resources, bool latePhase)
        : m_resources(std::move(resources)), m_latePhase(latePhase)
    {
        m_pso = br::extensions::MakeComputePipeline(br::extensions::GetComputePipelineLayout(),
            L"SARPShaders/ProceduralWind.hlsl", L"ActivateWindInstancesCS", {}, "ProceduralWind.ActivateInstances");
    }
    void Declare(org::PassBuilder& builder)
    {
		builder.WithShaderResource(m_resources->windTypes,
			m_resources->baseTypeLookup,
            Builtin::SkinnedAssemblyPlacements, Builtin::ActiveSkinnedAssemblyPlacements,
            Builtin::PerInstanceTransformBuffer, Builtin::CameraBuffer,
            Builtin::SkeletonResources::InverseBindMatrices)
			.WithConstantBuffer(Builtin::PerFrameBuffer)
            .WithShaderResource(m_latePhase ? Builtin::PrimaryCamera::LinearDepthMap : Builtin::LastFrameLinearDepthMaps)
            .WithUnorderedAccess(m_resources->activeInstances, m_resources->typeCounters, m_resources->deferredEntries, m_resources->diagnostics,
                m_resources->allocationCounters, Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Initialize() {}
    void Update(const org::UpdateExecutionContext&) override {}
    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation)
    {
        br::render::PreparedComputeDispatch data{};
        if (!m_resources->residentPlacementCount) return data;
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto constants = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices),
            WindFrameSettings(preparation));
        if (context) {
            const auto view = std::ranges::find(context->Views(),
                context->primaryViewID, &PreparedViewFrameData::id);
            if (view != context->Views().end()) constants.cameraIndex = view->cameraBufferIndex;
        }
        constants.capacityTarget = (std::max)(0.0f,
            context ? context->proceduralWind.innerRadius : 0.0f);
        constants.lateReserve = (std::max)(constants.capacityTarget,
            context ? context->proceduralWind.outerRadius : 0.0f);
        SetActivationPhaseAndDepth(constants, context, m_latePhase);
        constants.bones = m_resources->diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
        constants.fieldSlice0 = m_resources->activeSkinnedPlacements
            ? m_resources->activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u;
        constants.fieldSlice1 = m_resources->deferredEntries->GetUAVShaderVisibleInfo(0).slot.index;
        constants.fieldDimensions = m_resources->baseTypeLookup->GetSRVInfo(0).slot.index;
        constants.allocationRecords = m_resources->baseTypeLookup->Size();
        data.layout = br::extensions::GetComputePipelineLayout();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        static_assert(sizeof(constants) <= sizeof(data.constants)); std::memcpy(data.constants.data(), &constants, sizeof(constants));
        data.groupsX = (constants.placementCount + 63u) / 64u;
        return data;
    }
    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }
private:
    std::shared_ptr<WindSharedResources> m_resources;
    org::PipelineState m_pso;
    bool m_latePhase = false;
};

class WindBuildCommandsPass final : public org::TypedRenderGraphPass<WindBuildCommandsPass, br::render::PreparedComputeDispatch> {
public:
    WindBuildCommandsPass(std::shared_ptr<WindSharedResources> r, bool latePhase)
        : m_resources(std::move(r)), m_latePhase(latePhase) {
        m_pso = br::extensions::MakeComputePipeline(br::extensions::GetComputePipelineLayout(),
            L"SARPShaders/ProceduralWind.hlsl", L"BuildWindCommandsCS", {}, "ProceduralWind.BuildCommands");
    }
    void Declare(org::PassBuilder& b) {
        b.WithShaderResource(m_resources->windTypes, m_resources->typeCounters,
                Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->processedTypeCounts, m_resources->activeInstances,
                m_resources->allocationCounters, m_resources->indirectCommands,
                m_resources->allocationRecords, Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices)
            .PreferQueue(org::QueueKind::Compute);
    }
    void Initialize() {}
    void Update(const org::UpdateExecutionContext&) override {}
    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
        br::render::PreparedComputeDispatch data{};
        if (!m_resources->typeCount) return data;
        auto constants = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices),
            WindFrameSettings(preparation));
        constants.phaseAndDepthDescriptor = m_latePhase ? kLatePhaseBit : 0u;
        constants.bones = m_resources->diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
        constants.fieldSlice0 = m_resources->processedTypeCounts->GetUAVShaderVisibleInfo(0).slot.index;
        data.layout = br::extensions::GetComputePipelineLayout();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        static_assert(sizeof(constants) <= sizeof(data.constants)); std::memcpy(data.constants.data(), &constants, sizeof(constants));
        data.groupsX = 1u;
        return data;
    }
    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }
private: std::shared_ptr<WindSharedResources> m_resources; org::PipelineState m_pso; bool m_latePhase = false;
};

class WindFinalizeAllocationsPass final : public org::TypedRenderGraphPass<WindFinalizeAllocationsPass, br::render::PreparedComputeDispatch> {
public:
	explicit WindFinalizeAllocationsPass(std::shared_ptr<WindSharedResources> resources)
		: m_resources(std::move(resources)) {
		m_pso = br::extensions::MakeComputePipeline(
			br::extensions::GetComputePipelineLayout(),
			L"SARPShaders/ProceduralWind.hlsl", L"FinalizeWindAllocationsCS", {},
			"ProceduralWind.FinalizeAllocations");
	}
	void Declare(org::PassBuilder& builder) {
		builder.WithShaderResource(m_resources->windTypes, Builtin::SkeletonResources::InverseBindMatrices)
			.WithUnorderedAccess(m_resources->allocationRecords, m_resources->activeInstances,
				m_resources->visibleSkeletons, m_resources->visibleSkeletonCounter,
				m_resources->visibleSkeletonMembership,
				Builtin::SkeletonResources::SkinningInstanceInfo,
				Builtin::SkeletonResources::BoneTransforms,
				Builtin::SkeletonResources::InverseSkinMatrices,
				m_resources->diagnostics)
			.PreferQueue(org::QueueKind::Compute);
	}
	void Initialize() {}
	void Update(const org::UpdateExecutionContext&) override {}
	br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
		br::render::PreparedComputeDispatch data{};
		if (!m_resources->typeCount || !m_resources->residentPlacementCount) return data;
		auto constants = MakeTransientConstants(*m_resources,
			m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
			m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
			m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
			m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices),
            WindFrameSettings(preparation));
		constants.bones = m_resources->diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
		constants.fieldSlice0 = m_resources->boneRemaps->GetSRVInfo(0).slot.index;
		SetVisibleSkeletonConstants(constants, *m_resources);
		data.layout = br::extensions::GetComputePipelineLayout();
		auto program = CaptureProgramBinding(preparation, m_pso);
		data.program = program.program;
		data.descriptorIndices = std::move(program.descriptorIndices);
		static_assert(sizeof(constants) <= sizeof(data.constants)); std::memcpy(data.constants.data(), &constants, sizeof(constants));
		data.groupsX = (m_resources->residentPlacementCount + kThreadsPerGroup - 1u) / kThreadsPerGroup;
		return data;
	}
	static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
		br::render::RecordPreparedComputeDispatch(data, recording);
	}
private:
	std::shared_ptr<WindSharedResources> m_resources;
	org::PipelineState m_pso;
};

class WindIndirectSimulatePass final : public org::TypedRenderGraphPass<WindIndirectSimulatePass, br::render::PreparedComputeIndirect> {
public:
    WindIndirectSimulatePass(std::shared_ptr<WindSharedResources> r, bool latePhase = false)
        : m_resources(std::move(r)), m_latePhase(latePhase) {
        m_pso = br::extensions::MakeComputePipeline(br::extensions::GetComputePipelineLayout(), L"SARPShaders/ProceduralWind.hlsl", L"SimulateWindInstancesCS", {}, "ProceduralWind.SimulateIndirect");
        rhi::IndirectArg args[] = {{.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},{.kind=rhi::IndirectArgKind::Dispatch}};
        m_signature = std::make_shared<rhi::CommandSignaturePtr>();
        br::extensions::GetRenderDevice().CreateCommandSignature({rhi::Span<rhi::IndirectArg>(args,2),sizeof(WindIndirectCommand)}, br::extensions::GetComputePipelineLayout(), *m_signature);
    }
    void Declare(org::PassBuilder& b) {
        b.WithShaderResource(m_resources->windTypes,m_resources->boneEntries,m_resources->fieldSlices[0],m_resources->fieldSlices[1],Builtin::InstanceDrawRecordBuffer,Builtin::PerInstanceTransformBuffer,Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->activeInstances,m_resources->diagnostics,Builtin::SkeletonResources::SkinningInstanceInfo,Builtin::SkeletonResources::BoneTransforms,Builtin::SkeletonResources::InverseSkinMatrices);
        m_argumentsBinding = b.BindIndirectArguments(m_resources->indirectCommands);
        m_countBinding = b.BindIndirectArguments(m_resources->allocationCounters);
        b.PreferQueue(org::QueueKind::Compute);
    }
    void Initialize() {}
    void Update(const org::UpdateExecutionContext&) override {}
    br::render::PreparedComputeIndirect Prepare(const org::PassPrepareContext& preparation) {
        br::render::PreparedComputeIndirect data{};
        if (!m_resources->typeCount) { data.enabled = false; return data; }
        auto constants = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices),
            WindFrameSettings(preparation));
        constants.phaseAndDepthDescriptor = m_latePhase ? kLatePhaseBit : 0u;
        constants.allocationRecords = m_resources->diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.commandSignature = preparation.CaptureCommandSignature(m_signature);
        data.argumentsReference = preparation.CaptureResource(m_argumentsBinding);
        data.countBufferReference = preparation.CaptureResource(m_countBinding); data.countOffset = sizeof(uint32_t);
        data.maximumCount = m_resources->typeCount;
        static_assert(sizeof(constants) <= sizeof(data.constants)); std::memcpy(data.constants.data(), &constants, sizeof(constants));
        return data;
    }
    static void Record(const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirect(data, recording);
    }
private: std::shared_ptr<WindSharedResources> m_resources; org::PipelineState m_pso; std::shared_ptr<rhi::CommandSignaturePtr> m_signature; org::ResourceBindingToken m_argumentsBinding{}, m_countBinding{}; bool m_latePhase = false;
};

struct WindSkeletonDebugFrameData {
    bool drawSkeletons = false;
    bool drawBoundingSpheres = false;
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    org::PreparedDescriptorReference target{};
    DirectX::XMUINT2 outputResolution{};
    org::PreparedProgramBinding skeletonProgram{}, sphereProgram{};
    rhi::CommandSignatureHandle signature{};
    org::PreparedResourceReference indirectCommands{}, allocationCounters{};
    std::array<uint32_t, 12> constants{};
    uint32_t typeCount = 0;
    uint32_t residentPlacementCount = 0;
};

struct WindSkeletonDebugBindings {
    org::ResourceBindingToken indirectCommands{}, allocationCounters{}, target{};
};

class WindSkeletonDebugPass final
    : public org::TypedRenderGraphPass<WindSkeletonDebugPass,
        WindSkeletonDebugFrameData, WindSkeletonDebugBindings> {
public:
    explicit WindSkeletonDebugPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        ShaderInfoBundle shaders;
        shaders.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSWindMain", L"ms_6_6" };
        shaders.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSWindMain", L"ps_6_6" };
        const auto compiled = br::extensions::CompileShaders(shaders);
        m_bindings = compiled.resourceDescriptorSlots;
        auto layout = br::extensions::GetGraphicsPipelineLayout();
        rhi::SubobjLayout soLayout{ layout };
        rhi::SubobjShader soMS{ rhi::ShaderStage::Mesh, rhi::DXIL(compiled.meshShader.Get()), "MSWindMain" };
        rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "PSWindMain" };
        rhi::RasterState raster{}; raster.fill = rhi::FillMode::Solid; raster.cull = rhi::CullMode::None;
        rhi::SubobjRaster soRaster{ raster };
        rhi::BlendState blend{}; blend.numAttachments = 1; blend.attachments[0].writeMask = rhi::ColorWriteEnable::All;
        rhi::SubobjBlend soBlend{ blend };
        rhi::DepthStencilState depth{}; depth.depthEnable = false; depth.depthWrite = false; depth.depthFunc = rhi::CompareOp::Always;
        rhi::SubobjDepth soDepth{ depth };
        rhi::RenderTargets targets{}; targets.count = 1; targets.formats[0] = rhi::Format::R8G8B8A8_UNorm;
        rhi::SubobjRTVs soTargets{ targets };
        rhi::SubobjSample soSample{ rhi::SampleDesc{1, 0} };
        rhi::SubobjPrimitiveTopology soTopology{ rhi::PrimitiveTopology::LineList };
        const rhi::PipelineStreamItem items[] = { rhi::Make(soLayout), rhi::Make(soMS), rhi::Make(soPS),
            rhi::Make(soRaster), rhi::Make(soBlend), rhi::Make(soDepth), rhi::Make(soTargets),
            rhi::Make(soSample), rhi::Make(soTopology) };
        m_pso = std::make_shared<rhi::PipelinePtr>();
        if (Failed(br::extensions::GetRenderDevice().CreatePipeline(items, static_cast<uint32_t>(std::size(items)), *m_pso)))
            throw std::runtime_error("Failed to create procedural-wind skeleton debug PSO");
        (*m_pso)->SetName("ProceduralWind.SkeletonDebug.PSO");

        ShaderInfoBundle sphereShaders;
        sphereShaders.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSWindAssemblySphereMain", L"ms_6_6" };
        sphereShaders.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSWindAssemblySphereMain", L"ps_6_6" };
        const auto compiledSphere = br::extensions::CompileShaders(sphereShaders);
        m_sphereBindings = compiledSphere.resourceDescriptorSlots;
        rhi::SubobjShader sphereMS{ rhi::ShaderStage::Mesh, rhi::DXIL(compiledSphere.meshShader.Get()), "MSWindAssemblySphereMain" };
        rhi::SubobjShader spherePS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiledSphere.pixelShader.Get()), "PSWindAssemblySphereMain" };
        rhi::RasterState sphereRaster{}; sphereRaster.fill = rhi::FillMode::Wireframe; sphereRaster.cull = rhi::CullMode::None;
        rhi::SubobjRaster sphereRasterState{ sphereRaster };
        rhi::SubobjPrimitiveTopology sphereTopology{ rhi::PrimitiveTopology::TriangleList };
        const rhi::PipelineStreamItem sphereItems[] = { rhi::Make(soLayout), rhi::Make(sphereMS), rhi::Make(spherePS),
            rhi::Make(sphereRasterState), rhi::Make(soBlend), rhi::Make(soDepth), rhi::Make(soTargets),
            rhi::Make(soSample), rhi::Make(sphereTopology) };
        m_spherePso = std::make_shared<rhi::PipelinePtr>();
        if (Failed(br::extensions::GetRenderDevice().CreatePipeline(
                sphereItems, static_cast<uint32_t>(std::size(sphereItems)), *m_spherePso)))
            throw std::runtime_error("Failed to create procedural-wind assembly-sphere debug PSO");
        (*m_spherePso)->SetName("ProceduralWind.AssemblySphereDebug.PSO");
        rhi::IndirectArg args[] = {
            {.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},
            {.kind=rhi::IndirectArgKind::DispatchMesh}
        };
        m_signature = std::make_shared<rhi::CommandSignaturePtr>();
        br::extensions::GetRenderDevice().CreateCommandSignature(
            {rhi::Span<rhi::IndirectArg>(args, 2), sizeof(WindIndirectCommand)}, layout, *m_signature);
    }
    WindSkeletonDebugBindings Declare(org::PassBuilder& declaration)
    {
        WindSkeletonDebugBindings bindings{};
        declaration.WithShaderResource(m_resources->windTypes, m_resources->boneEntries,
            m_resources->activeInstances, Builtin::SkinnedAssemblyPlacements,
            Builtin::ActiveSkinnedAssemblyPlacements,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::InstanceDrawRecordBuffer, Builtin::PerMeshInstanceBuffer,
            Builtin::PerInstanceTransformBuffer, Builtin::CameraBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer);
        bindings.indirectCommands = declaration.BindIndirectArguments(m_resources->indirectCommands);
        bindings.allocationCounters = declaration.BindIndirectArguments(m_resources->allocationCounters);
        bindings.target = declaration.BindRenderTarget(org::ResourceIdentifier{Builtin::PresentationColor});
        return bindings;
    }
    WindSkeletonDebugFrameData Prepare(const WindSkeletonDebugBindings& bindings,
        const org::PassPrepareContext& preparation) const
    {
        const auto* rc = preparation.preparationData->Get<UpdateContext>();
        const auto outputType = rc ? rc->outputType : static_cast<unsigned int>(OutputType::COLOR);
        const bool drawSkeletons = outputType == static_cast<unsigned int>(OutputType::SKELETONS);
        const bool drawBoundingSpheres = outputType == static_cast<unsigned int>(OutputType::SKELETON_BOUNDING_SPHERES);
        WindSkeletonDebugFrameData data{};
        if ((!drawSkeletons && !drawBoundingSpheres) || !m_resources->typeCount) return data;
        data.drawSkeletons = drawSkeletons;
        data.drawBoundingSpheres = drawBoundingSpheres;
        data.resourceHeap = rc->textureDescriptorHeap.GetHandle();
        data.samplerHeap = rc->samplerDescriptorHeap.GetHandle();
        data.target = preparation.CaptureView(bindings.target,
            {org::BindlessViewKind::RenderTarget});
        data.outputResolution = rc->outputResolution;
        data.typeCount = m_resources->typeCount;
        data.residentPlacementCount = m_resources->residentPlacementCount;
        preparation.Retain(m_resources);
        if (drawSkeletons) {
            data.skeletonProgram = preparation.CaptureProgramBinding(m_pso, m_bindings);
            data.signature = preparation.CaptureCommandSignature(m_signature);
            data.indirectCommands = preparation.CaptureResource(bindings.indirectCommands);
            data.allocationCounters = preparation.CaptureResource(bindings.allocationCounters);
        }
        if (drawBoundingSpheres)
            data.sphereProgram = preparation.CaptureProgramBinding(m_spherePso, m_sphereBindings);
        data.constants = {
            m_resources->windTypes->GetSRVInfo(0).slot.index, m_resources->boneEntries->GetSRVInfo(0).slot.index,
            m_resources->activeInstances->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::PerFrameBuffer)->GetCBVInfo().slot.index,
            m_resourceRegistryView->RequestPtr<org::GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index,
            0u,
            m_resources->skinnedPlacements ? m_resources->skinnedPlacements->GetSRVInfo(0).slot.index : 0u,
            m_resources->activeSkinnedPlacements ? m_resources->activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u,
            m_resources->residentPlacementCount };
        return data;
    }
    static void Record(const WindSkeletonDebugBindings&, const WindSkeletonDebugFrameData& data,
        org::PassRecordContext& recording)
    {
        if (!data.drawSkeletons && !data.drawBoundingSpheres) return;
        auto& cmd = recording.Commands();
        cmd.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
        rhi::PassBeginInfo pass{}; rhi::ColorAttachment color{};
        color.rtv = recording.Resolve(data.target); color.loadOp = rhi::LoadOp::Load; color.storeOp = rhi::StoreOp::Store;
        pass.colors = { &color }; pass.width = data.outputResolution.x; pass.height = data.outputResolution.y; pass.debugName = "Wind Skeleton Debug Overlay";
        cmd.BeginPass(pass);
        const auto program = data.drawSkeletons ? data.skeletonProgram.program : data.sphereProgram.program;
        cmd.BindLayout(recording.ResolveLayout(program));
        cmd.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0, 12, data.constants.data());
        if (data.drawSkeletons) {
            cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::LineList);
            cmd.BindPipeline(recording.Resolve(data.skeletonProgram.program));
            if (!data.skeletonProgram.descriptorIndices.empty())
                cmd.PushConstants(rhi::ShaderStage::AllGraphics, 0,
                    org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                    static_cast<uint32_t>(data.skeletonProgram.descriptorIndices.size()),
                    data.skeletonProgram.descriptorIndices.data());
            cmd.ExecuteIndirect(data.signature, recording.Resolve(data.indirectCommands).GetHandle(), 0,
                recording.Resolve(data.allocationCounters).GetHandle(), sizeof(uint32_t), data.typeCount);
        }
        if (data.drawBoundingSpheres && data.residentPlacementCount != 0u) {
            cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
            cmd.BindPipeline(recording.Resolve(data.sphereProgram.program));
            if (!data.sphereProgram.descriptorIndices.empty())
                cmd.PushConstants(rhi::ShaderStage::AllGraphics, 0,
                    org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                    static_cast<uint32_t>(data.sphereProgram.descriptorIndices.size()),
                    data.sphereProgram.descriptorIndices.data());
            cmd.DispatchMesh(data.residentPlacementCount, 1u, 1u);
        }
        cmd.EndPass();
    }
private:
    std::shared_ptr<WindSharedResources> m_resources;
    std::shared_ptr<rhi::PipelinePtr> m_pso;
    std::shared_ptr<rhi::PipelinePtr> m_spherePso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_signature;
    org::PipelineResources m_bindings;
    org::PipelineResources m_sphereBindings;
};

} // namespace

ProceduralWindExtension::ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime) : m_runtime(std::move(runtime)) {}

void ProceduralWindExtension::GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& out)
{
    auto resources = std::make_shared<WindSharedResources>(m_runtime, rg.GetReadbackService());
	rg.RegisterResource("Builtin::DynamicWind::VisibleSkeletons", resources->visibleSkeletons);
	rg.RegisterResource("Builtin::DynamicWind::VisibleSkeletonCounter", resources->visibleSkeletonCounter);
	rg.RegisterResource("Builtin::DynamicWind::VisibleSkeletonMembership", resources->visibleSkeletonMembership);
	rg.RegisterResource(DynamicWindFieldSlice0ResourceName, resources->fieldSlices[0]);
	rg.RegisterResource(DynamicWindFieldSlice1ResourceName, resources->fieldSlices[1]);
	rg.RegisterResource(DynamicWindFrameStateResourceName, resources->frameState);
    auto earlyInsertion = org::RenderGraph::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::UploadFieldPair", std::make_shared<WindResidencyPass>(resources)).At(earlyInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ResetTransient", std::make_shared<WindResetPass>(resources)).At(earlyInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ActivateInstancesPhase1", std::make_shared<WindActivatePass>(resources, false)).At(earlyInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::BuildSimulationCommandsPhase1", std::make_shared<WindBuildCommandsPass>(resources, false)).At(earlyInsertion));
	out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::FinalizeSimulationAllocationsPhase1", std::make_shared<WindFinalizeAllocationsPass>(resources)).At(earlyInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateInstancesPhase1", std::make_shared<WindIndirectSimulatePass>(resources)).At(earlyInsertion));

    auto lateInsertion = org::RenderGraph::ExternalInsertPoint::After("CLodOpaque::LinearDepthDownsamplePass1");
    lateInsertion.AlsoBefore("CLodOpaque::HierarchicalCullingPass2");
    lateInsertion.AlsoBefore("CLodShadow::HierarchicalCullingPass1");
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ActivateInstancesPhase2", std::make_shared<WindActivatePass>(resources, true)).At(lateInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::BuildSimulationCommandsPhase2", std::make_shared<WindBuildCommandsPass>(resources, true)).At(lateInsertion));
	out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::FinalizeSimulationAllocationsPhase2", std::make_shared<WindFinalizeAllocationsPass>(resources)).At(lateInsertion));
    out.push_back(org::RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateInstancesPhase2", std::make_shared<WindIndirectSimulatePass>(resources, true)).At(lateInsertion));
    auto debugInsertion = org::RenderGraph::ExternalInsertPoint::After("TonemappingPass");
    // Keep debug composition before the scene output becomes presentation-ready.
    // final backbuffer access so the enhanced RENDER_TARGET -> PRESENT
    // transition is not undone before IDXGISwapChain::Present.
    debugInsertion.AlsoBefore("PresentationReadyPass");
    out.push_back(org::RenderGraph::ExternalPassDesc::Render(
        "ProceduralWind::DebugActiveSkeletons",
        std::make_shared<WindSkeletonDebugPass>(resources))
        .At(debugInsertion));
}

} // namespace br::wind
