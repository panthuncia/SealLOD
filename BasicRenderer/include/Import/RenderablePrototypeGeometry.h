#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

namespace br::import {

struct RenderablePrototypeVertex
{
	DirectX::XMFLOAT3 position{};
	DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT2 uv{};
	DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

struct RenderablePrototypeGeometry
{
	std::vector<RenderablePrototypeVertex> vertices;
	std::vector<std::uint32_t> indices;
	std::uint32_t vertexFlags{ 0 };
};

} // namespace br::import
