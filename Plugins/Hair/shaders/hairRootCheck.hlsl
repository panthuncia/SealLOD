// The self-check's root frames (hair.selfcheck): writes the frame of each root the CPU listed, this frame's and the
// previous frame's, into the check buffer, which the CPU reads back and compares with its own arithmetic. The frames
// are worked out as every hair pass works them out (hairRootFrame.hlsli); no pass reads what this one writes. It is
// dispatched only in a frame the self-check samples: one thread per listed root.

// Paths are relative to the renderer's runtime root, where the build stages this folder as HairShaders/.
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "HairShaders/hairStructs.hlsli"
#include "HairShaders/hairRootFrame.hlsli"

// HairRootCheckPass::Record. The renderer's own buffers (per-object rows, bone palettes, skinning instance rows) are
// bound by name, as in the renderer's shaders.
#define HAIR_INSTANCES UintRootConstant0        // StructuredBuffer<HairInstanceGPU>
#define HAIR_ROOT_BINDINGS UintRootConstant1    // StructuredBuffer<HairRootBinding>
#define HAIR_ROOT_CHECK UintRootConstant2       // RWStructuredBuffer<HairRootFramePair>
#define HAIR_ROOT_CHECK_LIST UintRootConstant3  // StructuredBuffer<HairRootCheckSample>
#define HAIR_FRAME_STATE UintRootConstant4      // StructuredBuffer<HairFrameGPU>
#define HAIR_ROOT_CHECK_COUNT UintRootConstant5 // rows of the list this frame

[shader("compute")]
[numthreads(64, 1, 1)]
void HairRootCheckCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= HAIR_ROOT_CHECK_COUNT)
    {
        return;
    }
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    StructuredBuffer<HairRootCheckSample> samples = ResourceDescriptorHeap[HAIR_ROOT_CHECK_LIST];
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    const HairRootCheckSample sample = samples[dispatchThreadId.x];
    const HairInstanceGPU instance = instances[sample.instanceIndex];
    if (sample.strandRow >= instance.strandCount)
    {
        return;
    }
    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<HairRootFramePair> rootCheck = ResourceDescriptorHeap[HAIR_ROOT_CHECK];
    rootCheck[dispatchThreadId.x] = HairMakeRootFramePair(instance, rootBindings[instance.strandBase + sample.strandRow],
        perObject[instance.perObjectIndex], frameState[0].frameNumber);
}
