#include <BasicRenderer/Assets/TexturePipeline.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <rhi_dx12.h>
#include <rhi_helpers.h>

#include "Assets/Textures/Pipeline/TexturePipelineStages.h"
#include "Assets/Textures/Processing/TextureProcessingManager.h"

namespace br::textures
{
	namespace
	{
		constexpr std::size_t kDdsHeaderBytes = 4u + 124u;
		constexpr std::size_t kDdsDx10HeaderBytes = 20u;
		constexpr std::size_t kDdsFourCCOffset = 4u + 72u + 8u;
		constexpr std::uint32_t kDx10FourCC = std::uint32_t('D') | (std::uint32_t('X') << 8) | (std::uint32_t('1') << 16) | (std::uint32_t('0') << 24);

		std::uint32_t FullMipCount(std::uint32_t width, std::uint32_t height)
		{
			std::uint32_t count = 1;
			for (std::uint32_t size = (std::max)(width, height); size > 1u; size >>= 1u) ++count;
			return count;
		}

		rhi::Format WithSrgbPreference(rhi::Format format, bool preferSRGB)
		{
			const DXGI_FORMAT dxgi = rhi::ToDxgi(format);
			return rhi::helpers::ToRHI(preferSRGB ? DirectX::MakeSRGB(dxgi) : DirectX::MakeLinear(dxgi));
		}

		// A block-compressed DDS keeps its mips as the GPU takes them: per array item, every mip in turn, tightly packed. So
		// its shape comes from the header, and a mip window is one byte range of the file.
		class DdsTextureMipProvider final : public ITextureMipProvider
		{
		public:
			DdsTextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName)
				: _bytes(std::move(bytes)), _debugName(std::move(debugName)) {}

			std::optional<TextureShape> DescribeTexture() override
			{
				std::call_once(_describeOnce, [this] { Describe(); });
				return _layout ? std::optional<TextureShape>(_layout->shape) : std::nullopt;
			}

			std::shared_ptr<TextureSourceData> ReadTextureMips(std::uint32_t topMip) override
			{
				if (!DescribeTexture()) throw std::runtime_error("DDS texture '" + _debugName + "' has no readable header");
				const Layout& layout = *_layout;
				if (!layout.direct) {
					// Stored as something the GPU can't take (24-bit, luminance): DirectXTex decodes the whole file.
					const auto size = _bytes->Size();
					const auto file = _bytes->Read(0, size);
					auto decoded = LoadTextureSourceDataFromImageBytes(file.data(), file.size(), false, ".dds", _debugName);
					if (!decoded) throw std::runtime_error("DDS texture '" + _debugName + "' could not be decoded");
					return decoded;
				}

				const std::uint32_t mips = layout.mipCount;
				const std::uint32_t items = layout.itemCount;
				// One item streams from its top mip; arrays and cubes only ever load whole.
				const std::uint32_t first = items == 1u ? (std::min)(topMip, mips - 1u) : 0u;
				const std::uint64_t begin = layout.offsets[first];
				const std::uint64_t end = layout.dataEnd;
				const auto bytes = _bytes->Read(begin, end - begin);
				if (bytes.size() < end - begin) {
					throw std::runtime_error("DDS texture '" + _debugName + "' is shorter than its header says");
				}

				auto data = std::make_shared<TextureSourceData>();
				data->desc = layout.shape.desc;
				data->hasFullMipChain = layout.shape.hasFullMipChain;
				data->isBlockCompressed = true;
				data->subresources.resize(static_cast<std::size_t>(items) * mips);
				for (std::uint32_t item = 0; item < items; ++item) {
					for (std::uint32_t mip = item == 0u ? first : 0u; mip < mips; ++mip) {
						const std::size_t index = static_cast<std::size_t>(item) * mips + mip;
						const auto offset = static_cast<std::size_t>(layout.offsets[index] - begin);
						const auto length = static_cast<std::size_t>(layout.shape.desc.imageDimensions[index].slicePitch);
						data->subresources[index] = std::make_shared<std::vector<std::uint8_t>>(
							bytes.begin() + offset, bytes.begin() + offset + length);
					}
				}
				return data;
			}

		private:
			struct Layout
			{
				TextureShape shape;
				bool direct = false;  // the file's mips are the GPU's
				std::uint32_t mipCount = 0;
				std::uint32_t itemCount = 0;
				std::vector<std::uint64_t> offsets;  // each subresource's place in the file
				std::uint64_t dataEnd = 0;
			};

