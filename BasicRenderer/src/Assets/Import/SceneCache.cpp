#include <BasicRenderer/Assets/Import/SceneCache.h>

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace br::import
{
	namespace
	{
		using json = nlohmann::json;

		json Floats(const float* values, size_t count)
		{
			json array = json::array();
			for (size_t i = 0; i < count; ++i) array.push_back(values[i]);
			return array;
		}

		void ReadFloats(const json& array, float* values, size_t count, const char* what)
		{
			if (!array.is_array() || array.size() != count) throw std::runtime_error(std::string("scene cache: '") + what + "' has the wrong size");
			for (size_t i = 0; i < count; ++i) values[i] = array[i].get<float>();
		}

		json Matrix(const DirectX::XMFLOAT4X4& matrix) { return Floats(&matrix.m[0][0], 16); }
		DirectX::XMFLOAT4X4 ReadMatrix(const json& array)
		{
			DirectX::XMFLOAT4X4 matrix{};
			ReadFloats(array, &matrix.m[0][0], 16, "matrix");
			return matrix;
		}

		json Slot(const TextureAndConstant& slot, std::int32_t texture)
		{
			json out = json::object();
			if (texture >= 0) out["texture"] = texture;
			if (slot.factor.HasValue()) out["factor"] = slot.factor.Get();
			out["channels"] = slot.channels;
			if (slot.uvSetIndex != 0) out["uvSet"] = slot.uvSetIndex;
			if (!slot.uvSetName.empty()) out["uvSetName"] = slot.uvSetName;
			if (!slot.sourcePath.empty()) out["sourcePath"] = slot.sourcePath;
			return out;
		}

		void ReadSlot(const json& in, TextureAndConstant& slot, std::int32_t& texture)
		{
			texture = in.value("texture", -1);
			if (in.contains("factor")) slot.factor = in["factor"].get<float>();
			slot.channels = in.value("channels", std::vector<std::uint32_t>{});
			slot.uvSetIndex = in.value("uvSet", 0u);
			slot.uvSetName = in.value("uvSetName", std::string());
			slot.sourcePath = in.value("sourcePath", std::string());
		}

		TextureAndConstant& SlotOf(MaterialDescription& desc, size_t slot)
		{
			switch (static_cast<SceneCacheTextureSlot>(slot)) {
			case SceneCacheTextureSlot::BaseColor: return desc.baseColor;
			case SceneCacheTextureSlot::Metallic: return desc.metallic;
			case SceneCacheTextureSlot::Roughness: return desc.roughness;
			case SceneCacheTextureSlot::Emissive: return desc.emissive;
			case SceneCacheTextureSlot::Opacity: return desc.opacity;
			case SceneCacheTextureSlot::AO: return desc.aoMap;
			case SceneCacheTextureSlot::Height: return desc.heightMap;
			default: return desc.normal;
			}
		}

		const TextureAndConstant& SlotOf(const MaterialDescription& desc, size_t slot)
		{
			return SlotOf(const_cast<MaterialDescription&>(desc), slot);
		}

		constexpr const char* kSlotNames[] = { "baseColor", "metallic", "roughness", "emissive", "opacity", "ao", "height", "normal" };
		static_assert(std::size(kSlotNames) == static_cast<size_t>(SceneCacheTextureSlot::Count));

		json Material(const SceneCacheMaterial& material)
		{
			const MaterialDescription& d = material.description;
			if (d.materialModel != MaterialModel::LegacyPreviewSurface) {
				throw std::runtime_error("scene cache: material '" + d.name + "' is OpenPBR, which the scene cache cannot hold");
			}
			json out = json::object();
			out["source"] = material.sourceMaterial;
			out["name"] = d.name;
			out["sourceMaterialIdentity"] = d.sourceMaterialIdentity;
			out["semanticFamily"] = d.semanticFamily;
			out["surfaceFlags"] = d.surfaceFlags;
			out["diagnosticReason"] = d.diagnosticReason;
			out["resolvedSpecularF0"] = Floats(&d.resolvedSpecularF0.x, 3);
			out["legacySpecularColor"] = Floats(&d.legacySpecularColor.x, 3);
			out["legacySpecularStrength"] = d.legacySpecularStrength;
			out["sourceGlossiness"] = d.sourceGlossiness;
			out["glossToRoughnessConvention"] = d.glossToRoughnessConvention;
			out["diffuseColor"] = Floats(&d.diffuseColor.x, 4);
			out["emissiveColor"] = Floats(&d.emissiveColor.x, 4);
			out["alphaCutoff"] = d.alphaCutoff;
			out["heightMapScale"] = d.heightMapScale;
			out["geometricDisplacement"] = { d.geometricDisplacementMin, d.geometricDisplacementMax };
			out["flags"] = {
				{ "negateNormals", d.negateNormals },
				{ "invertNormalGreen", d.invertNormalGreen },
				{ "forceDoubleSided", d.forceDoubleSided },
				{ "enableGeometricDisplacement", d.enableGeometricDisplacement },
				{ "geometricDisplacementOptIn", d.geometricDisplacementOptIn },
				{ "brniflyVertexAlpha", d.brniflyVertexAlpha },
				{ "brniflyZBufferWrite", d.brniflyZBufferWrite },
				{ "brniflyDecal", d.brniflyDecal },
				{ "brniflyDynamicDecal", d.brniflyDynamicDecal },
				{ "brniflyModelSpaceNormals", d.brniflyModelSpaceNormals },
				{ "heightMapFromBaseColorAlpha", d.heightMapFromBaseColorAlpha },
				{ "objectSurfaceUseTriplanarProjection", d.objectSurfaceUseTriplanarProjection },
				{ "objectSurfaceUseTripleTapStochastic", d.objectSurfaceUseTripleTapStochastic },
				{ "forceVoxelMaterial", d.forceVoxelMaterial },
				{ "glintEnabled", d.glintEnabled },
			};
			out["objectSurfaceSamplingMode"] = static_cast<std::uint32_t>(d.objectSurfaceSamplingMode);
			out["objectSurfaceTexelDensity"] = d.objectSurfaceTexelDensity;
			out["staticTextureOverrideSourceName"] = d.staticTextureOverrideSourceName;
			out["blendState"] = static_cast<std::uint32_t>(d.blendState);
			out["glintParameters"] = Floats(&d.glintParameters.x, 4);
			out["uvTransform"] = { d.uvTransform.offset.x, d.uvTransform.offset.y, d.uvTransform.scale.x, d.uvTransform.scale.y, d.uvTransform.rotation };
			json slots = json::object();
			for (size_t slot = 0; slot < material.textures.size(); ++slot) {
				slots[kSlotNames[slot]] = Slot(SlotOf(d, slot), material.textures[slot]);
			}
			out["slots"] = std::move(slots);
			return out;
		}

		SceneCacheMaterial ReadMaterial(const json& in)
		{
			SceneCacheMaterial material;
			MaterialDescription& d = material.description;
			material.sourceMaterial = in.at("source").get<std::uint32_t>();
			d.name = in.value("name", std::string());
			d.sourceMaterialIdentity = in.value("sourceMaterialIdentity", std::uint64_t{ 0 });
			d.semanticFamily = in.value("semanticFamily", 0u);
			d.surfaceFlags = in.value("surfaceFlags", 0u);
			d.diagnosticReason = in.value("diagnosticReason", 0u);
			ReadFloats(in.at("resolvedSpecularF0"), &d.resolvedSpecularF0.x, 3, "resolvedSpecularF0");
			ReadFloats(in.at("legacySpecularColor"), &d.legacySpecularColor.x, 3, "legacySpecularColor");
			d.legacySpecularStrength = in.at("legacySpecularStrength").get<float>();
			d.sourceGlossiness = in.at("sourceGlossiness").get<float>();
			d.glossToRoughnessConvention = in.value("glossToRoughnessConvention", 0u);
			ReadFloats(in.at("diffuseColor"), &d.diffuseColor.x, 4, "diffuseColor");
			ReadFloats(in.at("emissiveColor"), &d.emissiveColor.x, 4, "emissiveColor");
			d.alphaCutoff = in.at("alphaCutoff").get<float>();
			d.heightMapScale = in.at("heightMapScale").get<float>();
			d.geometricDisplacementMin = in.at("geometricDisplacement").at(0).get<float>();
			d.geometricDisplacementMax = in.at("geometricDisplacement").at(1).get<float>();
			const json& flags = in.at("flags");
			d.negateNormals = flags.value("negateNormals", d.negateNormals);
			d.invertNormalGreen = flags.value("invertNormalGreen", d.invertNormalGreen);
			d.forceDoubleSided = flags.value("forceDoubleSided", d.forceDoubleSided);
			d.enableGeometricDisplacement = flags.value("enableGeometricDisplacement", d.enableGeometricDisplacement);
			d.geometricDisplacementOptIn = flags.value("geometricDisplacementOptIn", d.geometricDisplacementOptIn);
			d.brniflyVertexAlpha = flags.value("brniflyVertexAlpha", d.brniflyVertexAlpha);
			d.brniflyZBufferWrite = flags.value("brniflyZBufferWrite", d.brniflyZBufferWrite);
			d.brniflyDecal = flags.value("brniflyDecal", d.brniflyDecal);
			d.brniflyDynamicDecal = flags.value("brniflyDynamicDecal", d.brniflyDynamicDecal);
			d.brniflyModelSpaceNormals = flags.value("brniflyModelSpaceNormals", d.brniflyModelSpaceNormals);
			d.heightMapFromBaseColorAlpha = flags.value("heightMapFromBaseColorAlpha", d.heightMapFromBaseColorAlpha);
			d.objectSurfaceUseTriplanarProjection = flags.value("objectSurfaceUseTriplanarProjection", d.objectSurfaceUseTriplanarProjection);
			d.objectSurfaceUseTripleTapStochastic = flags.value("objectSurfaceUseTripleTapStochastic", d.objectSurfaceUseTripleTapStochastic);
			d.forceVoxelMaterial = flags.value("forceVoxelMaterial", d.forceVoxelMaterial);
			d.glintEnabled = flags.value("glintEnabled", d.glintEnabled);
			d.objectSurfaceSamplingMode = static_cast<ObjectSurfaceSamplingMode>(in.value("objectSurfaceSamplingMode", 0u));
			d.objectSurfaceTexelDensity = in.value("objectSurfaceTexelDensity", d.objectSurfaceTexelDensity);
			d.staticTextureOverrideSourceName = in.value("staticTextureOverrideSourceName", std::string());
			d.blendState = static_cast<BlendState>(in.value("blendState", 0u));
			ReadFloats(in.at("glintParameters"), &d.glintParameters.x, 4, "glintParameters");
			float uv[5];
			ReadFloats(in.at("uvTransform"), uv, 5, "uvTransform");
			d.uvTransform.offset = { uv[0], uv[1] };
			d.uvTransform.scale = { uv[2], uv[3] };
			d.uvTransform.rotation = uv[4];
			const json& slots = in.at("slots");
			for (size_t slot = 0; slot < material.textures.size(); ++slot) {
				ReadSlot(slots.at(kSlotNames[slot]), SlotOf(d, slot), material.textures[slot]);
			}
			return material;
		}

		constexpr const char* kImageKinds[] = { "file", "fileRange", "embedded" };
	}

	nlohmann::json SceneCacheToJson(const SceneCacheDescription& scene)
	{
		json out = json::object();
		out["sourceIdentifier"] = scene.sourceIdentifier;
		if (!scene.notCacheableReason.empty()) out["notCacheableReason"] = scene.notCacheableReason;
		json images = json::array();
		for (const auto& image : scene.images) {
			json row = { { "kind", kImageKinds[static_cast<size_t>(image.kind)] }, { "sourceImage", image.sourceImage } };
			if (image.kind != SceneCacheImage::Kind::Embedded) row["file"] = image.file;
			if (image.kind == SceneCacheImage::Kind::FileRange) {
				row["offset"] = image.offset;
				row["length"] = image.length;
				row["bufferView"] = image.bufferView;
				row["mimeType"] = image.mimeType;
			}
			if (image.ktx2) row["ktx2"] = true;
			images.push_back(std::move(row));
		}
		out["images"] = std::move(images);
		json textures = json::array();
		for (const auto& texture : scene.textures) {
			textures.push_back({
				{ "image", texture.image },
				{ "sampler", { texture.wrapS, texture.wrapT, texture.minFilter, texture.magFilter } },
				{ "srgb", texture.preferSRGB },
				{ "semantic", static_cast<std::uint32_t>(texture.semantic) },
				{ "packed", texture.preservePackedChannels },
				{ "normalConvention", static_cast<std::uint32_t>(texture.normalConvention) },
			});
		}
		out["textures"] = std::move(textures);
		json materials = json::array();
		for (const auto& material : scene.materials) materials.push_back(Material(material));
		out["materials"] = std::move(materials);
		json primitives = json::array();
		for (const auto& primitive : scene.primitives) {
			primitives.push_back({
				{ "mesh", primitive.mesh },
				{ "primitive", primitive.primitive },
				{ "primPath", primitive.primPath },
				{ "doubleSidedVoxelSourceNormals", primitive.doubleSidedVoxelSourceNormals },
				{ "vertexFlags", primitive.vertexFlags },
				{ "vertexSize", primitive.vertexSize },
				{ "skinningVertexSize", primitive.skinningVertexSize },
				{ "uvSets", primitive.uvSetCount },
				{ "material", primitive.material },
			});
		}
		out["primitives"] = std::move(primitives);
		json parts = json::array();
		for (const auto& part : scene.parts) {
			json row = { { "name", part.name }, { "matrix", Matrix(part.localMatrix) }, { "primitives", part.primitives } };
			if (!part.instanceMatrices.empty()) {
				json instances = json::array();
				for (const auto& matrix : part.instanceMatrices) instances.push_back(Matrix(matrix));
				row["instances"] = std::move(instances);
			}
			parts.push_back(std::move(row));
		}
		out["parts"] = std::move(parts);
		return out;
	}

	SceneCacheDescription SceneCacheFromJson(const nlohmann::json& in)
	{
		try {
			SceneCacheDescription scene;
			scene.sourceIdentifier = in.at("sourceIdentifier").get<std::string>();
			scene.notCacheableReason = in.value("notCacheableReason", std::string());
			for (const auto& row : in.at("images")) {
				SceneCacheImage image;
				const std::string kind = row.at("kind").get<std::string>();
				if (kind == "file") image.kind = SceneCacheImage::Kind::File;
				else if (kind == "fileRange") image.kind = SceneCacheImage::Kind::FileRange;
				else if (kind == "embedded") image.kind = SceneCacheImage::Kind::Embedded;
				else throw std::runtime_error("unknown image kind '" + kind + "'");
				image.sourceImage = row.value("sourceImage", 0u);
				image.file = row.value("file", std::string());
				image.offset = row.value("offset", std::uint64_t{ 0 });
				image.length = row.value("length", std::uint64_t{ 0 });
				image.bufferView = row.value("bufferView", 0u);
				image.mimeType = row.value("mimeType", std::string());
				image.ktx2 = row.value("ktx2", false);
				scene.images.push_back(std::move(image));
			}
			for (const auto& row : in.at("textures")) {
				SceneCacheTexture texture;
				texture.image = row.at("image").get<std::uint32_t>();
				if (texture.image >= scene.images.size()) throw std::runtime_error("texture image index out of range");
				const auto& sampler = row.at("sampler");
				texture.wrapS = sampler.at(0).get<int>();
				texture.wrapT = sampler.at(1).get<int>();
				texture.minFilter = sampler.at(2).get<int>();
				texture.magFilter = sampler.at(3).get<int>();
				texture.preferSRGB = row.at("srgb").get<bool>();
				texture.semantic = static_cast<TextureSemantic>(row.at("semantic").get<std::uint32_t>());
				texture.preservePackedChannels = row.at("packed").get<bool>();
				texture.normalConvention = static_cast<NormalMapConvention>(row.at("normalConvention").get<std::uint32_t>());
				scene.textures.push_back(texture);
			}
			for (const auto& row : in.at("materials")) {
				auto material = ReadMaterial(row);
				for (const std::int32_t texture : material.textures) {
					if (texture >= static_cast<std::int32_t>(scene.textures.size())) throw std::runtime_error("material texture index out of range");
				}
				scene.materials.push_back(std::move(material));
			}
			for (const auto& row : in.at("primitives")) {
				SceneCachePrimitive primitive;
				primitive.mesh = row.at("mesh").get<std::uint32_t>();
				primitive.primitive = row.at("primitive").get<std::uint32_t>();
				primitive.primPath = row.at("primPath").get<std::string>();
				primitive.doubleSidedVoxelSourceNormals = row.at("doubleSidedVoxelSourceNormals").get<bool>();
				primitive.vertexFlags = row.at("vertexFlags").get<std::uint32_t>();
				primitive.vertexSize = row.at("vertexSize").get<std::uint32_t>();
				primitive.skinningVertexSize = row.at("skinningVertexSize").get<std::uint32_t>();
				primitive.uvSetCount = row.at("uvSets").get<std::uint32_t>();
				primitive.material = row.at("material").get<std::int32_t>();
				if (primitive.material >= static_cast<std::int32_t>(scene.materials.size())) throw std::runtime_error("primitive material index out of range");
				scene.primitives.push_back(std::move(primitive));
			}
			for (const auto& row : in.at("parts")) {
				SceneCachePart part;
				part.name = row.value("name", std::string());
				part.localMatrix = ReadMatrix(row.at("matrix"));
				part.primitives = row.at("primitives").get<std::vector<std::uint32_t>>();
				for (const auto index : part.primitives) {
					if (index >= scene.primitives.size()) throw std::runtime_error("part primitive index out of range");
				}
				if (row.contains("instances")) {
					for (const auto& matrix : row["instances"]) part.instanceMatrices.push_back(ReadMatrix(matrix));
				}
				scene.parts.push_back(std::move(part));
			}
			return scene;
		}
		catch (const nlohmann::json::exception& error) {
			throw std::runtime_error(std::string("scene cache: malformed scene section: ") + error.what());
		}
		catch (const std::runtime_error& error) {
			throw std::runtime_error(std::string("scene cache: ") + error.what());
		}
	}
}
