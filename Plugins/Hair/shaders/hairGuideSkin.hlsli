#ifndef HAIR_GUIDE_SKIN_HLSLI
#define HAIR_GUIDE_SKIN_HLSLI

// Linear hair skinning with a rotation: how a strand point follows a guide. The evaluate pass uses it for render
// strands and the guide-level pass for guides that are not simulated, so the two cannot drift.
//
// Include after hairCommon.hlsli and after defining HAIR_GUIDE_POINTS (the descriptor index of the guide state). A
// shader that also writes the guide state defines HAIR_GUIDE_POINT_BUFFER as its RWStructuredBuffer type.

#ifndef HAIR_GUIDE_POINT_BUFFER
#define HAIR_GUIDE_POINT_BUFFER StructuredBuffer<HairGuidePoint>
#endif

// A strand vertex this frame and in the previous one.
struct HairEvaluatedPoint
{
    float3 position;
    float3 previousPosition;
};

// One guide's part of a skinned point: the guide at curve parameter `parameter`, plus the point's rigid offset from the
// guide turned as the guide's tangent has turned from its rigid pose. Guides differ in point count, so they are sampled
// by parameter; the tangent is the central difference at the guide's points, interpolated, so the turn is continuous.
HairEvaluatedPoint HairSkinToGuide(uint guideBase, uint guideSegmentCount, float parameter, float3 rigid, float3 previousRigid)
{
    HAIR_GUIDE_POINT_BUFFER guidePoints = ResourceDescriptorHeap[HAIR_GUIDE_POINTS];
    const float along = parameter * (float)guideSegmentCount;
    const uint segment = min((uint)along, guideSegmentCount - 1u);
    const float share = along - (float)segment;
    const HairGuidePoint before = guidePoints[guideBase + max(segment, 1u) - 1u];
    const HairGuidePoint start = guidePoints[guideBase + segment];
    const HairGuidePoint end = guidePoints[guideBase + segment + 1u];
    const HairGuidePoint after = guidePoints[guideBase + min(segment + 2u, guideSegmentCount)];
    const float3 up = float3(0.0f, 1.0f, 0.0f);

    HairEvaluatedPoint skinned;
    const float3 rigidTangent = HairSafeNormalize(lerp(HairSafeNormalize(end.rigid - before.rigid, up),
        HairSafeNormalize(after.rigid - start.rigid, up), share), up);
    const float3 tangent = HairSafeNormalize(lerp(HairSafeNormalize(end.position - before.position, rigidTangent),
        HairSafeNormalize(after.position - start.position, rigidTangent), share), rigidTangent);
    skinned.position = lerp(start.position, end.position, share) +
        HairRotateBetween(rigidTangent, tangent, rigid - lerp(start.rigid, end.rigid, share));

    const float3 previousRigidTangent = HairSafeNormalize(lerp(HairSafeNormalize(end.previousRigid - before.previousRigid, up),
        HairSafeNormalize(after.previousRigid - start.previousRigid, up), share), up);
    const float3 previousTangent = HairSafeNormalize(
        lerp(HairSafeNormalize(end.previousPosition - before.previousPosition, previousRigidTangent),
            HairSafeNormalize(after.previousPosition - start.previousPosition, previousRigidTangent), share), previousRigidTangent);
    skinned.previousPosition = lerp(start.previousPosition, end.previousPosition, share) +
        HairRotateBetween(previousRigidTangent, previousTangent, previousRigid - lerp(start.previousRigid, end.previousRigid, share));
    return skinned;
}

// The same for a guide following a guide of the level below (hairGuideLevels.hlsl), with one difference: the turn is the
// smallest rotation of the chord from the guide's root to the sampled point, from its rigid pose to where it is, not of
// the tangent there. With the tangent, guides evaluated through three levels thrashed: on a wavy groom with only level 0
// simulated some moved 350 mm in a frame, against 12 mm with every guide simulated (30 mm on a straight groom); with
// the chord, 14 mm. A guide can be a fifth of a metre from its parent, and what it gets is the next level's parent. The
// likely cause: the tangent's turn carries no twist, so along a wavy parent it swings from side to side with the waves,
// which over such an offset puts waves of its own into the child, whose tangents then turn further than the parent's
// did. The chord is an integral of the curve, not a derivative: it turns slowly along the strand, and an error in a
// parent's points does not grow in its children.
HairEvaluatedPoint HairSkinToGuideChord(uint guideBase, uint guideSegmentCount, float parameter, float3 rigid, float3 previousRigid)
{
    HAIR_GUIDE_POINT_BUFFER guidePoints = ResourceDescriptorHeap[HAIR_GUIDE_POINTS];
    const float along = parameter * (float)guideSegmentCount;
    const uint segment = min((uint)along, guideSegmentCount - 1u);
    const float share = along - (float)segment;
    const HairGuidePoint root = guidePoints[guideBase];
    const HairGuidePoint start = guidePoints[guideBase + segment];
    const HairGuidePoint end = guidePoints[guideBase + segment + 1u];
    const float3 up = float3(0.0f, 1.0f, 0.0f);

    // At the root the chord has no length and both directions fall back to the same one: no turn.
    HairEvaluatedPoint skinned;
    const float3 guideRigid = lerp(start.rigid, end.rigid, share);
    const float3 guidePosition = lerp(start.position, end.position, share);
    const float3 rigidChord = HairSafeNormalize(guideRigid - root.rigid, up);
    const float3 chord = HairSafeNormalize(guidePosition - root.position, rigidChord);
    skinned.position = guidePosition + HairRotateBetween(rigidChord, chord, rigid - guideRigid);

    const float3 previousGuideRigid = lerp(start.previousRigid, end.previousRigid, share);
    const float3 previousGuidePosition = lerp(start.previousPosition, end.previousPosition, share);
    const float3 previousRigidChord = HairSafeNormalize(previousGuideRigid - root.previousRigid, up);
    const float3 previousChord = HairSafeNormalize(previousGuidePosition - root.previousPosition, previousRigidChord);
    skinned.previousPosition = previousGuidePosition +
        HairRotateBetween(previousRigidChord, previousChord, previousRigid - previousGuideRigid);
    return skinned;
}

#endif // HAIR_GUIDE_SKIN_HLSLI
