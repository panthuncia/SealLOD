#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>

#include <limits>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <bit>
#include <cmath>
#include <array>
#include <functional>
#include <cstring>
#include <iterator>
#include <mutex>
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <numeric>
#include <optional>
#include <span>
#include <chrono>
#include <string>
#include <format>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/Assets/SGGX.h>
#include "BasicRenderer/Assets/Import/VoxelGroupBuilder.h"
#include <meshoptimizer.h>

#include "../shaders/Common/defines.h"

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"

namespace
{
	constexpr uint32_t CLOD_STREAMING_PAGE_SIZE_BYTES = 256u * 1024u;
	struct ClusterLODPageTelemetryContext
	{
		std::string sourceIdentifier;
		uint64_t referenceCount = 0u;
	};

	thread_local ClusterLODPageTelemetryContext g_clusterLODPageTelemetryContext;
	std::mutex g_clusterLODPageTelemetryContextMutex;
	std::unordered_map<std::string, std::pair<uint64_t, uint32_t>>
		g_clusterLODPageTelemetryActiveContexts;
	std::mutex g_clusterLODPageTelemetryMutex;
	std::atomic<uint64_t> g_clusterLODPageTelemetryBuildId{ 0u };
	bool g_clusterLODPageTelemetryInitialized = false;
	std::filesystem::path g_clusterLODPageTelemetryInitializedPath;

	std::string CsvEscape(std::string_view value)
	{
		if (value.find_first_of(",\"\r\n") == std::string_view::npos)
		{
			return std::string(value);
		}

		std::string escaped;
		escaped.reserve(value.size() + 2u);
		escaped.push_back('"');
		for (char ch : value)
		{
			escaped.push_back(ch);
			if (ch == '"')
			{
				escaped.push_back('"');
			}
		}
		escaped.push_back('"');
		return escaped;
	}

	uint32_t SelectTelemetryPageClass(size_t pageBytes)
	{
		constexpr std::array<uint32_t, 5> pageClasses{
			16u * 1024u,
			32u * 1024u,
			64u * 1024u,
			128u * 1024u,
			CLOD_STREAMING_PAGE_SIZE_BYTES
		};
		for (uint32_t pageClass : pageClasses)
		{
			if (pageBytes <= pageClass)
			{
				return pageClass;
			}
		}
		return CLOD_STREAMING_PAGE_SIZE_BYTES;
	}

