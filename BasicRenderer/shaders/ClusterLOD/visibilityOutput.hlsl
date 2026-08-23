#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/visibilityPacking.hlsli"

static const uint CLOD_TELEMETRY_DISABLED_DESCRIPTOR = 0xFFFFFFFFu;
static const uint WG_COUNTER_RASTER_PIXEL_SHADER_INVOCATIONS = 125u;
static const uint WG_COUNTER_RASTER_PIXEL_SCISSOR_REJECTED = 126u;
static const uint WG_COUNTER_RASTER_PIXEL_TARGET_BOUNDS_REJECTED = 127u;
static const uint WG_COUNTER_RASTER_PIXEL_VISIBILITY_WRITES = 128u;

#ifndef CLOD_RASTER_PIXEL_TELEMETRY
#define CLOD_RASTER_PIXEL_TELEMETRY 0
#endif

void CLodRasterPixelTelemetryAdd(uint counterIndex, uint value)
{
#if CLOD_RASTER_PIXEL_TELEMETRY
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == CLOD_TELEMETRY_DISABLED_DESCRIPTOR || value == 0u)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
#endif
}

[shader("pixel")]
void VisibilityBufferPSMain(VisBufferPSInput input, bool isFrontFace : SV_IsFrontFace, uint primID : SV_PrimitiveID) : SV_TARGET
{
    CLodRasterPixelTelemetryAdd(WG_COUNTER_RASTER_PIXEL_SHADER_INVOCATIONS, 1u);

    uint2 pixel = input.position.xy;
    uint visBufferUAVIndex = CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX;
#if !defined(CLOD_RASTER_SINGLE_VIEW)
    if (visBufferUAVIndex == 0xFFFFFFFFu)
    {
        StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
            ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
        ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[input.viewID];

        if (pixel.x < viewRasterInfo.scissorMinX ||
            pixel.y < viewRasterInfo.scissorMinY ||
            pixel.x >= viewRasterInfo.scissorMaxX ||
            pixel.y >= viewRasterInfo.scissorMaxY)
        {
            CLodRasterPixelTelemetryAdd(WG_COUNTER_RASTER_PIXEL_SCISSOR_REJECTED, 1u);
            return;
        }
        visBufferUAVIndex = viewRasterInfo.visibilityUAVDescriptorIndex;
    }
#endif

    RWTexture2D<uint64_t> visBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(visBufferUAVIndex)];

    // Need to check alpha for alpha-tested materials
#if defined(PSO_ALPHA_TEST)
    TestAlpha(input.texcoord, input.materialDataIndex);
#endif

    // High 32 bits = depth
    uint64_t output = PackVisKey(input.linearDepth, input.visibleClusterIndex, primID);

    InterlockedMin(visBuffer[pixel], output);
    CLodRasterPixelTelemetryAdd(WG_COUNTER_RASTER_PIXEL_VISIBILITY_WRITES, 1u);
}
