#ifndef INSTANCE_DRAW_RECORD_HELPERS_HLSLI
#define INSTANCE_DRAW_RECORD_HELPERS_HLSLI

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

#endif // INSTANCE_DRAW_RECORD_HELPERS_HLSLI
