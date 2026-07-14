#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/occlusionCulling.hlsli"

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
static const uint WindBoneFlagTrunk = 1u;

typedef row_major float4x4 WindMatrix;

struct WindBoneGPU
{
    uint skinningSlot;
    uint jointIndex;
    uint parentEntry;
    uint simulationGroup;
    uint phaseSeed;
    uint flags;
    float influence;
    float meanBend;
    float parallelAmplitude;
    float perpendicularRatio;
    float torsionRatio;
    float frequencyScale;
    float maximumAngle;
    float gustAttenuation;
    float2 pad0;
    float3 frequencies;
    float pad1;
    float3 weights;
    float pad2;
    float3 branchAxis;
    float pad3;
    float3 branchTangent;
    float pad4;
    WindMatrix bindGlobal;
    WindMatrix inverseBind;
};

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
    const float a00=m[0][0], a01=m[0][1], a02=m[0][2];
    const float a10=m[1][0], a11=m[1][1], a12=m[1][2];
    const float a20=m[2][0], a21=m[2][1], a22=m[2][2];
    const float det = a00*(a11*a22-a12*a21) - a01*(a10*a22-a12*a20) + a02*(a10*a21-a11*a20);
    if (abs(det) < 1.0e-8f) return m;
    const float invDet = 1.0f / det;
    WindMatrix result = (WindMatrix)0;
    result[0][0]=(a11*a22-a12*a21)*invDet;
    result[0][1]=(a02*a21-a01*a22)*invDet;
    result[0][2]=(a01*a12-a02*a11)*invDet;
    result[1][0]=(a12*a20-a10*a22)*invDet;
    result[1][1]=(a00*a22-a02*a20)*invDet;
    result[1][2]=(a02*a10-a00*a12)*invDet;
    result[2][0]=(a10*a21-a11*a20)*invDet;
    result[2][1]=(a01*a20-a00*a21)*invDet;
    result[2][2]=(a00*a11-a01*a10)*invDet;
    result[3].xyz = -float3(
        dot(m[3].xyz, float3(result[0][0], result[1][0], result[2][0])),
        dot(m[3].xyz, float3(result[0][1], result[1][1], result[2][1])),
        dot(m[3].xyz, float3(result[0][2], result[1][2], result[2][2])));
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
    scratchForward[entryIndex] = rawForward;
    scratchInverse[entryIndex] = rawInverse;
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
        forwardSkin[forwardIndex] = baseSkin;
        WindMatrix baseInverse = scratchInverse[entryIndex];
        inverseSkin[inverseIndex] = baseInverse;
        return;
    }

    WindMatrix targetInverseBind = inverseBind[targetInfo.invBindOffsetMatrices + target.jointIndex];
    WindMatrix bindGlobal = WindInverse(targetInverseBind);
    // Row-vector skinning applies p * inverseBind * animatedGlobal.  Recover
    // the animated global from the captured skin matrix in the same order.
    WindMatrix deformedPose = mul(bindGlobal, baseSkin);
    float3 sampledWind = SampleLevel0Wind(deformedPose[3].xyz);
    float2 wind = normalize(length(sampledWind.xy) > 1.0e-5f ? sampledWind.xy : float2(WIND_X, WIND_Y));
    float windResponse = clamp(length(sampledWind), 0.0f, 4.0f);
    uint ancestor = entryIndex;
    [loop] for (uint depth = 0u; depth < 128u && ancestor != WindInvalidIndex; ++depth) {
        WindBoneGPU driver = entries[ancestor];
        SkinningInstanceGPUInfo driverInfo = instanceInfo[driver.skinningSlot];
        WindMatrix driverInverseBind = inverseBind[driverInfo.invBindOffsetMatrices + driver.jointIndex];
        WindMatrix driverBind = WindInverse(driverInverseBind);
        WindMatrix driverPose = mul(driverBind, scratchForward[ancestor]);
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
    WindMatrix result = mul(targetInverseBind, deformedPose);
    WindMatrix resultInverse = WindInverse(result);
    forwardSkin[forwardIndex] = result;
    inverseSkin[inverseIndex] = resultInverse;
}

