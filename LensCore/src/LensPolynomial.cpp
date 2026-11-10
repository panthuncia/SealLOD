#include "LensCore/LensPolynomial.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace lens {

	namespace {

		template <typename T>
		std::array<T, kTransportOutputs> Evaluate(const LensPolynomial& poly, const std::array<T, kTransportInputs>& inputs)
		{
			std::array<T, kTransportOutputs> y{};
			const uint32_t terms = poly.TermCount();
			if (terms == 0) {
				return y;
			}
			const T w = T(1000) / inputs[3];
			const std::array<T, kTransportInputs> v{ inputs[0], inputs[1], inputs[2], w * w };
			const uint32_t degree = poly.degree;
			std::vector<T> pw(static_cast<size_t>(kTransportInputs) * (degree + 1));
			for (uint32_t i = 0; i < kTransportInputs; ++i) {
				const T u = (v[i] - static_cast<T>(poly.center[i])) * static_cast<T>(poly.invScale[i]);
				T* row = pw.data() + static_cast<size_t>(i) * (degree + 1);
				row[0] = T(1);
				for (uint32_t e = 1; e <= degree; ++e) {
					row[e] = row[e - 1] * u;
				}
			}
			std::vector<T> m(terms);
			for (uint32_t k = 0; k < terms; ++k) {
				const auto& e = poly.exponents[k];
				m[k] = ((pw[e[0]] * pw[(degree + 1) + e[1]]) * pw[2 * (degree + 1) + e[2]]) * pw[3 * (degree + 1) + e[3]];
			}
			const float* c = poly.coefficients.data();
			for (uint32_t o = 0; o < kTransportOutputs; ++o) {
				const bool odd = LensPolynomial::IsOddOutput(o);
				const uint32_t first = odd ? poly.evenTerms : 0u;
				const uint32_t count = odd ? poly.OddTerms() : poly.evenTerms;
				T acc = T(0);
				for (uint32_t k = 0; k < count; ++k) {
					acc += static_cast<T>(c[k]) * m[first + k];
				}
				y[o] = acc;
				c += count;
			}
			return y;
		}

		// The polynomial's variables before scaling: (r, dx, dy, (1000 / lambda)^2).
		std::array<double, kTransportInputs> Variables(const float* input)
		{
			const double w = 1000.0 / static_cast<double>(input[3]);
			return { static_cast<double>(input[0]), static_cast<double>(input[1]), static_cast<double>(input[2]), w * w };
		}

		// Legendre polynomials' monomial coefficients: P_n(u) = sum_k legendre[n][k] u^k, n up to 16.
		const std::array<std::array<double, 17>, 17>& LegendreCoefficients()
		{
			static const std::array<std::array<double, 17>, 17> table = [] {
				std::array<std::array<double, 17>, 17> a{};
				a[0][0] = 1.0;
				a[1][1] = 1.0;
				for (int n = 1; n < 16; ++n) {
					// (n + 1) P_{n+1} = (2n + 1) u P_n - n P_{n-1}
					for (int k = 0; k <= n + 1; ++k) {
						const double shifted = k > 0 ? a[n][k - 1] : 0.0;
						a[n + 1][k] = ((2.0 * n + 1.0) * shifted - n * a[n - 1][k]) / (n + 1.0);
					}
				}
				return a;
			}();
			return table;
		}

		// In place: A = L L^T (lower triangle of a row-major n x n matrix). False if not positive definite.
		bool Cholesky(std::vector<double>& a, size_t n)
		{
			for (size_t j = 0; j < n; ++j) {
				double d = a[j * n + j];
				for (size_t k = 0; k < j; ++k) {
					d -= a[j * n + k] * a[j * n + k];
				}
				if (!(d > 0.0)) {
					return false;
				}
				d = std::sqrt(d);
				a[j * n + j] = d;
				for (size_t i = j + 1; i < n; ++i) {
					double s = a[i * n + j];
					for (size_t k = 0; k < j; ++k) {
						s -= a[i * n + k] * a[j * n + k];
					}
					a[i * n + j] = s / d;
				}
			}
			return true;
		}

		// Solves L L^T x = b for each of the columns of b (n x outputs, row-major).
		void CholeskySolve(const std::vector<double>& l, size_t n, std::vector<double>& b, size_t outputs)
		{
			for (size_t o = 0; o < outputs; ++o) {
				for (size_t i = 0; i < n; ++i) {
					double s = b[i * outputs + o];
					for (size_t k = 0; k < i; ++k) {
						s -= l[i * n + k] * b[k * outputs + o];
					}
					b[i * outputs + o] = s / l[i * n + i];
				}
				for (size_t i = n; i-- > 0;) {
					double s = b[i * outputs + o];
					for (size_t k = i + 1; k < n; ++k) {
						s -= l[k * n + i] * b[k * outputs + o];
					}
					b[i * outputs + o] = s / l[i * n + i];
				}
			}
		}

		// The equilibrated normal equations of one parity block, solved, with one refinement step. gram is the full symmetric
		// matrix, rhs n x outputs; the solution replaces rhs. Returns the refinement's largest relative correction, or < 0.
		double SolveNormalEquations(const std::vector<double>& gram, size_t n, std::vector<double>& rhs, size_t outputs)
		{
			std::vector<double> scale(n);
			for (size_t i = 0; i < n; ++i) {
				scale[i] = gram[i * n + i] > 0.0 ? 1.0 / std::sqrt(gram[i * n + i]) : 1.0;
			}
			std::vector<double> scaled(n * n);
			for (size_t i = 0; i < n; ++i) {
				for (size_t j = 0; j < n; ++j) {
					scaled[i * n + j] = gram[i * n + j] * scale[i] * scale[j];
				}
			}
			std::vector<double> factor = scaled;
			if (!Cholesky(factor, n)) {
				return -1.0;
			}
			std::vector<double> x(n * outputs);
			for (size_t i = 0; i < n; ++i) {
				for (size_t o = 0; o < outputs; ++o) {
					x[i * outputs + o] = rhs[i * outputs + o] * scale[i];
				}
			}
			const std::vector<double> b = x;
			CholeskySolve(factor, n, x, outputs);
			// One step of iterative refinement on the scaled system.
			std::vector<double> residual(n * outputs);
			for (size_t i = 0; i < n; ++i) {
				for (size_t o = 0; o < outputs; ++o) {
					double s = b[i * outputs + o];
					for (size_t k = 0; k < n; ++k) {
						s -= scaled[i * n + k] * x[k * outputs + o];
					}
					residual[i * outputs + o] = s;
				}
			}
			CholeskySolve(factor, n, residual, outputs);
			double largest = 0.0, norm = 0.0;
			for (size_t i = 0; i < n * outputs; ++i) {
				x[i] += residual[i];
				largest = std::max(largest, std::abs(residual[i]));
				norm = std::max(norm, std::abs(x[i]));
			}
			for (size_t i = 0; i < n; ++i) {
				for (size_t o = 0; o < outputs; ++o) {
					rhs[i * outputs + o] = x[i * outputs + o] * scale[i];
				}
			}
			return norm > 0.0 ? largest / norm : 0.0;
		}

	} // namespace

	void PolynomialExponents(uint32_t degree, std::vector<std::array<uint8_t, kTransportInputs>>& exponents, uint32_t& evenTerms)
	{
		std::vector<std::array<uint8_t, kTransportInputs>> even, odd;
		for (uint32_t a = 0; a <= degree; ++a) {
			for (uint32_t b = 0; a + b <= degree; ++b) {
				for (uint32_t c = 0; a + b + c <= degree; ++c) {
					for (uint32_t d = 0; a + b + c + d <= degree; ++d) {
						const std::array<uint8_t, kTransportInputs> e{ uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d) };
						(c % 2 ? odd : even).push_back(e);
					}
				}
			}
		}
		evenTerms = static_cast<uint32_t>(even.size());
		exponents = std::move(even);
		exponents.insert(exponents.end(), odd.begin(), odd.end());
	}

	bool LensPolynomial::IsValid() const
	{
		if (degree == 0 || degree > 16) {
			return false;
		}
		std::vector<std::array<uint8_t, kTransportInputs>> expected;
		uint32_t expectedEven = 0;
		PolynomialExponents(degree, expected, expectedEven);
		if (exponents != expected || evenTerms != expectedEven || coefficients.size() != CoefficientCount() || center[2] != 0.0f) {
			return false;
		}
		for (uint32_t i = 0; i < kTransportInputs; ++i) {
			if (!std::isfinite(center[i]) || !std::isfinite(invScale[i])) {
				return false;
			}
		}
		return std::all_of(coefficients.begin(), coefficients.end(), [](float c) { return std::isfinite(c); });
	}

	std::array<float, kTransportOutputs> EvalPolynomial(const LensPolynomial& poly, const std::array<float, kTransportInputs>& inputs)
	{
		return Evaluate(poly, inputs);
	}

	std::array<double, kTransportOutputs> EvalPolynomial(const LensPolynomial& poly, const std::array<double, kTransportInputs>& inputs)
	{
		return Evaluate(poly, inputs);
	}

	std::array<float, kTransportOutputs> EvalTransport(const LensPolynomial& poly, const std::array<float, kTransportInputs>& inputs)
	{
		std::array<float, kTransportOutputs> y = Evaluate(poly, inputs);
		const float length = std::sqrt(y[2] * y[2] + y[3] * y[3] + y[4] * y[4]);
		y[2] /= length;
		y[3] /= length;
		y[4] /= length;
		return y;
	}

	bool FitPolynomial(const std::vector<float>& inputs, const std::vector<float>& outputs, uint32_t degree, LensPolynomial& poly, std::string* error,
		uint32_t threads, PolynomialFitReport* report)
	{
		const auto started = std::chrono::steady_clock::now();
		const auto fail = [error](const char* message) {
			if (error) {
				*error = message;
			}
			return false;
		};
		const size_t rows = inputs.size() / kTransportInputs;
		if (degree == 0 || degree > 16 || inputs.size() != rows * kTransportInputs || outputs.size() != rows * kTransportOutputs) {
			return fail("FitPolynomial: inconsistent inputs");
		}
		poly = {};
		poly.degree = degree;
		PolynomialExponents(degree, poly.exponents, poly.evenTerms);
		const size_t terms = poly.TermCount(), even = poly.evenTerms, odd = poly.OddTerms();
		if (rows < terms * 4) {
			return fail("FitPolynomial: too few rows for the terms");
		}

		// The scaling: each variable's range onto [-1, 1]; dy keeps its sign (centre 0) so the parity blocks are exact. The
		// float values are what the evaluation uses, so the fit uses them too.
		std::array<double, kTransportInputs> lo, hi;
		lo.fill(std::numeric_limits<double>::infinity());
		hi.fill(-std::numeric_limits<double>::infinity());
		double maxDy = 0.0;
		for (size_t row = 0; row < rows; ++row) {
			const auto v = Variables(&inputs[row * kTransportInputs]);
			for (uint32_t i = 0; i < kTransportInputs; ++i) {
				lo[i] = std::min(lo[i], v[i]);
				hi[i] = std::max(hi[i], v[i]);
			}
			maxDy = std::max(maxDy, std::abs(v[2]));
		}
		for (uint32_t i = 0; i < kTransportInputs; ++i) {
			poly.center[i] = static_cast<float>(0.5 * (lo[i] + hi[i]));
			poly.invScale[i] = static_cast<float>(2.0 / std::max(hi[i] - lo[i], 1e-12));
		}
		poly.center[2] = 0.0f;
		poly.invScale[2] = static_cast<float>(1.0 / std::max(maxDy, 1e-12));

		// Normal equations per block, accumulated per thread over row chunks.
		const uint32_t evenOut[4] = { 0, 2, 4, 5 }, oddOut[2] = { 1, 3 };
		const uint32_t workerCount = std::max(1u, threads ? threads : std::thread::hardware_concurrency());
		struct Partial {
			std::vector<double> gramEven, gramOdd, rhsEven, rhsOdd;
		};
		std::vector<Partial> partials(workerCount);
		constexpr size_t kChunk = 4096;
		const size_t chunks = (rows + kChunk - 1) / kChunk;
		std::atomic<size_t> next{ 0 };
		const auto work = [&](uint32_t worker) {
			Partial& p = partials[worker];
			p.gramEven.assign(even * even, 0.0);
			p.gramOdd.assign(odd * odd, 0.0);
			p.rhsEven.assign(even * 4, 0.0);
			p.rhsOdd.assign(odd * 2, 0.0);
			std::vector<double> pw(kTransportInputs * (degree + 1));
			std::vector<double> m(terms);
			for (size_t chunk = next.fetch_add(1); chunk < chunks; chunk = next.fetch_add(1)) {
				const size_t end = std::min(rows, (chunk + 1) * kChunk);
				for (size_t row = chunk * kChunk; row < end; ++row) {
					const auto v = Variables(&inputs[row * kTransportInputs]);
					for (uint32_t i = 0; i < kTransportInputs; ++i) {
						const double u = (v[i] - static_cast<double>(poly.center[i])) * static_cast<double>(poly.invScale[i]);
						// Legendre values: the basis the normal equations are formed in is nearly orthogonal over the scaled
						// range, so they are well conditioned; the coefficients are converted to monomials after the solve.
						double* values = pw.data() + i * (degree + 1);
						values[0] = 1.0;
						if (degree >= 1) values[1] = u;
						for (uint32_t e = 1; e < degree; ++e) {
							values[e + 1] = ((2.0 * e + 1.0) * u * values[e] - e * values[e - 1]) / (e + 1.0);
						}
					}
					for (size_t k = 0; k < terms; ++k) {
						const auto& e = poly.exponents[k];
						m[k] = ((pw[e[0]] * pw[(degree + 1) + e[1]]) * pw[2 * (degree + 1) + e[2]]) * pw[3 * (degree + 1) + e[3]];
					}
					const float* y = &outputs[row * kTransportOutputs];
					for (size_t i = 0; i < even; ++i) {
						const double mi = m[i];
						double* g = &p.gramEven[i * even];
						for (size_t j = i; j < even; ++j) {
							g[j] += mi * m[j];
						}
						for (uint32_t o = 0; o < 4; ++o) {
							p.rhsEven[i * 4 + o] += mi * y[evenOut[o]];
						}
					}
					for (size_t i = 0; i < odd; ++i) {
						const double mi = m[even + i];
						double* g = &p.gramOdd[i * odd];
						for (size_t j = i; j < odd; ++j) {
							g[j] += mi * m[even + j];
						}
						for (uint32_t o = 0; o < 2; ++o) {
							p.rhsOdd[i * 2 + o] += mi * y[oddOut[o]];
						}
					}
				}
			}
		};
		{
			std::vector<std::thread> pool;
			for (uint32_t worker = 1; worker < workerCount; ++worker) {
				pool.emplace_back(work, worker);
			}
			work(0);
			for (auto& thread : pool) {
				thread.join();
			}
		}
		Partial total = std::move(partials[0]);
		for (uint32_t worker = 1; worker < workerCount; ++worker) {
			for (size_t i = 0; i < total.gramEven.size(); ++i) total.gramEven[i] += partials[worker].gramEven[i];
			for (size_t i = 0; i < total.gramOdd.size(); ++i) total.gramOdd[i] += partials[worker].gramOdd[i];
			for (size_t i = 0; i < total.rhsEven.size(); ++i) total.rhsEven[i] += partials[worker].rhsEven[i];
			for (size_t i = 0; i < total.rhsOdd.size(); ++i) total.rhsOdd[i] += partials[worker].rhsOdd[i];
		}
		const auto symmetrize = [](std::vector<double>& g, size_t n) {
			for (size_t i = 0; i < n; ++i) {
				for (size_t j = 0; j < i; ++j) {
					g[i * n + j] = g[j * n + i];
				}
			}
		};
		symmetrize(total.gramEven, even);
		symmetrize(total.gramOdd, odd);
		const double refineEven = SolveNormalEquations(total.gramEven, even, total.rhsEven, 4);
		const double refineOdd = SolveNormalEquations(total.gramOdd, odd, total.rhsOdd, 2);
		if (refineEven < 0.0 || refineOdd < 0.0) {
			return fail("FitPolynomial: the normal equations are not positive definite (too few or degenerate rows)");
		}
		// Legendre coefficients to monomial ones: P_n0(u0) P_n1(u1) P_n2(u2) P_n3(u3) expands into the monomials u^e with
		// e <= n componentwise and the same parities, which are terms of the same block (degree and dy parity can only drop).
		const auto& legendre = LegendreCoefficients();
		const auto toMonomials = [&](size_t first, size_t count, std::vector<double>& coefficients, size_t outputs) {
			std::vector<double> monomial(count * outputs, 0.0);
			for (size_t k = 0; k < count; ++k) {
				const auto& n = poly.exponents[first + k];
				for (size_t j = 0; j < count; ++j) {
					const auto& e = poly.exponents[first + j];
					double factor = 1.0;
					for (uint32_t i = 0; i < kTransportInputs && factor != 0.0; ++i) {
						factor = e[i] > n[i] ? 0.0 : factor * legendre[n[i]][e[i]];
					}
					if (factor == 0.0) {
						continue;
					}
					for (size_t o = 0; o < outputs; ++o) {
						monomial[j * outputs + o] += factor * coefficients[k * outputs + o];
					}
				}
			}
			coefficients = std::move(monomial);
		};
		toMonomials(0, even, total.rhsEven, 4);
		toMonomials(even, odd, total.rhsOdd, 2);
		poly.coefficients.clear();
		for (uint32_t o = 0; o < kTransportOutputs; ++o) {
			if (LensPolynomial::IsOddOutput(o)) {
				const uint32_t column = o == 1 ? 0u : 1u;
				for (size_t k = 0; k < odd; ++k) poly.coefficients.push_back(static_cast<float>(total.rhsOdd[k * 2 + column]));
			} else {
				const uint32_t column = o == 0 ? 0u : o == 2 ? 1u : o == 4 ? 2u : 3u;
				for (size_t k = 0; k < even; ++k) poly.coefficients.push_back(static_cast<float>(total.rhsEven[k * 4 + column]));
			}
		}
		if (report) {
			report->rows = rows;
			report->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			report->maxRelativeResidual = std::max(refineEven, refineOdd);
		}
		return poly.IsValid() ? true : fail("FitPolynomial: the fit is not finite");
	}

} // namespace lens
