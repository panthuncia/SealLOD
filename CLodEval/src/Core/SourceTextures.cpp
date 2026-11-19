#include "Core/SourceTextures.h"

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>

#include <BasicRenderer/Assets/ClusterLODTextureDecode.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "include/ThirdParty/stb/stb_image.h"

namespace clodeval {
namespace {
	// "/glTF/Mesh/<m>/Primitive/<p>" with an optional suffix.
	bool ParsePrimitive(std::string_view name, size_t& mesh, size_t& primitive)
	{
		constexpr std::string_view meshTag = "/glTF/Mesh/", primTag = "/Primitive/";
		if (!name.starts_with(meshTag)) return false;
		name.remove_prefix(meshTag.size());
		const size_t split = name.find(primTag);
		if (split == std::string_view::npos) return false;
		const auto number = [](std::string_view digits, size_t& out) {
			size_t end = 0;
			while (end < digits.size() && digits[end] >= '0' && digits[end] <= '9') ++end;
			if (end == 0) return false;
			out = 0;
			for (size_t i = 0; i < end; ++i) out = out * 10u + static_cast<size_t>(digits[i] - '0');
			return true;
		};
		return number(name.substr(0, split), mesh) && number(name.substr(split + primTag.size()), primitive);
	}
}

std::shared_ptr<SourceMaterials> SourceMaterials::Open(const std::filesystem::path& path, std::string& outError)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		outError = std::format("cannot open '{}'", path.string());
		return nullptr;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const auto readU32 = [&](size_t offset) { uint32_t v = 0; std::memcpy(&v, bytes.data() + offset, 4); return v; };
	// A .glb: its JSON chunk and binary chunk. Anything else is read as a .gltf's JSON.
	size_t jsonOffset = 0, jsonLength = bytes.size();
	size_t binOffset = 0, binLength = 0;
	const bool binary = bytes.size() >= 4u && readU32(0) == 0x46546C67u;
	if (binary) {
		if (bytes.size() < 20u) {
			outError = std::format("'{}' is not a binary glTF", path.string());
			return nullptr;
		}
		jsonOffset = 20u;
		jsonLength = readU32(12);
		if (readU32(16) != 0x4E4F534Au || 20u + jsonLength > bytes.size()) {
			outError = std::format("'{}' has no JSON chunk", path.string());
			return nullptr;
		}
		const size_t binHeader = 20u + ((jsonLength + 3u) & ~size_t{ 3 });
		if (binHeader + 8u <= bytes.size() && readU32(binHeader + 4u) == 0x004E4942u) {
			binOffset = binHeader + 8u;
			binLength = std::min<size_t>(readU32(binHeader), bytes.size() - binOffset);
		}
	}
	auto result = std::shared_ptr<SourceMaterials>(new SourceMaterials());
	result->m_path = path;
	try {
		const nlohmann::json json = nlohmann::json::parse(bytes.begin() + static_cast<std::ptrdiff_t>(jsonOffset),
			bytes.begin() + static_cast<std::ptrdiff_t>(jsonOffset + jsonLength));
		const nlohmann::json empty = nlohmann::json::array();
		const nlohmann::json& textures = json.contains("textures") ? json["textures"] : empty;
		const nlohmann::json& images = json.contains("images") ? json["images"] : empty;
		const nlohmann::json& views = json.contains("bufferViews") ? json["bufferViews"] : empty;
		const nlohmann::json& buffers = json.contains("buffers") ? json["buffers"] : empty;
		std::vector<bool> usedImages(images.size(), false);
		for (const nlohmann::json& material : json.value("materials", empty)) {
			SourceMaterial parsed;
			if (const auto pbr = material.find("pbrMetallicRoughness"); pbr != material.end()) {
				if (const auto factor = pbr->find("baseColorFactor"); factor != pbr->end() && factor->size() == 4u) {
					for (size_t c = 0; c < 4u; ++c) parsed.baseColorFactor[c] = (*factor)[c].get<float>();
				}
				if (const auto texture = pbr->find("baseColorTexture"); texture != pbr->end()) {
					const size_t index = texture->value("index", size_t{ 0 });
					parsed.texCoord = texture->value("texCoord", 0u);
					if (index < textures.size()) {
						// KHR_texture_basisu's KTX2 image when there is one (as the renderer draws it), else the image.
						const nlohmann::json& entry = textures[index];
						const nlohmann::json* source = entry.contains("source") ? &entry["source"] : nullptr;
						if (entry.contains("extensions") && entry["extensions"].contains("KHR_texture_basisu") &&
							entry["extensions"]["KHR_texture_basisu"].contains("source")) {
							source = &entry["extensions"]["KHR_texture_basisu"]["source"];
						}
						const size_t image = source != nullptr ? source->get<size_t>() : images.size();
						if (image < images.size()) {
							parsed.baseColorImage = static_cast<int32_t>(image);
							usedImages[image] = true;
						}
					}
					if (texture->contains("extensions") && (*texture)["extensions"].contains("KHR_texture_transform")) {
						const nlohmann::json& transform = (*texture)["extensions"]["KHR_texture_transform"];
						double offset[2] = { 0.0, 0.0 }, scale[2] = { 1.0, 1.0 };
						if (transform.contains("offset") && transform["offset"].size() == 2u) {
							offset[0] = transform["offset"][0].get<double>();
							offset[1] = transform["offset"][1].get<double>();
						}
						if (transform.contains("scale") && transform["scale"].size() == 2u) {
							scale[0] = transform["scale"][0].get<double>();
							scale[1] = transform["scale"][1].get<double>();
						}
						const double rotation = transform.value("rotation", 0.0);
						const double c = std::cos(rotation), s = std::sin(rotation);
						parsed.uvTransformed = true;
						parsed.uvTransform = { c * scale[0], s * scale[1], offset[0], -s * scale[0], c * scale[1], offset[1] };
						parsed.uvFootprintScale = std::sqrt(std::fabs(scale[0] * scale[1]));
						parsed.texCoord = transform.value("texCoord", parsed.texCoord);
					}
				}
			}
			parsed.masked = material.value("alphaMode", std::string("OPAQUE")) == "MASK";
			parsed.alphaCutoff = material.value("alphaCutoff", 0.5f);
			parsed.doubleSided = material.value("doubleSided", false);
			result->m_materials.push_back(parsed);
		}
		for (const nlohmann::json& mesh : json.value("meshes", empty)) {
			std::vector<int32_t> primitives;
			for (const nlohmann::json& primitive : mesh.value("primitives", empty)) {
				primitives.push_back(primitive.contains("material") ? primitive["material"].get<int32_t>() : -1);
			}
			result->m_primitiveMaterials.push_back(std::move(primitives));
		}
		const std::filesystem::path directory = path.parent_path();
		const auto isDataUri = [](const std::string& uri) { return uri.rfind("data:", 0) == 0; };
		result->m_imageSources.resize(images.size());
		for (size_t image = 0; image < images.size(); ++image) {
			if (!usedImages[image]) {
				continue;
			}
			ImageSource& source = result->m_imageSources[image];
			if (images[image].contains("uri")) {
				// A file beside the asset (data URIs are not read).
				const std::string uri = images[image]["uri"].get<std::string>();
				if (!isDataUri(uri)) source.file = directory / std::filesystem::path(uri);
				continue;
			}
			if (!images[image].contains("bufferView")) continue;
			const size_t viewIndex = images[image]["bufferView"].get<size_t>();
			if (viewIndex >= views.size()) continue;
			const size_t bufferIndex = views[viewIndex].value("buffer", size_t{ 0 });
			const size_t offset = views[viewIndex].value("byteOffset", size_t{ 0 });
			const size_t length = views[viewIndex].value("byteLength", size_t{ 0 });
			if (bufferIndex < buffers.size() && buffers[bufferIndex].contains("uri")) {
				const std::string uri = buffers[bufferIndex]["uri"].get<std::string>();
				if (!isDataUri(uri) && length > 0u) {
					source.file = directory / std::filesystem::path(uri);
					source.offset = offset;
					source.length = length;
				}
				continue;
			}
			if (!binary || offset + length > binLength) continue;
			source.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(binOffset + offset),
				bytes.begin() + static_cast<std::ptrdiff_t>(binOffset + offset + length));
		}
	}
	catch (const std::exception& error) {
		outError = std::format("'{}': {}", path.string(), error.what());
		return nullptr;
	}
	result->m_images.resize(result->m_imageSources.size());
	result->m_imageOnce.reset(new std::once_flag[std::max<size_t>(result->m_imageSources.size(), 1u)]);
	return result;
}