// Frame-transient, per-skinned-assembly wind skeleton path.
#define WT_TYPES UintRootConstant0
#define WT_BONES UintRootConstant1
#define WT_ACTIVE UintRootConstant2
#define WT_TYPE_COUNTERS UintRootConstant3
#define WT_COUNTERS UintRootConstant4
#define WT_GENERATIONS UintRootConstant5
#define WT_COMMANDS UintRootConstant6
#define WT_SKIN_INFO UintRootConstant7
#define WT_FORWARD UintRootConstant8
#define WT_INVERSE UintRootConstant9
#define WT_INVERSE_BIND UintRootConstant10
#define WT_PLACEMENT_COUNT UintRootConstant11
#define WT_TYPE_COUNT UintRootConstant12
#define WT_TRANSFORM_BASE UintRootConstant13
#define WT_INVERSE_BASE UintRootConstant14
#define WT_MATRIX_CAPACITY UintRootConstant15
#define WT_CAMERA_INDEX UintRootConstant16
#define WT_PHASE_DEPTH UintRootConstant17
#define WT_FIELD0 UintRootConstant18
#define WT_FIELD1 UintRootConstant19
#define WT_FIELD_DIMS UintRootConstant20
#define WT_FIELD_CELL asfloat(UintRootConstant21)
#define WT_FIELD_X asfloat(UintRootConstant22)
#define WT_FIELD_Y asfloat(UintRootConstant23)
#define WT_FIELD_LERP asfloat(UintRootConstant24)
#define WT_TIME asfloat(UintRootConstant25)
#define WT_WIND_X asfloat(UintRootConstant26)
#define WT_WIND_Y asfloat(UintRootConstant27)
#define WT_STRENGTH asfloat(UintRootConstant28)
#define WT_GUST asfloat(UintRootConstant29)

static const uint WindTransientSlotBase = 65536u;
static const uint WindTypeFlag = 2u;

static const uint WindLatePhaseBit = 0x80000000u;
static const uint WindDepthDescriptorMask = 0x7fffffffu;

struct WindTypeGPU {
    uint firstBone, boneCount, sourceSkinningSlot, bucketBase;
    uint bucketCapacity, diagnosticsDescriptor, activeEntriesDescriptor, transformCount;
    uint deferredEntriesDescriptor, processedTypeCountsDescriptor;
};
struct SkinnedAssemblyPlacementGPU {
    uint instanceTransformIndex, skinningTypeSlot, stableSceneId, generation;
    float4 localBoundingSphere;
    float boundsScale;
    uint3 pad;
};
struct WindActiveInstanceGPU { uint instanceTransformIndex, stableSceneId, transformOffsetMatrices, inverseSkinOffsetMatrices; };
struct WindIndirectCommandGPU { uint typeId, pad0, pad1; uint3 dispatch; };

[numthreads(64,1,1)]
void ResetWindTransientCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<WindTypeGPU> types = ResourceDescriptorHeap[WT_TYPES];
    if (WT_TYPE_COUNT == 0u) return;
    WindTypeGPU control = types[0];
    RWStructuredBuffer<SkinningInstanceGPUInfo> infos = ResourceDescriptorHeap[WT_SKIN_INFO];
    RWStructuredBuffer<uint> typeCounters = ResourceDescriptorHeap[WT_TYPE_COUNTERS];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[WT_COUNTERS];
    if (tid.x < control.transformCount) infos[WindTransientSlotBase + tid.x] = (SkinningInstanceGPUInfo)0;
    if (tid.x < WT_TYPE_COUNT) typeCounters[tid.x] = 0u;
    if (tid.x < WT_TYPE_COUNT) {
        RWStructuredBuffer<uint> processedTypeCounts = ResourceDescriptorHeap[control.processedTypeCountsDescriptor];
        processedTypeCounts[tid.x] = 0u;
    }
    if (tid.x < 15u) counters[tid.x] = 0u;
    if (tid.x < 32u) {
        RWStructuredBuffer<uint> diagnostics = ResourceDescriptorHeap[control.diagnosticsDescriptor];
        diagnostics[tid.x] = 0u;
    }
}

float WindMaxAxisScale(row_major float4x4 m)
{
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
}

