#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// What decides a streamed texture's resident window, as pure functions. TextureStreamingManager's worker calls them on
// its policy tick; nothing here touches a texture, the GPU or a lock.
//
// Demand is a footprint: log2 of the UV one screen pixel covers. It needs no texture's size, so a material reports one
// value however many textures it samples, and each texture turns it into its own mip: top = log2(largest side) + footprint.
namespace br::texturestreaming {

	inline constexpr std::uint32_t kNoWindow = ~0u;

	// Shader encoding of a material's footprint (utilities.hlsli, RecordMaterialStreamingFootprint).
	inline constexpr float kFootprintOffset = 32.0f;
	inline constexpr float kFootprintScale = 256.0f;
	inline float DecodeFootprint(std::uint32_t value) {
		return static_cast<float>(value) / kFootprintScale - kFootprintOffset;
	}

	/**
	 * @brief The window top a footprint asks for: the mip with one texel per pixel, rounded to the finer mip, never coarser
	 * than a_coarsestTop. An infinite footprint (no demand) gives a_coarsestTop.
	 */
	inline std::uint32_t TopForFootprint(std::uint32_t largestSide, float footprintLog2, std::uint32_t coarsestTop) {
		if (!std::isfinite(footprintLog2)) return coarsestTop;
		// The small bias keeps a texture's own requested mip (stored as a footprint, below) from rounding one mip finer.
		const float mip = std::log2(static_cast<float>((std::max)(largestSide, 1u))) + footprintLog2 + 1.0e-3f;
		if (!(mip > 0.0f)) return 0u;
		return (std::min)(static_cast<std::uint32_t>(mip), coarsestTop);
	}

	/** @brief The footprint at which a texture asks for exactly a_top: how a texture's own requested mip joins material demand. */
	inline float FootprintForTop(std::uint32_t largestSide, std::uint32_t top) {
		return static_cast<float>(top) - std::log2(static_cast<float>((std::max)(largestSide, 1u)));
	}

	// The worker ticks this often.
	inline constexpr std::uint32_t kTickMilliseconds = 250u;
	// A report counts as on screen this long, then is held at a lower priority this long before the texture lets it go.
	inline constexpr std::uint32_t kVisibleTicks = 4u;   // 1 s
	inline constexpr std::uint32_t kHoldTicks = 32u;     // 8 s

	inline constexpr std::uint32_t kSettleTicks = 1u;         // an upgrade's demand holds this long first
	inline constexpr std::uint32_t kUrgentMips = 3u;          // this far short of what is wanted: at once
	inline constexpr std::uint32_t kDowngradeSlackMips = 1u;  // this much finer than wanted before a downgrade is considered
	inline constexpr std::uint32_t kDowngradeTicks = 20u;     // and for this long (5 s)
	inline constexpr std::uint32_t kPendingTimeoutTicks = 40u;  // a window asked for and not resident after this long is asked again

	// The budget's controller. Everything that is not a streamed window comes off the budget first; what is left is the allowance.
	inline constexpr float kBudgetTarget = 0.92f;       // windows are fitted to this share of the budget, leaving room for the frame's other growth
	inline constexpr float kPullbackStep = 0.25f;       // in mips: a texture crosses a whole mip at its own point, so small steps change the total smoothly
	inline constexpr std::uint32_t kPullbackSteps = 4u; // steps a tick while over the allowance (a mip a tick)
	inline constexpr float kMaxPullback = 16.0f;        // more than any texture's chain: everything at its coarsest
	inline constexpr float kRelaxMargin = 0.95f;        // a step back must fit this share of the allowance, or it would be taken again at once
	inline constexpr std::uint32_t kRelaxTicks = 20u;   // and fit for this long (5 s) before the first step back

