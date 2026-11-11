#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "Materials/TextureStreaming/TextureStreamingPolicy.h"

namespace ts = br::texturestreaming;

namespace {
	void Expect(bool condition, const std::string& what) {
		if (!condition) throw std::runtime_error(what);
	}

	void FootprintCases() {
		// One texel per pixel on a 1024 texture: footprint log2(1/1024) wants mip 0; each doubling of the UV per pixel a mip coarser.
		Expect(ts::TopForFootprint(1024u, -10.0f, 10u) == 0u, "footprint -10 on 1024 is mip 0");
		Expect(ts::TopForFootprint(1024u, -8.0f, 10u) == 2u, "footprint -8 on 1024 is mip 2");
		Expect(ts::TopForFootprint(1024u, -8.5f, 10u) == 1u, "a fractional footprint rounds to the finer mip");
		Expect(ts::TopForFootprint(1024u, -20.0f, 10u) == 0u, "finer than mip 0 clamps to 0");
		Expect(ts::TopForFootprint(1024u, 3.0f, 10u) == 10u, "coarser than the last mip clamps to the coarsest");
		Expect(ts::TopForFootprint(1024u, -2.0f, 4u) == 4u, "a binding's cap limits how coarse it goes");
		Expect(ts::TopForFootprint(1024u, std::numeric_limits<float>::infinity(), 7u) == 7u, "no demand gives the coarsest");
		// The same footprint asks the same fraction of each texture's size: a material's one value serves all its textures.
		Expect(ts::TopForFootprint(4096u, -8.0f, 12u) == 4u, "footprint -8 on 4096 is mip 4");
		// A texture's own request survives the round trip through a footprint, power of two or not.
		for (std::uint32_t side : { 256u, 1000u, 1024u, 1536u, 4096u }) {
			for (std::uint32_t top = 0; top < 8u; ++top) {
				Expect(ts::TopForFootprint(side, ts::FootprintForTop(side, top), 12u) == top,
					"FootprintForTop round trip side=" + std::to_string(side) + " top=" + std::to_string(top));
			}
		}
		// The shader's encoding.
		const float footprint = -9.25f;
		const auto value = static_cast<std::uint32_t>((footprint + ts::kFootprintOffset) * ts::kFootprintScale);
		Expect(std::abs(ts::DecodeFootprint(value) - footprint) < 1.0f / ts::kFootprintScale, "footprint encoding round trip");
	}

	void DecideCases() {
		ts::Pace pace{};
		Expect(ts::Decide(ts::kNoWindow, 0u, pace) == ts::Action::None, "no window: nothing to decide");
		// One mip short waits a tick, then upgrades.
		pace = {};
		Expect(ts::Decide(4u, 3u, pace) == ts::Action::None, "an upgrade settles a tick");
		Expect(ts::Decide(4u, 3u, pace) == ts::Action::Upgrade, "then upgrades");
		// Three mips short is urgent.
		pace = {};
		Expect(ts::Decide(6u, 3u, pace) == ts::Action::Upgrade, "three mips short upgrades at once");
		// A downgrade needs the slack for kDowngradeTicks ticks in a row.
		pace = {};
		for (std::uint32_t tick = 1; tick < ts::kDowngradeTicks; ++tick) {
			Expect(ts::Decide(2u, 2u + ts::kDowngradeSlackMips, pace) == ts::Action::None, "a downgrade waits");
		}
		Expect(ts::Decide(2u, 2u + ts::kDowngradeSlackMips, pace) == ts::Action::Downgrade, "then downgrades");
		// A tick that wants the resident window again restarts the wait.
		pace = {};
		for (std::uint32_t tick = 1; tick < ts::kDowngradeTicks; ++tick) (void)ts::Decide(2u, 4u, pace);
		Expect(ts::Decide(2u, 2u, pace) == ts::Action::None, "wanted again");
		Expect(ts::Decide(2u, 4u, pace) == ts::Action::None, "the wait restarted");
	}

	// Textures whose cost halves per mip of pull-back: held ones and on-screen ones.
	struct Scene {
		std::uint64_t held = 0, visible = 0;
		std::uint64_t operator()(float heldPullback, float visiblePullback) const {
			return static_cast<std::uint64_t>(static_cast<double>(held) / std::exp2(std::floor(heldPullback)) +
				static_cast<double>(visible) / std::exp2(std::floor(visiblePullback)));
		}
	};

	void PullbackCases() {
		// Over the allowance, held demand goes first, a mip a tick, until the windows fit.
		Scene scene{ 4000u, 1000u };
		ts::Pullback pullback{};
		for (int tick = 0; tick < 40; ++tick) ts::StepPullback(pullback, 2500u, 16.0f, scene);
		Expect(scene(pullback.held, pullback.visible) <= 2500u, "held pull-back makes it fit");
		Expect(pullback.visible == 0.0f, "what is on screen is untouched while held demand can give");
		Expect(pullback.held <= 2.0f, "no further than it needs");

		// When held demand cannot make it fit, what is on screen steps down, within the limit.
		pullback = {};
		for (int tick = 0; tick < 80; ++tick) ts::StepPullback(pullback, 500u, 16.0f, scene);
		Expect(scene(pullback.held, pullback.visible) <= 500u, "on-screen pull-back makes it fit");
		Expect(pullback.visible > 0.0f, "on-screen demand gave");
		pullback = {};
		for (int tick = 0; tick < 80; ++tick) ts::StepPullback(pullback, 500u, 1.0f, scene);
		Expect(pullback.visible <= 1.0f, "the on-screen limit holds");

		// With room again, it steps back only after the wait, and never to where it would not fit.
		pullback = { 4.0f, 0.0f, 0u };
		Scene small{ 1000u, 100u };
		for (std::uint32_t tick = 0; tick < ts::kRelaxTicks; ++tick) ts::StepPullback(pullback, 100000u, 16.0f, small);
		Expect(pullback.held == 4.0f, "no step back before the wait");
		for (int tick = 0; tick < 40; ++tick) ts::StepPullback(pullback, 100000u, 16.0f, small);
		Expect(pullback.held == 0.0f, "steps back fully when everything fits");
		pullback = { 4.0f, 0.0f, 0u };
		for (int tick = 0; tick < 200; ++tick) ts::StepPullback(pullback, 600u, 16.0f, small);
		Expect(small(pullback.held, pullback.visible) <= 600u, "never steps back past the allowance");
	}

	void BytesCases() {
		Expect(ts::AlignTextureBytes(1u) == 4096u, "small textures take 4 KiB");
		Expect(ts::AlignTextureBytes(65536u) == 65536u, "64 KiB is one small allocation");
		Expect(ts::AlignTextureBytes(65537u) == 131072u, "larger ones round to 64 KiB");
	}
}

int main() {
	try {
		FootprintCases();
		DecideCases();
		PullbackCases();
		BytesCases();
	}
	catch (const std::exception& error) {
		std::cerr << "TextureStreamingPolicyTests failed: " << error.what() << '\n';
		return 1;
	}
	std::cout << "TextureStreamingPolicyTests passed\n";
	return 0;
}
