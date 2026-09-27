#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <BasicTelemetry/Tracy.h>
#include "Scene/ECS/RendererECSManager.h"
#include "Scene/Objects/ObjectManager.h"
#include "Scene/Views/ViewManager.h"
#include "Lighting/Lights/LightManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Streaming/TaskScheduler.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "Utilities/MathUtils.h"
#include "Utilities/Utilities.h"
#include "BasicRenderer/Scene/RendererComponents.h"

void Renderer::RunRenderResourceSyncStage() {
    BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync");

    auto& world = RendererECSManager::GetInstance().GetWorld();

    if (!m_renderSyncQueriesBuilt) {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::BuildQueries");
        m_renderSyncObjectQuery = world.query_builder<Components::Matrix, Components::RenderableObject, Components::ObjectDrawInfo, Components::MeshInstances>()
            .with<Components::Active>()
            .with<Components::RenderTransformUpdated>()
            .build();
        m_renderSyncCameraQuery = world.query_builder<Components::Matrix, Components::Camera, Components::RenderViewRef>()
            .with<Components::Active>()
            .build();
        m_renderSyncLightQuery = world.query_builder<Components::Matrix, Components::Light>()
            .with<Components::Active>()
            .build();
        m_renderTransformUpdatedCleanupQuery = world.query_builder<>()
            .with<Components::RenderTransformUpdated>()
            .build();
        m_renderSyncQueriesBuilt = true;
    }

    // Collect object entity data for parallel processing
    struct ObjectSyncItem {
        Components::Matrix* worldMatrix;
        Components::RenderableObject* object;
        Components::ObjectDrawInfo* drawInfo;
        Components::MeshInstances* meshInstances;
        const Components::InstanceTransforms* instanceTransforms;
    };
    std::vector<ObjectSyncItem> objectItems;
    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CollectObjectsAndMaterials");
        m_renderSyncObjectQuery.run([&](flecs::iter& it) {
            while (it.next()) {
                auto matrices = it.field<Components::Matrix>(0);
                auto objects = it.field<Components::RenderableObject>(1);
                auto drawInfos = it.field<Components::ObjectDrawInfo>(2);
                auto meshInstances = it.field<Components::MeshInstances>(3);
                for (auto i : it) {
                    objectItems.push_back({
                        &matrices[i],
                        &objects[i],
                        &drawInfos[i],
                        &meshInstances[i],
                        it.entity(i).try_get<Components::InstanceTransforms>()
                    });
                }
            }
        });
    }

    if (auto* terrainService = m_sceneIngestionServices.sceneAssetRequests) {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ProcessPendingTerrainUpdates");
        terrainService->ProcessPendingTerrainUpdates();
    }

    auto* objectManager = m_pObjectManager.get();
    std::vector<std::pair<size_t, size_t>> perObjectDirtyRanges;
    std::vector<std::pair<size_t, size_t>> perInstanceTransformDirtyRanges;
    std::vector<std::pair<size_t, size_t>> normalMatrixDirtyRanges;
    perObjectDirtyRanges.reserve(objectItems.size());
    perInstanceTransformDirtyRanges.reserve(objectItems.size());
    normalMatrixDirtyRanges.reserve(objectItems.size());

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ScanObjectDirtyRanges");
        const auto appendRanges = [](std::vector<std::pair<size_t, size_t>>& ranges, const std::vector<std::shared_ptr<org::BufferView>>& views, size_t stride) {
            for (const auto& view : views) {
                if (!view) {
                    continue;
                }
                const size_t begin = view->GetOffset();
                ranges.emplace_back(begin, begin + stride);
            }
        };
        for (const auto& item : objectItems) {
            if (!item.drawInfo->perObjectCBViews.empty()) {
                appendRanges(perObjectDirtyRanges, item.drawInfo->perObjectCBViews, sizeof(PerObjectCB));
            } else if (item.drawInfo->perObjectCBView) {
                const size_t perObjectBegin = item.drawInfo->perObjectCBView->GetOffset();
                perObjectDirtyRanges.emplace_back(perObjectBegin, perObjectBegin + sizeof(PerObjectCB));
            }
            appendRanges(perInstanceTransformDirtyRanges, item.drawInfo->perInstanceTransformViews, sizeof(PerInstanceTransformCB));
            if (!item.drawInfo->normalMatrixViews.empty()) {
                appendRanges(normalMatrixDirtyRanges, item.drawInfo->normalMatrixViews, sizeof(DirectX::XMFLOAT4X4));
            } else if (item.drawInfo->normalMatrixView) {
                const size_t normalMatrixBegin = item.drawInfo->normalMatrixView->GetOffset();
                normalMatrixDirtyRanges.emplace_back(normalMatrixBegin, normalMatrixBegin + sizeof(DirectX::XMFLOAT4X4));
            }
        }
    }

    // Pre-size scratch buffers single-threaded so the parallel loop can
    // memcpy into non-overlapping regions without any synchronization.
    auto perObjectHandle = objectManager->BeginPerObjectBulkWrite();
    auto perInstanceTransformHandle = objectManager->BeginPerInstanceTransformBulkWrite();
    auto normalMatrixHandle = objectManager->BeginNormalMatrixBulkWrite();

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ObjectSync");
        TaskSchedulerManager::GetInstance().ParallelFor("ObjectSync", objectItems.size(),
            [&objectItems, &perObjectHandle, &perInstanceTransformHandle, &normalMatrixHandle](size_t idx) {
                auto& item = objectItems[idx];
                auto* worldMatrix = item.worldMatrix;
                auto* object = item.object;
                auto* drawInfo = item.drawInfo;
                const auto computeNormalMatrix = [](const XMMATRIX& modelMatrix) {
                    const XMMATRIX upperLeft3x3 = XMMatrixSet(
                        XMVectorGetX(modelMatrix.r[0]), XMVectorGetY(modelMatrix.r[0]), XMVectorGetZ(modelMatrix.r[0]), 0.0f,
                        XMVectorGetX(modelMatrix.r[1]), XMVectorGetY(modelMatrix.r[1]), XMVectorGetZ(modelMatrix.r[1]), 0.0f,
                        XMVectorGetX(modelMatrix.r[2]), XMVectorGetY(modelMatrix.r[2]), XMVectorGetZ(modelMatrix.r[2]), 0.0f,
                        0.0f, 0.0f, 0.0f, 1.0f);
                    DirectX::XMFLOAT4X4 stored{};
                    XMStoreFloat4x4(&stored, XMMatrixTranspose(XMMatrixInverse(nullptr, upperLeft3x3)));
                    return stored;
                };
                const auto writeRow = [&](size_t rowIndex, PerObjectCB perObject) {
                    const auto perObjectView = rowIndex < drawInfo->perObjectCBViews.size()
                        ? drawInfo->perObjectCBViews[rowIndex]
                        : drawInfo->perObjectCBView;
                    const auto instanceTransformView = rowIndex < drawInfo->perInstanceTransformViews.size()
                        ? drawInfo->perInstanceTransformViews[rowIndex]
                        : nullptr;
                    const auto normalMatrixView = rowIndex < drawInfo->normalMatrixViews.size()
                        ? drawInfo->normalMatrixViews[rowIndex]
                        : drawInfo->normalMatrixView;

                    if (normalMatrixView) {
                        perObject.normalMatrixBufferIndex = static_cast<uint32_t>(normalMatrixView->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
                    }

                    if (perObjectView) {
                        const size_t offset = perObjectView->GetOffset();
                        std::memcpy(perObjectHandle.data + offset, &perObject, sizeof(PerObjectCB));
                    }
                    if (instanceTransformView) {
                        const size_t offset = instanceTransformView->GetOffset();
                        std::memcpy(perInstanceTransformHandle.data + offset, &perObject, sizeof(PerInstanceTransformCB));
                    }
                    if (normalMatrixView) {
                        const size_t offset = normalMatrixView->GetOffset();
                        const auto storedNormal = computeNormalMatrix(perObject.modelMatrix);
                        std::memcpy(normalMatrixHandle.data + offset, &storedNormal, sizeof(DirectX::XMFLOAT4X4));
                    }
                };

                const bool hasInstanceTransforms = item.instanceTransforms && !item.instanceTransforms->transforms.empty();
                if (hasInstanceTransforms) {
                    const size_t rowCount = std::min({
                        item.instanceTransforms->transforms.size(),
                        drawInfo->perObjectCBViews.size(),
                        drawInfo->perInstanceTransformViews.size(),
                        drawInfo->normalMatrixViews.size()
                    });
                    XMMATRIX previousFirstTransform = item.instanceTransforms->transforms.front().matrix;
                    for (size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
                        const auto& instanceTransform = item.instanceTransforms->transforms[rowIndex];
                        auto perObject = object->perObjectCB;
                        perObject.modelMatrix = instanceTransform.matrix;
                        perObject.prevModelMatrix = instanceTransform.matrix;
                        const auto& instanceTransformView = drawInfo->perInstanceTransformViews[rowIndex];
                        if (instanceTransformView && perInstanceTransformHandle.data) {
                            const size_t offset = instanceTransformView->GetOffset();
                            if (offset <= perInstanceTransformHandle.capacity &&
                                sizeof(PerInstanceTransformCB) <= perInstanceTransformHandle.capacity - offset) {
                                // The CPU shadow still contains the last uploaded row
                                // until writeRow replaces it below.
                                PerInstanceTransformCB previousRow{};
                                std::memcpy(
                                    &previousRow,
                                    perInstanceTransformHandle.data + offset,
                                    sizeof(previousRow));
                                perObject.prevModelMatrix = previousRow.modelMatrix;
                            }
                        }
                        if (rowIndex == 0) {
                            previousFirstTransform = perObject.prevModelMatrix;
                        }
                        perObject.modelInverseMatrix = XMMatrixInverse(nullptr, instanceTransform.matrix);
                        const XMVECTOR det = XMMatrixDeterminant(instanceTransform.matrix);
                        perObject.objectFlags = (XMVectorGetX(det) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
                        writeRow(rowIndex, perObject);
                    }
                    if (!item.instanceTransforms->transforms.empty()) {
                        object->perObjectCB.modelMatrix = item.instanceTransforms->transforms.front().matrix;
                        object->perObjectCB.prevModelMatrix = previousFirstTransform;
                        object->perObjectCB.modelInverseMatrix = XMMatrixInverse(nullptr, object->perObjectCB.modelMatrix);
                    }
                } else {
                    object->perObjectCB.prevModelMatrix = object->perObjectCB.modelMatrix;
                    object->perObjectCB.modelMatrix = worldMatrix->matrix;
                    object->perObjectCB.modelInverseMatrix = XMMatrixInverse(nullptr, worldMatrix->matrix);
                    const XMVECTOR det = XMMatrixDeterminant(worldMatrix->matrix);
                    object->perObjectCB.objectFlags = (XMVectorGetX(det) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
                    writeRow(0, object->perObjectCB);
                }
            });
    }

    // Register dirty ranges single-threaded.
    // Upload only the range actually written this frame. Uploading the entire
    // grown backing every frame scales badly
    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitObjectBulkWrites");
        const auto commitRanges = [](auto& ranges, auto&& commit) {
            if (ranges.empty()) {
                return;
            }
            std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
            });

            size_t begin = ranges.front().first;
            size_t end = ranges.front().second;
            for (size_t i = 1; i < ranges.size(); ++i) {
                const auto [nextBegin, nextEnd] = ranges[i];
                if (nextBegin <= end) {
                    end = std::max(end, nextEnd);
                    continue;
                }
                commit(begin, end - begin);
                begin = nextBegin;
                end = nextEnd;
            }
            commit(begin, end - begin);
        };

        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitPerObjectRanges");
            commitRanges(perObjectDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndPerObjectBulkWrite(offset, size);
            });
        }
        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitPerInstanceTransformRanges");
            commitRanges(perInstanceTransformDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndPerInstanceTransformBulkWrite(offset, size);
            });
        }
        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitNormalMatrixRanges");
            commitRanges(normalMatrixDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndNormalMatrixBulkWrite(offset, size);
            });
        }
    }

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CameraSync");
        m_renderSyncCameraQuery.each([&](flecs::entity entity, Components::Matrix& worldMatrix, Components::Camera& camera, Components::RenderViewRef& renderView) {
            const auto* externalCamera = entity.try_get<Components::ExternalCameraMatrices>();
            const XMMATRIX cameraModel = externalCamera ? externalCamera->info.viewInverse : RemoveScalingFromMatrix(worldMatrix.matrix);
            const XMMATRIX view = externalCamera ? externalCamera->info.view : XMMatrixInverse(nullptr, cameraModel);
            DirectX::XMMATRIX projection = camera.info.unjitteredProjection;
            camera.info.prevJitteredProjection = camera.info.jitteredProjection;
            camera.info.prevUnjitteredProjection = camera.info.unjitteredProjection;
            if (externalCamera) {
                projection = externalCamera->info.unjitteredProjection;
                camera.info.clippingPlanes[0] = externalCamera->info.clippingPlanes[0];
                camera.info.clippingPlanes[1] = externalCamera->info.clippingPlanes[1];
                camera.info.clippingPlanes[2] = externalCamera->info.clippingPlanes[2];
                camera.info.clippingPlanes[3] = externalCamera->info.clippingPlanes[3];
                camera.info.clippingPlanes[4] = externalCamera->info.clippingPlanes[4];
                camera.info.clippingPlanes[5] = externalCamera->info.clippingPlanes[5];
                camera.info.fov = externalCamera->info.fov;
                camera.info.aspectRatio = externalCamera->info.aspectRatio;
                camera.info.zNear = externalCamera->info.zNear;
                camera.info.zFar = externalCamera->info.zFar;
            } else if (m_jitter && entity.has<Components::PrimaryCamera>()) {
                const auto jitterPixelSpace = UpscalingManager::GetInstance().GetJitter(m_totalFramesRendered);
                camera.jitterPixelSpace = jitterPixelSpace;
                const auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
                const DirectX::XMFLOAT2 jitterNDC = {
                    (2.0f * jitterPixelSpace.x / renderRes.x),
                    (-2.0f * jitterPixelSpace.y / renderRes.y)
                };
                camera.jitterNDC = jitterNDC;
                const auto jitterMatrix = DirectX::XMMatrixTranslation(jitterNDC.x, jitterNDC.y, 0.0f);
                projection = XMMatrixMultiply(projection, jitterMatrix);
            }

            camera.info.jitteredProjection = projection;
            camera.info.prevView = camera.info.view;
            camera.info.view = view;
            camera.info.viewInverse = cameraModel;
            camera.info.viewProjection = XMMatrixMultiply(camera.info.view, projection);
            camera.info.projectionInverse = XMMatrixInverse(nullptr, projection);

            if (externalCamera) {
                camera.info.positionWorldSpace = externalCamera->info.positionWorldSpace;
            } else {
                const auto pos = GetGlobalPositionFromMatrix(worldMatrix.matrix);
                camera.info.positionWorldSpace = { pos.x, pos.y, pos.z, 1.0f };
            }

            m_pViewManager->UpdateCamera(renderView.viewID, camera.info);
        });
    }

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::LightSync");
        m_renderSyncLightQuery.each([&](flecs::entity entity, Components::Matrix& worldMatrix, Components::Light& light) {
            const XMVECTOR worldForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            light.lightInfo.dirWorldSpace = XMVector3Normalize(XMVector3TransformNormal(worldForward, worldMatrix.matrix));
            light.lightInfo.posWorldSpace = XMVectorSet(
                XMVectorGetX(worldMatrix.matrix.r[3]),
                XMVectorGetY(worldMatrix.matrix.r[3]),
                XMVectorGetZ(worldMatrix.matrix.r[3]),
                1.0f);
            switch (light.lightInfo.type) {
            case Components::LightType::Spot:
                light.lightInfo.boundingSphere = ComputeConeBoundingSphere(light.lightInfo.posWorldSpace, light.lightInfo.dirWorldSpace, light.lightInfo.maxRange, acos(light.lightInfo.outerConeAngle));
                break;
            case Components::LightType::Point:
                light.lightInfo.boundingSphere = {{
                    XMVectorGetX(worldMatrix.matrix.r[3]),
                    XMVectorGetY(worldMatrix.matrix.r[3]),
                    XMVectorGetZ(worldMatrix.matrix.r[3]),
                    light.lightInfo.maxRange }};
                break;
            default:
                break;
            }

            if (light.lightInfo.shadowCaster && entity.has<Components::LightViewInfo>()) {
                const Components::LightViewInfo& viewInfo = entity.get<Components::LightViewInfo>();
                m_pLightManager->UpdateLightBufferView(viewInfo.lightBufferView.get(), light.lightInfo);
                m_pLightManager->UpdateLightViewInfo(entity);
            }
        });
    }

}

