#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"

#define WIND_BONE_ENTRIES UintRootConstant0
#define WIND_SCRATCH_FORWARD UintRootConstant1
#define WIND_SCRATCH_INVERSE UintRootConstant2
#define WIND_SKINNING_INFO UintRootConstant3
#define WIND_FORWARD_SKIN UintRootConstant4
#define WIND_INVERSE_SKIN UintRootConstant5
#define WIND_INVERSE_BIND UintRootConstant6
#define WIND_BONE_COUNT UintRootConstant7
#define WIND_TIME asfloat(UintRootConstant8)
#define WIND_X asfloat(UintRootConstant9)
#define WIND_Y asfloat(UintRootConstant10)
#define WIND_STRENGTH asfloat(UintRootConstant11)
#define WIND_GUST asfloat(UintRootConstant12)
#define WIND_FIELD_SLICE0 UintRootConstant13
#define WIND_FIELD_SLICE1 UintRootConstant14
#define WIND_FIELD_DIMENSIONS UintRootConstant15
#define WIND_FIELD_CELL_SIZE asfloat(UintRootConstant16)
#define WIND_FIELD_ORIGIN_X asfloat(UintRootConstant17)
#define WIND_FIELD_ORIGIN_Y asfloat(UintRootConstant18)
#define WIND_FIELD_INTERPOLATION asfloat(UintRootConstant19)
#define WIND_FIELD_VALID UintRootConstant20

static const uint WindInvalidIndex = 0xFFFFFFFFu;
static const uint WindRowVectorSkinMatrix = 1u;

struct WindBoneGPU
{
    uint skinningSlot;
    uint jointIndex;
    uint parentEntry;
    uint simulationGroup;
    uint phaseSeed;
    float influence;
    float meanBend;
    float parallelAmplitude;
    float perpendicularRatio;
    float torsionRatio;
    float frequencyScale;
    float maximumAngle;
    float3 frequencies;
    float pad0;
    float3 weights;
    float pad1;
};

typedef row_major float4x4 WindMatrix;

float WindPhase(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return (seed & 0x00FFFFFFu) * (6.28318530718f / 16777216.0f);
}

WindMatrix WindInverse(WindMatrix m)
{
    float3x3 r = (float3x3)m;
    float det = determinant(r);
    if (abs(det) < 1.0e-8f) return m;
    float3x3 ri = float3x3(
        cross(r[1], r[2]),
        cross(r[2], r[0]),
        cross(r[0], r[1])) / det;
    ri = transpose(ri);
    WindMatrix result = (WindMatrix)0;
    result[0].xyz = ri[0];
    result[1].xyz = ri[1];
    result[2].xyz = ri[2];
    result[3].xyz = -mul(m[3].xyz, ri);
    result[3][3] = 1.0f;
    return result;
}

WindMatrix WindTranslation(float3 p)
{
    WindMatrix m = (WindMatrix)0;
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
    m[3].xyz = p;
    return m;
}

WindMatrix WindRotation(float3 axis, float angle)
{
    axis = normalize(axis);
    float s, c;
    sincos(angle, s, c);
    float t = 1.0f - c, x = axis.x, y = axis.y, z = axis.z;
    WindMatrix m = (WindMatrix)0;
    m[0] = float4(c + x*x*t, x*y*t + z*s, x*z*t - y*s, 0);
    m[1] = float4(x*y*t - z*s, c + y*y*t, y*z*t + x*s, 0);
    m[2] = float4(x*z*t + y*s, y*z*t - x*s, c + z*z*t, 0);
    m[3][3] = 1.0f;
    return m;
}

float WindHarmonics(WindBoneGPU bone, float phaseOffset)
{
    float value = 0.0f;
    [unroll] for (uint i = 0u; i < 3u; ++i) {
        value += bone.weights[i] * sin(6.28318530718f * bone.frequencies[i] * bone.frequencyScale * WIND_TIME + WindPhase(bone.phaseSeed + i * 1013u) + phaseOffset);
    }
    return value;
}

float4 LoadWindCell(StructuredBuffer<uint> field, uint cellIndex)
{
    uint xy = field[cellIndex * 2u];
    uint za = field[cellIndex * 2u + 1u];
    return float4(f16tof32(xy & 0xFFFFu), f16tof32(xy >> 16u), f16tof32(za & 0xFFFFu), f16tof32(za >> 16u));
}

