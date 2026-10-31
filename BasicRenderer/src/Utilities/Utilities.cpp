#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include "Utilities/Utilities.h"

#include <wrl.h>
#include <stdexcept>
#include <algorithm>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_set>
#define _USE_MATH_DEFINES
#include <math.h>
#include <optional>
#include <gsl/gsl>
#include <rhi_helpers.h>
#include <rhi_conversions_dx12.h>
#include <tracy/Tracy.hpp>

#include "Assets/Textures/Cache/ProcessedTextureCache.h"
#include "Runtime/Device/DeviceManager.h"

#include "Scene/Views/DefaultDirection.h"
#include "Runtime/IO/DirectStorageManager.h"
#include "Resources/Sampler.h"
#include "Render/DescriptorHeap.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "BasicRenderer/Assets/Material.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Assets/Texture.h"

using namespace DirectX;

void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        char message[64]{};
        std::snprintf(message, sizeof(message), "HRESULT failed: 0x%08lX", static_cast<unsigned long>(hr));
        std::cerr << message << std::endl;
        throw std::runtime_error(message);
    }
}

std::shared_ptr<Mesh> MeshFromData(MeshData&& meshData, std::wstring name, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD) {
    const bool hasTexcoords = !meshData.uvSets.empty() && !meshData.uvSets[0].values.empty();
    const bool hasColors = meshData.colors.size() == (meshData.positions.size() / 3u);
    bool hasJoints = !meshData.joints.empty() && !meshData.weights.empty();

    std::unique_ptr<std::vector<std::byte>> rawData = std::make_unique<std::vector<std::byte>>();
    uint32_t numVertices = static_cast<uint32_t>(meshData.positions.size()) / 3;
    uint32_t vertexFlags = meshData.flags;
    if (hasTexcoords) {
        vertexFlags |= VertexFlags::VERTEX_TEXCOORDS;
    }
    if (hasColors) {
        vertexFlags |= VertexFlags::VERTEX_COLORS;
    }

    const uint8_t vertexSize = static_cast<uint8_t>(MeshVertexLayout::VertexSize(vertexFlags));
    rawData->resize(numVertices * vertexSize);

    for (unsigned int i = 0; i < numVertices; i++) {
        size_t baseOffset = i * vertexSize;
        memcpy(rawData->data() + baseOffset, &meshData.positions[i * 3], sizeof(XMFLOAT3));
        size_t offset = MeshVertexLayout::NormalOffset;
        memcpy(rawData->data() + baseOffset + offset, &meshData.normals[i * 3], sizeof(XMFLOAT3));
        if (hasTexcoords) {
            offset = MeshVertexLayout::TexcoordOffset(vertexFlags);
            memcpy(rawData->data() + baseOffset + offset, &meshData.uvSets[0].values[i], sizeof(XMFLOAT2));
        }
        if (hasColors) {
            offset = MeshVertexLayout::ColorOffset(vertexFlags);
            memcpy(rawData->data() + baseOffset + offset, &meshData.colors[i], sizeof(XMFLOAT3));
        }
    }
    constexpr size_t kMaxSkinInfluences = 8u;
    // position,       normal            joints[8],        weights[8]
    unsigned int skinningVertexSize = sizeof(XMFLOAT3) + sizeof(XMFLOAT3)
        + static_cast<unsigned int>(sizeof(uint32_t) * kMaxSkinInfluences + sizeof(float) * kMaxSkinInfluences);
    std::unique_ptr<std::vector<std::byte>> skinningData = std::make_unique<std::vector<std::byte>>();
    if (hasJoints) {
        skinningData->resize(numVertices * skinningVertexSize);
        for (unsigned int i = 0; i < numVertices; i++) {
            size_t baseOffset = i * skinningVertexSize;
            memcpy(skinningData->data() + baseOffset, &meshData.positions[i * 3], sizeof(XMFLOAT3));
            size_t offset = sizeof(XMFLOAT3);
            memcpy(skinningData->data() + baseOffset + offset, &meshData.normals[i * 3], sizeof(XMFLOAT3));
            offset += sizeof(XMFLOAT3);
            const size_t availableJointCount = meshData.joints.size() / numVertices;
            const size_t availableWeightCount = meshData.weights.size() / numVertices;
            const size_t influenceCount = std::min({ kMaxSkinInfluences, availableJointCount, availableWeightCount });
            std::array<uint32_t, kMaxSkinInfluences> joints{};
            std::array<float, kMaxSkinInfluences> weights{};
            for (size_t influenceIndex = 0; influenceIndex < influenceCount; ++influenceIndex) {
                joints[influenceIndex] = meshData.joints[i * availableJointCount + influenceIndex];
                weights[influenceIndex] = meshData.weights[i * availableWeightCount + influenceIndex];
            }
            memcpy(skinningData->data() + baseOffset + offset, joints.data(), sizeof(uint32_t) * kMaxSkinInfluences);
            offset += sizeof(uint32_t) * kMaxSkinInfluences;
            memcpy(skinningData->data() + baseOffset + offset, weights.data(), sizeof(float) * kMaxSkinInfluences);
        }
    }

    std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices;
	if (hasJoints) {
		skinningVertices = std::move(skinningData);
	}

    auto mesh = Mesh::CreateShared(std::move(rawData), vertexSize, std::move(skinningVertices), skinningVertexSize, meshData.indices, std::move(meshData.uvSets), meshData.material, vertexFlags, std::move(prebuiltClusterLOD));
    if (hasJoints && numVertices != 0) {
        const size_t availableJointCount = meshData.joints.size() / numVertices;
        const size_t availableWeightCount = meshData.weights.size() / numVertices;
        const size_t sampleCount = std::min({ kMaxSkinInfluences, availableJointCount, availableWeightCount });
        std::vector<uint32_t> sampleJoints;
        std::vector<float> sampleWeights;
        sampleJoints.reserve(sampleCount);
        sampleWeights.reserve(sampleCount);
        for (size_t influenceIndex = 0; influenceIndex < sampleCount; ++influenceIndex) {
            sampleJoints.push_back(meshData.joints[influenceIndex]);
            sampleWeights.push_back(meshData.weights[influenceIndex]);
        }
        mesh->SetSkinningDebugSample(std::move(sampleJoints), std::move(sampleWeights));
    }
    return mesh;
}

XMMATRIX RemoveScalingFromMatrix(const XMMATRIX& initialMatrix) {
    XMVECTOR translation = initialMatrix.r[3];
    XMVECTOR right = initialMatrix.r[0];
    XMVECTOR up = initialMatrix.r[1];
    XMVECTOR forward = initialMatrix.r[2];
    right = XMVector3Normalize(right);
    up = XMVector3NormalizeEst(up);
    forward = XMVector3Normalize(forward);

    XMMATRIX result = XMMatrixIdentity();
    result.r[0] = right;
    result.r[1] = up;
    result.r[2] = forward;
    result.r[3] = translation;

    return result;
}

struct ImageData {
    stbi_uc* data;
    int width;
    int height;
    int channels;

    ~ImageData() {
        if (data) {
            stbi_image_free(data);
        }
    }
};

//static RawImage RawFromSTBI(const ImageData& in)
//{
//    RawImage out{};
//    out.width = static_cast<uint32_t>(in.width);
//    out.height = static_cast<uint32_t>(in.height);
//    out.channels = static_cast<uint32_t>(in.channels);
//
//    // Map channels to DXGI_FORMAT (simple 8 bit path)
//    switch (out.channels) {
//    case 1: out.format = DXGI_FORMAT_R8_UNORM;        break;
//    case 2: out.format = DXGI_FORMAT_R8G8_UNORM;      break;
//    case 3: // fallthrough
//    case 4: out.format = DXGI_FORMAT_R8G8B8A8_UNORM; break; // force RGBA upload
//    default: throw std::runtime_error("Unsupported channel count");
//    }
//
//    out.rowPitch = out.width * out.channels;
//    out.slicePitch = out.rowPitch * out.height;
//    out.pixels = reinterpret_cast<const uint8_t*>(in.data);
//    out.alphaAllOpaque = false; // No way to detect with stbi
//
//    return out;
//}

