// Guide simulation: one dispatch over every simulated instance (HairSimGroup: each thread group is 64 guides of one
// instance, and only the guide levels that can hold a simulated guide are covered), one thread per guide (a
// guide-capable strand, by rank), with a serial loop over the guide's points. The state is HairGuidePoint rows in world space, so root motion gives inertia.
//
// Simulation LOD: a thread whose guide the instance's simulation level does not ask for (HairGuideIsSimulated), and that
// is not still being demoted, returns after reading four rows (the instance, the guide's binding, its point range and
// its first point): hairGuideLevels.hlsl evaluates that guide from its parents. A guide the level newly asks for is
// simulated from the state its rows hold, which that pass kept ready (position, and a velocity in solverPrevious).
// This pass keeps each guide's blend state (HAIR_GUIDE_STATE_*) in its row 0; the pull itself is that pass's.
//
// An instance is only simulated while a view shows it. One that starts (HAIR_SIM_FLAG_RESET) puts its guides on their
// rigid pose, at rest. One that starts again after it was simulated before (HAIR_SIM_FLAG_KEEP_SHAPE as well) has the
// shape it was left in in its rows: a guide the solver owned then, and that the level still asks for, starts at rest in
// that shape, each point at the offset from its rigid pose that it had; the others are left to the guide-level pass.
//
// A frame takes stepCount fixed steps of `substeps` substeps each, and a guide whose root or whose fastest point
// travels far in the frame divides the same time into more substeps (HAIR_SIM_SUBSTEP_TRAVEL). One substep, for a
// guide of n points:
//   1. Points 0 and 1 are kinematic: they sit on the rest shape in the root's frame, which fixes the root's position
//      and the direction the strand leaves it. Across the frame's substeps they go from where the last frame left
//      them to this frame's root frame.
//   2. The other points are integrated (position Verlet): drag toward the air's velocity (the wind, or rest), gravity.
//   3. Shape: each point is pulled toward where the rest shape puts it in the root's frame (its rigid pose), softly,
//      and more softly toward the tip. That keeps the style: which way each strand leaves the head and falls.
//   4. Bend, from the root to the tip: point i belongs where the rest shape puts it given its two parents,
//      x[i-1] + R * (rest[i] - rest[i-1]), R being the smallest rotation that takes the rest direction of segment
//      (i-2, i-1) in the root's frame to its current direction. That keeps waves and curls wherever the strand hangs.
//      The goal turns with the parent segment, so the correction goes to all three points, by the constraint's
//      gradient: for a straight rest shape it is the usual (1, -2, 1). (Giving it to the child alone, or to the child
//      and the parent alone, leaves a torque with no reaction, and strands then feed themselves energy: they whip
//      apart, or never come to rest.)
//   5. Collision with the instance's capsules, each at the pose it has at this substep, then stretch: distance
//      constraints between neighbours. A strand's stretch constraints form a chain, so the linear system for their
//      multipliers is tridiagonal and is solved exactly (Thomas algorithm), not by sweeps. Three times.
//   6. Collision again, so the substep ends with no point inside a collider, and friction.
// Every constraint is XPBD in its small-step form: one solve per substep, so the multipliers start at zero and none
// are stored. One sweep of the bend constraint is stiff against kinks and weak against long bends, which is what the
// shape pull is for.
//
// A frame that takes no step (its time has not filled one yet) still moves the root and the colliders. The hair goes
// with its root as it is, every point moving as the root does, and one substep of no length follows: steps 1, 5 and 6
// only, and what they move keeps its velocity.

#include "HairShaders/hairCommon.hlsli"

