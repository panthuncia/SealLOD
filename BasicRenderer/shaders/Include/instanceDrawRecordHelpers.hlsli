#ifndef INSTANCE_DRAW_RECORD_HELPERS_HLSLI
#define INSTANCE_DRAW_RECORD_HELPERS_HLSLI

InstanceDrawRecordBuffer LoadInstanceDrawRecord(uint drawRecordIndex);

#include "include/clodStructs.hlsli"

InstanceDrawRecordBuffer LoadInstanceDrawRecord(uint drawRecordIndex)
{
    StructuredBuffer<InstanceDrawRecordBuffer> drawRecords =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::InstanceDrawRecordBuffer)];
    return drawRecords[drawRecordIndex];
}

PerMeshInstanceBuffer LoadMeshTemplateForDrawRecord(InstanceDrawRecordBuffer record)
{
    StructuredBuffer<PerMeshInstanceBuffer> meshTemplates =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    return meshTemplates[record.meshTemplateIndex];
}

PerMeshInstanceBuffer LoadMeshTemplateForDraw(uint drawRecordIndex)
{
    return LoadMeshTemplateForDrawRecord(LoadInstanceDrawRecord(drawRecordIndex));
}

PerObjectBuffer LoadInstanceTransformForDrawRecord(InstanceDrawRecordBuffer record)
{
    StructuredBuffer<PerObjectBuffer> instanceTransforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    return instanceTransforms[record.instanceTransformIndex];
}

PerObjectBuffer LoadInstanceTransformForDraw(uint drawRecordIndex)
{
    return LoadInstanceTransformForDrawRecord(LoadInstanceDrawRecord(drawRecordIndex));
}

row_major float4x4 CLodAssemblyTransformToMatrix(ClusterLODAssemblyTransform transform)
{
    return float4x4(
        transform.row0.x, transform.row1.x, transform.row2.x, 0.0f,
        transform.row0.y, transform.row1.y, transform.row2.y, 0.0f,
        transform.row0.z, transform.row1.z, transform.row2.z, 0.0f,
        transform.row0.w, transform.row1.w, transform.row2.w, 1.0f);
}

ClusterLODAssemblyTransform CLodInvertAssemblyTransform(ClusterLODAssemblyTransform transform)
{
    const float3 r0 = transform.row0.xyz;
    const float3 r1 = transform.row1.xyz;
    const float3 r2 = transform.row2.xyz;
    const float3 c0 = cross(r1, r2);
    const float3 c1 = cross(r2, r0);
    const float3 c2 = cross(r0, r1);
    const float det = dot(r0, c0);
    if (abs(det) <= 1.0e-8f)
    {
        ClusterLODAssemblyTransform identityTransform;
        identityTransform.row0 = float4(1.0f, 0.0f, 0.0f, 0.0f);
        identityTransform.row1 = float4(0.0f, 1.0f, 0.0f, 0.0f);
        identityTransform.row2 = float4(0.0f, 0.0f, 1.0f, 0.0f);
        return identityTransform;
    }
    const float invDet = rcp(det);

    ClusterLODAssemblyTransform inverseTransform;
    inverseTransform.row0.xyz = float3(c0.x, c1.x, c2.x) * invDet;
    inverseTransform.row1.xyz = float3(c0.y, c1.y, c2.y) * invDet;
    inverseTransform.row2.xyz = float3(c0.z, c1.z, c2.z) * invDet;

    const float3 t = float3(transform.row0.w, transform.row1.w, transform.row2.w);
    inverseTransform.row0.w = -dot(inverseTransform.row0.xyz, t);
    inverseTransform.row1.w = -dot(inverseTransform.row1.xyz, t);
    inverseTransform.row2.w = -dot(inverseTransform.row2.xyz, t);
    return inverseTransform;
}

PerObjectBuffer ComposeAssemblyTransformForDrawRecord(InstanceDrawRecordBuffer record, uint assemblyTransformIndex)
{
    PerObjectBuffer objectData = LoadInstanceTransformForDrawRecord(record);
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        return objectData;
    }

    StructuredBuffer<ClusterLODAssemblyTransform> assemblyTransforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyTransforms)];
    const ClusterLODAssemblyTransform assemblyTransform = assemblyTransforms[assemblyTransformIndex];
    const row_major matrix assemblyLocalToObject = CLodAssemblyTransformToMatrix(assemblyTransform);
    const row_major matrix assemblyObjectToLocal = CLodAssemblyTransformToMatrix(CLodInvertAssemblyTransform(assemblyTransform));
    objectData.model = mul(assemblyLocalToObject, objectData.model);
    objectData.prevModel = mul(assemblyLocalToObject, objectData.prevModel);
    objectData.modelInverse = mul(objectData.modelInverse, assemblyObjectToLocal);
    return objectData;
}

PerObjectBuffer LoadInstanceTransformForDrawRecordWithAssemblyTransform(InstanceDrawRecordBuffer record, uint assemblyTransformIndex)
{
    return ComposeAssemblyTransformForDrawRecord(record, assemblyTransformIndex);
}

PerObjectBuffer LoadInstanceTransformForDrawWithAssemblyTransform(uint drawRecordIndex, uint assemblyTransformIndex)
{
    return LoadInstanceTransformForDrawRecordWithAssemblyTransform(LoadInstanceDrawRecord(drawRecordIndex), assemblyTransformIndex);
}

#endif // INSTANCE_DRAW_RECORD_HELPERS_HLSLI
