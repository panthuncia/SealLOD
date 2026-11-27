// Root binding: one dispatch per groom instance, one thread per strand root. Writes each root's frame (position,
// normal, tangent) in world space for this frame and for the previous frame's matrices.
//
// A root's rest frame is in the entity's mesh space. It is skinned like a vertex of the entity's mesh (row-vector:
// position * skin, then * model) when the instance names a skinning slot, by one joint (attachBone) or by the root's
// own joints and weights. Without a slot only the entity's model matrix applies. Skeleton LOD remaps (procedural wind's
// transient palettes) are not followed.

// Paths are relative to the renderer's runtime root, where the build stages this folder as HairShaders/.
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "HairShaders/hairStructs.hlsli"

// HairRootBindingPass::Record. The renderer's own buffers (per-object rows, bone palettes, skinning instance rows) are
// bound by name, as in the renderer's shaders.
#define HAIR_INSTANCES UintRootConstant0     // StructuredBuffer<HairInstanceGPU>
#define HAIR_ROOT_BINDINGS UintRootConstant1 // StructuredBuffer<HairRootBinding>
#define HAIR_ROOT_FRAMES UintRootConstant2   // RWStructuredBuffer<HairRootFramePair>
#define HAIR_COUNTERS UintRootConstant3      // RWStructuredBuffer<uint>
#define HAIR_FRAME_STATE UintRootConstant4   // StructuredBuffer<HairFrameGPU>
#define HAIR_INSTANCE_INDEX UintRootConstant5

// As skinningCommon.hlsli reads Builtin::SkeletonResources::BoneTransforms.
typedef row_major float4x4 HairSkinMatrix;

static const float4x4 HairIdentity = float4x4(
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

void LoadRootSkin(HairInstanceGPU instance, HairRootBinding binding, out float4x4 current, out float4x4 previous)
{
    current = HairIdentity;
    previous = HairIdentity;
    if (instance.skinningInstanceSlot == HAIR_INVALID_INDEX)
    {
        return;
    }
    StructuredBuffer<SkinningInstanceGPUInfo> infos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
    StructuredBuffer<HairSkinMatrix> bones =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];
    const SkinningInstanceGPUInfo info = infos[instance.skinningInstanceSlot];
    if (instance.attachBone != HAIR_INVALID_INDEX)
    {
        if (instance.attachBone < info.boneCount)
        {
            current = bones[info.transformOffsetMatrices + instance.attachBone];
            previous = bones[info.previousTransformOffsetMatrices + instance.attachBone];
        }
        return;
    }
    float4x4 currentSum = (float4x4)0;
    float4x4 previousSum = (float4x4)0;
    float weightSum = 0.0f;
    for (uint i = 0u; i < 4u; ++i)
    {
        const float weight = binding.weights[i];
        const uint joint = binding.joints[i];
        if (weight > 0.0f && joint < info.boneCount)
        {
            currentSum += weight * bones[info.transformOffsetMatrices + joint];
            previousSum += weight * bones[info.previousTransformOffsetMatrices + joint];
            weightSum += weight;
        }
    }
    if (weightSum > 1.0e-8f)
    {
        current = currentSum * rcp(weightSum);
        previous = previousSum * rcp(weightSum);
    }
}

HairRootFrame MakeRootFrame(HairRootBinding binding, float4x4 skin, float4x4 model, uint frameNumber)
{
    HairRootFrame frame;
    const float3 skinned = mul(float4(binding.restPosition, 1.0f), skin).xyz;
    frame.position = mul(float4(skinned, 1.0f), model).xyz;
    frame.writtenFrame = frameNumber;
    frame.normal = normalize(mul(mul(binding.restNormal, (float3x3)skin), (float3x3)model));
    frame.pad0 = 0.0f;
    frame.tangent = normalize(mul(mul(binding.restTangent, (float3x3)skin), (float3x3)model));
    frame.pad1 = 0.0f;
    return frame;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairRootBindingCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    const HairInstanceGPU instance = instances[HAIR_INSTANCE_INDEX];
    if (dispatchThreadId.x == 0u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
        InterlockedAdd(counters[HAIR_COUNTER_ROOT_BINDING_DISPATCHES], 1u);
    }
    if (dispatchThreadId.x >= instance.strandCount)
    {
        return;
    }

    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<HairRootFramePair> rootFrames = ResourceDescriptorHeap[HAIR_ROOT_FRAMES];

    const HairRootBinding binding = rootBindings[instance.strandBase + dispatchThreadId.x];
    const PerObjectBuffer object = perObject[instance.perObjectIndex];
    const uint frameNumber = frameState[0].frameNumber;

    float4x4 skin;
    float4x4 previousSkin;
    LoadRootSkin(instance, binding, skin, previousSkin);

    HairRootFramePair pair;
    pair.current = MakeRootFrame(binding, skin, object.model, frameNumber);
    pair.previous = MakeRootFrame(binding, previousSkin, object.prevModel, frameNumber);
    rootFrames[instance.rootFrameBase + dispatchThreadId.x] = pair;
}
