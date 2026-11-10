#include "LensCore/BrLens.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace lens {

	namespace {
		void SetError(std::string* error, std::string message)
		{
			if (error) {
				*error = std::move(message);
			}
		}

		class Writer {
		public:
			template <typename T>
			void Pod(const T& value)
			{
				const auto* p = reinterpret_cast<const uint8_t*>(&value);
				bytes.insert(bytes.end(), p, p + sizeof(T));
			}
			void Floats(const float* v, size_t n)
			{
				const auto* p = reinterpret_cast<const uint8_t*>(v);
				bytes.insert(bytes.end(), p, p + n * sizeof(float));
			}
			void Str(const std::string& s)
			{
				Pod(static_cast<uint32_t>(s.size()));
				bytes.insert(bytes.end(), s.begin(), s.end());
			}

			std::vector<uint8_t> bytes;
		};

		class Reader {
		public:
			Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

			template <typename T>
			bool Pod(T& value)
			{
				if (!Need(sizeof(T))) {
					return false;
				}
				std::memcpy(&value, data_ + at_, sizeof(T));
				at_ += sizeof(T);
				return true;
			}
			bool Bytes(void* v, size_t n)
			{
				if (!Need(n)) {
					return false;
				}
				std::memcpy(v, data_ + at_, n);
				at_ += n;
				return true;
			}
			bool Str(std::string& s)
			{
				uint32_t n = 0;
				if (!Pod(n) || !Need(n)) {
					return false;
				}
				s.assign(reinterpret_cast<const char*>(data_ + at_), n);
				at_ += n;
				return true;
			}
			size_t Position() const { return at_; }
			bool ok = true;

		private:
			bool Need(size_t n)
			{
				if (!ok || n > size_ - at_) {
					ok = false;
					return false;
				}
				return true;
			}
			const uint8_t* data_;
			size_t size_;
			size_t at_ = 0;
		};

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return false;
			}
			const std::streamoff size = file.tellg();
			bytes.resize(static_cast<size_t>(std::max<std::streamoff>(size, 0)));
			file.seekg(0);
			return bytes.empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
		}

		double* DomainField(SamplingDomain& d, size_t index)
		{
			double* fields[] = { &d.inputPlaneZ, &d.outputPlaneZ, &d.lambdaMinNm, &d.lambdaMaxNm, &d.sensorRadius, &d.sensorZMin, &d.sensorZMax,
				&d.rearHalfExtent };
			return fields[index];
		}
		constexpr size_t kDomainFields = 8;
	} // namespace

	uint32_t Crc32(const uint8_t* bytes, size_t size)
	{
		static const std::array<uint32_t, 256> table = [] {
			std::array<uint32_t, 256> t{};
			for (uint32_t i = 0; i < 256; ++i) {
				uint32_t c = i;
				for (int k = 0; k < 8; ++k) {
					c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				}
				t[i] = c;
			}
			return t;
		}();
		uint32_t crc = ~0u;
		for (size_t i = 0; i < size; ++i) {
			crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
		}
		return ~crc;
	}

	const LensPolynomial* BrLensFile::Find(uint32_t degree) const
	{
		for (const LensPolynomial& poly : polynomials) {
			if (poly.degree == degree) {
				return &poly;
			}
		}
		return nullptr;
	}

	std::vector<uint8_t> EncodeBrLens(const BrLensFile& file)
	{
		Writer w;
		w.bytes.insert(w.bytes.end(), kBrLensMagic, kBrLensMagic + 8);
		w.Pod(kBrLensVersion);
		w.Pod(file.sourceHash);
		w.Str(file.lensName);
		SamplingDomain domain = file.domain;
		for (size_t index = 0; index < kDomainFields; ++index) {
			w.Pod(*DomainField(domain, index));
		}
		w.Pod(static_cast<uint32_t>(file.polynomials.size()));
		for (const LensPolynomial& poly : file.polynomials) {
			w.Pod(poly.degree);
			w.Pod(poly.TermCount());
			w.Pod(poly.evenTerms);
			w.Floats(poly.center.data(), poly.center.size());
			w.Floats(poly.invScale.data(), poly.invScale.size());
			for (const auto& e : poly.exponents) {
				w.bytes.insert(w.bytes.end(), e.begin(), e.end());
			}
			w.Floats(poly.coefficients.data(), poly.coefficients.size());
		}
		w.Str(file.metadataJson);
		w.Pod(Crc32(w.bytes.data(), w.bytes.size()));
		return std::move(w.bytes);
	}

	std::optional<BrLensFile> DecodeBrLens(const std::vector<uint8_t>& bytes, const std::optional<uint64_t>& expectedHash, std::string* error)
	{
		if (bytes.size() < 8 + 4 + 8 + 4 || std::memcmp(bytes.data(), kBrLensMagic, 8) != 0) {
			SetError(error, "not a .brlens file (bad magic)");
			return std::nullopt;
		}
		Reader r(bytes.data() + 8, bytes.size() - 8);
		BrLensFile file;
		r.Pod(file.version);
		if (file.version != kBrLensVersion) {
			SetError(error, "a version " + std::to_string(file.version) + " .brlens; only version " + std::to_string(kBrLensVersion)
				+ " (polynomial) is read. Versions 1 and 2 held the retired neural networks: refit it with `LensBake fitpoly`");
			return std::nullopt;
		}
		uint32_t storedCrc = 0;
		std::memcpy(&storedCrc, bytes.data() + bytes.size() - 4, 4);
		if (Crc32(bytes.data(), bytes.size() - 4) != storedCrc) {
			SetError(error, ".brlens checksum mismatch (truncated or corrupted)");
			return std::nullopt;
		}
		r.Pod(file.sourceHash);
		if (expectedHash && *expectedHash != file.sourceHash) {
			SetError(error, ".brlens was fitted to prescription " + HashHex(file.sourceHash) + ", not " + HashHex(*expectedHash) + "; refit it");
			return std::nullopt;
		}
		r.Str(file.lensName);
		file.domain.direction = PathDirection::Backward;
		for (size_t index = 0; index < kDomainFields; ++index) {
			r.Pod(*DomainField(file.domain, index));
		}
		uint32_t count = 0;
		if (!r.Pod(count) || count > 64) {
			SetError(error, "malformed .brlens header");
			return std::nullopt;
		}
		file.polynomials.resize(count);
		for (LensPolynomial& poly : file.polynomials) {
			uint32_t terms = 0;
			if (!r.Pod(poly.degree) || !r.Pod(terms) || !r.Pod(poly.evenTerms) || terms > 1u << 16 || poly.evenTerms > terms) {
				SetError(error, "malformed .brlens polynomial");
				return std::nullopt;
			}
			r.Bytes(poly.center.data(), sizeof(poly.center));
			r.Bytes(poly.invScale.data(), sizeof(poly.invScale));
			poly.exponents.resize(terms);
			for (auto& e : poly.exponents) {
				r.Bytes(e.data(), e.size());
			}
			poly.coefficients.resize(poly.CoefficientCount());
			r.Bytes(poly.coefficients.data(), poly.coefficients.size() * sizeof(float));
			if (!r.ok || !poly.IsValid()) {
				SetError(error, ".brlens polynomial of degree " + std::to_string(poly.degree) + " is inconsistent");
				return std::nullopt;
			}
		}
		r.Str(file.metadataJson);
		if (!r.ok || r.Position() + 8 + 4 != bytes.size()) {
			SetError(error, "malformed .brlens (size mismatch)");
			return std::nullopt;
		}
		return file;
	}

	bool WriteBrLens(const std::filesystem::path& path, const BrLensFile& file, std::string* error)
	{
		const std::vector<uint8_t> bytes = EncodeBrLens(file);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			SetError(error, "cannot write " + path.string());
			return false;
		}
		return true;
	}

	std::optional<BrLensFile> ReadBrLens(const std::filesystem::path& path, const std::optional<uint64_t>& expectedHash, std::string* error)
	{
		std::vector<uint8_t> bytes;
		if (!ReadFileBytes(path, bytes)) {
			SetError(error, "cannot read " + path.string());
			return std::nullopt;
		}
		std::string inner;
		std::optional<BrLensFile> file = DecodeBrLens(bytes, expectedHash, &inner);
		if (!file) {
			SetError(error, path.string() + ": " + inner);
		}
		return file;
	}

} // namespace lens
