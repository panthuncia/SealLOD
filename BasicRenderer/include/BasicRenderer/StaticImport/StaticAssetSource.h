#pragma once

// The asset cache surface the static import pipeline depends on.
//
// The pipeline never loads or owns renderable assets itself. It asks a host-provided
// source for the parts of an asset (optionally under a material variant), polls and
// publishes asynchronous loads, and learns about failures through this interface. The
// host decides what an asset key means (SARP uses canonical game model paths) and how
// a material variant's overrides are stored.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <BasicRenderer/Assets/RenderableAsset.h>

namespace br::staticimport
{
	// Lifecycle of an asynchronously requested asset.
	enum class StaticAssetStatus
	{
		Invalid,        // The key does not name an asset.
		Ready,          // Published; its parts are available.
		ReadyToPublish, // Loaded, waiting to be published.
		Queued,
		Loading,
		Failed,
		Unsupported
	};

	// A material variant of an asset. `overrides` is host-owned data describing the
	// variant and is opaque to the pipeline; null (and hash 0) selects the asset's own
	// materials. `hash` is the host's identity for the override set.
	struct StaticMaterialVariant
	{
		std::uint64_t hash = 0;
		std::shared_ptr<const void> overrides;
	};

	class IStaticAssetSource
	{
	public:
		using ReadyAssetsCallback = std::function<void()>;

		virtual ~IStaticAssetSource() = default;

		// The canonical form of an asset key: keys naming the same asset map to the
		// same string. The pipeline keys asset dependencies by it and falls back to
		// the key as given when the result is empty.
		[[nodiscard]] virtual std::string CanonicalAssetKey(std::string_view assetKey) const
		{
			return std::string{ assetKey };
		}

		// Invoked (from any thread) when asynchronous loads have results ready to
		// publish. An empty callback clears it.
		virtual void SetReadyRenderableAssetsCallback(ReadyAssetsCallback callback) = 0;

		// The parts of an already published asset or material variant, or null when
		// it is not ready. Never starts a load.
		virtual const std::vector<br::import::RenderableAssetPart>* TryGetReadyParts(
			std::string_view assetKey,
			const StaticMaterialVariant& variant) const = 0;

		// Starts an asynchronous load if none is known, otherwise reports its state.
		// Does not publish a loaded result.
		virtual StaticAssetStatus QueueOrPollRenderableAsset(std::string_view assetKey) = 0;

		// Publishes the asset if its load has finished.
		virtual StaticAssetStatus PublishRenderableAssetIfReady(std::string_view assetKey) = 0;

		// Publishes up to maxCount finished loads; returns how many were published.
		virtual std::size_t PublishReadyRenderableAssets(std::size_t maxCount) = 0;

		[[nodiscard]] virtual bool HasReadyRenderableAssets() const = 0;

		// Requests the asset under the given material variant (the plain asset for the
		// default variant). Returns true once the variant's parts are available.
		virtual bool RequestMaterialVariant(std::string_view assetKey, const StaticMaterialVariant& variant) = 0;

		virtual bool HasKnownFailure(std::string_view assetKey) const = 0;

		// The reason the asset can never be imported as static geometry, if known.
		virtual std::optional<std::string> KnownUnsupportedStaticImportReason(std::string_view assetKey) const = 0;
	};
}
