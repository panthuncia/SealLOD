#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "fullscreenVS.hlsli"
#include "include/gammaCorrection.hlsli"
#include "include/outputTypes.hlsli"
#include "include/debugPayload.hlsli"

float3 MaterialSelectedMipDebugColor(uint selectedMipLevel, uint selectedMipMaxLevel)
{
    const float t = selectedMipMaxLevel > 0u ? saturate((float)selectedMipLevel / (float)selectedMipMaxLevel) : 0.0f;
    if (t < 0.25f)
    {
        return lerp(float3(0.10f, 0.20f, 0.95f), float3(0.10f, 0.80f, 1.00f), t / 0.25f);
    }
    if (t < 0.50f)
    {
        return lerp(float3(0.10f, 0.80f, 1.00f), float3(0.15f, 0.90f, 0.20f), (t - 0.25f) / 0.25f);
    }
    if (t < 0.75f)
    {
        return lerp(float3(0.15f, 0.90f, 0.20f), float3(0.98f, 0.80f, 0.15f), (t - 0.50f) / 0.25f);
    }

    return lerp(float3(0.98f, 0.80f, 0.15f), float3(0.95f, 0.20f, 0.15f), (t - 0.75f) / 0.25f);
}

float4 PSMain(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    // The fullscreen triangle covers the output resolution.
    // Map the UV back to render-resolution pixel coordinates.
    float2 uv = input.uv;
    uv.y = 1.0f - uv.y;

    uint renderW = perFrameBuffer.screenResX;
    uint renderH = perFrameBuffer.screenResY;

    uint2 pixel = uint2(uv * float2(renderW, renderH));
    pixel = min(pixel, uint2(renderW - 1, renderH - 1));

    Texture2D<uint2> debugTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    uint2 payload = debugTex[pixel];

    // Sentinel means no debug data was written, show the tonemapped scene.
    if (payload.x == DEBUG_SENTINEL && payload.y == DEBUG_SENTINEL) {
        discard;
    }

    float3 color = float3(1, 0, 1); // magenta fallback
    uint outputType = perFrameBuffer.outputType;

    switch (outputType) {
        case OUTPUT_NORMAL:
        case OUTPUT_ALBEDO:
        case OUTPUT_TERRAIN_GRASS_OVERLAY:
        case OUTPUT_METALLIC:
        case OUTPUT_ROUGHNESS:
        case OUTPUT_EMISSIVE:
        case OUTPUT_AO:
        case OUTPUT_DEPTH:
        case OUTPUT_DIFFUSE_IBL:
        case OUTPUT_SPECULAR_IBL:
        case OUTPUT_MODEL_NORMALS:
        case OUTPUT_MOTION_VECTORS:
        case OUTPUT_MATERIAL_UV:
        case OUTPUT_MATERIAL_UV_DERIVATIVE:
        case OUTPUT_REYES_SOURCE_BARYCENTRICS:
        case OUTPUT_MATERIAL_EVAL_FEATURES:
        case OUTPUT_REYES_GEOMETRY_PATH:
        case OUTPUT_VOXEL_GEOMETRY_PATH:
        case OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE:
        case OUTPUT_VSM_PREFERRED_CLIPMAP:
        case OUTPUT_VSM_SAMPLED_CLIPMAP:
        case OUTPUT_VSM_PAGE_STATE:
        case OUTPUT_VSM_RERENDERED_THIS_FRAME:
        case OUTPUT_TRANSPARENT_DEPTH_COMPLEXITY:
        case OUTPUT_PARALLAX_PIXELS:
        case OUTPUT_TERRAIN_RVT_HIT:
        case OUTPUT_TERRAIN_RVT_PAGE_UV:
        case OUTPUT_TERRAIN_RVT_ATLAS_UV:
        case OUTPUT_TERRAIN_RVT_SAMPLED_ALBEDO:
        case OUTPUT_TERRAIN_RVT_PHYSICAL_TILE_UV:
        case OUTPUT_TERRAIN_RVT_SAMPLED_NORMAL:
        case OUTPUT_TERRAIN_RVT_SAMPLED_MATERIAL:
        case OUTPUT_TERRAIN_RVT_SAMPLED_ALBEDO_POINT:
        case OUTPUT_TERRAIN_RVT_HEIGHT_SCALE:
        case OUTPUT_TERRAIN_GEOMETRIC_HEIGHT:
            color = UnpackDebugFloat3(payload);
            break;
        case OUTPUT_TRANSPARENT_VBOIT_TRANSMITTANCE:
        case OUTPUT_TRANSPARENT_VBOIT_COVERAGE:
        case OUTPUT_TRANSPARENT_VBOIT_ZERO_SLICE:
        case OUTPUT_TRANSPARENT_VBOIT_VIRTUAL_SLICE_COUNT:
        case OUTPUT_TRANSPARENT_VBOIT_PHYSICAL_SLICE_COUNT:
        case OUTPUT_TRANSPARENT_VBOIT_FITTED_VIRTUAL_SLICE_COUNT:
        case OUTPUT_TRANSPARENT_VBOIT_OCCUPIED_VIRTUAL_SLICE_COUNT:
        case OUTPUT_TRANSPARENT_VBOIT_DEPTH_DISTRIBUTION_EXPONENT:
            color = UnpackDebugFloat1(payload).xxx;
            break;
        case OUTPUT_SW_RASTER:
            color = float3(1, 0, 0);
            break;
        case OUTPUT_TRANSPARENT_NODE_COUNT:
        case OUTPUT_TRANSPARENT_RESOLVED_SAMPLE_COUNT:
            color = saturate(float(UnpackDebugUint(payload)) / 16.0f).xxx;
            break;
        case OUTPUT_MESHLETS:
        case OUTPUT_GEOMETRY_GROUP:
        case OUTPUT_CLOD_ASSEMBLY_PARTS:
        case OUTPUT_LIGHT_CLUSTER_ID:
        case OUTPUT_VSM_PHYSICAL_PAGE:
        case OUTPUT_TERRAIN_RVT_VIRTUAL_PAGE:
        case OUTPUT_TERRAIN_RVT_PHYSICAL_PAGE:
        case OUTPUT_TERRAIN_RVT_ATLAS_POOL:
        case OUTPUT_TERRAIN_RVT_FALLBACK_REASON:
        case OUTPUT_TERRAIN_RVT_OWNER_PAGE:
        case OUTPUT_TERRAIN_RVT_PAGE_DELTA:
            color = HashToColor(UnpackDebugUint(payload));
            break;
        case OUTPUT_LIGHT_CLUSTER_LIGHT_COUNT:
            color = HashToColor(UnpackDebugUint(payload));
            break;
        case OUTPUT_MATERIAL_SELECTED_MIP:
        case OUTPUT_TERRAIN_RVT_REQUESTED_MIP:
        case OUTPUT_TERRAIN_RVT_RESIDENT_MIP:
            color = MaterialSelectedMipDebugColor(payload.x, max(payload.x, payload.y));
            break;
    }

    color = LinearToSRGB(color);
    return float4(color, 1.0);
}