//std::shared_ptr<Texture>
//LoadTextureFromFileSTBI(const std::string& filenameUtf8, std::shared_ptr<Sampler> sampler)
//{
//    ImageData img = LoadSTBImage(filenameUtf8.c_str());
//    if (!img.data) {
//		throw std::runtime_error("Failed to load texture from file (stb): " + filenameUtf8);
//    }
//
//    RawImage raw = RawFromSTBI(img);
//    raw.filepathUtf8 = filenameUtf8;
//    return CreateTextureFromRaw(raw, sampler);
//    stbi_image_free(img.data);
//}
//
//std::shared_ptr<Texture>
//LoadTextureFromMemorySTBI(const void* bytes, size_t byteCount, std::shared_ptr<Sampler> sampler)
//{
//    int w = 0, h = 0, n = 0;
//    // Force 4 channels: TODO: Don't I handle this elsewhere already?
//    unsigned char* pixels = stbi_load_from_memory(
//        reinterpret_cast<const stbi_uc*>(bytes), static_cast<int>(byteCount), &w, &h, &n, 4);
//    if (!pixels) throw std::runtime_error("Failed to load texture from memory (stb)");
//
//    ImageData img{};
//    img.width = w; img.height = h; img.channels = 4; img.data = pixels;
//    auto guard = gsl::finally([&]() { stbi_image_free(img.data); });
//
//    RawImage raw = RawFromSTBI(img);
//    return CreateTextureFromRaw(raw, sampler);
//}


namespace detail {


    bool ReadProcessedTextureCacheHeader(
        const std::wstring& filePath,
        br::processed_texture_cache::FileHeader& header)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return false;
        }

        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || static_cast<size_t>(file.gcount()) != sizeof(header)) {
            return false;
        }

        if (header.magic != br::processed_texture_cache::kMagic ||
            header.version != br::processed_texture_cache::kVersion ||
            header.headerSize != sizeof(header) ||
            header.dataOffset < sizeof(header) ||
            header.dataSizeBytes == 0) {
            return false;
        }

        return true;
    }

    std::optional<org::TextureDescription> TryBuildDeferredConditionedCacheDescription(
        const std::wstring& filePath,
        bool allowRTV,
        bool allowUAV,
        std::string* outFailureReason = nullptr)
    {
        if (outFailureReason) {
            outFailureReason->clear();
        }

        br::processed_texture_cache::FileHeader header{};
        if (!ReadProcessedTextureCacheHeader(filePath, header)) {
            if (outFailureReason) {
                *outFailureReason = "failed to read conditioned texture cache header";
            }
            return std::nullopt;
        }

        if (header.baseWidth == 0 || header.baseHeight == 0 || header.mipLevels == 0 ||
            header.subresourceCount == 0 || header.totalArraySlices == 0 ||
            header.subresourceCount != header.totalArraySlices * header.mipLevels) {
            if (outFailureReason) {
                *outFailureReason = "conditioned texture cache header has inconsistent dimensions";
            }
            return std::nullopt;
        }

        org::TextureDescription desc{};
        desc.format = static_cast<rhi::Format>(header.format);
        desc.channels = static_cast<unsigned short>(header.channels);
        desc.isCubemap = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsCubemap);
        desc.isArray = br::processed_texture_cache::HasFlag(header, br::processed_texture_cache::FlagIsArray);
        desc.arraySize = desc.isCubemap
            ? (std::max)(1u, header.totalArraySlices / 6u)
            : (std::max)(1u, header.arraySize);
        desc.hasRTV = allowRTV;
        desc.hasUAV = allowUAV;
        desc.generateMipMaps = false;
        desc.initialLayout = rhi::ResourceLayout::Common;
        desc.imageDimensions.reserve(header.subresourceCount);

        const DXGI_FORMAT dxgiFormat = rhi::ToDxgi(desc.format);
        for (uint32_t arraySlice = 0; arraySlice < header.totalArraySlices; ++arraySlice) {
            for (uint32_t mip = 0; mip < header.mipLevels; ++mip) {
                const size_t mipWidth = (std::max)(size_t(1), static_cast<size_t>(header.baseWidth) >> mip);
                const size_t mipHeight = (std::max)(size_t(1), static_cast<size_t>(header.baseHeight) >> mip);
                size_t rowPitch = 0;
                size_t slicePitch = 0;
                if (FAILED(DirectX::ComputePitch(dxgiFormat, mipWidth, mipHeight, rowPitch, slicePitch))) {
                    if (outFailureReason) {
                        *outFailureReason = "failed to compute conditioned texture cache pitch";
                    }
                    return std::nullopt;
                }

                org::ImageDimensions dims{};
                dims.width = static_cast<uint32_t>(mipWidth);
                dims.height = static_cast<uint32_t>(mipHeight);
                dims.rowPitch = rowPitch;
                dims.slicePitch = slicePitch;
                desc.imageDimensions.push_back(dims);
            }
        }

        return desc;
    }

}

std::optional<org::TextureDescription> br::textures::ReadConditionedCacheDescription(const std::wstring& path)
{
    return ::detail::TryBuildDeferredConditionedCacheDescription(path, false, false);
}

DirectX::XMMATRIX createDirectionalLightViewMatrix(XMVECTOR lightDir, XMVECTOR center) {
    auto mat = XMMatrixLookToRH(center, lightDir, XMVectorSet(0, 1, 0, 1));
    return mat;
}

void CalculateFrustumCorners(const DirectX::XMVECTOR& camPos, const DirectX::XMVECTOR& camDir, const DirectX::XMVECTOR& camUp, float nearPlane, float farPlane, float fovY, float aspectRatio, std::array<XMVECTOR, 8>& corners) {

    // Calculate the dimensions of the near and far planes
    float tanHalfFovy = tanf(fovY / 2.0f);
    float nearHeight = 2.0f * tanHalfFovy * nearPlane;
    float nearWidth = nearHeight * aspectRatio;

    float farHeight = 2.0f * tanHalfFovy * farPlane;
    float farWidth = farHeight * aspectRatio;

    XMVECTOR camRight = XMVector3Cross(camDir, camUp);

    XMVECTOR nearCenter = camPos + camDir * nearPlane;
    XMVECTOR farCenter = camPos + camDir * farPlane;

    // Near plane
    corners[0] = nearCenter + (camUp * (nearHeight / 2.0f)) - (camRight * (nearWidth / 2.0f)); // Top-left
    corners[1] = nearCenter + (camUp * (nearHeight / 2.0f)) + (camRight * (nearWidth / 2.0f)); // Top-right
    corners[2] = nearCenter - (camUp * (nearHeight / 2.0f)) - (camRight * (nearWidth / 2.0f)); // Bottom-left
    corners[3] = nearCenter - (camUp * (nearHeight / 2.0f)) + (camRight * (nearWidth / 2.0f)); // Bottom-right

    // Far plane
    corners[4] = farCenter + (camUp * (farHeight / 2.0f)) - (camRight * (farWidth / 2.0f)); // Top-left
    corners[5] = farCenter + (camUp * (farHeight / 2.0f)) + (camRight * (farWidth / 2.0f)); // Top-right
    corners[6] = farCenter - (camUp * (farHeight / 2.0f)) - (camRight * (farWidth / 2.0f)); // Bottom-left
    corners[7] = farCenter - (camUp * (farHeight / 2.0f)) + (camRight * (farWidth / 2.0f)); // Bottom-right
}

