#ifndef CLOD_RAY_TRACING_REFLECTION_HIT_HLSLI
#define CLOD_RAY_TRACING_REFLECTION_HIT_HLSLI

#define CLOD_RT_REFLECTION_NO_HIT 0xFFFFFFFFu

struct CLodRtReflectionHit
{
    uint drawRecord; // CLOD_RT_REFLECTION_NO_HIT when the pixel has nothing to shade.
    uint clasSlot;
    uint primitive;
    uint frontFace;
    float2 barycentrics;
    float t;
    float coneWidth;
    float3 origin;
    float pad0;
    float3 direction;
    float pad1;
};

float3 CLodRtSampleEnvironment(float3 direction, float perceptualRoughness)
{
    ConstantBuffer<PerFrameBuffer> frame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
    TextureCube<float4> prefiltered = ResourceDescriptorHeap[environments[frame.activeEnvironmentIndex].prefilteredCubemapDescriptorIndex];
    uint width = 1u;
    uint height = 1u;
    uint mips = 1u;
    prefiltered.GetDimensions(0u, width, height, mips);
    return prefiltered.SampleLevel(g_linearClamp, direction, saturate(perceptualRoughness) * float(mips - 1u)).rgb;
}

#endif // CLOD_RAY_TRACING_REFLECTION_HIT_HLSLI
