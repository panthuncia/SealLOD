#pragma once

#include <memory>
#include <flecs.h>
#include <cstdint>

#include "Resources/Resource.h"
#include "Render/RenderPhase.h"

namespace Components {
	
	struct Resource {
		std::weak_ptr<::org::Resource> resource;
	};

    struct CLodOnlyDrawWorkload {};
    struct GeneralDrawWorkload {};

	struct BelongsToView {};
	struct ParticipatesInPass {};
}
