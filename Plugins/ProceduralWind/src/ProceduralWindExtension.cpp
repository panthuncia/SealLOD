#include "ProceduralWind/ProceduralWindExtension.h"

#include "Animation/Skeleton.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ObjectManager.h"
#include "Managers/SkeletonManager.h"
#include "Managers/ViewManager.h"
#include "Render/BuiltinResources.h"
#include "Render/IndirectCommand.h"
#include "Render/PassBuilders.h"
#include "Render/RenderContext.h"
#include "Render/OutputTypes.h"
#include "Render/RendererSettings.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/PixelBuffer.h"
#include "Render/Runtime/IReadbackService.h"
#include "ShaderBuffers.h"

#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace br::wind {
namespace {

constexpr std::uint32_t kThreadsPerGroup = 64u;
constexpr std::uint32_t kWindLodVariantCount = 6u;
constexpr std::uint32_t kTransientBoneCapacity = 1048576;
constexpr std::uint32_t kLatePhaseBit = 0x80000000u;
constexpr std::uint32_t kDepthDescriptorMask = 0x7fffffffu;
constexpr std::uint32_t kWindBoneFlagTrunk = 1u << 0u;

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
};

struct WindActiveInstanceGPU {
    std::uint32_t instanceTransformIndex = 0u;
    std::uint32_t stableSceneId = 0u;
    std::uint32_t transformOffsetMatrices = 0u;
    std::uint32_t inverseSkinOffsetMatrices = 0u;
};

struct WindIndirectCommand {
    std::uint32_t typeId = 0u;
    std::uint32_t pad0 = 0u;
    std::uint32_t pad1 = 0u;
    D3D12_DISPATCH_ARGUMENTS dispatch{};
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
	std::array<float, 6> lodThresholds;
	float lodHysteresis;
	std::int32_t forcedLod;
};

static_assert(sizeof(WindBoneGPU) == 256u);
static_assert(sizeof(WindTypeGPU) == 64u);
static_assert(sizeof(WindRootConstants) % sizeof(std::uint32_t) == 0u);