	std::string FormatDepthCounts(const std::map<int32_t, uint64_t>& counts)
	{
		std::string result;
		for (const auto& [depth, count] : counts)
		{
			if (!result.empty())
			{
				result.push_back(';');
			}
			result += std::format("{}:{}", depth, count);
		}
		return result;
	}

}

	void EmitClusterLODPagePackingTelemetry(
		std::string_view buildKind,
		const ClusterLODPrebuiltData& data,
		const std::vector<std::vector<std::byte>>& pageBlobs)
	{
		const std::string telemetryPathText =
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_PAGE_PACKING_TELEMETRY");
		if (telemetryPathText.empty() || pageBlobs.empty())
		{
			return;
		}

		struct PageStats
		{
			std::unordered_set<uint32_t> groups;
			std::unordered_set<uint32_t> rootGroups;
			std::map<int32_t, uint64_t> groupsByDepth;
			std::map<int32_t, uint64_t> segmentsByDepth;
			std::map<int32_t, uint64_t> meshletsByDepth;
			uint64_t rootMeshlets = 0u;
			uint64_t nonRootMeshlets = 0u;
			int32_t minDepth = std::numeric_limits<int32_t>::max();
			int32_t maxDepth = std::numeric_limits<int32_t>::min();
		};

		std::vector<uint32_t> incomingParentCounts(data.groups.size(), 0u);
		for (const ClusterLODGroupSegment& segment : data.segments)
		{
			if (segment.refinedGroup >= 0 &&
				static_cast<uint32_t>(segment.refinedGroup) < incomingParentCounts.size())
			{
				incomingParentCounts[static_cast<uint32_t>(segment.refinedGroup)]++;
			}
		}

		uint32_t rootGroupBegin = 0u;
		uint32_t rootGroupEnd = static_cast<uint32_t>(data.groups.size());
		if (!data.partRecords.empty() && data.rootPartIndex < data.partRecords.size())
		{
			const ClusterLODPartRecord& rootPart = data.partRecords[data.rootPartIndex];
			rootGroupBegin = std::min(rootPart.groupBase, rootGroupEnd);
			rootGroupEnd = rootGroupBegin +
				std::min(rootPart.groupCount, rootGroupEnd - rootGroupBegin);
			if (rootGroupBegin == rootGroupEnd)
			{
				rootGroupBegin = 0u;
				rootGroupEnd = static_cast<uint32_t>(data.groups.size());
			}
		}

		std::vector<bool> rootGroups(data.groups.size(), false);
		for (uint32_t groupIndex = rootGroupBegin; groupIndex < rootGroupEnd; ++groupIndex)
		{
			rootGroups[groupIndex] = incomingParentCounts[groupIndex] == 0u;
		}

		std::vector<PageStats> pageStats(pageBlobs.size());
		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(data.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = data.groups[groupIndex];
			const uint32_t segmentEnd = std::min<uint32_t>(
				group.firstSegment + group.segmentCount,
				static_cast<uint32_t>(data.segments.size()));
			const uint32_t referenceOffset =
				groupIndex < data.groupPageReferenceOffsets.size()
				? data.groupPageReferenceOffsets[groupIndex]
				: 0u;
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < segmentEnd; ++segmentIndex)
			{
				const ClusterLODGroupSegment& segment = data.segments[segmentIndex];
				if (segment.meshletCount == 0u ||
					segment.pageIndex < group.pageMapBase)
				{
					continue;
				}
				const uint32_t localPage = segment.pageIndex - group.pageMapBase;
				const uint64_t referenceIndex =
					static_cast<uint64_t>(referenceOffset) + localPage;
				if (localPage >= group.pageCount ||
					referenceIndex >= data.groupPageReferences.size())
				{
					continue;
				}
				const uint32_t meshPageIndex =
					data.groupPageReferences[static_cast<size_t>(referenceIndex)];
				if (meshPageIndex >= pageStats.size())
				{
					continue;
				}

				PageStats& stats = pageStats[meshPageIndex];
				if (stats.groups.insert(groupIndex).second)
				{
					stats.groupsByDepth[group.depth]++;
				}
				if (rootGroups[groupIndex])
				{
					stats.rootGroups.insert(groupIndex);
					stats.rootMeshlets += segment.meshletCount;
				}
				else
				{
					stats.nonRootMeshlets += segment.meshletCount;
				}
				stats.segmentsByDepth[group.depth]++;
				stats.meshletsByDepth[group.depth] += segment.meshletCount;
				stats.minDepth = std::min(stats.minDepth, group.depth);
				stats.maxDepth = std::max(stats.maxDepth, group.depth);
			}
		}

		ClusterLODPageTelemetryContext telemetryContext =
			g_clusterLODPageTelemetryContext;
		if (telemetryContext.sourceIdentifier.empty())
		{
			std::scoped_lock contextLock(g_clusterLODPageTelemetryContextMutex);
			if (g_clusterLODPageTelemetryActiveContexts.size() == 1u)
			{
				const auto& [sourceIdentifier, value] =
					*g_clusterLODPageTelemetryActiveContexts.begin();
				telemetryContext.sourceIdentifier = sourceIdentifier;
				telemetryContext.referenceCount = value.first;
			}
		}

		const std::filesystem::path telemetryPath(telemetryPathText);
		const uint64_t buildId =
			g_clusterLODPageTelemetryBuildId.fetch_add(1u, std::memory_order_relaxed);
		std::scoped_lock lock(g_clusterLODPageTelemetryMutex);
		if (!telemetryPath.parent_path().empty())
		{
			std::error_code ec;
			std::filesystem::create_directories(telemetryPath.parent_path(), ec);
		}
		if (!g_clusterLODPageTelemetryInitialized ||
			g_clusterLODPageTelemetryInitializedPath != telemetryPath)
		{
			std::ofstream reset(telemetryPath, std::ios::trunc);
			if (!reset)
			{
				spdlog::warn(
					"ClusterLOD page telemetry could not initialize '{}'",
					telemetryPath.string());
				return;
			}
			reset
				<< "schema_version,build_id,source_identifier,reference_count,build_kind,"
				<< "page_index,representation,page_bytes,page_capacity_bytes,empty_bytes,"
				<< "utilization,variable_page_class_bytes,variable_empty_bytes,pinned_page,"
				<< "root_only_page,group_count,root_group_count,min_depth,max_depth,"
				<< "root_meshlets,non_root_meshlets,root_meshlet_fraction,"
				<< "groups_by_depth,segments_by_depth,meshlets_by_depth\n";
			g_clusterLODPageTelemetryInitialized = true;
			g_clusterLODPageTelemetryInitializedPath = telemetryPath;
		}

		std::ofstream out(telemetryPath, std::ios::app);
		if (!out)
		{
			spdlog::warn(
				"ClusterLOD page telemetry could not append '{}'",
				telemetryPath.string());
			return;
		}

		for (uint32_t pageIndex = 0u; pageIndex < static_cast<uint32_t>(pageBlobs.size()); ++pageIndex)
		{
			const PageStats& stats = pageStats[pageIndex];
			const size_t pageBytes = pageBlobs[pageIndex].size();
			const uint32_t pageClass = SelectTelemetryPageClass(pageBytes);
			const bool pinned = !stats.rootGroups.empty();
			const bool rootOnly = pinned && stats.rootGroups.size() == stats.groups.size();
			const uint64_t attributedMeshlets = stats.rootMeshlets + stats.nonRootMeshlets;
			const double rootFraction = attributedMeshlets != 0u
				? static_cast<double>(stats.rootMeshlets) / static_cast<double>(attributedMeshlets)
				: 0.0;
			const int32_t minDepth =
				stats.minDepth == std::numeric_limits<int32_t>::max() ? -1 : stats.minDepth;
			const int32_t maxDepth =
				stats.maxDepth == std::numeric_limits<int32_t>::min() ? -1 : stats.maxDepth;

			out
				<< 1 << ','
				<< buildId << ','
				<< CsvEscape(telemetryContext.sourceIdentifier) << ','
				<< telemetryContext.referenceCount << ','
				<< buildKind << ','
				<< pageIndex << ','
				<< (pageIndex < data.trianglePageCount ? "triangle" : "voxel") << ','
				<< pageBytes << ','
				<< CLOD_STREAMING_PAGE_SIZE_BYTES << ','
				<< (CLOD_STREAMING_PAGE_SIZE_BYTES - pageBytes) << ','
				<< std::setprecision(9)
				<< (static_cast<double>(pageBytes) / CLOD_STREAMING_PAGE_SIZE_BYTES) << ','
				<< pageClass << ','
				<< (pageClass - pageBytes) << ','
				<< (pinned ? 1 : 0) << ','
				<< (rootOnly ? 1 : 0) << ','
				<< stats.groups.size() << ','
				<< stats.rootGroups.size() << ','
				<< minDepth << ','
				<< maxDepth << ','
				<< stats.rootMeshlets << ','
				<< stats.nonRootMeshlets << ','
				<< rootFraction << ','
				<< CsvEscape(FormatDepthCounts(stats.groupsByDepth)) << ','
				<< CsvEscape(FormatDepthCounts(stats.segmentsByDepth)) << ','
				<< CsvEscape(FormatDepthCounts(stats.meshletsByDepth))
				<< '\n';
		}
	}

