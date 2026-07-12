#include "Import/USDMaterialCache.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>

#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <spdlog/spdlog.h>

#include "Import/CLodCache.h"

namespace USDMaterialCache {
namespace {

constexpr std::uint32_t kMagic = 0x54414D55u; // UMAT
constexpr std::wstring_view kManifestFileName = L"usd_assembly_materials_v1.bin";

class Writer {
public:
	explicit Writer(const std::filesystem::path& path) : stream(path, std::ios::binary | std::ios::trunc) {}
	template <typename T> void Pod(const T& value) { stream.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
	void String(const std::string& value) {
		const std::uint64_t size = value.size(); Pod(size);
		stream.write(value.data(), static_cast<std::streamsize>(value.size()));
	}
	template <typename T> void Vector(const std::vector<T>& values) {
		const std::uint64_t count = values.size(); Pod(count);
		if (!values.empty()) stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
	}
	bool Good() const { return stream.good(); }
	void Close() { stream.close(); }
private:
	std::ofstream stream;
};

class Reader {
public:
	explicit Reader(const std::filesystem::path& path) : stream(path, std::ios::binary) {}
	template <typename T> bool Pod(T& value) { return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value))); }
	bool String(std::string& value) {
		std::uint64_t size = 0; if (!Pod(size) || size > (64ull << 20)) return false;
		value.resize(static_cast<std::size_t>(size));
		return size == 0 || static_cast<bool>(stream.read(value.data(), static_cast<std::streamsize>(size)));
	}
	template <typename T> bool Vector(std::vector<T>& values) {
		std::uint64_t count = 0; if (!Pod(count) || count > (1ull << 20)) return false;
		values.resize(static_cast<std::size_t>(count));
		return values.empty() || static_cast<bool>(stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size())));
	}
	explicit operator bool() const { return stream.good(); }
private:
	std::ifstream stream;
};

void WriteBinding(Writer& out, const TextureAndConstant& value) {
	out.Pod(value.factor); out.Vector(value.channels); out.Pod(value.uvSetIndex); out.String(value.uvSetName); out.String(value.sourcePath);
}

bool ReadBinding(Reader& in, TextureAndConstant& value) {
	value.texture.reset();
	return in.Pod(value.factor) && in.Vector(value.channels) && in.Pod(value.uvSetIndex) && in.String(value.uvSetName) && in.String(value.sourcePath);
}

template <typename Fn> void ForBindings(MaterialDescription& value, Fn&& fn) {
	fn(value.baseColor); fn(value.metallic); fn(value.roughness); fn(value.emissive); fn(value.opacity); fn(value.aoMap); fn(value.heightMap); fn(value.normal);
	fn(value.openPBRTextures.coatColor); fn(value.openPBRTextures.coatWeight); fn(value.openPBRTextures.coatRoughness);
	fn(value.openPBRTextures.fuzzColor); fn(value.openPBRTextures.fuzzWeight); fn(value.openPBRTextures.fuzzRoughness);
}

void WriteMaterial(Writer& out, MaterialDescription value) {
	ForBindings(value, [](TextureAndConstant& binding) { binding.texture.reset(); });
	out.Pod(value.materialModel); out.String(value.name); out.Pod(value.diffuseColor); out.Pod(value.emissiveColor);
	out.Pod(value.alphaCutoff); out.Pod(value.heightMapScale); out.Pod(value.geometricDisplacementMin); out.Pod(value.geometricDisplacementMax);
	out.Pod(value.negateNormals); out.Pod(value.invertNormalGreen); out.Pod(value.forceDoubleSided); out.Pod(value.enableGeometricDisplacement);
	out.Pod(value.geometricDisplacementOptIn); out.Pod(value.brniflyVertexAlpha); out.Pod(value.brniflyZBufferWrite); out.Pod(value.brniflyDecal);
	out.Pod(value.brniflyDynamicDecal); out.Pod(value.brniflyModelSpaceNormals); out.Pod(value.heightMapFromBaseColorAlpha);
	out.Pod(value.objectSurfaceSamplingMode); out.Pod(value.objectSurfaceUseTriplanarProjection); out.Pod(value.objectSurfaceUseTripleTapStochastic);
	out.Pod(value.objectSurfaceTexelDensity); out.String(value.staticTextureOverrideSourceName); out.Pod(value.forceVoxelMaterial); out.Pod(value.blendState);
	ForBindings(value, [&](TextureAndConstant& binding) { WriteBinding(out, binding); });
	out.Pod(value.openPBR); out.Pod(value.glintEnabled); out.Pod(value.glintParameters);
}

