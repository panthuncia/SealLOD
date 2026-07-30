#include "include/vertex.hlsli"
#include "include/utilities.hlsli"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/materialflags.hlsli"
#include "include/lighting.hlsli"
#include "include/gammaCorrection.hlsli"
#include "include/outputTypes.hlsli"
#include "include/debugPayload.hlsli"

[numthreads(8, 8, 1)]
void DeferredCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    uint screenW = perFrameBuffer.screenResX;
    uint screenH = perFrameBuffer.screenResY;

    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
    {
        return;
    }
    
    uint2 pixel = dispatchThreadId.xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(screenW, screenH);
    uv.y = 1.0f - uv.y;

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];
    
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    float depth = depthTexture[pixel];
    if (asuint(depth) == 0x7F7FFFFF) // TODO: When we need more shading paths, we will move to prefix sum and indirect dispatch
    {
        if (GetRootEnableSkybox())
        {
            const float2 ndc = uv * 2.0f - 1.0f;
            const float3 viewDir = normalize(float3(
                ndc.x * mainCamera.projectionInverse[0][0],
                ndc.y * mainCamera.projectionInverse[1][1],
                -1.0f));
            const float3 worldDir = normalize(mul(float4(viewDir, 0.0f), mainCamera.viewInverse).xyz);

            const float3 currentViewDir = normalize(mul(float4(worldDir, 0.0f), mainCamera.view).xyz);
            const float4 currentClip = mul(float4(currentViewDir, 1.0f), mainCamera.unjitteredProjection);
            const float2 currentNdc = currentClip.xy / max(abs(currentClip.w), 1e-6f);

            const float3 prevViewDir = normalize(mul(float4(worldDir, 0.0f), mainCamera.prevView).xyz);
            const float4 prevClip = mul(float4(prevViewDir, 1.0f), mainCamera.prevUnjitteredProjection);
            const float2 prevNdc = prevClip.xy / max(abs(prevClip.w), 1e-6f);

            StructuredBuffer<EnvironmentInfo> environmentInfo =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
            const EnvironmentInfo envInfo = environmentInfo[perFrameBuffer.activeEnvironmentIndex];
            TextureCube<float4> skyboxTexture = ResourceDescriptorHeap[envInfo.cubeMapDescriptorIndex];

            RWTexture2D<float4> hdrTarget =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
            hdrTarget[pixel] = float4(skyboxTexture.SampleLevel(g_linearClamp, worldDir, 0.0f).rgb, 1.0f);

            RWTexture2D<float2> motionVectors =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::GBuffer::MotionVectors)];
            motionVectors[pixel] = currentNdc - prevNdc;
        }
        return;
    }
    
    //float linearZ = unprojectDepth(depth, mainCamera.zNear, mainCamera.zFar);
    float linearZ = depth;

    float2 ndc = uv * 2.0f - 1.0f;
    // The renderer's infinite reverse-Z perspective projection has no skew or
    // jitter terms: projectionInverse transforms (ndc, 1, 1) to
    // (ndc.x / xScale, ndc.y / yScale, -1, 0). Reconstruct that ray directly
    // instead of issuing a general 4x4 matrix multiply.
    float3 positionVS = float3(
        ndc.x * mainCamera.projectionInverse[0][0],
        ndc.y * mainCamera.projectionInverse[1][1],
        -1.0f) * linearZ;

    float4 worldPosH = mul(float4(positionVS, 1.0f), mainCamera.viewInverse);
    float3 positionWS = worldPosH.xyz;

    float3 viewDirWS = mul(
        float4(normalize(-positionVS), 0.0f),
        mainCamera.viewInverse).xyz;

    FragmentInfo fragmentInfo;
    GetFragmentInfoScreenSpace(pixel, viewDirWS, positionVS, positionWS, GetRootEnableGTAO(), fragmentInfo);
    
    LightingOutput lightingOutput = lightFragment(fragmentInfo, mainCamera, perFrameBuffer.activeEnvironmentIndex, ResourceDescriptorIndex(Builtin::Environment::InfoBuffer), true);
    
    float3 lighting = lightingOutput.lighting;

    RWTexture2D<float4> hdrTarget = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    
    // Always write lighting to HDR target
    hdrTarget[pixel] = float4(lighting, 1.0);

    // Write debug payload for lighting-derived modes
    if (perFrameBuffer.outputType != OUTPUT_COLOR) {
        RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
        uint2 payload = uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
        switch (perFrameBuffer.outputType) {
            case OUTPUT_AO:
                payload = PackDebugFloat3(fragmentInfo.diffuseAmbientOcclusion.xxx);
                break;
            case OUTPUT_DEPTH: {
                float scaledDepth = abs(linearZ) * 0.1;
                payload = PackDebugFloat3(scaledDepth.xxx);
                break;
            }
#if defined(PSO_IMAGE_BASED_LIGHTING)
            case OUTPUT_DIFFUSE_IBL:
                payload = PackDebugFloat3(lightingOutput.diffuseIBL.rgb);
                break;
            case OUTPUT_SPECULAR_IBL:
                payload = PackDebugFloat3(lightingOutput.specularIBL.rgb);
                break;
#endif
#if defined(PSO_CLUSTERED_LIGHTING)
            case OUTPUT_LIGHT_CLUSTER_ID:
                payload = PackDebugUint(lightingOutput.clusterIndex);
                break;
            case OUTPUT_LIGHT_CLUSTER_LIGHT_COUNT:
                payload = PackDebugUint(lightingOutput.clusterLightCount);
                break;
            case OUTPUT_VSM_PREFERRED_CLIPMAP:
            case OUTPUT_VSM_SAMPLED_CLIPMAP:
            case OUTPUT_VSM_PAGE_STATE:
            case OUTPUT_VSM_PHYSICAL_PAGE:
            case OUTPUT_VSM_RERENDERED_THIS_FRAME:
            case OUTPUT_VSM_CACHED_BASIS_CORRECTION:
            case OUTPUT_VSM_PAGE_LOCAL_TEXEL:
            case OUTPUT_VSM_DEPTH_MARGIN:
            case OUTPUT_VSM_CLIP_COMPARISON:
            case OUTPUT_VSM_CLIP_GRID_OFFSET:
            case OUTPUT_VSM_TRACE_FOOTPRINT:
                payload = lightingOutput.shadowDebugPayload;
                break;
#endif
        }
        if (payload.x != DEBUG_SENTINEL) {
            WriteDebugPixel(debugVisTex, pixel, payload);
        }
    }
}
