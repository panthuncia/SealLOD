#include "Hair/AlembicGroomReader.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace br::hair {

namespace {

    namespace Abc = Alembic::Abc;
    namespace AbcA = Alembic::AbcCoreAbstract;
    namespace AbcGeom = Alembic::AbcGeom;

    // The values of one property as flat scalars, whatever integer or float type and extent it was authored with.
    struct Attribute {
        bool found = false;
        std::vector<double> scalars;
    };

    template <typename T>
    void AppendScalars(const void* data, std::size_t count, std::vector<double>& out)
    {
        const T* values = static_cast<const T*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(static_cast<double>(values[i]));
        }
    }

    // False for types that are not plain numbers (strings, half floats).
    bool AppendPod(Alembic::Util::PlainOldDataType pod, const void* data, std::size_t count, std::vector<double>& out)
    {
        switch (pod) {
        case Alembic::Util::kBooleanPOD: // stored as one byte
        case Alembic::Util::kUint8POD: AppendScalars<std::uint8_t>(data, count, out); return true;
        case Alembic::Util::kInt8POD: AppendScalars<std::int8_t>(data, count, out); return true;
        case Alembic::Util::kUint16POD: AppendScalars<std::uint16_t>(data, count, out); return true;
        case Alembic::Util::kInt16POD: AppendScalars<std::int16_t>(data, count, out); return true;
        case Alembic::Util::kUint32POD: AppendScalars<std::uint32_t>(data, count, out); return true;
        case Alembic::Util::kInt32POD: AppendScalars<std::int32_t>(data, count, out); return true;
        case Alembic::Util::kUint64POD: AppendScalars<std::uint64_t>(data, count, out); return true;
        case Alembic::Util::kInt64POD: AppendScalars<std::int64_t>(data, count, out); return true;
        case Alembic::Util::kFloat32POD: AppendScalars<float>(data, count, out); return true;
        case Alembic::Util::kFloat64POD: AppendScalars<double>(data, count, out); return true;
        default: return false;
        }
    }

    // Reads property `name` of `parent` at its first sample: a scalar property, an array property, or an indexed
    // geom param (a compound of .vals and .indices, expanded here). Not found when the property does not exist.
    Attribute ReadAttribute(const Abc::ICompoundProperty& parent, const std::string& name)
    {
        Attribute attribute;
        if (!parent.valid()) {
            return attribute;
        }
        const AbcA::PropertyHeader* header = parent.getPropertyHeader(name);
        if (header == nullptr) {
            return attribute;
        }
        const auto unsupported = [&name] { return std::runtime_error("attribute '" + name + "' is not a numeric property"); };

        if (header->isScalar()) {
            const AbcA::DataType type = header->getDataType();
            Abc::IScalarProperty property(parent, name);
            std::vector<double> storage(type.getExtent() + 1u); // 8-byte aligned, large enough for any numeric type
            if (type.getPod() == Alembic::Util::kStringPOD || type.getPod() == Alembic::Util::kWstringPOD || property.getNumSamples() == 0u) {
                throw unsupported();
            }
            property.get(storage.data(), Abc::ISampleSelector(Abc::index_t{ 0 }));
            if (!AppendPod(type.getPod(), storage.data(), type.getExtent(), attribute.scalars)) {
                throw unsupported();
            }
            attribute.found = true;
            return attribute;
        }

        Abc::IArrayProperty values;
        Abc::IUInt32ArrayProperty indices;
        if (header->isCompound()) {
            const Abc::ICompoundProperty compound(parent, name);
            if (compound.getPropertyHeader(".vals") == nullptr) {
                throw unsupported();
            }
            values = Abc::IArrayProperty(compound, ".vals");
            if (compound.getPropertyHeader(".indices") != nullptr) {
                indices = Abc::IUInt32ArrayProperty(compound, ".indices");
            }
        } else {
            values = Abc::IArrayProperty(parent, name);
        }
        if (values.getNumSamples() == 0u) {
            return attribute;
        }
        AbcA::ArraySamplePtr sample;
        values.get(sample, Abc::ISampleSelector(Abc::index_t{ 0 }));
        const AbcA::DataType type = sample->getDataType();
        const std::size_t extent = type.getExtent();
        std::vector<double> scalars;
        if (!AppendPod(type.getPod(), sample->getData(), sample->size() * extent, scalars)) {
            throw unsupported();
        }
        if (indices.valid() && indices.getNumSamples() != 0u) {
            const Abc::UInt32ArraySamplePtr indexSample = indices.getValue(Abc::ISampleSelector(Abc::index_t{ 0 }));
            for (std::size_t i = 0; i < indexSample->size(); ++i) {
                const std::size_t element = (*indexSample)[i];
                if (element >= sample->size()) {
                    throw std::runtime_error("attribute '" + name + "' has an index outside its values");
                }
                attribute.scalars.insert(attribute.scalars.end(), scalars.begin() + static_cast<std::ptrdiff_t>(element * extent),
                                         scalars.begin() + static_cast<std::ptrdiff_t>((element + 1u) * extent));
            }
        } else {
            attribute.scalars = std::move(scalars);
        }
        attribute.found = true;
        return attribute;
    }

    // The nearest authoring of `name`: `containers` runs from the archive root down to the curves object.
    Attribute FindAttribute(const std::vector<Abc::ICompoundProperty>& containers, const std::string& name)
    {
        for (auto container = containers.rbegin(); container != containers.rend(); ++container) {
            Attribute attribute = ReadAttribute(*container, name);
            if (attribute.found) {
                return attribute;
            }
        }
        return {};
    }

    // An attribute laid out against one curves object: one element for the object, one per curve or one per point,
    // told apart by the element count. Each element has `components` scalars.
    class CurveAttribute {
    public:
        CurveAttribute(Attribute attribute, const std::string& name, std::size_t components, std::size_t curveCount, std::size_t pointCount)
            : m_scalars(std::move(attribute.scalars))
            , m_components(components)
        {
            if (!attribute.found) {
                return;
            }
            const std::size_t elements = m_scalars.size() / components;
            if (m_scalars.size() % components == 0u && elements == 1u) {
                m_layout = Layout::Constant;
            } else if (m_scalars.size() % components == 0u && elements == curveCount) {
                m_layout = Layout::PerCurve;
            } else if (m_scalars.size() % components == 0u && elements == pointCount) {
                m_layout = Layout::PerPoint;
            } else {
                throw std::runtime_error("attribute '" + name + "' has " + std::to_string(m_scalars.size()) + " values; expected " +
                                         std::to_string(components) + " (constant), " + std::to_string(components * curveCount) + " (per curve) or " +
                                         std::to_string(components * pointCount) + " (per point)");
            }
        }

        explicit operator bool() const { return m_layout != Layout::Missing; }

        // `point` is the point's index within the curves object; per-curve readers pass the curve's first point.
        double At(std::size_t curve, std::size_t point, std::size_t component = 0u) const
        {
            const std::size_t element = m_layout == Layout::Constant ? 0u : m_layout == Layout::PerCurve ? curve : point;
            return m_scalars[element * m_components + component];
        }

    private:
        enum class Layout { Missing, Constant, PerCurve, PerPoint };
        std::vector<double> m_scalars;
        std::size_t m_components;
        Layout m_layout = Layout::Missing;
    };

    struct Import {
        AlembicGroomImportSettings settings;
        GroomSource groom;
        AlembicGroomImportInfo info;
        std::vector<std::int32_t> strandIds; // per strand: groom_id, or the curve's index in archive order
        std::int32_t curveCount = 0;         // curves seen so far, skipped ones included
        bool hasGuideFlags = false;
        bool hasGroupIds = false;
        bool hasRootUVs = false;
        bool hasClosestGuides = false;
    };

    void ReadCurves(const Abc::IObject& object, const Imath::M44d& toWorld, const std::vector<Abc::ICompoundProperty>& containers, Import& import)
    {
        AbcGeom::ICurves curves(object, Abc::kWrapExisting);
        const AbcGeom::ICurvesSchema& schema = curves.getSchema();
        ++import.info.curveObjectCount;
        if (schema.getNumSamples() == 0u) {
            return;
        }
        // Linear curves are read as they are. For cubic and variable-order curves the stored control vertices are
        // taken as a polyline rather than evaluating the basis: good enough for the POC, and exact at the root and
        // tip only for bases that interpolate their end points.
        AbcGeom::ICurvesSchema::Sample sample;
        schema.get(sample, Abc::ISampleSelector(Abc::index_t{ 0 }));
        const Abc::P3fArraySamplePtr positions = sample.getPositions();
        const Abc::Int32ArraySamplePtr counts = sample.getCurvesNumVertices();
        if (!positions || !counts) {
            return;
        }
        const std::size_t curveCount = counts->size();
        const std::size_t pointCount = positions->size();
        std::size_t pointTotal = 0u;
        for (std::size_t curve = 0; curve < curveCount; ++curve) {
            if ((*counts)[curve] < 0) {
                throw std::runtime_error("curve " + std::to_string(curve) + " has a negative point count");
            }
            pointTotal += static_cast<std::size_t>((*counts)[curve]);
        }
        if (pointTotal != pointCount) {
            throw std::runtime_error("curve point counts add up to " + std::to_string(pointTotal) + " but there are " + std::to_string(pointCount) + " points");
        }

        Attribute widthAttribute = FindAttribute(containers, "groom_width");
        if (!widthAttribute.found) {
            if (const AbcGeom::IFloatGeomParam widthParam = schema.getWidthsParam(); widthParam.valid() && widthParam.getNumSamples() != 0u) {
                const Abc::FloatArraySamplePtr values = widthParam.getExpandedValue(Abc::ISampleSelector(Abc::index_t{ 0 })).getVals();
                if (values) {
                    widthAttribute.found = true;
                    widthAttribute.scalars.assign(values->get(), values->get() + values->size());
                }
            }
        }
        const CurveAttribute width(std::move(widthAttribute), "width", 1u, curveCount, pointCount);
        const CurveAttribute guide(FindAttribute(containers, "groom_guide"), "groom_guide", 1u, curveCount, pointCount);
        const CurveAttribute group(FindAttribute(containers, "groom_group_id"), "groom_group_id", 1u, curveCount, pointCount);
        const CurveAttribute id(FindAttribute(containers, "groom_id"), "groom_id", 1u, curveCount, pointCount);
        const CurveAttribute rootUV(FindAttribute(containers, "groom_root_uv"), "groom_root_uv", 2u, curveCount, pointCount);
        const CurveAttribute closest(FindAttribute(containers, "groom_closest_guides"), "groom_closest_guides", 3u, curveCount, pointCount);
        const CurveAttribute weights(FindAttribute(containers, "groom_guide_weights"), "groom_guide_weights", 3u, curveCount, pointCount);
        const bool bound = closest && weights;
        import.hasGuideFlags = import.hasGuideFlags || guide;
        import.hasGroupIds = import.hasGroupIds || group;
        import.hasRootUVs = import.hasRootUVs || rootUV;
        import.hasClosestGuides = import.hasClosestGuides || bound;

        if (import.info.versionMajor < 0) {
            if (const Attribute major = FindAttribute(containers, "groom_version_major"); major.found && !major.scalars.empty()) {
                import.info.versionMajor = static_cast<int>(major.scalars[0]);
            }
            if (const Attribute minor = FindAttribute(containers, "groom_version_minor"); minor.found && !minor.scalars.empty()) {
                import.info.versionMinor = static_cast<int>(minor.scalars[0]);
            }
        }

        constexpr float kFallbackWidth = 1.0f; // the schema's 1 cm, in file units
        const float unitScale = import.settings.unitScale;
        GroomSource& groom = import.groom;
        std::size_t firstPoint = 0u;
        for (std::size_t curve = 0; curve < curveCount; ++curve) {
            const std::size_t curvePoints = static_cast<std::size_t>((*counts)[curve]);
            const std::int32_t archiveIndex = import.curveCount++;
            if (curvePoints < 2u) {
                ++import.info.skippedStrandCount;
                firstPoint += curvePoints;
                continue;
            }
            for (std::size_t point = firstPoint; point < firstPoint + curvePoints; ++point) {
                Imath::V3d position;
                toWorld.multVecMatrix(Imath::V3d((*positions)[point]), position);
                position *= static_cast<double>(unitScale);
                if (import.settings.upAxis == GroomUpAxis::Z) {
                    position = Imath::V3d(position.x, position.z, -position.y);
                }
                groom.positions.push_back({ static_cast<float>(position.x), static_cast<float>(position.y), static_cast<float>(position.z) });
                // Widths are not scaled by the parent transforms, only by the unit conversion.
                groom.widths.push_back((width ? static_cast<float>(width.At(curve, point)) : kFallbackWidth) * unitScale);
            }
            if (groom.strandPointOffsets.empty()) {
                groom.strandPointOffsets.push_back(0u);
            }
            groom.strandPointOffsets.push_back(static_cast<std::uint32_t>(groom.positions.size()));

            import.strandIds.push_back(id ? static_cast<std::int32_t>(id.At(curve, firstPoint)) : archiveIndex);
            groom.guideFlags.push_back(guide && guide.At(curve, firstPoint) != 0.0 ? 1u : 0u);
            groom.groupIds.push_back(group ? static_cast<std::int32_t>(group.At(curve, firstPoint)) : 0);
            groom.rootUVs.push_back(rootUV ? Float2{ static_cast<float>(rootUV.At(curve, firstPoint, 0u)), static_cast<float>(rootUV.At(curve, firstPoint, 1u)) }
                                           : Float2{});
            std::array<std::int32_t, 3> guides{ -1, -1, -1 };
            std::array<float, 3> guideWeights{ 0.0f, 0.0f, 0.0f };
            for (std::size_t i = 0; bound && i < 3u; ++i) {
                guides[i] = static_cast<std::int32_t>(closest.At(curve, firstPoint, i));
                guideWeights[i] = static_cast<float>(weights.At(curve, firstPoint, i));
            }
            groom.closestGuides.push_back(guides);
            groom.closestGuideWeights.push_back(guideWeights);
            firstPoint += curvePoints;
        }
    }

    void Visit(const Abc::IObject& object, const Imath::M44d& parentToWorld, std::vector<Abc::ICompoundProperty>& containers, Import& import)
    {
        Imath::M44d toWorld = parentToWorld;
        const std::size_t containerCount = containers.size();
        if (AbcGeom::IXform::matches(object.getHeader())) {
            AbcGeom::IXform xform(object, Abc::kWrapExisting);
            const AbcGeom::IXformSchema& schema = xform.getSchema();
            if (schema.getNumSamples() != 0u) {
                AbcGeom::XformSample sample;
                schema.get(sample, Abc::ISampleSelector(Abc::index_t{ 0 }));
                toWorld = sample.getInheritsXforms() ? sample.getMatrix() * parentToWorld : sample.getMatrix();
            }
            containers.push_back(schema.getUserProperties());
            containers.push_back(schema.getArbGeomParams());
        } else if (AbcGeom::ICurves::matches(object.getHeader())) {
            AbcGeom::ICurves curves(object, Abc::kWrapExisting);
            containers.push_back(curves.getSchema().getUserProperties());
            containers.push_back(curves.getSchema().getArbGeomParams());
            try {
                ReadCurves(object, toWorld, containers, import);
            } catch (const std::exception& exception) {
                throw std::runtime_error(object.getFullName() + ": " + exception.what());
            }
        }
        for (std::size_t child = 0; child < object.getNumChildren(); ++child) {
            Visit(object.getChild(child), toWorld, containers, import);
        }
        containers.erase(containers.begin() + static_cast<std::ptrdiff_t>(containerCount), containers.end());
    }

    void Finish(Import& import)
    {
        GroomSource& groom = import.groom;
        if (!import.hasGuideFlags) groom.guideFlags.clear();
        if (!import.hasGroupIds) groom.groupIds.clear();
        if (!import.hasRootUVs) groom.rootUVs.clear();
        if (!import.hasClosestGuides) {
            groom.closestGuides.clear();
            groom.closestGuideWeights.clear();
            return;
        }
        // Authored guide references (groom_id values or archive curve indices) to strand indices.
        std::unordered_map<std::int32_t, std::int32_t> strandOfId;
        for (std::size_t strand = 0; strand < import.strandIds.size(); ++strand) {
            strandOfId.emplace(import.strandIds[strand], static_cast<std::int32_t>(strand));
        }
        for (auto& guides : groom.closestGuides) {
            for (std::int32_t& guide : guides) {
                const auto found = guide < 0 ? strandOfId.end() : strandOfId.find(guide);
                guide = found != strandOfId.end() ? found->second : -1;
            }
        }
    }

} // namespace

