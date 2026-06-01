#pragma once

#include <BasicScene/Components.h>

#include <memory>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Materials/TechniqueDescriptor.h"
#include "Resources/Buffers/BufferView.h"
#include "ShaderBuffers.h"

class PixelBuffer;

namespace Components {

    struct DepthMap {
        DepthMap() = default;
        DepthMap(std::shared_ptr<PixelBuffer> depthMap, std::shared_ptr<PixelBuffer> linearDepthMap, std::shared_ptr<PixelBuffer> projectedDepthMap)
            : depthMap(depthMap), linearDepthMap(linearDepthMap), projectedDepthMap(projectedDepthMap) {
        }
        std::shared_ptr<PixelBuffer> depthMap;
        std::shared_ptr<PixelBuffer> linearDepthMap;
        std::shared_ptr<PixelBuffer> projectedDepthMap;
    };

    struct RenderViewRef {
        uint64_t viewID = 0;
    };

    struct LightViewInfo {
        std::vector<uint64_t> viewIDs;
        std::vector<int64_t> virtualShadowUnwrappedPageOffsetX;
        std::vector<int64_t> virtualShadowUnwrappedPageOffsetY;
        std::shared_ptr<BufferView> lightBufferView;
        uint32_t lightBufferIndex = 0;
        uint32_t viewInfoBufferIndex = 0;
        Matrix projectionMatrix;
        uint32_t depthResX = 0;
        uint32_t depthResY = 0;
    };

    struct RenderableObject {
        RenderableObject() {
            perObjectCB.modelMatrix = DirectX::XMMatrixIdentity();
            perObjectCB.prevModelMatrix = DirectX::XMMatrixIdentity();
            perObjectCB.modelInverseMatrix = DirectX::XMMatrixIdentity();
            perObjectCB.normalMatrixBufferIndex = 0;
            perObjectCB.objectFlags = 0;
            perObjectCB.pad[0] = 0;
            perObjectCB.pad[1] = 0;
        }
        RenderableObject(PerObjectCB cb) : perObjectCB(cb) {}
        PerObjectCB perObjectCB;
    };

    struct IndirectDrawInfo {
        std::vector<unsigned int> indices;
        std::vector<std::shared_ptr<BufferView>> views;
        std::vector<std::vector<DrawWorkloadKey>> drawWorkloadKeysPerDraw;
    };

    struct ObjectDrawInfo {
        struct BufferRange {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t stride = 0;
            uint64_t count = 0;

            bool IsValid() const {
                return size != 0 && stride != 0 && count != 0;
            }
        };

        IndirectDrawInfo drawInfo;
        std::vector<uint32_t> perMeshInstanceBufferIndices;
        std::vector<uint32_t> instanceDrawRecordIndices;
        std::shared_ptr<BufferView> perObjectCBView;
        uint32_t perObjectCBIndex;
        std::shared_ptr<BufferView> normalMatrixView;
        uint32_t normalMatrixIndex;
        std::vector<std::shared_ptr<BufferView>> perObjectCBViews;
        std::vector<std::shared_ptr<BufferView>> perInstanceTransformViews;
        std::vector<std::shared_ptr<BufferView>> normalMatrixViews;
        std::vector<std::shared_ptr<BufferView>> instanceDrawRecordViews;
        BufferRange perObjectCBRange;
        BufferRange perInstanceTransformRange;
        BufferRange normalMatrixRange;
        BufferRange instanceDrawRecordRange;
        std::vector<BufferRange> ownedPerObjectCBPages;
        std::vector<BufferRange> ownedPerInstanceTransformPages;
        std::vector<BufferRange> ownedNormalMatrixPages;
        std::vector<BufferRange> ownedInstanceDrawRecordPages;
    };

    struct PerPassMeshes {
        std::unordered_map<uint64_t, std::vector<std::shared_ptr<MeshInstance>>> meshesByPass;
    };

    /// Tag added to render-world entities whose data was updated during IngestSnapshot.
    /// Consumed by RunRenderResourceSyncStage to limit GPU buffer writes.
    struct RenderTransformUpdated {};

} // namespace Components
