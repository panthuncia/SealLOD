#include "Hair/GroomSource.h"
#include "Hair/HairFileReader.h"
#include "Hair/HairGPUTypes.h"
#include "Hair/ProceduralGroom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace br::hair;

int RunGroomBuilderTests(); // GroomBuilderTests.cpp; returns its failed check count
int RunAlembicGroomTests(); // AlembicGroomTests.cpp; likewise

namespace {

    int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << "(" << __LINE__ << "): check failed: " #condition "\n"; \
            ++g_failures; \
        } \
    } while (false)

    bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

    // Largest distance of any strand point from the line through the root along `normal`.
    float MaxLateralDeviation(const GroomSource& groom, std::uint32_t strand, Float3 normal)
    {
        const std::uint32_t begin = groom.StrandPointOffset(strand);
        float deviation = 0.0f;
        for (std::uint32_t i = 1; i < groom.StrandPointCount(strand); ++i) {
            const Float3 offset = groom.positions[begin + i] - groom.positions[begin];
            deviation = std::max(deviation, Length(offset - normal * Dot(offset, normal)));
        }
        return deviation;
    }

    void TestSphereCapGenerator()
    {
        for (const ProceduralHairStyle style : { ProceduralHairStyle::Straight, ProceduralHairStyle::Wavy, ProceduralHairStyle::Curly }) {
            ProceduralGroomSettings settings;
            settings.strandCount = 500u;
            settings.pointsPerStrand = 33u;
            settings.style = style;
            settings.seed = 7u;
            settings.capCenter = { 1.0f, 2.0f, -3.0f };
            settings.capAxis = { 0.0f, 3.0f, 4.0f }; // not unit length on purpose
            settings.capAngleRadians = 1.0f;
            const GroomSource groom = GenerateGroomOnSphereCap(settings);

            CHECK(ValidateGroomSource(groom).empty());
            CHECK(groom.StrandCount() == 500u);
            CHECK(groom.PointCount() == 500u * 33u);
            CHECK(groom.widths.size() == groom.positions.size());
            CHECK(groom.colors.empty() && groom.guideFlags.empty() && groom.closestGuides.empty());

            const Float3 axis = Normalize(settings.capAxis);
            const float cosCap = std::cos(settings.capAngleRadians);
            bool rootsOnCap = true, outward = true, counts = true, shape = true, widths = true;
            float minCos = 1.0f;
            for (std::uint32_t strand = 0; strand < groom.StrandCount(); ++strand) {
                counts = counts && groom.StrandPointCount(strand) == 33u;
                const std::uint32_t begin = groom.StrandPointOffset(strand);
                const Float3 fromCenter = groom.positions[begin] - settings.capCenter;
                const Float3 normal = Normalize(fromCenter);
                rootsOnCap = rootsOnCap && Near(Length(fromCenter), settings.capRadius, 1.0e-5f) && Dot(normal, axis) >= cosCap - 1.0e-4f;
                minCos = std::min(minCos, Dot(normal, axis));

                // Leaves along the normal, and every later point is outside the sphere and further out than the last.
                float previousHeight = 0.0f;
                for (std::uint32_t i = 1; i < 33u; ++i) {
                    const Float3 point = groom.positions[begin + i];
                    const float height = Dot(point - groom.positions[begin], normal);
                    outward = outward && height > previousHeight && Length(point - settings.capCenter) > settings.capRadius;
                    previousHeight = height;
                }
                const float maxLength = settings.strandLength;
                const float minLength = settings.strandLength * (1.0f - settings.lengthVariation);
                shape = shape && previousHeight <= maxLength + 1.0e-5f && previousHeight >= minLength - 1.0e-5f;

                const float deviation = MaxLateralDeviation(groom, strand, normal);
                if (style == ProceduralHairStyle::Straight) {
                    shape = shape && deviation < 1.0e-5f;
                } else {
                    shape = shape && deviation > 0.5f * settings.waveAmplitude && deviation <= settings.waveAmplitude + 1.0e-5f;
                }
                widths = widths && groom.widths[begin] == settings.rootWidth && Near(groom.widths[begin + 32u], settings.tipWidth, 1.0e-9f);
            }
            CHECK(counts);
            CHECK(rootsOnCap);
            CHECK(outward);
            CHECK(shape);
            CHECK(widths);
            CHECK(minCos < cosCap + 0.05f); // roots reach the rim of the cap, not just its pole
        }
    }

    void TestGeneratorDeterminism()
    {
        ProceduralGroomSettings settings;
        settings.strandCount = 200u;
        settings.pointsPerStrand = 9u;
        settings.style = ProceduralHairStyle::Curly;
        const GroomSource a = GenerateGroomOnSphereCap(settings);
        const GroomSource b = GenerateGroomOnSphereCap(settings);
        CHECK(a.positions == b.positions && a.widths == b.widths && a.strandPointOffsets == b.strandPointOffsets);

        ProceduralGroomSettings other = settings;
        other.seed = settings.seed + 1u;
        CHECK(!(GenerateGroomOnSphereCap(other).positions == a.positions));

        // Strand i depends only on (seed, i): a larger groom starts with the smaller one.
        ProceduralGroomSettings larger = settings;
        larger.strandCount = 400u;
        const GroomSource c = GenerateGroomOnSphereCap(larger);
        CHECK(c.StrandCount() == 400u);
        CHECK(std::equal(a.positions.begin(), a.positions.end(), c.positions.begin()));

        settings.pointsPerStrand = 1u;
        CHECK(GenerateGroomOnSphereCap(settings).StrandCount() == 0u);
        settings.pointsPerStrand = 9u;
        settings.strandCount = 0u;
        CHECK(GenerateGroomOnSphereCap(settings).StrandCount() == 0u);
    }

    // Gravity droop: off by default, and when on the strands hang from the same roots, keep their length and stay
    // outside the sphere (or on the outer side of their face).
    void TestGeneratorDroop()
    {
        ProceduralGroomSettings settings;
        settings.strandCount = 600u;
        settings.pointsPerStrand = 17u;
        settings.capRadius = 0.5f;
        settings.capCenter = { 0.25f, 1.0f, -0.5f };
        settings.strandLength = 0.6f;
        for (const ProceduralHairStyle style : { ProceduralHairStyle::Straight, ProceduralHairStyle::Curly }) {
            settings.style = style;
            settings.droop = 0.0f;
            const GroomSource straight = GenerateGroomOnSphereCap(settings);
            ProceduralGroomSettings turned = settings;
            turned.droopDirection = { 1.0f, 0.0f, 0.0f }; // the direction means nothing while droop is 0
            CHECK(GenerateGroomOnSphereCap(turned).positions == straight.positions);

            settings.droop = 3.0f;
            const GroomSource drooped = GenerateGroomOnSphereCap(settings);
            CHECK(ValidateGroomSource(drooped).empty());
            CHECK(drooped.strandPointOffsets == straight.strandPointOffsets && drooped.widths == straight.widths);
            CHECK(GenerateGroomOnSphereCap(settings).positions == drooped.positions);

            bool sameRoots = true, outside = true, finite = true, lengthKept = true;
            double straightTipHeight = 0.0, droopedTipHeight = 0.0;
            std::uint32_t hanging = 0u;
            for (std::uint32_t strand = 0; strand < drooped.StrandCount(); ++strand) {
                const std::uint32_t begin = drooped.StrandPointOffset(strand);
                const std::uint32_t count = drooped.StrandPointCount(strand);
                sameRoots = sameRoots && drooped.positions[begin] == straight.positions[begin];
                float length = 0.0f, straightLength = 0.0f;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const Float3 position = drooped.positions[begin + i];
                    finite = finite && std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
                    outside = outside && Length(position - settings.capCenter) >= settings.capRadius * (1.0f - 1.0e-5f);
                    if (i != 0u) {
                        length += Length(position - drooped.positions[begin + i - 1u]);
                        straightLength += Length(straight.positions[begin + i] - straight.positions[begin + i - 1u]);
                    }
                }
                // The straight style keeps its arc length; a curl wound around a bent centreline may not exactly.
                if (style == ProceduralHairStyle::Straight) {
                    lengthKept = lengthKept && length > 0.9f * straightLength && length < 1.1f * straightLength;
                }
                const Float3 tip = drooped.positions[begin + count - 1u];
                straightTipHeight += straight.positions[begin + count - 1u].y;
                droopedTipHeight += tip.y;
                hanging += tip.y < drooped.positions[begin + count - 2u].y ? 1u : 0u; // the last segment points down
            }
            CHECK(sameRoots);
            CHECK(finite);
            CHECK(outside);
            CHECK(lengthKept);
            CHECK(droopedTipHeight < straightTipHeight - 0.2 * drooped.StrandCount()); // tips are 20 cm lower on average
            // All but a strand or two that came to rest on top of the sphere.
            if (style == ProceduralHairStyle::Straight) CHECK(hanging * 100u >= drooped.StrandCount() * 99u);
        }

        // On triangles a drooping strand stays on the outer side of its face: here a floor facing +y, pulled down.
        const std::array<Float3, 3> positions{ Float3{ 0, 0, 0 }, Float3{ 0, 0, 1 }, Float3{ 1, 0, 0 } };
        const std::array<std::uint32_t, 3> indices{ 0, 1, 2 };
        ProceduralGroomSettings floor;
        floor.strandCount = 100u;
        floor.droop = 4.0f;
        const GroomSource lying = GenerateGroomOnTriangles(floor, positions, indices);
        CHECK(lying.StrandCount() == 100u && ValidateGroomSource(lying).empty());
        bool above = true, bent = false;
        for (const Float3& position : lying.positions) {
            above = above && position.y >= 0.0f;
            bent = bent || std::abs(position.x) > 1.0f || position.x < 0.0f || position.z < 0.0f || position.x + position.z > 1.0f;
        }
        CHECK(above);
        CHECK(bent); // some strand left the column above its triangle: it did not just stand up
    }

    void TestTriangleGenerator()
    {
        // Two triangles in z = 0 facing +z, areas 0.5 and 4.5, plus a zero-area triangle that must get no roots.
        const std::array<Float3, 9> positions{
            Float3{ 0, 0, 0 }, Float3{ 1, 0, 0 }, Float3{ 0, 1, 0 },
            Float3{ 10, 0, 0 }, Float3{ 13, 0, 0 }, Float3{ 10, 3, 0 },
            Float3{ 20, 0, 0 }, Float3{ 21, 0, 0 }, Float3{ 22, 0, 0 }
        };
        const std::array<std::uint32_t, 9> indices{ 0, 1, 2, 6, 7, 8, 3, 4, 5 };
        ProceduralGroomSettings settings;
        settings.strandCount = 2000u;
        settings.pointsPerStrand = 5u;
        settings.style = ProceduralHairStyle::Wavy;
        const GroomSource groom = GenerateGroomOnTriangles(settings, positions, indices);
        CHECK(ValidateGroomSource(groom).empty());
        CHECK(groom.StrandCount() == 2000u);
        CHECK(groom.PointCount() == 2000u * 5u);

        bool onSurface = true, outward = true;
        std::uint32_t inLarge = 0u;
        for (std::uint32_t strand = 0; strand < groom.StrandCount(); ++strand) {
            const std::uint32_t begin = groom.StrandPointOffset(strand);
            const Float3 root = groom.positions[begin];
            const bool inSmallTriangle = root.x >= 0.0f && root.y >= 0.0f && root.x + root.y <= 1.0f + 1.0e-5f;
            const bool inLargeTriangle = root.x >= 10.0f && root.y >= 0.0f && (root.x - 10.0f) + root.y <= 3.0f + 1.0e-5f;
            onSurface = onSurface && root.z == 0.0f && (inSmallTriangle || inLargeTriangle);
            inLarge += inLargeTriangle ? 1u : 0u;
            for (std::uint32_t i = 1; i < 5u; ++i) {
                outward = outward && groom.positions[begin + i].z > groom.positions[begin + i - 1u].z;
            }
        }
        CHECK(onSurface);
        CHECK(outward);
        CHECK(inLarge > 1740u && inLarge < 1860u); // 90% of the area, +-3%

        const GroomSource again = GenerateGroomOnTriangles(settings, positions, indices);
        CHECK(again.positions == groom.positions);

        const std::array<std::uint32_t, 3> outOfRange{ 0, 1, 9 };
        CHECK(GenerateGroomOnTriangles(settings, positions, outOfRange).StrandCount() == 0u);
        const std::array<std::uint32_t, 4> notTriangles{ 0, 1, 2, 3 };
        CHECK(GenerateGroomOnTriangles(settings, positions, notTriangles).StrandCount() == 0u);
        const std::array<std::uint32_t, 3> noArea{ 6, 7, 8 };
        CHECK(GenerateGroomOnTriangles(settings, positions, noArea).StrandCount() == 0u);
    }

    // Synthetic .hair file contents, written exactly as the format lays them out.
    struct HairFile {
        std::uint32_t strandCount = 0u;
        std::uint32_t pointCount = 0u;
        std::uint32_t flags = 0u;
        std::uint32_t defaultSegments = 0u;
        float defaultThickness = 0.0f;
        float defaultTransparency = 0.0f;
        Float3 defaultColor{ 1.0f, 1.0f, 1.0f };
        std::vector<std::uint16_t> segments;
        std::vector<Float3> points;
        std::vector<float> thickness;
        std::vector<float> transparency;
        std::vector<Float3> colors;
        std::array<char, 4> signature{ 'H', 'A', 'I', 'R' };

        std::vector<char> Bytes() const
        {
            std::vector<char> bytes(128u, 0);
            std::memcpy(bytes.data(), signature.data(), 4u);
            std::memcpy(bytes.data() + 4u, &strandCount, 4u);
            std::memcpy(bytes.data() + 8u, &pointCount, 4u);
            std::memcpy(bytes.data() + 12u, &flags, 4u);
            std::memcpy(bytes.data() + 16u, &defaultSegments, 4u);
            std::memcpy(bytes.data() + 20u, &defaultThickness, 4u);
            std::memcpy(bytes.data() + 24u, &defaultTransparency, 4u);
            std::memcpy(bytes.data() + 28u, &defaultColor, 12u);
            std::memcpy(bytes.data() + 40u, "synthetic test file", 19u);
            const auto append = [&bytes](const void* data, std::size_t size) {
                const char* begin = static_cast<const char*>(data);
                bytes.insert(bytes.end(), begin, begin + size);
            };
            append(segments.data(), segments.size() * 2u);
            append(points.data(), points.size() * 12u);
            append(thickness.data(), thickness.size() * 4u);
            append(transparency.data(), transparency.size() * 4u);
            append(colors.data(), colors.size() * 12u);
            return bytes;
        }
    };

    const std::filesystem::path& TestFilePath()
    {
        static const std::filesystem::path path = std::filesystem::temp_directory_path() / "br_hair_core_tests.hair";
        return path;
    }

    void WriteTestFile(const std::vector<char>& bytes)
    {
        std::ofstream file(TestFilePath(), std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // Three strands of 2, 3 and 1 segments with every optional array present.
    HairFile MakeFullFile()
    {
        HairFile file;
        file.strandCount = 3u;
        file.pointCount = 9u;
        file.flags = 0x1Fu;
        file.defaultSegments = 99u; // ignored: the segments array is present
        file.defaultThickness = 5.0f;
        file.segments = { 2u, 3u, 1u };
        for (std::uint32_t i = 0; i < 9u; ++i) {
            const float f = static_cast<float>(i);
            file.points.push_back({ f, f * 0.5f, -f });
            file.thickness.push_back(0.1f + f);
            file.transparency.push_back(0.25f);
            file.colors.push_back({ f / 9.0f, 0.5f, 1.0f - f / 9.0f });
        }
        return file;
    }

    // Four strands of two segments each, relying on the header defaults.
    HairFile MakeMinimalFile()
    {
        HairFile file;
        file.strandCount = 4u;
        file.pointCount = 12u;
        file.flags = 0x2u;
        file.defaultSegments = 2u;
        file.defaultThickness = 0.75f;
        for (std::uint32_t i = 0; i < 12u; ++i) {
            file.points.push_back({ static_cast<float>(i), 1.0f, 2.0f });
        }
        return file;
    }

    void ExpectRejected(const std::vector<char>& bytes, const char* expectedInError, int line)
    {
        WriteTestFile(bytes);
        GroomSource groom;
        groom.positions.push_back({}); // must be cleared on failure
        std::string error;
        const bool loaded = LoadHairFile(TestFilePath(), groom, &error);
        if (loaded || error.find(expectedInError) == std::string::npos || groom.StrandCount() != 0u || !groom.positions.empty()) {
            std::cerr << __FILE__ << "(" << line << "): expected rejection mentioning \"" << expectedInError << "\", got "
                      << (loaded ? "a successful load" : "\"" + error + "\"") << "\n";
            ++g_failures;
        }
    }

    void TestHairFileReader()
    {
        std::string error;
        {
            const HairFile file = MakeFullFile();
            WriteTestFile(file.Bytes());
            GroomSource groom;
            CHECK(LoadHairFile(TestFilePath(), groom, &error));
            CHECK(error.empty());
            CHECK(ValidateGroomSource(groom).empty());
            CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 3u, 7u, 9u }));
            CHECK(groom.positions == file.points);
            CHECK(groom.widths == file.thickness);
            CHECK(groom.colors == file.colors);
            CHECK(groom.guideFlags.empty() && groom.groupIds.empty() && groom.rootUVs.empty());
        }
        {
            const HairFile file = MakeMinimalFile();
            WriteTestFile(file.Bytes());
            GroomSource groom;
            CHECK(LoadHairFile(TestFilePath(), groom)); // error pointer is optional
            CHECK(ValidateGroomSource(groom).empty());
            CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 3u, 6u, 9u, 12u }));
            CHECK(groom.positions == file.points);
            CHECK(groom.widths == std::vector<float>(12u, 0.75f));
            CHECK(groom.colors.empty());
        }
        {
            // A generated groom written as .hair reads back identically.
            ProceduralGroomSettings settings;
            settings.strandCount = 50u;
            settings.pointsPerStrand = 17u;
            settings.style = ProceduralHairStyle::Curly;
            const GroomSource source = GenerateGroomOnSphereCap(settings);
            HairFile file;
            file.strandCount = source.StrandCount();
            file.pointCount = source.PointCount();
            file.flags = 0x2u | 0x4u;
            file.defaultSegments = 16u;
            file.points = source.positions;
            file.thickness = source.widths;
            WriteTestFile(file.Bytes());
            GroomSource groom;
            CHECK(LoadHairFile(TestFilePath(), groom, &error));
            CHECK(groom.strandPointOffsets == source.strandPointOffsets);
            CHECK(groom.positions == source.positions && groom.widths == source.widths);
        }

        // Rejections.
        {
            GroomSource groom;
            error.clear();
            CHECK(!LoadHairFile(std::filesystem::temp_directory_path() / "br_hair_core_tests_missing.hair", groom, &error));
            CHECK(error.find("cannot open") != std::string::npos);
        }
        ExpectRejected({}, "header", __LINE__);
        {
            std::vector<char> bytes = MakeFullFile().Bytes();
            bytes.resize(127u);
            ExpectRejected(bytes, "header", __LINE__);
        }
        {
            HairFile file = MakeFullFile();
            file.signature = { 'H', 'A', 'I', 'X' };
            ExpectRejected(file.Bytes(), "signature", __LINE__);
        }
        {
            HairFile file = MakeMinimalFile();
            file.flags = 0u;
            ExpectRejected(file.Bytes(), "no points array", __LINE__);
        }
        {
            HairFile file = MakeMinimalFile();
            file.strandCount = 0u;
            ExpectRejected(file.Bytes(), "no strands", __LINE__);
        }
        {
            // Cutting the full file inside each array in turn names that array.
            const std::vector<char> full = MakeFullFile().Bytes();
            const std::size_t segmentsEnd = 128u + 3u * 2u;
            const std::size_t pointsEnd = segmentsEnd + 9u * 12u;
            const std::size_t thicknessEnd = pointsEnd + 9u * 4u;
            const std::size_t transparencyEnd = thicknessEnd + 9u * 4u;
            const std::size_t colorEnd = transparencyEnd + 9u * 12u;
            CHECK(full.size() == colorEnd);
            const std::pair<std::size_t, const char*> cuts[] = {
                { segmentsEnd - 1u, "segments" }, { pointsEnd - 1u, "points" }, { thicknessEnd - 1u, "thickness" },
                { transparencyEnd - 1u, "transparency" }, { colorEnd - 1u, "colour" }, { 128u, "segments" }
            };
            for (const auto& [size, name] : cuts) {
                ExpectRejected(std::vector<char>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size)), name, __LINE__);
            }
        }
        {
            // A point count far beyond the file must fail the size check rather than allocate.
            HairFile file = MakeMinimalFile();
            file.pointCount = 0xFFFFFFFFu;
            ExpectRejected(file.Bytes(), "truncated", __LINE__);
        }
        {
            HairFile file = MakeFullFile();
            file.segments = { 2u, 3u, 2u }; // 10 points, header says 9
            ExpectRejected(file.Bytes(), "add up", __LINE__);
            file.segments = { 2u, 2u, 1u }; // 8 points
            ExpectRejected(file.Bytes(), "add up", __LINE__);
            file.segments = { 2u, 0u, 4u };
            ExpectRejected(file.Bytes(), "no segments", __LINE__);
        }
        {
            HairFile file = MakeMinimalFile();
            file.defaultSegments = 4u; // 4 strands x 5 points != 12
            ExpectRejected(file.Bytes(), "add up", __LINE__);
            file.defaultSegments = 0u;
            ExpectRejected(file.Bytes(), "no segments", __LINE__);
        }
        {
            HairFile file = MakeMinimalFile();
            file.points[5].y = std::numeric_limits<float>::quiet_NaN();
            ExpectRejected(file.Bytes(), "non-finite", __LINE__);
        }

        std::error_code ignored;
        std::filesystem::remove(TestFilePath(), ignored);
    }

    void TestValidateGroomSource()
    {
        ProceduralGroomSettings settings;
        settings.strandCount = 4u;
        settings.pointsPerStrand = 3u;
        const GroomSource good = GenerateGroomOnSphereCap(settings);
        CHECK(ValidateGroomSource(good).empty());
        CHECK(ValidateGroomSource(GroomSource{}).empty());

        GroomSource bad = good;
        bad.widths.pop_back();
        CHECK(!ValidateGroomSource(bad).empty());
        bad = good;
        bad.strandPointOffsets[1] = bad.strandPointOffsets[0] + 1u; // one-point strand
        CHECK(!ValidateGroomSource(bad).empty());
        bad = good;
        bad.guideFlags = { 1u, 0u };
        CHECK(!ValidateGroomSource(bad).empty());
        bad = good;
        bad.closestGuides.assign(4u, { 0, 1, 4 }); // index 4 is out of range
        bad.closestGuideWeights.assign(4u, { 0.5f, 0.25f, 0.25f });
        CHECK(!ValidateGroomSource(bad).empty());
        bad.closestGuides.assign(4u, { 0, 1, -1 });
        CHECK(ValidateGroomSource(bad).empty());
    }

} // namespace

int main()
{
    TestSphereCapGenerator();
    TestGeneratorDeterminism();
    TestTriangleGenerator();
    TestGeneratorDroop();
    TestHairFileReader();
    TestValidateGroomSource();
    g_failures += RunGroomBuilderTests();
    g_failures += RunAlembicGroomTests();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "HairCoreTests passed\n";
    return 0;
}