bool WindHZBOccluded(
    Camera camera,
    row_major matrix projection,
    float3 viewSpaceCenter,
    float scaledRadius,
    uint depthMapDescriptorIndex)
{
    const float2 viewRes = float2(camera.depthResX, camera.depthResY);
    viewSpaceCenter.y = -viewSpaceCenter.y;
    float4 lbtr = sphere_screen_extents(viewSpaceCenter, scaledRadius, projection);
    lbtr.x = -lbtr.x;
    lbtr.z = -lbtr.z;
    const float4 uvTransform = float4(0.5f, -0.5f, 0.5f, -0.5f);
    const float4 viewportUV = saturate(lbtr.xwzy * uvTransform + 0.5f);
    const float2 extentsPixels = (viewportUV.zw - viewportUV.xy) * viewRes;
    const float selectedMip = clamp(ceil(log2(max(extentsPixels.x, extentsPixels.y))), 0.0f, camera.numDepthMips - 1.0f);

    const float4 paddedUV = viewportUV * camera.UVScaleToNextPowerOf2.xyxy;
    const float2 safeScale = max(camera.UVScaleToNextPowerOf2, float2(1.0e-6f, 1.0e-6f));
    const uint mipLevel = (uint)selectedMip;
    const uint2 hzbRes = max(uint2(1u, 1u), (uint2)round(viewRes / safeScale));
    const uint2 mipRes = max(uint2(1u, 1u), hzbRes >> mipLevel);
    const uint4 maxCoord = uint4(mipRes - 1u, mipRes - 1u);
    const uint4 coords = min((uint4)floor(paddedUV * float4(mipRes, mipRes)), maxCoord);
    Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];
    const float4 depths = float4(
        depthBuffer.Load(int3(coords.xy, mipLevel)),
        depthBuffer.Load(int3(coords.zy, mipLevel)),
        depthBuffer.Load(int3(coords.zw, mipLevel)),
        depthBuffer.Load(int3(coords.xw, mipLevel)));
    const float sampledMaxDepth = max(max(depths.x, depths.y), max(depths.z, depths.w));
    const float sphereNearDepth = -viewSpaceCenter.z - scaledRadius;
    return sampledMaxDepth < sphereNearDepth;
}

