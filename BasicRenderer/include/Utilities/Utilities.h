#pragma once

#include <cstdint>

#include <windows.h>
#include <iostream>
#include <ThirdParty/stb/stb_image.h>
#include <array>
#include <memory>
#include <DirectXMath.h>
#include <unordered_map>
#include <DirectXTex.h>

#include "Utilities/CachePathUtilities.h"
#include "Import/MeshData.h"
#include "Mesh/Mesh.h"
#include "Render/DescriptorHeap.h"
#include "Resources/HeapIndexInfo.h"
#include "ShaderBuffers.h"
#include "Scene/Components.h"
#include "Resources/TextureDescription.h"

#ifndef NDEBUG
#define DEBUG_ONLY(x)   x
#else
//#define DEBUG_ONLY(x)   x
#define DEBUG_ONLY(x)   ((void)0)
#endif

class DescriptorHeap;
class Sampler;
class TextureAsset;
struct TextureFileMeta;
class Buffer;

void ThrowIfFailed(HRESULT hr);

template<typename... Args>
void print(Args... args) {
	(std::cout << ... << args) << std::endl;
}

std::shared_ptr<Mesh> MeshFromData(MeshData&& meshData, std::wstring name, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD = std::nullopt);

DirectX::XMMATRIX RemoveScalingFromMatrix(const DirectX::XMMATRIX& initialMatrix);

struct LoadFlags {
	DirectX::DDS_FLAGS dds = DirectX::DDS_FLAGS_NONE;
	DirectX::TGA_FLAGS tga = DirectX::TGA_FLAGS_NONE;
	DirectX::WIC_FLAGS wic = DirectX::WIC_FLAGS_IGNORE_SRGB;
	// HDR has no flags
};

std::shared_ptr<TextureAsset> LoadTextureFromFile(
	const std::wstring& filePath,
	std::shared_ptr<Sampler> sampler = nullptr,
	bool preferSRGB = false,
	const LoadFlags& flags = {}, bool allowRTV = false, bool allowUAV = false);

std::shared_ptr<TextureAsset> LoadTextureFromFileDeferred(
	const std::wstring& filePath,
	std::shared_ptr<Sampler> sampler = nullptr,
	bool preferSRGB = false,
	const TextureFileMeta* metaOverride = nullptr,
	bool allowRTV = false,
	bool allowUAV = false);

std::shared_ptr<TextureAsset> LoadTextureFromMemory(
	const void* bytes,
	size_t byteCount,
	std::shared_ptr<Sampler> sampler = nullptr,
	const LoadFlags& flags = {},
	bool preferSRGB = false, bool allowRTV = false , bool allowUAV = false);
std::shared_ptr<TextureAsset> LoadCubemapFromFile(const char* topPath, const char* bottomPath, const char* leftPath, const char* rightPath, const char* frontPath, const char* backPath);
std::shared_ptr<TextureAsset> LoadCubemapFromFile(std::wstring ddsFilePath, bool allowRTV = false, bool allowUAV = false);

#if __has_include(<bit>) && (__cpp_lib_bit_cast >= 201806L)
#include <bit>
inline uint32_t as_uint(float f) noexcept { return std::bit_cast<uint32_t>(f); }
inline float    as_float(uint32_t u) noexcept { return std::bit_cast<float>(u); }
#else
inline uint32_t as_uint(float f) noexcept {
	static_assert(sizeof(uint32_t) == sizeof(float), "sizes must match");
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));  // strict-aliasing safe
	return u;
}
inline float as_float(uint32_t u) noexcept {
	static_assert(sizeof(uint32_t) == sizeof(float), "sizes must match");
	float f;
	std::memcpy(&f, &u, sizeof(f));  // strict-aliasing safe
	return f;
}
#endif

template <typename T1, typename T2>
bool mapHasKeyNotAsValue(std::unordered_map<T1, T2>& map, T1 key, T2 val) {
	return map.contains(key) && map[key] != val;
}

template <typename T1, typename T2>
bool mapHasKeyAsValue(std::unordered_map<T1, T2>& map, T1 key, T2 val) {
	return map.contains(key) && map[key] == val;
}

template <typename T1, typename T2>
void CombineMaps(std::unordered_map<T1, T2>& dest, const std::unordered_map<T1, T2>& src) {
	for (const auto& [key, val] : src) {
		dest[key] = val;
	}
}

struct Cascade {
	float size;
	DirectX::XMFLOAT4 worldCenter;
	int64_t pageOffsetX = 0;
	int64_t pageOffsetY = 0;
	float nearPlane = 0.0f;
	float farPlane = 0.0f;
	DirectX::XMMATRIX orthoMatrix;
	DirectX::XMMATRIX viewMatrix;
	std::array<ClippingPlane, 6> frustumPlanes;
};

DirectX::XMMATRIX createDirectionalLightViewMatrix(DirectX::XMVECTOR lightDir, DirectX::XMVECTOR center);

std::vector<Cascade> setupCascades(int numCascades, const DirectX::XMVECTOR& lightDir, const DirectX::XMVECTOR& camPos, const DirectX::XMVECTOR& camDir, const DirectX::XMVECTOR& camUp, float nearPlane, float fovY, float aspectRatio, const std::vector<float>& cascadeSplits);

std::vector<Cascade> setupDirectionalClipmaps(int numClipmaps, const DirectX::XMVECTOR& lightDir, const DirectX::XMVECTOR& camPos, const DirectX::XMVECTOR& camDir, const DirectX::XMVECTOR& camUp, float nearPlane, float fovY, float aspectRatio, const std::vector<float>& clipFarPlanes, float shadowDistanceLowerBound, float clipSceneExtent, float resolutionScale = 1.0f);