bool ReadMaterial(Reader& in, MaterialDescription& value) {
	if (!in.Pod(value.materialModel) || !in.String(value.name) || !in.Pod(value.diffuseColor) || !in.Pod(value.emissiveColor) ||
		!in.Pod(value.alphaCutoff) || !in.Pod(value.heightMapScale) || !in.Pod(value.geometricDisplacementMin) || !in.Pod(value.geometricDisplacementMax) ||
		!in.Pod(value.negateNormals) || !in.Pod(value.invertNormalGreen) || !in.Pod(value.forceDoubleSided) || !in.Pod(value.enableGeometricDisplacement) ||
		!in.Pod(value.geometricDisplacementOptIn) || !in.Pod(value.brniflyVertexAlpha) || !in.Pod(value.brniflyZBufferWrite) || !in.Pod(value.brniflyDecal) ||
		!in.Pod(value.brniflyDynamicDecal) || !in.Pod(value.brniflyModelSpaceNormals) || !in.Pod(value.heightMapFromBaseColorAlpha) ||
		!in.Pod(value.objectSurfaceSamplingMode) || !in.Pod(value.objectSurfaceUseTriplanarProjection) || !in.Pod(value.objectSurfaceUseTripleTapStochastic) ||
		!in.Pod(value.objectSurfaceTexelDensity) || !in.String(value.staticTextureOverrideSourceName) || !in.Pod(value.forceVoxelMaterial) || !in.Pod(value.blendState)) return false;
	bool ok = true; ForBindings(value, [&](TextureAndConstant& binding) { ok = ok && ReadBinding(in, binding); });
	return ok && in.Pod(value.openPBR) && in.Pod(value.glintEnabled) && in.Pod(value.glintParameters);
}

std::filesystem::path ManifestPath(const std::string& sourceIdentifier) {
	return CLodCache::GetCacheFilePathForSource(std::wstring(kManifestFileName), sourceIdentifier);
}

pxr::UsdShadeShader ResolveShader(const pxr::UsdShadeConnectableAPI& source, const pxr::TfToken& output, unsigned depth = 0) {
	if (!source || depth > 16) return pxr::UsdShadeShader();
	if (pxr::UsdShadeShader shader(source.GetPrim()); shader) return shader;
	const pxr::UsdShadeOutput sourceOutput = source.GetOutput(output);
	if (!sourceOutput) return pxr::UsdShadeShader();
	const auto connected = sourceOutput.GetConnectedSources();
	if (connected.empty()) return pxr::UsdShadeShader();
	return ResolveShader(connected.front().source, connected.front().sourceName, depth + 1);
}

TextureAndConstant* BindingFor(MaterialDescription& value, std::string_view name) {
	if (name == "diffuseColor" || name == "baseColor") return &value.baseColor;
	if (name == "metallic" || name == "base_metalness") return &value.metallic;
	if (name == "roughness" || name == "specular_roughness") return &value.roughness;
	if (name == "emissiveColor" || name == "emission_color") return &value.emissive;
	if (name == "opacity" || name == "geometry_opacity") return &value.opacity;
	if (name == "normal") return &value.normal;
	return nullptr;
}

std::vector<std::uint32_t> Swizzle(std::string_view value) {
	std::vector<std::uint32_t> result;
	for (char ch : value) {
		if (ch == 'r' || ch == 'x') result.push_back(0); else if (ch == 'g' || ch == 'y') result.push_back(1);
		else if (ch == 'b' || ch == 'z') result.push_back(2); else if (ch == 'a' || ch == 'w') result.push_back(3);
	}
	return result;
}

} // namespace