[numthreads(64,1,1)]
void ActivateWindInstancesCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<WindTypeGPU> types = ResourceDescriptorHeap[WT_TYPES];
    const WindTypeGPU control = types[0];
    const bool latePhase = (WT_PHASE_DEPTH & WindLatePhaseBit) != 0u;
    const uint depthMapDescriptorIndex = WT_PHASE_DEPTH & WindDepthDescriptorMask;
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[WT_COUNTERS];
    uint activeEntryIndex = tid.x;
    if (latePhase) {
        const uint deferredCount = min(counters[10], WT_PLACEMENT_COUNT);
        if (activeEntryIndex >= deferredCount) return;
        StructuredBuffer<uint> deferredEntries = ResourceDescriptorHeap[control.deferredEntriesDescriptor];
        activeEntryIndex = deferredEntries[activeEntryIndex];
    }
    else if (activeEntryIndex >= WT_PLACEMENT_COUNT) return;

    StructuredBuffer<uint2> activeEntries = ResourceDescriptorHeap[control.activeEntriesDescriptor];
    const uint2 activeEntry = activeEntries[activeEntryIndex];
    StructuredBuffer<SkinnedAssemblyPlacementGPU> placements = ResourceDescriptorHeap[WT_GENERATIONS];
    const SkinnedAssemblyPlacementGPU placement = placements[activeEntry.x];
    if (placement.generation != activeEntry.y) { InterlockedAdd(counters[6], 1u); return; }
    if (!latePhase) InterlockedAdd(counters[5], 1u);

    RWStructuredBuffer<SkinningInstanceGPUInfo> infos = ResourceDescriptorHeap[WT_SKIN_INFO];
    SkinningInstanceGPUInfo source = infos[placement.skinningTypeSlot];
    if ((source.flags & WindTypeFlag) == 0u || source.pad0 >= WT_TYPE_COUNT) return;
    WindTypeGPU type = types[source.pad0];
    if (type.boneCount == 0u) return;

    StructuredBuffer<PerObjectBuffer> transforms = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    PerObjectBuffer objectData = transforms[placement.instanceTransformIndex];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera camera = cameras[WT_CAMERA_INDEX];
    float3 centerWS = mul(float4(placement.localBoundingSphere.xyz, 1.0f), objectData.model).xyz;
    float radiusWS = placement.localBoundingSphere.w * placement.boundsScale * WindMaxAxisScale(objectData.model);
    float3 centerVS = mul(float4(centerWS, 1.0f), camera.view).xyz;
    [unroll] for (uint p=0u; p<6u; ++p) if (dot(camera.clippingPlanes[p].plane.xyz, centerVS) + camera.clippingPlanes[p].plane.w < -radiusWS) { InterlockedAdd(counters[7], 1u); return; }
    if (distance(centerWS, camera.positionWorldSpace.xyz) > 32768.0f + radiusWS) { InterlockedAdd(counters[8], 1u); return; }

    bool occlusionCulled = false;
    if (depthMapDescriptorIndex != 0u) {
        if (latePhase) {
            occlusionCulled = WindHZBOccluded(
                camera, camera.projection, centerVS, radiusWS, depthMapDescriptorIndex);
        }
        else {
            const float3 previousCenterWS = mul(float4(placement.localBoundingSphere.xyz, 1.0f), objectData.prevModel).xyz;
            const float previousRadiusWS = placement.localBoundingSphere.w * placement.boundsScale * WindMaxAxisScale(objectData.prevModel);
            const float3 previousCenterVS = mul(float4(previousCenterWS, 1.0f), camera.prevView).xyz;
            occlusionCulled = WindHZBOccluded(
                camera, camera.prevUnjitteredProjection, previousCenterVS, previousRadiusWS,
                depthMapDescriptorIndex);
        }
    }
    if (occlusionCulled) {
        if (latePhase) {
            InterlockedAdd(counters[12], 1u);
        }
        else {
            uint deferredIndex;
            InterlockedAdd(counters[10], 1u, deferredIndex);
            if (deferredIndex < WT_PLACEMENT_COUNT) {
                RWStructuredBuffer<uint> deferredEntries = ResourceDescriptorHeap[control.deferredEntriesDescriptor];
                deferredEntries[deferredIndex] = activeEntryIndex;
                InterlockedAdd(counters[11], 1u);
            }
            else {
                InterlockedAdd(counters[14], 1u);
            }
        }
        return;
    }
    RWStructuredBuffer<uint> typeCounters = ResourceDescriptorHeap[WT_TYPE_COUNTERS];
    uint localIndex; InterlockedAdd(typeCounters[source.pad0], 1u, localIndex);
    if (localIndex >= type.bucketCapacity) { InterlockedAdd(counters[3], 1u); return; }

    WindActiveInstanceGPU active;
    active.instanceTransformIndex = placement.instanceTransformIndex;
    active.stableSceneId = placement.stableSceneId;
    active.transformOffsetMatrices = 0u;
    active.inverseSkinOffsetMatrices = 0u;
    RWStructuredBuffer<WindActiveInstanceGPU> activeInstances = ResourceDescriptorHeap[WT_ACTIVE];
    activeInstances[type.bucketBase + localIndex] = active;
    if (latePhase) InterlockedAdd(counters[13], 1u);
    InterlockedAdd(counters[9], 1u);
    if (localIndex == 0u && source.pad0 == 0u) {
        RWStructuredBuffer<uint> diagnostics = ResourceDescriptorHeap[control.diagnosticsDescriptor];
        diagnostics[0] = asuint(length(objectData.model[0].xyz));
        diagnostics[1] = asuint(length(objectData.model[1].xyz));
        diagnostics[2] = asuint(length(objectData.model[2].xyz));
        diagnostics[3] = asuint(objectData.model[3].x);
        diagnostics[4] = asuint(objectData.model[3].y);
        diagnostics[5] = asuint(objectData.model[3].z);
        diagnostics[6] = placement.instanceTransformIndex;
        diagnostics[7] = source.pad0;
    }
}

