// Simulation LOD: the guides the solver does not own. One dispatch per guide level, coarsest level first, after the
// simulate pass, over that level's guides of every simulated instance (HairSimGroup: each thread group is 64 guides of
// one instance); one thread per guide.
//
// A guide that is not simulated is evaluated from its (up to) three parents in the level below, as render strands are
// from their guides (linear skinning with a rotation, sampled by curve parameter) but with the rotation taken from the
// parent's chord, not its tangent (HairSkinToGuideChord in hairGuideSkin.hlsli says why); a level 0 guide has no parents
// and goes to its rigid pose. Its rows are what deeper levels and the evaluate pass read, and they are kept ready for the
// solver: the velocity in solverPrevious is the point's motion over the frame, so a promoted guide is simulated from
// them as they are.
//
// A guide in a blend was simulated this frame (hairSimulate.hlsl, which also keeps the blend's clock). This pass pulls
// its state onto the evaluated one by the blend's share: all of it in a promotion's first frame and less each frame
// after, nothing in a demotion's first frame and all of it in its last, after which the guide is no longer simulated.
// The pull moves the solver's previous position with the point, so it adds no velocity.
//
// A simulated guide in no blend is left alone: the thread returns after reading the instance, the guide's point range
// and its first point.
//
// Levels are dispatched in order with a barrier between them: a level reads the rows of the level below and writes its
// own, in the same buffer.

#include "HairShaders/hairCommon.hlsli"

// HairGuideLevelsPass::Record. The entities' rows are bound by name.
#define HAIR_INSTANCES UintRootConstant0            // StructuredBuffer<HairInstanceGPU>
#define HAIR_CLUSTERS UintRootConstant1             // StructuredBuffer<HairClusterHeader>
#define HAIR_STRANDS UintRootConstant2              // StructuredBuffer<HairStrandRecord>
#define HAIR_ROOT_BINDINGS UintRootConstant3        // StructuredBuffer<HairRootBinding>
#define HAIR_POINTS UintRootConstant5               // StructuredBuffer<HairPackedPoint>
#define HAIR_GUIDE_STRAND_INDICES UintRootConstant6 // StructuredBuffer<uint>
#define HAIR_GUIDE_POINT_RANGES UintRootConstant7   // StructuredBuffer<HairGuidePointRange>
#define HAIR_GUIDE_POINTS UintRootConstant8         // RWStructuredBuffer<HairGuidePoint>
#define HAIR_GUIDE_BINDINGS UintRootConstant9       // StructuredBuffer<HairGuideBinding>
#define HAIR_SIM_FRAME UintRootConstant10           // StructuredBuffer<HairSimFrameGPU>
#define HAIR_SIM_METRICS UintRootConstant11         // RWStructuredBuffer<uint>
#define HAIR_SIM_GROUP_FIRST UintRootConstant12     // the dispatch's first row of HAIR_SIM_GROUPS: one guide level's
#define HAIR_SIM_GROUPS UintRootConstant13          // StructuredBuffer<HairSimGroup>: a thread group's instance and guides

#define HAIR_GUIDE_POINT_BUFFER RWStructuredBuffer<HairGuidePoint>
#include "HairShaders/hairGuideSkin.hlsli"
#include "HairShaders/hairRootFrame.hlsli"