std::vector<Cascade> setupCascades(
    int numCascades, 
    const DirectX::XMVECTOR& lightDir, 
    const DirectX::XMVECTOR& camPos, 
    const DirectX::XMVECTOR& camDir, 
    const DirectX::XMVECTOR& camUp, 
    float nearPlane, 
    float fovY, 
    float aspectRatio, 
    const std::vector<float>& cascadeSplits)
{
    using namespace DirectX;
    std::vector<Cascade> cascades;
    cascades.reserve(numCascades);

    // Compute the camera's right vector
    XMVECTOR camRight = XMVector3Normalize(XMVector3Cross(XMVector3Normalize(camDir), XMVector3Normalize(camUp)));

    // Loop over cascades.
    for (int i = 0; i < numCascades; ++i)
    {
        // Determine the near and far distances for this cascade
        float cascadeNear = (i == 0) ? nearPlane : cascadeSplits[i - 1];
        float cascadeFar  = cascadeSplits[i];

        // Compute the center of the near and far planes
        XMVECTOR nearCenter = camPos + camDir * cascadeNear;
        XMVECTOR farCenter  = camPos + camDir * cascadeFar;

        // Calculate half-heights and half-widths at the near and far planes
        float tanFov = tanf(fovY * 0.5f);
        float nearHeight = tanFov * cascadeNear;
        float nearWidth  = nearHeight * aspectRatio;
        float farHeight  = tanFov * cascadeFar;
        float farWidth   = farHeight * aspectRatio;

        // Compute the 8 corners of the cascade frustum in world space
        // Near plane corners
        XMVECTOR nearTopLeft     = nearCenter + camUp * nearHeight - camRight * nearWidth;
        XMVECTOR nearTopRight    = nearCenter + camUp * nearHeight + camRight * nearWidth;
        XMVECTOR nearBottomLeft  = nearCenter - camUp * nearHeight - camRight * nearWidth;
        XMVECTOR nearBottomRight = nearCenter - camUp * nearHeight + camRight * nearWidth;
        // Far plane corners
        XMVECTOR farTopLeft     = farCenter + camUp * farHeight - camRight * farWidth;
        XMVECTOR farTopRight    = farCenter + camUp * farHeight + camRight * farWidth;
        XMVECTOR farBottomLeft  = farCenter - camUp * farHeight - camRight * farWidth;
        XMVECTOR farBottomRight = farCenter - camUp * farHeight + camRight * farWidth;

        // Collect all eight frustum corners
        XMVECTOR frustumCorners[8] = {
            nearTopLeft, nearTopRight, nearBottomLeft, nearBottomRight,
            farTopLeft, farTopRight, farBottomLeft, farBottomRight
        };

        // Compute the centroid of the frustum corners
        XMVECTOR frustumCenter = XMVectorZero();
        for (int j = 0; j < 8; ++j)
        {
            frustumCenter = XMVectorAdd(frustumCenter, frustumCorners[j]);
        }
        frustumCenter = XMVectorScale(frustumCenter, 1.0f / 8.0f);

        // Determine the radius of a sphere that bounds all frustum corners
        float radius = 0.0f;
        for (int j = 0; j < 8; ++j)
        {
            float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(frustumCorners[j], frustumCenter)));
            radius = std::max(radius, distance);
        }
        // Quantize the radius to reduce shimmering
        radius = ceilf(radius * 16.0f) / 16.0f;

        // Position the light so that it covers the cascade bounding sphere
        // The light position is shifted back along the light direction
        XMVECTOR lightPos = frustumCenter -lightDir * radius * 2.0;
        // Choose a suitable "up" vector for the light (avoid colinearity with the light direction)
        XMVECTOR lightUp = (fabs(XMVectorGetY(lightDir)) > 0.99f) ? XMVectorSet(0, 0, -1, 0) : XMVectorSet(0, 1, 0, 0);
        XMMATRIX lightView = XMMatrixLookAtRH(lightPos, frustumCenter, lightUp);

        // Transform frustum corners into light space
        XMVECTOR lightSpaceCorners[8];
        for (int j = 0; j < 8; ++j)
        {
            lightSpaceCorners[j] = XMVector3TransformCoord(frustumCorners[j], lightView);
        }

        // Compute the axis-aligned bounding box in light space
        XMVECTOR mins = lightSpaceCorners[0];
        XMVECTOR maxs = lightSpaceCorners[0];
        for (int j = 1; j < 8; ++j)
        {
            mins = XMVectorMin(mins, lightSpaceCorners[j]);
            maxs = XMVectorMax(maxs, lightSpaceCorners[j]);
        }
        // Extract the bounds
        float l = XMVectorGetX(mins);
        float r = XMVectorGetX(maxs);
        float b = XMVectorGetY(mins);
        float t = XMVectorGetY(maxs);
        float n = (std::min)(XMVectorGetZ(maxs), -20.0f); // TODO: hack to avoid near shadows disappearing on objects behind the camera. Is there a better way?
        float f = -XMVectorGetZ(mins); // far

        XMMATRIX lightOrtho = XMMatrixOrthographicOffCenterRH(l, r, b, t, n, f);

        // Prepare the cascade.
        Cascade cascade;
        cascade.size = radius * 2;
        cascade.viewMatrix = lightView;
        cascade.orthoMatrix = lightOrtho;

        // Combine view and projection matrices for plane extraction
        XMMATRIX comboMatrix = lightOrtho;

        // Helper lambda to extract one clipping plane from the combined matrix
        auto ExtractPlane = [&comboMatrix](int planeIndex) -> ClippingPlane {
            // Store the combined matrix into a float4x4 structure
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, comboMatrix);
            XMVECTOR planeVec;
            // 0: left, 1: right, 2: bottom, 3: top, 4: near, 5: far
            switch (planeIndex) {
            case 0: // left
                planeVec = XMVectorSet(m._14 + m._11,
                    m._24 + m._21,
                    m._34 + m._31,
                    m._44 + m._41);
                break;
            case 1: // right
                planeVec = XMVectorSet(m._14 - m._11,
                    m._24 - m._21,
                    m._34 - m._31,
                    m._44 - m._41);
                break;
            case 2: // bottom
                planeVec = XMVectorSet(m._14 + m._12,
                    m._24 + m._22,
                    m._34 + m._32,
                    m._44 + m._42);
                break;
            case 3: // top
                planeVec = XMVectorSet(m._14 - m._12,
                    m._24 - m._22,
                    m._34 - m._32,
                    m._44 - m._42);
                break;
            case 4: // near
                planeVec = XMVectorSet(m._13,
                    m._23,
                    m._33,
                    m._43);
                break;
            case 5: // far
                planeVec = XMVectorSet(m._14 - m._13,
                    m._24 - m._23,
                    m._34 - m._33,
                    m._44 - m._43);
                break;
            default:
                planeVec = XMVectorZero();
                break;
            }
            // Normalize the plane
            planeVec = XMPlaneNormalize(planeVec);
            ClippingPlane result;
            XMStoreFloat4(&result.plane, planeVec);
            return result;
            };

        // Extract all six clipping planes
        for (int p = 0; p < 6; ++p)
        {
            cascade.frustumPlanes[p] = ExtractPlane(p);
        }

        cascades.push_back(cascade);
    }
    return cascades;
}