[numthreads(1,1,1)]
void BuildWindCommandsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<WindTypeGPU> types = ResourceDescriptorHeap[WT_TYPES];
    StructuredBuffer<uint> typeCounters = ResourceDescriptorHeap[WT_TYPE_COUNTERS];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[WT_COUNTERS];
    RWStructuredBuffer<WindIndirectCommandGPU> commands = ResourceDescriptorHeap[WT_COMMANDS];
    RWStructuredBuffer<WindActiveInstanceGPU> activeInstances = ResourceDescriptorHeap[WT_ACTIVE];
    RWStructuredBuffer<SkinningInstanceGPUInfo> infos = ResourceDescriptorHeap[WT_SKIN_INFO];
    RWStructuredBuffer<uint> processedTypeCounts = ResourceDescriptorHeap[types[0].processedTypeCountsDescriptor];
    const bool latePhase = (WT_PHASE_DEPTH & WindLatePhaseBit) != 0u;

    // A single thread walks types in stable order. This makes matrix allocation exact and
    // deterministic without a global CAS loop or an atomic-add/refund race.
    uint allocatedBones = latePhase ? counters[0] : 0u;
    uint allocatedAssemblies = latePhase ? counters[4] : 0u;
    uint commandCount = 0u;
    uint capacityRejects = latePhase ? counters[2] : 0u;
    for (uint typeId = 0u; typeId < WT_TYPE_COUNT; ++typeId) {
        WindTypeGPU type = types[typeId];
        const uint candidateCount = min(typeCounters[typeId], type.bucketCapacity);
        const uint previousTypeState = latePhase ? processedTypeCounts[typeId] : 0u;
        const uint processedCount = min(previousTypeState & 0xffffu, candidateCount);
        const uint previousAcceptedCount = previousTypeState >> 16u;
        const uint newCandidateCount = candidateCount - processedCount;
        if (type.boneCount == 0u || newCandidateCount == 0u) {
            if (latePhase && type.boneCount != 0u && previousAcceptedCount != 0u) {
                WindIndirectCommandGPU existingCommand;
                existingCommand.typeId = typeId;
                existingCommand.pad0 = existingCommand.pad1 = 0u;
                existingCommand.dispatch = uint3((type.boneCount + 63u) / 64u, previousAcceptedCount, 1u);
                commands[commandCount++] = existingCommand;
            }
            continue;
        }

        const uint remainingBones = allocatedBones < WT_MATRIX_CAPACITY
            ? WT_MATRIX_CAPACITY - allocatedBones
            : 0u;
        const uint acceptedCount = min(newCandidateCount, remainingBones / type.boneCount);
        capacityRejects += newCandidateCount - acceptedCount;
        const uint totalAcceptedCount = previousAcceptedCount + acceptedCount;
        processedTypeCounts[typeId] = (candidateCount & 0xffffu) | (totalAcceptedCount << 16u);
        if (acceptedCount == 0u) {
            if (latePhase && previousAcceptedCount != 0u) {
                WindIndirectCommandGPU existingCommand;
                existingCommand.typeId = typeId;
                existingCommand.pad0 = existingCommand.pad1 = 0u;
                existingCommand.dispatch = uint3((type.boneCount + 63u) / 64u, previousAcceptedCount, 1u);
                commands[commandCount++] = existingCommand;
            }
            continue;
        }

        const uint typeMatrixBase = allocatedBones;
        SkinningInstanceGPUInfo source = infos[type.sourceSkinningSlot];
        for (uint newInstanceIndex = 0u; newInstanceIndex < acceptedCount; ++newInstanceIndex) {
            const uint instanceIndex = processedCount + newInstanceIndex;
            const uint bucketIndex = type.bucketBase + instanceIndex;
            WindActiveInstanceGPU active = activeInstances[bucketIndex];
            const uint matrixOffset = typeMatrixBase + newInstanceIndex * type.boneCount;
            active.transformOffsetMatrices = WT_TRANSFORM_BASE + matrixOffset;
            active.inverseSkinOffsetMatrices = WT_INVERSE_BASE + matrixOffset;
            activeInstances[bucketIndex] = active;

            SkinningInstanceGPUInfo transientInfo = source;
            transientInfo.transformOffsetMatrices = active.transformOffsetMatrices;
            transientInfo.inverseSkinOffsetMatrices = active.inverseSkinOffsetMatrices;
            transientInfo.boneCount = type.boneCount;
            transientInfo.flags |= WindRowVectorSkinMatrix;
            infos[WindTransientSlotBase + active.instanceTransformIndex] = transientInfo;
        }

        WindIndirectCommandGPU command;
        command.typeId = typeId;
        command.pad0 = command.pad1 = 0u;
        command.dispatch = uint3((type.boneCount + 63u) / 64u, totalAcceptedCount, 1u);
        commands[commandCount++] = command;
        allocatedBones += acceptedCount * type.boneCount;
        allocatedAssemblies += acceptedCount;
    }

    counters[0] = allocatedBones;
    counters[1] = commandCount;
    counters[2] = capacityRejects;
    counters[4] = allocatedAssemblies;
}