bool LoadAlembicGroom(const std::filesystem::path& path,
                      const AlembicGroomImportSettings& settings,
                      GroomSource& groom,
                      std::string* error,
                      AlembicGroomImportInfo* info)
{
    groom = {};
    if (info != nullptr) {
        *info = {};
    }
    std::string message;
    Import import;
    import.settings = settings;
    try {
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(path, ignored)) {
            message = "cannot open file";
        } else {
            // The factory tries each archive format built into the library (Ogawa here) and returns an invalid
            // archive for anything else.
            Alembic::AbcCoreFactory::IFactory factory;
            const Abc::IArchive archive = factory.getArchive(path.string());
            if (!archive.valid()) {
                message = "not a readable Alembic (Ogawa) archive";
            } else {
                std::vector<Abc::ICompoundProperty> containers;
                Visit(archive.getTop(), Imath::M44d(), containers, import);
                if (import.info.curveObjectCount == 0u) {
                    message = "archive has no curves";
                } else if (import.groom.StrandCount() == 0u) {
                    message = "archive has no curve with two or more points (" + std::to_string(import.info.skippedStrandCount) + " skipped)";
                }
            }
        }
    } catch (const std::exception& exception) {
        message = exception.what();
    }

    if (!message.empty()) {
        if (error != nullptr) {
            *error = path.string() + ": " + message;
        }
        return false;
    }
    Finish(import);
    groom = std::move(import.groom);
    if (info != nullptr) {
        *info = import.info;
    }
    return true;
}

} // namespace br::hair
