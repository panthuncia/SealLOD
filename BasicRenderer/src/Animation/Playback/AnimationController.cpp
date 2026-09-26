#include "BasicRenderer/Scene/Animation/AnimationController.h"

AnimationController::AnimationController()
    : animationClip(nullptr), currentTime(0.0f), isPlaying(true) {
}

AnimationController::AnimationController(const AnimationController& other)
	: animationClip(other.animationClip), currentTime(other.currentTime), isPlaying(other.isPlaying) {
}

void AnimationController::setAnimationClip(std::shared_ptr<AnimationClip> newAnimationClip) {
    animationClip = newAnimationClip;
    currentTime = 0.0f;
    m_lastPositionKeyframeIndex = 0;
    m_lastRotationKeyframeIndex = 0;
    m_lastScaleKeyframeIndex = 0;
    //node->ForceUpdate();
    UpdateTransform();
}

void AnimationController::reset() {
    currentTime = 0.0f;
    m_lastPositionKeyframeIndex = 0;
    m_lastRotationKeyframeIndex = 0;
    m_lastScaleKeyframeIndex = 0;
}

void AnimationController::pause() {
    isPlaying = false;
}

void AnimationController::unpause() {
    isPlaying = true;
}

Components::Transform& AnimationController::GetUpdatedTransform(float elapsedTime, bool force) {
    if (!force && (!isPlaying || !animationClip)) return m_transform;

    if (animationClip->duration > 0.0f) {
        currentTime += elapsedTime * m_animationSpeed;
        currentTime = fmod(currentTime, animationClip->duration);
    }
    else {
        currentTime = 0.0f;
    }

    UpdateTransform();
    return m_transform;
}

std::pair<unsigned int, unsigned int> findBoundingKeyframes(float currentTime, const std::vector<Keyframe>& keyframes, unsigned int& counter) {
    if (keyframes.empty()) {
        counter = 0;
        return std::make_pair(0u, 0u);
    }

    if (keyframes.size() == 1) {
        counter = 0;
        return std::make_pair(0u, 0u);
    }

    const unsigned int lastKeyframeIndex = static_cast<unsigned int>(keyframes.size() - 1);
    if (counter >= lastKeyframeIndex) {
        counter = 0;
    }

    if (currentTime <= keyframes.front().time) {
        counter = 0;
        return std::make_pair(0u, 0u);
    }

    if (currentTime >= keyframes.back().time) {
        counter = lastKeyframeIndex;
        return std::make_pair(lastKeyframeIndex, lastKeyframeIndex);
    }

    for (unsigned int i = counter; i < lastKeyframeIndex; ++i) {
        if (currentTime >= keyframes[i].time && currentTime < keyframes[i + 1].time) {
            counter = i;
            return std::make_pair(i, i + 1);
        }
    }

    for (unsigned int i = 0; i < counter; ++i) {
        if (currentTime >= keyframes[i].time && currentTime < keyframes[i + 1].time) {
            counter = i;
            return std::make_pair(i, i + 1);
        }
    }

    counter = 0;
    return std::make_pair(0u, 0u);
}

void AnimationController::UpdateTransform() {
    if (!animationClip) return ;

    auto lerpVec3 = [](const XMVECTOR& start, const XMVECTOR& end, float t) {
        XMVECTOR lerped = XMVectorLerp(start, end, t);
        return lerped;
        };

    auto lerpRotation = [](const XMVECTOR& start, const XMVECTOR& end, float t) {
        XMVECTOR lerped = XMQuaternionSlerp(start, end, t);
        return lerped;
        };

    // Check if position keyframes are available
    if (!animationClip->positionKeyframes.empty()) {
        auto boundingPositionFrames = findBoundingKeyframes(currentTime, animationClip->positionKeyframes, m_lastPositionKeyframeIndex);
		Keyframe* prevKeyframe = &animationClip->positionKeyframes[boundingPositionFrames.first];
		Keyframe* nextKeyframe = &animationClip->positionKeyframes[boundingPositionFrames.second];
        if (animationClip->positionInterpolation == AnimationInterpolationMode::Step) {
            m_transform.pos = prevKeyframe->value;
        }
        else if (prevKeyframe->time != nextKeyframe->time) {
            float timeElapsed = currentTime - prevKeyframe->time;
            float diff = nextKeyframe->time - prevKeyframe->time;
            float t = diff > 0 ? timeElapsed / diff : 0;
            XMVECTOR interpolatedPosition = lerpVec3(prevKeyframe->value, nextKeyframe->value, t);
            //node->transform.setLocalPosition(interpolatedPosition);
			m_transform.pos = interpolatedPosition;
        }
        else {
            m_transform.pos = prevKeyframe->value;
        }
    }

    // Check if rotation keyframes are available
    if (!animationClip->rotationKeyframes.empty()) {
        auto boundingRotationFrames = findBoundingKeyframes(currentTime, animationClip->rotationKeyframes, m_lastRotationKeyframeIndex);
		Keyframe* prevKeyframe = &animationClip->rotationKeyframes[boundingRotationFrames.first];
		Keyframe* nextKeyframe = &animationClip->rotationKeyframes[boundingRotationFrames.second];
        if (animationClip->rotationInterpolation == AnimationInterpolationMode::Step) {
            m_transform.rot = prevKeyframe->value;
        }
        else if (prevKeyframe->time != nextKeyframe->time) {
            float timeElapsed = currentTime - prevKeyframe->time;
            float diff = nextKeyframe->time - prevKeyframe->time;
            float t = diff > 0 ? timeElapsed / diff : 0;
            XMVECTOR interpolatedRotation = lerpRotation(prevKeyframe->value, nextKeyframe->value, t);
            //node->transform.setLocalRotationFromQuaternion(interpolatedRotation);
			m_transform.rot = interpolatedRotation;
        }
        else {
            m_transform.rot = prevKeyframe->value;
        }
    }

    // Check if scale keyframes are available
    if (!animationClip->scaleKeyframes.empty()) {
        auto boundingScaleFrames = findBoundingKeyframes(currentTime, animationClip->scaleKeyframes, m_lastScaleKeyframeIndex);
		Keyframe* prevKeyframe = &animationClip->scaleKeyframes[boundingScaleFrames.first];
		Keyframe* nextKeyframe = &animationClip->scaleKeyframes[boundingScaleFrames.second];
        if (animationClip->scaleInterpolation == AnimationInterpolationMode::Step) {
            m_transform.scale = prevKeyframe->value;
        }
        else if (prevKeyframe->time != nextKeyframe->time) {
            float timeElapsed = currentTime - prevKeyframe->time;
            float diff = nextKeyframe->time - prevKeyframe->time;
            float t = diff > 0 ? timeElapsed / diff : 0;
            XMVECTOR interpolatedScale = lerpVec3(prevKeyframe->value, nextKeyframe->value, t);
            //node->transform.setLocalScale(interpolatedScale);
			m_transform.scale = interpolatedScale;
        }
        else {
            m_transform.scale = prevKeyframe->value;
        }
    }
}

void AnimationController::SetAnimationSpeed(float speed) { m_animationSpeed = speed; }
float AnimationController::GetAnimationSpeed() { return m_animationSpeed; }