struct WindSharedResources {
    explicit WindSharedResources(std::shared_ptr<ProceduralWindRuntime> runtimeIn, rg::runtime::IReadbackService* readbackServiceIn)
        : runtime(std::move(runtimeIn))
		, readbackService(readbackServiceIn)
        , fieldSlices{ DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice0"),
                       DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.FieldSlice1") }
        , boneEntries(DynamicStructuredBuffer<WindBoneGPU>::CreateShared(1u, "ProceduralWind.BoneEntries"))
		, boneRemaps(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.BoneRemaps"))
		, baseTypeLookup(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.BaseTypeBySkeletonSlot"))
        , windTypes(DynamicStructuredBuffer<WindTypeGPU>::CreateShared(1u, "ProceduralWind.Types"))
        , activeInstances(DynamicStructuredBuffer<WindActiveInstanceGPU>::CreateShared(1u, "ProceduralWind.ActiveInstances", true))
        , typeCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.TypeCounters", true))
        , processedTypeCounts(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.ProcessedTypeCounts", true))
        , deferredEntries(DynamicStructuredBuffer<std::uint32_t>::CreateShared(1u, "ProceduralWind.DeferredEntries", true))
        , allocationCounters(DynamicStructuredBuffer<std::uint32_t>::CreateShared(15u, "ProceduralWind.AllocationCounters", true))
		, diagnostics(DynamicStructuredBuffer<std::uint32_t>::CreateShared(64u, "ProceduralWind.Diagnostics", true))
		, indirectCommands(DynamicStructuredBuffer<WindIndirectCommand>::CreateShared(1u, "ProceduralWind.IndirectCommands", true))
    {
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
        readbackService->RequestReadbackCapture("ProceduralWind::SimulateInstancesPhase2", allocationCounters.get(), {},
            [](ReadbackCaptureResult&& result) {
                if (result.data.size() < 15u * sizeof(std::uint32_t)) return;
                std::array<std::uint32_t, 15> c{};
                std::memcpy(c.data(), result.data.data(), (std::min)(result.data.size(), sizeof(c)));
				EmitWindTelemetry(fmt::format(
					"ProceduralWind GPU telemetry: allocatedBones={} commands={} capacityRejects={} bucketOverflow={} allocatedAssemblies={} livePlacements={} stalePlacements={} frustumRejected={} distanceRejected={} visibleBucketed={} deferred={} deferredWritten={} lateOccluded={} lateAccepted={} deferredOverflow={}.",
					c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14]));
            }, QueueKind::Copy);
        const float currentScale = displacementScale;
        const float currentStrength = state.strength;
        readbackService->RequestReadbackCapture("ProceduralWind::SimulateInstancesPhase2", diagnostics.get(), {},
            [currentScale, currentStrength](ReadbackCaptureResult&& result) {
				if (result.data.size() < 64u * sizeof(std::uint32_t)) return;
				std::array<std::uint32_t, 64> d{};
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
				EmitWindTelemetry(fmt::format(
					"ProceduralWind skeleton LOD telemetry: desired=[{},{},{},{},{},{},static={}] actual=[{},{},{},{},{},{}] "
					"transitions={} staticFallbacks={} duplicateAllocations={} duplicateStableId={} duplicateLods={}->{} duplicateTransform={} "
					"missingVariant={} invalidSourceJoint={} invalidParent={} invalidBaseTypeLookup={} invalidPaletteWrites={} simulatedWrites=[{},{},{},{},{},{}] maxDriverDepth={} maxPaletteOffset={}.",
					d[32], d[33], d[34], d[35], d[36], d[37], d[38],
					d[24], d[25], d[26], d[27], d[28], d[29], d[30], d[31],
					d[39], d[40], d[41] & 0xffffu, d[41] >> 16u, d[42], d[43], d[44], d[45], d[46], d[47],
					d[48], d[49], d[50], d[51], d[52], d[53], d[54], d[55]));
            }, QueueKind::Copy);
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

    void UpdateTypes(const UpdateExecutionContext& context)
    {
        const auto* update = context.hostData ? context.hostData->Get<UpdateContext>() : nullptr;
        if (!update || !update->skeletonManager) {
            activeBoneCount = 0u;
            return;
        }
        const auto transformCount = update->objectManager ? static_cast<std::uint32_t>(update->objectManager->GetResidentInstanceTransformCount()) : 0u;
        residentTransformCount = transformCount;
        if (update->objectManager) {
            skinnedPlacements = update->objectManager->GetSkinnedAssemblyPlacements();
            activeSkinnedPlacements = update->objectManager->GetActiveSkinnedAssemblyPlacements();
        }
        std::vector<WindBoneGPU> next;
		std::vector<std::uint32_t> nextRemaps;
        std::vector<WindTypeGPU> types;
		std::ostringstream layoutSummary;
        std::uint32_t registeredTypes = 0u;
		const std::uint32_t placementCapacity = (std::max)(1u, activeSkinnedPlacements
			? static_cast<std::uint32_t>(activeSkinnedPlacements->ResidentSize()) : 0u);
		const auto activeInstancesView = update->skeletonManager->GetActiveInstanceViews();
		std::uint32_t baseTypeCount = 0u;
		std::uint32_t lookupCount = 0u;
		for (const auto& instance : activeInstancesView) if (instance.skeleton && instance.skeleton->HasWindSimulationGroups()) {
			++baseTypeCount;
			lookupCount = (std::max)(lookupCount, instance.instanceSlot + 1u);
		}
		std::vector<std::uint32_t> sourceSlotToBaseType((std::max)(1u, lookupCount), 0xFFFFFFFFu);
		types.resize(baseTypeCount * kWindLodVariantCount);
		std::uint32_t baseTypeIndex = 0u;
		for (const auto& instance : activeInstancesView) {
            if (!instance.skeleton || !instance.skeleton->HasWindSimulationGroups()) continue;
			sourceSlotToBaseType[instance.instanceSlot] = baseTypeIndex;
            const auto& authoredWind = instance.skeleton->GetDynamicWindMetadata();
            const auto profile = runtime->ResolveProfile(instance.skeleton->GetWindProfileIdentity());
			const auto lodVariants = instance.skeleton->GetSkeletonLodVariants();
			const std::uint32_t availableVariants = lodVariants.empty() ? 1u : (std::min)(kWindLodVariantCount, static_cast<std::uint32_t>(lodVariants.size()));
			layoutSummary << " slot=" << instance.instanceSlot << " source=" << instance.boneCount << " lods=[";
			for (std::uint32_t lod = 0u; lod < availableVariants; ++lod) {
				const SkeletonLodVariant* variant = lodVariants.empty() ? nullptr : &lodVariants[lod];
				const auto baseGroups = instance.skeleton->GetWindSimulationGroupIndices();
				const auto baseParents = instance.skeleton->GetParentIndices();
				const auto baseInvariants = instance.skeleton->GetWindBoneInvariants();
				const auto baseBindGlobals = instance.skeleton->GetBindGlobalMatrices();
				const auto baseInverseBinds = instance.skeleton->GetInverseBindMatrices();
				const std::uint32_t boneCount = variant ? static_cast<std::uint32_t>(variant->lodToBaseBone.size()) : instance.boneCount;
				if (lod != 0u) layoutSummary << ',';
				layoutSummary << boneCount;
				if (variant) {
					std::uint32_t invalidRemaps = 0u;
					std::uint32_t invalidParents = 0u;
					for (const auto mapped : variant->baseToLodBone) invalidRemaps += mapped >= boneCount ? 1u : 0u;
					for (std::uint32_t compact = 0u; compact < variant->parentIndices.size(); ++compact) {
						const auto parent = variant->parentIndices[compact];
						invalidParents += parent >= 0 && static_cast<std::uint32_t>(parent) >= compact ? 1u : 0u;
					}
					const bool aligned = variant->level == lod && variant->baseToLodBone.size() == instance.boneCount &&
						variant->parentIndices.size() == boneCount && variant->inverseBindMatrices.size() == boneCount &&
						variant->bindGlobalMatrices.size() == boneCount && variant->windSimulationGroupIndices.size() == boneCount;
					if (!aligned || invalidRemaps != 0u || invalidParents != 0u) {
						spdlog::error(
							"ProceduralWind invalid skeleton LOD: slot={} lod={} sourceBones={} compactBones={} aligned={} invalidRemaps={} invalidParents={}.",
							instance.instanceSlot, lod, instance.boneCount, boneCount, aligned, invalidRemaps, invalidParents);
					}
				}
				const std::uint32_t typeId = lod * baseTypeCount + baseTypeIndex;
				auto& type = types[typeId];
				type.firstBone = static_cast<std::uint32_t>(next.size());
				type.boneCount = boneCount;
				type.sourceSkinningSlot = instance.instanceSlot;
				type.bucketBase = typeId * placementCapacity;
				type.bucketCapacity = placementCapacity;
				type.diagnosticsDescriptor = diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
				type.activeEntriesDescriptor = activeSkinnedPlacements ? activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u;
				type.transformCount = transformCount;
				type.deferredEntriesDescriptor = deferredEntries->GetUAVShaderVisibleInfo(0).slot.index;
				type.processedTypeCountsDescriptor = processedTypeCounts->GetUAVShaderVisibleInfo(0).slot.index;
				type.remapOffset = static_cast<std::uint32_t>(nextRemaps.size());
				type.sourceBoneCount = instance.boneCount;
				type.lodLevel = lod;
				if (variant) nextRemaps.insert(nextRemaps.end(), variant->baseToLodBone.begin(), variant->baseToLodBone.end());
				else for (std::uint32_t bone = 0; bone < instance.boneCount; ++bone) nextRemaps.push_back(bone);
				++registeredTypes;
				for (std::uint32_t joint = 0u; joint < boneCount; ++joint) {
                WindBoneGPU entry{};
                entry.skinningSlot = instance.instanceSlot;
				const std::uint32_t sourceJoint = variant ? variant->lodToBaseBone[joint] : joint;
                entry.jointIndex = sourceJoint;
                // Keep every bone in an authored chain on the same harmonic phase.
                // Independent per-joint phases are especially destructive on the
                // densely segmented trunk: torsion barely moves the debug joint
                // origins, but alternating rotations make the skinned surface wavy.
				const auto groups = variant ? std::span<const std::uint32_t>(variant->windSimulationGroupIndices) : baseGroups;
				const auto parents = variant ? std::span<const std::int32_t>(variant->parentIndices) : baseParents;
				const auto invariants = variant ? std::span<const SkeletonWindBoneInvariant>(variant->windBoneInvariants) : baseInvariants;
				std::uint32_t phaseJoint = sourceJoint;
                if (authoredWind.enabled && sourceJoint < authoredWind.bones.size()) {
                    const std::uint32_t chainOrigin = authoredWind.bones[sourceJoint].chainOriginBoneIndex;
                    if (chainOrigin != 0xFFFFFFFFu && chainOrigin < instance.boneCount) {
                        phaseJoint = chainOrigin;
					}
				}
                entry.phaseSeed = instance.instanceSlot * 747796405u +
					(phaseJoint < baseInvariants.size()
						? baseInvariants[phaseJoint].phaseSeed
                        : phaseJoint * 2891336453u + 277803737u);
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
                next.push_back(entry);
				}
			}
			layoutSummary << ']';
			++baseTypeIndex;
        }
        if (!types.empty()) {
            // Shaders use slot zero as shared control metadata even when the first
            // registered wind skeleton occupies a later persistent instance slot.
            auto& control = types.front();
            control.diagnosticsDescriptor = diagnostics->GetUAVShaderVisibleInfo(0).slot.index;
            control.activeEntriesDescriptor = activeSkinnedPlacements ? activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u;
            control.transformCount = transformCount;
            control.deferredEntriesDescriptor = deferredEntries->GetUAVShaderVisibleInfo(0).slot.index;
            control.processedTypeCountsDescriptor = processedTypeCounts->GetUAVShaderVisibleInfo(0).slot.index;
        }
		const auto requestedBoneCount = static_cast<std::uint32_t>(next.size());
		boneEntries->ReplaceData(std::move(next));
		boneRemaps->ReplaceData(std::move(nextRemaps));
		baseTypeLookup->ReplaceData(std::move(sourceSlotToBaseType));
		const std::uint32_t remapDescriptor = boneRemaps->GetSRVInfo(0).slot.index;
		const std::uint32_t baseTypeLookupDescriptor = baseTypeLookup->GetSRVInfo(0).slot.index;
		for (auto& type : types) {
			type.remapDescriptor = remapDescriptor;
			type.baseTypeLookupDescriptor = baseTypeLookupDescriptor;
			type.baseTypeLookupCount = lookupCount;
		}
        windTypes->ReplaceData(std::move(types));
        typeCount = windTypes->Size();
		registeredTypeCount = registeredTypes;
        if (registeredTypes != 0u) {
            transientRegion = update->skeletonManager->ReserveTransientWindRegion(kTransientBoneCapacity);
            update->skeletonManager->EnsureTransientWindInstanceSlots(transformCount);
            residentPlacementCount = activeSkinnedPlacements
                ? static_cast<std::uint32_t>(activeSkinnedPlacements->ResidentSize())
                : 0u;
        }
        else {
            residentPlacementCount = 0u;
        }
        typeCounters->EnsureSize(std::max(1u, typeCount));
        processedTypeCounts->EnsureSize(std::max(1u, typeCount));
        deferredEntries->EnsureSize(std::max(1u, residentPlacementCount));
        indirectCommands->EnsureSize(std::max(1u, typeCount));
		activeInstances->EnsureSize((std::max)(1u, typeCount * placementCapacity));
        activeBoneCount = std::min(requestedBoneCount, boneEntries->ResidentCapacity());
        TracyPlot("ProceduralWind.CandidatePlacements", static_cast<std::int64_t>(residentPlacementCount));
        TracyPlot("ProceduralWind.RegisteredTypes", static_cast<std::int64_t>(registeredTypes));
        if (registeredTypes != lastLoggedRegisteredTypes || residentPlacementCount != lastLoggedPlacementCount) {
			EmitWindTelemetry(fmt::format(
				"ProceduralWind transient: registeredVariants={} typeSlots={} typeBones={} activePlacementEntries={} matrixCapacity={} bucketCapacityPerVariant={} distance=32768",
                registeredTypes, typeCount, activeBoneCount, residentPlacementCount,
				transientRegion.capacityMatrices, placementCapacity));
            lastLoggedRegisteredTypes = registeredTypes;
            lastLoggedPlacementCount = residentPlacementCount;
        }
		if (layoutSummary.str() != lastLoggedLayoutSummary) {
			lastLoggedLayoutSummary = layoutSummary.str();
			EmitWindTelemetry(fmt::format("ProceduralWind skeleton LOD layouts:{}", lastLoggedLayoutSummary));
		}
        elapsedSeconds += context.deltaTime;
        state = runtime->SnapshotWindState();
		displacementScale = std::clamp(
			SettingsManager::GetInstance().getSettingGetter<float>(ProceduralWindDisplacementScaleSettingName)(),
			0.0f,
			100.0f);
		RequestTelemetryReadback();
        TracyPlot("ProceduralWind.SimulatedBones", static_cast<std::int64_t>(activeBoneCount));
    }

    std::shared_ptr<ProceduralWindRuntime> runtime;
	rg::runtime::IReadbackService* readbackService = nullptr;
    std::array<std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>>, 2> fieldSlices;
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
    std::shared_ptr<DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>> skinnedPlacements;
    std::shared_ptr<SortedUnsignedIntBuffer> activeSkinnedPlacements;
    std::shared_ptr<DynamicStructuredBuffer<WindIndirectCommand>> indirectCommands;
    std::uint64_t fieldRevision = 0u;
    bool fieldReady = false;
    std::uint32_t activeBoneCount = 0u;
    std::uint32_t typeCount = 0u;
    std::uint32_t residentPlacementCount = 0u;
    std::uint32_t residentTransformCount = 0u;
    std::uint32_t lastLoggedRegisteredTypes = ~0u;
    std::uint32_t lastLoggedPlacementCount = ~0u;
	std::uint32_t registeredTypeCount = 0u;
	std::string lastLoggedLayoutSummary;
    float nextTelemetrySeconds = 2.0f;
    float elapsedSeconds = 0.0f;
    WindState state{};
    float displacementScale = 1.0f;
    ResidentWindPair residentPair{};
    SkeletonManager::TransientWindRegion transientRegion{};
};

void BindAndDispatch(PassExecutionContext& executionContext, const PipelineState& pso, const WindRootConstants& constants)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& commandList = executionContext.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
    commandList.Dispatch((constants.boneCount + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);
}

class WindResidencyPass final : public ComputePass {
public:
    explicit WindResidencyPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources)) {}
    void DeclareResourceUsages(ComputePassBuilder* builder) override { builder->WithShaderResource(m_resources->fieldSlices[0], m_resources->fieldSlices[1]); }
    void Setup() override {}
    void Update(const UpdateExecutionContext&) override { m_resources->UpdateFieldPair(); }
    PassReturn Execute(PassExecutionContext&) override { return {}; }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
};

void PrepareTransient(PassExecutionContext& context, const PipelineState& pso, const WindTransientConstants& constants)
{
    auto* renderContext = context.hostData->Get<RenderContext>();
    auto& commandList = context.commandList;
    commandList.SetDescriptorHeaps(renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        sizeof(constants) / sizeof(std::uint32_t), reinterpret_cast<const std::uint32_t*>(&constants));
}

WindTransientConstants MakeTransientConstants(WindSharedResources& resources, GloballyIndexedResource* skinInfo,
    GloballyIndexedResource* forward, GloballyIndexedResource* inverse, GloballyIndexedResource* inverseBind)
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
	const auto thresholds = SettingsManager::GetInstance().getSettingGetter<std::vector<float>>(ProceduralWindSkeletonLodThresholdsSettingName)();
	const std::array<float, 6> defaults{ 512.0f, 256.0f, 128.0f, 64.0f, 32.0f, 12.0f };
	for (std::size_t i = 0; i < c.lodThresholds.size(); ++i)
		c.lodThresholds[i] = i < thresholds.size() ? (std::max)(0.0f, thresholds[i]) : defaults[i];
	c.lodHysteresis = std::clamp(
		SettingsManager::GetInstance().getSettingGetter<float>(ProceduralWindSkeletonLodHysteresisSettingName)(), 0.0f, 0.49f);
	c.forcedLod = std::clamp(
		SettingsManager::GetInstance().getSettingGetter<int32_t>(ProceduralWindForcedSkeletonLodSettingName)(), -1, 6);
    return c;
}

void SetActivationPhaseAndDepth(
    WindTransientConstants& constants,
    const RenderContext* renderContext,
    bool latePhase)
{
    constants.phaseAndDepthDescriptor = latePhase ? kLatePhaseBit : 0u;
    if (!renderContext || !renderContext->viewManager ||
        !SettingsManager::GetInstance().getSettingGetter<bool>("enableOcclusionCulling")())
        return;

    const auto* view = renderContext->viewManager->Get(renderContext->primaryViewID);
    if (!view) return;
    const auto depthMap = latePhase
        ? view->gpu.linearDepthMap
        : (view->gpu.lastFrameLinearDepthValid ? view->gpu.lastFrameLinearDepthMap : nullptr);
    if (!depthMap || depthMap->GetNumSRVSlices() == 0u) return;

    std::uint32_t slice = view->cameraInfo.depthBufferArrayIndex >= 0
        ? static_cast<std::uint32_t>(view->cameraInfo.depthBufferArrayIndex)
        : 0u;
    slice = (std::min)(slice, depthMap->GetNumSRVSlices() - 1u);
    constants.phaseAndDepthDescriptor |= depthMap->GetSRVInfo(0, slice).slot.index & kDepthDescriptorMask;
}

class WindResetPass final : public ComputePass {
public:
    explicit WindResetPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"ResetWindTransientCS", {}, "ProceduralWind.ResetTransient");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->windTypes,
			m_resources->baseTypeLookup,
            Builtin::SkinnedAssemblyPlacements, Builtin::ActiveSkinnedAssemblyPlacements,
            Builtin::SkeletonResources::InverseBindMatrices)
            .WithUnorderedAccess(m_resources->typeCounters, m_resources->allocationCounters,
                m_resources->processedTypeCounts, m_resources->deferredEntries,
                m_resources->indirectCommands, m_resources->activeInstances, m_resources->diagnostics,
                Builtin::SkeletonResources::SkinningInstanceInfo,
                Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices);
    }
    void Setup() override {}
    void Update(const UpdateExecutionContext& context) override { m_resources->UpdateTypes(context); }
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->transientRegion.valid) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        PrepareTransient(context, m_pso, c);
        BindResourceDescriptorIndices(context.commandList, m_pso.GetResourceDescriptorSlots());
        context.commandList.Dispatch(((std::max)({ m_resources->residentTransformCount, c.typeCount, 64u }) + 63u) / 64u, 1u, 1u);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
};