std::vector<Cascade> setupDirectionalClipmaps(
    int numClipmaps,
    const DirectX::XMVECTOR& lightDir,
    const DirectX::XMVECTOR& camPos,
    const DirectX::XMVECTOR& camDir,
    const DirectX::XMVECTOR& camUp,
    float nearPlane,
    float fovY,
    float aspectRatio,
    const std::vector<float>& clipFarPlanes,
    float shadowDistanceLowerBound,
    float clipSceneExtent,
    float resolutionScale)
{
    using namespace DirectX;
    std::vector<Cascade> clipmaps;
    clipmaps.reserve(numClipmaps);

    (void)camDir;
    (void)camUp;
    XMVECTOR lightUp = (fabs(XMVectorGetY(lightDir)) > 0.99f) ? XMVectorSet(0, 0, -1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMVECTOR normalizedLightDir = XMVector3Normalize(lightDir);
    const XMMATRIX defaultLightView = XMMatrixLookToRH(
        XMVectorZero(),
        normalizedLightDir,
        lightUp);
    const XMMATRIX defaultLightViewInverse = XMMatrixInverse(nullptr, defaultLightView);

    const float clipZeroFar = !clipFarPlanes.empty()
        ? std::max(clipFarPlanes.front(), nearPlane)
        : std::max(nearPlane * 2.0f, 1.0f);
    const float tanHalfFov = tanf(fovY * 0.5f);
    const float clipZeroHalfHeight = clipZeroFar * tanHalfFov;
    const float clipZeroHalfWidth = clipZeroHalfHeight * aspectRatio;
    const float cameraDerivedClipZeroScale = std::max(
        std::sqrt(clipZeroHalfWidth * clipZeroHalfWidth + clipZeroHalfHeight * clipZeroHalfHeight),
        1.0f);
    // A positive scene extent is the maximum camera-to-edge distance that the
    // coarsest clip must cover.  Work backwards through the power-of-two ladder
    // so the final clip fits that extent exactly, just as the terrain RVT fits
    // its clip ladder to the terrain domain.  This prevents a very large camera
    // far plane from making every VSM page unnecessarily coarse.
    const float clipLadderScale = std::pow(2.0f, static_cast<float>(std::max(numClipmaps - 1, 0)));
    const float unscaledClipZeroScale = clipSceneExtent > 0.0f
        ? std::max(clipSceneExtent / clipLadderScale, 1.0e-4f)
        : cameraDerivedClipZeroScale;
    // Scale the complete physical clip ladder continuously while keeping the
    // virtual page table fixed. The GPU retains the same LOD bias for clip
    // ownership, so fractional values alter world texel/page footprint without
    // snapping ownership to an adjacent integer clip level.
    const float clipZeroScale = unscaledClipZeroScale *
        std::max(resolutionScale, 1.0e-4f);
    const float ndcPageSize = 2.0f / static_cast<float>(CLodVirtualShadowFixedVirtualPageCountPerAxis);
    const float clampedShadowDistanceLowerBound = std::max(shadowDistanceLowerBound, 1.0f);
    const float clipHeightOffsetScale = 5.0f;
    const float clipNearScale = 0.01f;
    const float clipFarScale = 10.0f;

    for (int i = 0; i < numClipmaps; ++i)
    {
        const float clipScale = clipZeroScale * std::pow(2.0f, static_cast<float>(i));
        // Preserve per-level depth precision, but never contract below the
        // content-configured caster distance. Content can raise this bound for
        // unusually tall or distant casters without forcing every inner clip
        // to inherit the coarsest clip's potentially enormous depth range.
        const float nearDistance = std::max(
            std::max(clipNearScale * clipScale, clampedShadowDistanceLowerBound * 0.001f),
            0.01f);
        const float farDistance = std::max(
            std::max(clipFarScale * clipScale, clampedShadowDistanceLowerBound),
            nearDistance + 1.0f);
        const XMMATRIX lightOrtho = XMMatrixOrthographicOffCenterRH(
            -clipScale,
            clipScale,
            -clipScale,
            clipScale,
            nearDistance,
            farDistance);

        const float pageWorldSize = clipScale * ndcPageSize;
        const XMVECTOR targetLightView = XMVector3TransformCoord(camPos, defaultLightView);
        const float targetLightViewX = XMVectorGetX(targetLightView);
        const float targetLightViewY = XMVectorGetY(targetLightView);
        const float targetLightViewZ = XMVectorGetZ(targetLightView);

        const int64_t pageOffsetX = -static_cast<int64_t>(std::ceil(targetLightViewX / pageWorldSize));
        const int64_t pageOffsetY = -static_cast<int64_t>(std::ceil(-targetLightViewY / pageWorldSize));

        const XMVECTOR alignedTargetLightView = XMVectorSet(
            static_cast<float>(-pageOffsetX) * pageWorldSize,
            static_cast<float>(pageOffsetY) * pageWorldSize,
            targetLightViewZ,
            1.0f);
        const XMVECTOR alignedTargetWorld = XMVector3TransformCoord(alignedTargetLightView, defaultLightViewInverse);

        const float lightDistance = std::max(clipHeightOffsetScale * clipScale, farDistance * 0.5f);
        const XMVECTOR lightPos = alignedTargetWorld - normalizedLightDir * lightDistance;
        // Build every clip directly in the same light-space basis. Rebuilding
        // the view from a clip-specific world-space eye performs an
        // inverse-transform/LookTo round trip whose float cancellation leaves
        // slightly different X/Y translations at each level. Those residuals
        // move a projected point relative to the nested texel grids.
        XMMATRIX lightView = defaultLightView;
        lightView.r[3] = XMVectorSet(
            -XMVectorGetX(alignedTargetLightView),
            -XMVectorGetY(alignedTargetLightView),
            -XMVectorGetZ(alignedTargetLightView) - lightDistance,
            1.0f);
        Cascade clipmap;
        clipmap.size = clipScale * 2.0f;
        XMStoreFloat4(&clipmap.worldCenter, lightPos);
        clipmap.pageOffsetX = pageOffsetX;
        clipmap.pageOffsetY = pageOffsetY;
        clipmap.nearPlane = nearDistance;
        clipmap.farPlane = farDistance;
        clipmap.viewMatrix = lightView;
        clipmap.orthoMatrix = lightOrtho;

        const std::array<XMVECTOR, 6> viewSpacePlanes = {
            XMVectorSet(1.0f, 0.0f, 0.0f, clipScale),
            XMVectorSet(-1.0f, 0.0f, 0.0f, clipScale),
            XMVectorSet(0.0f, 1.0f, 0.0f, clipScale),
            XMVectorSet(0.0f, -1.0f, 0.0f, clipScale),
            XMVectorSet(0.0f, 0.0f, -1.0f, -nearDistance),
            XMVectorSet(0.0f, 0.0f, 1.0f, farDistance),
        };

        for (size_t planeIndex = 0; planeIndex < viewSpacePlanes.size(); ++planeIndex)
        {
            ClippingPlane plane{};
            XMStoreFloat4(&plane.plane, XMPlaneNormalize(viewSpacePlanes[planeIndex]));
            clipmap.frustumPlanes[planeIndex] = plane;
        }

        clipmaps.push_back(clipmap);
    }

    return clipmaps;
}

std::vector<float> calculateCascadeSplits(int numCascades, float zNear, float zFar, float maxDist, float lambda) {
    std::vector<float> splits(numCascades);
    float end = (std::min)(zFar, maxDist);
    float logNear = std::log(zNear);
    float logFar = std::log(end);
    float logRange = logFar - logNear;
    float uniformRange = end - zNear;

    for (int i = 0; i < numCascades; i++) {
        float p = (i + 1.0f) / numCascades;
        float logSplit = std::exp(logNear + logRange * p);
        float uniformSplit = zNear + uniformRange * p;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }

    return splits;
}

DXGI_FORMAT DetermineTextureFormat(int channels, bool sRGB, bool isDSV) {
    if (isDSV) {
        return DXGI_FORMAT_R32_TYPELESS;
    }

    switch (channels) {
    case 1:
        return DXGI_FORMAT_R8_UNORM;
    case 3:
    case 4:
        return sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        throw std::invalid_argument("Unsupported channel count");
    }
}

org::ShaderVisibleIndexInfo CreateShaderResourceView(
    rhi::Device& device,
    rhi::Resource& resource,
    rhi::Format format,
    org::DescriptorHeap* srvHeap,
    int mipLevels,
    bool isCubemap,
    bool isArray,
    int arraySize) {

	rhi::SrvDesc desc = {};
	desc.formatOverride = format;
    if (isCubemap) {
		desc.dimension = isArray ? rhi::SrvDim::TextureCubeArray : rhi::SrvDim::TextureCube;
        if (isArray) {
            desc.cubeArray.mipLevels = mipLevels;
            desc.cubeArray.numCubes = arraySize;
        }
        else {
            desc.cube.mipLevels = mipLevels;
		}
        }
	else {
        desc.dimension = isArray ? rhi::SrvDim::Texture2DArray : rhi::SrvDim::Texture2D;
        if (isArray) {
            desc.tex2DArray.mipLevels = mipLevels;
            desc.tex2DArray.arraySize = arraySize;
        }
        else {
            desc.tex2D.mipLevels = mipLevels;
		}
    }

    UINT descriptorIndex = srvHeap->AllocateDescriptor();

    device.CreateShaderResourceView({ srvHeap->GetHeap().GetHandle(), descriptorIndex}, resource.GetHandle(), desc);

    org::ShaderVisibleIndexInfo srvInfo;
    srvInfo.slot.index = descriptorIndex;
	srvInfo.slot.heap = srvHeap->GetHeap().GetHandle();

    return srvInfo;
}

std::vector<std::vector<org::ShaderVisibleIndexInfo>> CreateShaderResourceViewsPerMip(
    rhi::Device&     device,
    rhi::Resource&   resource,
    rhi::Format       format,
    org::DescriptorHeap*   srvHeap,
    int               mipLevels,
    bool              isCubemap,
    bool              isArray,
    int               arraySize)
{
    // If it's not an array, treat it as a single slice
    int sliceCount = isArray ? arraySize : 1;

    // Outer vector size == number of slices
    std::vector<std::vector<org::ShaderVisibleIndexInfo>> result(sliceCount);

    for (int slice = 0; slice < sliceCount; ++slice) {
        // Reserve inner vector for mipLevels entries
        auto& sliceSRVs = result[slice];
        sliceSRVs.reserve(mipLevels);

        for (int mip = 0; mip < mipLevels; ++mip) {
            rhi::SrvDesc srvDesc = {};
            srvDesc.formatOverride = format;
            if (isCubemap) {
                if (isArray) {
                    // One cubemap per slice
                    srvDesc.dimension = rhi::SrvDim::TextureCubeArray;
                    srvDesc.cubeArray.mostDetailedMip  = mip;
                    srvDesc.cubeArray.mipLevels        = mipLevels - mip;
                    srvDesc.cubeArray.first2DArrayFace = slice * 6;
                    srvDesc.cubeArray.numCubes         = arraySize - slice;
                } else {
                    // Single cubemap resource
                    srvDesc.dimension = rhi::SrvDim::TextureCube;
                    srvDesc.cube.mostDetailedMip = mip;
                    srvDesc.cube.mipLevels       = mipLevels - mip;
                }
            } else {
                if (isArray) {
                    // One 2D slice per array index
                    srvDesc.dimension = rhi::SrvDim::Texture2DArray;
                    srvDesc.tex2DArray.mostDetailedMip   = mip;
                    srvDesc.tex2DArray.mipLevels         = mipLevels - mip;
                    srvDesc.tex2DArray.firstArraySlice   = slice;
                    srvDesc.tex2DArray.arraySize         = arraySize - slice;
                    srvDesc.tex2DArray.planeSlice        = 0;
                } else {
                    // Plain 2D texture
                    srvDesc.dimension = rhi::SrvDim::Texture2D;
                    srvDesc.tex2D.mostDetailedMip = mip;
                    srvDesc.tex2D.mipLevels       = mipLevels - mip;
                    srvDesc.tex2D.planeSlice      = 0;
                }
            }

            // allocate one descriptor for this (slice, mip)
            unsigned descriptorIndex = srvHeap->AllocateDescriptor();

			device.CreateShaderResourceView({ srvHeap->GetHeap().GetHandle(), descriptorIndex}, resource.GetHandle(), srvDesc);

            org::ShaderVisibleIndexInfo srvInfo;
            srvInfo.slot.index = descriptorIndex;
			srvInfo.slot.heap = srvHeap->GetHeap().GetHandle();

            sliceSRVs.push_back(srvInfo);
        }
    }

    return result;
}
org::ShaderVisibleIndexInfo CreateUnorderedAccessView(
    rhi::Device& device,
    rhi::Resource& resource,
    rhi::Format format,
    org::DescriptorHeap* uavHeap,
    bool isArray,
    int arraySize,
    int mipSlice,
    int firstArraySlice,
    int planeSlice) {
    rhi::UavDesc uavDesc = {};
	uavDesc.formatOverride = format;
    // For now, only support Texture2D or Texture2DArray.
    // TODO: consolidate other uav creation into this?
    if (isArray) {
        uavDesc.dimension = rhi::UavDim::Texture2DArray;
        uavDesc.texture2DArray.mipSlice = mipSlice;
        uavDesc.texture2DArray.firstArraySlice = firstArraySlice;
        uavDesc.texture2DArray.arraySize = arraySize;
        uavDesc.texture2DArray.planeSlice = planeSlice;
    }
    else {
        uavDesc.dimension = rhi::UavDim::Texture2D;
        uavDesc.texture2D.mipSlice = mipSlice;
        uavDesc.texture2D.planeSlice = planeSlice;
    }

    UINT descriptorIndex = uavHeap->AllocateDescriptor();

	// No counter for texture UAVs
    device.CreateUnorderedAccessView({uavHeap->GetHeap().GetHandle(), descriptorIndex}, resource.GetHandle(), uavDesc);

    org::ShaderVisibleIndexInfo uavInfo;
    uavInfo.slot.index = descriptorIndex;
	uavInfo.slot.heap = uavHeap->GetHeap().GetHandle();

    return uavInfo;
}

org::NonShaderVisibleIndexInfo CreateNonShaderVisibleUnorderedAccessView( // Clear operations need a non-shader visible UAV
    rhi::Device& device,
    rhi::Resource& resource,
    rhi::Format format,
    org::DescriptorHeap* uavHeap,
    bool isArray,
    int arraySize,
    int mipSlice,
    int firstArraySlice,
    int planeSlice) {
    rhi::UavDesc uavDesc = {};
    uavDesc.formatOverride = format;
    if (isArray) {
        uavDesc.dimension = rhi::UavDim::Texture2DArray;
        uavDesc.texture2DArray.mipSlice = mipSlice;
        uavDesc.texture2DArray.firstArraySlice = firstArraySlice;
        uavDesc.texture2DArray.arraySize = arraySize;
        uavDesc.texture2DArray.planeSlice = planeSlice;
    }
    else {
        uavDesc.dimension = rhi::UavDim::Texture2D;
        uavDesc.texture2D.mipSlice = mipSlice;
        uavDesc.texture2D.planeSlice = planeSlice;
    }

    UINT descriptorIndex = uavHeap->AllocateDescriptor();

    // No counter for texture UAVs
	device.CreateUnorderedAccessView({ uavHeap->GetHeap().GetHandle(), descriptorIndex}, resource.GetHandle(), uavDesc);

    org::NonShaderVisibleIndexInfo uavInfo;
    uavInfo.slot.index = descriptorIndex;
	uavInfo.slot.heap = uavHeap->GetHeap().GetHandle();

    return uavInfo;
}

std::vector<std::vector<org::ShaderVisibleIndexInfo>> CreateUnorderedAccessViewsPerMip(
    rhi::Device& device,
    rhi::Resource& resource,
    rhi::Format      format,
    org::DescriptorHeap* uavHeap,
    int              mipLevels,
    bool             isArray,
    int              arraySize,
    int              planeSlice,
    bool             isCubemap)
{
    // If not an array, treat as a single slice
    const int sliceCount = isArray ? arraySize : 1;
    std::vector<std::vector<org::ShaderVisibleIndexInfo>> result(sliceCount);

    for (int slice = 0; slice < sliceCount; ++slice) {
        auto& sliceUAVs = result[slice];
        sliceUAVs.reserve(mipLevels);

        for (int mip = 0; mip < mipLevels; ++mip) {
            rhi::UavDesc uavDesc = {};
            uavDesc.formatOverride = format;

            if (isCubemap) {
                // Map cube/cube-array UAVs to 2D array views:
                // - 1 cube == 6 array slices
                // - For a cube array, slice N starts at firstArraySlice = N*6
                //   and (to mirror your SRV behavior) spans the remaining cubes.
                uavDesc.dimension = rhi::UavDim::Texture2DArray;
                uavDesc.texture2DArray.mipSlice = mip;
                uavDesc.texture2DArray.firstArraySlice = isArray ? (slice * 6) : 0;
                uavDesc.texture2DArray.arraySize = isArray ? ((arraySize - slice) * 6) : 6;
                uavDesc.texture2DArray.planeSlice = 0; // planar formats: usually 0 for cubemaps
            }
            else {
                if (isArray) {
                    uavDesc.dimension = rhi::UavDim::Texture2DArray;
                    uavDesc.texture2DArray.mipSlice = mip;
                    uavDesc.texture2DArray.firstArraySlice = slice;
                    uavDesc.texture2DArray.arraySize = arraySize - slice;
                    uavDesc.texture2DArray.planeSlice = planeSlice;
                }
                else {
                    uavDesc.dimension = rhi::UavDim::Texture2D;
                    uavDesc.texture2D.mipSlice = mip;
                    uavDesc.texture2D.planeSlice = planeSlice;
                }
            }

            const UINT descriptorIndex = uavHeap->AllocateDescriptor();
            device.CreateUnorderedAccessView({ uavHeap->GetHeap().GetHandle(), descriptorIndex }, resource.GetHandle(), uavDesc);

            org::ShaderVisibleIndexInfo uavInfo{ { uavHeap->GetHeap().GetHandle(), descriptorIndex } };
            sliceUAVs.push_back(uavInfo);
        }
    }

    return result;
}

std::vector<std::vector<org::NonShaderVisibleIndexInfo>> CreateNonShaderVisibleUnorderedAccessViewsPerMip(
    rhi::Device&      device,
    rhi::Resource&   resource,
    rhi::Format        format,
    org::DescriptorHeap*    uavHeap,
    int                mipLevels,
    bool               isArray,
    int                arraySize,
    int                planeSlice)
{
    // Determine how many "slices" we'll emit (1 if not an array)
    int sliceCount = isArray ? arraySize : 1;
    std::vector<std::vector<org::NonShaderVisibleIndexInfo>> result(sliceCount);

    for (int slice = 0; slice < sliceCount; ++slice) {
        auto& sliceUAVs = result[slice];
        sliceUAVs.reserve(mipLevels);

        for (int mip = 0; mip < mipLevels; ++mip) {
            rhi::UavDesc uavDesc = {};
            uavDesc.formatOverride = format;
            if (isArray) {
                uavDesc.dimension               = rhi::UavDim::Texture2DArray;
                uavDesc.texture2DArray.mipSlice     = mip;
                uavDesc.texture2DArray.firstArraySlice = slice;
                uavDesc.texture2DArray.arraySize    = 1;
                uavDesc.texture2DArray.planeSlice   = planeSlice;
            } else {
                uavDesc.dimension = rhi::UavDim::Texture2D;
                uavDesc.texture2D.mipSlice      = mip;
                uavDesc.texture2D.planeSlice    = planeSlice;
            }

            UINT    idx = uavHeap->AllocateDescriptor();

            // Create the UAV (no counter for texture UAVs)
			device.CreateUnorderedAccessView({ uavHeap->GetHeap().GetHandle(), idx}, resource.GetHandle(), uavDesc);

            org::NonShaderVisibleIndexInfo info;
            info.slot.index = idx;
			info.slot.heap = uavHeap->GetHeap().GetHandle();
            sliceUAVs.push_back(info);
        }
    }

    return result;
}

std::vector<std::vector<org::NonShaderVisibleIndexInfo>> CreateRenderTargetViews(
    rhi::Device&      device,
    rhi::Resource&    resource,
    rhi::Format        format,
    org::DescriptorHeap*    rtvHeap,
    bool               isCubemap,
    bool               isArray,
    int                arraySize,
    int                mipLevels)
{
    // Determine how many 2D slices we need:
    //   - for a cubemap: 6 faces � arraySize cubes
    //   - otherwise: arraySize slices (arraySize should be 1 if not an array)
    int sliceCount = isCubemap ? (6 * arraySize) : arraySize;

    // Prepare the outer vector: one entry per slice
    std::vector<std::vector<org::NonShaderVisibleIndexInfo>> result(sliceCount);

    // Common bits of the RTV description
    rhi::RtvDesc rtvDesc = {};
    rtvDesc.formatOverride        = format;
    rtvDesc.dimension = rhi::RtvDim::Texture2DArray;
	rtvDesc.range = { 0, 1, 0, 1 }; // Only one mip level and one array slice per RTV

    for ( int slice = 0; slice < sliceCount; ++slice) {
        auto& sliceRTVs = result[slice];
        sliceRTVs.reserve(mipLevels);

        for (int mip = 0; mip < mipLevels; ++mip) {
			rtvDesc.range = { static_cast<uint32_t>(mip), 1, static_cast<uint32_t>(slice), 1 };

            // Allocate one descriptor for this (slice, mip)
            UINT idx = rtvHeap->AllocateDescriptor();

            // Create the RTV
			device.CreateRenderTargetView({ rtvHeap->GetHeap().GetHandle(), idx}, resource.GetHandle(), rtvDesc);

            org::NonShaderVisibleIndexInfo info;
            info.slot.index = idx;
			info.slot.heap = rtvHeap->GetHeap().GetHandle();
            sliceRTVs.push_back(info);
        }
    }

    return result;
}

std::vector<std::vector<org::NonShaderVisibleIndexInfo>> CreateDepthStencilViews(
    rhi::Device&      device,
    rhi::Resource&    resource,
    org::DescriptorHeap*    dsvHeap,
    rhi::Format        format,
    bool               isCubemap,
    bool               isArray,
    int                arraySize,
    int                mipLevels)
{
    // 6 faces per cube, or just arraySize for non-cubemaps.
    int sliceCount = isCubemap ? (6 * arraySize) : arraySize;
    std::vector<std::vector<org::NonShaderVisibleIndexInfo>> result(sliceCount);

    // Base DSV descriptor
    rhi::DsvDesc dsvDesc = {};
    dsvDesc.formatOverride        = format;
    dsvDesc.dimension = rhi::DsvDim::Texture2DArray;
    //dsvDesc.Texture2DArray.ArraySize = 1;
	dsvDesc.range = { 0, 1, 0, 1 }; // One mip level, one array slice

    for (int slice = 0; slice < sliceCount; ++slice) {
        auto& sliceDSVs = result[slice];
        sliceDSVs.reserve(mipLevels);

        for (int mip = 0; mip < mipLevels; ++mip) {
			dsvDesc.range = { static_cast<uint32_t>(mip), 1, static_cast<uint32_t>(slice), 1 };

            // allocate and get CPU handle
            UINT idx = dsvHeap->AllocateDescriptor();

            // create the DSV
			device.CreateDepthStencilView({ dsvHeap->GetHeap().GetHandle(), idx}, resource.GetHandle(), dsvDesc);

            org::NonShaderVisibleIndexInfo info;
            info.slot.index = idx;
			info.slot.heap = dsvHeap->GetHeap().GetHandle();
            sliceDSVs.push_back(info);
        }
    }

    return result;
}

std::vector<stbi_uc> ExpandImageData(const stbi_uc* image, int width, int height) {
    std::vector<stbi_uc> expandedData(width * height * 4);
    for (int i = 0; i < width * height; ++i) {
        expandedData[i * 4] = image[i * 3];         // R
        expandedData[i * 4 + 1] = image[i * 3 + 1]; // G
        expandedData[i * 4 + 2] = image[i * 3 + 2]; // B
        expandedData[i * 4 + 3] = 255;              // A
    }
    return expandedData;
}

std::array<DirectX::XMMATRIX, 6> GetCubemapViewMatrices(XMFLOAT3 pos) {
    // Define directions and up vectors for the six faces of the cubemap
    // Directions for the cubemap faces
    XMVECTOR targets[6] = {
        XMVectorSet(1.0f,  0.0f,  0.0f, 0.0f), // +X
        XMVectorSet(-1.0f,  0.0f,  0.0f, 0.0f), // -X
        XMVectorSet(0.0f,  1.0f,  0.0f, 0.0f), // +Y
        XMVectorSet(0.0f, -1.0f,  0.0f, 0.0f), // -Y
        XMVectorSet(0.0f,  0.0f, -1.0f, 0.0f), // +Z
        XMVectorSet(0.0f,  0.0f, 1.0f, 0.0f), // -Z
    };

    // Up vectors for the cubemap faces
    XMVECTOR ups[6] = {
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f), // +X
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f), // -X
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), // +Y
        XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), // -Y
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f), // +Z
        XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f), // -Z
    };

    std::array<XMMATRIX, 6> viewMatrices{};
    XMVECTOR lightPos = XMLoadFloat3(&pos);

    for (int i = 0; i < 6; ++i) {
        viewMatrices[i] = XMMatrixLookToRH(
            lightPos,     // Eye position
            targets[i],   // Look direction
            ups[i]        // Up direction
        );
    }

    return viewMatrices;
}

