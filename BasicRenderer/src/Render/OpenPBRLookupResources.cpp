#include "Render/OpenPBRLookupResources.h"

#include <DirectXPackedVector.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Factories/TextureFactory.h"
#include "Resources/PixelBuffer.h"

#include "../../../openpbr-bsdf/openpbr_data_constants.h"

namespace {

using OpenPBREnergyElement = uint16_t;

constexpr uint32_t kOpenPBREnergyTableSize = OpenPBR_EnergyTableSize;
constexpr uint32_t kOpenPBRLTCTableSize = OpenPBR_LTCTableSize;

constexpr size_t kEnergy2DEntryCount =
    static_cast<size_t>(kOpenPBREnergyTableSize) * static_cast<size_t>(kOpenPBREnergyTableSize);
constexpr size_t kEnergy3DEntryCount =
    kEnergy2DEntryCount * static_cast<size_t>(kOpenPBREnergyTableSize);
constexpr size_t kEnergy1DEntryCount = static_cast<size_t>(kOpenPBREnergyTableSize);
constexpr size_t kLTCEntryCount =
    static_cast<size_t>(kOpenPBRLTCTableSize) * static_cast<size_t>(kOpenPBRLTCTableSize);

const std::array<OpenPBREnergyElement, kEnergy3DEntryCount> kIdealDielectricEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ideal_dielectric_energy_complement_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy2DEntryCount> kIdealDielectricAverageEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ideal_dielectric_avg_energy_complement_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy2DEntryCount> kIdealDielectricReflectionRatio = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ideal_dielectric_reflection_ratio_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy3DEntryCount> kOpaqueDielectricEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_opaque_dielectric_energy_complement_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy2DEntryCount> kOpaqueDielectricAverageEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_opaque_dielectric_avg_energy_complement_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy2DEntryCount> kIdealMetalEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ideal_metal_energy_complement_data.h"
};

const std::array<OpenPBREnergyElement, kEnergy1DEntryCount> kIdealMetalAverageEnergyComplement = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ideal_metal_avg_energy_complement_data.h"
};

struct OpenPBRLTCEntry {
    float aInv;
    float bInv;
    float reflectance;

    constexpr OpenPBRLTCEntry(double inAInv, double inBInv, double inReflectance)
        : aInv(static_cast<float>(inAInv))
        , bInv(static_cast<float>(inBInv))
        , reflectance(static_cast<float>(inReflectance)) {
    }
};

#define vec3 OpenPBRLTCEntry
const std::array<OpenPBRLTCEntry, kLTCEntryCount> kFuzzLTC = {
#include "../../../openpbr-bsdf/impl/data/openpbr_ltc_data.h"
};
#undef vec3

ImageDimensions MakeImageDimensions(uint32_t width, uint32_t height, uint64_t bytesPerPixel)
{
    ImageDimensions dims{};
    dims.width = width;
    dims.height = height;
    dims.rowPitch = static_cast<uint64_t>(width) * bytesPerPixel;
    dims.slicePitch = dims.rowPitch * height;
    return dims;
}

template <size_t Count>
std::shared_ptr<std::vector<uint8_t>> MakeScalarBytes(
    const std::array<OpenPBREnergyElement, Count>& source,
    size_t offset,
    size_t valueCount)
{
    auto bytes = std::make_shared<std::vector<uint8_t>>(valueCount * sizeof(OpenPBREnergyElement));
    std::memcpy(bytes->data(), source.data() + offset, bytes->size());
    return bytes;
}

template <size_t Count>
std::shared_ptr<PixelBuffer> CreateScalarTableTexture2D(
    const TextureFactory& textureFactory,
    const std::array<OpenPBREnergyElement, Count>& data,
    uint32_t width,
    uint32_t height,
    std::string_view debugName)
{
    TextureDescription desc{};
    desc.channels = 1;
    desc.format = rhi::Format::R16_UNorm;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R16_UNorm;
    desc.imageDimensions.push_back(MakeImageDimensions(width, height, sizeof(OpenPBREnergyElement)));

    TextureFactory::TextureInitialData initialData;
    initialData.subresources.push_back(MakeScalarBytes(data, 0, static_cast<size_t>(width) * height));

    auto texture = textureFactory.CreateAlwaysResidentPixelBuffer(desc, std::move(initialData), debugName);
    org::memory::SetResourceUsageHint(*texture, "OpenPBR Lookup Resources");
    return texture;
}