class WindActivatePass final : public ComputePass {
public:
    WindActivatePass(std::shared_ptr<WindSharedResources> resources, bool latePhase)
        : m_resources(std::move(resources)), m_latePhase(latePhase)
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"ActivateWindInstancesCS", {}, "ProceduralWind.ActivateInstances");
    }
    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(m_resources->windTypes,
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
    void Setup() override {}
    void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override
    {
        if (!m_resources->residentPlacementCount) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        const auto* rc = context.hostData->Get<RenderContext>();
        if (rc && rc->viewManager) {
            if (const auto* view = rc->viewManager->Get(rc->primaryViewID)) c.cameraIndex = view->gpu.cameraBufferIndex;
        }
        SetActivationPhaseAndDepth(c, rc, m_latePhase);
        PrepareTransient(context, m_pso, c);
        BindResourceDescriptorIndices(context.commandList, m_pso.GetResourceDescriptorSlots());
        context.commandList.Dispatch((c.placementCount + 63u) / 64u, 1u, 1u);
        return {};
    }
    void Cleanup() override {}
private:
    std::shared_ptr<WindSharedResources> m_resources;
    PipelineState m_pso;
    bool m_latePhase = false;
};

class WindBuildCommandsPass final : public ComputePass {
public:
    WindBuildCommandsPass(std::shared_ptr<WindSharedResources> r, bool latePhase)
        : m_resources(std::move(r)), m_latePhase(latePhase) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"SARPShaders/ProceduralWind.hlsl", L"BuildWindCommandsCS", {}, "ProceduralWind.BuildCommands");
    }
    void DeclareResourceUsages(ComputePassBuilder* b) override { b->WithShaderResource(m_resources->windTypes, m_resources->typeCounters, Builtin::SkeletonResources::InverseBindMatrices).WithUnorderedAccess(m_resources->processedTypeCounts, m_resources->activeInstances, m_resources->allocationCounters, m_resources->indirectCommands, Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseSkinMatrices); }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override {
        if (!m_resources->typeCount) return {};
        auto c = MakeTransientConstants(*m_resources,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        c.phaseAndDepthDescriptor = m_latePhase ? kLatePhaseBit : 0u;
        PrepareTransient(context, m_pso, c);
        BindResourceDescriptorIndices(context.commandList, m_pso.GetResourceDescriptorSlots());
        context.commandList.Dispatch(1u, 1u, 1u); return {};
    }
    void Cleanup() override {}
private: std::shared_ptr<WindSharedResources> m_resources; PipelineState m_pso; bool m_latePhase = false;
};

