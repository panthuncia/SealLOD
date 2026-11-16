#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "Resources/PixelBuffer.h"
#include "Resources/TextureDescription.h"
#include "Resources/Buffers/Buffer.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/stochasticLightingRootConstants.h"

namespace br::render {

// What stochastic lighting's passes share across frames (docs/stochastic_lighting.md). Pairs are ping-ponged by the frame
// number's parity: frame n writes [n & 1] and reads [(n & 1) ^ 1]. Made at graph build, at render resolution.
struct StochasticLightingResources {
	uint32_t width = 0u;
	uint32_t height = 0u;
	// 1, or 2 for one sampled pixel per 2x2 block (docs/stochastic_lighting.md "Reduced resolution"). The estimator's thread
	// groups and visible lists then cover 16x16 pixels, and the noisy inputs hold one texel per block.
	uint32_t downsample = 1u;
	// Per thread group's tile (8x8 sampled pixels), a count and room for STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY entries (guiding).
	std::array<std::shared_ptr<org::Buffer>, 2> visibleLists{};
	// Per pixel, R32G32_UInt: linear depth bits and an octahedral normal. Written by the estimator, read by guiding and the
	// denoiser to validate reprojection.
	std::array<std::shared_ptr<org::PixelBuffer>, 2> surfaceHistory{};
	// The denoiser's. Null without it. Noisy: the estimator's raw diffuse and specular radiance. History: demodulated
	// radiance (rgb) and, in each one's alpha, its history length; moments: diffuse and specular luminance first and second
	// moments. Change: the slow and fast averages of the 5x5 neighbourhood's compressed luminance mean, diffuse then specular,
	// for change-aware rectification. Variance: RG16F, this frame's diffuse and specular luminance variance for the spatial filter.
	std::shared_ptr<org::PixelBuffer> noisyDiffuse, noisySpecular, variance;
	std::array<std::shared_ptr<org::PixelBuffer>, 2> historyDiffuse{}, historySpecular{}, historyMoments{}, historyChange{};
	// With the denoiser: the guide (docs/stochastic_lighting.md "Guide-normalized accumulation"), R32G32_Float at render resolution, the
	// unshadowed diffuse and specular luminance in the denoiser's demodulated domain. Written by StochasticGuidePass, read by
	// shading and both denoiser passes while stochasticLightingNormalize is on.
	std::shared_ptr<org::PixelBuffer> guide;
	// With the denoiser: light-mixture confidence (docs/stochastic_lighting.md "Guide-normalized accumulation"), R32G32_UInt at render
	// resolution, 8 byte shares each. The signature: this frame's mixture of lights in the unshadowed light, written by
	// StochasticGuidePass; the mixture history: the mixture the denoiser's history was made under, by parity, written and read
	// by the temporal pass. Used while stochasticLightingMixtureConfidence is on (with stochasticLightingNormalize). With
	// STOCHASTIC_SIGNATURE_LOWRES (the default) one texel per 4x4 block: the shaders only ever touch that corner, so the
	// textures are made that size (6 MB instead of 98 MB at 2560x1600; "Speed options (2026-11-17)").
	std::shared_ptr<org::PixelBuffer> signature;
	std::array<std::shared_ptr<org::PixelBuffer>, 2> mixtureHistory{};
	// The estimator's (docs/stochastic_lighting.md "Kernel split"; null for the reference). Per
	// sampled pixel: slotCapacity sample slots (uint3) and a pixel word; the compacted ray list (uint3, one per slot at most);
	// the ray counter (appended, then the count the trace reads) and the trace's indirect arguments. All but the counter live
	// within a frame, so the graph may alias them.
	uint32_t slotCapacity = 0u;
	uint32_t rayCapacity = 0u;
	std::shared_ptr<org::Buffer> slots, pixelWords, rays, rayCounter, rayArgs;
	// The estimator's with the denoiser: visibility gradients (docs/stochastic_lighting.md "Visibility gradients"), uints: per cell of STOCHASTIC_GRADIENT_MIN_CELL^2 sampled pixels, two records (by parity), the raw and two
	// held gradients (stochasticLightingRootConstants.h, StochasticGradientCapacity). Kept across frames, so not aliased. Used
	// while stochasticLightingVisibilityGradient is on (with stochasticLightingNormalize).
	std::shared_ptr<org::Buffer> gradient;

	bool Denoised() const { return noisyDiffuse != nullptr; }

	// The guide's graph name, for capture.hdr's stochastic_guide target (registered by StochasticLightingExtension).
	static constexpr const char* kGuideResourceName = "Builtin::StochasticLighting::Guide";

	static std::shared_ptr<org::PixelBuffer> MakeTexture(uint32_t width, uint32_t height, rhi::Format format, uint32_t channels, const char* name) {
		org::TextureDescription description;
		description.channels = channels;
		description.format = format;
		description.hasSRV = description.hasUAV = true;
		description.imageDimensions.emplace_back(width, height, 0, 0);
		auto texture = org::PixelBuffer::CreateSharedUnmaterialized(description);
		texture->SetName(name);
		return texture;
	}