template <size_t Count>
std::shared_ptr<PixelBuffer> CreateScalarTableTexture2DArray(
    const TextureFactory& textureFactory,
    const std::array<OpenPBREnergyElement, Count>& data,
    uint32_t width,
    uint32_t height,
    uint32_t slices,
    std::string_view debugName)
{
    const size_t sliceValueCount = static_cast<size_t>(width) * height;

    TextureDescription desc{};
    desc.channels = 1;
    desc.format = rhi::Format::R16_UNorm;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R16_UNorm;
    desc.isArray = true;
    desc.arraySize = slices;
    desc.imageDimensions.reserve(slices);
    for (uint32_t slice = 0; slice < slices; ++slice) {
        desc.imageDimensions.push_back(MakeImageDimensions(width, height, sizeof(OpenPBREnergyElement)));
    }

    TextureFactory::TextureInitialData initialData;
    initialData.subresources.reserve(slices);
    for (uint32_t slice = 0; slice < slices; ++slice) {
        initialData.subresources.push_back(MakeScalarBytes(data, static_cast<size_t>(slice) * sliceValueCount, sliceValueCount));
    }

    auto texture = textureFactory.CreateAlwaysResidentPixelBuffer(desc, std::move(initialData), debugName);
    org::memory::SetResourceUsageHint(*texture, "OpenPBR Lookup Resources");
    return texture;
}

std::shared_ptr<PixelBuffer> CreateFuzzLTCTableTexture(
    const TextureFactory& textureFactory,
    std::string_view debugName)
{
    std::vector<uint16_t> halfData(kLTCEntryCount * 4u, 0u);
    for (size_t index = 0; index < kLTCEntryCount; ++index) {
        const OpenPBRLTCEntry& entry = kFuzzLTC[index];
        halfData[index * 4u + 0u] = DirectX::PackedVector::XMConvertFloatToHalf(entry.aInv);
        halfData[index * 4u + 1u] = DirectX::PackedVector::XMConvertFloatToHalf(entry.bInv);
        halfData[index * 4u + 2u] = DirectX::PackedVector::XMConvertFloatToHalf(entry.reflectance);
        halfData[index * 4u + 3u] = 0u;
    }

    auto bytes = std::make_shared<std::vector<uint8_t>>(halfData.size() * sizeof(uint16_t));
    std::memcpy(bytes->data(), halfData.data(), bytes->size());

    TextureDescription desc{};
    desc.channels = 4;
    desc.format = rhi::Format::R16G16B16A16_Float;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R16G16B16A16_Float;
    desc.imageDimensions.push_back(MakeImageDimensions(kOpenPBRLTCTableSize, kOpenPBRLTCTableSize, sizeof(uint16_t) * 4u));

    TextureFactory::TextureInitialData initialData;
    initialData.subresources.push_back(bytes);

    auto texture = textureFactory.CreateAlwaysResidentPixelBuffer(desc, std::move(initialData), debugName);
    org::memory::SetResourceUsageHint(*texture, "OpenPBR Lookup Resources");
    return texture;
}

} // namespace

bool OpenPBRLookupResources::HasAny() const
{
    return idealDielectricEnergyComplement ||
        idealDielectricAverageEnergyComplement ||
        idealDielectricReflectionRatio ||
        opaqueDielectricEnergyComplement ||
        opaqueDielectricAverageEnergyComplement ||
        idealMetalEnergyComplement ||
        idealMetalAverageEnergyComplement ||
        fuzzLTC;
}

OpenPBRLookupResources CreateOpenPBRLookupResources(const TextureFactory& textureFactory)
{
    OpenPBRLookupResources resources;
    resources.idealDielectricEnergyComplement = CreateScalarTableTexture2DArray(
        textureFactory,
        kIdealDielectricEnergyComplement,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Ideal Dielectric Energy Complement");
    resources.idealDielectricAverageEnergyComplement = CreateScalarTableTexture2D(
        textureFactory,
        kIdealDielectricAverageEnergyComplement,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Ideal Dielectric Average Energy Complement");
    resources.idealDielectricReflectionRatio = CreateScalarTableTexture2D(
        textureFactory,
        kIdealDielectricReflectionRatio,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Ideal Dielectric Reflection Ratio");
    resources.opaqueDielectricEnergyComplement = CreateScalarTableTexture2DArray(
        textureFactory,
        kOpaqueDielectricEnergyComplement,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Opaque Dielectric Energy Complement");
    resources.opaqueDielectricAverageEnergyComplement = CreateScalarTableTexture2D(
        textureFactory,
        kOpaqueDielectricAverageEnergyComplement,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Opaque Dielectric Average Energy Complement");
    resources.idealMetalEnergyComplement = CreateScalarTableTexture2D(
        textureFactory,
        kIdealMetalEnergyComplement,
        kOpenPBREnergyTableSize,
        kOpenPBREnergyTableSize,
        "OpenPBR Ideal Metal Energy Complement");
    resources.idealMetalAverageEnergyComplement = CreateScalarTableTexture2D(
        textureFactory,
        kIdealMetalAverageEnergyComplement,
        kOpenPBREnergyTableSize,
        1u,
        "OpenPBR Ideal Metal Average Energy Complement");
    resources.fuzzLTC = CreateFuzzLTCTableTexture(textureFactory, "OpenPBR Fuzz LTC");
    return resources;
}