class WindIndirectSimulatePass final : public ComputePass {
public:
    WindIndirectSimulatePass(std::shared_ptr<WindSharedResources> r, bool latePhase = false)
        : m_resources(std::move(r)), m_latePhase(latePhase) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(PSOManager::GetInstance().GetComputeRootSignature().GetHandle(), L"SARPShaders/ProceduralWind.hlsl", L"SimulateWindInstancesCS", {}, "ProceduralWind.SimulateIndirect");
        rhi::IndirectArg args[] = {{.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},{.kind=rhi::IndirectArgKind::Dispatch}};
        DeviceManager::GetInstance().GetDevice().CreateCommandSignature({rhi::Span<rhi::IndirectArg>(args,2),sizeof(WindIndirectCommand)}, PSOManager::GetInstance().GetComputeRootSignature().GetHandle(), m_signature);
    }
    void DeclareResourceUsages(ComputePassBuilder* b) override { b->WithShaderResource(m_resources->windTypes,m_resources->boneEntries,m_resources->activeInstances,m_resources->fieldSlices[0],m_resources->fieldSlices[1],Builtin::InstanceDrawRecordBuffer,Builtin::PerInstanceTransformBuffer,Builtin::SkeletonResources::InverseBindMatrices).WithUnorderedAccess(m_resources->diagnostics,Builtin::SkeletonResources::SkinningInstanceInfo,Builtin::SkeletonResources::BoneTransforms,Builtin::SkeletonResources::InverseSkinMatrices).WithIndirectArguments(m_resources->indirectCommands,m_resources->allocationCounters); }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override {
        if (!m_resources->typeCount) return {};
        auto c=MakeTransientConstants(*m_resources,m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseSkinMatrices),m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices));
        c.phaseAndDepthDescriptor = m_latePhase ? kLatePhaseBit : 0u;
        auto* rc=context.hostData->Get<RenderContext>(); auto& cmd=context.commandList; cmd.SetDescriptorHeaps(rc->textureDescriptorHeap.GetHandle(),rc->samplerDescriptorHeap.GetHandle()); cmd.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle()); cmd.BindPipeline(m_pso.GetAPIPipelineState().GetHandle()); BindResourceDescriptorIndices(cmd, m_pso.GetResourceDescriptorSlots()); cmd.PushConstants(rhi::ShaderStage::Compute,0,MiscUintRootSignatureIndex,0,sizeof(c)/4,reinterpret_cast<const uint32_t*>(&c)); cmd.ExecuteIndirect(m_signature->GetHandle(),m_resources->indirectCommands->GetAPIResource().GetHandle(),0,m_resources->allocationCounters->GetAPIResource().GetHandle(),sizeof(uint32_t),m_resources->typeCount); return {};
    }
    void Cleanup() override { m_signature.Reset(); }
