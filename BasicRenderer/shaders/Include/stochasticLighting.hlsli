#ifndef __STOCHASTIC_LIGHTING_HLSLI__
#define __STOCHASTIC_LIGHTING_HLSLI__

// What stochastic lighting's passes share (docs/stochastic_lighting.md): the primary surface rebuilt from the canonical
// surface textures exactly as the deferred pass builds it, a light's unshadowed contribution through the raster light
// loop's own functions, and one shadow ray toward a point on the light's sphere (or a direction in a directional light's
// disc).
//
// The shading convention matches raster (and Unreal's MegaLights): a light's contribution is its unshadowed point-light
// contribution (EvaluateDirectLightContribution) times the visibility of one ray toward a point drawn uniformly from the
// solid angle its sphere (sourceRadius) subtends, or from a directional light's disc (sourceHalfAngleDegrees). Its expected
// value is that contribution times the visible fraction of the emitter, so a lit, unoccluded surface matches the
// unshadowed loop exactly.
//
// Include after lighting.hlsli (with LIGHTING_NO_VIRTUAL_SHADOW_MAPS), clodRayTracingHits.hlsli and
// stochasticLightingRootConstants.h.

// A light with no source radius is still a sphere this size: the light model's falloff floor (LIGHT_MIN_DISTANCE_SQUARED).
#define STOCHASTIC_MIN_SOURCE_RADIUS 0.01f
// How far a directional light's shadow ray looks, as the ray traced sun shadow's (rayTracedShadows.hlsl).
#define STOCHASTIC_DIRECTIONAL_RAY_LENGTH 100000.0f
// Shadow rays take the first hit, and skip back faces: a ray leaving a surface would otherwise hit the inside of a coarser
// BVH LOD of that same surface. Double-sided materials' instances disable culling (rayTracingSceneBuild.hlsl).
#ifndef STOCHASTIC_SHADOW_RAY_FLAGS
#define STOCHASTIC_SHADOW_RAY_FLAGS (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES)
#endif

float StochasticLuminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

uint StochasticHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct StochasticRng
{
    uint state;
};

// A stream of uniform numbers, different for every pixel, frame and stream.
StochasticRng StochasticRngCreate(uint2 pixel, uint frame, uint stream)
{
    StochasticRng rng;
    rng.state = StochasticHash(pixel.x ^ StochasticHash(pixel.y ^ StochasticHash(frame * 0x9E3779B9u + stream)));
    return rng;
}

float StochasticRngNext(inout StochasticRng rng)
{
    rng.state = rng.state * 747796405u + 2891336453u;
    uint word = ((rng.state >> ((rng.state >> 28u) + 4u)) ^ rng.state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8u) * (1.0f / 16777216.0f);
}

float2 StochasticRngNext2(inout StochasticRng rng)
{
    const float x = StochasticRngNext(rng);
    return float2(x, StochasticRngNext(rng));
}

// One number per pixel, frame and dimension: a hashed per-pixel offset advanced by the golden ratio each frame (an R1 Weyl
// sequence). Every pixel's sequence is equidistributed over time, so an estimator driven by it stays unbiased per pixel,
// and it is low-discrepancy in time, which the temporal accumulation likes; in space it is white. It stands in for
// spatiotemporal blue noise (STBN) textures, which are not vendored. (Interleaved gradient noise with a per-frame shift was
// tried first: its 64-frame cycle is not equidistributed at a fixed pixel, and each pixel converged to a biased value.)
float StochasticTemporalSequence(uint2 pixel, uint frame, uint dimension)
{
    const float offset = float(StochasticHash(pixel.x ^ StochasticHash(pixel.y ^ StochasticHash(dimension * 0x632BE5ABu))) >> 8u)
        * (1.0f / 16777216.0f);
    return frac(offset + float(frame & 0xFFFFFu) * 0.61803398875f);
}

struct StochasticSurface
{
    FragmentInfo fragment;
    LightingParameters parameters;
    OpenPBRBaseLayerState baseState;
    float3 position;
    float distanceToCamera;
};

// The primary surface at a pixel, as deferred.hlsl rebuilds it. False for background pixels.
bool StochasticLoadSurface(uint2 pixel, PerFrameBuffer perFrame, Camera camera, out StochasticSurface surface)
{
    surface = (StochasticSurface)0;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    const float linearZ = depthTexture[pixel];
    if (asuint(linearZ) == 0x7F7FFFFF)
    {
        return false;
    }
    float2 uv = (float2(pixel) + 0.5f) / float2(perFrame.screenResX, perFrame.screenResY);
    uv.y = 1.0f - uv.y;
    const float2 ndc = uv * 2.0f - 1.0f;
    const float3 positionVS = float3(ndc.x * camera.projectionInverse[0][0], ndc.y * camera.projectionInverse[1][1], -1.0f) * linearZ;
    const float3 positionWS = mul(float4(positionVS, 1.0f), camera.viewInverse).xyz;
    const float3 viewDirWS = mul(float4(normalize(-positionVS), 0.0f), camera.viewInverse).xyz;
    GetFragmentInfoCanonicalSurface(pixel, viewDirWS, positionVS, positionWS, SCREEN_SPACE_AO_MODE_OFF, surface.fragment);
    surface.parameters = MakeLightingParameters(surface.fragment);
    surface.baseState = MakeOpenPBRBaseLayerStateForLighting(surface.parameters);
    surface.position = positionWS;
    surface.distanceToCamera = length(positionVS);
    return true;
}