float4 SampleWindSlice(StructuredBuffer<uint> field, float2 position, uint width, uint height)
{
    float2 grid = (position - float2(WIND_FIELD_ORIGIN_X, WIND_FIELD_ORIGIN_Y)) / WIND_FIELD_CELL_SIZE - 0.5f;
    if (any(grid < 0.0f) || grid.x >= width - 1u || grid.y >= height - 1u) return float4(0, 0, 0, -1);
    uint2 base = (uint2)floor(grid);
    float2 fraction = frac(grid);
    float4 a = lerp(LoadWindCell(field, base.y * width + base.x), LoadWindCell(field, base.y * width + base.x + 1u), fraction.x);
    float4 b = lerp(LoadWindCell(field, (base.y + 1u) * width + base.x), LoadWindCell(field, (base.y + 1u) * width + base.x + 1u), fraction.x);
    return lerp(a, b, fraction.y);
}

float3 SampleLevel0Wind(float3 position)
{
    float3 fallback = float3(WIND_X, WIND_Y, 0.0f);
    if (WIND_FIELD_VALID == 0u || WIND_FIELD_CELL_SIZE <= 0.0f) return fallback;
    uint width = WIND_FIELD_DIMENSIONS & 0xFFFFu;
    uint height = WIND_FIELD_DIMENSIONS >> 16u;
    StructuredBuffer<uint> field0 = ResourceDescriptorHeap[WIND_FIELD_SLICE0];
    StructuredBuffer<uint> field1 = ResourceDescriptorHeap[WIND_FIELD_SLICE1];
    float4 a = SampleWindSlice(field0, position.xy, width, height);
    float4 b = SampleWindSlice(field1, position.xy, width, height);
    if (min(a.w, b.w) < 0.5f) return fallback;
    return lerp(a.xyz, b.xyz, WIND_FIELD_INTERPOLATION);
}

[numthreads(64, 1, 1)]
void CaptureWindBasePoseCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint entryIndex = dispatchThreadID.x;
    if (entryIndex >= WIND_BONE_COUNT) return;
    StructuredBuffer<WindBoneGPU> entries = ResourceDescriptorHeap[WIND_BONE_ENTRIES];
    StructuredBuffer<SkinningInstanceGPUInfo> instanceInfo = ResourceDescriptorHeap[WIND_SKINNING_INFO];
    StructuredBuffer<WindMatrix> forwardSkin = ResourceDescriptorHeap[WIND_FORWARD_SKIN];
    StructuredBuffer<WindMatrix> inverseSkin = ResourceDescriptorHeap[WIND_INVERSE_SKIN];
    RWStructuredBuffer<WindMatrix> scratchForward = ResourceDescriptorHeap[WIND_SCRATCH_FORWARD];
    RWStructuredBuffer<WindMatrix> scratchInverse = ResourceDescriptorHeap[WIND_SCRATCH_INVERSE];
    WindBoneGPU bone = entries[entryIndex];
    SkinningInstanceGPUInfo info = instanceInfo[bone.skinningSlot];
    WindMatrix rawForward = forwardSkin[info.transformOffsetMatrices + bone.jointIndex];
    WindMatrix rawInverse = inverseSkin[info.inverseSkinOffsetMatrices + bone.jointIndex];
    scratchForward[entryIndex] = (info.flags & WindRowVectorSkinMatrix) != 0u ? rawForward : transpose(rawForward);
    scratchInverse[entryIndex] = (info.flags & WindRowVectorSkinMatrix) != 0u ? rawInverse : transpose(rawInverse);
}

