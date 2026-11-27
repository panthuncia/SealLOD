// Alembic importer tests. Run from HairCoreTests.cpp's main(). Every archive is written here with OCurves, in the
// UE groom schema's units (centimetres), and read back through LoadAlembicGroom.

#include "Hair/AlembicGroomReader.h"
#include "Hair/GroomBuilder.h"
#include "Hair/GroomSource.h"
#include "Hair/HairGPUTypes.h"
#include "Hair/ProceduralGroom.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace br::hair;

namespace {

    namespace Abc = Alembic::Abc;
    namespace AbcGeom = Alembic::AbcGeom;

    int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << "(" << __LINE__ << "): check failed: " #condition "\n"; \
            ++g_failures; \
        } \
    } while (false)

    bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }
    bool Near(Float3 a, Float3 b, float tolerance) { return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance); }

    const std::filesystem::path& ArchivePath()
    {
        static const std::filesystem::path path = std::filesystem::temp_directory_path() / "br_hair_core_tests.abc";
        return path;
    }

    Abc::OArchive CreateArchive() { return Abc::OArchive(Alembic::AbcCoreOgawa::WriteArchive(), ArchivePath().string()); }

    // Curve c of `curveCount` has counts[c] points at (c, 2 * i, 0.5 * c - i).
    struct TestCurves {
        std::vector<Imath::V3f> points;
        std::vector<std::int32_t> counts;
    };

    TestCurves MakeCurves(const std::vector<std::int32_t>& counts)
    {
        TestCurves curves;
        curves.counts = counts;
        for (std::size_t curve = 0; curve < counts.size(); ++curve) {
            for (std::int32_t i = 0; i < counts[curve]; ++i) {
                const float c = static_cast<float>(curve), f = static_cast<float>(i);
                curves.points.push_back({ c, 2.0f * f, 0.5f * c - f });
            }
        }
        return curves;
    }

    void SetCurves(AbcGeom::OCurves& object, const TestCurves& curves, AbcGeom::CurveType type = AbcGeom::kLinear)
    {
        object.getSchema().set(AbcGeom::OCurvesSchema::Sample(Abc::P3fArraySample(curves.points), Abc::Int32ArraySample(curves.counts), type,
                                                              AbcGeom::kNonPeriodic));
    }

    // Writes a geom param of type `Param` (for example AbcGeom::OV3iGeomParam) with one sample.
    template <typename Param>
    void WriteParam(Abc::OCompoundProperty parent,
                    const std::string& name,
                    const std::vector<typename Param::value_type>& values,
                    AbcGeom::GeometryScope scope,
                    std::size_t arrayExtent = 1u)
    {
        Param param(parent, name, false, scope, arrayExtent);
        param.set(typename Param::Sample(typename Param::prop_type::sample_type(values), scope));
    }

    GroomSource Import(const AlembicGroomImportSettings& settings = {}, AlembicGroomImportInfo* info = nullptr)
    {
        GroomSource groom;
        std::string error;
        if (!LoadAlembicGroom(ArchivePath(), settings, groom, &error, info)) {
            std::cerr << "import failed: " << error << "\n";
            ++g_failures;
        }
        CHECK(ValidateGroomSource(groom).empty());
        return groom;
    }

    void ExpectRejected(const std::filesystem::path& path, const char* expectedInError, int line)
    {
        GroomSource groom;
        groom.positions.push_back({}); // must be cleared on failure
        std::string error;
        const bool loaded = LoadAlembicGroom(path, {}, groom, &error);
        if (loaded || error.find(expectedInError) == std::string::npos || groom.StrandCount() != 0u || !groom.positions.empty()) {
            std::cerr << __FILE__ << "(" << line << "): expected rejection mentioning \"" << expectedInError << "\", got "
                      << (loaded ? "a successful load" : "\"" + error + "\"") << "\n";
            ++g_failures;
        }
    }

    // Every optional attribute, authored per curve (widths per point), with groom_id naming the closest guides.
    void TestFullGroom()
    {
        const TestCurves curves = MakeCurves({ 4, 4, 4, 4, 4, 4 });
        std::vector<float> widths;
        for (std::size_t point = 0; point < curves.points.size(); ++point) {
            widths.push_back(0.02f + 0.001f * static_cast<float>(point));
        }
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OXform root(archive.getTop(), "groom");
            Abc::OInt16Property(root.getSchema().getUserProperties(), "groom_version_major").set(1);
            Abc::OInt16Property(root.getSchema().getUserProperties(), "groom_version_minor").set(5);
            AbcGeom::OCurves object(root, "strands");
            SetCurves(object, curves);
            const Abc::OCompoundProperty arb = object.getSchema().getArbGeomParams();
            WriteParam<AbcGeom::OFloatGeomParam>(arb, "groom_width", widths, AbcGeom::kVertexScope);
            WriteParam<AbcGeom::OInt32GeomParam>(arb, "groom_guide", { 1, 0, 0, 1, 0, 0 }, AbcGeom::kUniformScope);
            WriteParam<AbcGeom::OInt32GeomParam>(arb, "groom_id", { 100, 110, 120, 130, 140, 150 }, AbcGeom::kUniformScope);
            {
                // An indexed geom param: two values shared between the curves.
                AbcGeom::OInt32GeomParam group(arb, "groom_group_id", true, AbcGeom::kUniformScope, 1u);
                const std::vector<std::int32_t> values{ 7, 9 };
                const std::vector<std::uint32_t> indices{ 0u, 1u, 1u, 0u, 1u, 0u };
                group.set(AbcGeom::OInt32GeomParam::Sample(Abc::Int32ArraySample(values), Abc::UInt32ArraySample(indices), AbcGeom::kUniformScope));
            }
            std::vector<Imath::V2f> rootUVs;
            std::vector<Imath::V3i> closest;
            std::vector<Imath::V3f> weights;
            for (int curve = 0; curve < 6; ++curve) {
                rootUVs.push_back({ static_cast<float>(curve) / 6.0f, 0.25f });
                closest.push_back({ 100, 130, 999 }); // curve 0, curve 3, and an id no curve has
                weights.push_back({ 0.5f, 0.3f, 0.2f });
            }
            WriteParam<AbcGeom::OV2fGeomParam>(arb, "groom_root_uv", rootUVs, AbcGeom::kUniformScope);
            WriteParam<AbcGeom::OV3iGeomParam>(arb, "groom_closest_guides", closest, AbcGeom::kUniformScope);
            WriteParam<AbcGeom::OV3fGeomParam>(arb, "groom_guide_weights", weights, AbcGeom::kUniformScope);
        }

        AlembicGroomImportInfo info;
        const GroomSource groom = Import({}, &info);
        CHECK(info.curveObjectCount == 1u && info.skippedStrandCount == 0u);
        CHECK(info.versionMajor == 1 && info.versionMinor == 5);
        CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 4u, 8u, 12u, 16u, 20u, 24u }));
        CHECK(groom.PointCount() == 24u && groom.widths.size() == 24u);
        bool positions = true, widthsMatch = true;
        for (std::uint32_t point = 0; point < groom.PointCount() && point < 24u; ++point) {
            // Centimetres to metres.
            const Imath::V3f source = curves.points[point];
            positions = positions && Near(groom.positions[point], Float3{ source.x * 0.01f, source.y * 0.01f, source.z * 0.01f }, 1.0e-7f);
            widthsMatch = widthsMatch && Near(groom.widths[point], widths[point] * 0.01f, 1.0e-9f);
        }
        CHECK(positions);
        CHECK(widthsMatch);
        CHECK((groom.guideFlags == std::vector<std::uint8_t>{ 1u, 0u, 0u, 1u, 0u, 0u }));
        CHECK((groom.groupIds == std::vector<std::int32_t>{ 7, 9, 9, 7, 9, 7 }));
        CHECK(groom.rootUVs.size() == 6u && groom.closestGuides.size() == 6u && groom.closestGuideWeights.size() == 6u);
        CHECK(groom.colors.empty());
        for (std::size_t strand = 0; strand < groom.rootUVs.size() && strand < groom.closestGuides.size(); ++strand) {
            CHECK(Near(groom.rootUVs[strand].x, static_cast<float>(strand) / 6.0f, 1.0e-7f) && groom.rootUVs[strand].y == 0.25f);
            CHECK((groom.closestGuides[strand] == std::array<std::int32_t, 3>{ 0, 3, -1 })); // ids became strand indices
            CHECK((groom.closestGuideWeights[strand] == std::array<float, 3>{ 0.5f, 0.3f, 0.2f }));
        }
    }

    // No attributes at all, then the schema's own width property, cubic curves, and the unit and up-axis settings.
    void TestBareGroom()
    {
        const TestCurves curves = MakeCurves({ 3, 5 });
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            SetCurves(object, curves);
        }
        AlembicGroomImportInfo info;
        GroomSource groom = Import({}, &info);
        CHECK(info.curveObjectCount == 1u && info.versionMajor == -1 && info.versionMinor == -1);
        CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 3u, 8u }));
        CHECK(groom.widths == std::vector<float>(8u, 0.01f)); // the 1 cm fallback
        CHECK(groom.colors.empty() && groom.guideFlags.empty() && groom.groupIds.empty() && groom.rootUVs.empty());
        CHECK(groom.closestGuides.empty() && groom.closestGuideWeights.empty());

        // The same file in its own units, treated as Z-up: (x, y, z) -> (x, z, -y).
        AlembicGroomImportSettings settings;
        settings.unitScale = 1.0f;
        settings.upAxis = GroomUpAxis::Z;
        groom = Import(settings);
        CHECK(groom.widths == std::vector<float>(8u, 1.0f));
        bool rotated = groom.PointCount() == 8u;
        for (std::uint32_t point = 0; rotated && point < 8u; ++point) {
            const Imath::V3f source = curves.points[point];
            rotated = groom.positions[point] == Float3{ source.x, source.z, -source.y };
        }
        CHECK(rotated);

        // Cubic curves keep their control vertices as the polyline; the schema's width property is constant here.
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            const std::vector<float> width{ 0.5f };
            object.getSchema().set(AbcGeom::OCurvesSchema::Sample(Abc::P3fArraySample(curves.points), Abc::Int32ArraySample(curves.counts), AbcGeom::kCubic,
                                                                  AbcGeom::kNonPeriodic,
                                                                  AbcGeom::OFloatGeomParam::Sample(Abc::FloatArraySample(width), AbcGeom::kConstantScope)));
        }
        groom = Import();
        CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 3u, 8u }));
        CHECK(groom.widths == std::vector<float>(8u, 0.5f * 0.01f));
        CHECK(groom.PointCount() == 8u && Near(groom.positions[7], Float3{ 0.01f, 0.08f, -0.035f }, 1.0e-7f));
    }

    // Two curves objects under different transforms are concatenated in traversal order; attributes authored
    // constant on an object or on a parent transform apply to every curve below.
    void TestParentTransforms()
    {
        const TestCurves a = MakeCurves({ 2, 2, 2 });
        const TestCurves b = MakeCurves({ 3, 3 });
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OXform head(archive.getTop(), "head");
            AbcGeom::XformSample translate;
            translate.setTranslation(Imath::V3d(10.0, 20.0, 30.0));
            head.getSchema().set(translate);
            Abc::OInt32Property(head.getSchema().getUserProperties(), "groom_group_id").set(4);

            AbcGeom::OXform scaled(head, "scaled");
            AbcGeom::XformSample scale;
            scale.setScale(Imath::V3d(2.0, 2.0, 2.0));
            scaled.getSchema().set(scale);
            AbcGeom::OCurves guides(scaled, "guides");
            SetCurves(guides, a);
            WriteParam<AbcGeom::OInt16GeomParam>(guides.getSchema().getArbGeomParams(), "groom_guide", { 1 }, AbcGeom::kConstantScope);

            AbcGeom::OCurves strands(head, "strands");
            SetCurves(strands, b);
        }
        AlembicGroomImportInfo info;
        const GroomSource groom = Import({}, &info);
        CHECK(info.curveObjectCount == 2u);
        CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 2u, 4u, 6u, 9u, 12u }));
        CHECK((groom.guideFlags == std::vector<std::uint8_t>{ 1u, 1u, 1u, 0u, 0u }));
        CHECK((groom.groupIds == std::vector<std::int32_t>{ 4, 4, 4, 4, 4 }));
        bool positions = groom.PointCount() == 12u;
        for (std::uint32_t point = 0; positions && point < 12u; ++point) {
            // Scale then translate for the first object, translate only for the second; then centimetres to metres.
            const Imath::V3f source = point < 6u ? a.points[point] * 2.0f : b.points[point - 6u];
            positions = Near(groom.positions[point], Float3{ (source.x + 10.0f) * 0.01f, (source.y + 20.0f) * 0.01f, (source.z + 30.0f) * 0.01f }, 1.0e-6f);
        }
        CHECK(positions);
        CHECK(groom.widths == std::vector<float>(12u, 0.01f)); // widths are not scaled by the transforms
    }

    // Curves of 3, 1, 5, 2 and 0 points: the short ones are skipped and counted, and per-point and per-curve
    // attributes stay aligned with the curves that remain. Closest guides are curve indices (no groom_id).
    void TestMixedCurveLengths()
    {
        const TestCurves curves = MakeCurves({ 3, 1, 5, 2, 0 });
        std::vector<float> widths;
        for (std::size_t point = 0; point < curves.points.size(); ++point) {
            widths.push_back(static_cast<float>(point + 1u));
        }
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            object.getSchema().set(AbcGeom::OCurvesSchema::Sample(Abc::P3fArraySample(curves.points), Abc::Int32ArraySample(curves.counts), AbcGeom::kLinear,
                                                                  AbcGeom::kNonPeriodic,
                                                                  AbcGeom::OFloatGeomParam::Sample(Abc::FloatArraySample(widths), AbcGeom::kVertexScope)));
            const Abc::OCompoundProperty arb = object.getSchema().getArbGeomParams();
            WriteParam<AbcGeom::OCharGeomParam>(arb, "groom_guide", { 0, 1, 1, 0, 1 }, AbcGeom::kUniformScope);
            // Plain int32 and float arrays with an array extent of 3 instead of vector types.
            WriteParam<AbcGeom::OInt32GeomParam>(arb, "groom_closest_guides", { 2, 3, 1, /**/ 0, 0, 0, /**/ 0, 3, -1, /**/ 0, 2, 7, /**/ 0, 0, 0 },
                                                 AbcGeom::kUniformScope, 3u);
            WriteParam<AbcGeom::OFloatGeomParam>(arb, "groom_guide_weights", { 3, 2, 1, /**/ 1, 1, 1, /**/ 1, 1, 0, /**/ 2, 1, 1, /**/ 1, 1, 1 },
                                                 AbcGeom::kUniformScope, 3u);
        }
        AlembicGroomImportInfo info;
        const GroomSource groom = Import({}, &info);
        CHECK(info.skippedStrandCount == 2u);
        CHECK((groom.strandPointOffsets == std::vector<std::uint32_t>{ 0u, 3u, 8u, 10u }));
        // Source points 0-2, 4-8 and 9-10 remain; point 3 was the one-point curve.
        const std::vector<float> expectedWidths{ 0.01f, 0.02f, 0.03f, 0.05f, 0.06f, 0.07f, 0.08f, 0.09f, 0.10f, 0.11f };
        bool aligned = groom.PointCount() == 10u;
        for (std::uint32_t point = 0; aligned && point < 10u; ++point) {
            const Imath::V3f source = curves.points[point < 3u ? point : point + 1u];
            aligned = Near(groom.widths[point], expectedWidths[point], 1.0e-7f) &&
                      Near(groom.positions[point], Float3{ source.x * 0.01f, source.y * 0.01f, source.z * 0.01f }, 1.0e-7f);
        }
        CHECK(aligned);
        CHECK((groom.guideFlags == std::vector<std::uint8_t>{ 0u, 1u, 0u }));
        // Curve indices 0, 2, 3 became strands 0, 1, 2; the skipped curve 1 and the out-of-range 7 became -1.
        CHECK(groom.closestGuides.size() == 3u && groom.closestGuideWeights.size() == 3u);
        if (groom.closestGuides.size() == 3u && groom.closestGuideWeights.size() == 3u) {
            CHECK((groom.closestGuides[0] == std::array<std::int32_t, 3>{ 1, 2, -1 }));
            CHECK((groom.closestGuides[1] == std::array<std::int32_t, 3>{ 0, 2, -1 }));
            CHECK((groom.closestGuides[2] == std::array<std::int32_t, 3>{ 0, 1, -1 }));
            CHECK((groom.closestGuideWeights[0] == std::array<float, 3>{ 3.0f, 2.0f, 1.0f }));
            CHECK((groom.closestGuideWeights[2] == std::array<float, 3>{ 2.0f, 1.0f, 1.0f }));
        }
    }

    void TestBadInputs()
    {
        ExpectRejected(std::filesystem::temp_directory_path() / "br_hair_core_tests_missing.abc", "cannot open", __LINE__);
        {
            std::ofstream file(ArchivePath(), std::ios::binary | std::ios::trunc);
            file << "this is not an Alembic archive, just some text long enough to look like a file header";
        }
        ExpectRejected(ArchivePath(), "not a readable Alembic", __LINE__);
        {
            std::ofstream file(ArchivePath(), std::ios::binary | std::ios::trunc); // empty file
        }
        ExpectRejected(ArchivePath(), "not a readable Alembic", __LINE__);
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OXform root(archive.getTop(), "groom");
            AbcGeom::XformSample identity;
            root.getSchema().set(identity);
        }
        ExpectRejected(ArchivePath(), "no curves", __LINE__);
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            SetCurves(object, MakeCurves({ 1, 1, 0 }));
        }
        ExpectRejected(ArchivePath(), "two or more points (3 skipped)", __LINE__);
        {
            // Two values fit neither the object, its three curves nor its six points.
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            SetCurves(object, MakeCurves({ 2, 2, 2 }));
            WriteParam<AbcGeom::OInt32GeomParam>(object.getSchema().getArbGeomParams(), "groom_guide", { 1, 0 }, AbcGeom::kUniformScope);
        }
        ExpectRejected(ArchivePath(), "/strands: attribute 'groom_guide' has 2 values", __LINE__);
        {
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            SetCurves(object, MakeCurves({ 2, 2, 2 }));
            WriteParam<AbcGeom::OStringGeomParam>(object.getSchema().getArbGeomParams(), "groom_group_id", { "a", "b", "c" }, AbcGeom::kUniformScope);
        }
        ExpectRejected(ArchivePath(), "'groom_group_id' is not a numeric property", __LINE__);
    }

    // A generated groom written as an Alembic groom, imported, and built: the authored guide flags put those strands
    // first in the ranking, and authored closest guides are kept wherever the builder says it keeps them (all of a
    // strand's guides are guide-capable).
    void TestImportedGroomBuilds()
    {
        ProceduralGroomSettings generator;
        generator.strandCount = 2000u;
        generator.pointsPerStrand = 9u;
        const GroomSource source = GenerateGroomOnSphereCap(generator);
        const std::uint32_t strandCount = source.StrandCount();

        // Every 7th strand is a guide; every strand names three of them with weights 2:1:1, every 5th only two.
        std::vector<std::int32_t> guideFlags(strandCount, 0), flagged;
        for (std::uint32_t strand = 0; strand < strandCount; strand += 7u) {
            guideFlags[strand] = 1;
            flagged.push_back(static_cast<std::int32_t>(strand));
        }
        std::vector<Imath::V3i> closest;
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            closest.push_back({ flagged[strand % flagged.size()], flagged[(strand * 3u + 1u) % flagged.size()],
                                strand % 5u == 0u ? -1 : flagged[(strand * 5u + 2u) % flagged.size()] });
        }
        {
            TestCurves curves;
            std::vector<float> widths;
            for (std::uint32_t point = 0; point < source.PointCount(); ++point) {
                const Float3 position = source.positions[point] * 100.0f; // metres to centimetres
                curves.points.push_back({ position.x, position.y, position.z });
                widths.push_back(source.widths[point] * 100.0f);
            }
            curves.counts.assign(strandCount, 9);
            Abc::OArchive archive = CreateArchive();
            AbcGeom::OCurves object(archive.getTop(), "strands");
            SetCurves(object, curves);
            const Abc::OCompoundProperty arb = object.getSchema().getArbGeomParams();
            WriteParam<AbcGeom::OFloatGeomParam>(arb, "groom_width", widths, AbcGeom::kVertexScope);
            WriteParam<AbcGeom::OInt32GeomParam>(arb, "groom_guide", guideFlags, AbcGeom::kUniformScope);
            WriteParam<AbcGeom::OV3iGeomParam>(arb, "groom_closest_guides", closest, AbcGeom::kUniformScope);
            WriteParam<AbcGeom::OV3fGeomParam>(arb, "groom_guide_weights", std::vector<Imath::V3f>(strandCount, Imath::V3f(2.0f, 1.0f, 1.0f)),
                                               AbcGeom::kUniformScope);
        }

        const GroomSource imported = Import();
        CHECK(imported.strandPointOffsets == source.strandPointOffsets);
        if (imported.strandPointOffsets != source.strandPointOffsets || imported.closestGuides.size() != strandCount ||
            imported.guideFlags.size() != strandCount) {
            ++g_failures;
            return;
        }
        bool geometry = true, attributes = true;
        for (std::uint32_t point = 0; point < source.PointCount(); ++point) {
            geometry = geometry && Near(imported.positions[point], source.positions[point], 1.0e-6f) &&
                       Near(imported.widths[point], source.widths[point], 1.0e-9f);
        }
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            attributes = attributes && imported.guideFlags[strand] == guideFlags[strand] &&
                         imported.closestGuides[strand] == std::array<std::int32_t, 3>{ closest[strand].x, closest[strand].y, closest[strand].z } &&
                         imported.closestGuideWeights[strand] == std::array<float, 3>{ 2.0f, 1.0f, 1.0f };
        }
        CHECK(geometry);
        CHECK(attributes);

        // 256 guide-capable strands: fewer than the 286 flagged ones, so some authored guides are not guide-capable.
        GroomBuildSettings settings;
        settings.maxGuideCount = 256u;
        BuiltGroom groom;
        GroomBuildInfo info;
        std::string error;
        CHECK(BuildGroom(imported, nullptr, settings, groom, &error, &info));
        CHECK(ValidateBuiltGroom(groom).empty());
        if (groom.strands.size() != strandCount) {
            return;
        }
        std::vector<std::uint32_t> rankOfSource(strandCount);
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            rankOfSource[info.sourceStrandIndices[strand]] = groom.strands[strand].rank;
        }
        bool flaggedFirst = true, authoredKept = true;
        std::uint32_t authoredCount = 0u, derivedCount = 0u;
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            const HairStrandRecord& record = groom.strands[strand];
            const std::uint32_t sourceStrand = info.sourceStrandIndices[strand];
            flaggedFirst = flaggedFirst && (record.rank < flagged.size()) == (guideFlags[sourceStrand] != 0);
            if (record.rank < groom.GuideCount()) {
                continue; // guide-capable strands bind to themselves
            }
            const std::array<std::int32_t, 3>& guides = imported.closestGuides[sourceStrand];
            bool allCapable = true;
            for (const std::int32_t guide : guides) {
                allCapable = allCapable && (guide < 0 || rankOfSource[guide] < groom.GuideCount());
            }
            if (!allCapable) {
                ++derivedCount;
                continue;
            }
            ++authoredCount;
            const bool two = guides[2] < 0;
            authoredKept = authoredKept && record.guideIndices[0] == rankOfSource[guides[0]] && record.guideIndices[1] == rankOfSource[guides[1]] &&
                           record.guideIndices[2] == (two ? HAIR_INVALID_INDEX : rankOfSource[guides[2]]) &&
                           Near(record.guideWeights[0], two ? 2.0f / 3.0f : 0.5f, 1.0e-6f) && Near(record.guideWeights[1], two ? 1.0f / 3.0f : 0.25f, 1.0e-6f) &&
                           Near(record.guideWeights[2], two ? 0.0f : 0.25f, 1.0e-6f);
        }
        CHECK(flaggedFirst);
        CHECK(authoredKept);
        CHECK(authoredCount > 100u && derivedCount > 100u); // both binding paths ran
        std::cout << "  alembic: " << strandCount << " strands imported and built, " << flagged.size() << " authored guides ranked first, "
                  << authoredCount << " strands kept authored closest guides, " << derivedCount << " fell back to nearest guides\n";
    }

} // namespace

int RunAlembicGroomTests()
{
    TestFullGroom();
    TestBareGroom();
    TestParentTransforms();
    TestMixedCurveLengths();
    TestBadInputs();
    TestImportedGroomBuilds();

    std::error_code ignored;
    std::filesystem::remove(ArchivePath(), ignored);
    return g_failures;
}