private: std::shared_ptr<WindSharedResources> m_resources; PipelineState m_pso; rhi::CommandSignaturePtr m_signature; bool m_latePhase = false;
};

class WindSkeletonDebugPass final : public RenderPass {
public:
    explicit WindSkeletonDebugPass(std::shared_ptr<WindSharedResources> resources) : m_resources(std::move(resources))
    {
        ShaderInfoBundle shaders;
        shaders.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSWindMain", L"ms_6_6" };
        shaders.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSWindMain", L"ps_6_6" };
        const auto compiled = PSOManager::GetInstance().CompileShaders(shaders);
        m_bindings = compiled.resourceDescriptorSlots;
        auto& layout = PSOManager::GetInstance().GetRootSignature();
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
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
        if (Failed(DeviceManager::GetInstance().GetDevice().CreatePipeline(items, static_cast<uint32_t>(std::size(items)), m_pso)))
            throw std::runtime_error("Failed to create procedural-wind skeleton debug PSO");
        m_pso->SetName("ProceduralWind.SkeletonDebug.PSO");

        ShaderInfoBundle sphereShaders;
        sphereShaders.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSWindAssemblySphereMain", L"ms_6_6" };
        sphereShaders.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSWindAssemblySphereMain", L"ps_6_6" };
        const auto compiledSphere = PSOManager::GetInstance().CompileShaders(sphereShaders);
        m_sphereBindings = compiledSphere.resourceDescriptorSlots;
        rhi::SubobjShader sphereMS{ rhi::ShaderStage::Mesh, rhi::DXIL(compiledSphere.meshShader.Get()), "MSWindAssemblySphereMain" };
        rhi::SubobjShader spherePS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiledSphere.pixelShader.Get()), "PSWindAssemblySphereMain" };
        rhi::RasterState sphereRaster{}; sphereRaster.fill = rhi::FillMode::Wireframe; sphereRaster.cull = rhi::CullMode::None;
        rhi::SubobjRaster sphereRasterState{ sphereRaster };
        rhi::SubobjPrimitiveTopology sphereTopology{ rhi::PrimitiveTopology::TriangleList };
        const rhi::PipelineStreamItem sphereItems[] = { rhi::Make(soLayout), rhi::Make(sphereMS), rhi::Make(spherePS),
            rhi::Make(sphereRasterState), rhi::Make(soBlend), rhi::Make(soDepth), rhi::Make(soTargets),
            rhi::Make(soSample), rhi::Make(sphereTopology) };
        if (Failed(DeviceManager::GetInstance().GetDevice().CreatePipeline(
                sphereItems, static_cast<uint32_t>(std::size(sphereItems)), m_spherePso)))
            throw std::runtime_error("Failed to create procedural-wind assembly-sphere debug PSO");
        m_spherePso->SetName("ProceduralWind.AssemblySphereDebug.PSO");
        rhi::IndirectArg args[] = {
            {.kind=rhi::IndirectArgKind::Constant,.u={.rootConstants={IndirectCommandSignatureRootSignatureIndex,0,3}}},
            {.kind=rhi::IndirectArgKind::DispatchMesh}
        };
        DeviceManager::GetInstance().GetDevice().CreateCommandSignature(
            {rhi::Span<rhi::IndirectArg>(args, 2), sizeof(WindIndirectCommand)}, layout.GetHandle(), m_signature);
    }
    void DeclareResourceUsages(RenderPassBuilder* b) override
    {
        b->WithShaderResource(m_resources->windTypes, m_resources->boneEntries, m_resources->activeInstances,
            Builtin::SkinnedAssemblyPlacements, Builtin::ActiveSkinnedAssemblyPlacements,
            Builtin::SkeletonResources::BoneTransforms, Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::SkinningInstanceInfo, Builtin::InstanceDrawRecordBuffer,
            Builtin::PerMeshInstanceBuffer, Builtin::PerInstanceTransformBuffer, Builtin::CameraBuffer)
            .WithConstantBuffer(Builtin::PerFrameBuffer)
            .WithIndirectArguments(m_resources->indirectCommands, m_resources->allocationCounters)
            .WithRenderTarget(Builtin::Backbuffer);
    }
    void Setup() override {} void Update(const UpdateExecutionContext&) override {}
    PassReturn Execute(PassExecutionContext& context) override
    {
        const auto outputType = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")();
        const bool drawSkeletons = outputType == static_cast<unsigned int>(OutputType::SKELETONS);
        const bool drawBoundingSpheres = outputType == static_cast<unsigned int>(OutputType::SKELETON_BOUNDING_SPHERES);
        if ((!drawSkeletons && !drawBoundingSpheres) || !m_resources->typeCount) return {};
        auto* rc = context.hostData->Get<RenderContext>(); auto& cmd = context.commandList;
        cmd.SetDescriptorHeaps(rc->textureDescriptorHeap.GetHandle(), rc->samplerDescriptorHeap.GetHandle());
        rhi::PassBeginInfo pass{}; rhi::ColorAttachment color{};
        color.rtv = { rc->rtvHeap.GetHandle(), rc->frameIndex }; color.loadOp = rhi::LoadOp::Load; color.storeOp = rhi::StoreOp::Store;
        pass.colors = { &color }; pass.width = rc->outputResolution.x; pass.height = rc->outputResolution.y; pass.debugName = "Wind Skeleton Debug Overlay";
        cmd.BeginPass(pass);
        cmd.BindLayout(PSOManager::GetInstance().GetRootSignature().GetHandle());
        uint32_t constants[12] = {
            m_resources->windTypes->GetSRVInfo(0).slot.index, m_resources->boneEntries->GetSRVInfo(0).slot.index,
            m_resources->activeInstances->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::BoneTransforms)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::InverseBindMatrices)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::SkeletonResources::SkinningInstanceInfo)->GetSRVInfo(0).slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PerFrameBuffer)->GetCBVInfo().slot.index,
            m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index,
            0u,
            m_resources->skinnedPlacements ? m_resources->skinnedPlacements->GetSRVInfo(0).slot.index : 0u,
            m_resources->activeSkinnedPlacements ? m_resources->activeSkinnedPlacements->GetSRVInfo(0).slot.index : 0u,
            m_resources->residentPlacementCount };
        cmd.PushConstants(rhi::ShaderStage::AllGraphics, 0, MiscUintRootSignatureIndex, 0, 12, constants);
        if (drawSkeletons) {
            cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::LineList);
            cmd.BindPipeline(m_pso->GetHandle());
            BindResourceDescriptorIndices(cmd, m_bindings);
            cmd.ExecuteIndirect(m_signature->GetHandle(), m_resources->indirectCommands->GetAPIResource().GetHandle(), 0,
                m_resources->allocationCounters->GetAPIResource().GetHandle(), sizeof(uint32_t), m_resources->typeCount);
        }
        if (drawBoundingSpheres && m_resources->residentPlacementCount != 0u) {
            cmd.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
            cmd.BindPipeline(m_spherePso->GetHandle());
            BindResourceDescriptorIndices(cmd, m_sphereBindings);
            cmd.DispatchMesh(m_resources->residentPlacementCount, 1u, 1u);
        }
        return {};
    }
    void Cleanup() override { m_signature.Reset(); m_spherePso.Reset(); m_pso.Reset(); }
