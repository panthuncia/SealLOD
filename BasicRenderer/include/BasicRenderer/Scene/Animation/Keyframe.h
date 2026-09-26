#pragma once

#include <DirectXMath.h>

using namespace DirectX;

class Keyframe {
public:
    float time;
    XMVECTOR value;

    Keyframe(float t, const XMVECTOR& v) : time(t), value(v) {}
};