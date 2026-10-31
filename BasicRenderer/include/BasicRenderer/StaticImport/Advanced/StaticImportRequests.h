#pragma once

// Static-import request and dependency descriptors.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <BasicRenderer/StaticImport/StaticAssetSource.h>
#include <BasicRenderer/StaticImport/StaticImportExtension.h>
#include <BasicRenderer/Pipeline/ShaderVariantRequestService.h>

namespace br::staticimport
{

	// Placed: a group the host added through the flat intake (AddOrUpdateGroups,
	// AddPreparedGroups); it is live while that add is the desired one.
	// HostGroup: a group whose residency an extension manages (see StaticHostGroupRequest).
	// HostImport: a prepared import an extension submitted and takes back on publication
	// (see br::staticimport::StaticHostImportPublication).
	enum class StaticImportResidencyClass : std::uint8_t { Placed, HostGroup, HostImport };

	struct DirectStaticTemplateKey
	{
		std::uint64_t meshID{ 0 };
		std::uint64_t materialID{ 0 };
		std::uint64_t rasterFlags{ 0 };

		bool operator==(const DirectStaticTemplateKey&) const = default;
	};

	struct DirectStaticTemplateKeyHash
	{
		std::size_t operator()(const DirectStaticTemplateKey& key) const noexcept
		{
			std::size_t hash = std::hash<std::uint64_t>{}(key.meshID);
			hash ^= std::hash<std::uint64_t>{}(key.materialID) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			hash ^= std::hash<std::uint64_t>{}(key.rasterFlags) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}
	};

	// `variant.hash` is the host's content hash of the override set; it keys the
	// request's dependency and deduplicates queued requests.
	struct StaticMaterialVariantRequest
	{
		std::string modelPath;
		br::staticimport::StaticMaterialVariant variant;
	};

	struct StaticShaderVariantRequest
	{
		ShaderVariantRequest request;
	};

	// A dependency resolved by the extension that requested it.
	struct StaticHostDependencyRequest
	{
		br::staticimport::IStaticImportExtension* owner{ nullptr };
		br::staticimport::StaticHostDependency dependency;
	};

	struct StaticAssetPublishRequest
	{
		std::string modelPath;
	};

	struct StaticTemplateRequest
	{
		DirectStaticTemplateKey key;
		std::shared_ptr<Mesh> mesh;
		std::shared_ptr<Material> material;
		// The material slot this template's own usage admission reserved. Set
		// before the request reaches the publication drain.
		std::optional<std::uint32_t> materialDataIndex;
	};

	enum class StaticImportDependencyKind
	{
		Template,
		Asset,
		MaterialVariant,
		ShaderVariant,
		Host
	};

	struct StaticImportDependencyKey
	{
		StaticImportDependencyKind kind{ StaticImportDependencyKind::Asset };
		std::uint64_t numericKey{ 0 };
		std::uint64_t numericKey2{ 0 };
		std::uint64_t numericKey3{ 0 };
		std::string stringKey;

		bool operator==(const StaticImportDependencyKey&) const = default;
	};

