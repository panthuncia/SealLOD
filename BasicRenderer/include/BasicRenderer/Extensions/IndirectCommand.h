#pragma once

#include <d3d12.h>

struct DispatchMeshIndirectCommand {
	unsigned int perObjectBufferIndex;
	unsigned int perMeshBufferIndex;
	unsigned int perMeshInstanceBufferIndex;
	D3D12_DISPATCH_MESH_ARGUMENTS dispatchMeshArguments;
};

struct DispatchIndirectCommand {
	unsigned int perObjectBufferIndex;
	unsigned int perMeshBufferIndex;
	unsigned int perMeshInstanceBufferIndex;
	D3D12_DISPATCH_ARGUMENTS dispatchArguments;
};

struct MaterialEvaluationIndirectCommand {
	// Root constants (all uints):
	unsigned int materialId; // IndirectCommandSignatureRootConstant0
	unsigned int baseOffset; // IndirectCommandSignatureRootConstant1
	unsigned int count; // IndirectCommandSignatureRootConstant2
	unsigned int dispatchXDimension; // IndirectCommandSignatureRootConstant3
	D3D12_DISPATCH_ARGUMENTS dispatchArguments;
};

struct TerrainRegionMaterialEvaluationIndirectCommand {
	// Root constants (all uints):
	unsigned int terrainSetIndex; // IndirectCommandSignatureRootConstant0
	unsigned int regionIndex; // IndirectCommandSignatureRootConstant1
	unsigned int baseOffset; // IndirectCommandSignatureRootConstant2
	unsigned int count; // IndirectCommandSignatureRootConstant3
	unsigned int dispatchXDimension; // IndirectCommandSignatureRootConstant4
	D3D12_DISPATCH_ARGUMENTS dispatchArguments;
};
