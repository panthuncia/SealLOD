#pragma once

#include <cmath>

namespace br::hair {

struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Float3 operator+(Float3 a, Float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Float3 operator-(Float3 a, Float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Float3 operator*(Float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline Float3 operator*(Float3 a, Float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; } // per component, as HLSL
inline bool operator==(Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

inline float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Float3 Cross(Float3 a, Float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Length(Float3 a) { return std::sqrt(Dot(a, a)); }

// Returns the zero vector for a zero-length input.
inline Float3 Normalize(Float3 a)
{
    const float length = Length(a);
    return length > 0.0f ? a * (1.0f / length) : Float3{};
}

// Two unit vectors that complete a right-handed orthonormal frame (tangent, bitangent, normal) around a unit normal.
inline void BuildTangentFrame(Float3 normal, Float3& tangent, Float3& bitangent)
{
    const Float3 helper = std::abs(normal.y) < 0.9f ? Float3{ 0.0f, 1.0f, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
    tangent = Normalize(Cross(helper, normal));
    bitangent = Cross(normal, tangent);
}

inline Float3 Lerp(Float3 a, Float3 b, float t) { return a + (b - a) * t; }
inline Float3 Min(Float3 a, Float3 b) { return { a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z }; }
inline Float3 Max(Float3 a, Float3 b) { return { a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z }; }

// Closest point to `p` on triangle (a, b, c), as barycentrics: the point is a + (b - a) * u + (c - a) * v.
// Degenerate triangles are handled (the result lies on the longest remaining edge or vertex).
inline Float3 ClosestPointOnTriangle(Float3 p, Float3 a, Float3 b, Float3 c, float& u, float& v)
{
    // Ericson, Real-Time Collision Detection, 5.1.5.
    const Float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    u = 0.0f;
    v = 0.0f;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const Float3 bp = p - b;
    const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        u = 1.0f;
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        u = d1 / (d1 - d3);
        return a + ab * u;
    }
    const Float3 cp = p - c;
    const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        v = 1.0f;
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        v = d2 / (d2 - d6);
        return a + ac * v;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        u = 1.0f - v;
        return b + (c - b) * v;
    }
    const float sum = va + vb + vc;
    if (!(sum > 0.0f)) {
        return a; // zero-area triangle whose edges all failed the tests above
    }
    u = vb / sum;
    v = vc / sum;
    return a + ab * u + ac * v;
}

} // namespace br::hair
