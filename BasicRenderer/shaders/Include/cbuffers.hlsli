#ifndef __CBUFFERS_HLSL__
#define __CBUFFERS_HLSL__

#include "structs.hlsli"

// point-clamp at s0
SamplerState g_pointClamp : register(s0);

// linear-clamp at s1
SamplerState g_linearClamp : register(s1);

cbuffer MiscUintRootConstants : register(b4) { // Used for pass-specific one-off constants
    uint UintRootConstant0;
    uint UintRootConstant1;
    uint UintRootConstant2;
    uint UintRootConstant3;
    uint UintRootConstant4;
    uint UintRootConstant5;
    uint UintRootConstant6;
    uint UintRootConstant7;
    uint UintRootConstant8;
    uint UintRootConstant9;
    uint UintRootConstant10;
    uint UintRootConstant11;
    uint UintRootConstant12;
    uint UintRootConstant13;
    uint UintRootConstant14;
    uint UintRootConstant15;
    uint UintRootConstant16;
    uint UintRootConstant17;
    uint UintRootConstant18;
    uint UintRootConstant19;
    uint UintRootConstant20;
    uint UintRootConstant21;
    uint UintRootConstant22;
    uint UintRootConstant23;
    uint UintRootConstant24;
    uint UintRootConstant25;
    uint UintRootConstant26;
    uint UintRootConstant27;
    uint UintRootConstant28;
}

cbuffer ResourceDescriptorIndices : register(b5) {
    uint ResourceDescriptorIndex0;
    uint ResourceDescriptorIndex1;
    uint ResourceDescriptorIndex2;
    uint ResourceDescriptorIndex3;
    uint ResourceDescriptorIndex4;
    uint ResourceDescriptorIndex5;
    uint ResourceDescriptorIndex6;
    uint ResourceDescriptorIndex7;
    uint ResourceDescriptorIndex8;
    uint ResourceDescriptorIndex9;
    uint ResourceDescriptorIndex10;
    uint ResourceDescriptorIndex11;
    uint ResourceDescriptorIndex12;
    uint ResourceDescriptorIndex13;
    uint ResourceDescriptorIndex14;
    uint ResourceDescriptorIndex15;
    uint ResourceDescriptorIndex16;
    uint ResourceDescriptorIndex17;
    uint ResourceDescriptorIndex18;
    uint ResourceDescriptorIndex19;
    uint ResourceDescriptorIndex20;
    uint ResourceDescriptorIndex21;
    uint ResourceDescriptorIndex22;
    uint ResourceDescriptorIndex23;
    uint ResourceDescriptorIndex24;
    uint ResourceDescriptorIndex25;
    uint ResourceDescriptorIndex26;
    uint ResourceDescriptorIndex27;
    uint ResourceDescriptorIndex28;
    uint ResourceDescriptorIndex29;
    uint ResourceDescriptorIndex30;
    uint ResourceDescriptorIndex31;
	uint ResourceDescriptorIndex32;
    uint ResourceDescriptorIndex33;
    uint ResourceDescriptorIndex34;
    uint ResourceDescriptorIndex35;
    uint ResourceDescriptorIndex36;
    uint ResourceDescriptorIndex37;
    uint ResourceDescriptorIndex38;
    uint ResourceDescriptorIndex39;
    uint ResourceDescriptorIndex40;
    uint ResourceDescriptorIndex41;
    uint ResourceDescriptorIndex42;
    uint ResourceDescriptorIndex43;
    uint ResourceDescriptorIndex44;
    uint ResourceDescriptorIndex45;
    uint ResourceDescriptorIndex46;
    uint ResourceDescriptorIndex47;
	uint ResourceDescriptorIndex48;
    uint ResourceDescriptorIndex49;
    uint ResourceDescriptorIndex50;
    uint ResourceDescriptorIndex51;
    uint ResourceDescriptorIndex52;
    uint ResourceDescriptorIndex53;
    uint ResourceDescriptorIndex54;
    uint ResourceDescriptorIndex55;
    uint ResourceDescriptorIndex56;
    uint ResourceDescriptorIndex57;
    uint ResourceDescriptorIndex58;
    uint ResourceDescriptorIndex59;
    uint ResourceDescriptorIndex60;
    uint ResourceDescriptorIndex61;
    uint ResourceDescriptorIndex62;
    uint ResourceDescriptorIndex63;
};

cbuffer IndirectCommandSignatureRootConstants : register(b6)
{
    uint IndirectCommandSignatureRootConstant0;
    uint IndirectCommandSignatureRootConstant1;
    uint IndirectCommandSignatureRootConstant2;
    uint IndirectCommandSignatureRootConstant3;
    uint IndirectCommandSignatureRootConstant4;
};

uint GetRootPerObjectBufferIndex()
{
#if defined(USE_MISC_DRAW_ROOT_CONSTANTS)
    return UintRootConstant19;
#else
    return IndirectCommandSignatureRootConstant0;
#endif
}

uint GetRootPerMeshBufferIndex()
{
#if defined(USE_MISC_DRAW_ROOT_CONSTANTS)
    return UintRootConstant20;
#else
    return IndirectCommandSignatureRootConstant1;
#endif
}

uint GetRootPerMeshInstanceBufferIndex()
{
#if defined(USE_MISC_DRAW_ROOT_CONSTANTS)
    return UintRootConstant21;
#else
    return IndirectCommandSignatureRootConstant2;
#endif
}

int GetRootCurrentLightID() { return (int)UintRootConstant22; }
int GetRootLightViewIndex() { return (int)UintRootConstant23; }
bool GetRootEnableShadows() { return UintRootConstant24 != 0u; }
bool GetRootEnablePunctualLights() { return UintRootConstant25 != 0u; }
bool GetRootEnableGTAO() { return UintRootConstant26 != 0u; }

#endif // __CBUFFERS_HLSL__