// HairSimulatePass::Record. The entities' rows and the bone palettes are bound by name.
#define HAIR_INSTANCES UintRootConstant0            // StructuredBuffer<HairInstanceGPU>
#define HAIR_CLUSTERS UintRootConstant1             // StructuredBuffer<HairClusterHeader>
#define HAIR_STRANDS UintRootConstant2              // StructuredBuffer<HairStrandRecord>
#define HAIR_ROOT_BINDINGS UintRootConstant3        // StructuredBuffer<HairRootBinding>
#define HAIR_POINTS UintRootConstant5               // StructuredBuffer<HairPackedPoint>
#define HAIR_GUIDE_STRAND_INDICES UintRootConstant6 // StructuredBuffer<uint>
#define HAIR_GUIDE_POINT_RANGES UintRootConstant7   // StructuredBuffer<HairGuidePointRange>
#define HAIR_GUIDE_POINTS UintRootConstant8         // RWStructuredBuffer<HairGuidePoint>
#define HAIR_COLLIDERS UintRootConstant9            // StructuredBuffer<HairColliderGPU>
#define HAIR_SIM_FRAME UintRootConstant10           // StructuredBuffer<HairSimFrameGPU>
#define HAIR_SIM_METRICS UintRootConstant11         // RWStructuredBuffer<uint>
#define HAIR_SIM_GROUP_FIRST UintRootConstant12     // the dispatch's first row of HAIR_SIM_GROUPS
#define HAIR_GUIDE_BINDINGS UintRootConstant13      // StructuredBuffer<HairGuideBinding>
#define HAIR_SIM_GROUPS UintRootConstant14          // StructuredBuffer<HairSimGroup>: a thread group's instance and guides

// Stretch solves a substep. One leaves about 1e-3 of a segment's length on a calm strand, two 1e-5; the third is for
// roots and colliders that jump (a frame that repeats or skips the entity's motion).
#define HAIR_STRETCH_PASSES 3u

#include "HairShaders/hairRootFrame.hlsli"

typedef HairRootSkinMatrix HairSkinMatrix;

// A collider in world space at the previous frame's pose (0) and this frame's (1).
struct HairColliderMotion
{
    float3 a0;
    float3 b0;
    float3 a1;
    float3 b1;
    float radius;
};

