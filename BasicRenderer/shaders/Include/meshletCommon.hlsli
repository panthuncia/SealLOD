#ifndef MESHLET_COMMON_HLSLI
#define MESHLET_COMMON_HLSLI

#include "Common/defines.h"
#include "include/structs.hlsli"
#include "include/cbuffers.hlsli"
#include "include/loadingUtils.hlsli"
#include "include/vertex.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
// Meshlet description
struct Meshlet
{
    uint VertOffset;
    uint TriOffset;
    uint VertCount;
    uint TriCount;
};

Meshlet LoadMeshlet(uint4 raw)
{
    Meshlet m;
    m.VertOffset = raw.x;
    m.TriOffset = raw.y;
    m.VertCount = raw.z;
    m.TriCount = raw.w;
    return m;
}

// Load a Meshlet from a page-pool slab ByteAddressBuffer.
// meshletByteAddr = slabByteOffset + meshletIntraPageByteOffset + localMeshletIndex * 16
Meshlet LoadMeshletFromSlab(uint slabDescriptorIndex, uint meshletByteAddr)
{
    ByteAddressBuffer slabBuffer = ResourceDescriptorHeap[slabDescriptorIndex];
    return LoadMeshlet(slabBuffer.Load4(meshletByteAddr));
}

struct MeshletSetup
{
    uint drawRecordIndex;
    uint viewID; // Which view this meshlet is being rendered for (for CLod path)
    uint shadowClipmapIndex;
    uint virtualShadowPayload;
    uint meshletIndex;
    Meshlet meshlet; // Used by non-CLod path
    PerMeshBuffer meshBuffer;
    PerMeshInstanceBuffer meshInstanceBuffer;
    PerObjectBuffer objectBuffer;
    // The opaque rigid CLod path only needs these members. Keeping that path out
    // of the full PerObjectBuffer avoids carrying three matrices through every
    // mesh-shader lane.
    row_major float4x4 clodRasterModel;
    uint clodRasterObjectFlags;
    uint vertCount;
    uint triCount;
    uint vertOffset; // Non-CLod: meshlet.VertOffset; CLod: unused (0)

    // Non-CLod vertex/triangle addressing
    uint postSkinningBufferOffset;
    uint prevPostSkinningBufferOffset;
    uint groupMeshletTrianglesByteOffset; // Non-CLod triangle buffer offset

    // Per-meshlet page stream addressing from CLodMeshletDescriptor
    uint bitsX;
    uint bitsY;
    uint bitsZ;
    int3 minQ;
    uint positionBitOffset;     // byte offset within page position stream
    uint vertexAttributeOffset; // element offset within page vertex-attribute arrays
    uint triangleByteOffset;    // byte offset within page triangle stream
    uint boneListOffset;        // uint offset within page bone-index stream
    uint boneCount;
    uint assemblyTransformIndex;
    CLodMeshMetadata clodMetadata;
    uint pageAttributeMask;
    uint uvSetCount;
    uint uvDescriptorBase;
    uint uvBitstreamDirectoryBase;

    // Page-level stream base byte offsets (absolute in slab)
    uint pageByteOffset;
    uint positionBitstreamBase;
    uint normalArrayBase;
    uint colorArrayBase;
    uint jointArrayBase;
    uint weightArrayBase;
    uint triangleStreamBase;
    uint boneIndexStreamBase;

    // Mesh-wide quantization
    uint compressedPositionQuantExp;

    // Page-pool addressing (0 = non-CLod / not loaded)
    uint pagePoolSlabDescriptorIndex;  // Descriptor-heap index of the slab ByteAddressBuffer
};

bool HasValidMeshShaderOutputCounts(uint vertCount, uint triCount)
{
    return vertCount <= MS_MESHLET_SIZE && triCount <= MS_MESHLET_SIZE;
}

// Internal common initialization (indices already chosen)
bool InitializeMeshletInternal(
    uint meshletLocalIndex,
    PerMeshInstanceBuffer meshInstance,
    out MeshletSetup setup)
{
    setup.meshletIndex = meshletLocalIndex;
    setup.meshInstanceBuffer = meshInstance;
    setup.viewID = 0; // Unused
    setup.shadowClipmapIndex = CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX;
    setup.virtualShadowPayload = CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<PerObjectBuffer> perObjectBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    StructuredBuffer<Meshlet> meshletBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::MeshResources::MeshletOffsets)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];

    setup.meshBuffer = perMeshBuffer[meshInstance.perMeshBufferIndex];
    setup.objectBuffer = perObjectBuffer[meshInstance.perObjectBufferIndex];

    uint meshletOffset = setup.meshBuffer.clodMeshletBufferOffset;
    setup.meshlet = meshletBuffer[meshletOffset + setup.meshletIndex];

    setup.vertCount = setup.meshlet.VertCount;
    setup.triCount = setup.meshlet.TriCount;
    setup.vertOffset = setup.meshlet.VertOffset;
    setup.groupMeshletTrianglesByteOffset = setup.meshBuffer.clodMeshletTrianglesBufferOffset;

    // CLod per-meshlet fields unused in non-CLod path
    setup.bitsX = 0;
    setup.bitsY = 0;
    setup.bitsZ = 0;
    setup.minQ = int3(0, 0, 0);
    setup.positionBitOffset = 0;
    setup.vertexAttributeOffset = 0;
    setup.triangleByteOffset = 0;
    setup.boneListOffset = 0;
    setup.boneCount = 0;
    setup.assemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    setup.clodMetadata = (CLodMeshMetadata)0;
    setup.pageAttributeMask = 0;
    setup.uvSetCount = 0;
    setup.uvDescriptorBase = 0;
    setup.uvBitstreamDirectoryBase = 0;
    setup.pageByteOffset = 0;
    setup.positionBitstreamBase = 0;
    setup.normalArrayBase = 0;
    setup.colorArrayBase = 0;
    setup.jointArrayBase = 0;
    setup.weightArrayBase = 0;
    setup.triangleStreamBase = 0;
    setup.boneIndexStreamBase = 0;
    setup.compressedPositionQuantExp = 0;

    // setup.vertexBuffer = vertexBuffer;
    // setup.meshletTrianglesBuffer = meshletTrianglesBuffer;
    // setup.meshletVerticesBuffer = meshletVerticesBuffer;

    setup.pagePoolSlabDescriptorIndex = 0;

    uint postSkinningBase = meshInstance.postSkinningVertexBufferOffset;
    setup.postSkinningBufferOffset = postSkinningBase;
    setup.prevPostSkinningBufferOffset = postSkinningBase;

    if (setup.meshBuffer.vertexFlags & VERTEX_SKINNED)
    {
        uint stride = setup.meshBuffer.vertexByteSize * setup.meshBuffer.numVertices;
        setup.postSkinningBufferOffset += stride * (perFrameBuffer.frameIndex % 2);
        setup.prevPostSkinningBufferOffset += stride * ((perFrameBuffer.frameIndex + 1) % 2);
    }

    if (setup.meshletIndex >= setup.meshBuffer.numMeshlets)
    {
        return false;
    }

    return true;
}

