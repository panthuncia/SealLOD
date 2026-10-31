#pragma once

// Pure identity, fingerprint and cost helpers for the static-import pipeline.

#include <algorithm>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>

#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportSupport.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>

namespace br::staticimport
{

	inline StaticImportTransactionCost StaticImportPreparedGroupCost(
		const br::render::PreparedStaticGroupInfo& group)
	{
		StaticImportTransactionCost cost;
		const auto transforms = static_cast<std::uint64_t>(group.PerObjectRows().size());
		const auto meshTemplates = group.MeshTemplates();
		const auto templates = static_cast<std::uint64_t>(meshTemplates.size());
		cost.groups = 1;
		cost.drawRecords = transforms * templates;
		cost.bytes += transforms * sizeof(PerObjectCB);
		cost.bytes += transforms * sizeof(DirectX::XMFLOAT4X4);
		cost.bytes += cost.drawRecords * sizeof(InstanceDrawRecordCB);
		for (std::size_t meshIndex = 0; meshIndex < meshTemplates.size(); ++meshIndex) {
			const auto workloadKeys = meshIndex < group.workloadKeysByMeshTemplate.size()
				? std::span<const DrawWorkloadKey>{ group.workloadKeysByMeshTemplate[meshIndex] }
				: meshTemplates[meshIndex].WorkloadKeys();
			cost.activeInsertIndices += transforms * static_cast<std::uint64_t>(workloadKeys.size());
		}
		return cost;
	}

	inline StaticImportTransactionCost StaticImportPreparedRangeCost(
		const br::render::PreparedStaticGroupsBulkPlan& plan,
		std::size_t firstGroup,
		std::size_t groupCount)
	{
		StaticImportTransactionCost cost;
		const auto groupEnd = (std::min)(plan.groups.size(), firstGroup + groupCount);
		for (std::size_t groupIndex = firstGroup; groupIndex < groupEnd; ++groupIndex) {
			cost.Add(StaticImportPreparedGroupCost(plan.groups[groupIndex]));
		}
		return cost;
	}

	inline br::render::ArtifactAddress StaticTemplateArtifactAddress(
		const DirectStaticTemplateKey& key)
	{
		return {
			br::render::ArtifactKind::StaticTemplate,
			key.meshID,
			key.materialID ^ (key.rasterFlags + 0x9e3779b97f4a7c15ull +
				(key.materialID << 6u) + (key.materialID >> 2u))
		};
	}

	inline std::uint64_t StaticTemplateSourceFingerprint(
		const DirectStaticTemplateKey& key)
	{
		std::uint64_t fingerprint = key.meshID ^ 0xcbf29ce484222325ull;
		fingerprint ^= key.materialID + 0x9e3779b97f4a7c15ull +
			(fingerprint << 6u) + (fingerprint >> 2u);
		fingerprint ^= key.rasterFlags + 0x9e3779b97f4a7c15ull +
			(fingerprint << 6u) + (fingerprint >> 2u);
		return fingerprint == 0 ? 1u : fingerprint;
	}

	inline std::uint64_t StaticVariantFingerprint(const PendingStaticGroup& group,
		const StaticSceneRenderSettings& settings)
	{
		std::uint64_t value = std::hash<std::string_view>{}(group.ModelPath());
		const auto mix = [&value](std::uint64_t item) {
			value ^= item + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
		};
		mix(group.materialVariant.hash);
		mix(static_cast<std::uint64_t>(settings.softwareRasterMode));
		mix(static_cast<std::uint64_t>(settings.vsmRasterMode));
		mix(static_cast<std::uint64_t>(settings.transparencyMode));
		mix(settings.reyesEnabled ? 1u : 0u);
		return value == 0 ? 1u : value;
	}

	inline br::render::ArtifactAddress StaticVariantArtifactAddress(
		const PendingStaticGroup& group, const StaticSceneRenderSettings& settings)
	{
		const auto fingerprint = StaticVariantFingerprint(group, settings);
		return { br::render::ArtifactKind::StaticVariant, fingerprint,
			group.materialVariant.hash ^ (fingerprint >> 32u) };
	}

