#ifndef LENS_POLYNOMIAL_HLSLI
#define LENS_POLYNOMIAL_HLSLI

// The polynomial lens (lensMode polynomial, SARP's docs/lens_simulation.md): a camera ray's transport from the rear plane to the
// front plane as a polynomial in the ray's canonical inputs (LensCore LensPolynomial.h), in place of tracing the surfaces.
//
// Its code is generated: LensManager writes LensPolynomialEvaluate for the loaded lens and degree, with the coefficients as
// literals, and registers it as an in-memory include (br::shaderstore::SetGeneratedSource). A pipeline that defines
// LENS_POLYNOMIAL_INCLUDE as that include's name compiles it in; the name holds the code's hash, so the shader store's
// variant key changes with the code. The generated file defines LENS_POLYNOMIAL_KEY_LO and _HI, the same hash, which the
// lens data's header carries too: a shader compiled for another polynomial (or none) traces the surfaces instead, as
// while a new one compiles.
//
// LensPolynomialEvaluate(float4 inputs, out float y[6]): inputs (r, dx, dy, lambda nm), outputs (px, py, dx, dy, dz,
// throughput) in the canonical frame, the direction not yet normalized; the evaluation order is LensCore's EvalPolynomial.

#include "include/lens/lensCommon.hlsli"

#ifdef LENS_POLYNOMIAL_INCLUDE
#include LENS_POLYNOMIAL_INCLUDE
#endif

// Whether this shader has the polynomial code the lens data asks for.
bool LensPolynomialCompiledFor(LensHeader header)
{
#ifdef LENS_POLYNOMIAL_KEY_LO
    return (header.flags & LENS_FLAG_POLYNOMIAL) != 0u && header.polynomialKey.x == LENS_POLYNOMIAL_KEY_LO
        && header.polynomialKey.y == LENS_POLYNOMIAL_KEY_HI;
#else
    return false;
#endif
}

// The canonical inputs of a backward ray at the rear-plane point `rear` (lens mm) travelling `direction` (-z): the point
// rotated onto +x and mirrored so dy >= 0, as LensCore's Canonicalize. frame = (cos, sin, mirror sign).
float4 LensPolynomialCanonicalize(float2 rear, float3 direction, float lambdaNm, out float3 frame)
{
    const float r = length(rear);
    const float2 cs = r > 0.0f ? rear / r : float2(1.0f, 0.0f);
    const float dx = cs.x * direction.x + cs.y * direction.y;
    float dy = -cs.y * direction.x + cs.x * direction.y;
    frame = float3(cs, dy < 0.0f ? -1.0f : 1.0f);
    dy = abs(dy);
    return float4(r, dx, dy, lambdaNm);
}

float2 LensPolynomialUncanonicalize(float2 v, float3 frame)
{
    v.y *= frame.z;
    return float2(frame.x * v.x - frame.y * v.y, frame.y * v.x + frame.x * v.y);
}

// The polynomial's outputs for canonical inputs, the direction normalized: position (px, py) on the front plane, unit
// direction, throughput. False without compiled code.
bool LensPolynomialOutputs(float4 inputs, out float2 position, out float3 direction, out float throughput)
{
    position = 0.0f.xx;
    direction = float3(0.0f, 0.0f, -1.0f);
    throughput = 0.0f;
#ifdef LENS_POLYNOMIAL_KEY_LO
    float y[6];
    LensPolynomialEvaluate(inputs, y);
    position = float2(y[0], y[1]);
    direction = float3(y[2], y[3], y[4]);
    direction /= length(direction);
    throughput = y[5];
    return true;
#else
    return false;
#endif
}

// The backward transmission of a camera ray that crosses the rear plane at `rear` with unit `direction` (-z), through the
// polynomial. It doesn't know whether the ray clears the lens: the exact pupil sampler draws rays that do (up to the
// transmission factor the pupil nodes carry). False outside the fitted domain or when the exit doesn't head into the
// scene. On success, origin is on the front plane and direction is the exit direction, lens millimetres.
bool LensPolynomialTransport(LensHeader header, float2 rear, inout float3 direction, float lambdaNm, out float3 origin, out float throughput)
{
    origin = 0.0f.xxx;
    throughput = 0.0f;
    if (any(abs(rear) > header.polynomialDomain.ww)) return false;
    float3 frame;
    const float4 inputs = LensPolynomialCanonicalize(rear, direction, clamp(lambdaNm, header.polynomialDomain.y, header.polynomialDomain.z), frame);
    float2 position;
    float3 exitDirection;
    if (!LensPolynomialOutputs(inputs, position, exitDirection, throughput)) return false;
    if (!(exitDirection.z < 0.0f)) return false;
    origin = float3(LensPolynomialUncanonicalize(position, frame), header.frontPlaneZ);
    direction = float3(LensPolynomialUncanonicalize(exitDirection.xy, frame), exitDirection.z);
    throughput = max(throughput, 0.0f);
    return all(isfinite(origin)) && all(isfinite(direction));
}

#endif // LENS_POLYNOMIAL_HLSLI
