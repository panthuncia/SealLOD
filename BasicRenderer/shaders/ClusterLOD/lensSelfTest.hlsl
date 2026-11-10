// lens.selftest: traces the active lens's fixed ray set (LensManager builds it into the lens data, generated from the
// prescription) through lensExplicit.hlsli, for the CPU to compare against LensCore's TraceSequential. With a polynomial
// lens whose code this shader was compiled with (LENS_POLYNOMIAL_INCLUDE, as the path tracer's), the same rays also go
// through the polynomial from their rear-plane crossing, and the CPU compares those with LensCore's EvalTransport on the
// canonical inputs written here, and with TraceCanonical.
// Constant 0 is the output UAV, 1 the request sequence (written to the first word so the reader can tell results apart).
#include "include/cbuffers.hlsli"
#include "include/lens/lensExplicit.hlsli"
#include "include/lens/lensPolynomial.hlsli"

[numthreads(64, 1, 1)]
void LensSelfTestCS(uint3 dispatchThread : SV_DispatchThreadID)
{
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[UintRootConstant0];
    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);
    const uint rayCount = header.valid ? min(header.selfTestRayCount, LENS_MAX_SELFTEST_RAYS) : 0u;
    if (dispatchThread.x == 0u) {
        output[0] = asfloat(uint4(UintRootConstant1, rayCount, asuint(data[LENS_ROW_HEADER5]).y, 0u));
    }
    const uint index = dispatchThread.x;
    if (index >= rayCount) return;
    const float4 input0 = data[header.selfTestOffset + index * LENS_SELFTEST_INPUT_ROWS];
    const float4 input1 = data[header.selfTestOffset + index * LENS_SELFTEST_INPUT_ROWS + 1u];
    float3 origin = input0.xyz;
    float3 direction = input1.xyz;
    float throughput;
    const uint status = LensTraceBackward(data, header, origin, direction, input0.w, throughput);
    const uint row = 1u + index * LENS_SELFTEST_OUTPUT_ROWS;
    output[row] = asfloat(uint4(status, 0u, 0u, 0u));
    output[row + 1u] = float4(origin, throughput);
    output[row + 2u] = float4(direction, 0.0f);

    // The polynomial's outputs on the ray's canonical inputs at the rear plane, whether or not the ray clears the lens.
    float4 inputs = 0.0f.xxxx;
    float2 position = 0.0f.xx;
    float3 exitDirection = 0.0f.xxx;
    float exitThroughput = 0.0f;
    bool evaluated = false;
    if (LensPolynomialCompiledFor(header)) {
        const float3 start = input0.xyz;
        const float3 startDirection = normalize(input1.xyz);
        const float2 rear = (start + startDirection * ((header.rearPlaneZ - start.z) / startDirection.z)).xy;
        float3 frame;
        inputs = LensPolynomialCanonicalize(rear, startDirection, input0.w, frame);
        evaluated = LensPolynomialOutputs(inputs, position, exitDirection, exitThroughput);
    }
    output[row + 3u] = inputs;
    output[row + 4u] = float4(position, exitThroughput, asfloat(evaluated ? 1u : 0u));
    output[row + 5u] = float4(exitDirection, 0.0f);
}