// A light's contribution at the surface without shadowing, by the raster loop's functions. False where the
// light does not reach (out of range); the contribution is then zero.
bool StochasticEvaluateLightUnshadowed(LightInfo light, StochasticSurface surface, out DirectLightContribution contribution)
{
    contribution = (DirectLightContribution)0;
    const LightFragmentData data = EvaluateLightAtPoint(light, surface.position);
    if (!LightInRange(light, data))
    {
        return false;
    }
    contribution = EvaluateDirectLightContribution(data, surface.parameters, surface.baseState);
    return true;
}

// A light's selection importance from a cheap stand-in for its contribution: Lambert diffuse plus a GGX distribution term
// for specular (no Fresnel or masking), times its unshadowed radiance. Only the probability of choosing the light uses it;
// the chosen sample is shaded with the full BRDF, so the estimate stays unbiased as long as this is positive wherever the
// full contribution is (N.L and N.V both positive, in range), which the floor guarantees.
float StochasticCheapImportance(LightInfo light, StochasticSurface surface)
{
    const LightFragmentData data = EvaluateLightAtPoint(light, surface.position);
    if (!LightInRange(light, data))
    {
        return 0.0f;
    }
    const float3 n = surface.parameters.normal;
    const float3 v = surface.parameters.viewDir;
    const float nl = dot(n, data.lightToFrag);
    const float nv = dot(n, v);
    if (nl <= 0.0f || nv <= 0.0f)
    {
        return 0.0f;
    }
    const float nh = saturate(dot(n, normalize(data.lightToFrag + v)));
    const float a2 = max(surface.parameters.specularAlpha * surface.parameters.specularAlpha, 1e-4f);
    const float denominator = nh * nh * (a2 - 1.0f) + 1.0f;
    const float distribution = a2 / (PI * denominator * denominator);
    const float diffuse = StochasticLuminance(surface.parameters.diffuseColor) * (1.0f / PI) * nl;
    const float specular = StochasticLuminance(surface.parameters.F0) * distribution / (4.0f * nv);
    const float value = StochasticLuminance(LightIncidentRadiance(data)) * (diffuse + specular);
    return log2(1.0f + max(value, 1e-6f));
}

float StochasticLightSphereRadius(LightInfo light)
{
    return max(light.sourceRadius, STOCHASTIC_MIN_SOURCE_RADIUS);
}

// ---- Guiding helpers (docs/stochastic_lighting.md "Guiding").

// StochasticPackNormal and StochasticUnpackNormal are in stochasticLightingRootConstants.h (the denoiser's spatial pass reads
// the surface history too).

// A visible-list entry: light buffer index (20 bits), the light's generation's low 8 bits (a reused slot is not the same
// light), and which quadrants of the emitter's sampling domain a ray reached.
#define STOCHASTIC_ENTRY_INDEX_MASK 0xFFFFFu
uint StochasticPackEntry(uint lightIndex, uint generation, uint quadrants)
{
    return (lightIndex & STOCHASTIC_ENTRY_INDEX_MASK) | ((generation & 0xFFu) << 20u) | ((quadrants & 0xFu) << 28u);
}

// Picks a quadrant of the emitter's [0,1)^2 sampling domain, favouring those the visible list says rays reached, and
// remaps u into it. Unreached quadrants keep STOCHASTIC_QUADRANT_FLOOR of a reached one's weight, so one that becomes
// visible is still found. Returns the density correction 1 / (4 p(quadrant)) the estimate is scaled by.
#define STOCHASTIC_QUADRANT_FLOOR 0.15f
float StochasticSampleQuadrant(uint quadrants, inout float2 u, out uint quadrant)
{
    float weights[4];
    float total = 0.0f;
    [unroll]
    for (uint q = 0; q < 4u; ++q)
    {
        weights[q] = (quadrants & (1u << q)) != 0u ? 1.0f : STOCHASTIC_QUADRANT_FLOOR;
        total += weights[q];
    }
    // u.x picks the quadrant and is warped back to [0, 1) within it; the quadrant's other axis comes from u.y.
    float target = u.x * total;
    quadrant = 3u;
    [unroll]
    for (uint q = 0; q < 3u; ++q)
    {
        if (target < weights[q])
        {
            quadrant = q;
            break;
        }
        target -= weights[q];
    }
    const float local = saturate(target / weights[quadrant]);
    u = float2((float(quadrant & 1u) + min(local, 0.99999994f)) * 0.5f, (float(quadrant >> 1u) + u.y) * 0.5f);
    const float probability = weights[quadrant] / total;
    return 0.25f / probability;
}