void SetClusterLODPagePackingTelemetryContext(
	std::string sourceIdentifier,
	std::uint64_t referenceCount)
{
	g_clusterLODPageTelemetryContext.sourceIdentifier = std::move(sourceIdentifier);
	g_clusterLODPageTelemetryContext.referenceCount = referenceCount;
	if (!g_clusterLODPageTelemetryContext.sourceIdentifier.empty())
	{
		std::scoped_lock lock(g_clusterLODPageTelemetryContextMutex);
		auto& active =
			g_clusterLODPageTelemetryActiveContexts[
				g_clusterLODPageTelemetryContext.sourceIdentifier];
		active.first = referenceCount;
		active.second++;
	}
}

void ClearClusterLODPagePackingTelemetryContext()
{
	if (!g_clusterLODPageTelemetryContext.sourceIdentifier.empty())
	{
		std::scoped_lock lock(g_clusterLODPageTelemetryContextMutex);
		const auto activeIt = g_clusterLODPageTelemetryActiveContexts.find(
			g_clusterLODPageTelemetryContext.sourceIdentifier);
		if (activeIt != g_clusterLODPageTelemetryActiveContexts.end())
		{
			if (activeIt->second.second <= 1u)
			{
				g_clusterLODPageTelemetryActiveContexts.erase(activeIt);
			}
			else
			{
				activeIt->second.second--;
			}
		}
	}
	g_clusterLODPageTelemetryContext = {};
}

