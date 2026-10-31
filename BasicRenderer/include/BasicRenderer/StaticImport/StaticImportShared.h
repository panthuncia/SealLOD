#pragma once

// State the static import pipeline shares with the host subsystems that ride on it.
//
// The host owns one StaticImportShared and hands it to the static import runtime; the
// host's own static-adjacent subsystems (terrain, grass, LOD blocks, ...) see the same
// object, so they observe the same stop flag, task scope, worker scheduler and scene
// ingestion services as the pipeline.

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>
#include <BasicRenderer/Pipeline/CLodOptions.h>
#include <BasicRenderer/Scene/SceneIngestionServices.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include <BasicRenderer/StaticImport/Advanced/StaticImportWorkScheduler.h>

namespace br::staticimport
{
	// Renderer settings that shape static imports, captured when scene ingestion
	// services attach.
	struct StaticSceneRenderSettings
	{
		bool reyesEnabled = true;
		bool terrainReyesEnabled = false;
		float terrainDisplacementScale = 0.0f;
		float terrainParallaxFadeEndDistance = 0.0f;
		CLodSoftwareRasterMode softwareRasterMode{};
		CLodVSMRasterMode vsmRasterMode{};
		CLodTransparencyMode transparencyMode{};

		static StaticSceneRenderSettings Capture()
		{
			StaticSceneRenderSettings result;
			result.reyesEnabled = !br::extensions::MakeSettingGetter<bool>(CLodDisableReyesRasterizationSettingName)();
			result.terrainReyesEnabled = br::extensions::MakeSettingGetter<bool>("enableTerrainReyesDisplacement")();
			result.terrainDisplacementScale = (std::max)(0.0f,
				br::extensions::MakeSettingGetter<float>("terrainReyesDisplacementScale")());
			result.terrainParallaxFadeEndDistance = (std::max)(0.0f,
				br::extensions::MakeSettingGetter<float>("terrainParallaxFadeEndDistance")());
			result.softwareRasterMode = br::extensions::MakeSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)();
			result.vsmRasterMode = br::extensions::MakeSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)();
			result.transparencyMode = br::extensions::MakeSettingGetter<CLodTransparencyMode>(CLodTransparencyModeSettingName)();
			return result;
		}
	};

	struct GraphCompletionHandoff
	{
		std::atomic_bool alive{ true };
		std::atomic_bool materialTableWake{ false };
		mutable std::mutex staticGroupMutex;
		std::deque<br::render::ArtifactSnapshot> staticGroups;
		std::deque<br::render::ArtifactSnapshot> terminalStaticVariants;
		std::set<br::render::ArtifactVersionID> observedStaticGroupVersions;
		std::function<void()> staticGroupWake;

		void PushStaticGroup(const br::render::ArtifactSnapshot& snapshot)
		{
			std::function<void()> wake;
			{
				std::lock_guard lock(staticGroupMutex);
				if (!alive.load(std::memory_order_acquire)) return;
				if (!observedStaticGroupVersions.insert(snapshot.Version()).second) return;
				staticGroups.push_back(snapshot);
				wake = staticGroupWake;
			}
			if (wake) wake();
		}

		void PushTerminalStaticVariant(const br::render::ArtifactSnapshot& snapshot)
		{
			std::function<void()> wake;
			{
				std::lock_guard lock(staticGroupMutex);
				if (!alive.load(std::memory_order_acquire)) return;
				terminalStaticVariants.push_back(snapshot);
				wake = staticGroupWake;
			}
			if (wake) wake();
		}

		[[nodiscard]] std::size_t StaticGroupCount() const
		{
			std::lock_guard lock(staticGroupMutex);
			return staticGroups.size();
		}
	};

	struct StaticImportShared
	{
		std::atomic_bool staticImportServiceStop{ false };
		br::TaskScope staticStreamingScope;
		StaticImportWorkScheduler staticImportWorkScheduler;
		::br::render::StaticSceneIngestionAccess sceneIngestionServices{};
		bool hasSceneIngestionServices{ false };
		StaticSceneRenderSettings staticSceneRenderSettings{};
		std::shared_ptr<GraphCompletionHandoff> graphCompletions =
			std::make_shared<GraphCompletionHandoff>();
	};
}