float3 SampleTransientLevel0(float3 position)
{
    // SARP wind artifacts stay in native Skyrim space (X, Y, Z-up), while
    // renderer space is (X, Z, -Y) with Y-up. Keep that conversion at the
    // plugin boundary so the cache does not depend on renderer conventions.
    const float3 positionSkyrim = float3(position.x, -position.z, position.y);
    const float3 fallback = float3(WT_WIND_X, 0.0f, -WT_WIND_Y);
    if (WT_FIELD_CELL<=0 || WT_FIELD_DIMS==0u) return fallback;
    uint w=WT_FIELD_DIMS&0xffffu,h=WT_FIELD_DIMS>>16u;
    float2 grid=(positionSkyrim.xy-float2(WT_FIELD_X,WT_FIELD_Y))/WT_FIELD_CELL-0.5f;
    if(any(grid<0)||grid.x>=w-1||grid.y>=h-1) return fallback;
    uint2 b=(uint2)floor(grid); float2 f=frac(grid);
    StructuredBuffer<uint> a=ResourceDescriptorHeap[WT_FIELD0],z=ResourceDescriptorHeap[WT_FIELD1];
    float4 a0=lerp(LoadWindCell(a,b.y*w+b.x),LoadWindCell(a,b.y*w+b.x+1),f.x),a1=lerp(LoadWindCell(a,(b.y+1)*w+b.x),LoadWindCell(a,(b.y+1)*w+b.x+1),f.x);
    float4 z0=lerp(LoadWindCell(z,b.y*w+b.x),LoadWindCell(z,b.y*w+b.x+1),f.x),z1=lerp(LoadWindCell(z,(b.y+1)*w+b.x),LoadWindCell(z,(b.y+1)*w+b.x+1),f.x);
    float4 va=lerp(a0,a1,f.y),vz=lerp(z0,z1,f.y);
    if (min(va.w, vz.w) < 0.5f) return fallback;
    const float3 windSkyrim = lerp(va.xyz, vz.xyz, WT_FIELD_LERP);
    return float3(windSkyrim.x, windSkyrim.z, -windSkyrim.y);
}

float3 WindSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10f ? value * rsqrt(lengthSquared) : fallback;
}

float WindMaxAbs4(float4 value)
{
    value = abs(value);
    return max(max(value.x, value.y), max(value.z, value.w));
}

float3 WindWorldToObjectDirection(float3 directionWS, WindMatrix model)
{
    const float3 objectXAxisWS = WindSafeNormalize(model[0].xyz, float3(1.0f, 0.0f, 0.0f));
    const float3 objectYAxisWS = WindSafeNormalize(model[1].xyz, float3(0.0f, 1.0f, 0.0f));
    const float3 objectZAxisWS = WindSafeNormalize(model[2].xyz, float3(0.0f, 0.0f, 1.0f));
    return WindSafeNormalize(
        float3(dot(directionWS, objectXAxisWS), dot(directionWS, objectYAxisWS), dot(directionWS, objectZAxisWS)),
        float3(1.0f, 0.0f, 0.0f));
}

float WindInstanceHarmonics(WindBoneGPU bone, uint stableSceneId, uint channel)
{
    float value = 0.0f;
    const uint channelSeed = channel * 0x9e3779b9u;
    [unroll] for (uint i = 0u; i < 3u; ++i) {
        const uint phaseSeed = stableSceneId ^ bone.phaseSeed ^ channelSeed ^ (i * 0x85ebca6bu);
        value += bone.weights[i] * sin(
            6.28318530718f * bone.frequencies[i] * bone.frequencyScale * WT_TIME + WindPhase(phaseSeed));
    }
    return value;
}

WindMatrix WindRotationAroundPivot(float3 pivot, float3 axis, float angle)
{
    return mul(mul(WindTranslation(-pivot), WindRotation(axis, angle)), WindTranslation(pivot));
}

