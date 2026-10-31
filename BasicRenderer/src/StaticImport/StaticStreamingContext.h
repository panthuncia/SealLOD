#pragma once

// The objects every static-streaming subsystem needs.
//
// The shared pipeline state, the work-bus queues, the pipeline state, the drain
// scheduler, the telemetry counters, the static-import settings, the asset source and
// the registered extensions travel together into the coordinator and the streaming
// pump.
//
// Subsystems may still keep their own pointers internally;
// this exists so the *interface* names the group once.

#include <BasicRenderer/StaticImport/StaticAssetSource.h>
#include <BasicRenderer/StaticImport/StaticImportExtension.h>
#include <BasicRenderer/StaticImport/StaticImportSettings.h>

#include <span>

#include <BasicRenderer/StaticImport/StaticImportShared.h>
#include <BasicRenderer/StaticImport/StaticStreamCounters.h>
#include "StaticImport/StaticImportDrainScheduler.h"
#include "StaticImport/StaticImportPipelineState.h"
#include "StaticImport/StaticImportWorkBus.h"

namespace br::staticimport
{
	struct StaticStreamingContext
	{
		StaticImportShared& shared;
		StaticImportWorkBus& bus;
		StaticImportPipelineState& importState;
		StaticImportDrainScheduler& drains;
		StaticStreamCounters& counters;
		const br::staticimport::StaticImportSettings& settings;
		br::staticimport::IStaticAssetSource& assets;
		// In registration order; every extension hook is called on each of them.
		std::span<br::staticimport::IStaticImportExtension* const> extensions;
	};
}
