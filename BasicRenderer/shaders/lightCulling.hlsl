#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/lightModel.hlsli"
#include "PerPassRootConstants/lightCullingRootConstants.h"

// Returns true if a sphere (with a given center and radius) intersects an AABB
// defined by aabbMin and aabbMax
bool sphereAABBIntersection(float3 center, float radius, float3 aabbMin, float3 aabbMax) {
    // Determine the closest point on the AABB to the sphere center.
    float3 closestPoint = max(aabbMin, min(center, aabbMax));
    float3 distance = closestPoint - center;
    return dot(distance, distance) <= radius * radius;
}

bool testSphereAABB(matrix viewMatrix, float3 position, float radius, Cluster cluster) {
    // Transform the point light position into view space.
    float3 center = mul(float4(position, 1.0), viewMatrix).xyz;
    float3 aabbMin = cluster.minPoint.xyz;
    float3 aabbMax = cluster.maxPoint.xyz;
    return sphereAABBIntersection(center, radius, aabbMin, aabbMax);
}

unsigned int AllocatePage(RWStructuredBuffer<uint> LinkedListCounter) {
    uint index;
    InterlockedAdd(LinkedListCounter[0], 1, index);

    if (index >= LIGHT_PAGES_POOL_SIZE)
        return LIGHT_PAGE_ADDRESS_NULL;

    return index;
}

int MakePageLink(int nAddress, int nNewHeadAddress, RWStructuredBuffer<Cluster> lightClustersUAV) {
    int nOldHeadAddress;
    InterlockedExchange(lightClustersUAV[nAddress].ptrFirstPage, nNewHeadAddress, nOldHeadAddress);
    return nOldHeadAddress;
}

// Compute shader entry point.
// Each thread is responsible for processing a single cluster.
[numthreads(128, 1, 1)]
void CSMain(uint3 groupID : SV_GroupID,
          uint3 threadID : SV_GroupThreadID) {
    // Compute the global index for the cluster.
    uint index = groupID.x * 128 + threadID.x;
    
    StructuredBuffer<unsigned int> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint lightCount = perFrameBuffer.numLights;

    uint totalClusters =
        perFrameBuffer.lightClusterGridSizeX *
        perFrameBuffer.lightClusterGridSizeY *
        perFrameBuffer.lightClusterGridSizeZ;
    if (index >= totalClusters)
        return;
    
    RWStructuredBuffer<Cluster> clusters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ClusterBuffer)];
    Cluster cluster = clusters[index];
    
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera primaryCamera = cameras[perFrameBuffer.mainCameraIndex];

    // Light pages, clusters index into linked list
    RWStructuredBuffer<LightPage> lightPages = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PagesBuffer)];
    RWStructuredBuffer<uint> LinkedListCounter = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PagesCounter)];

    // Process every point light.
    
    // Start with a new page for the light.
    unsigned int pageIndex = AllocatePage(LinkedListCounter);
    cluster.numLights = 0;
    cluster.ptrFirstPage = pageIndex;

    if (pageIndex == LIGHT_PAGE_ADDRESS_NULL) {
        // No more pages available.
        clusters[index] = cluster;
        return;
    }
    
    lightPages[pageIndex].ptrNextPage = LIGHT_PAGE_ADDRESS_NULL; // Initialize the next page pointer to null.
    
    uint numLightsInPage = 0;
    for (uint i = 0; i < lightCount; ++i) {
        
        // When we process LIGHTS_PER_PAGE lights, we need to allocate a new page.
        if (numLightsInPage >= LIGHTS_PER_PAGE) {
            lightPages[pageIndex].numLightsInPage = LIGHTS_PER_PAGE; // Store the number of lights in the current page.
            uint oldPageIndex = pageIndex; // Store the current page index.
            pageIndex = AllocatePage(LinkedListCounter); // Allocate a new page.
            if (pageIndex == LIGHT_PAGE_ADDRESS_NULL) {
                break;
            }
            lightPages[pageIndex].ptrNextPage = oldPageIndex; // Link the new page to the old page.
            cluster.ptrFirstPage = pageIndex; // Update the cluster with the new page index.
            numLightsInPage = 0;
        }
        
        // If the light intersects the cluster. Pages store light buffer indices, not active-list slots.
        LightInfo light = lights[activeLightIndices[i]];
        switch (light.type) {
            case LIGHT_TYPE_POINT:
            case LIGHT_TYPE_SPOT: // Its bounding sphere encloses the cone.
                if (testSphereAABB(primaryCamera.view, light.boundingSphere.sphere.xyz, light.boundingSphere.sphere.w, cluster)) {
                    lightPages[pageIndex].lightIndices[numLightsInPage] = activeLightIndices[i];
                    numLightsInPage++;
                    cluster.numLights++;
                }
                break;
            case LIGHT_TYPE_DIRECTIONAL:
                // Directional lights always intersect the cluster.
                lightPages[pageIndex].lightIndices[numLightsInPage] = activeLightIndices[i];
                numLightsInPage++;
                cluster.numLights++;
                break;
        }
    }
    
    // Update last page count
    if (pageIndex != LIGHT_PAGE_ADDRESS_NULL) {
        lightPages[pageIndex].numLightsInPage = numLightsInPage;
    }
    
    // Write back the updated cluster.
    clusters[index] = cluster;
}