	// slotCapacity: 0 without the split estimator, else the most samples a sampled pixel takes (4 with reduced resolution).
	static std::shared_ptr<StochasticLightingResources> Create(uint32_t width, uint32_t height, bool denoise, uint32_t downsample,
		uint32_t slotCapacity = 0u) {
		auto resources = std::make_shared<StochasticLightingResources>();
		resources->width = width;
		resources->height = height;
		resources->downsample = downsample;
		const uint32_t tilePixels = STOCHASTIC_TILE_SIZE * downsample;
		const uint64_t tiles = uint64_t((width + tilePixels - 1u) / tilePixels) * ((height + tilePixels - 1u) / tilePixels);
		const uint32_t sampledWidth = (width + downsample - 1u) / downsample;
		const uint32_t sampledHeight = (height + downsample - 1u) / downsample;
#if STOCHASTIC_SIGNATURE_LOWRES
		const uint32_t mixtureWidth = (width + STOCHASTIC_SIGNATURE_BLOCK - 1u) / STOCHASTIC_SIGNATURE_BLOCK;
		const uint32_t mixtureHeight = (height + STOCHASTIC_SIGNATURE_BLOCK - 1u) / STOCHASTIC_SIGNATURE_BLOCK;
#else
		const uint32_t mixtureWidth = width;
		const uint32_t mixtureHeight = height;
#endif
		for (uint32_t index = 0; index < 2u; ++index) {
			const bool a = index == 0u;
			resources->visibleLists[index] = CreateIndexedStructuredBuffer(tiles * STOCHASTIC_VISIBLE_LIST_STRIDE, sizeof(uint32_t), true);
			resources->visibleLists[index]->SetName(a ? "Stochastic visible lists A" : "Stochastic visible lists B");
			resources->surfaceHistory[index] = MakeTexture(width, height, rhi::Format::R32G32_UInt, 2,
				a ? "Stochastic surface history A" : "Stochastic surface history B");
			if (denoise) {
				resources->historyDiffuse[index] = MakeTexture(width, height, rhi::Format::R16G16B16A16_Float, 4,
					a ? "Stochastic diffuse history A" : "Stochastic diffuse history B");
				resources->historySpecular[index] = MakeTexture(width, height, rhi::Format::R16G16B16A16_Float, 4,
					a ? "Stochastic specular history A" : "Stochastic specular history B");
				resources->historyMoments[index] = MakeTexture(width, height, rhi::Format::R16G16B16A16_Float, 4,
					a ? "Stochastic moments history A" : "Stochastic moments history B");
				resources->historyChange[index] = MakeTexture(width, height, rhi::Format::R16G16B16A16_Float, 4,
					a ? "Stochastic change history A" : "Stochastic change history B");
				resources->mixtureHistory[index] = MakeTexture(mixtureWidth, mixtureHeight, rhi::Format::R32G32_UInt, 2,
					a ? "Stochastic mixture history A" : "Stochastic mixture history B");
			}
		}
		if (denoise) {
			resources->noisyDiffuse = MakeTexture(sampledWidth, sampledHeight, rhi::Format::R16G16B16A16_Float, 4, "Stochastic noisy diffuse");
			resources->noisySpecular = MakeTexture(sampledWidth, sampledHeight, rhi::Format::R16G16B16A16_Float, 4, "Stochastic noisy specular");
			resources->variance = MakeTexture(width, height, rhi::Format::R16G16_Float, 2, "Stochastic variance");
			resources->guide = MakeTexture(width, height, rhi::Format::R32G32_Float, 2, "Stochastic guide");
			resources->signature = MakeTexture(mixtureWidth, mixtureHeight, rhi::Format::R32G32_UInt, 2, "Stochastic mixture signature");
		}
		if (slotCapacity > 0u) {
			const auto makeBuffer = [](uint32_t elements, uint32_t stride, const char* name) {
				auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(elements, stride, true, false, false, rhi::HeapType::DeviceLocal);
				buffer->SetAllowAlias(true);
				buffer->SetName(name);
				return buffer;
			};
			const uint32_t sampled = sampledWidth * sampledHeight;
			resources->slotCapacity = slotCapacity;
			resources->rayCapacity = sampled * slotCapacity;
			resources->slots = makeBuffer(sampled * slotCapacity, 3u * sizeof(uint32_t), "Stochastic sample slots");
			resources->pixelWords = makeBuffer(sampled, sizeof(uint32_t), "Stochastic pixel words");
			resources->rays = makeBuffer(sampled * slotCapacity, 3u * sizeof(uint32_t), "Stochastic shadow rays");
			// Cleared by an upload every frame before selection, so it is a resource of its own.
			resources->rayCounter = CreateIndexedStructuredBuffer(2u, sizeof(uint32_t), true);
			resources->rayCounter->SetName("Stochastic ray counter");
			resources->rayArgs = makeBuffer(3u, sizeof(uint32_t), "Stochastic trace arguments");
			if (denoise) {
				const uint64_t cells = uint64_t((sampledWidth + STOCHASTIC_GRADIENT_MIN_CELL - 1u) / STOCHASTIC_GRADIENT_MIN_CELL)
					* ((sampledHeight + STOCHASTIC_GRADIENT_MIN_CELL - 1u) / STOCHASTIC_GRADIENT_MIN_CELL);
				resources->gradient = CreateIndexedStructuredBuffer(cells * STOCHASTIC_GRADIENT_WORDS_PER_CELL, sizeof(uint32_t), true);
				resources->gradient->SetName("Stochastic visibility gradients");
			}
		}
		return resources;
	}
};

} // namespace br::render