std::string ToLower(const std::string& str) {
	std::string lower = str;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	return lower;
}

std::vector<std::string> GetFilesInDirectoryMatchingExtension(const std::wstring& directory, const std::wstring& extension) {
    std::vector<std::string> hdrFiles;

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == extension)
            {
                hdrFiles.push_back(entry.path().stem().string());
            }
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error(std::string("Error accessing directory: ") + e.what());
    }

    return hdrFiles;
}

bool OpenFileDialog(std::wstring& selectedFile, const std::wstring& filter) {
    wchar_t fileBuffer[MAX_PATH] = { 0 };

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter.c_str();  // Use the provided filter
    ofn.nFilterIndex = 1;  // Default to the first filter
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;  // Prevent directory change

    // Show the file dialog
    if (GetOpenFileNameW(&ofn) == TRUE) {
        selectedFile = fileBuffer;
        return true;  // File was selected
    }

    return false;  // Dialog was canceled or failed
}

void CopyFileToDirectory(const std::wstring& sourceFile, const std::wstring& destinationDirectory) {
    try
    {
        std::filesystem::path destinationPath = destinationDirectory;
        destinationPath /= std::filesystem::path(sourceFile).filename();

        // Copy the file to the destination
        std::filesystem::copy_file(sourceFile, destinationPath, std::filesystem::copy_options::overwrite_existing);

        std::ofstream fileStream(destinationPath, std::ios::out | std::ios::binary | std::ios::app);
        fileStream.flush();  // Flush the file stream to ensure the data is written
        fileStream.close();

        spdlog::info("File copied to: {}", ws2s(destinationPath.wstring()));
    }
    catch (const std::exception& e)
    {
        spdlog::error(std::string("Error copying file: ") + e.what());
    }
}