// The air's velocity at a point: the wind, its speed varying along and across its direction and in time.
float3 HairWind(HairSimFrameGPU sim, float3 position, float time)
{
    const float speedSquared = dot(sim.wind, sim.wind);
    if (speedSquared < 1.0e-12f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 direction = sim.wind * rsqrt(speedSquared);
    // A gust is a few metres long and is not the same across the wind.
    const float phase = 6.2831853f * (sim.gustFrequency * time - 0.4f * dot(position, direction)) +
        2.1f * dot(position, float3(0.7f, 0.3f, -0.6f));
    const float gust = 0.6f * sin(phase) + 0.4f * sin(2.3f * phase + 1.7f);
    return sim.wind * (1.0f + sim.gustAmplitude * gust);
}

// Signed distance from a point to a capsule's surface, with the nearest point of its axis and that point's parameter.
float HairCapsuleDistance(float3 position, float3 a, float3 b, float radius, out float3 nearest, out float parameter)
{
    const float3 axis = b - a;
    const float axisSquared = dot(axis, axis);
    parameter = axisSquared > 1.0e-12f ? saturate(dot(position - a, axis) / axisSquared) : 0.0f;
    nearest = a + axis * parameter;
    return distance(position, nearest) - radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairSimulateCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
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

    // Whether this guide is simulated this frame: the level asks for it, or it was simulated and is being demoted.
    StructuredBuffer<HairGuideBinding> guideBindings = ResourceDescriptorHeap[HAIR_GUIDE_BINDINGS];
    StructuredBuffer<HairGuidePointRange> guidePointRanges = ResourceDescriptorHeap[HAIR_GUIDE_POINT_RANGES];
    RWStructuredBuffer<HairGuidePoint> guidePoints = ResourceDescriptorHeap[HAIR_GUIDE_POINTS];
    const HairGuideBinding binding = guideBindings[instance.guideBase + rank];
    const HairGuidePointRange range = guidePointRanges[instance.guideBase + rank];
    const uint pointCount = range.count;
    const uint base = instance.guidePointBase + range.offset;
    const bool wanted = HairGuideIsSimulated(instance, binding);
    // The rows of an instance that starts hold nothing, or the shape it was left in.
    const bool instanceReset = (instance.simFlags & HAIR_SIM_FLAG_RESET) != 0u;
    const uint storedState = guidePoints[base].guideState;
    const bool keepShape = instanceReset && (instance.simFlags & HAIR_SIM_FLAG_KEEP_SHAPE) != 0u &&
        (storedState & HAIR_GUIDE_STATE_SIMULATED) != 0u;
    uint guideState = instanceReset ? 0u : storedState;
    float guideBlend = guidePoints[base].guideBlend;
    if (!wanted && (guideState & HAIR_GUIDE_STATE_SIMULATED) == 0u)
    {
        if (instanceReset)
        {
            guidePoints[base].guideState = 0u;
        }
        return;
    }

    StructuredBuffer<HairClusterHeader> clusters = ResourceDescriptorHeap[HAIR_CLUSTERS];
    StructuredBuffer<HairStrandRecord> strands = ResourceDescriptorHeap[HAIR_STRANDS];
    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<HairPackedPoint> points = ResourceDescriptorHeap[HAIR_POINTS];
    StructuredBuffer<uint> guideStrandIndices = ResourceDescriptorHeap[HAIR_GUIDE_STRAND_INDICES];
    StructuredBuffer<HairColliderGPU> colliders = ResourceDescriptorHeap[HAIR_COLLIDERS];
    StructuredBuffer<HairSimFrameGPU> simFrame = ResourceDescriptorHeap[HAIR_SIM_FRAME];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<uint> metrics = ResourceDescriptorHeap[HAIR_SIM_METRICS];

    const HairSimFrameGPU sim = simFrame[0];
    const uint strandRow = guideStrandIndices[instance.guideBase + rank];
    const HairStrandRecord strand = strands[instance.strandBase + strandRow];
    const HairClusterHeader cluster = clusters[instance.clusterBase + strand.clusterIndex];
    const uint restBase = instance.pointBase + strand.pointOffset;

    const HairRootBinding rootBinding = rootBindings[instance.strandBase + strandRow];
    const PerObjectBuffer object = perObject[instance.perObjectIndex];
    const HairRootFramePair rootFrame = HairMakeRootFramePair(instance, rootBinding, object, sim.frameNumber);
    const float scale = HairModelScale(object.model);
    const HairRootFollow follow = HairMakeRootFollow(rootBinding, rootFrame.current, scale);
    const HairRootFollow previousFollow = HairMakeRootFollow(rootBinding, rootFrame.previous, HairModelScale(object.prevModel));

    // A root that jumped is a teleport: the guide starts again from its rest pose.
    const bool reset = instanceReset ||
        distance(rootFrame.current.position, rootFrame.previous.position) > sim.teleportDistance;

    // The guide's blend state for this frame. A blend that turns round keeps its pull.
    const bool promoted = !reset && (guideState & HAIR_GUIDE_STATE_SIMULATED) == 0u;
    if (reset)
    {
        if (!wanted || (instanceReset && (instance.simFlags & HAIR_SIM_FLAG_KEEP_SHAPE) != 0u && !keepShape))
        {
            // A teleport in a demotion ends it: the guide-level pass puts the guide on what its parents give it. So it
            // does a guide the solver did not own when its instance was left: the level asks for it now, and it is
            // promoted from that in the next frame, like any other.
            guidePoints[base].guideState = 0u;
            return;
        }
        guideState = HAIR_GUIDE_STATE_SIMULATED;
        guideBlend = 0.0f;
    }
    else if (promoted)
    {
        guideState = HAIR_GUIDE_STATE_SIMULATED | HAIR_GUIDE_STATE_CHANGED |
            (sim.promoteSeconds > 0.0f ? HAIR_GUIDE_STATE_PROMOTING : 0u);
        guideBlend = 0.0f;
    }
    else
    {
        const float frameSeconds = sim.stepSeconds * (float)sim.stepCount;
        guideState &= ~HAIR_GUIDE_STATE_CHANGED;
        if (wanted && (guideState & HAIR_GUIDE_STATE_DEMOTING) != 0u)
        {
            guideBlend = HairGuideBlendMirror(guideBlend, sim.demoteSeconds, sim.promoteSeconds);
            guideState = HAIR_GUIDE_STATE_SIMULATED | (sim.promoteSeconds > 0.0f ? HAIR_GUIDE_STATE_PROMOTING : 0u);
        }
        else if (!wanted && (guideState & HAIR_GUIDE_STATE_PROMOTING) != 0u)
        {
            guideBlend = HairGuideBlendMirror(guideBlend, sim.promoteSeconds, sim.demoteSeconds);
            guideState = HAIR_GUIDE_STATE_SIMULATED | HAIR_GUIDE_STATE_DEMOTING;
        }
        else if (!wanted && (guideState & HAIR_GUIDE_STATE_DEMOTING) == 0u)
        {
            guideState |= HAIR_GUIDE_STATE_DEMOTING;
            guideBlend = 0.0f;
        }
        else if ((guideState & (HAIR_GUIDE_STATE_PROMOTING | HAIR_GUIDE_STATE_DEMOTING)) != 0u)
        {
            guideBlend += frameSeconds;
            if ((guideState & HAIR_GUIDE_STATE_PROMOTING) != 0u && guideBlend >= sim.promoteSeconds)
            {
                guideState &= ~HAIR_GUIDE_STATE_PROMOTING;
            }
        }
    }

    // The frame the renderer calls previous is not always the one this guide was last simulated in: the entity's row
    // comes from a published version of the scene, and a frame can bind the version of the frame before again, or skip
    // one. So the root starts each frame from where the simulation left it, and `rootShift` is how far that is from the
    // previous root frame: the previous pose of everything else that follows the entity is moved by it too.
    float3 rootShift = float3(0.0f, 0.0f, 0.0f);
    float3 rootMove = float3(0.0f, 0.0f, 0.0f); // how far the root goes this frame
    float fastest = 0.0f;                       // the guide's fastest point, m/s
    // Whether time passes this frame. A frame whose time has not filled a step yet takes none, and the hair then goes
    // with its root as it is: every point moves as the root does.
    const bool advance = sim.stepCount != 0u;
    uint pointIndex;
    for (pointIndex = 0u; pointIndex < pointCount; ++pointIndex)
    {
        const float3 rest = HairDecodePointPosition(cluster, points[restBase + pointIndex]);
        HairGuidePoint state = guidePoints[base + pointIndex];
        // The shape the instance was left in, as each point's offset from the rigid pose it had then.
        const float3 keptOffset = keepShape ? state.position - state.rigid : float3(0.0f, 0.0f, 0.0f);
        state.rigid = HairFollowRoot(follow, rest);
        state.previousRigid = HairFollowRoot(previousFollow, rest);
        if (pointIndex == 0u)
        {
            state.guideState = guideState;
            state.guideBlend = guideBlend;
        }
        else if (pointIndex == 1u)
        {
            // A guide that goes on from the state its rows hold must find them written for this instance in the frame
            // before: counted when not. Then the stamp of this frame, which the passes that read the guide check.
            if (!reset && state.guideState != HAIR_GUIDE_STAMP(instanceIndex, sim.frameNumber - 1u))
            {
                InterlockedAdd(metrics[instanceIndex * HAIR_SIM_METRIC_COUNT + HAIR_SIM_METRIC_STALE_GUIDES], 1u);
            }
            state.guideState = HAIR_GUIDE_STAMP(instanceIndex, sim.frameNumber);
        }
        if (reset)
        {
            state.position = state.rigid + keptOffset;
            state.solverPrevious = state.position;
            state.previousPosition = state.previousRigid + keptOffset;
            state.solverSeconds = 0.0f;
        }
        else
        {
            state.previousPosition = state.position;
            if (pointIndex == 0u)
            {
                rootShift = state.position - state.previousRigid;
                rootMove = state.rigid - state.position;
            }
            else if (pointIndex > 1u)
            {
                if (state.solverSeconds > 0.0f)
                {
                    fastest = max(fastest, distance(state.position, state.solverPrevious) / state.solverSeconds);
                }
                if (!advance)
                {
                    state.position += rootMove;
                    state.solverPrevious += rootMove;
                }
            }
        }
        guidePoints[base + pointIndex] = state;
    }

    // The colliders follow the attach bone when the instance has one, and the entity.
    float4x4 colliderTransform = object.model;
    float4x4 previousColliderTransform = object.prevModel;
    if (instance.skinningInstanceSlot != HAIR_INVALID_INDEX && instance.attachBone != HAIR_INVALID_INDEX)
    {
        StructuredBuffer<SkinningInstanceGPUInfo> infos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
        StructuredBuffer<HairSkinMatrix> bones =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];
        const SkinningInstanceGPUInfo info = infos[instance.skinningInstanceSlot];
        if (instance.attachBone < info.boneCount)
        {
            colliderTransform = mul(bones[info.transformOffsetMatrices + instance.attachBone], object.model);
            previousColliderTransform = mul(bones[info.previousTransformOffsetMatrices + instance.attachBone], object.prevModel);
        }
    }
    const uint colliderCount = min(instance.colliderCount, HAIR_MAX_COLLIDERS_PER_INSTANCE);
    HairColliderMotion motions[HAIR_MAX_COLLIDERS_PER_INSTANCE];
    uint colliderIndex;
    for (colliderIndex = 0u; colliderIndex < HAIR_MAX_COLLIDERS_PER_INSTANCE; ++colliderIndex)
    {
        HairColliderMotion motion = (HairColliderMotion)0;
        if (colliderIndex < colliderCount)
        {
            const HairColliderGPU collider = colliders[instance.colliderBase + colliderIndex];
            motion.a0 = mul(float4(collider.a, 1.0f), previousColliderTransform).xyz + rootShift;
            motion.b0 = mul(float4(collider.b, 1.0f), previousColliderTransform).xyz + rootShift;
            motion.a1 = mul(float4(collider.a, 1.0f), colliderTransform).xyz;
            motion.b1 = mul(float4(collider.b, 1.0f), colliderTransform).xyz;
            motion.radius = collider.radius * scale;
        }
        motions[colliderIndex] = motion;
    }

    // When time passes there are enough substeps for how far the root goes and how far the guide's fastest point would.
    // Otherwise one substep of no length puts the kinematic points on the root and takes the rest out of the colliders,
    // which have turned with the entity where the points only moved with the root.
    const uint askedSubsteps = advance ? sim.stepCount * max(sim.substeps, 1u) : 1u;
    const float travel = advance ? max(length(rootMove), fastest * sim.stepSeconds * (float)sim.stepCount) : 0.0f;
    const uint substepCount = reset || pointCount < 3u ? 0u
        : clamp((uint)ceil(travel / HAIR_SIM_SUBSTEP_TRAVEL), askedSubsteps, max(askedSubsteps, HAIR_SIM_MAX_SUBSTEPS));
    const float substepSeconds = advance && substepCount != 0u
        ? sim.stepSeconds * (float)sim.stepCount / (float)substepCount
        : sim.stepSeconds / (float)max(sim.substeps, 1u);
    const float dragShare = 1.0f - exp(-sim.damping * substepSeconds);
    const float3 gravityStep = sim.gravity * (substepSeconds * substepSeconds);
    const float stretchCompliance = sim.stretchCompliance / (substepSeconds * substepSeconds);
    const float bendCompliance = sim.bendCompliance / (substepSeconds * substepSeconds);
    for (uint substep = 0u; substep < substepCount; ++substep)
    {
        const float frameShare = (float)(substep + 1u) / (float)substepCount;
        const float time = sim.time + (float)(substep + 1u) * substepSeconds;

        // 1. The kinematic points.
        for (pointIndex = 0u; pointIndex < 2u; ++pointIndex)
        {
            HairGuidePoint state = guidePoints[base + pointIndex];
            state.solverPrevious = state.position;
            state.position = lerp(state.previousPosition, state.rigid, frameShare);
            guidePoints[base + pointIndex] = state;
        }

        // 2. Integrate. A velocity is kept as the distance covered in a substep, of the length the point last took.
        for (pointIndex = 2u; pointIndex < pointCount && advance; ++pointIndex)
        {
            HairGuidePoint state = guidePoints[base + pointIndex];
            float3 step = state.solverSeconds > 0.0f
                ? (state.position - state.solverPrevious) * (substepSeconds / state.solverSeconds)
                : float3(0.0f, 0.0f, 0.0f);
            step += (HairWind(sim, state.position, time) * substepSeconds - step) * dragShare;
            state.solverPrevious = state.position;
            state.solverSeconds = substepSeconds;
            state.position += step + gravityStep;
            guidePoints[base + pointIndex] = state;
        }

        // 3. Shape: toward the rigid pose at this substep, with a compliance that grows from the root to the tip.
        for (pointIndex = 2u; pointIndex < pointCount && advance; ++pointIndex)
        {
            const HairGuidePoint state = guidePoints[base + pointIndex];
            const float along = (float)pointIndex / (float)(pointCount - 1u);
            const float compliance = sim.shapeCompliance * along * along / (substepSeconds * substepSeconds);
            const float3 goal = lerp(state.previousRigid + rootShift, state.rigid, frameShare);
            guidePoints[base + pointIndex].position = state.position + (goal - state.position) / (1.0f + compliance);
        }

        // 4. Bend, the parents first. C = x[i] - x[i-1] - offset, where the offset is the rest offset turned with the
        //    parent segment; its gradient is 1 at the point, -(1 + M) at the parent and M at the grandparent, M being
        //    how the offset turns when the parent segment does.
        HairGuidePoint grandparent = guidePoints[base];
        HairGuidePoint parent = guidePoints[base + 1u];
        for (pointIndex = 2u; pointIndex < pointCount && advance; ++pointIndex)
        {
            HairGuidePoint state = guidePoints[base + pointIndex];
            const float3 restDirection = HairSafeNormalize(parent.rigid - grandparent.rigid, follow.basis[2]);
            const float3 parentSegment = parent.position - grandparent.position;
            const float parentLength = max(length(parentSegment), 1.0e-9f);
            const float3 direction = HairSafeNormalize(parentSegment, restDirection);
            const float3 offset = HairRotateBetween(restDirection, direction, state.rigid - parent.rigid);
            // Points 0 and 1 are held on the root.
            const float parentWeight = pointIndex > 2u ? 1.0f : 0.0f;
            const float grandparentWeight = pointIndex > 3u ? 1.0f : 0.0f;
            const float turn = length(offset) / parentLength;
            const float3 multiplier = (parent.position + offset - state.position) /
                (1.0f + parentWeight * (1.0f + turn) * (1.0f + turn) + grandparentWeight * turn * turn + bendCompliance);
            const float3 turned = cross(cross(offset, multiplier), direction) / parentLength; // transpose(M) * multiplier
            state.position += multiplier;
            parent.position -= parentWeight * (multiplier + turned);
            grandparent.position += grandparentWeight * turned;
            guidePoints[base + pointIndex].position = state.position;
            guidePoints[base + pointIndex - 1u].position = parent.position;
            guidePoints[base + pointIndex - 2u].position = grandparent.position;
            grandparent = parent;
            parent = state;
        }

        // 5 and 6. Collide and solve stretch, HAIR_STRETCH_PASSES times, then collide once more: the substep ends with
        // no point inside a collider. The last collision applies friction.
        const uint lastPass = HAIR_STRETCH_PASSES;
        for (uint pass = 0u; pass <= lastPass; ++pass)
        {
            for (pointIndex = 2u; pointIndex < pointCount && colliderCount != 0u; ++pointIndex)
            {
                HairGuidePoint state = guidePoints[base + pointIndex];
                bool moved = false;
                for (colliderIndex = 0u; colliderIndex < colliderCount; ++colliderIndex)
                {
                    const HairColliderMotion motion = motions[colliderIndex];
                    const float3 a = lerp(motion.a0, motion.a1, frameShare);
                    const float3 b = lerp(motion.b0, motion.b1, frameShare);
                    const float radius = motion.radius + sim.collisionMargin;
                    float3 nearest;
                    float parameter;
                    const float gap = HairCapsuleDistance(state.position, a, b, radius, nearest, parameter);
                    if (gap >= 0.0f)
                    {
                        continue;
                    }
                    const float3 normal = HairSafeNormalize(state.position - nearest, float3(0.0f, 1.0f, 0.0f));
                    state.position = nearest + normal * radius;
                    moved = true;
                    if (pass != lastPass || !advance)
                    {
                        continue;
                    }
                    // Friction: the point's motion along the collider's surface, relative to the collider's own this substep.
                    const float3 colliderStep = lerp(motion.a1 - motion.a0, motion.b1 - motion.b0, parameter) / (float)substepCount;
                    const float3 relative = state.position - state.solverPrevious - colliderStep;
                    const float outward = dot(relative, normal);
                    const float3 kept = (relative - normal * outward) * (1.0f - sim.friction) + normal * max(outward, 0.0f);
                    state.solverPrevious = state.position - kept - colliderStep;
                }
                if (moved)
                {
                    guidePoints[base + pointIndex] = state;
                }
            }

            if (pass == lastPass)
            {
                break;
            }

            // Stretch. Constraint c joins points c and c + 1, for c = 1 .. n - 2 (segment 0 is kinematic):
            // C = |x[c+1] - x[c]| - rest, with unit gradient n[c] at the upper point and -n[c] at the lower. With unit
            // inverse masses (0 for point 1) the system for the multipliers has the diagonal w[c] + 1 + compliance and
            // the off-diagonals -dot(n[c-1], n[c]). Forward elimination keeps each row's pivot and right side in the
            // upper point's scratch; back substitution moves the points.
            float3 normalBefore = float3(0.0f, 0.0f, 0.0f);
            float pivotBefore = 1.0f;
            float solvedBefore = 0.0f;
            uint constraint;
            for (constraint = 1u; constraint + 1u < pointCount; ++constraint)
            {
                const HairGuidePoint lower = guidePoints[base + constraint];
                const HairGuidePoint upper = guidePoints[base + constraint + 1u];
                const float3 delta = upper.position - lower.position;
                const float3 restDelta = upper.rigid - lower.rigid;
                const float3 normal = HairSafeNormalize(delta, HairSafeNormalize(restDelta, follow.basis[2]));
                const float lowerWeight = constraint == 1u ? 0.0f : 1.0f;
                const float offDiagonal = -lowerWeight * dot(normalBefore, normal);
                const float pivot = lowerWeight + 1.0f + stretchCompliance - offDiagonal * offDiagonal / pivotBefore;
                const float solved = (length(restDelta) - length(delta) - offDiagonal * solvedBefore) / pivot;
                guidePoints[base + constraint + 1u].scratch0 = pivot;
                guidePoints[base + constraint + 1u].scratch1 = solved;
                normalBefore = normal;
                pivotBefore = pivot;
                solvedBefore = solved;
            }
            float3 normalAfter = float3(0.0f, 0.0f, 0.0f);
            float multiplierAfter = 0.0f;
            for (constraint = pointCount - 2u; constraint >= 1u; --constraint)
            {
                const HairGuidePoint lower = guidePoints[base + constraint];
                HairGuidePoint upper = guidePoints[base + constraint + 1u];
                const float3 normal = HairSafeNormalize(upper.position - lower.position,
                    HairSafeNormalize(upper.rigid - lower.rigid, follow.basis[2]));
                const float multiplier = upper.scratch1 + dot(normal, normalAfter) * multiplierAfter / upper.scratch0;
                upper.position += normal * multiplier - normalAfter * multiplierAfter;
                guidePoints[base + constraint + 1u].position = upper.position;
                normalAfter = normal;
                multiplierAfter = multiplier;
            }
        }
    }

    // What a substep of no length moved keeps its velocity.
    for (pointIndex = 2u; pointIndex < pointCount && substepCount != 0u && !advance; ++pointIndex)
    {
        const HairGuidePoint state = guidePoints[base + pointIndex];
        guidePoints[base + pointIndex].solverPrevious = state.solverPrevious + (state.position - state.previousPosition - rootMove);
    }

    // What the frame left, for the CPU: stretch, speed, distance from the rigid pose and clearance of the simulated
    // points (2 and up), and how far the frame moved them relative to the root.
    float stretchMax = 0.0f;
    float stretchSum = 0.0f;
    float speedMax = 0.0f;
    float speedSum = 0.0f;
    float deviationSum = 0.0f;
    float clearance = 1.0e6f;
    float motion = 0.0f;
    const HairGuidePoint rootPoint = guidePoints[base];
    const float3 rootMotion = rootPoint.position - rootPoint.previousPosition;
    HairGuidePoint lowerPoint = guidePoints[base + min(1u, pointCount - 1u)];
    for (pointIndex = 2u; pointIndex < pointCount; ++pointIndex)
    {
        const HairGuidePoint state = guidePoints[base + pointIndex];
        const float restLength = distance(state.rigid, lowerPoint.rigid);
        const float stretch = restLength > 1.0e-9f ? abs(distance(state.position, lowerPoint.position) / restLength - 1.0f) : 0.0f;
        stretchMax = max(stretchMax, stretch);
        stretchSum += stretch;
        const float speed = state.solverSeconds > 0.0f ? distance(state.position, state.solverPrevious) / state.solverSeconds : 0.0f;
        speedMax = max(speedMax, speed);
        speedSum += speed;
        deviationSum += distance(state.position, state.rigid);
        motion = max(motion, HairGuidePointMotion(state, rootMotion));
        for (colliderIndex = 0u; colliderIndex < colliderCount; ++colliderIndex)
        {
            float3 nearest;
            float parameter;
            clearance = min(clearance, HairCapsuleDistance(state.position, motions[colliderIndex].a1, motions[colliderIndex].b1,
                motions[colliderIndex].radius, nearest, parameter));
        }
        lowerPoint = state;
    }
    const uint metricBase = instanceIndex * HAIR_SIM_METRIC_COUNT;
    const float simulatedPoints = (float)max(pointCount, 3u) - 2.0f;
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_GUIDES], 1u);
    InterlockedMax(metrics[metricBase + HAIR_SIM_METRIC_STRETCH_MAX], asuint(stretchMax));
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_STRETCH_SUM],
        (uint)min(stretchSum / simulatedPoints / HAIR_SIM_METRIC_STRETCH_UNIT + 0.5f, HAIR_SIM_METRIC_SUM_CAP));
    InterlockedMax(metrics[metricBase + HAIR_SIM_METRIC_SPEED_MAX], asuint(speedMax));
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_SPEED_SUM],
        (uint)min(speedSum / simulatedPoints / HAIR_SIM_METRIC_SPEED_UNIT + 0.5f, HAIR_SIM_METRIC_SUM_CAP));
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_DEVIATION_SUM],
        (uint)min(deviationSum / simulatedPoints / HAIR_SIM_METRIC_DEVIATION_UNIT + 0.5f, HAIR_SIM_METRIC_SUM_CAP));
    if (colliderCount != 0u && pointCount > 2u)
    {
        const int units = (int)clamp(clearance / HAIR_SIM_METRIC_DISTANCE_UNIT, -2.0e9f, 2.0e9f);
        InterlockedMin(metrics[metricBase + HAIR_SIM_METRIC_COLLIDER_DISTANCE], (uint)units + HAIR_SIM_METRIC_DISTANCE_BIAS);
    }
    if (stretchMax > HAIR_SIM_METRIC_STRETCH_LIMIT)
    {
        InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_STRETCHED_GUIDES], 1u);
    }
    InterlockedMax(metrics[metricBase + HAIR_SIM_METRIC_ROOT_SHIFT], asuint(length(rootShift)));
    if (reset)
    {
        InterlockedAdd(metrics[metricBase + (keepShape ? HAIR_SIM_METRIC_RESUMED_GUIDES : HAIR_SIM_METRIC_RESET_GUIDES)], 1u);
    }
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_LEVEL_GUIDES + min(binding.level, HAIR_MAX_GUIDE_LEVELS - 1u)], 1u);
    InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_SIMULATED_POINTS], pointCount);
    if (promoted)
    {
        InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_PROMOTED_GUIDES], 1u);
    }
    if ((guideState & (HAIR_GUIDE_STATE_PROMOTING | HAIR_GUIDE_STATE_DEMOTING)) != 0u)
    {
        // The guide-level pass pulls it toward its evaluated state yet, and measures its motion after that.
        InterlockedAdd(metrics[metricBase + HAIR_SIM_METRIC_BLENDING_GUIDES], 1u);
    }
    else
    {
        InterlockedMax(metrics[metricBase + ((guideState & HAIR_GUIDE_STATE_CHANGED) != 0u
            ? HAIR_SIM_METRIC_TRANSITION_MOTION : HAIR_SIM_METRIC_STEADY_MOTION)], asuint(motion));
    }
}
