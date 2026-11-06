#ifndef CLOD_SOURCE_GROUP_VALIDATION_HLSLI
#define CLOD_SOURCE_GROUP_VALIDATION_HLSLI

// Diagnostic for page addressing bugs: checks that a visible cluster's encoded source group,
// page owner and mesh identity agree with what its draw record says it should be, and records
// the exact page and segment of any mismatch. Compiled in with
// CLOD_ENABLE_SOURCE_GROUP_VALIDATION. Requires clodRasterizationRootConstants.h.

#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/visibleClusterPacking.hlsli"

static const uint CLOD_SOURCE_GROUP_MISMATCH_DETAIL_CAPACITY = 1024u;

struct CLodSourceGroupMismatchDetail
{
    uint expectedGroupLocalIndex;
    uint foundGroupLocalIndex;
    uint expectedGroupGlobalIndex;
    uint foundGroupGlobalIndex;
    uint clodMeshMetadataIndex;
    uint groupsBase;
    uint expectedSegmentGlobalIndex;
    uint expectedSegmentPageIndex;
    uint expectedSegmentFirstMeshlet;
    uint expectedSegmentMeshletCount;
    uint expectedSegmentPageSlabDescriptorIndex;
    uint expectedSegmentPageSlabByteOffset;
    uint pageLocalMeshletIndex;
    uint pageSlabDescriptorIndex;
    uint pageSlabByteOffset;
    uint visibleClusterIndex;
    uint unsortedClusterIndex;
    uint instanceId;
    uint viewId;
    uint bucketMeshletIndex;
    uint bucketCount;
    uint actualOwnerMeshMetadataIndex;
    uint expectedTemplateMeshMetadataIndex;
    uint expectedMeshIdentityLo;
    uint expectedMeshIdentityHi;
    uint actualMeshIdentityLo;
    uint actualMeshIdentityHi;
};