std::wstring GetExePath() {
    TCHAR buffer[MAX_PATH] = { 0 };
    GetModuleFileName(NULL, buffer, MAX_PATH);
    std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");
    return std::wstring(buffer).substr(0, pos);
}

std::wstring getFileNameFromPath(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    size_t fileNameStart = (lastSlash == std::wstring::npos) ? 0 : lastSlash + 1;

    size_t lastDot = path.find_last_of(L'.');
    if (lastDot == std::wstring::npos || lastDot < fileNameStart) {
        lastDot = path.length();
    }

    return path.substr(fileNameStart, lastDot - fileNameStart);
}

std::array<ClippingPlane, 6> GetFrustumPlanesPerspective(const float aspectRatio, const float fovRad, const float nearClip, const float farClip) {
    std::array<ClippingPlane, 6> planes = {};

    float tanHalfFOV = tan(fovRad / 2.0f);

    // Near and Far Planes (aligned with Z-axis)
    planes[0] = { DirectX::XMFLOAT4(0, 0, -1, -nearClip) }; // Near plane
    planes[1] = { DirectX::XMFLOAT4(0, 0, 1, farClip) };    // Far plane

    planes[2] = { DirectX::XMFLOAT4(1, 0, -tanHalfFOV * aspectRatio, 0) }; // Left plane
    planes[3] = { DirectX::XMFLOAT4( - 1, 0, -tanHalfFOV * aspectRatio, 0) }; // Right plane

    planes[4] = { DirectX::XMFLOAT4(0, 1, -tanHalfFOV, 0) }; // Bottom plane
    planes[5] = { DirectX::XMFLOAT4(0, -1, -tanHalfFOV, 0) }; // Top plane

    // Normalize the planes
    for (int i = 0; i < 6; ++i) {
        float A = planes[i].plane.x;
        float B = planes[i].plane.y;
        float C = planes[i].plane.z;
        float D = planes[i].plane.w;
        float length = sqrt(A * A + B * B + C * C);
        planes[i].plane.x = A / length;
        planes[i].plane.y = B / length;
        planes[i].plane.z = C / length;
        planes[i].plane.w = D / length;
    }

    return planes;
}

