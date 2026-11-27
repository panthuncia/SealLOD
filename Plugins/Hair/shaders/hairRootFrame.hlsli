#ifndef HAIR_ROOT_FRAME_HLSLI
#define HAIR_ROOT_FRAME_HLSLI

// A strand root's frame in world space, from its rest frame. No buffer holds root frames: every pass that needs one
// works it out here, from the root's binding, the entity's row and the bone palettes of the frame: the select pass for
// a cluster's first strand, the evaluate pass for each drawn strand, the simulate and guide-level passes for each guide
// they write. So a root's frame is always this frame's, and the passes cannot drift.
//
// A root's rest frame is in the entity's mesh space. It is skinned like a vertex of the entity's mesh (row-vector:
// position * skin, then * model) when the instance names a skinning slot, by one joint (attachBone) or by the root's
// own joints and weights. Without a slot only the entity's model matrix applies. Skeleton LOD remaps (procedural wind's
// transient palettes) are not followed.
//
// Include after include/structs.hlsli and hairStructs.hlsli. The bone palettes and the skinning instance rows are bound
// by name, as in the renderer's shaders: the pass declares them.

// As skinningCommon.hlsli reads Builtin::SkeletonResources::BoneTransforms.
typedef row_major float4x4 HairRootSkinMatrix;

static const float4x4 HairRootIdentity = float4x4(
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

void HairLoadRootSkin(HairInstanceGPU instance, HairRootBinding binding, out float4x4 current, out float4x4 previous)
{
    current = HairRootIdentity;
    previous = HairRootIdentity;
    if (instance.skinningInstanceSlot == HAIR_INVALID_INDEX)
    {
        return;
    }
    StructuredBuffer<SkinningInstanceGPUInfo> infos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
    StructuredBuffer<HairRootSkinMatrix> bones =
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

HairRootFrame HairMakeRootFrame(HairRootBinding binding, float4x4 skin, float4x4 model, uint frameNumber)
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

// A root's frame this frame and under the previous frame's matrices (the previous bone palette and model matrix).
HairRootFramePair HairMakeRootFramePair(HairInstanceGPU instance, HairRootBinding binding, PerObjectBuffer object, uint frameNumber)
{
    float4x4 skin;
    float4x4 previousSkin;
    HairLoadRootSkin(instance, binding, skin, previousSkin);
    HairRootFramePair pair;
    pair.current = HairMakeRootFrame(binding, skin, object.model, frameNumber);
    pair.previous = HairMakeRootFrame(binding, previousSkin, object.prevModel, frameNumber);
    return pair;
}

#endif // HAIR_ROOT_FRAME_HLSLI
