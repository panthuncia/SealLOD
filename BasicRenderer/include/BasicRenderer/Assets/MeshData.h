#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Material;

struct MeshUvSetData {
    std::string name;
    std::vector<DirectX::XMFLOAT2> values;
};

struct MeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<DirectX::XMFLOAT4> tangents;
    std::vector<DirectX::XMFLOAT3> colors;
    std::vector<uint32_t> indices;
    std::vector<MeshUvSetData> uvSets;
    std::vector<uint32_t> joints;
    std::vector<float> weights;
    std::shared_ptr<Material> material;
    unsigned int flags = 0;
	int skinIndex = -1;
};