std::vector<float> calculateCascadeSplits(int numCascades, float zNear, float zFar, float maxDist, float lambda = 0.8f);

DXGI_FORMAT DetermineTextureFormat(int channels, bool sRGB, bool isDSV);

std::vector<stbi_uc> ExpandImageData(const stbi_uc* image, int width, int height);

// Helper functions for creating resources

std::vector<std::vector<ShaderVisibleIndexInfo>> CreateShaderResourceViewsPerMip(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format       format,
	DescriptorHeap* srvHeap,
	int               mipLevels,
	bool              isCubemap,
	bool              isArray,
	int               arraySize);

ShaderVisibleIndexInfo CreateUnorderedAccessView(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format format,
	DescriptorHeap* uavHeap,
	bool isArray,
	int arraySize,
	int mipSlice = 0,
	int firstArraySlice = 0,
	int planeSlice = 0);

NonShaderVisibleIndexInfo CreateNonShaderVisibleUnorderedAccessView(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format format,
	DescriptorHeap* uavHeap,
	bool isArray,
	int arraySize,
	int mipSlice = 0,
	int firstArraySlice = 0,
	int planeSlice = 0);

std::vector<std::vector<ShaderVisibleIndexInfo>> CreateUnorderedAccessViewsPerMip(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format format,
	DescriptorHeap* uavHeap,
	int               mipLevels,
	bool              isArray,
	int               arraySize,
	int               planeSlice,
	bool              isCubemap);

std::vector<std::vector<NonShaderVisibleIndexInfo>> CreateNonShaderVisibleUnorderedAccessViewsPerMip(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format format,
	DescriptorHeap* uavHeap,
	int                mipLevels,
	bool               isArray,
	int                arraySize,
	int                planeSlice);

std::vector<std::vector<NonShaderVisibleIndexInfo>> CreateRenderTargetViews(
	rhi::Device& device,
	rhi::Resource& resource,
	rhi::Format        format,
	DescriptorHeap* rtvHea,
	bool               isCubemap,
	bool               isArray,
	int                arraySize = 1,
	int                mipLevels = 1);

std::vector<std::vector<NonShaderVisibleIndexInfo>> CreateDepthStencilViews(
	rhi::Device& device,
	rhi::Resource& resource,
	DescriptorHeap* dsvHeap,
	rhi::Format        format,
	bool               isCubemap = false,
	bool               isArray = false,
	int                arraySize = 1,
	int                mipLevels = 1);

//void UploadTextureData(rhi::Resource& dstTexture, const TextureDescription& desc, const std::vector<const stbi_uc*>& initialData, unsigned int arraySize, unsigned int mipLevels);

std::array<DirectX::XMMATRIX, 6> GetCubemapViewMatrices(DirectX::XMFLOAT3 pos);

inline uint16_t CalculateMipLevels(uint16_t width, uint16_t height) {
	return static_cast<uint16_t>(std::floor(std::log2((std::max)(width, height)))) + 1;
}

std::string ToLower(const std::string& str);

std::vector<std::string> GetFilesInDirectoryMatchingExtension(const std::wstring& directory, const std::wstring& extension);

bool OpenFileDialog(std::wstring& selectedFile, const std::wstring& filter);

void CopyFileToDirectory(const std::wstring& sourceFile, const std::wstring& destinationDirectory);

std::wstring GetExePath();

std::wstring getFileNameFromPath(const std::wstring& path);

std::array<ClippingPlane, 6> GetFrustumPlanesPerspective(const float aspectRatio, const float fovRad, const float nearClip, const float farClip);

std::array<ClippingPlane, 6> GetFrustumPlanesOrthographic(const float left, const float right, const float top, const float bottom, const float nearClip, const float farClip, DirectX::XMFLOAT3 cameraPosWorld);

DirectX::XMFLOAT3 Subtract(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

DirectX::XMFLOAT3 Add(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

DirectX::XMFLOAT3 Scale(const DirectX::XMFLOAT3& a, const float scalar);

DirectX::XMFLOAT3X3 GetUpperLeft3x3(const DirectX::XMMATRIX& matrix);

template <class T>
inline void hash_combine(std::size_t& s, const T& v) {
	std::hash<T> h;
	s ^= h(v) + 0x9e3779b9 + (s << 6) + (s >> 2);
}

std::string GetFileExtension(const std::string& filePath);

DirectX::XMMATRIX GetProjectionMatrixForLight(LightInfo info);

DirectX::XMVECTOR QuaternionFromAxisAngle(const DirectX::XMFLOAT3& dir);

DirectX::XMFLOAT3 GetGlobalPositionFromMatrix(const DirectX::XMMATRIX& mat);

Components::DepthMap CreateDepthMapComponent(unsigned int xRes, unsigned int yRes, unsigned int arraySize, bool isCubemap);

uint32_t NumMips(uint32_t width, uint32_t height);

std::string GetDirectoryFromPath(const std::string& path);

constexpr UINT CalcSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept {
	return MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize;
}

std::shared_ptr<Buffer> CreateIndexedStructuredBuffer(size_t numElements, unsigned int elementSize, bool UAV = false, bool UAVCounter = false);

std::shared_ptr<Buffer> CreateIndexedTypedBuffer(
	uint32_t        numElements,
	rhi::Format   elementFormat,
	bool          UAV = false);


std::shared_ptr<Buffer> CreateIndexedConstantBuffer(size_t bufferSize, std::string name = "");