MaterialDescription ExtractMaterialDescription(const pxr::UsdShadeMaterial& material) {
	MaterialDescription result;
	if (!material) return result;
	result.name = material.GetPrim().GetPath().GetString();
	result.alphaCutoff = 0.0f;
	const auto surface = material.GetSurfaceOutput(pxr::UsdShadeTokens->universalRenderContext);
	if (!surface) return result;
	const auto sources = surface.GetConnectedSources();
	if (sources.empty()) return result;
	const pxr::UsdShadeShader shader = ResolveShader(sources.front().source, sources.front().sourceName);
	if (!shader) return result;
	pxr::TfToken shaderId; shader.GetIdAttr().Get(&shaderId);
	const bool openPbr = shaderId.GetString().find("OpenPBR") != std::string::npos;
	if (openPbr) result.materialModel = MaterialModel::OpenPBR;

	for (const pxr::UsdShadeInput& input : shader.GetInputs()) {
		const std::string name = input.GetBaseName().GetString();
		const auto connected = input.GetConnectedSources();
		if (connected.empty()) {
			pxr::GfVec3f color;
			float scalar = 0.0f;
			if ((name == "diffuseColor" || name == "base_color") && input.Get(&color)) result.diffuseColor = { color[0], color[1], color[2], 1.0f };
			else if ((name == "emissiveColor" || name == "emission_color") && input.Get(&color)) result.emissiveColor = { color[0], color[1], color[2], 1.0f };
			else if ((name == "metallic" || name == "base_metalness") && input.Get(&scalar)) result.metallic.factor = scalar;
			else if ((name == "roughness" || name == "specular_roughness") && input.Get(&scalar)) result.roughness.factor = scalar;
			else if ((name == "opacity" || name == "geometry_opacity") && input.Get(&scalar)) result.opacity.factor = scalar;
			else if (name == "opacityThreshold" && input.Get(&scalar)) result.alphaCutoff = scalar;
			continue;
		}
		const pxr::UsdShadeShader producer = ResolveShader(connected.front().source, connected.front().sourceName);
		pxr::TfToken producerId; if (!producer || !producer.GetIdAttr().Get(&producerId) || producerId != pxr::TfToken("UsdUVTexture")) continue;
		TextureAndConstant* binding = BindingFor(result, name);
		if (!binding) continue;
		pxr::SdfAssetPath asset; producer.GetInput(pxr::TfToken("file")).Get(&asset);
		binding->sourcePath = asset.GetAssetPath().empty() ? asset.GetResolvedPath() : asset.GetAssetPath();
		binding->channels = Swizzle(connected.front().sourceName.GetString());
		if ((name == "diffuseColor" || name == "base_color") && binding->channels.size() == 3) binding->channels.push_back(3);
		const auto st = producer.GetInput(pxr::TfToken("st"));
		if (!st) continue;
		const auto stSources = st.GetConnectedSources();
		if (!stSources.empty()) {
			const pxr::UsdShadeShader reader = ResolveShader(stSources.front().source, stSources.front().sourceName);
			pxr::TfToken uvName; if (reader) reader.GetInput(pxr::TfToken("varname")).Get(&uvName);
			binding->uvSetName = uvName.GetString();
		}
	}
	return result;
}

bool SaveAssemblyMaterialManifest(const std::string& sourceIdentifier, const std::vector<AssemblyMaterialEntry>& entries) {
	const auto path = ManifestPath(sourceIdentifier);
	std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
	const auto temp = path.string() + ".tmp";
	Writer out(temp); out.Pod(kMagic); out.Pod(kManifestVersion); out.String(sourceIdentifier);
	const std::uint64_t count = entries.size(); out.Pod(count);
	for (const auto& entry : entries) {
		spdlog::info(
			"USD cached assembly material: name='{}' baseColor=({}, {}, {}, {}) albedo='{}'.",
			entry.material.name,
			entry.material.diffuseColor.x, entry.material.diffuseColor.y,
			entry.material.diffuseColor.z, entry.material.diffuseColor.w,
			entry.material.baseColor.sourcePath);
		out.String(entry.identity.sourceIdentifier); out.String(entry.identity.primPath); out.String(entry.identity.subsetName);
		out.Pod(entry.identity.doubleSidedVoxelSourceNormals); WriteMaterial(out, entry.material);
	}
	if (!out.Good()) return false;
	out.Close();
	std::filesystem::rename(temp, path, ec);
	if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(temp, path, ec); }
	return !ec;
}

void RemoveAssemblyMaterialManifest(const std::string& sourceIdentifier) {
	std::error_code ec;
	std::filesystem::remove(ManifestPath(sourceIdentifier), ec);
}

std::optional<std::vector<AssemblyMaterialEntry>> LoadAssemblyMaterialManifest(const std::string& sourceIdentifier) {
	Reader in(ManifestPath(sourceIdentifier)); if (!in) return std::nullopt;
	std::uint32_t magic = 0, version = 0; std::string storedSource; std::uint64_t count = 0;
	if (!in.Pod(magic) || !in.Pod(version) || magic != kMagic || version != kManifestVersion || !in.String(storedSource) || storedSource != sourceIdentifier || !in.Pod(count) || count > 4096) return std::nullopt;
	std::vector<AssemblyMaterialEntry> entries(static_cast<std::size_t>(count));
	for (auto& entry : entries) {
		if (!in.String(entry.identity.sourceIdentifier) || !in.String(entry.identity.primPath) || !in.String(entry.identity.subsetName) ||
			!in.Pod(entry.identity.doubleSidedVoxelSourceNormals) || !ReadMaterial(in, entry.material)) return std::nullopt;
	}
	return entries;
}

} // namespace USDMaterialCache