uint StochasticQuadrantOf(float2 u)
{
    return (u.x >= 0.5f ? 1u : 0u) | (u.y >= 0.5f ? 2u : 0u);
}

// The same ray from an origin already offset off its surface (StochasticLightShadowRay's), toward the light where it is now:
// visibility gradients re-trace last frame's sample this way (docs/stochastic_lighting.md "Visibility gradients").
bool StochasticLightShadowRayFrom(LightInfo light, float3 origin, float2 u, out RayDesc ray)
{
    ray.Origin = origin;
    ray.TMin = 0.0f;
    if (light.type == LIGHT_TYPE_DIRECTIONAL)
    {
        // A direction in the light's disc (sourceHalfAngleDegrees), the same u domain as a sphere's, so quadrant masks
        // apply. The disc is the same at every pixel, which makes a tile's mask a good guide to its penumbrae.
        ray.Direction = LightSampleConeDirection(normalize(-light.dirWorldSpace.xyz), LightSourceHalfAngleRadians(light), u);
        ray.TMax = STOCHASTIC_DIRECTIONAL_RAY_LENGTH;
        return true;
    }
    const float3 toCenter = light.posWorldSpace.xyz - ray.Origin;
    const float distanceSquared = dot(toCenter, toCenter);
    const float radius = StochasticLightSphereRadius(light);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMax = 0.0f;
    if (distanceSquared <= radius * radius)
    {
        return false; // Inside the emitter.
    }
    const float distance = sqrt(distanceSquared);
    const float3 axis = toCenter / distance;
    ray.Direction = LightSampleConeDirection(axis, asin(saturate(radius / distance)), u);
    // Where the ray meets the sphere's near side. A grazing direction can just miss it by rounding; it ends at the
    // closest approach instead.
    const float along = dot(ray.Direction, toCenter);
    const float hitDistance = along - sqrt(max(along * along - (distanceSquared - radius * radius), 0.0f));
    // Short of the emitter, so geometry the light sits on or in (its fixture) does not hide it.
    const float endBias = max(0.01f, hitDistance * 1e-3f);
    ray.TMax = max(hitDistance - endBias, 0.0f);
    return ray.TMax > 0.0f;
}

// The shadow ray toward the point on the light's sphere that u picks (uniform over the solid angle the sphere subtends), or
// out of the scene in the direction u picks in a directional light's disc. False when no ray is needed because the point is
// visible by construction (the surface is inside the emitter, or right against it).
bool StochasticLightShadowRay(LightInfo light, StochasticSurface surface, float2 u, out RayDesc ray)
{
    // Off the surface by about a pixel at its distance, as the ray traced sun shadow does: the BVH can be at a coarser LOD
    // than the surface the ray starts from.
    const float offset = max(0.05f, surface.distanceToCamera * 2e-3f);
    return StochasticLightShadowRayFrom(light, surface.position + surface.fragment.normalWS * offset, u, ray);
}

// Whether one shadow ray (StochasticLightShadowRay) reaches the light. 1 visible, 0 occluded.
float StochasticTraceShadowRay(RayDesc ray, Camera camera, PerFrameBuffer perFrame)
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
    const CLodRtHit hit = CLodRtTrace(scene, clasInfo, ray, STOCHASTIC_SHADOW_RAY_FLAGS, 0xFFu,
        CLodRtPixelConeSpread(camera, float(perFrame.screenResY)));
    return hit.hit ? 0.0f : 1.0f;
}

// Whether one ray from the surface reaches the point on the light's sphere that u picks, or leaves the scene in the
// direction u picks in a directional light's disc. 1 visible, 0 occluded.
float StochasticLightVisibility(LightInfo light, StochasticSurface surface, float2 u, Camera camera, PerFrameBuffer perFrame)
{
    RayDesc ray;
    if (!StochasticLightShadowRay(light, surface, u, ray))
    {
        return 1.0f;
    }
    return StochasticTraceShadowRay(ray, camera, perFrame);
}

// The pixel's froxel in the light grid (lightGrid.hlsli): its opaque run of light buffer indices, read with
// LightGridLight.
LightGridRange StochasticLoadLightRange(StochasticSurface surface, ConstantBuffer<PerFrameBuffer> perFrame, Camera camera)
{
    return LightGridLoadRange(surface.fragment.pixelCoords, surface.fragment.fragPosViewSpace.z, perFrame, camera);
}

#endif // __STOCHASTIC_LIGHTING_HLSLI__