// per-draw invocation (mesh shader path uses root constants set externally)
bool InitializeMeshlet(uint meshletLocalIndex, out MeshletSetup setup)
{
    StructuredBuffer<PerMeshInstanceBuffer> meshInstanceBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    PerMeshInstanceBuffer meshInstance = meshInstanceBuffer[GetRootPerMeshInstanceBufferIndex()];
    return InitializeMeshletInternal(meshletLocalIndex, meshInstance, setup);
}

// Screen-space / compute path: drawCallID directly indexes PerMeshInstanceBuffer
bool InitializeMeshletFromDrawCall(uint drawCallID, uint meshletLocalIndex, out MeshletSetup setup)
{
    StructuredBuffer<PerMeshInstanceBuffer> meshInstanceBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    PerMeshInstanceBuffer meshInstance = meshInstanceBuffer[drawCallID];
    return InitializeMeshletInternal(meshletLocalIndex, meshInstance, setup);
}

uint3 DecodeTriangleFromBuffer(ByteAddressBuffer triangleBuffer, uint triOffset)
{
    uint alignedOffset = (triOffset / 4) * 4;
    uint firstWord = triangleBuffer.Load(alignedOffset);
    uint byteOffset = triOffset % 4;

    // Load first byte
    uint b0 = (firstWord >> (byteOffset * 8)) & 0xFF;
    uint b1, b2;

    if (byteOffset <= 1)
    {
        // All three bytes are within the same word
        b1 = (firstWord >> ((byteOffset + 1) * 8)) & 0xFF;
        b2 = (firstWord >> ((byteOffset + 2) * 8)) & 0xFF;
    }
    else if (byteOffset == 2)
    {
        // The second byte is in this word, but the third byte spills into the next word
        b1 = (firstWord >> ((byteOffset + 1) * 8)) & 0xFF;
        uint secondWord = triangleBuffer.Load(alignedOffset + 4);
        b2 = secondWord & 0xFF;
    }
    else
    { // byteOffset == 3
        // The first byte is at the last position in firstWord,
        // The next two bytes must come from the next word.
        uint secondWord = triangleBuffer.Load(alignedOffset + 4);
        b1 = secondWord & 0xFF;
        b2 = (secondWord >> 8) & 0xFF;
    }

    return uint3(b0, b1, b2);
}

uint3 DecodeTriangle(uint triLocalIndex, MeshletSetup setup)
{
    ByteAddressBuffer triangleBuffer = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint triOffset = setup.triangleStreamBase + setup.triangleByteOffset + triLocalIndex * 3;
    return DecodeTriangleFromBuffer(triangleBuffer, triOffset);
}

CLodMeshletUvDescriptor LoadMeshletUvDescriptorAbsolute(MeshletSetup setup, uint uvSetIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint descriptorIndex = setup.meshletIndex * setup.uvSetCount + uvSetIndex;
    uint addr = setup.uvDescriptorBase + descriptorIndex * CLOD_MESHLET_UV_DESCRIPTOR_STRIDE;
    uint4 d0 = slab.Load4(addr + 0u);
    uint4 d1 = slab.Load4(addr + 16u);

    CLodMeshletUvDescriptor desc;
    desc.uvBitOffset = d0.x;
    desc.uvMinU = asfloat(d0.y);
    desc.uvMinV = asfloat(d0.z);
    desc.uvScaleU = asfloat(d0.w);
    desc.uvScaleV = asfloat(d1.x);
    desc.uvBits = d1.y;
    desc.reserved0 = d1.z;
    desc.reserved1 = d1.w;
    return desc;
}

uint LoadPageUvBitstreamBaseAbsolute(MeshletSetup setup, uint uvSetIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint relativeOffset = slab.Load(setup.uvBitstreamDirectoryBase + uvSetIndex * 4u);
    return setup.pageByteOffset + relativeOffset;
}

#endif // MESHLET_COMMON_HLSLI