	inline StaticImportTransactionCost StaticImportPreparedBatchCost(const PreparedDirectStaticImportBatch& batch)
	{
		StaticImportTransactionCost cost;
		for (const auto& prepared : batch.imports) {
			if (prepared.recipePrepared) {
				cost.Add({
					.groups = prepared.submittedGroupCount,
					.bytes = prepared.recipeImportCost.preparedBytes,
					.drawRecords = prepared.recipeImportCost.drawRecords,
					.activeInsertIndices = prepared.recipeImportCost.activeInsertIndices
				});
			} else {
				cost.Add(StaticImportPreparedRangeCost(
					batch.transactionBuild.prepared,
					prepared.firstBuildGroup,
					prepared.submittedGroupCount));
			}
		}
		return cost;
	}

	inline void RecomputePreparedStaticPlanTotals(br::render::PreparedStaticGroupsBulkPlan& plan)
	{
		plan.transformRows = 0;
		plan.drawRecords = 0;
		plan.preparedBytes = 0;
		for (const auto& group : plan.groups) {
			const auto transforms = static_cast<std::uint64_t>(group.PerObjectRows().size());
			const auto templates = static_cast<std::uint64_t>(group.MeshTemplates().size());
			plan.transformRows += transforms;
			plan.drawRecords += transforms * templates;
			plan.preparedBytes += transforms * sizeof(PerObjectCB);
			plan.preparedBytes += transforms * sizeof(DirectX::XMFLOAT4X4);
			plan.preparedBytes += transforms * templates * sizeof(InstanceDrawRecordCB);
		}
	}

	inline void AppendStaticObjectRemovalPayload(
		br::render::StaticObjectRemovalPayload& dst,
		br::render::StaticObjectRemovalPayload& src)
	{
		if (dst.drawInfoCount == 0 && dst.inlineBufferRangeCount == 0 &&
			dst.bufferRanges.empty() && dst.activeDrawSetRemovals.empty() &&
			!dst.sharedActiveDrawSetRemovals && dst.drawRecordIndices.empty()) {
			dst = std::move(src);
			return;
		}
		if (src.sharedActiveDrawSetRemovals) {
			if (!dst.sharedActiveDrawSetRemovals) {
				dst.sharedActiveDrawSetRemovals = src.sharedActiveDrawSetRemovals;
				dst.firstActiveDrawSetRemovalRange = src.firstActiveDrawSetRemovalRange;
				dst.activeDrawSetRemovalRangeCount = src.activeDrawSetRemovalRangeCount;
			} else {
				const auto expectedNext = dst.firstActiveDrawSetRemovalRange +
					dst.activeDrawSetRemovalRangeCount;
				if (dst.sharedActiveDrawSetRemovals != src.sharedActiveDrawSetRemovals ||
					expectedNext != src.firstActiveDrawSetRemovalRange) {
					throw std::logic_error("non-contiguous static-import removal ownership merge");
				}
				dst.activeDrawSetRemovalRangeCount += src.activeDrawSetRemovalRangeCount;
			}
		}
		dst.drawInfoCount += src.drawInfoCount;
		dst.bufferRanges.reserve(
			dst.bufferRanges.size() + src.inlineBufferRangeCount + src.bufferRanges.size());
		for (std::size_t i = 0; i < src.inlineBufferRangeCount; ++i) {
			dst.bufferRanges.push_back(std::move(src.inlineBufferRanges[i]));
		}
		src.inlineBufferRangeCount = 0;
		dst.bufferRanges.insert(dst.bufferRanges.end(),
			std::make_move_iterator(src.bufferRanges.begin()),
			std::make_move_iterator(src.bufferRanges.end()));
		dst.activeDrawSetRemovals.reserve(
			dst.activeDrawSetRemovals.size() + src.activeDrawSetRemovals.size());
		dst.activeDrawSetRemovals.insert(dst.activeDrawSetRemovals.end(),
			std::make_move_iterator(src.activeDrawSetRemovals.begin()),
			std::make_move_iterator(src.activeDrawSetRemovals.end()));
		dst.drawRecordIndices.reserve(
			dst.drawRecordIndices.size() + src.drawRecordIndices.size());
		dst.drawRecordIndices.insert(dst.drawRecordIndices.end(),
			std::make_move_iterator(src.drawRecordIndices.begin()),
			std::make_move_iterator(src.drawRecordIndices.end()));
	}
}
