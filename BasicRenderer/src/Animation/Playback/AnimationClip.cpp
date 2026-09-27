#include "BasicRenderer/Scene/Animation/AnimationClip.h"

AnimationClip::AnimationClip() : duration(0.0f) {}

void AnimationClip::addPositionKeyframe(float time, const XMFLOAT3& position, AnimationInterpolationMode interpolation) {
    positionKeyframes.emplace_back(time, XMLoadFloat3(&position));
    positionInterpolation = interpolation;
    updateDuration(time);
}

void AnimationClip::addRotationKeyframe(float time, const XMVECTOR& rotation, AnimationInterpolationMode interpolation) {
    rotationKeyframes.emplace_back(time, rotation);
    rotationInterpolation = interpolation;
    updateDuration(time);
}

void AnimationClip::addScaleKeyframe(float time, const XMFLOAT3& scale, AnimationInterpolationMode interpolation) {
    scaleKeyframes.emplace_back(time, XMLoadFloat3(&scale));
    scaleInterpolation = interpolation;
    updateDuration(time);
}

void AnimationClip::updateDuration(float time) {
    if (time > duration) {
        duration = time;
    }
}

std::pair<Keyframe, Keyframe> AnimationClip::findBoundingKeyframes(float currentTime, const std::vector<Keyframe>& keyframes) const {
    Keyframe prevKeyframe = keyframes.front();
    Keyframe nextKeyframe = keyframes.back();

    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        if (currentTime >= keyframes[i].time && currentTime < keyframes[i + 1].time) {
            prevKeyframe = keyframes[i];
            nextKeyframe = keyframes[i + 1];
            break;
        }
    }

    return std::make_pair(prevKeyframe, nextKeyframe);
}