[numthreads(64, 1, 1)]
void SimulateWindBonesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint entryIndex = dispatchThreadID.x;
    if (entryIndex >= WIND_BONE_COUNT) return;
    StructuredBuffer<WindBoneGPU> entries = ResourceDescriptorHeap[WIND_BONE_ENTRIES];
    StructuredBuffer<WindMatrix> scratchForward = ResourceDescriptorHeap[WIND_SCRATCH_FORWARD];
    StructuredBuffer<WindMatrix> scratchInverse = ResourceDescriptorHeap[WIND_SCRATCH_INVERSE];
    StructuredBuffer<SkinningInstanceGPUInfo> instanceInfo = ResourceDescriptorHeap[WIND_SKINNING_INFO];
    StructuredBuffer<WindMatrix> inverseBind = ResourceDescriptorHeap[WIND_INVERSE_BIND];
    RWStructuredBuffer<WindMatrix> forwardSkin = ResourceDescriptorHeap[WIND_FORWARD_SKIN];
    RWStructuredBuffer<WindMatrix> inverseSkin = ResourceDescriptorHeap[WIND_INVERSE_SKIN];

    WindBoneGPU target = entries[entryIndex];
    SkinningInstanceGPUInfo targetInfo = instanceInfo[target.skinningSlot];
    const uint forwardIndex = targetInfo.transformOffsetMatrices + target.jointIndex;
    const uint inverseIndex = targetInfo.inverseSkinOffsetMatrices + target.jointIndex;
    WindMatrix baseSkin = scratchForward[entryIndex];
    if (WIND_STRENGTH <= 0.0f) {
        forwardSkin[forwardIndex] = (targetInfo.flags & WindRowVectorSkinMatrix) != 0u ? baseSkin : transpose(baseSkin);
        WindMatrix baseInverse = scratchInverse[entryIndex];
        inverseSkin[inverseIndex] = (targetInfo.flags & WindRowVectorSkinMatrix) != 0u ? baseInverse : transpose(baseInverse);
        return;
    }

    WindMatrix targetInverseBind = inverseBind[targetInfo.invBindOffsetMatrices + target.jointIndex];
    if ((targetInfo.flags & WindRowVectorSkinMatrix) == 0u) targetInverseBind = transpose(targetInverseBind);
    WindMatrix bindGlobal = WindInverse(targetInverseBind);
    WindMatrix deformedPose = mul(baseSkin, bindGlobal);
    float3 sampledWind = SampleLevel0Wind(deformedPose[3].xyz);
    float2 wind = normalize(length(sampledWind.xy) > 1.0e-5f ? sampledWind.xy : float2(WIND_X, WIND_Y));
    float windResponse = clamp(length(sampledWind), 0.0f, 4.0f);
    uint ancestor = entryIndex;
    [loop] for (uint depth = 0u; depth < 128u && ancestor != WindInvalidIndex; ++depth) {
        WindBoneGPU driver = entries[ancestor];
        SkinningInstanceGPUInfo driverInfo = instanceInfo[driver.skinningSlot];
        WindMatrix driverInverseBind = inverseBind[driverInfo.invBindOffsetMatrices + driver.jointIndex];
        if ((driverInfo.flags & WindRowVectorSkinMatrix) == 0u) driverInverseBind = transpose(driverInverseBind);
        WindMatrix driverBind = WindInverse(driverInverseBind);
        WindMatrix driverPose = mul(scratchForward[ancestor], driverBind);
        float3 pivot = driverPose[3].xyz;
        float gust = 1.0f + WIND_GUST * (0.5f + 0.5f * sin(WIND_TIME * 0.71f + WindPhase(driver.phaseSeed ^ 0x9e3779b9u)));
        float parallel = driver.meanBend + driver.parallelAmplitude * WindHarmonics(driver, 0.0f);
        float perpendicular = driver.parallelAmplitude * driver.perpendicularRatio * WindHarmonics(driver, 1.57079632679f);
        float bendAngle = clamp(length(float2(parallel, perpendicular)) * driver.influence * WIND_STRENGTH * windResponse * gust, 0.0f, driver.maximumAngle);
        float3 bendAxis = normalize(float3(-wind.y, wind.x, 0.0f) * parallel + float3(wind.x, wind.y, 0.0f) * perpendicular + float3(0, 0, 1.0e-5f));
        WindMatrix aroundPivot = mul(mul(WindTranslation(-pivot), WindRotation(bendAxis, bendAngle)), WindTranslation(pivot));
        float torsion = driver.parallelAmplitude * driver.torsionRatio * WindHarmonics(driver, 0.78539816339f) * driver.influence * WIND_STRENGTH * windResponse;
        float3 boneAxis = normalize(driverPose[2].xyz + float3(0, 0, 1.0e-5f));
        aroundPivot = mul(aroundPivot, mul(mul(WindTranslation(-pivot), WindRotation(boneAxis, torsion)), WindTranslation(pivot)));
        deformedPose = mul(deformedPose, aroundPivot);
        ancestor = driver.parentEntry;
    }
    WindMatrix result = mul(deformedPose, targetInverseBind);
    WindMatrix resultInverse = WindInverse(result);
    forwardSkin[forwardIndex] = (targetInfo.flags & WindRowVectorSkinMatrix) != 0u ? result : transpose(result);
    inverseSkin[inverseIndex] = (targetInfo.flags & WindRowVectorSkinMatrix) != 0u ? resultInverse : transpose(resultInverse);
}