[shader("compute")]
[numthreads(64, 1, 1)]
void HairGuideLevelCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    StructuredBuffer<HairSimGroup> simGroups = ResourceDescriptorHeap[HAIR_SIM_GROUPS];
    const HairSimGroup simGroup = simGroups[HAIR_SIM_GROUP_FIRST + groupId.x];
    const uint instanceIndex = simGroup.instanceIndex;
    const HairInstanceGPU instance = instances[instanceIndex];
    const uint rank = simGroup.firstRank + groupIndex;
    if ((instance.simFlags & HAIR_SIM_FLAG_ENABLED) == 0u || instance.guidePointBase == HAIR_INVALID_INDEX ||
        groupIndex >= simGroup.count || rank >= instance.guideCount)
    {
        return;
    }

    StructuredBuffer<HairGuidePointRange> guidePointRanges = ResourceDescriptorHeap[HAIR_GUIDE_POINT_RANGES];
    RWStructuredBuffer<HairGuidePoint> guidePoints = ResourceDescriptorHeap[HAIR_GUIDE_POINTS];
    const HairGuidePointRange range = guidePointRanges[instance.guideBase + rank];
    const uint pointCount = range.count;
    const uint base = instance.guidePointBase + range.offset;
    // The simulate pass has run: on an instance's first frame it left every guide's state valid.
    const uint guideState = guidePoints[base].guideState;
    const bool simulated = (guideState & HAIR_GUIDE_STATE_SIMULATED) != 0u;
    const bool blending = (guideState & (HAIR_GUIDE_STATE_PROMOTING | HAIR_GUIDE_STATE_DEMOTING)) != 0u;
    if (simulated && !blending)
    {
        return;
    }

    StructuredBuffer<HairClusterHeader> clusters = ResourceDescriptorHeap[HAIR_CLUSTERS];
    StructuredBuffer<HairStrandRecord> strands = ResourceDescriptorHeap[HAIR_STRANDS];
    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<HairPackedPoint> points = ResourceDescriptorHeap[HAIR_POINTS];
    StructuredBuffer<uint> guideStrandIndices = ResourceDescriptorHeap[HAIR_GUIDE_STRAND_INDICES];
    StructuredBuffer<HairGuideBinding> guideBindings = ResourceDescriptorHeap[HAIR_GUIDE_BINDINGS];
    StructuredBuffer<HairSimFrameGPU> simFrame = ResourceDescriptorHeap[HAIR_SIM_FRAME];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<uint> metrics = ResourceDescriptorHeap[HAIR_SIM_METRICS];

    const HairSimFrameGPU sim = simFrame[0];
    const HairGuideBinding binding = guideBindings[instance.guideBase + rank];
    const uint strandRow = guideStrandIndices[instance.guideBase + rank];
    const HairStrandRecord strand = strands[instance.strandBase + strandRow];
    const HairClusterHeader cluster = clusters[instance.clusterBase + strand.clusterIndex];
    const uint restBase = instance.pointBase + strand.pointOffset;
    const HairRootBinding rootBinding = rootBindings[instance.strandBase + strandRow];
    const PerObjectBuffer object = perObject[instance.perObjectIndex];
    const HairRootFramePair rootFrame = HairMakeRootFramePair(instance, rootBinding, object, sim.frameNumber);
    const HairRootFollow follow = HairMakeRootFollow(rootBinding, rootFrame.current, HairModelScale(object.model));
    const HairRootFollow previousFollow = HairMakeRootFollow(rootBinding, rootFrame.previous, HairModelScale(object.prevModel));
    // As the simulate pass resets a guide: the instance starts, or the root jumped.
    const bool reset = (instance.simFlags & HAIR_SIM_FLAG_RESET) != 0u ||
        distance(rootFrame.current.position, rootFrame.previous.position) > sim.teleportDistance;
    const float frameSeconds = sim.stepSeconds * (float)sim.stepCount;

    // The parents: guides of the level below, whose rows are final for this frame.
    uint parentBases[3];
    uint parentSegmentCounts[3];
    float parentWeights[3];
    float weightSum = 0.0f;
    for (uint parent = 0u; parent < 3u; ++parent)
    {
        const uint parentRank = binding.parentIndices[parent];
        const bool used = binding.level != 0u && parentRank != HAIR_INVALID_INDEX && parentRank < instance.guideCount &&
            binding.parentWeights[parent] > 0.0f;
        HairGuidePointRange parentRange;
        parentRange.offset = 0u;
        parentRange.count = 2u;
        if (used)
        {
            parentRange = guidePointRanges[instance.guideBase + parentRank];
        }
        parentBases[parent] = instance.guidePointBase + parentRange.offset;
        parentSegmentCounts[parent] = parentRange.count - 1u;
        parentWeights[parent] = used ? binding.parentWeights[parent] : 0.0f;
        weightSum += parentWeights[parent];
    }

    // How much of the evaluated state the rows take: all of it for a guide that is not simulated.
    const float blendSeconds = (guideState & HAIR_GUIDE_STATE_DEMOTING) != 0u ? sim.demoteSeconds : sim.promoteSeconds;
    const float pull = simulated ? HairGuideBlendPull(guideState, guidePoints[base].guideBlend, blendSeconds) : 1.0f;
    // A demotion whose pull has reached 1 is over: the rows are the evaluated state, and stay so.
    const bool demoted = simulated && (guideState & HAIR_GUIDE_STATE_DEMOTING) != 0u && pull >= 1.0f;

    // The stamp of the rows this pass writes. A guide that goes on from the state its rows hold must find them written
    // for this instance in the frame before, or in this one by the simulate pass: counted when not.
    const uint guideStamp = HAIR_GUIDE_STAMP(instanceIndex, sim.frameNumber);
    const uint previousStamp = HAIR_GUIDE_STAMP(instanceIndex, sim.frameNumber - 1u);
    float motion = 0.0f;
    float3 rootMotion = float3(0.0f, 0.0f, 0.0f);
    for (uint pointIndex = 0u; pointIndex < pointCount; ++pointIndex)
    {
        HairGuidePoint state = guidePoints[base + pointIndex];
        if (!simulated)
        {
            // The simulate pass wrote these for the guides it simulated.
            const float3 rest = HairDecodePointPosition(cluster, points[restBase + pointIndex]);
            state.rigid = HairFollowRoot(follow, rest);
            state.previousRigid = HairFollowRoot(previousFollow, rest);
        }

        HairEvaluatedPoint evaluated;
        evaluated.position = state.rigid;
        evaluated.previousPosition = state.previousRigid;
        if (weightSum > 0.0f)
        {
            const float parameter = (float)pointIndex / (float)(pointCount - 1u);
            evaluated.position = float3(0.0f, 0.0f, 0.0f);
            evaluated.previousPosition = float3(0.0f, 0.0f, 0.0f);
            for (uint skinParent = 0u; skinParent < 3u; ++skinParent)
            {
                if (parentWeights[skinParent] <= 0.0f)
                {
                    continue;
                }
                const HairEvaluatedPoint skinned = HairSkinToGuideChord(parentBases[skinParent], parentSegmentCounts[skinParent],
                    parameter, state.rigid, state.previousRigid);
                evaluated.position += parentWeights[skinParent] * skinned.position;
                evaluated.previousPosition += parentWeights[skinParent] * skinned.previousPosition;
            }
            evaluated.position /= weightSum;
            evaluated.previousPosition /= weightSum;
        }

        if (simulated)
        {
            const float3 pulled = (evaluated.position - state.position) * pull;
            state.position += pulled;
            state.solverPrevious += pulled;
        }
        else if (reset)
        {
            state.position = evaluated.position;
            state.previousPosition = evaluated.previousPosition;
            state.solverPrevious = evaluated.position;
            state.solverSeconds = 0.0f;
        }
        else
        {
            // Ready for the solver: the point's velocity is its motion over the frame's simulated time. A frame that
            // simulated no time moved the point with its root, and keeps the velocity it had.
            const float3 moved = evaluated.position - state.position;
            state.previousPosition = state.position;
            state.position = evaluated.position;
            if (frameSeconds > 0.0f)
            {
                state.solverPrevious = state.previousPosition;
                state.solverSeconds = frameSeconds;
            }
            else
            {
                state.solverPrevious += moved;
            }
        }
        if (pointIndex == 1u)
        {
            if (!reset && state.guideState != guideStamp && state.guideState != previousStamp)
            {
                InterlockedAdd(metrics[instanceIndex * HAIR_SIM_METRIC_COUNT + HAIR_SIM_METRIC_STALE_GUIDES], 1u);
            }
            state.guideState = guideStamp;
        }
        if (pointIndex == 0u)
        {
            rootMotion = state.position - state.previousPosition;
            // CHANGED on a guide that is not simulated: its demotion ended in the frame before.
            state.guideState = demoted ? HAIR_GUIDE_STATE_CHANGED : simulated ? guideState : 0u;
            if (!simulated || demoted)
            {
                state.guideBlend = 0.0f;
            }
        }
        else if (pointIndex > 1u)
        {
            motion = max(motion, HairGuidePointMotion(state, rootMotion));
        }
        guidePoints[base + pointIndex] = state;
    }

    const uint metricBase = instanceIndex * HAIR_SIM_METRIC_COUNT;
    const bool transition = simulated || (guideState & HAIR_GUIDE_STATE_CHANGED) != 0u;
    InterlockedMax(metrics[metricBase + (transition && !reset ? HAIR_SIM_METRIC_TRANSITION_MOTION : HAIR_SIM_METRIC_STEADY_MOTION)],
        asuint(motion));
    if (demoted)
    {
        InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_DEMOTED_GUIDES], 1u);
    }
    if (!simulated)
    {
        InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_EVALUATED_GUIDES], 1u);
    }
}