[numthreads(64,1,1)]
void SimulateWindInstancesCS(uint3 tid : SV_DispatchThreadID)
{
    if ((WT_PHASE_DEPTH & WindLatePhaseBit) != 0u) {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[WT_COUNTERS];
        if (counters[13] == 0u) return;
    }
    uint typeId=IndirectCommandSignatureRootConstant0;
    StructuredBuffer<WindTypeGPU> types=ResourceDescriptorHeap[WT_TYPES]; WindTypeGPU type=types[typeId];
    if(tid.x>=type.boneCount) return;
    StructuredBuffer<WindActiveInstanceGPU> instances=ResourceDescriptorHeap[WT_ACTIVE]; WindActiveInstanceGPU inst=instances[type.bucketBase+tid.y];
    StructuredBuffer<WindBoneGPU> bones=ResourceDescriptorHeap[WT_BONES]; WindBoneGPU target=bones[type.firstBone+tid.x];
    StructuredBuffer<SkinningInstanceGPUInfo> infos=ResourceDescriptorHeap[WT_SKIN_INFO]; SkinningInstanceGPUInfo source=infos[type.sourceSkinningSlot];
    WindMatrix targetInv=target.inverseBind;
    WindMatrix bindPose=target.bindGlobal;
    const WindMatrix bindIdentity=mul(targetInv,bindPose);
    const WindMatrix identityMatrix=WindTranslation(float3(0.0f,0.0f,0.0f));
    const float bindIdentityError=max(
        max(WindMaxAbs4(bindIdentity[0]-identityMatrix[0]),WindMaxAbs4(bindIdentity[1]-identityMatrix[1])),
        max(WindMaxAbs4(bindIdentity[2]-identityMatrix[2]),WindMaxAbs4(bindIdentity[3]-identityMatrix[3])));
    WindMatrix pose=bindPose;
    StructuredBuffer<PerObjectBuffer> transforms=ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    PerObjectBuffer objectData=transforms[inst.instanceTransformIndex]; float3 rootWS=objectData.model[3].xyz;
    float3 sampled=SampleTransientLevel0(rootWS);
    float2 rawWind = length(sampled.xz) > 1e-5 ? sampled.xz : float2(WT_WIND_X, -WT_WIND_Y);
    float rawWindLength = length(rawWind);
    float2 horizontalWind = rawWindLength > 1e-5 ? rawWind / rawWindLength : float2(1.0f, 0.0f);
    float3 windWS = float3(horizontalWind.x, 0.0f, horizontalWind.y);
    float3 windOS = WindWorldToObjectDirection(windWS, objectData.model);
    float response=clamp(length(sampled),0,4);
    float maximumForcing = 0.0f;
    float maximumBendAngle = 0.0f;
    float maximumTorsionAngle = 0.0f;
    bool appliedWind = false;
    uint ancestor=type.firstBone+tid.x;
    [loop] for(uint depth=0;depth<128 && ancestor!=WindInvalidIndex;++depth){
        WindBoneGPU driver=bones[ancestor]; WindMatrix bind=driver.bindGlobal; float3 pivot=bind[3].xyz;
        if (driver.simulationGroup != WindInvalidIndex && driver.maximumAngle > 0.0f && driver.influence > 0.0f) {
            const float3 branchAxis = WindSafeNormalize(driver.branchAxis, float3(0.0f, 0.0f, 1.0f));
            const float3 branchTangent = WindSafeNormalize(driver.branchTangent, float3(1.0f, 0.0f, 0.0f));
            const float3 acrossBranch = WindSafeNormalize(
                windOS - branchAxis * dot(windOS, branchAxis),
                WindSafeNormalize(cross(branchTangent, branchAxis), float3(1.0f, 0.0f, 0.0f)));
            const float3 dragAxis = WindSafeNormalize(cross(branchAxis, acrossBranch), branchTangent);

            const float phase = WindPhase(inst.stableSceneId ^ driver.phaseSeed ^ 0x68bc21ebu);
            const float gustNoise =
                0.70f * sin(WT_TIME * 0.71f + phase) +
                0.30f * sin(WT_TIME * 1.93f + phase * 1.37f);
            const float gust = max(0.0f, 1.0f + WT_GUST * (1.0f - saturate(driver.gustAttenuation)) * gustNoise);
            const float forcing = driver.influence * WT_STRENGTH * response * gust;
            maximumForcing = max(maximumForcing, abs(forcing));

            float drag = driver.meanBend + driver.parallelAmplitude * WindInstanceHarmonics(driver, inst.stableSceneId, 0u);
            float lateral = driver.parallelAmplitude * driver.perpendicularRatio * WindInstanceHarmonics(driver, inst.stableSceneId, 1u);
            float torsion = driver.parallelAmplitude * driver.torsionRatio * WindInstanceHarmonics(driver, inst.stableSceneId, 2u);
            float3 lateralAxis = acrossBranch;

            if ((driver.flags & WindBoneFlagTrunk) == 0u) {
                // GPU Gems models branches as wing-like segments. Upwind-facing branches
                // are suppressed, lee-side branches sway more freely, and branches across
                // the flow receive the strongest lift/twist contribution.
                const float facingWind = clamp(dot(branchAxis, windOS), -1.0f, 1.0f);
                const float windward = saturate(-facingWind);
                const float leeSide = saturate(facingWind);
                const float acrossFlow = 1.0f - abs(facingWind);
                drag *= lerp(1.0f, 0.35f, windward) * lerp(1.0f, 1.25f, leeSide);
                lateral *= 0.25f + 0.75f * acrossFlow;
                torsion *= 0.25f + 0.75f * acrossFlow;
                lateralAxis = branchTangent;
            }

            float2 bendAngles = float2(drag, lateral) * forcing;
            const float bendMagnitude = length(bendAngles);
            if (bendMagnitude > driver.maximumAngle)
                bendAngles *= driver.maximumAngle / bendMagnitude;
            torsion = clamp(torsion * forcing, -driver.maximumAngle, driver.maximumAngle);
            maximumBendAngle = max(maximumBendAngle, length(bendAngles));
            maximumTorsionAngle = max(maximumTorsionAngle, abs(torsion));
            appliedWind = appliedWind || any(abs(bendAngles) > 1.0e-7f) || abs(torsion) > 1.0e-7f;

            WindMatrix angularMotion = WindRotationAroundPivot(pivot, dragAxis, bendAngles.x);
            angularMotion = mul(angularMotion, WindRotationAroundPivot(pivot, lateralAxis, bendAngles.y));
            angularMotion = mul(angularMotion, WindRotationAroundPivot(pivot, branchAxis, torsion));
            pose = mul(pose, angularMotion);
        }
        ancestor=driver.parentEntry;
    }
    // Consumers use row vectors (mul(position, skinMatrix)), so the skin
    // transform is inverse bind followed by the animated global pose.
    WindMatrix result=mul(targetInv,pose);
    WindMatrix resultInverse=WindInverse(result);
    RWStructuredBuffer<WindMatrix> forward=ResourceDescriptorHeap[WT_FORWARD],inverse=ResourceDescriptorHeap[WT_INVERSE];
    // The transient palette uses the same shader-native row-vector layout as every
    // other skinning buffer. CPU producers convert before upload; GPU producers write
    // their logical matrices directly, so readers never need orientation branches.
    forward[inst.transformOffsetMatrices+tid.x]=result;
    inverse[inst.inverseSkinOffsetMatrices+tid.x]=resultInverse;
    RWStructuredBuffer<uint> diagnostics=ResourceDescriptorHeap[types[0].diagnosticsDescriptor];
    const bool finiteResult = all(isfinite(result[0])) && all(isfinite(result[1])) && all(isfinite(result[2])) && all(isfinite(result[3]));
    InterlockedAdd(diagnostics[finiteResult ? 12u : 13u], 1u);
    if (target.simulationGroup != WindInvalidIndex) InterlockedAdd(diagnostics[14], 1u);
    if (appliedWind) InterlockedAdd(diagnostics[15], 1u);
    InterlockedMax(diagnostics[16], asuint(maximumForcing));
    InterlockedMax(diagnostics[17], asuint(maximumBendAngle));
    InterlockedMax(diagnostics[18], asuint(maximumTorsionAngle));
    InterlockedMax(diagnostics[19], asuint(length(pose[3].xyz - bindPose[3].xyz)));
    const WindMatrix identity = WindTranslation(float3(0.0f, 0.0f, 0.0f));
    const float skinDelta = max(
        max(WindMaxAbs4(result[0] - identity[0]), WindMaxAbs4(result[1] - identity[1])),
        max(WindMaxAbs4(result[2] - identity[2]), WindMaxAbs4(result[3] - identity[3])));
    InterlockedMax(diagnostics[20], asuint(skinDelta));
    InterlockedAdd(diagnostics[21], 1u);
    InterlockedMax(diagnostics[22], asuint(bindIdentityError));
    const float3 mappedBindOrigin = mul(float4(bindPose[3].xyz, 1.0f), result).xyz;
    InterlockedMax(diagnostics[23], asuint(length(mappedBindOrigin - pose[3].xyz)));
    if (tid.x == 0u && tid.y == 0u && typeId == 0u) {
        diagnostics[8]=asuint(result[3].x); diagnostics[9]=asuint(result[3].y); diagnostics[10]=asuint(result[3].z); diagnostics[11]=finiteResult ? 1u : 0u;
    }
}
