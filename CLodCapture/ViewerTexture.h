#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

// An RGBA8 image the CPU rewrites and ImGui draws. See ViewerTexture.cpp for why it lives in system memory.
namespace clodcapture {

class ViewerTexture {
public:
	ViewerTexture();
	~ViewerTexture();

	// Reallocates if the size changed. pixels holds width * height RGBA8 values.
	void Upload(uint32_t width, uint32_t height, const std::vector<uint32_t>& pixels);

	bool Valid() const;
	ImTextureID Id() const;
	uint32_t Width() const;
	uint32_t Height() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

}