private:
    std::shared_ptr<WindSharedResources> m_resources;
    rhi::PipelinePtr m_pso;
    rhi::PipelinePtr m_spherePso;
    rhi::CommandSignaturePtr m_signature;
    PipelineResources m_bindings;
    PipelineResources m_sphereBindings;
};

} // namespace

ProceduralWindExtension::ProceduralWindExtension(std::shared_ptr<ProceduralWindRuntime> runtime) : m_runtime(std::move(runtime)) {}

void ProceduralWindExtension::GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& out)
{
    auto resources = std::make_shared<WindSharedResources>(m_runtime, rg.GetReadbackService());
    auto earlyInsertion = RenderGraph::ExternalInsertPoint::Before("CLodOpaque::HierarchicalCullingPass1");
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::UploadFieldPair", std::make_shared<WindResidencyPass>(resources)).At(earlyInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ResetTransient", std::make_shared<WindResetPass>(resources)).At(earlyInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ActivateInstancesPhase1", std::make_shared<WindActivatePass>(resources, false)).At(earlyInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::BuildSimulationCommandsPhase1", std::make_shared<WindBuildCommandsPass>(resources, false)).At(earlyInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateInstancesPhase1", std::make_shared<WindIndirectSimulatePass>(resources)).At(earlyInsertion));

    auto lateInsertion = RenderGraph::ExternalInsertPoint::After("CLodOpaque::LinearDepthDownsamplePass1");
    lateInsertion.AlsoBefore("CLodOpaque::HierarchicalCullingPass2");
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::ActivateInstancesPhase2", std::make_shared<WindActivatePass>(resources, true)).At(lateInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::BuildSimulationCommandsPhase2", std::make_shared<WindBuildCommandsPass>(resources, true)).At(lateInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Compute("ProceduralWind::SimulateInstancesPhase2", std::make_shared<WindIndirectSimulatePass>(resources, true)).At(lateInsertion));
    out.push_back(RenderGraph::ExternalPassDesc::Render("ProceduralWind::DebugActiveSkeletons", std::make_shared<WindSkeletonDebugPass>(resources))
        .At(RenderGraph::ExternalInsertPoint::After("TonemappingPass")));
}

} // namespace br::wind