	/** @brief How far demand is pulled back, in mips. Added to the footprint, so coarser. */
	struct Pullback {
		float held = 0.0f;     // of demand held from earlier reports: surfaces not seen in the last second
		float visible = 0.0f;  // of what is on screen: only once every held demand is at its coarsest, and within the setting's limit
		std::uint32_t relaxTicks = 0;
	};

	/**
	 * @brief One tick of the controller. a_cost(held, visible) is the bytes the windows take at that pull-back, which never
	 * rises as either does. Over a_allowance, it steps until the windows fit: held demand first, then what is on screen, by
	 * a_maxVisible mips at most. Under the allowance, it steps back after a wait, as long as the step would still fit with a
	 * margin. It reads what the windows would take rather than measured VRAM, so it does not wind up while they catch up.
	 */
	template <class Cost>
	void StepPullback(Pullback& pullback, std::uint64_t allowance, float maxVisible, const Cost& cost) {
		pullback.visible = (std::min)(pullback.visible, maxVisible);
		if (cost(pullback.held, pullback.visible) > allowance) {
			pullback.relaxTicks = 0;
			// No held demand can make it fit: no need to walk through the steps that change nothing.
			if (cost(kMaxPullback, pullback.visible) > allowance) pullback.held = kMaxPullback;
			for (std::uint32_t step = 0; step < kPullbackSteps && cost(pullback.held, pullback.visible) > allowance; ++step) {
				if (pullback.held < kMaxPullback) pullback.held += kPullbackStep;
				else if (pullback.visible < maxVisible) pullback.visible = (std::min)(pullback.visible + kPullbackStep, maxVisible);
				else break;
			}
			return;
		}
		auto next = pullback;
		if (next.visible > 0.0f) next.visible = (std::max)(next.visible - kPullbackStep, 0.0f);
		else if (next.held > 0.0f) next.held = (std::max)(next.held - kPullbackStep, 0.0f);
		else return;
		if (static_cast<double>(cost(next.held, next.visible)) > static_cast<double>(allowance) * kRelaxMargin) {
			pullback.relaxTicks = 0;
			return;
		}
		// After the wait, a step a tick: most steps back through a large pull-back change nothing.
		if (++pullback.relaxTicks > kRelaxTicks) {
			next.relaxTicks = pullback.relaxTicks;
			pullback = next;
		}
	}

	enum class Action : std::uint8_t { None, Upgrade, Downgrade };

	/** @brief One texture's counters between ticks. */
	struct Pace {
		std::uint32_t upgradeTicks = 0;    // ticks the wanted window has been finer than the resident one
		std::uint32_t downgradeTicks = 0;  // ticks the resident window has been finer than wanted by the slack
	};

	/** @brief What to do this tick, given the resident top and the wanted one. Advances a_pace. */
	inline Action Decide(std::uint32_t residentTop, std::uint32_t wantedTop, Pace& pace) {
		if (residentTop == kNoWindow) return Action::None;
		if (wantedTop < residentTop) {
			pace.downgradeTicks = 0;
			const bool urgent = residentTop - wantedTop >= kUrgentMips;
			if (urgent || pace.upgradeTicks >= kSettleTicks) {
				pace.upgradeTicks = 0;
				return Action::Upgrade;
			}
			++pace.upgradeTicks;
			return Action::None;
		}
		pace.upgradeTicks = 0;
		if (wantedTop >= residentTop + kDowngradeSlackMips) {
			if (++pace.downgradeTicks >= kDowngradeTicks) {
				pace.downgradeTicks = 0;
				return Action::Downgrade;
			}
			return Action::None;
		}
		pace.downgradeTicks = 0;
		return Action::None;
	}

	/** @brief Allocation granularity of a placed texture: 4 KiB for small resources, 64 KiB otherwise. */
	inline std::uint64_t AlignTextureBytes(std::uint64_t bytes) {
		const std::uint64_t alignment = bytes <= 65536u ? 4096u : 65536u;
		return (bytes + alignment - 1u) / alignment * alignment;
	}
}
