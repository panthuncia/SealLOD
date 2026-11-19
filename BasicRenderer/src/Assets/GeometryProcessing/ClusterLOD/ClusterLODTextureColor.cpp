#include <BasicRenderer/Assets/ClusterLODTextureColor.h>

#include <cmath>

namespace clod_texture {

	namespace {

		const std::array<float, 256>& SrgbToLinearTable()
		{
			static const std::array<float, 256> table = [] {
				std::array<float, 256> values{};
				for (int i = 0; i < 256; ++i)
				{
					const double c = i / 255.0;
					values[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
				}
				return values;
			}();
			return table;
		}

		uint8_t LinearToSrgb8(double linear)
		{
			const double c = std::clamp(linear, 0.0, 1.0);
			const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
			return static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
		}

		std::array<float, 4> Bilinear(const Texture& texture, uint32_t level, double u, double v)
		{
			const auto& toLinear = SrgbToLinearTable();
			const uint32_t w = texture.MipWidth(level), h = texture.MipHeight(level);
			const std::vector<uint8_t>& texels = texture.mips[level];
			const double x = u * w - 0.5, y = v * h - 0.5;
			const double fx = std::floor(x), fy = std::floor(y);
			const double tx = x - fx, ty = y - fy;
			const auto wrap = [](int64_t i, uint32_t n) {
				const int64_t m = i % static_cast<int64_t>(n);
				return static_cast<uint32_t>(m < 0 ? m + n : m);
			};
			const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
			std::array<float, 4> out{};
			for (int dy = 0; dy < 2; ++dy)
			{
				for (int dx = 0; dx < 2; ++dx)
				{
					const double weight = (dx ? tx : 1.0 - tx) * (dy ? ty : 1.0 - ty);
					const uint8_t* p = &texels[(static_cast<size_t>(wrap(iy + dy, h)) * w + wrap(ix + dx, w)) * 4u];
					for (int c = 0; c < 3; ++c)
					{
						out[c] += static_cast<float>(weight * toLinear[p[c]]);
					}
					out[3] += static_cast<float>(weight * p[3] / 255.0);
				}
			}
			return out;
		}

	}

	void BuildMips(Texture& texture)
	{
		const auto& toLinear = SrgbToLinearTable();
		if (texture.mips.empty())
		{
			return;
		}
		while (texture.mips.back().size() > 4u)
		{
			const uint32_t level = texture.Levels() - 1u;
			const uint32_t w = texture.MipWidth(level), h = texture.MipHeight(level);
			const uint32_t nw = std::max(w / 2u, 1u), nh = std::max(h / 2u, 1u);
			const std::vector<uint8_t>& src = texture.mips.back();
			std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4u);
			for (uint32_t y = 0; y < nh; ++y)
			{
				for (uint32_t x = 0; x < nw; ++x)
				{
					double sum[4] = {};
					for (uint32_t dy = 0; dy < 2u; ++dy)
					{
						for (uint32_t dx = 0; dx < 2u; ++dx)
						{
							const uint32_t sx = std::min(x * 2u + dx, w - 1u), sy = std::min(y * 2u + dy, h - 1u);
							const uint8_t* p = &src[(static_cast<size_t>(sy) * w + sx) * 4u];
							for (int c = 0; c < 3; ++c)
							{
								sum[c] += toLinear[p[c]];
							}
							sum[3] += p[3] / 255.0;
						}
					}
					uint8_t* q = &dst[(static_cast<size_t>(y) * nw + x) * 4u];
					for (int c = 0; c < 3; ++c)
					{
						q[c] = LinearToSrgb8(sum[c] * 0.25);
					}
					q[3] = static_cast<uint8_t>(std::lround(std::clamp(sum[3] * 0.25, 0.0, 1.0) * 255.0));
				}
			}
			texture.mips.push_back(std::move(dst));
		}
	}

	double TextureLod(const Texture& texture, double footprintUv)
	{
		const double texels = footprintUv * static_cast<double>(std::max(texture.width, texture.height));
		return texels > 1.0 ? std::log2(texels) : 0.0;
	}

	std::array<float, 4> SampleLinear(const Texture& texture, double u, double v, double lod)
	{
		const double maxLevel = static_cast<double>(texture.Levels() - 1u);
		const double level = std::clamp(std::isfinite(lod) ? lod : 0.0, 0.0, maxLevel);
		const uint32_t low = static_cast<uint32_t>(std::floor(level));
		const double t = level - low;
		std::array<float, 4> a = Bilinear(texture, low, u, v);
		if (t > 0.0 && low + 1u < texture.Levels())
		{
			const std::array<float, 4> b = Bilinear(texture, low + 1u, u, v);
			for (int c = 0; c < 4; ++c)
			{
				a[c] = static_cast<float>(a[c] * (1.0 - t) + b[c] * t);
			}
		}
		return a;
	}

	std::array<double, 3> LinearToLab(const std::array<float, 4>& linear)
	{
		const double r = linear[0], g = linear[1], b = linear[2];
		const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.9505;
		const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.089;
		const auto f = [](double t) { return t > 0.008856 ? std::cbrt(std::max(t, 0.0)) : 7.787 * t + 16.0 / 116.0; };
		const double fx = f(x), fy = f(y), fz = f(z);
		return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
	}

	double DeltaE(const std::array<float, 4>& a, const std::array<float, 4>& b)
	{
		const std::array<double, 3> la = LinearToLab(a), lb = LinearToLab(b);
		const double d0 = la[0] - lb[0], d1 = la[1] - lb[1], d2 = la[2] - lb[2];
		return std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
	}

}