	struct StaticImportDependencyKeyHash
	{
		std::size_t operator()(const StaticImportDependencyKey& key) const noexcept
		{
			std::size_t hash = std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(key.kind));
			hash ^= std::hash<std::uint64_t>{}(key.numericKey) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			hash ^= std::hash<std::uint64_t>{}(key.numericKey2) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			hash ^= std::hash<std::uint64_t>{}(key.numericKey3) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			hash ^= std::hash<std::string>{}(key.stringKey) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}
	};

	struct StaticImportDependencyRequest
	{
		StaticImportDependencyKey key;
		StaticTemplateRequest templateRequest;
		StaticAssetPublishRequest assetRequest;
		StaticMaterialVariantRequest materialVariantRequest;
		StaticShaderVariantRequest shaderVariantRequest;
		StaticHostDependencyRequest hostRequest;
	};
	inline std::string MakeShaderVariantDependencyStringKey(const StaticShaderVariantRequest& request)
	{
		const auto normalized = NormalizeShaderVariantRequest(request.request);
		std::ostringstream key;
		key << "shader:"
			<< static_cast<std::uint32_t>(normalized.kind)
			<< ":mc=" << static_cast<std::uint64_t>(normalized.materialCompileFlags)
			<< ":mr=" << static_cast<std::uint32_t>(normalized.materialRasterFlags)
			<< ":ro=" << static_cast<std::uint32_t>(normalized.rasterOutputKind)
			<< ":wf=" << (normalized.wireframe ? 1 : 0)
			<< ":sv=" << (normalized.singleView ? 1 : 0);
		return key.str();
	}


	// Pure factories over the types above; extensions, the coordinator and the worker
	// pool all build dependency requests, so they live with the types.
	inline StaticImportDependencyKey MakeStaticImportDependencyKey(const StaticTemplateRequest& request)
	{
		return StaticImportDependencyKey{
			.kind = StaticImportDependencyKind::Template,
			.numericKey = request.key.meshID,
			.numericKey2 = request.key.materialID,
			.numericKey3 = request.key.rasterFlags
		};
	}

	inline DirectStaticTemplateKey DirectStaticTemplateKeyFromDependency(const StaticImportDependencyKey& key)
	{
		return DirectStaticTemplateKey{
			.meshID = key.numericKey,
			.materialID = key.numericKey2,
			.rasterFlags = key.numericKey3
		};
	}

	inline StaticImportDependencyKey MakeStaticImportDependencyKey(const br::staticimport::IStaticAssetSource& assets, const StaticAssetPublishRequest& request)
	{
		auto key = assets.CanonicalAssetKey(request.modelPath);
		if (key.empty()) {
			key = request.modelPath;
		}
		return StaticImportDependencyKey{
			.kind = StaticImportDependencyKind::Asset,
			.stringKey = std::move(key)
		};
	}

	inline StaticImportDependencyKey MakeStaticImportDependencyKey(const br::staticimport::IStaticAssetSource& assets, const StaticMaterialVariantRequest& request)
	{
		auto key = assets.CanonicalAssetKey(request.modelPath);
		if (key.empty()) {
			key = request.modelPath;
		}
		key += "#txsw:";
		key += std::to_string(request.variant.hash);
		return StaticImportDependencyKey{
			.kind = StaticImportDependencyKind::MaterialVariant,
			.stringKey = std::move(key)
		};
	}

	inline StaticImportDependencyKey MakeStaticImportDependencyKey(const StaticShaderVariantRequest& request)
	{
		return StaticImportDependencyKey{
			.kind = StaticImportDependencyKind::ShaderVariant,
			.stringKey = MakeShaderVariantDependencyStringKey(request)
		};
	}

	// Host dependencies are keyed by their owner as well as their own key, so two
	// extensions' keys never collide.
	inline StaticImportDependencyKey MakeStaticImportDependencyKey(const StaticHostDependencyRequest& request)
	{
		return StaticImportDependencyKey{
			.kind = StaticImportDependencyKind::Host,
			.numericKey = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(request.owner)),
			.stringKey = request.dependency.key
		};
	}

	inline StaticImportDependencyRequest MakeStaticImportDependencyRequest(StaticTemplateRequest request)
	{
		StaticImportDependencyRequest dependency;
		dependency.key = MakeStaticImportDependencyKey(request);
		dependency.templateRequest = std::move(request);
		return dependency;
	}

	inline StaticImportDependencyRequest MakeStaticImportDependencyRequest(const br::staticimport::IStaticAssetSource& assets, StaticAssetPublishRequest request)
	{
		StaticImportDependencyRequest dependency;
		dependency.key = MakeStaticImportDependencyKey(assets, request);
		dependency.assetRequest = std::move(request);
		if (!dependency.key.stringKey.empty()) {
			dependency.assetRequest.modelPath = dependency.key.stringKey;
		}
		return dependency;
	}

	inline StaticImportDependencyRequest MakeStaticImportDependencyRequest(const br::staticimport::IStaticAssetSource& assets, StaticMaterialVariantRequest request)
	{
		StaticImportDependencyRequest dependency;
		dependency.key = MakeStaticImportDependencyKey(assets, request);
		dependency.materialVariantRequest = std::move(request);
		auto canonicalModel = assets.CanonicalAssetKey(dependency.materialVariantRequest.modelPath);
		if (!canonicalModel.empty()) {
			dependency.materialVariantRequest.modelPath = std::move(canonicalModel);
		}
		return dependency;
	}

	inline StaticImportDependencyRequest MakeStaticImportDependencyRequest(StaticShaderVariantRequest request)
	{
		request.request = NormalizeShaderVariantRequest(request.request);
		StaticImportDependencyRequest dependency;
		dependency.key = MakeStaticImportDependencyKey(request);
		dependency.shaderVariantRequest = request;
		return dependency;
	}

	inline StaticImportDependencyRequest MakeStaticImportDependencyRequest(StaticHostDependencyRequest request)
	{
		StaticImportDependencyRequest dependency;
		dependency.key = MakeStaticImportDependencyKey(request);
		dependency.hostRequest = std::move(request);
		return dependency;
	}

}
