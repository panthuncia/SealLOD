#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include <DirectXMath.h>

#include "BasicRenderer/Scene/Animation/Keyframe.h"

enum class AnimationInterpolationMode : uint8_t {
    Linear = 0,
    Step = 1,
};

class AnimationClip {
public:
    std::vector<Keyframe> positionKeyframes;
    std::vector<Keyframe> rotationKeyframes;
    std::vector<Keyframe> scaleKeyframes;
    AnimationInterpolationMode positionInterpolation = AnimationInterpolationMode::Linear;
    AnimationInterpolationMode rotationInterpolation = AnimationInterpolationMode::Linear;
    AnimationInterpolationMode scaleInterpolation = AnimationInterpolationMode::Linear;
    float duration;
    AnimationClip();

    void addPositionKeyframe(float time, const DirectX::XMFLOAT3& position, AnimationInterpolationMode interpolation = AnimationInterpolationMode::Linear);
    void addRotationKeyframe(float time, const DirectX::XMVECTOR& rotation, AnimationInterpolationMode interpolation = AnimationInterpolationMode::Linear);
    void addScaleKeyframe(float time, const DirectX::XMFLOAT3& scale, AnimationInterpolationMode interpolation = AnimationInterpolationMode::Linear);

    std::pair<Keyframe, Keyframe> findBoundingKeyframes(float currentTime, const std::vector<Keyframe>& keyframes) const;

private:
    void updateDuration(float time);
};
