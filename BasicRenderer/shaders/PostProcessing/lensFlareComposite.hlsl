// Ghost flares, stage 4 (GhostFlareManager): adds the flare target (which may be smaller than the output) into the upscaled
// HDR colour, bilinearly, through additive blending. It runs after the upscaler, so Ray Reconstruction never sees ghosts,
// and before bloom and tonemapping.
#include "include/cbuffers.hlsli"
#include "fullscreenVS.hlsli"
#include "include/lens/lensGhostLayout.h"

float4 LensFlareCompositePS(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    Texture2D<float4> flare = ResourceDescriptorHeap[LENS_FLARE_RC_FLARE_SRV];
    const float2 uv = float2(input.uv.x, 1.0f - input.uv.y);
    return float4(flare.SampleLevel(g_linearClamp, uv, 0.0f).rgb, 0.0f);
}