			void Describe()
			{
				try {
					const auto fileSize = _bytes->Size();
					const auto header = _bytes->Read(0, (std::min<std::uint64_t>)(fileSize, kDdsHeaderBytes + kDdsDx10HeaderBytes));
					DirectX::TexMetadata metadata{};
					if (header.size() < kDdsHeaderBytes ||
						FAILED(DirectX::GetMetadataFromDDSMemory(header.data(), header.size(), DirectX::DDS_FLAGS_NONE, metadata)) ||
						metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.depth != 1 || metadata.mipLevels == 0 ||
						metadata.arraySize == 0) {
						spdlog::warn("Provided texture '{}': not a readable 2D DDS", _debugName);
						return;
					}
					Layout layout;
					layout.mipCount = static_cast<std::uint32_t>(metadata.mipLevels);
					layout.itemCount = static_cast<std::uint32_t>(metadata.arraySize);
					// Only block-compressed payloads are certain to be stored as the GPU reads them; DirectXTex may convert
					// anything else.
					layout.direct = DirectX::IsCompressed(metadata.format);
					std::uint32_t fourCC = 0;
					std::memcpy(&fourCC, header.data() + kDdsFourCCOffset, sizeof(fourCC));
					std::uint64_t offset = kDdsHeaderBytes + (fourCC == kDx10FourCC ? kDdsDx10HeaderBytes : 0u);

					auto& desc = layout.shape.desc;
					for (std::size_t item = 0; item < metadata.arraySize; ++item) {
						std::size_t width = metadata.width;
						std::size_t height = metadata.height;
						for (std::size_t mip = 0; mip < metadata.mipLevels; ++mip) {
							std::size_t rowPitch = 0;
							std::size_t slicePitch = 0;
							if (FAILED(DirectX::ComputePitch(metadata.format, width, height, rowPitch, slicePitch))) {
								spdlog::warn("Provided texture '{}': cannot lay out mip {}", _debugName, mip);
								return;
							}
							org::ImageDimensions dims{};
							dims.width = static_cast<std::uint32_t>(width);
							dims.height = static_cast<std::uint32_t>(height);
							dims.rowPitch = rowPitch;
							dims.slicePitch = slicePitch;
							desc.imageDimensions.push_back(dims);
							layout.offsets.push_back(offset);
							offset += slicePitch;
							width = (std::max)(std::size_t(1), width / 2u);
							height = (std::max)(std::size_t(1), height / 2u);
						}
					}
					layout.dataEnd = offset;
					if (layout.direct && offset > fileSize) {
						spdlog::warn("Provided texture '{}': {} bytes, its header needs {}", _debugName, fileSize, offset);
						return;
					}
					desc.format = rhi::helpers::ToRHI(metadata.format);
					desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(desc.format));
					desc.isCubemap = metadata.IsCubemap();
					desc.isArray = metadata.arraySize > 1 && !desc.isCubemap;
					desc.arraySize = desc.isCubemap
						? static_cast<std::uint32_t>((std::max)(std::size_t(1), metadata.arraySize / std::size_t(6)))
						: static_cast<std::uint32_t>(metadata.arraySize);
					layout.shape.isBlockCompressed = layout.direct;
					layout.shape.hasFullMipChain = metadata.mipLevels == FullMipCount(
						static_cast<std::uint32_t>(metadata.width), static_cast<std::uint32_t>(metadata.height));
					_layout = std::move(layout);
				}
				catch (const std::exception& error) {
					spdlog::warn("Provided texture '{}': header unreadable: {}", _debugName, error.what());
				}
			}

