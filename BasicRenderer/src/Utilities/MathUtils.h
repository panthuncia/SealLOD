#pragma once
#include <DirectXMath.h>

#include "BasicRenderer/Scene/Components.h"
#include <BasicScene/MovementState.h>
#include "BasicRenderer/Extensions/ShaderBuffers.h"

void ApplyMovement(Components::Position& pos, const Components::Rotation& rot, const MovementState& movement, float deltaTime);
void RotatePitchYaw(Components::Rotation& rot, float pitch, float yaw);
DirectX::XMVECTOR GetForwardFromMatrix(const DirectX::XMMATRIX& matrix);
DirectX::XMVECTOR GetUpFromMatrix(const DirectX::XMMATRIX& matrix);
float CalculateLightRadius(float intensity, float constant, float linear, float quadratic, float threshold = 0.3f);
BoundingSphere ComputeConeBoundingSphere(const DirectX::XMVECTOR& origin, const DirectX::XMVECTOR& direction, float height, float halfAngle);
unsigned int GetNextPowerOfTwo(unsigned int value);
inline uint32_t DivRoundUp(uint32_t num, uint32_t den) { return (num + den - 1) / den; }
DirectX::XMFLOAT2 hammersley(uint i, float numSamples);
float Halton(uint32_t i, uint32_t b);
struct BasisVectors
{
    DirectX::XMVECTOR Right;
    DirectX::XMVECTOR   Up;
    DirectX::XMVECTOR Forward;
};

// Extract and normalize the three axes.
// � Right  = local +X  
// � Up     = local +Y  
// � Forward= local +Z  (or �Z for a right-handed "forward")
inline BasisVectors XM_CALLCONV GetBasisVectors(const DirectX::XMMATRIX& M, bool rightHandedForward = true)
{
    BasisVectors B;

    // rows 0,1,2 are your basis X,Y,Z
    B.Right = DirectX::XMVector3Normalize(M.r[0]);
    B.Up = DirectX::XMVector3Normalize(M.r[1]);

    // In a LH system forward is +Z; in a RH "forward" is �Z
    DirectX::XMVECTOR zAxis = DirectX::XMVector3Normalize(M.r[2]);
    B.Forward = rightHandedForward
        ? DirectX::XMVectorNegate(zAxis)
        : zAxis;

    return B;
}

struct Basis3f
{
    DirectX::XMFLOAT3 Right;
    DirectX::XMFLOAT3 Up;
    DirectX::XMFLOAT3 Forward;
};

inline Basis3f XM_CALLCONV GetBasisVectors3f(const DirectX::XMMATRIX& M, bool rightHandedForward = true)
{
    // grab & normalize the rows
    DirectX::XMVECTOR r = DirectX::XMVector3Normalize(M.r[0]);
    DirectX::XMVECTOR u = DirectX::XMVector3Normalize(M.r[1]);
    DirectX::XMVECTOR z = DirectX::XMVector3Normalize(M.r[2]);

    // choose sign on Z-axis for forward
    DirectX::XMVECTOR f = rightHandedForward
        ? DirectX::XMVectorNegate(z)
        : z;

    // store into floats
    Basis3f out;
    DirectX::XMStoreFloat3(&out.Right, r);
    DirectX::XMStoreFloat3(&out.Up, u);
    DirectX::XMStoreFloat3(&out.Forward, f);
    return out;
}