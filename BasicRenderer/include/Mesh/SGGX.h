#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace br::mesh::sggx
{
	struct Float3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		Float3() = default;
		Float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
		Float3 operator+(const Float3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Float3 operator-(const Float3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Float3 operator*(float s) const { return { x * s, y * s, z * s }; }
		float dot(const Float3& o) const { return x * o.x + y * o.y + z * o.z; }
		Float3 cross(const Float3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
		float lengthSq() const { return dot(*this); }
		float length() const { return std::sqrt(lengthSq()); }
		Float3 normalized() const
		{
			const float len = length();
			return len > 1.0e-12f ? (*this) * (1.0f / len) : Float3(0.0f, 0.0f, 1.0f);
		}
	};

	struct SymmetricMatrix3
	{
		float xx = 0.0f;
		float yy = 0.0f;
		float zz = 0.0f;
		float xy = 0.0f;
		float xz = 0.0f;
		float yz = 0.0f;

		SymmetricMatrix3 operator+(const SymmetricMatrix3& o) const
		{
			return { xx + o.xx, yy + o.yy, zz + o.zz, xy + o.xy, xz + o.xz, yz + o.yz };
		}

		SymmetricMatrix3 operator*(float s) const
		{
			return { xx * s, yy * s, zz * s, xy * s, xz * s, yz * s };
		}
	};

	struct AxialSGGX
	{
		Float3 axis = Float3(0.0f, 0.0f, 1.0f);
		float sigmaPerp = 1.0e-4f;
		float sigmaParallel = 0.5f;
	};

	[[nodiscard]] inline Float3 Mul(const SymmetricMatrix3& m, const Float3& v)
	{
		return {
			m.xx * v.x + m.xy * v.y + m.xz * v.z,
			m.xy * v.x + m.yy * v.y + m.yz * v.z,
			m.xz * v.x + m.yz * v.y + m.zz * v.z
		};
	}

	[[nodiscard]] inline SymmetricMatrix3 Outer(const Float3& v, float weight = 1.0f)
	{
		return {
			v.x * v.x * weight,
			v.y * v.y * weight,
			v.z * v.z * weight,
			v.x * v.y * weight,
			v.x * v.z * weight,
			v.y * v.z * weight
		};
	}

	[[nodiscard]] inline Float3 SafeNormalizeNormal(const Float3& n)
	{
		return n.lengthSq() > 1.0e-20f ? n.normalized() : Float3(0.0f, 0.0f, 1.0f);
	}

	inline void BuildFallbackBasis(const Float3& n, Float3& t, Float3& b)
	{
		const Float3 up = std::abs(n.z) < 0.999f ? Float3(0.0f, 0.0f, 1.0f) : Float3(0.0f, 1.0f, 0.0f);
		t = up.cross(n).normalized();
		b = n.cross(t).normalized();
	}

	[[nodiscard]] inline SymmetricMatrix3 SGGXFromNormal(const Float3& normal)
	{
		const Float3 n = SafeNormalizeNormal(normal);
		Float3 t, b;
		BuildFallbackBasis(n, t, b);
		constexpr float kMinSigma = 1.0e-4f;
		const float minS = kMinSigma * kMinSigma;
		return Outer(n, 0.25f) + Outer(t, minS) + Outer(b, minS);
	}

	[[nodiscard]] inline SymmetricMatrix3 SGGXFromAxial(const AxialSGGX& axial)
	{
		const Float3 axis = SafeNormalizeNormal(axial.axis);
		const float sigmaPerp = std::max(axial.sigmaPerp, 1.0e-4f);
		const float sigmaParallel = std::max(axial.sigmaParallel, 1.0e-4f);
		const float sp2 = sigmaPerp * sigmaPerp;
		const float sa2 = sigmaParallel * sigmaParallel;
		SymmetricMatrix3 result{};
		result.xx = sp2;
		result.yy = sp2;
		result.zz = sp2;
		return result + Outer(axis, sa2 - sp2);
	}

	[[nodiscard]] inline Float3 CanonicalizeAxialAxis(Float3 axis)
	{
		axis = SafeNormalizeNormal(axis);
		if (axis.z < 0.0f || (axis.z == 0.0f && (axis.y < 0.0f || (axis.y == 0.0f && axis.x < 0.0f)))) {
			axis = axis * -1.0f;
		}
		return axis;
	}

	[[nodiscard]] inline DirectX::XMFLOAT2 EncodeOctahedralAxis(Float3 axis)
	{
		axis = CanonicalizeAxialAxis(axis);
		const float invL1 = 1.0f / std::max(std::abs(axis.x) + std::abs(axis.y) + std::abs(axis.z), 1.0e-12f);
		return DirectX::XMFLOAT2(axis.x * invL1, axis.y * invL1);
	}

	[[nodiscard]] inline Float3 DecodeOctahedralAxis(float octX, float octY)
	{
		Float3 axis(octX, octY, 1.0f - std::abs(octX) - std::abs(octY));
		if (axis.z < 0.0f) {
			const float x = axis.x;
			const float y = axis.y;
			axis.x = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
			axis.y = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
		}
		return CanonicalizeAxialAxis(axis);
	}

	inline void JacobiRotate(float a[3][3], float v[3][3], int p, int q)
	{
		if (std::abs(a[p][q]) <= 1.0e-12f) {
			return;
		}

		const float tau = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
		const float t = (tau >= 0.0f ? 1.0f : -1.0f) / (std::abs(tau) + std::sqrt(1.0f + tau * tau));
		const float c = 1.0f / std::sqrt(1.0f + t * t);
		const float s = t * c;
		const float app = a[p][p];
		const float aqq = a[q][q];
		const float apq = a[p][q];
		a[p][p] = app - t * apq;
		a[q][q] = aqq + t * apq;
		a[p][q] = 0.0f;
		a[q][p] = 0.0f;

		for (int r = 0; r < 3; ++r) {
			if (r == p || r == q) {
				continue;
			}
			const float arp = a[r][p];
			const float arq = a[r][q];
			a[r][p] = c * arp - s * arq;
			a[p][r] = a[r][p];
			a[r][q] = s * arp + c * arq;
			a[q][r] = a[r][q];
		}

		for (int r = 0; r < 3; ++r) {
			const float vrp = v[r][p];
			const float vrq = v[r][q];
			v[r][p] = c * vrp - s * vrq;
			v[r][q] = s * vrp + c * vrq;
		}
	}

	[[nodiscard]] inline std::array<Float3, 3> EigenvectorsSymmetric(const SymmetricMatrix3& m)
	{
		float a[3][3] = {
			{ m.xx, m.xy, m.xz },
			{ m.xy, m.yy, m.yz },
			{ m.xz, m.yz, m.zz }
		};
		float v[3][3] = {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};

		for (int iter = 0; iter < 10; ++iter) {
			JacobiRotate(a, v, 0, 1);
			JacobiRotate(a, v, 0, 2);
			JacobiRotate(a, v, 1, 2);
		}

		return {
			SafeNormalizeNormal(Float3(v[0][0], v[1][0], v[2][0])),
			SafeNormalizeNormal(Float3(v[0][1], v[1][1], v[2][1])),
			SafeNormalizeNormal(Float3(v[0][2], v[1][2], v[2][2]))
		};
	}

	[[nodiscard]] inline AxialSGGX CompressSGGXToAxial(const SymmetricMatrix3& sggx)
	{
		const std::array<Float3, 3> axes = EigenvectorsSymmetric(sggx);
		std::array<float, 3> eigenvalues{};
		for (size_t axisIndex = 0; axisIndex < axes.size(); ++axisIndex) {
			eigenvalues[axisIndex] = std::max(axes[axisIndex].dot(Mul(sggx, axes[axisIndex])), 1.0e-8f);
		}

		size_t parallelAxisIndex = 0;
		for (size_t axisIndex = 1; axisIndex < eigenvalues.size(); ++axisIndex) {
			if (eigenvalues[axisIndex] > eigenvalues[parallelAxisIndex]) {
				parallelAxisIndex = axisIndex;
			}
		}

		float perpendicularSum = 0.0f;
		for (size_t axisIndex = 0; axisIndex < eigenvalues.size(); ++axisIndex) {
			if (axisIndex != parallelAxisIndex) {
				perpendicularSum += eigenvalues[axisIndex];
			}
		}

		AxialSGGX axial{};
		axial.axis = CanonicalizeAxialAxis(axes[parallelAxisIndex]);
		axial.sigmaPerp = std::sqrt(std::max(perpendicularSum * 0.5f, 1.0e-8f));
		axial.sigmaParallel = std::sqrt(std::max(eigenvalues[parallelAxisIndex], 1.0e-8f));
		return axial;
	}

	[[nodiscard]] inline DirectX::XMFLOAT4 EncodeAxialSGGX(const AxialSGGX& axial)
	{
		const DirectX::XMFLOAT2 oct = EncodeOctahedralAxis(axial.axis);
		return DirectX::XMFLOAT4(oct.x, oct.y, std::max(axial.sigmaPerp, 1.0e-4f), std::max(axial.sigmaParallel, 1.0e-4f));
	}

	[[nodiscard]] inline SymmetricMatrix3 DecodeAxialSGGX(const DirectX::XMFLOAT4& packed)
	{
		AxialSGGX axial{};
		axial.axis = DecodeOctahedralAxis(packed.x, packed.y);
		axial.sigmaPerp = packed.z;
		axial.sigmaParallel = packed.w;
		return SGGXFromAxial(axial);
	}

	[[nodiscard]] inline SymmetricMatrix3 BuildSGGXFromNormals(const std::vector<Float3>& normals)
	{
		if (normals.empty()) {
			return SGGXFromNormal(Float3(0.0f, 0.0f, 1.0f));
		}

		SymmetricMatrix3 moment{};
		for (const Float3& sampleNormal : normals) {
			moment = moment + Outer(SafeNormalizeNormal(sampleNormal));
		}
		const float invCount = 1.0f / static_cast<float>(normals.size());
		moment = moment * invCount;

		const std::array<Float3, 3> axes = EigenvectorsSymmetric(moment);
		SymmetricMatrix3 sggx{};
		constexpr float kMinSigma = 1.0e-4f;
		for (const Float3& axis : axes) {
			float sigma = 0.0f;
			for (const Float3& sampleNormal : normals) {
				sigma += std::abs(axis.dot(SafeNormalizeNormal(sampleNormal)));
			}
			sigma = std::max(kMinSigma, 0.5f * sigma * invCount);
			sggx = sggx + Outer(axis, sigma * sigma);
		}
		return sggx;
	}

	[[nodiscard]] inline DirectX::XMFLOAT4 EncodeSGGXFromNormals(const std::vector<Float3>& normals)
	{
		return EncodeAxialSGGX(CompressSGGXToAxial(BuildSGGXFromNormals(normals)));
	}
}
