#pragma once

// The polynomial lens: a camera ray's backward transport from the rear plane to the front plane as one polynomial per
// output, fitted by least squares to exact traces (FitPolynomial; `LensBake fitpoly`). Inputs x = (r, dx, dy, lambda_nm)
// in the canonical frame (LensDataset.h); outputs y = (px, py, dx, dy, dz, throughput) in the same frame.
//
// Evaluation (EvalPolynomial; the HLSL that LensManager generates follows the same order):
//   v = (r, dx, dy, w * w) with w = 1000 / lambda_nm      (lambda as 1/um^2: dispersion is about linear in it)
//   u[i] = (v[i] - center[i]) * invScale[i]               (center[2] is 0, so u[2] keeps dy's sign)
//   pw[i][0] = 1, pw[i][e] = pw[i][e - 1] * u[i]
//   m[k] = ((pw[0][e0] * pw[1][e1]) * pw[2][e2]) * pw[3][e3]   with (e0, e1, e2, e3) = exponents[k]
//   Terms [0, evenTerms) have e2 even, [evenTerms, TermCount()) e2 odd (mirror symmetry: py and dy are odd in the input
//   dy, everything else even). Even outputs (px, dx, dz, throughput) sum the even block, odd outputs (py, dy) the odd one:
//   y[o] = sum over the block's terms k, in order, of c[o][k] * m[k], from 0.
//   coefficients holds the blocks back to back in output order: px[E] py[O] dx[E] dy[O] dz[E] tp[E].
// The direction (y[2..4]) is normalized after evaluation (EvalTransport); nothing is clamped.
//
// The float overload keeps every intermediate in float32, as the shader does; the double one widens the coefficients.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lens {

	inline constexpr uint32_t kTransportInputs = 4;  // r, dx, dy, lambda_nm
	inline constexpr uint32_t kTransportOutputs = 6; // px, py, dx, dy, dz, throughput

	struct LensPolynomial {
		uint32_t degree = 0; // total degree: every monomial of degree <= it is a term
		std::array<float, kTransportInputs> center{};
		std::array<float, kTransportInputs> invScale{ 1.0f, 1.0f, 1.0f, 1.0f };
		uint32_t evenTerms = 0;
		std::vector<std::array<uint8_t, kTransportInputs>> exponents; // per term (r, dx, dy, lambda) powers
		std::vector<float> coefficients;                              // px[E] py[O] dx[E] dy[O] dz[E] tp[E]

		uint32_t TermCount() const { return static_cast<uint32_t>(exponents.size()); }
		uint32_t OddTerms() const { return TermCount() - evenTerms; }
		static constexpr bool IsOddOutput(uint32_t output) { return output == 1 || output == 3; }
		size_t CoefficientCount() const { return 4u * static_cast<size_t>(evenTerms) + 2u * OddTerms(); }
		// The exponents are PolynomialExponents(degree), the coefficient count matches, center[2] is 0, all finite.
		bool IsValid() const;
	};

	// Every (e_r, e_dx, e_dy, e_lambda) of total degree <= degree: the terms even in dy first, then the odd ones, each in
	// lexicographic order (the lambda power fastest). Degree 5 has 80 + 46 terms, 7 has 200 + 130.
	void PolynomialExponents(uint32_t degree, std::vector<std::array<uint8_t, kTransportInputs>>& exponents, uint32_t& evenTerms);

	std::array<float, kTransportOutputs> EvalPolynomial(const LensPolynomial& poly, const std::array<float, kTransportInputs>& inputs);
	std::array<double, kTransportOutputs> EvalPolynomial(const LensPolynomial& poly, const std::array<double, kTransportInputs>& inputs);
	// The polynomial with its direction normalized: the transport the renderer uses.
	std::array<float, kTransportOutputs> EvalTransport(const LensPolynomial& poly, const std::array<float, kTransportInputs>& inputs);

	// Least squares on exact rays (inputs [N][4], outputs [N][6]): the scaling from the inputs' range, then per parity block
	// the normal equations in a product-Legendre basis (nearly orthogonal over the scaled range, so well conditioned), in
	// double, equilibrated and solved by Cholesky with one step of iterative refinement, then converted to monomials. The rows are
	// accumulated on `threads` threads (0: hardware concurrency).
	struct PolynomialFitReport {
		uint64_t rows = 0;
		double seconds = 0.0;
		double maxRelativeResidual = 0.0; // the refinement step's largest correction, relative to the coefficients
	};
	bool FitPolynomial(const std::vector<float>& inputs, const std::vector<float>& outputs, uint32_t degree, LensPolynomial& poly, std::string* error,
		uint32_t threads = 0, PolynomialFitReport* report = nullptr);

} // namespace lens