// Returns true when the cluster disagrees with its draw record, after recording the details.
bool CLodValidateSourceGroup(
    uint4 packedCluster,
    uint visibleClusterIndex,
    uint unsortedClusterIndex,
    uint bucketMeshletIndex,
    uint bucketCount)
{
    const uint instanceId = CLodVisibleClusterInstanceID(packedCluster);
    const uint expectedGroupGlobalIndex = CLodVisibleClusterGroupID(packedCluster);
    const uint pageLocalMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    const CLodPageHeader pageHeader = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(
        pageSlabDescriptorIndex, pageSlabByteOffset, pageHeader.descriptorOffset, pageLocalMeshletIndex);
    const uint foundGroupLocalIndex = descriptor.sourceGroupLocalIndex;

    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(instanceId);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceId);
    const PerMeshInstanceBuffer meshTemplate = LoadMeshTemplateForDrawRecord(drawRecord);
    const uint expectedTemplateMeshMetadataIndex = meshTemplate.expectedClodMeshMetadataIndex;
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<ClusterLODGroupSegment> segments =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    const ClusterLODGroup expectedGroup = groups[expectedGroupGlobalIndex];
    // Page descriptors name their group mesh-locally.
    const uint expectedGroupLocalIndex = expectedGroupGlobalIndex - metadata.groupsBase;

    // The segment of the expected group that should hold this meshlet.
    uint actualOwnerMeshMetadataIndex = 0xFFFFFFFFu;
    uint expectedSegmentGlobalIndex = 0xFFFFFFFFu;
    uint expectedSegmentPageIndex = 0xFFFFFFFFu;
    uint expectedSegmentFirstMeshlet = 0u;
    uint expectedSegmentMeshletCount = 0u;
    uint expectedSegmentPageSlabDescriptorIndex = 0u;
    uint expectedSegmentPageSlabByteOffset = 0u;
    for (uint segmentIndex = expectedGroup.firstSegment;
        segmentIndex < expectedGroup.firstSegment + expectedGroup.segmentCount;
        ++segmentIndex)
    {
        const ClusterLODGroupSegment segment = segments[segmentIndex];
        if (pageLocalMeshletIndex < segment.firstMeshletInPage ||
            pageLocalMeshletIndex >= segment.firstMeshletInPage + segment.meshletCount)
        {
            continue;
        }

        const GroupPageMapEntry entry = LoadGroupPageMapEntry(0u, segment.pageIndex);
        if (entry.slabDescriptorIndex != pageSlabDescriptorIndex || entry.slabByteOffset != pageSlabByteOffset)
        {
            continue;
        }

        actualOwnerMeshMetadataIndex = entry.ownerMeshMetadataIndex;
        expectedSegmentGlobalIndex = segmentIndex;
        expectedSegmentPageIndex = segment.pageIndex;
        expectedSegmentFirstMeshlet = segment.firstMeshletInPage;
        expectedSegmentMeshletCount = segment.meshletCount;
        expectedSegmentPageSlabDescriptorIndex = entry.slabDescriptorIndex;
        expectedSegmentPageSlabByteOffset = entry.slabByteOffset;
        break;
    }

    bool mismatch = foundGroupLocalIndex != 0xFFFFFFFFu && foundGroupLocalIndex != expectedGroupLocalIndex;
    mismatch = mismatch || actualOwnerMeshMetadataIndex != offsets.clodMeshMetadataIndex;
    mismatch = mismatch || (expectedTemplateMeshMetadataIndex != 0xFFFFFFFFu &&
        expectedTemplateMeshMetadataIndex != offsets.clodMeshMetadataIndex);
    const bool templateIdentityTagged =
        (meshTemplate.expectedClodMeshIdentityLo | meshTemplate.expectedClodMeshIdentityHi) != 0u;
    mismatch = mismatch || (templateIdentityTagged &&
        (meshTemplate.expectedClodMeshIdentityLo != metadata.meshIdentityLo ||
         meshTemplate.expectedClodMeshIdentityHi != metadata.meshIdentityHi));
    const bool drawRecordIdentityTagged =
        (drawRecord.expectedMeshIdentityLo | drawRecord.expectedMeshIdentityHi) != 0u;
    mismatch = mismatch || (drawRecordIdentityTagged &&
        (drawRecord.expectedMeshIdentityLo != metadata.meshIdentityLo ||
         drawRecord.expectedMeshIdentityHi != metadata.meshIdentityHi));
    if (!mismatch)
    {
        return false;
    }

    if (CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
        CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return true;
    }

    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX];
    uint detailIndex = 0u;
    InterlockedAdd(counter[0], 1u, detailIndex);
    if (detailIndex >= CLOD_SOURCE_GROUP_MISMATCH_DETAIL_CAPACITY)
    {
        return true;
    }

    CLodSourceGroupMismatchDetail detail;
    detail.expectedGroupLocalIndex = expectedGroupLocalIndex;
    detail.foundGroupLocalIndex = foundGroupLocalIndex;
    detail.expectedGroupGlobalIndex = metadata.groupsBase + expectedGroupLocalIndex;
    detail.foundGroupGlobalIndex = foundGroupLocalIndex != 0xFFFFFFFFu ? metadata.groupsBase + foundGroupLocalIndex : 0xFFFFFFFFu;
    detail.clodMeshMetadataIndex = offsets.clodMeshMetadataIndex;
    detail.groupsBase = metadata.groupsBase;
    detail.expectedSegmentGlobalIndex = expectedSegmentGlobalIndex;
    detail.expectedSegmentPageIndex = expectedSegmentPageIndex;
    detail.expectedSegmentFirstMeshlet = expectedSegmentFirstMeshlet;
    detail.expectedSegmentMeshletCount = expectedSegmentMeshletCount;
    detail.expectedSegmentPageSlabDescriptorIndex = expectedSegmentPageSlabDescriptorIndex;
    detail.expectedSegmentPageSlabByteOffset = expectedSegmentPageSlabByteOffset;
    detail.pageLocalMeshletIndex = pageLocalMeshletIndex;
    detail.pageSlabDescriptorIndex = pageSlabDescriptorIndex;
    detail.pageSlabByteOffset = pageSlabByteOffset;
    detail.visibleClusterIndex = visibleClusterIndex;
    detail.unsortedClusterIndex = unsortedClusterIndex;
    detail.instanceId = instanceId;
    detail.viewId = CLodVisibleClusterViewID(packedCluster);
    detail.bucketMeshletIndex = bucketMeshletIndex;
    detail.bucketCount = bucketCount;
    detail.actualOwnerMeshMetadataIndex = actualOwnerMeshMetadataIndex;
    detail.expectedTemplateMeshMetadataIndex = expectedTemplateMeshMetadataIndex;
    detail.expectedMeshIdentityLo = drawRecord.expectedMeshIdentityLo;
    detail.expectedMeshIdentityHi = drawRecord.expectedMeshIdentityHi;
    detail.actualMeshIdentityLo = metadata.meshIdentityLo;
    detail.actualMeshIdentityHi = metadata.meshIdentityHi;

    RWStructuredBuffer<CLodSourceGroupMismatchDetail> details =
        ResourceDescriptorHeap[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX];
    details[detailIndex] = detail;
    return true;
}

#endif // CLOD_SOURCE_GROUP_VALIDATION_HLSLI