			std::shared_ptr<ITextureByteSource> _bytes;
			std::string _debugName;
			std::once_flag _describeOnce;
			std::optional<Layout> _layout;
		};
	}

	std::shared_ptr<ITextureMipProvider> MakeDdsTextureMipProvider(std::shared_ptr<ITextureByteSource> bytes, std::string debugName)
	{
		if (!bytes) throw std::invalid_argument("MakeDdsTextureMipProvider: no byte source");
		return std::make_shared<DdsTextureMipProvider>(std::move(bytes), std::move(debugName));
	}

	std::shared_ptr<ITextureMipProvider> ProvidedTextureMipProvider(const TextureRequest& request)
	{
		const auto* provided = std::get_if<ProvidedTextureSource>(&request.source.data);
		return provided ? provided->provider : nullptr;
	}

	std::optional<TextureShape> ProvidedPassthroughShape(const TextureRequest& request)
	{
		const auto provider = ProvidedTextureMipProvider(request);
		if (!provider || request.preparation != TexturePreparation::Processed) return std::nullopt;
		auto shape = provider->DescribeTexture();
		if (!shape || shape->desc.imageDimensions.empty()) return std::nullopt;
		// The processing decision reads only the shape: mips, compression, the request's conversions.
		TextureSourceData stub;
		stub.desc = shape->desc;
		stub.hasFullMipChain = shape->hasFullMipChain;
		stub.isBlockCompressed = shape->isBlockCompressed;
		stub.subresources.resize(shape->desc.imageDimensions.size());
		auto meta = TextureProcessingMeta(request);
		// Block-compressed chains commonly stop at the block (Skyrim's at 2x2 or 4x4). The levels below it are one block
		// each and sampling clamps to the smallest level there is, so a chain that reaches a block is complete: generating
		// the rest would decompress and re-encode every level of the texture.
		const std::uint32_t faces = shape->desc.isCubemap ? 6u : 1u;
		const std::size_t items = static_cast<std::size_t>(faces) * (std::max)(1u, shape->desc.arraySize);
		const std::size_t mipCount = shape->desc.imageDimensions.size() / (std::max)(std::size_t(1), items);
		if (shape->isBlockCompressed && !shape->hasFullMipChain && mipCount > 1u && meta.processing.maxMipLevels == 0u) {
			const auto& smallest = shape->desc.imageDimensions[mipCount - 1u];
			if ((std::min)(smallest.width, smallest.height) <= 4u) {
				meta.processing.maxMipLevels = static_cast<std::uint32_t>(mipCount);
			}
		}
		// A request that keeps its texture uncompressed (height) would decompress a block-compressed source, which only
		// makes it larger: its blocks already are the data.
		if (shape->isBlockCompressed) {
			meta.processing.requestBlockCompression = true;
		}
		if (TextureProcessingManager::GetInstance().NeedsProcessing(stub, meta)) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1, std::memory_order_relaxed) < 64u) {
				const auto& top = shape->desc.imageDimensions.front();
				spdlog::info("Provided texture '{}' needs processing: {}x{} format={} mips={} fullChain={} bc={} semantic={} wantMips={} wantBc={} maxMips={} normalConvention={}",
					request.source.identity, top.width, top.height, static_cast<std::uint32_t>(shape->desc.format),
					shape->desc.imageDimensions.size(), shape->hasFullMipChain, shape->isBlockCompressed,
					static_cast<std::uint32_t>(meta.processing.semantic), meta.processing.requestMipChain,
					meta.processing.requestBlockCompression, meta.processing.maxMipLevels,
					static_cast<std::uint32_t>(meta.processing.normalConvention));
			}
			return std::nullopt;
		}
		shape->desc.format = WithSrgbPreference(shape->desc.format, request.preferSRGB);
		return shape;
	}

	bool IsProvidedPassthrough(const TextureRequest& request)
	{
		return ProvidedPassthroughShape(request).has_value();
	}

	std::shared_ptr<TextureSourceData> ReadProvidedTextureMips(const TextureRequest& request, std::uint32_t topMip)
	{
		const auto provider = ProvidedTextureMipProvider(request);
		if (!provider) throw std::runtime_error("texture source '" + request.source.identity + "' has no provider");
		auto data = provider->ReadTextureMips(topMip);
		if (!data || data->desc.imageDimensions.empty() || data->subresources.size() != data->desc.imageDimensions.size()) {
			throw std::runtime_error("texture provider for '" + request.source.identity + "' returned no texture");
		}
		const std::uint32_t faces = data->desc.isCubemap ? 6u : 1u;
		const std::size_t items = static_cast<std::size_t>(faces) * (std::max)(1u, data->desc.arraySize);
		const std::size_t mips = data->subresources.size() / (std::max)(std::size_t(1), items);
		const std::size_t first = items == 1u ? (std::min<std::size_t>)(topMip, mips - 1u) : 0u;
		for (std::size_t index = 0; index < data->subresources.size(); ++index) {
			if (index % mips >= first && !data->subresources[index]) {
				throw std::runtime_error("texture provider for '" + request.source.identity + "' left out a requested mip");
			}
		}
		data->desc.format = WithSrgbPreference(data->desc.format, request.preferSRGB);
		return data;
	}
}