std::array<ClippingPlane, 6> GetFrustumPlanesOrthographic(const float left, const float right, const float top, const float bottom, const float nearClip, const float farClip, DirectX::XMFLOAT3 cameraPosWorld) {
    std::array<ClippingPlane, 6> planes = {};

	// Near and Far Planes (aligned with Z-axis, repositioned to camera space)
    planes[0] = { DirectX::XMFLOAT4(0, 0, -1, -nearClip) }; // Near plane
    planes[1] = { DirectX::XMFLOAT4(0, 0, 1, farClip) };    // Far plane

    planes[2] = { DirectX::XMFLOAT4(1, 0, 0, -left) }; // Left plane
    planes[3] = { DirectX::XMFLOAT4(-1, 0, 0, right) }; // Right plane

    planes[4] = { DirectX::XMFLOAT4(0, 1, 0, -bottom) }; // Bottom plane
    planes[5] = { DirectX::XMFLOAT4(0, -1, 0, top) }; // Top plane

    return planes;
}

DirectX::XMFLOAT3 Subtract(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
	return DirectX::XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

DirectX::XMFLOAT3 Add(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
	return DirectX::XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

DirectX::XMFLOAT3 Scale(const DirectX::XMFLOAT3& a, const float scale) {
	return DirectX::XMFLOAT3(a.x * scale, a.y * scale, a.z * scale);
}

XMFLOAT3X3 GetUpperLeft3x3(const XMMATRIX& matrix) {
    XMFLOAT3X3 result;

    // Extract the upper-left 3x3 part of the XMMATRIX
    result.m[0][0] = XMVectorGetX(matrix.r[0]); // Row 0, Col 0
    result.m[0][1] = XMVectorGetY(matrix.r[0]); // Row 0, Col 1
    result.m[0][2] = XMVectorGetZ(matrix.r[0]); // Row 0, Col 2

    result.m[1][0] = XMVectorGetX(matrix.r[1]); // Row 1, Col 0
    result.m[1][1] = XMVectorGetY(matrix.r[1]); // Row 1, Col 1
    result.m[1][2] = XMVectorGetZ(matrix.r[1]); // Row 1, Col 2

    result.m[2][0] = XMVectorGetX(matrix.r[2]); // Row 2, Col 0
    result.m[2][1] = XMVectorGetY(matrix.r[2]); // Row 2, Col 1
    result.m[2][2] = XMVectorGetZ(matrix.r[2]); // Row 2, Col 2

    return result;
}

std::string GetFileExtension(const std::string& filePath) {
    size_t dotPos = filePath.find_last_of('.');
    if (dotPos == std::string::npos || dotPos == filePath.length() - 1) {
        return ""; // No extension found or ends with a dot
    }
    return filePath.substr(dotPos + 1);
}

DirectX::XMMATRIX GetProjectionMatrixForLight(LightInfo info) {
    switch (info.type) {
    case Components::LightType::Spot:
        return XMMatrixPerspectiveFovRH(acos(info.outerConeAngle) * 2, 1.0, info.nearPlane, info.farPlane);
        break;
    case Components::LightType::Point:
        return XMMatrixPerspectiveFovRH(XM_PI / 2, 1.0, info.nearPlane, info.farPlane);
        break;
    case Components::LightType::Directional:
		throw std::runtime_error("Implemented elsewhere"); // TODO: Consolidate?
    default:
		throw std::runtime_error("Unknown light type for projection matrix");
    }
}

DirectX::XMVECTOR QuaternionFromAxisAngle(const XMFLOAT3& dir) {
    XMVECTOR targetDirection = XMVector3Normalize(XMLoadFloat3(&dir));
    float dotProduct = XMVectorGetX(XMVector3Dot(defaultDirection, targetDirection));
	DirectX::XMVECTOR rot;
    if (dotProduct < -0.9999f) {
        XMVECTOR perpendicularAxis = XMVector3Cross(defaultDirection, XMVectorSet(1, 0, 0, 0));
        if (XMVectorGetX(XMVector3Length(perpendicularAxis)) < 0.01f) {
            perpendicularAxis = XMVector3Cross(defaultDirection, XMVectorSet(0, 1, 0, 0));
        }
        perpendicularAxis = XMVector3Normalize(perpendicularAxis);
        rot = XMQuaternionRotationAxis(perpendicularAxis, XM_PI);
    }
    else if (dotProduct > 0.9999f) {
        rot = XMQuaternionIdentity();
    }
    else {
        XMVECTOR rotationAxis = XMVector3Normalize(XMVector3Cross(defaultDirection, targetDirection));
        float rotationAngle = acosf(dotProduct);
        rot = XMQuaternionRotationAxis(rotationAxis, rotationAngle);
    }
	return rot;
}

XMFLOAT3 GetGlobalPositionFromMatrix(const DirectX::XMMATRIX& mat) {
    XMFLOAT4X4 matFloats;
    XMStoreFloat4x4(&matFloats, mat);
    return XMFLOAT3(matFloats._41, matFloats._42, matFloats._43);
}

Components::DepthMap CreateDepthMapComponent(unsigned int xRes, unsigned int yRes, unsigned int arraySize, bool isCubemap) {
	org::TextureDescription desc;
	org::ImageDimensions dims;
	dims.width = xRes;
	dims.height = yRes;
	desc.imageDimensions.push_back(dims);
	desc.format = rhi::Format::R32_Typeless;
    desc.arraySize = arraySize;
	desc.isArray = arraySize > 1;
	desc.hasDSV = true;
	desc.hasSRV = true;
	desc.isCubemap = isCubemap;
	desc.channels = 1;
	desc.srvFormat = rhi::Format::R32_Float;
	desc.dsvFormat = rhi::Format::D32_Float;
    desc.generateMipMaps = false;

	std::shared_ptr<org::PixelBuffer> depthBuffer = org::PixelBuffer::CreateShared(desc);
	depthBuffer->SetName("Depth Buffer");
	org::memory::SetResourceUsageHint(*depthBuffer, "Depth resources");

    org::TextureDescription downsampledDesc;
    // Pad yres and xres to power of two
	dims.height = yRes;
	dims.width = xRes;
	downsampledDesc.imageDimensions.push_back(dims);
	downsampledDesc.format = rhi::Format::R32_Float;
	downsampledDesc.arraySize = arraySize;
	downsampledDesc.isArray = arraySize > 1;
	downsampledDesc.hasDSV = false;
	downsampledDesc.hasSRV = true;
	downsampledDesc.hasUAV = true;
	downsampledDesc.hasNonShaderVisibleUAV = true;
	downsampledDesc.isCubemap = isCubemap;
	downsampledDesc.channels = 1;
	downsampledDesc.srvFormat = rhi::Format::R32_Float;
	downsampledDesc.uavFormat = rhi::Format::R32_Float;
	downsampledDesc.generateMipMaps = true;
    downsampledDesc.hasRTV = true;
	downsampledDesc.rtvFormat = rhi::Format::R32_Float;
    downsampledDesc.clearColor[0] = std::numeric_limits<float>().max();
	downsampledDesc.padInternalResolution = true;

    std::shared_ptr<org::PixelBuffer> linearDepthBuffer = org::PixelBuffer::CreateShared(downsampledDesc);
    linearDepthBuffer->SetName("linear Depth Buffer");
	org::memory::SetResourceUsageHint(*linearDepthBuffer, "Depth resources");

	// Projected (non-linear) depth for upscalers — R32_Float with UAV+SRV, same resolution as depth buffer
	org::TextureDescription projectedDesc;
	org::ImageDimensions projDims;
	projDims.width = xRes;
	projDims.height = yRes;
	projectedDesc.imageDimensions.push_back(projDims);
	projectedDesc.format = rhi::Format::R32_Float;
	projectedDesc.arraySize = arraySize;
	projectedDesc.isArray = arraySize > 1;
	projectedDesc.hasDSV = false;
	projectedDesc.hasSRV = true;
	projectedDesc.hasUAV = true;
	projectedDesc.isCubemap = isCubemap;
	projectedDesc.channels = 1;
	projectedDesc.srvFormat = rhi::Format::R32_Float;
	projectedDesc.uavFormat = rhi::Format::R32_Float;
	projectedDesc.generateMipMaps = false;

	std::shared_ptr<org::PixelBuffer> projectedDepthBuffer = org::PixelBuffer::CreateShared(projectedDesc);
	projectedDepthBuffer->SetName("Projected Depth Buffer");
	org::memory::SetResourceUsageHint(*projectedDepthBuffer, "Depth resources");

	Components::DepthMap depthMap;
	depthMap.depthMap = depthBuffer;
	depthMap.linearDepthMap = linearDepthBuffer;
	depthMap.projectedDepthMap = projectedDepthBuffer;

	return depthMap;
}

uint32_t NumMips(uint32_t width, uint32_t height) {
    uint32_t maxSize = std::max(width, height);
    return 1 + static_cast<uint32_t>(std::floor(std::log2(float(maxSize))));
}

std::string GetDirectoryFromPath(const std::string& path) {
	size_t lastSlash = path.find_last_of("/\\");
	if (lastSlash == std::string::npos) {
		return ""; // No directory found
	}
	return path.substr(0, lastSlash);
}

std::shared_ptr<org::Buffer> CreateIndexedStructuredBuffer(size_t numElements, unsigned int elementSize, bool UAV, bool UAVCounter) {
    auto dataBuffer = org::Buffer::CreateUnmaterializedStructuredBuffer(
        static_cast<uint32_t>(numElements),
        static_cast<uint32_t>(elementSize),
        UAV,
        UAVCounter,
        false,
        rhi::HeapType::DeviceLocal);
    dataBuffer->Materialize();

    return dataBuffer;
}

std::shared_ptr<org::Buffer> CreateIndexedTypedBuffer(
    uint32_t        numElements,
    rhi::Format   elementFormat,
    bool          UAV)
{

    auto device = DeviceManager::GetInstance().GetDevice();

    const size_t elementSize = rhi::helpers::BytesPerBlock(elementFormat);
    assert(elementFormat != rhi::Format::Unknown && "Typed buffers require a concrete format");
    assert(elementSize > 0 && "Unsupported/invalid format for typed buffer");

    const size_t bufferSize = numElements * elementSize;

    auto dataBuffer = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, bufferSize, UAV);

    org::BufferBase::DescriptorRequirements descReq{};

    descReq.createCBV = false;
    descReq.createSRV = true;
    descReq.createUAV = UAV;
    descReq.createNonShaderVisibleUAV = UAV;
    descReq.uavCounterOffset = 0;                  // typed UAVs cannot have counters

    // SRV (typed)
    descReq.srvDesc = rhi::SrvDesc{
        .dimension = rhi::SrvDim::Buffer,
        .formatOverride = elementFormat, // required for typed SRV
        .buffer = {
            .kind = rhi::BufferViewKind::Typed,
            .firstElement = 0,
            .numElements = numElements,
            .structureByteStride = 0,    // ignored for typed
        },
    };

    // UAV (typed)
    descReq.uavDesc = rhi::UavDesc{
        .dimension = rhi::UavDim::Buffer,
        .formatOverride = elementFormat, // required for typed UAV
        .buffer = {
            .kind = rhi::BufferViewKind::Typed,
            .firstElement = 0,
            .numElements = numElements,
            .structureByteStride = 0,    // ignored for typed
            .counterOffsetInBytes = 0,   // no counter
        },
    };

    dataBuffer->SetDescriptorRequirements(descReq);
    dataBuffer->RefreshDescriptorContents();

    return dataBuffer;
}

std::shared_ptr<org::Buffer> CreateIndexedConstantBuffer(size_t bufferSize, std::string name) {
    auto device = DeviceManager::GetInstance().GetDevice();

    // Calculate the size of the buffer to be 256-byte aligned
    UINT paddedSize = (bufferSize + 255) & ~255;

    auto dataBuffer = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, paddedSize, false);
    dataBuffer->SetName(name);

    org::BufferBase::DescriptorRequirements descReq{};

    descReq.createCBV = true;

    descReq.cbvDesc = {
        .byteOffset = 0,
        .byteSize = paddedSize,
    };

    dataBuffer->SetDescriptorRequirements(descReq);
    dataBuffer->RefreshDescriptorContents();

    return dataBuffer;
}