const SourceMaterial* SourceMaterials::MaterialForMesh(std::string_view meshName) const
{
	size_t mesh = 0, primitive = 0;
	if (!ParsePrimitive(meshName, mesh, primitive) || mesh >= m_primitiveMaterials.size() || primitive >= m_primitiveMaterials[mesh].size()) {
		return nullptr;
	}
	const int32_t material = m_primitiveMaterials[mesh][primitive];
	return material >= 0 && static_cast<size_t>(material) < m_materials.size() ? &m_materials[static_cast<size_t>(material)] : nullptr;
}

const SourceTexture* SourceMaterials::Image(int32_t image) const
{
	if (image < 0 || static_cast<size_t>(image) >= m_imageSources.size()) {
		return nullptr;
	}
	std::call_once(m_imageOnce[static_cast<size_t>(image)], [&] {
		const ImageSource& source = m_imageSources[static_cast<size_t>(image)];
		std::vector<uint8_t> read;
		if (!source.file.empty()) {
			std::ifstream stream(source.file, std::ios::binary);
			if (!stream) return;
			uint64_t length = source.length;
			if (length == 0u) {
				std::error_code error;
				length = std::filesystem::file_size(source.file, error);
				if (error) return;
			}
			stream.seekg(static_cast<std::streamoff>(source.offset));
			read.resize(static_cast<size_t>(length));
			stream.read(reinterpret_cast<char*>(read.data()), static_cast<std::streamsize>(length));
			if (static_cast<uint64_t>(stream.gcount()) != length) return;
		}
		const std::vector<uint8_t>& encoded = source.file.empty() ? source.bytes : read;
		if (encoded.empty()) return;
		if (clod_texture::IsKtx2(encoded)) {
			std::shared_ptr<SourceTexture> decoded = clod_texture::DecodeKtx2(encoded);
			if (decoded != nullptr) m_images[static_cast<size_t>(image)] = std::make_unique<SourceTexture>(std::move(*decoded));
			return;
		}
		if (encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return;
		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 4);
		if (pixels == nullptr || w <= 0 || h <= 0) {
			stbi_image_free(pixels);
			return;
		}
		auto texture = std::make_unique<SourceTexture>();
		texture->width = static_cast<uint32_t>(w);
		texture->height = static_cast<uint32_t>(h);
		texture->mips.emplace_back(pixels, pixels + static_cast<size_t>(w) * h * 4u);
		stbi_image_free(pixels);
		clod_texture::BuildMips(*texture);
		m_images[static_cast<size_t>(image)] = std::move(texture);
	});
	return m_images[static_cast<size_t>(image)].get();
}

std::array<float, 4> BaseColor(const SourceMaterial& material, const SourceTexture* texture, double u, double v, double lod)
{
	std::array<float, 4> color = material.baseColorFactor;
	if (texture != nullptr && std::isfinite(u) && std::isfinite(v)) {
		if (material.uvTransformed) {
			const std::array<double, 6>& m = material.uvTransform;
			const double tu = m[0] * u + m[1] * v + m[2], tv = m[3] * u + m[4] * v + m[5];
			u = tu;
			v = tv;
		}
		const std::array<float, 4> sample = SampleLinear(*texture, u, v, lod);
		for (int c = 0; c < 4; ++c) color[c] *= sample[c];
	}
	return color;
}

}
