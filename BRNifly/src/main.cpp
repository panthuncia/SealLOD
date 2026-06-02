#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stacktrace>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <pxr/pxr.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <DbgHelp.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace pxr;

namespace {

#ifdef _WIN32
namespace CrashLog {
thread_local std::string t_action;
thread_local std::string t_inputPath;

struct Context {
    std::string action;
    std::string inputPath;
};

void SetContext(std::string action, std::string inputPath = {})
{
    t_action = std::move(action);
    t_inputPath = std::move(inputPath);
}

Context SnapshotContext()
{
    return { t_action, t_inputPath };
}

fs::path MakeCrashPath(std::string_view extension)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    (void)localtime_s(std::addressof(localTime), std::addressof(time));

    std::ostringstream name;
    name << "BRNifly-"
         << std::put_time(std::addressof(localTime), "%Y%m%d-%H%M%S")
         << "-pid" << GetCurrentProcessId()
         << extension;

    std::error_code ec;
    fs::create_directories("crashes", ec);
    return fs::current_path() / "crashes" / name.str();
}

std::string StacktraceString()
{
#if defined(__cpp_lib_stacktrace) && (__cpp_lib_stacktrace >= 202011L)
    try {
        std::ostringstream output;
        output << std::stacktrace::current();
        return output.str();
    } catch (...) {
        return "(stacktrace capture failed)";
    }
#else
    return "(no <stacktrace> support in this build)";
#endif
}

fs::path WriteMiniDump(EXCEPTION_POINTERS* exceptionPointers)
{
    const auto dumpPath = MakeCrashPath(".dmp");
    const auto file = CreateFileW(
        dumpPath.wstring().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const auto dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules);

    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        dumpType,
        exceptionPointers ? std::addressof(exceptionInfo) : nullptr,
        nullptr,
        nullptr);
    CloseHandle(file);

    return ok ? dumpPath : fs::path{};
}

void WriteCrashTextReport(const char* title, DWORD exceptionCode, void* exceptionAddress, const fs::path& minidumpPath)
{
    const auto reportPath = MakeCrashPath(".txt");
    std::ofstream report(reportPath, std::ios::trunc);
    if (!report) {
        return;
    }

    const auto context = SnapshotContext();
    report << title << "\n";
    report << "exception_code=0x" << std::hex << std::uppercase << exceptionCode << std::dec << "\n";
    report << "exception_address=" << exceptionAddress << "\n";
    report << "minidump='" << minidumpPath.string() << "'\n";
    report << "action='" << context.action << "'\n";
    report << "input_path='" << context.inputPath << "'\n\n";
    report << "Stacktrace:\n" << StacktraceString() << "\n";
}

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
{
    const auto exceptionRecord = exceptionPointers ? exceptionPointers->ExceptionRecord : nullptr;
    const auto code = exceptionRecord ? exceptionRecord->ExceptionCode : 0;
    void* const address = exceptionRecord ? exceptionRecord->ExceptionAddress : nullptr;
    const auto context = SnapshotContext();
    const auto dumpPath = WriteMiniDump(exceptionPointers);

    try {
        std::cerr << "BRNifly crashed: exception=0x"
                  << std::hex << std::uppercase << code << std::dec
                  << " address=" << address
                  << " action='" << context.action
                  << "' input='" << context.inputPath
                  << "' dump='" << (dumpPath.empty() ? std::string("(dump failed)") : dumpPath.string())
                  << "'\n";
    } catch (...) {}

    WriteCrashTextReport("BRNifly unhandled exception", code, address, dumpPath);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void TerminateHandler() noexcept
{
    try {
        if (auto exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                std::cerr << "BRNifly terminated by uncaught exception: " << typeid(e).name() << " what(): " << e.what() << "\n";
            } catch (...) {
                std::cerr << "BRNifly terminated by uncaught non-std exception\n";
            }
        } else {
            std::cerr << "BRNifly terminated without an active exception\n";
        }

        const auto context = SnapshotContext();
        const auto dumpPath = WriteMiniDump(nullptr);
        std::cerr << "Terminate context: action='" << context.action
                  << "' input='" << context.inputPath
                  << "' dump='" << (dumpPath.empty() ? std::string("(dump failed)") : dumpPath.string())
                  << "'\n";
        WriteCrashTextReport("BRNifly terminate", 0, nullptr, dumpPath);
    } catch (...) {}

    std::_Exit(EXIT_FAILURE);
}

void Install()
{
    SetUnhandledExceptionFilter(UnhandledExceptionFilter);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, [](int) {
        TerminateHandler();
    });
}
}
#endif

constexpr std::string_view kUsdContentIdentityVersion = "brnifly-usd-content-v8";

struct Diagnostic {
    std::string level;
    std::string message;
};

struct FlagDef {
    uint32_t mask;
    const char* name;
};

constexpr std::array<FlagDef, 32> kShaderFlags1 = {{
    {1u << 0, "SPECULAR"},
    {1u << 1, "SKINNED"},
    {1u << 2, "TEMP_REFRACTION"},
    {1u << 3, "VERTEX_ALPHA"},
    {1u << 4, "GREYSCALE_COLOR"},
    {1u << 5, "GREYSCALE_ALPHA"},
    {1u << 6, "USE_FALLOFF"},
    {1u << 7, "ENVIRONMENT_MAPPING"},
    {1u << 8, "RECEIVE_SHADOWS"},
    {1u << 9, "CAST_SHADOWS"},
    {1u << 10, "FACEGEN_DETAIL_MAP"},
    {1u << 11, "PARALLAX"},
    {1u << 12, "MODEL_SPACE_NORMALS"},
    {1u << 13, "NON_PROJECTIVE_SHADOWS"},
    {1u << 14, "LANDSCAPE"},
    {1u << 15, "REFRACTION"},
    {1u << 16, "FIRE_REFRACTION"},
    {1u << 17, "EYE_ENVIRONMENT_MAPPING"},
    {1u << 18, "HAIR_SOFT_LIGHTING"},
    {1u << 19, "SCREENDOOR_ALPHA_FADE"},
    {1u << 20, "LOCALMAP_HIDE_SECRET"},
    {1u << 21, "FACEGEN_RGB_TINT"},
    {1u << 22, "OWN_EMIT"},
    {1u << 23, "PROJECTED_UV"},
    {1u << 24, "MULTIPLE_TEXTURES"},
    {1u << 25, "REMAPPABLE_TEXTURES"},
    {1u << 26, "DECAL"},
    {1u << 27, "DYNAMIC_DECAL"},
    {1u << 28, "PARALLAX_OCCLUSION"},
    {1u << 29, "EXTERNAL_EMITTANCE"},
    {1u << 30, "SOFT_EFFECT"},
    {1u << 31, "ZBUFFER_TEST"}
}};

constexpr std::array<FlagDef, 32> kShaderFlags2 = {{
    {1u << 0, "ZBUFFER_WRITE"},
    {1u << 1, "LOD_LANDSCAPE"},
    {1u << 2, "LOD_OBJECTS"},
    {1u << 3, "NO_FADE"},
    {1u << 4, "DOUBLE_SIDED"},
    {1u << 5, "VERTEX_COLORS"},
    {1u << 6, "GLOW_MAP"},
    {1u << 7, "ASSUME_SHADOWMASK"},
    {1u << 8, "PACKED_TANGENT"},
    {1u << 9, "MULTI_INDEX_SNOW"},
    {1u << 10, "VERTEX_LIGHTING"},
    {1u << 11, "UNIFORM_SCALE"},
    {1u << 12, "FIT_SLOPE"},
    {1u << 13, "BILLBOARD"},
    {1u << 14, "NO_LOD_LAND_BLEND"},
    {1u << 15, "ENVMAP_LIGHT_FADE"},
    {1u << 16, "WIREFRAME"},
    {1u << 17, "WEAPON_BLOOD"},
    {1u << 18, "HIDE_ON_LOCAL_MAP"},
    {1u << 19, "PREMULT_ALPHA"},
    {1u << 20, "CLOUD_LOD"},
    {1u << 21, "ANISOTROPIC_LIGHTING"},
    {1u << 22, "NO_TRANSPARENCY_MULTISAMPLING"},
    {1u << 23, "UNUSED01"},
    {1u << 24, "MULTI_LAYER_PARALLAX"},
    {1u << 25, "SOFT_LIGHTING"},
    {1u << 26, "RIM_LIGHTING"},
    {1u << 27, "BACK_LIGHTING"},
    {1u << 28, "UNUSED02"},
    {1u << 29, "TREE_ANIM"},
    {1u << 30, "EFFECT_LIGHTING"},
    {1u << 31, "HD_LOD_OBJECTS"}
}};

constexpr std::array<const char*, 21> kLightingShaderTypeNames = {{
    "DEFAULT",
    "ENVMAP",
    "GLOWMAP",
    "PARALLAX",
    "FACE",
    "SKINTINT",
    "HAIRTINT",
    "PARALLAXOCC",
    "MULTITEXTURELANDSCAPE",
    "LODLANDSCAPE",
    "SNOW",
    "MULTILAYERPARALLAX",
    "TREEANIM",
    "LODOBJECTS",
    "MULTIINDEXSNOW",
    "LODOBJECTSHD",
    "EYE",
    "CLOUD",
    "LODLANDSCAPENOISE",
    "MULTITEXTURELANDSCAPELODBLEND",
    "DISMEMBERMENT"
}};

json DiagnosticJson(const Diagnostic& diagnostic)
{
    return json{{"level", diagnostic.level}, {"message", diagnostic.message}};
}

void AddDiagnostic(std::vector<Diagnostic>& diagnostics, std::string level, std::string message)
{
    diagnostics.push_back(Diagnostic{std::move(level), std::move(message)});
}

std::string SanitizePrimName(std::string name)
{
    if (name.empty()) {
        return "Shape";
    }
    for (char& ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) {
            ch = '_';
        }
    }
    if (name.front() >= '0' && name.front() <= '9') {
        name.insert(name.begin(), '_');
    }
    return name;
}

uint64_t Fnv1a64(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HexHash(uint64_t hash)
{
    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::string Hex32(uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

json DecodeShaderFlags(uint32_t flags, const std::array<FlagDef, 32>& definitions)
{
    json names = json::array();
    for (const FlagDef& def : definitions) {
        if ((flags & def.mask) != 0u) {
            names.push_back(def.name);
        }
    }
    return names;
}

std::string ShaderTypeName(uint32_t shaderType)
{
    if (shaderType < kLightingShaderTypeNames.size()) {
        return kLightingShaderTypeNames[shaderType];
    }
    return "UNKNOWN_" + std::to_string(shaderType);
}

std::string HexBytes(const std::vector<char>& bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2u);
    for (unsigned char byte : bytes) {
        text.push_back(kHex[(byte >> 4u) & 0xfu]);
        text.push_back(kHex[byte & 0xfu]);
    }
    return text;
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string MakeUniqueName(const std::string& baseName, std::set<std::string>& usedNames)
{
    std::string name = SanitizePrimName(baseName);
    std::string candidate = name;
    int suffix = 1;
    while (!usedNames.insert(candidate).second) {
        candidate = name + "_" + std::to_string(suffix++);
    }
    return candidate;
}

std::vector<std::string> ServiceList()
{
    return {
        "nif.open",
        "nif.close",
        "nif.describe",
        "nif.inspect.shaderFlags",
        "nif.convert.usd",
        "nif.stream.blocks",
        "nif.stream.geometry",
        "nif.stream.materials",
        "nif.stream.skeleton",
        "nif.stream.animation",
        "nif.stream.morphs"
    };
}

std::vector<std::string> SupportedGames()
{
    return {
        "Skyrim LE",
        "Skyrim SE",
        "Fallout 3",
        "Fallout NV",
        "Fallout 4",
        "Fallout 76"
    };
}

#ifdef _WIN32
using LoadFn = void* (*)(const char*);
using DestroyFn = void (*)(void*);
using GetVersionFn = const int* (*)();
using GetGameNameFn = int (*)(void*, char*, int);
using GetShapesFn = int (*)(void*, void**, int, int);
using GetShapeNameFn = int (*)(void*, char*, int);
using GetShapeBlockNameFn = int (*)(void*, char*, int);
using GetBlockIdFn = int (*)(void*, void*);
using GetVertsForShapeFn = int (*)(void*, void*, float*, int, int);
using GetNormalsForShapeFn = int (*)(void*, void*, float*, int, int);
using GetTangentsForShapeFn = int (*)(void*, void*, float*, int, int);
using GetTrianglesFn = int (*)(void*, void*, uint16_t*, int, int);
using GetUVsFn = int (*)(void*, void*, float*, int, int);
using GetColorsForShapeFn = int (*)(void*, void*, float*, int);
using GetShaderTextureSlotFn = int (*)(void*, void*, int, char*, int);
using GetShaderBlockNameFn = const char* (*)(void*, void*);
using GetShaderTypeFn = uint32_t (*)(void*, void*);
using GetShaderFlagsFn = uint32_t (*)(void*, void*);
using GetMessageLogFn = int (*)(char*, int);
using ClearMessageLogFn = void (*)();

struct TransformBuf {
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f};
    float scale = 1.0f;
};

struct VertexWeightPair {
    uint16_t vertex = 0;
    float weight = 0.0f;
};

struct BSLSPAttrs {
    uint32_t shaderType = 0;
    uint32_t shaderFlags1 = 0;
    uint32_t shaderFlags2 = 0;
    float uvOffsetU = 0.0f;
    float uvOffsetV = 0.0f;
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;
    float emissiveColorR = 0.0f;
    float emissiveColorG = 0.0f;
    float emissiveColorB = 0.0f;
    float emissiveColorA = 1.0f;
    float emissiveMult = 1.0f;
    float environmentMapScale = 0.0f;
    uint32_t texClampMode = 0;
    float alpha = 1.0f;
    float refractionStrength = 0.0f;
    float glossiness = 0.0f;
    float specColorR = 0.0f;
    float specColorG = 0.0f;
    float specColorB = 0.0f;
    float specStrength = 0.0f;
    float softLighting = 0.0f;
    float rimLightPower = 0.0f;
    float skinTintAlpha = 0.0f;
    float skinTintColorR = 0.0f;
    float skinTintColorG = 0.0f;
    float skinTintColorB = 0.0f;
};

struct BSESPAttrs {
    uint32_t shaderFlags1 = 0;
    uint32_t shaderFlags2 = 0;
    float uvOffsetU = 0.0f;
    float uvOffsetV = 0.0f;
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;
    uint32_t texClampMode = 0;
    unsigned char lightingInfluence = 0;
    unsigned char envMapMinLod = 0;
    float falloffStartAngle = 0.0f;
    float falloffStopAngle = 0.0f;
    float falloffStartOpacity = 0.0f;
    float falloffStopOpacity = 0.0f;
    float emissiveColorR = 0.0f;
    float emissiveColorG = 0.0f;
    float emissiveColorB = 0.0f;
    float emissiveColorA = 1.0f;
    float emissiveMult = 1.0f;
    float softFalloffDepth = 0.0f;
    float envMapScale = 0.0f;
};

struct AlphaPropertyBuf {
    uint16_t flags = 0;
    uint8_t threshold = 0;
};

using GetTransformFn = void (*)(void*, TransformBuf*);
using GetNodeCountFn = int (*)(void*);
using GetNodesFn = void (*)(void*, void**);
using GetNodeNameFn = int (*)(void*, char*, int);
using GetNodeBlocknameFn = int (*)(void*, char*, int);
using GetNodeFlagsFn = int (*)(void*);
using GetNodeParentFn = void* (*)(void*, void*);
using GetNodeSwitchIndexFn = int (*)(void*);
using GetNodeChildCountFn = int (*)(void*);
using GetNodeChildIDByIndexFn = int (*)(void*, int);
using GetShapeBoneCountFn = int (*)(void*, void*);
using GetShapeBoneNamesFn = int (*)(void*, void*, char*, int);
using GetShapeBoneWeightsCountFn = int (*)(void*, void*, int);
using GetShapeBoneWeightsFn = int (*)(void*, void*, int, VertexWeightPair*, int);
using GetShapeSkinBoneCountFn = int (*)(void*, void*);
using GetShapeSkinBoneNamesFn = int (*)(void*, void*, char*, int);
using GetShapeSkinWeightsCountFn = int (*)(void*, void*, int);
using GetShapeSkinWeightsFn = int (*)(void*, void*, int, VertexWeightPair*, int);
using GetShapePartitionSkinningFn = int (*)(void*, void*, uint16_t*, float*, int);
using GetShapeSkinToBoneFn = bool (*)(void*, void*, const char*, float*);
using GetShapeSkinToBoneByIndexFn = bool (*)(void*, void*, int, float*);
using SegmentCountFn = int (*)(void*, void*);
using GetSegmentFileFn = int (*)(void*, void*, char*, int);
using GetSegmentsFn = int (*)(void*, void*, int*, int);
using GetSubsegmentsFn = int (*)(void*, void*, int, uint32_t*, int);
using GetPartitionsFn = int (*)(void*, void*, uint16_t*, int);
using GetPartitionTrisFn = int (*)(void*, void*, uint16_t*, int);
using GetShaderAttrsFn = int (*)(void*, void*, BSLSPAttrs*);
using GetEffectShaderAttrsFn = int (*)(void*, void*, BSESPAttrs*);
using GetAlphaPropertyFn = int (*)(void*, void*, AlphaPropertyBuf*);
using GetExtraDataLenFn = int (*)(void*, void*, int, int*, int*);
using GetStringExtraDataFn = int (*)(void*, void*, int, char*, int, char*, int);
using GetBGExtraDataFn = int (*)(void*, void*, int, char*, int, char*, int, uint16_t*);
using GetCollisionFn = void* (*)(void*, void*);
using GetCollBodyIdFn = int (*)(void*, void*);
using GetCollShapeIdFn = int (*)(void*, int);
using GetCollShapeBlockNameFn = int (*)(void*, int, char*, int);
using GetCollShapeVertsFn = int (*)(void*, int, float*, int);
using GetCollShapeNormalsFn = int (*)(void*, int, float*, int);
using GetCollDataIdFn = int (*)(void*, int);
using GetCollShapeTrisFn = int (*)(void*, int, uint16_t*, int);

struct NiflyApi {
    HMODULE module = nullptr;
    LoadFn load = nullptr;
    DestroyFn destroy = nullptr;
    GetVersionFn getVersion = nullptr;
    GetGameNameFn getGameName = nullptr;
    GetShapesFn getShapes = nullptr;
    GetShapeNameFn getShapeName = nullptr;
    GetShapeBlockNameFn getShapeBlockName = nullptr;
    GetBlockIdFn getBlockId = nullptr;
    GetVertsForShapeFn getVertsForShape = nullptr;
    GetNormalsForShapeFn getNormalsForShape = nullptr;
    GetTangentsForShapeFn getTangentsForShape = nullptr;
    GetTangentsForShapeFn getBitangentsForShape = nullptr;
    GetTrianglesFn getTriangles = nullptr;
    GetUVsFn getUVs = nullptr;
    GetColorsForShapeFn getColorsForShape = nullptr;
    GetShaderTextureSlotFn getShaderTextureSlot = nullptr;
    GetShaderBlockNameFn getShaderBlockName = nullptr;
    GetShaderTypeFn getShaderType = nullptr;
    GetShaderFlagsFn getShaderFlags1 = nullptr;
    GetShaderFlagsFn getShaderFlags2 = nullptr;
    GetMessageLogFn getMessageLog = nullptr;
    ClearMessageLogFn clearMessageLog = nullptr;
    GetTransformFn getTransform = nullptr;
    GetTransformFn getNodeTransform = nullptr;
    GetNodeCountFn getNodeCount = nullptr;
    GetNodesFn getNodes = nullptr;
    GetNodeNameFn getNodeName = nullptr;
    GetNodeBlocknameFn getNodeBlockname = nullptr;
    GetNodeFlagsFn getNodeFlags = nullptr;
    GetNodeParentFn getNodeParent = nullptr;
    GetNodeSwitchIndexFn getNodeSwitchIndex = nullptr;
    GetNodeChildCountFn getNodeChildCount = nullptr;
    GetNodeChildIDByIndexFn getNodeChildIDByIndex = nullptr;
    GetShapeBoneCountFn getShapeBoneCount = nullptr;
    GetShapeBoneNamesFn getShapeBoneNames = nullptr;
    GetShapeBoneWeightsCountFn getShapeBoneWeightsCount = nullptr;
    GetShapeBoneWeightsFn getShapeBoneWeights = nullptr;
    GetShapeSkinBoneCountFn getShapeSkinBoneCount = nullptr;
    GetShapeSkinBoneNamesFn getShapeSkinBoneNames = nullptr;
    GetShapeSkinWeightsCountFn getShapeSkinWeightsCount = nullptr;
    GetShapeSkinWeightsFn getShapeSkinWeights = nullptr;
    GetShapePartitionSkinningFn getShapePartitionSkinning = nullptr;
    GetShapeSkinToBoneFn getShapeSkinToBone = nullptr;
    GetShapeSkinToBoneByIndexFn getShapeSkinToBoneByIndex = nullptr;
    SegmentCountFn segmentCount = nullptr;
    GetSegmentFileFn getSegmentFile = nullptr;
    GetSegmentsFn getSegments = nullptr;
    GetSubsegmentsFn getSubsegments = nullptr;
    GetPartitionsFn getPartitions = nullptr;
    GetPartitionTrisFn getPartitionTris = nullptr;
    GetShaderAttrsFn getShaderAttrs = nullptr;
    GetEffectShaderAttrsFn getEffectShaderAttrs = nullptr;
    GetAlphaPropertyFn getAlphaProperty = nullptr;
    GetExtraDataLenFn getStringExtraDataLen = nullptr;
    GetStringExtraDataFn getStringExtraData = nullptr;
    GetExtraDataLenFn getBGExtraDataLen = nullptr;
    GetBGExtraDataFn getBGExtraData = nullptr;
    GetExtraDataLenFn getClothExtraDataLen = nullptr;
    GetStringExtraDataFn getClothExtraData = nullptr;
    GetCollisionFn getCollision = nullptr;
    GetCollBodyIdFn getCollBodyId = nullptr;
    GetCollShapeIdFn getRigidBodyShapeId = nullptr;
    GetCollShapeBlockNameFn getCollShapeBlockname = nullptr;
    GetCollShapeVertsFn getCollShapeVerts = nullptr;
    GetCollShapeNormalsFn getCollShapeNormals = nullptr;
    GetCollDataIdFn getCollCompressedMeshShapeDataId = nullptr;
    GetCollShapeVertsFn getCollCompressedMeshShapeVerts = nullptr;
    GetCollShapeTrisFn getCollCompressedMeshShapeTris = nullptr;
    GetCollDataIdFn getCollPackedStripsDataId = nullptr;
    GetCollShapeVertsFn getCollPackedStripsShapeVerts = nullptr;
    GetCollShapeTrisFn getCollPackedStripsShapeTris = nullptr;

    NiflyApi() = default;
    NiflyApi(const NiflyApi&) = delete;
    NiflyApi& operator=(const NiflyApi&) = delete;
    NiflyApi(NiflyApi&& other) noexcept
        : module(other.module),
          load(other.load),
          destroy(other.destroy),
          getVersion(other.getVersion),
          getGameName(other.getGameName),
          getShapes(other.getShapes),
          getShapeName(other.getShapeName),
          getShapeBlockName(other.getShapeBlockName),
          getBlockId(other.getBlockId),
          getVertsForShape(other.getVertsForShape),
          getNormalsForShape(other.getNormalsForShape),
          getTangentsForShape(other.getTangentsForShape),
          getBitangentsForShape(other.getBitangentsForShape),
          getTriangles(other.getTriangles),
          getUVs(other.getUVs),
          getColorsForShape(other.getColorsForShape),
          getShaderTextureSlot(other.getShaderTextureSlot),
          getShaderBlockName(other.getShaderBlockName),
          getShaderType(other.getShaderType),
          getShaderFlags1(other.getShaderFlags1),
          getShaderFlags2(other.getShaderFlags2),
          getMessageLog(other.getMessageLog),
          clearMessageLog(other.clearMessageLog),
          getTransform(other.getTransform),
          getNodeTransform(other.getNodeTransform),
          getNodeCount(other.getNodeCount),
          getNodes(other.getNodes),
          getNodeName(other.getNodeName),
          getNodeBlockname(other.getNodeBlockname),
          getNodeFlags(other.getNodeFlags),
          getNodeParent(other.getNodeParent),
          getNodeSwitchIndex(other.getNodeSwitchIndex),
          getNodeChildCount(other.getNodeChildCount),
          getNodeChildIDByIndex(other.getNodeChildIDByIndex),
          getShapeBoneCount(other.getShapeBoneCount),
          getShapeBoneNames(other.getShapeBoneNames),
          getShapeBoneWeightsCount(other.getShapeBoneWeightsCount),
          getShapeBoneWeights(other.getShapeBoneWeights),
          getShapeSkinBoneCount(other.getShapeSkinBoneCount),
          getShapeSkinBoneNames(other.getShapeSkinBoneNames),
          getShapeSkinWeightsCount(other.getShapeSkinWeightsCount),
          getShapeSkinWeights(other.getShapeSkinWeights),
          getShapePartitionSkinning(other.getShapePartitionSkinning),
          getShapeSkinToBone(other.getShapeSkinToBone),
          getShapeSkinToBoneByIndex(other.getShapeSkinToBoneByIndex),
          segmentCount(other.segmentCount),
          getSegmentFile(other.getSegmentFile),
          getSegments(other.getSegments),
          getSubsegments(other.getSubsegments),
          getPartitions(other.getPartitions),
          getPartitionTris(other.getPartitionTris),
          getShaderAttrs(other.getShaderAttrs),
          getEffectShaderAttrs(other.getEffectShaderAttrs),
          getAlphaProperty(other.getAlphaProperty),
          getStringExtraDataLen(other.getStringExtraDataLen),
          getStringExtraData(other.getStringExtraData),
          getBGExtraDataLen(other.getBGExtraDataLen),
          getBGExtraData(other.getBGExtraData),
          getClothExtraDataLen(other.getClothExtraDataLen),
          getClothExtraData(other.getClothExtraData),
          getCollision(other.getCollision),
          getCollBodyId(other.getCollBodyId),
          getRigidBodyShapeId(other.getRigidBodyShapeId),
          getCollShapeBlockname(other.getCollShapeBlockname),
          getCollShapeVerts(other.getCollShapeVerts),
          getCollShapeNormals(other.getCollShapeNormals),
          getCollCompressedMeshShapeDataId(other.getCollCompressedMeshShapeDataId),
          getCollCompressedMeshShapeVerts(other.getCollCompressedMeshShapeVerts),
          getCollCompressedMeshShapeTris(other.getCollCompressedMeshShapeTris),
          getCollPackedStripsDataId(other.getCollPackedStripsDataId),
          getCollPackedStripsShapeVerts(other.getCollPackedStripsShapeVerts),
          getCollPackedStripsShapeTris(other.getCollPackedStripsShapeTris)
    {
        other.module = nullptr;
    }

    NiflyApi& operator=(NiflyApi&& other) noexcept
    {
        if (this != &other) {
            if (module) {
                FreeLibrary(module);
            }
            module = other.module;
            load = other.load;
            destroy = other.destroy;
            getVersion = other.getVersion;
            getGameName = other.getGameName;
            getShapes = other.getShapes;
            getShapeName = other.getShapeName;
            getShapeBlockName = other.getShapeBlockName;
            getBlockId = other.getBlockId;
            getVertsForShape = other.getVertsForShape;
            getNormalsForShape = other.getNormalsForShape;
            getTangentsForShape = other.getTangentsForShape;
            getBitangentsForShape = other.getBitangentsForShape;
            getTriangles = other.getTriangles;
            getUVs = other.getUVs;
            getColorsForShape = other.getColorsForShape;
            getShaderTextureSlot = other.getShaderTextureSlot;
            getShaderBlockName = other.getShaderBlockName;
            getShaderType = other.getShaderType;
            getShaderFlags1 = other.getShaderFlags1;
            getShaderFlags2 = other.getShaderFlags2;
            getMessageLog = other.getMessageLog;
            clearMessageLog = other.clearMessageLog;
            getTransform = other.getTransform;
            getNodeTransform = other.getNodeTransform;
            getNodeCount = other.getNodeCount;
            getNodes = other.getNodes;
            getNodeName = other.getNodeName;
            getNodeBlockname = other.getNodeBlockname;
            getNodeFlags = other.getNodeFlags;
            getNodeParent = other.getNodeParent;
            getNodeSwitchIndex = other.getNodeSwitchIndex;
            getNodeChildCount = other.getNodeChildCount;
            getNodeChildIDByIndex = other.getNodeChildIDByIndex;
            getShapeBoneCount = other.getShapeBoneCount;
            getShapeBoneNames = other.getShapeBoneNames;
            getShapeBoneWeightsCount = other.getShapeBoneWeightsCount;
            getShapeBoneWeights = other.getShapeBoneWeights;
            getShapeSkinBoneCount = other.getShapeSkinBoneCount;
            getShapeSkinBoneNames = other.getShapeSkinBoneNames;
            getShapeSkinWeightsCount = other.getShapeSkinWeightsCount;
            getShapeSkinWeights = other.getShapeSkinWeights;
            getShapePartitionSkinning = other.getShapePartitionSkinning;
            getShapeSkinToBone = other.getShapeSkinToBone;
            getShapeSkinToBoneByIndex = other.getShapeSkinToBoneByIndex;
            segmentCount = other.segmentCount;
            getSegmentFile = other.getSegmentFile;
            getSegments = other.getSegments;
            getSubsegments = other.getSubsegments;
            getPartitions = other.getPartitions;
            getPartitionTris = other.getPartitionTris;
            getShaderAttrs = other.getShaderAttrs;
            getEffectShaderAttrs = other.getEffectShaderAttrs;
            getAlphaProperty = other.getAlphaProperty;
            getStringExtraDataLen = other.getStringExtraDataLen;
            getStringExtraData = other.getStringExtraData;
            getBGExtraDataLen = other.getBGExtraDataLen;
            getBGExtraData = other.getBGExtraData;
            getClothExtraDataLen = other.getClothExtraDataLen;
            getClothExtraData = other.getClothExtraData;
            getCollision = other.getCollision;
            getCollBodyId = other.getCollBodyId;
            getRigidBodyShapeId = other.getRigidBodyShapeId;
            getCollShapeBlockname = other.getCollShapeBlockname;
            getCollShapeVerts = other.getCollShapeVerts;
            getCollShapeNormals = other.getCollShapeNormals;
            getCollCompressedMeshShapeDataId = other.getCollCompressedMeshShapeDataId;
            getCollCompressedMeshShapeVerts = other.getCollCompressedMeshShapeVerts;
            getCollCompressedMeshShapeTris = other.getCollCompressedMeshShapeTris;
            getCollPackedStripsDataId = other.getCollPackedStripsDataId;
            getCollPackedStripsShapeVerts = other.getCollPackedStripsShapeVerts;
            getCollPackedStripsShapeTris = other.getCollPackedStripsShapeTris;
            other.module = nullptr;
        }
        return *this;
    }

    ~NiflyApi()
    {
        if (module) {
            FreeLibrary(module);
        }
    }
};

std::optional<fs::path> FindNiflyDll(const char* argv0)
{
    std::vector<fs::path> candidates;

#ifdef _WIN32
    char* envPath = nullptr;
    size_t envPathLength = 0;
    if (_dupenv_s(&envPath, &envPathLength, "NIFLYDLL_PATH") == 0 && envPath) {
        candidates.emplace_back(envPath);
        std::free(envPath);
    }
#else
    if (const char* envPath = std::getenv("NIFLYDLL_PATH")) {
        candidates.emplace_back(envPath);
    }
#endif

    const fs::path exePath = fs::absolute(argv0).parent_path();
    candidates.push_back(exePath / "NiflyDLL.dll");
    candidates.push_back(exePath / ".." / "PyNifly" / "NiflyDLL.dll");
    candidates.push_back(exePath / ".." / ".." / ".." / ".." / "PyNifly" / "NiflyDLL" / "out" / "build" / "x64-Release-vs" / "bin" / "Release" / "NiflyDLL.dll");
    candidates.push_back(exePath / ".." / "PyNifly" / "NiflyDLL" / "out" / "build" / "x64-Debug" / "NiflyDLL.dll");
    candidates.push_back(exePath / ".." / "PyNifly" / "NiflyDLL" / "out" / "build" / "x64-Release" / "NiflyDLL.dll");

    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return fs::weakly_canonical(candidate, ec);
        }
    }

    return std::nullopt;
}

template <typename Fn>
Fn LoadProc(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

std::optional<NiflyApi> LoadNiflyApi(const char* argv0, std::vector<Diagnostic>& diagnostics)
{
    auto dllPath = FindNiflyDll(argv0);
    if (!dllPath) {
        AddDiagnostic(diagnostics, "error", "NiflyDLL.dll was not found. Set NIFLYDLL_PATH or place it next to BRNifly.exe.");
        return std::nullopt;
    }

    HMODULE module = LoadLibraryW(dllPath->wstring().c_str());
    if (!module) {
        AddDiagnostic(diagnostics, "error", "Failed to load NiflyDLL.dll from " + dllPath->string());
        return std::nullopt;
    }

    NiflyApi api{};
    api.module = module;
    api.load = LoadProc<LoadFn>(module, "load");
    api.destroy = LoadProc<DestroyFn>(module, "destroy");
    api.getVersion = LoadProc<GetVersionFn>(module, "getVersion");
    api.getGameName = LoadProc<GetGameNameFn>(module, "getGameName");
    api.getShapes = LoadProc<GetShapesFn>(module, "getShapes");
    api.getShapeName = LoadProc<GetShapeNameFn>(module, "getShapeName");
    api.getShapeBlockName = LoadProc<GetShapeBlockNameFn>(module, "getShapeBlockName");
    api.getBlockId = LoadProc<GetBlockIdFn>(module, "getBlockID");
    if (!api.getBlockId) {
        api.getBlockId = LoadProc<GetBlockIdFn>(module, "getBlockId");
    }
    api.getVertsForShape = LoadProc<GetVertsForShapeFn>(module, "getVertsForShape");
    api.getNormalsForShape = LoadProc<GetNormalsForShapeFn>(module, "getNormalsForShape");
    api.getTangentsForShape = LoadProc<GetTangentsForShapeFn>(module, "getTangentsForShape");
    api.getBitangentsForShape = LoadProc<GetTangentsForShapeFn>(module, "getBitangentsForShape");
    api.getTriangles = LoadProc<GetTrianglesFn>(module, "getTriangles");
    api.getUVs = LoadProc<GetUVsFn>(module, "getUVs");
    api.getColorsForShape = LoadProc<GetColorsForShapeFn>(module, "getColorsForShape");
    api.getShaderTextureSlot = LoadProc<GetShaderTextureSlotFn>(module, "getShaderTextureSlot");
    api.getShaderBlockName = LoadProc<GetShaderBlockNameFn>(module, "getShaderBlockName");
    api.getShaderType = LoadProc<GetShaderTypeFn>(module, "getShaderType");
    api.getShaderFlags1 = LoadProc<GetShaderFlagsFn>(module, "getShaderFlags1");
    api.getShaderFlags2 = LoadProc<GetShaderFlagsFn>(module, "getShaderFlags2");
    api.getMessageLog = LoadProc<GetMessageLogFn>(module, "getMessageLog");
    api.clearMessageLog = LoadProc<ClearMessageLogFn>(module, "clearMessageLog");
    api.getTransform = LoadProc<GetTransformFn>(module, "getTransform");
    api.getNodeTransform = LoadProc<GetTransformFn>(module, "getNodeTransform");
    api.getNodeCount = LoadProc<GetNodeCountFn>(module, "getNodeCount");
    api.getNodes = LoadProc<GetNodesFn>(module, "getNodes");
    api.getNodeName = LoadProc<GetNodeNameFn>(module, "getNodeName");
    api.getNodeBlockname = LoadProc<GetNodeBlocknameFn>(module, "getNodeBlockname");
    api.getNodeFlags = LoadProc<GetNodeFlagsFn>(module, "getNodeFlags");
    api.getNodeParent = LoadProc<GetNodeParentFn>(module, "getNodeParent");
    api.getNodeSwitchIndex = LoadProc<GetNodeSwitchIndexFn>(module, "getNodeSwitchIndex");
    api.getNodeChildCount = LoadProc<GetNodeChildCountFn>(module, "getNodeChildCount");
    api.getNodeChildIDByIndex = LoadProc<GetNodeChildIDByIndexFn>(module, "getNodeChildIDByIndex");
    api.getShapeBoneCount = LoadProc<GetShapeBoneCountFn>(module, "getShapeBoneCount");
    api.getShapeBoneNames = LoadProc<GetShapeBoneNamesFn>(module, "getShapeBoneNames");
    api.getShapeBoneWeightsCount = LoadProc<GetShapeBoneWeightsCountFn>(module, "getShapeBoneWeightsCount");
    api.getShapeBoneWeights = LoadProc<GetShapeBoneWeightsFn>(module, "getShapeBoneWeights");
    api.getShapeSkinBoneCount = LoadProc<GetShapeSkinBoneCountFn>(module, "getShapeSkinBoneCount");
    api.getShapeSkinBoneNames = LoadProc<GetShapeSkinBoneNamesFn>(module, "getShapeSkinBoneNames");
    api.getShapeSkinWeightsCount = LoadProc<GetShapeSkinWeightsCountFn>(module, "getShapeSkinWeightsCount");
    api.getShapeSkinWeights = LoadProc<GetShapeSkinWeightsFn>(module, "getShapeSkinWeights");
    api.getShapePartitionSkinning = LoadProc<GetShapePartitionSkinningFn>(module, "getShapePartitionSkinning");
    api.getShapeSkinToBone = LoadProc<GetShapeSkinToBoneFn>(module, "getShapeSkinToBone");
    api.getShapeSkinToBoneByIndex = LoadProc<GetShapeSkinToBoneByIndexFn>(module, "getShapeSkinToBoneByIndex");
    api.segmentCount = LoadProc<SegmentCountFn>(module, "segmentCount");
    api.getSegmentFile = LoadProc<GetSegmentFileFn>(module, "getSegmentFile");
    api.getSegments = LoadProc<GetSegmentsFn>(module, "getSegments");
    api.getSubsegments = LoadProc<GetSubsegmentsFn>(module, "getSubsegments");
    api.getPartitions = LoadProc<GetPartitionsFn>(module, "getPartitions");
    api.getPartitionTris = LoadProc<GetPartitionTrisFn>(module, "getPartitionTris");
    api.getShaderAttrs = LoadProc<GetShaderAttrsFn>(module, "getShaderAttrs");
    api.getEffectShaderAttrs = LoadProc<GetEffectShaderAttrsFn>(module, "getEffectShaderAttrs");
    api.getAlphaProperty = LoadProc<GetAlphaPropertyFn>(module, "getAlphaProperty");
    api.getStringExtraDataLen = LoadProc<GetExtraDataLenFn>(module, "getStringExtraDataLen");
    api.getStringExtraData = LoadProc<GetStringExtraDataFn>(module, "getStringExtraData");
    api.getBGExtraDataLen = LoadProc<GetExtraDataLenFn>(module, "getBGExtraDataLen");
    api.getBGExtraData = LoadProc<GetBGExtraDataFn>(module, "getBGExtraData");
    api.getClothExtraDataLen = LoadProc<GetExtraDataLenFn>(module, "getClothExtraDataLen");
    api.getClothExtraData = LoadProc<GetStringExtraDataFn>(module, "getClothExtraData");
    api.getCollision = LoadProc<GetCollisionFn>(module, "getCollTarget");
    if (!api.getCollision) {
        api.getCollision = LoadProc<GetCollisionFn>(module, "getCollision");
    }
    api.getCollBodyId = LoadProc<GetCollBodyIdFn>(module, "getCollBodyID");
    api.getRigidBodyShapeId = LoadProc<GetCollShapeIdFn>(module, "getRigidBodyShapeID");
    api.getCollShapeBlockname = LoadProc<GetCollShapeBlockNameFn>(module, "getCollShapeBlockname");
    api.getCollShapeVerts = LoadProc<GetCollShapeVertsFn>(module, "getCollShapeVerts");
    api.getCollShapeNormals = LoadProc<GetCollShapeNormalsFn>(module, "getCollShapeNormals");
    api.getCollCompressedMeshShapeDataId = LoadProc<GetCollDataIdFn>(module, "getCollCompressedMeshShapeDataID");
    api.getCollCompressedMeshShapeVerts = LoadProc<GetCollShapeVertsFn>(module, "getCollCompressedMeshShapeVerts");
    api.getCollCompressedMeshShapeTris = LoadProc<GetCollShapeTrisFn>(module, "getCollCompressedMeshShapeTris");
    api.getCollPackedStripsDataId = LoadProc<GetCollDataIdFn>(module, "getCollPackedStripsDataID");
    api.getCollPackedStripsShapeVerts = LoadProc<GetCollShapeVertsFn>(module, "getCollPackedStripsShapeVerts");
    api.getCollPackedStripsShapeTris = LoadProc<GetCollShapeTrisFn>(module, "getCollPackedStripsShapeTris");

    if (!api.load || !api.destroy || !api.getShapes || !api.getShapeName || !api.getVertsForShape || !api.getTriangles) {
        AddDiagnostic(diagnostics, "error", "NiflyDLL.dll is missing required geometry entry points.");
        return std::nullopt;
    }

    AddDiagnostic(diagnostics, "info", "Loaded NiflyDLL.dll from " + dllPath->string());
    return api;
}
#else
struct NiflyApi {};
std::optional<NiflyApi> LoadNiflyApi(const char*, std::vector<Diagnostic>& diagnostics)
{
    AddDiagnostic(diagnostics, "error", "BRNifly currently supports niflyDLL loading on Windows only.");
    return std::nullopt;
}
#endif

std::string VersionString(const NiflyApi& api)
{
#ifdef _WIN32
    if (!api.getVersion) {
        return "unknown";
    }
    const int* version = api.getVersion();
    if (!version) {
        return "unknown";
    }
    return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." + std::to_string(version[2]);
#else
    (void)api;
    return "unavailable";
#endif
}

std::string GetMessageLog(const NiflyApi& api)
{
#ifdef _WIN32
    if (!api.getMessageLog) {
        return {};
    }
    std::array<char, 8192> buffer{};
    const int written = api.getMessageLog(buffer.data(), static_cast<int>(buffer.size()));
    if (written <= 0) {
        return {};
    }
    return std::string(buffer.data());
#else
    (void)api;
    return {};
#endif
}

json DescribeServicesJson(const char* argv0)
{
    std::vector<Diagnostic> diagnostics;
    std::string niflyVersion = "unavailable";
    if (auto api = LoadNiflyApi(argv0, diagnostics)) {
        niflyVersion = VersionString(*api);
    }

    json response;
    response["status"] = "ok";
    response["protocolVersion"] = "1.0";
    response["niflyVersion"] = niflyVersion;
    response["openUsdVersion"] = std::to_string(PXR_MAJOR_VERSION) + "." +
        std::to_string(PXR_MINOR_VERSION) + "." + std::to_string(PXR_PATCH_VERSION);
    response["services"] = ServiceList();
    response["supportedGames"] = SupportedGames();
    response["preferredPayload"] = "usda-text";
    response["diagnostics"] = json::array();
    for (const Diagnostic& diagnostic : diagnostics) {
        response["diagnostics"].push_back(DiagnosticJson(diagnostic));
    }
    return response;
}

struct ShapeData {
    void* handle = nullptr;
    int blockId = -1;
    int parentBlockId = -1;
    std::string name;
    std::string blockName;
    TransformBuf transform;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> tangents;
    std::vector<float> uvs;
    std::vector<float> colors;
    std::vector<uint16_t> triangles;
    std::vector<std::string> textures;
    std::vector<std::string> boneNames;
    std::vector<int> boneSourceIndices;
    std::vector<float> skinToBoneTransforms;
    std::vector<int> jointIndices;
    std::vector<float> jointWeights;
    json shader = json::object();
    json alpha = json::object();
    json segmentation = json::object();
    json extraData = json::array();
};

float Dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> Cross3(const float* a, const float* b)
{
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0]
    };
}

void Normalize3(float* v)
{
    const float lenSq = Dot3(v, v);
    if (lenSq <= 1.0e-20f) {
        v[0] = 1.0f;
        v[1] = 0.0f;
        v[2] = 0.0f;
        return;
    }

    const float invLen = 1.0f / std::sqrt(lenSq);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
}

struct NodeData {
    void* handle = nullptr;
    int blockId = -1;
    int parentBlockId = -1;
    std::string name;
    std::string blockName;
    int flags = 0;
    int switchIndex = -1;
    int lod0ChildBlockId = -1;
    TransformBuf transform;
    SdfPath path;
};

struct CollisionProxyData {
    std::string name;
    std::string blockName;
    int sourceNodeId = -1;
    int bodyId = -1;
    int shapeId = -1;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint16_t> triangles;
};

std::optional<std::string> ReadCString(std::function<int(char*, int)> reader)
{
    std::array<char, 1024> buffer{};
    int count = reader(buffer.data(), static_cast<int>(buffer.size()));
    if (count <= 0) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

GfMatrix4d ToUsdMatrix(const TransformBuf& transform)
{
    GfMatrix4d matrix(1.0);
    matrix[0][0] = transform.rotation[0] * transform.scale;
    matrix[0][1] = transform.rotation[1] * transform.scale;
    matrix[0][2] = transform.rotation[2] * transform.scale;
    matrix[1][0] = transform.rotation[3] * transform.scale;
    matrix[1][1] = transform.rotation[4] * transform.scale;
    matrix[1][2] = transform.rotation[5] * transform.scale;
    matrix[2][0] = transform.rotation[6] * transform.scale;
    matrix[2][1] = transform.rotation[7] * transform.scale;
    matrix[2][2] = transform.rotation[8] * transform.scale;
    matrix[3][0] = transform.translation[0];
    matrix[3][1] = transform.translation[1];
    matrix[3][2] = transform.translation[2];
    return matrix;
}

void ApplyTransform(const UsdPrim& prim, const TransformBuf& transform)
{
    UsdGeomXformable xformable(prim);
    if (!xformable) {
        return;
    }
    xformable.AddTransformOp().Set(ToUsdMatrix(transform));
}

json TransformJson(const TransformBuf& transform)
{
    return json{
        {"translation", {transform.translation[0], transform.translation[1], transform.translation[2]}},
        {"rotation", {
            transform.rotation[0], transform.rotation[1], transform.rotation[2],
            transform.rotation[3], transform.rotation[4], transform.rotation[5],
            transform.rotation[6], transform.rotation[7], transform.rotation[8]}},
        {"scale", transform.scale}
    };
}

std::string ReadCStringDynamic(std::function<int(char*, int)> reader)
{
    const int required = reader(nullptr, 0);
    const int capacity = std::max(required + 1, 1024);
    std::vector<char> buffer(static_cast<size_t>(capacity));
    const int written = reader(buffer.data(), capacity);
    if (written <= 0) {
        return {};
    }
    return std::string(buffer.data());
}

json ReadExtraDataList(
    const NiflyApi& api,
    void* nifHandle,
    void* ownerHandle)
{
    json entries = json::array();
    auto readTextEntries = [&](const char* kind, GetExtraDataLenFn getLen, GetStringExtraDataFn getValue) {
        if (!getLen || !getValue) {
            return;
        }
        for (int index = 0; index < 256; ++index) {
            int nameLen = 0;
            int valueLen = 0;
            if (!getLen(nifHandle, ownerHandle, index, &nameLen, &valueLen)) {
                break;
            }
            std::vector<char> name(static_cast<size_t>(std::max(nameLen + 1, 1)));
            std::vector<char> value(static_cast<size_t>(std::max(valueLen + 1, 1)));
            if (getValue(nifHandle, ownerHandle, index, name.data(), static_cast<int>(name.size()), value.data(), static_cast<int>(value.size()))) {
                entries.push_back(json{{"kind", kind}, {"name", std::string(name.data())}, {"value", std::string(value.data())}});
            }
        }
    };

    readTextEntries("NiStringExtraData", api.getStringExtraDataLen, api.getStringExtraData);

    if (api.getBGExtraDataLen && api.getBGExtraData) {
        for (int index = 0; index < 256; ++index) {
            int nameLen = 0;
            int valueLen = 0;
            if (!api.getBGExtraDataLen(nifHandle, ownerHandle, index, &nameLen, &valueLen)) {
                break;
            }
            std::vector<char> name(static_cast<size_t>(std::max(nameLen + 1, 1)));
            std::vector<char> value(static_cast<size_t>(std::max(valueLen + 1, 1)));
            uint16_t controlsBaseSkeleton = 0;
            if (api.getBGExtraData(nifHandle, ownerHandle, index, name.data(), static_cast<int>(name.size()), value.data(), static_cast<int>(value.size()), &controlsBaseSkeleton)) {
                entries.push_back(json{{"kind", "BSBehaviorGraphExtraData"}, {"name", std::string(name.data())}, {"value", std::string(value.data())}, {"controlsBaseSkeleton", controlsBaseSkeleton != 0}});
            }
        }
    }

    if (api.getClothExtraDataLen && api.getClothExtraData) {
        for (int index = 0; index < 256; ++index) {
            int nameLen = 0;
            int valueLen = 0;
            if (!api.getClothExtraDataLen(nifHandle, ownerHandle, index, &nameLen, &valueLen)) {
                break;
            }
            std::vector<char> name(static_cast<size_t>(std::max(nameLen + 1, 1)));
            std::vector<char> value(static_cast<size_t>(std::max(valueLen, 1)));
            if (api.getClothExtraData(nifHandle, ownerHandle, index, name.data(), static_cast<int>(name.size()), value.data(), static_cast<int>(value.size()))) {
                entries.push_back(json{{"kind", "BSClothExtraData"}, {"name", std::string(name.data())}, {"encoding", "hex"}, {"value", HexBytes(value)}});
            }
        }
    }

    return entries;
}

#ifdef _WIN32
std::vector<NodeData> ReadNodes(const NiflyApi& api, void* nifHandle)
{
    if (!api.getNodeCount || !api.getNodes || !api.getBlockId) {
        return {};
    }

    const int nodeCount = api.getNodeCount(nifHandle);
    if (nodeCount <= 0) {
        return {};
    }

    std::vector<void*> nodeHandles(static_cast<size_t>(nodeCount));
    api.getNodes(nifHandle, nodeHandles.data());

    std::vector<NodeData> nodes;
    nodes.reserve(nodeHandles.size());
    for (size_t index = 0; index < nodeHandles.size(); ++index) {
        void* nodeHandle = nodeHandles[index];
        if (!nodeHandle) {
            continue;
        }

        NodeData node{};
        node.handle = nodeHandle;
        node.blockId = api.getBlockId(nifHandle, nodeHandle);
        node.name = api.getNodeName
            ? ReadCString([&](char* buffer, int size) { return api.getNodeName(nodeHandle, buffer, size); }).value_or("Node_" + std::to_string(index))
            : "Node_" + std::to_string(index);
        node.blockName = api.getNodeBlockname
            ? ReadCString([&](char* buffer, int size) { return api.getNodeBlockname(nodeHandle, buffer, size); }).value_or("")
            : std::string();
        node.flags = api.getNodeFlags ? api.getNodeFlags(nodeHandle) : 0;
        if (api.getNodeSwitchIndex) {
            node.switchIndex = api.getNodeSwitchIndex(nodeHandle);
        }
        if (node.blockName == "NiSwitchNode" || node.blockName == "NiLODNode") {
            if (api.getNodeChildCount && api.getNodeChildIDByIndex && api.getNodeChildCount(nodeHandle) > 0) {
                node.lod0ChildBlockId = api.getNodeChildIDByIndex(nodeHandle, 0);
            }
        }
        if (api.getNodeTransform) {
            api.getNodeTransform(nodeHandle, &node.transform);
        }
        if (api.getNodeParent) {
            if (void* parent = api.getNodeParent(nifHandle, nodeHandle)) {
                node.parentBlockId = api.getBlockId(nifHandle, parent);
            }
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

void ReadShapeSkinning(const NiflyApi& api, void* nifHandle, void* shapeHandle, int vertexCount, ShapeData& shape)
{
    auto readSkinToBoneTransforms = [&](std::span<const int> originalBoneIndices = {}) {
        shape.skinToBoneTransforms.clear();
        if (shape.boneNames.empty()) {
            return;
        }

        constexpr size_t kTransformFloatCount = 13;
        shape.skinToBoneTransforms.reserve(shape.boneNames.size() * kTransformFloatCount);
        std::array<float, kTransformFloatCount> xform{};
        for (size_t boneIndex = 0; boneIndex < shape.boneNames.size(); ++boneIndex) {
            xform.fill(0.0f);
            bool hasTransform = false;
            if (api.getShapeSkinToBoneByIndex && boneIndex < originalBoneIndices.size()) {
                // Partition skinning compacts NiSkinData indices for BasicRenderer's
                // single mesh palette. Keep skin-to-bone binds tied to the original
                // NiSkinData slot, since name lookup is ambiguous on actor assets with
                // duplicated or side-remapped bone display names.
                hasTransform = api.getShapeSkinToBoneByIndex(nifHandle, shapeHandle, originalBoneIndices[boneIndex], xform.data());
            }
            if (!hasTransform && api.getShapeSkinToBone) {
                const auto& boneName = shape.boneNames[boneIndex];
                hasTransform = api.getShapeSkinToBone(nifHandle, shapeHandle, boneName.c_str(), xform.data());
            }
            if (hasTransform) {
                shape.skinToBoneTransforms.insert(shape.skinToBoneTransforms.end(), xform.begin(), xform.end());
            } else {
                shape.skinToBoneTransforms.clear();
                return;
            }
        }
    };

    if (api.getShapePartitionSkinning && api.getShapeSkinBoneCount && api.getShapeSkinBoneNames) {
        const int boneCount = api.getShapeSkinBoneCount(nifHandle, shapeHandle);
        if (boneCount > 0 && vertexCount > 0) {
            std::vector<uint16_t> partitionJointIndices(static_cast<size_t>(vertexCount) * 4u, 0);
            std::vector<float> partitionJointWeights(static_cast<size_t>(vertexCount) * 4u, 0.0f);
            // Skyrim loads NiSkinPartition bone indices as local palette slots and remaps
            // them through Partition::bones before indexing NiSkinData/boneWorldTransforms.
            const int touchedVertices = api.getShapePartitionSkinning(
                nifHandle,
                shapeHandle,
                partitionJointIndices.data(),
                partitionJointWeights.data(),
                vertexCount);
            if (touchedVertices > 0) {
                const std::string namesText = ReadCStringDynamic([&](char* buffer, int size) {
                    return api.getShapeSkinBoneNames(nifHandle, shapeHandle, buffer, size);
                });
                std::vector<std::string> boneNames = SplitLines(namesText);
                if (boneNames.empty()) {
                    for (int i = 0; i < boneCount; ++i) {
                        boneNames.push_back("bone_" + std::to_string(i));
                    }
                }

                std::vector<int> jointIndices;
                jointIndices.reserve(partitionJointIndices.size());
                std::vector<int> globalToCompact(static_cast<size_t>(boneCount), -1);
                std::vector<std::string> compactBoneNames;
                std::vector<int> compactToGlobalBoneIndices;

                for (size_t i = 0; i < partitionJointIndices.size(); ++i) {
                    const uint16_t globalBone = partitionJointIndices[i];
                    if (i < partitionJointWeights.size() && partitionJointWeights[i] <= 0.0f) {
                        jointIndices.push_back(0);
                        continue;
                    }
                    if (globalBone >= static_cast<uint16_t>(boneCount)) {
                        jointIndices.push_back(0);
                        continue;
                    }

                    int& compactBone = globalToCompact[globalBone];
                    if (compactBone < 0) {
                        compactBone = static_cast<int>(compactBoneNames.size());
                        compactToGlobalBoneIndices.push_back(static_cast<int>(globalBone));
                        if (globalBone < boneNames.size() && !boneNames[globalBone].empty()) {
                            compactBoneNames.push_back(boneNames[globalBone]);
                        } else {
                            compactBoneNames.push_back("bone_" + std::to_string(globalBone));
                        }
                    }
                    jointIndices.push_back(compactBone);
                }

                // The game renders each skin partition through a compact palette of
                // bones actually used by that partition. BasicRenderer uses one mesh
                // palette, so collapse NiSkinData indices to the used set instead of
                // exporting the full shape bone list; otherwise vertices can index a
                // different palette than Skyrim's live CBuffer order.
                shape.boneNames = std::move(compactBoneNames);
                shape.boneSourceIndices = std::move(compactToGlobalBoneIndices);
                readSkinToBoneTransforms(compactToGlobalBoneIndices);
                shape.jointIndices = std::move(jointIndices);
                shape.jointWeights = std::move(partitionJointWeights);
                return;
            }
        }
    }

    // Fallback for older NiflyDLL builds and non-partitioned skins. The raw BSTriShape
    // weightBones stream can be in partition-local palette space on Skyrim SSE assets,
    // so it is intentionally lower priority than getShapePartitionSkinning above.
    const bool hasShapeWeightApi =
        api.getShapeBoneCount &&
        api.getShapeBoneNames &&
        api.getShapeBoneWeightsCount &&
        api.getShapeBoneWeights &&
        api.getShapeBoneCount(nifHandle, shapeHandle) > 0;
    const bool hasSkinDataFallbackApi =
        api.getShapeSkinBoneCount &&
        api.getShapeSkinBoneNames &&
        api.getShapeSkinWeightsCount &&
        api.getShapeSkinWeights &&
        api.getShapeSkinBoneCount(nifHandle, shapeHandle) > 0;

    auto readWeights = [&](auto getBoneCount, auto getBoneNames, auto getWeightsCount, auto getWeights) {
        const int boneCount = getBoneCount(nifHandle, shapeHandle);
        if (boneCount <= 0 || vertexCount <= 0) {
            return false;
        }

        const std::string namesText = ReadCStringDynamic([&](char* buffer, int size) { return getBoneNames(nifHandle, shapeHandle, buffer, size); });
        std::vector<std::string> boneNames = SplitLines(namesText);
        if (boneNames.empty()) {
            for (int i = 0; i < boneCount; ++i) {
                boneNames.push_back("bone_" + std::to_string(i));
            }
        }

        struct Influence { int joint = 0; float weight = 0.0f; };
        std::vector<std::vector<Influence>> influences(static_cast<size_t>(vertexCount));
        for (int boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            const int weightCount = getWeightsCount(nifHandle, shapeHandle, boneIndex);
            if (weightCount <= 0) {
                continue;
            }
            std::vector<VertexWeightPair> weights(static_cast<size_t>(weightCount));
            const int written = getWeights(nifHandle, shapeHandle, boneIndex, weights.data(), weightCount);
            for (int i = 0; i < written && i < weightCount; ++i) {
                const uint16_t vertex = weights[static_cast<size_t>(i)].vertex;
                if (vertex < influences.size() && weights[static_cast<size_t>(i)].weight > 0.0f) {
                    influences[vertex].push_back(Influence{boneIndex, weights[static_cast<size_t>(i)].weight});
                }
            }
        }

        std::vector<int> jointIndices(static_cast<size_t>(vertexCount) * 4u, 0);
        std::vector<float> jointWeights(static_cast<size_t>(vertexCount) * 4u, 0.0f);
        bool hasAnyWeights = false;
        for (size_t vertex = 0; vertex < influences.size(); ++vertex) {
            auto& vertexInfluences = influences[vertex];
            std::sort(vertexInfluences.begin(), vertexInfluences.end(), [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
            const size_t count = std::min<size_t>(4, vertexInfluences.size());
            float total = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                total += vertexInfluences[i].weight;
            }
            if (total <= 0.0f) {
                continue;
            }
            hasAnyWeights = true;
            for (size_t i = 0; i < count; ++i) {
                jointIndices[vertex * 4u + i] = vertexInfluences[i].joint;
                jointWeights[vertex * 4u + i] = vertexInfluences[i].weight / total;
            }
        }
        if (!hasAnyWeights) {
            return false;
        }

        shape.boneNames = std::move(boneNames);
        shape.boneSourceIndices.clear();
        shape.boneSourceIndices.reserve(static_cast<size_t>(boneCount));
        for (int boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            shape.boneSourceIndices.push_back(boneIndex);
        }
        readSkinToBoneTransforms();
        shape.jointIndices = std::move(jointIndices);
        shape.jointWeights = std::move(jointWeights);
        return true;
    };

    if (hasShapeWeightApi && readWeights(api.getShapeBoneCount, api.getShapeBoneNames, api.getShapeBoneWeightsCount, api.getShapeBoneWeights)) {
        return;
    }
    if (hasSkinDataFallbackApi && readWeights(api.getShapeSkinBoneCount, api.getShapeSkinBoneNames, api.getShapeSkinWeightsCount, api.getShapeSkinWeights)) {
        return;
    }
}

json ReadShapeSegmentation(const NiflyApi& api, void* nifHandle, void* shapeHandle)
{
    json segmentation = json::object();
    if (api.getSegmentFile) {
        const std::string segmentFile = ReadCStringDynamic([&](char* buffer, int size) { return api.getSegmentFile(nifHandle, shapeHandle, buffer, size); });
        if (!segmentFile.empty()) {
            segmentation["segmentFile"] = segmentFile;
        }
    }

    if (api.segmentCount && api.getSegments) {
        const int segmentCount = api.segmentCount(nifHandle, shapeHandle);
        if (segmentCount > 0) {
            std::vector<int> segmentData(static_cast<size_t>(segmentCount) * 2u);
            const int written = api.getSegments(nifHandle, shapeHandle, segmentData.data(), segmentCount);
            json segments = json::array();
            for (int i = 0; i < written; ++i) {
                json segment{{"id", segmentData[static_cast<size_t>(i) * 2u]}, {"subsegmentCount", segmentData[static_cast<size_t>(i) * 2u + 1u]}};
                if (api.getSubsegments && segment["subsegmentCount"].get<int>() > 0) {
                    std::vector<uint32_t> subsegments(static_cast<size_t>(segment["subsegmentCount"].get<int>()) * 3u);
                    const int subCount = api.getSubsegments(nifHandle, shapeHandle, segment["id"].get<int>(), subsegments.data(), segment["subsegmentCount"].get<int>());
                    json subJson = json::array();
                    for (int sub = 0; sub < subCount; ++sub) {
                        subJson.push_back(json{{"partId", subsegments[static_cast<size_t>(sub) * 3u]}, {"userSlot", subsegments[static_cast<size_t>(sub) * 3u + 1u]}, {"material", subsegments[static_cast<size_t>(sub) * 3u + 2u]}});
                    }
                    segment["subsegments"] = subJson;
                }
                segments.push_back(segment);
            }
            segmentation["segments"] = segments;
        }
    }

    if (api.getPartitions) {
        const int partitionCount = api.getPartitions(nifHandle, shapeHandle, nullptr, 0);
        if (partitionCount > 0) {
            std::vector<uint16_t> partitions(static_cast<size_t>(partitionCount) * 2u);
            const int written = api.getPartitions(nifHandle, shapeHandle, partitions.data(), partitionCount);
            json partitionJson = json::array();
            for (int i = 0; i < written; ++i) {
                partitionJson.push_back(json{{"bodyPart", partitions[static_cast<size_t>(i) * 2u]}, {"flags", partitions[static_cast<size_t>(i) * 2u + 1u]}});
            }
            segmentation["partitions"] = partitionJson;
        }
    }

    if (api.getPartitionTris) {
        const int triPartCount = api.getPartitionTris(nifHandle, shapeHandle, nullptr, 0);
        if (triPartCount > 0) {
            std::vector<uint16_t> triParts(static_cast<size_t>(triPartCount));
            const int written = api.getPartitionTris(nifHandle, shapeHandle, triParts.data(), triPartCount);
            json triJson = json::array();
            for (int i = 0; i < written; ++i) {
                triJson.push_back(triParts[static_cast<size_t>(i)]);
            }
            segmentation["trianglePartitionIndices"] = triJson;
        }
    }
    return segmentation;
}

std::vector<CollisionProxyData> ReadCollisionProxies(const NiflyApi& api, void* nifHandle, const std::vector<NodeData>& nodes)
{
    std::vector<CollisionProxyData> proxies;
    if (!api.getCollision || !api.getBlockId) {
        return proxies;
    }

    for (const NodeData& node : nodes) {
        void* collisionHandle = api.getCollision(nifHandle, node.handle);
        if (!collisionHandle) {
            continue;
        }

        const int collisionId = api.getBlockId(nifHandle, collisionHandle);
        auto appendProxy = [&](std::string name, int shapeId, std::vector<float> positions, std::vector<uint16_t> triangles) {
            if (positions.empty()) {
                return;
            }
            CollisionProxyData proxy{};
            proxy.name = std::move(name);
            proxy.sourceNodeId = node.blockId;
            proxy.bodyId = collisionId;
            proxy.shapeId = shapeId;
            proxy.positions = std::move(positions);
            proxy.triangles = std::move(triangles);
            if (api.getCollShapeBlockname && shapeId >= 0) {
                proxy.blockName = ReadCString([&](char* buffer, int size) { return api.getCollShapeBlockname(nifHandle, shapeId, buffer, size); }).value_or("");
            }
            proxies.push_back(std::move(proxy));
        };

        if (api.getCollShapeVerts) {
            const int vertexCount = api.getCollShapeVerts(nifHandle, collisionId, nullptr, 0);
            if (vertexCount > 0) {
                std::vector<float> verts(static_cast<size_t>(vertexCount) * 3u);
                api.getCollShapeVerts(nifHandle, collisionId, verts.data(), static_cast<int>(verts.size()));
                appendProxy(node.name + "_Collision", collisionId, std::move(verts), {});
            }
        }

        if (api.getCollCompressedMeshShapeDataId && api.getCollCompressedMeshShapeVerts && api.getCollCompressedMeshShapeTris) {
            const int dataId = api.getCollCompressedMeshShapeDataId(nifHandle, collisionId);
            if (dataId >= 0) {
                const int vertexCount = api.getCollCompressedMeshShapeVerts(nifHandle, dataId, nullptr, 0);
                const int triCount = api.getCollCompressedMeshShapeTris(nifHandle, dataId, nullptr, 0);
                if (vertexCount > 0) {
                    std::vector<float> verts(static_cast<size_t>(vertexCount) * 3u);
                    api.getCollCompressedMeshShapeVerts(nifHandle, dataId, verts.data(), static_cast<int>(verts.size()));
                    std::vector<uint16_t> tris;
                    if (triCount > 0) {
                        tris.resize(static_cast<size_t>(triCount) * 3u);
                        api.getCollCompressedMeshShapeTris(nifHandle, dataId, tris.data(), static_cast<int>(tris.size()));
                    }
                    appendProxy(node.name + "_CompressedCollision", dataId, std::move(verts), std::move(tris));
                }
            }
        }

        if (api.getCollPackedStripsDataId && api.getCollPackedStripsShapeVerts && api.getCollPackedStripsShapeTris) {
            const int dataId = api.getCollPackedStripsDataId(nifHandle, collisionId);
            if (dataId >= 0) {
                const int vertexCount = api.getCollPackedStripsShapeVerts(nifHandle, dataId, nullptr, 0);
                const int triCount = api.getCollPackedStripsShapeTris(nifHandle, dataId, nullptr, 0);
                if (vertexCount > 0) {
                    std::vector<float> verts(static_cast<size_t>(vertexCount) * 3u);
                    api.getCollPackedStripsShapeVerts(nifHandle, dataId, verts.data(), static_cast<int>(verts.size()));
                    std::vector<uint16_t> tris;
                    if (triCount > 0) {
                        tris.resize(static_cast<size_t>(triCount) * 3u);
                        api.getCollPackedStripsShapeTris(nifHandle, dataId, tris.data(), static_cast<int>(tris.size()));
                    }
                    appendProxy(node.name + "_PackedCollision", dataId, std::move(verts), std::move(tris));
                }
            }
        }
    }

    return proxies;
}

void ReadShapeMaterial(const NiflyApi& api, void* nifHandle, void* shapeHandle, ShapeData& shape)
{
    if (api.getShaderBlockName) {
        if (const char* blockName = api.getShaderBlockName(nifHandle, shapeHandle)) {
            shape.shader["blockName"] = blockName;
        }
    }
    if (api.getShaderType) {
        const uint32_t shaderType = api.getShaderType(nifHandle, shapeHandle);
        shape.shader["shaderType"] = shaderType;
        shape.shader["shaderTypeName"] = ShaderTypeName(shaderType);
    }
    if (api.getShaderFlags1) {
        const uint32_t flags = api.getShaderFlags1(nifHandle, shapeHandle);
        shape.shader["shaderFlags1"] = flags;
        shape.shader["shaderFlags1Hex"] = Hex32(flags);
        shape.shader["shaderFlags1Names"] = DecodeShaderFlags(flags, kShaderFlags1);
    }
    if (api.getShaderFlags2) {
        const uint32_t flags = api.getShaderFlags2(nifHandle, shapeHandle);
        shape.shader["shaderFlags2"] = flags;
        shape.shader["shaderFlags2Hex"] = Hex32(flags);
        shape.shader["shaderFlags2Names"] = DecodeShaderFlags(flags, kShaderFlags2);
    }
    if (api.getShaderAttrs) {
        BSLSPAttrs attrs{};
        if (api.getShaderAttrs(nifHandle, shapeHandle, &attrs) == 0) {
            shape.shader["lightingShader"] = json{
                {"uvOffset", {attrs.uvOffsetU, attrs.uvOffsetV}},
                {"uvScale", {attrs.uvScaleU, attrs.uvScaleV}},
                {"emissiveColor", {attrs.emissiveColorR, attrs.emissiveColorG, attrs.emissiveColorB, attrs.emissiveColorA}},
                {"emissiveMult", attrs.emissiveMult},
                {"environmentMapScale", attrs.environmentMapScale},
                {"alpha", attrs.alpha},
                {"glossiness", attrs.glossiness},
                {"specularColor", {attrs.specColorR, attrs.specColorG, attrs.specColorB}},
                {"specularStrength", attrs.specStrength},
                {"refractionStrength", attrs.refractionStrength},
                {"rimLightPower", attrs.rimLightPower},
                {"skinTintColor", {attrs.skinTintColorR, attrs.skinTintColorG, attrs.skinTintColorB, attrs.skinTintAlpha}}
            };
        }
    }
    if (api.getEffectShaderAttrs) {
        BSESPAttrs attrs{};
        if (api.getEffectShaderAttrs(nifHandle, shapeHandle, &attrs) == 0) {
            shape.shader["effectShader"] = json{
                {"uvOffset", {attrs.uvOffsetU, attrs.uvOffsetV}},
                {"uvScale", {attrs.uvScaleU, attrs.uvScaleV}},
                {"emissiveColor", {attrs.emissiveColorR, attrs.emissiveColorG, attrs.emissiveColorB, attrs.emissiveColorA}},
                {"emissiveMult", attrs.emissiveMult},
                {"falloffStartAngle", attrs.falloffStartAngle},
                {"falloffStopAngle", attrs.falloffStopAngle},
                {"falloffStartOpacity", attrs.falloffStartOpacity},
                {"falloffStopOpacity", attrs.falloffStopOpacity},
                {"softFalloffDepth", attrs.softFalloffDepth},
                {"envMapScale", attrs.envMapScale}
            };
        }
    }
    if (api.getAlphaProperty) {
        AlphaPropertyBuf alpha{};
        if (api.getAlphaProperty(nifHandle, shapeHandle, &alpha)) {
            shape.alpha = json{{"flags", alpha.flags}, {"threshold", alpha.threshold}};
        }
    }
}

std::vector<ShapeData> ReadShapes(const NiflyApi& api, void* nifHandle, std::vector<Diagnostic>& diagnostics)
{
    const int shapeCount = api.getShapes(nifHandle, nullptr, 0, 0);
    if (shapeCount <= 0) {
        return {};
    }

    std::vector<void*> shapeHandles(static_cast<size_t>(shapeCount));
    api.getShapes(nifHandle, shapeHandles.data(), shapeCount, 0);

    std::vector<ShapeData> shapes;
    shapes.reserve(shapeHandles.size());

    for (size_t shapeIndex = 0; shapeIndex < shapeHandles.size(); ++shapeIndex) {
        void* shapeHandle = shapeHandles[shapeIndex];
        ShapeData shape{};
        shape.handle = shapeHandle;
        shape.blockId = api.getBlockId ? api.getBlockId(nifHandle, shapeHandle) : -1;
        if (api.getNodeParent && api.getBlockId) {
            if (void* parent = api.getNodeParent(nifHandle, shapeHandle)) {
                shape.parentBlockId = api.getBlockId(nifHandle, parent);
            }
        }
        shape.name = ReadCString([&](char* buffer, int size) { return api.getShapeName(shapeHandle, buffer, size); })
            .value_or("Shape_" + std::to_string(shapeIndex));
        shape.blockName = api.getShapeBlockName
            ? ReadCString([&](char* buffer, int size) { return api.getShapeBlockName(shapeHandle, buffer, size); }).value_or("")
            : std::string();
        if (api.getTransform) {
            api.getTransform(shapeHandle, &shape.transform);
        }

        const int vertexCount = api.getVertsForShape(nifHandle, shapeHandle, nullptr, 0, 0);
        const int triangleCount = api.getTriangles(nifHandle, shapeHandle, nullptr, 0, 0);
        if (vertexCount <= 0 || triangleCount <= 0) {
            AddDiagnostic(diagnostics, "warning", "Skipping shape '" + shape.name + "' because it has no drawable geometry.");
            continue;
        }

        shape.positions.resize(static_cast<size_t>(vertexCount) * 3u);
        api.getVertsForShape(nifHandle, shapeHandle, shape.positions.data(), static_cast<int>(shape.positions.size()), 0);

        if (api.getNormalsForShape) {
            const int normalCount = api.getNormalsForShape(nifHandle, shapeHandle, nullptr, 0, 0);
            if (normalCount == vertexCount) {
                shape.normals.resize(static_cast<size_t>(vertexCount) * 3u);
                api.getNormalsForShape(nifHandle, shapeHandle, shape.normals.data(), static_cast<int>(shape.normals.size()), 0);
            }
        }

        if (api.getTangentsForShape && api.getBitangentsForShape && shape.normals.size() == static_cast<size_t>(vertexCount) * 3u) {
            const int tangentCount = api.getTangentsForShape(nifHandle, shapeHandle, nullptr, 0, 0);
            const int bitangentCount = api.getBitangentsForShape(nifHandle, shapeHandle, nullptr, 0, 0);
            if (tangentCount == vertexCount && bitangentCount == vertexCount) {
                std::vector<float> tangentVectors(static_cast<size_t>(vertexCount) * 3u);
                std::vector<float> bitangentVectors(static_cast<size_t>(vertexCount) * 3u);
                api.getTangentsForShape(nifHandle, shapeHandle, tangentVectors.data(), static_cast<int>(tangentVectors.size()), 0);
                api.getBitangentsForShape(nifHandle, shapeHandle, bitangentVectors.data(), static_cast<int>(bitangentVectors.size()), 0);

                shape.tangents.resize(static_cast<size_t>(vertexCount) * 4u);
                for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                    const size_t vecBase = static_cast<size_t>(vertexIndex) * 3u;
                    const size_t tangentBase = static_cast<size_t>(vertexIndex) * 4u;
                    float normal[3] = { shape.normals[vecBase + 0u], shape.normals[vecBase + 1u], shape.normals[vecBase + 2u] };
                    float tangent[3] = { tangentVectors[vecBase + 0u], tangentVectors[vecBase + 1u], tangentVectors[vecBase + 2u] };
                    const float bitangent[3] = { bitangentVectors[vecBase + 0u], bitangentVectors[vecBase + 1u], bitangentVectors[vecBase + 2u] };
                    Normalize3(normal);
                    const float tangentProjection = Dot3(tangent, normal);
                    tangent[0] -= normal[0] * tangentProjection;
                    tangent[1] -= normal[1] * tangentProjection;
                    tangent[2] -= normal[2] * tangentProjection;
                    Normalize3(tangent);
                    const auto nCrossT = Cross3(normal, tangent);
                    const float handedness = (nCrossT[0] * bitangent[0] + nCrossT[1] * bitangent[1] + nCrossT[2] * bitangent[2]) < 0.0f
                        ? 1.0f
                        : -1.0f;
                    shape.tangents[tangentBase + 0u] = tangent[0];
                    shape.tangents[tangentBase + 1u] = tangent[1];
                    shape.tangents[tangentBase + 2u] = tangent[2];
                    shape.tangents[tangentBase + 3u] = handedness;
                }
            }
        }

        if (api.getUVs) {
            const int uvCount = api.getUVs(nifHandle, shapeHandle, nullptr, 0, 0);
            if (uvCount == vertexCount) {
                shape.uvs.resize(static_cast<size_t>(vertexCount) * 2u);
                api.getUVs(nifHandle, shapeHandle, shape.uvs.data(), static_cast<int>(shape.uvs.size()), 0);
            }
        }

        if (api.getColorsForShape) {
            const int colorCount = api.getColorsForShape(nifHandle, shapeHandle, nullptr, 0);
            if (colorCount == vertexCount) {
                shape.colors.resize(static_cast<size_t>(vertexCount) * 4u);
                api.getColorsForShape(nifHandle, shapeHandle, shape.colors.data(), static_cast<int>(shape.colors.size()));
            }
        }

        shape.triangles.resize(static_cast<size_t>(triangleCount) * 3u);
        api.getTriangles(nifHandle, shapeHandle, shape.triangles.data(), static_cast<int>(shape.triangles.size()), 0);

        if (api.getShaderTextureSlot) {
            for (int slot = 0; slot < 10; ++slot) {
                std::array<char, 1024> textureBuffer{};
                const int len = api.getShaderTextureSlot(nifHandle, shapeHandle, slot, textureBuffer.data(), static_cast<int>(textureBuffer.size()));
                if (len > 0 && textureBuffer[0] != '\0') {
                    shape.textures.emplace_back(textureBuffer.data());
                }
            }
        }

        ReadShapeSkinning(api, nifHandle, shapeHandle, vertexCount, shape);
        shape.segmentation = ReadShapeSegmentation(api, nifHandle, shapeHandle);
        ReadShapeMaterial(api, nifHandle, shapeHandle, shape);
        shape.extraData = ReadExtraDataList(api, nifHandle, shapeHandle);

        shapes.push_back(std::move(shape));
    }

    return shapes;
}

std::vector<ShapeData> ReadShapeShaderMetadata(const NiflyApi& api, void* nifHandle)
{
    if (!api.getShapes) {
        return {};
    }

    const int shapeCount = api.getShapes(nifHandle, nullptr, 0, 0);
    if (shapeCount <= 0) {
        return {};
    }

    std::vector<void*> shapeHandles(static_cast<size_t>(shapeCount));
    api.getShapes(nifHandle, shapeHandles.data(), shapeCount, 0);

    std::vector<ShapeData> shapes;
    shapes.reserve(shapeHandles.size());
    for (size_t shapeIndex = 0; shapeIndex < shapeHandles.size(); ++shapeIndex) {
        void* shapeHandle = shapeHandles[shapeIndex];
        ShapeData shape{};
        shape.handle = shapeHandle;
        shape.blockId = api.getBlockId ? api.getBlockId(nifHandle, shapeHandle) : -1;
        shape.name = api.getShapeName
            ? ReadCString([&](char* buffer, int size) { return api.getShapeName(shapeHandle, buffer, size); }).value_or("Shape_" + std::to_string(shapeIndex))
            : "Shape_" + std::to_string(shapeIndex);
        shape.blockName = api.getShapeBlockName
            ? ReadCString([&](char* buffer, int size) { return api.getShapeBlockName(shapeHandle, buffer, size); }).value_or("")
            : std::string();
        ReadShapeMaterial(api, nifHandle, shapeHandle, shape);
        shapes.push_back(std::move(shape));
    }

    return shapes;
}
#endif

json ShaderFlagDefinitionJson(const char* setName, const FlagDef& def)
{
    return json{
        {"set", setName},
        {"name", def.name},
        {"mask", def.mask},
        {"maskHex", Hex32(def.mask)}
    };
}

json ShapeShaderSummaryJson(const ShapeData& shape)
{
    json item{
        {"name", shape.name},
        {"blockName", shape.blockName},
        {"blockId", shape.blockId},
        {"shader", shape.shader}
    };

    if (shape.shader.contains("shaderFlags1") && shape.shader.contains("shaderFlags2")) {
        const std::string flags1Hex = shape.shader.value("shaderFlags1Hex", Hex32(shape.shader.value("shaderFlags1", 0u)));
        const std::string flags2Hex = shape.shader.value("shaderFlags2Hex", Hex32(shape.shader.value("shaderFlags2", 0u)));
        item["combinationKey"] = flags1Hex + "|" + flags2Hex;
        item["combinationNames"] = json{
            {"shaderFlags1", shape.shader.contains("shaderFlags1Names") ? shape.shader["shaderFlags1Names"] : json::array()},
            {"shaderFlags2", shape.shader.contains("shaderFlags2Names") ? shape.shader["shaderFlags2Names"] : json::array()}
        };
    }

    return item;
}

json ShaderFlagsJson(const char* argv0, const fs::path& nifPath)
{
    std::vector<Diagnostic> diagnostics;
    json response;
    response["status"] = "error";
    response["sourcePath"] = nifPath.string();

    auto api = LoadNiflyApi(argv0, diagnostics);
    if (!api) {
        response["message"] = "Unable to load niflyDLL.";
        response["diagnostics"] = json::array();
        for (const Diagnostic& diagnostic : diagnostics) {
            response["diagnostics"].push_back(DiagnosticJson(diagnostic));
        }
        return response;
    }

#ifdef _WIN32
    if (api->clearMessageLog) {
        api->clearMessageLog();
    }

    void* nifHandle = api->load(nifPath.string().c_str());
    if (!nifHandle) {
        AddDiagnostic(diagnostics, "error", "niflyDLL failed to load '" + nifPath.string() + "'. " + GetMessageLog(*api));
        response["message"] = "niflyDLL failed to load the NIF file.";
        response["diagnostics"] = json::array();
        for (const Diagnostic& diagnostic : diagnostics) {
            response["diagnostics"].push_back(DiagnosticJson(diagnostic));
        }
        return response;
    }

    std::string gameName = "unknown";
    if (api->getGameName) {
        gameName = ReadCString([&](char* buffer, int size) { return api->getGameName(nifHandle, buffer, size); }).value_or("unknown");
    }

    std::vector<ShapeData> shapes = ReadShapeShaderMetadata(*api, nifHandle);
    api->destroy(nifHandle);

    json discoveredFlags = json::array();
    std::set<std::string> discoveredKeys;
    json combinations = json::array();
    std::set<std::string> combinationKeys;
    json shapeItems = json::array();

    auto addDiscovered = [&](const char* setName, uint32_t flags, const std::array<FlagDef, 32>& definitions) {
        for (const FlagDef& def : definitions) {
            if ((flags & def.mask) == 0u) {
                continue;
            }
            const std::string key = std::string(setName) + ":" + def.name;
            if (discoveredKeys.insert(key).second) {
                discoveredFlags.push_back(ShaderFlagDefinitionJson(setName, def));
            }
        }
    };

    for (const ShapeData& shape : shapes) {
        shapeItems.push_back(ShapeShaderSummaryJson(shape));
        const uint32_t flags1 = shape.shader.value("shaderFlags1", 0u);
        const uint32_t flags2 = shape.shader.value("shaderFlags2", 0u);
        addDiscovered("shaderFlags1", flags1, kShaderFlags1);
        addDiscovered("shaderFlags2", flags2, kShaderFlags2);

        const std::string combinationKey = Hex32(flags1) + "|" + Hex32(flags2);
        if (combinationKeys.insert(combinationKey).second) {
            combinations.push_back(json{
                {"shaderFlags1", flags1},
                {"shaderFlags1Hex", Hex32(flags1)},
                {"shaderFlags1Names", DecodeShaderFlags(flags1, kShaderFlags1)},
                {"shaderFlags2", flags2},
                {"shaderFlags2Hex", Hex32(flags2)},
                {"shaderFlags2Names", DecodeShaderFlags(flags2, kShaderFlags2)}
            });
        }
    }

    response["status"] = "ok";
    response["protocolVersion"] = "1.0";
    response["sourcePath"] = nifPath.string();
    response["gameName"] = gameName;
    response["shapeCount"] = shapes.size();
    response["shapes"] = shapeItems;
    response["discoveredFlags"] = discoveredFlags;
    response["uniqueCombinations"] = combinations;
    response["diagnostics"] = json::array();
    for (const Diagnostic& diagnostic : diagnostics) {
        response["diagnostics"].push_back(DiagnosticJson(diagnostic));
    }
    return response;
#else
    response["message"] = "BRNifly shader flag inspection is currently implemented for Windows only.";
    return response;
#endif
}

std::optional<std::string> ConvertShapesToUsd(
    const std::vector<ShapeData>& shapes,
    std::vector<NodeData> nodes,
    const json& rootExtraData,
    const std::vector<CollisionProxyData>& collisionProxies,
    const fs::path& nifPath,
    const std::string& gameName,
    std::vector<Diagnostic>& diagnostics)
{
    SdfLayerRefPtr rootLayer = SdfLayer::CreateAnonymous("brnifly.usda");
    UsdStageRefPtr stage = UsdStage::Open(rootLayer);
    if (!stage) {
        AddDiagnostic(diagnostics, "error", "Failed to create OpenUSD stage.");
        return std::nullopt;
    }

    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->z);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);

    UsdGeomXform root = UsdGeomXform::Define(stage, SdfPath("/BRNifly"));
    stage->SetDefaultPrim(root.GetPrim());
    // Keep generated USD independent from the absolute path BRNifly was handed.
    // The renderer cache is keyed by the game path; baking MO2/temp/loose-file
    // paths into the layer makes one asset produce multiple content hashes.
    std::string sourceFileName = nifPath.filename().string();
    std::transform(sourceFileName.begin(), sourceFileName.end(), sourceFileName.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    root.GetPrim().SetCustomDataByKey(TfToken("brnifly:sourcePath"), VtValue(sourceFileName));
    root.GetPrim().SetCustomDataByKey(TfToken("brnifly:gameName"), VtValue(gameName));
    root.GetPrim().SetCustomDataByKey(TfToken("brnifly:unsupportedDataPolicy"), VtValue(std::string("preserve-as-usd-custom-data")));
    if (!rootExtraData.empty()) {
        root.GetPrim().SetCustomDataByKey(TfToken("brnifly:extraData"), VtValue(rootExtraData.dump()));
    }

    std::map<int, size_t> nodeIndexByBlockId;
    std::set<int> shapeBlockIds;
    for (const ShapeData& shape : shapes) {
        if (shape.blockId >= 0) {
            shapeBlockIds.insert(shape.blockId);
        }
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].blockId >= 0 && shapeBlockIds.find(nodes[i].blockId) == shapeBlockIds.end()) {
            nodeIndexByBlockId[nodes[i].blockId] = i;
        }
    }

    std::set<std::string> usedNodePrimNames;
    std::function<SdfPath(int)> ensureNodePath = [&](int blockId) -> SdfPath {
        auto it = nodeIndexByBlockId.find(blockId);
        if (it == nodeIndexByBlockId.end()) {
            return SdfPath("/BRNifly");
        }
        NodeData& node = nodes[it->second];
        if (!node.path.IsEmpty()) {
            return node.path;
        }
        const SdfPath parentPath = node.parentBlockId >= 0 ? ensureNodePath(node.parentBlockId) : SdfPath("/BRNifly");
        const std::string primName = MakeUniqueName(node.name.empty() ? "Node_" + std::to_string(node.blockId) : node.name, usedNodePrimNames);
        node.path = parentPath.AppendChild(TfToken(primName));
        UsdGeomXform nodePrim = UsdGeomXform::Define(stage, node.path);
        nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:blockId"), VtValue(node.blockId));
        if (!node.blockName.empty()) {
            nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:blockName"), VtValue(node.blockName));
        }
        nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:flags"), VtValue(node.flags));
        if (node.switchIndex >= 0) {
            nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:switchIndex"), VtValue(node.switchIndex));
        }
        if (node.lod0ChildBlockId >= 0) {
            nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:lod0ChildBlockId"), VtValue(node.lod0ChildBlockId));
        }
        nodePrim.GetPrim().SetCustomDataByKey(TfToken("brnifly:transform"), VtValue(TransformJson(node.transform).dump()));
        ApplyTransform(nodePrim.GetPrim(), node.transform);
        return node.path;
    };

    for (const auto& [blockId, index] : nodeIndexByBlockId) {
        (void)index;
        ensureNodePath(blockId);
    }

    SdfPath materialsPath("/BRNifly/Materials");
    UsdGeomXform::Define(stage, materialsPath);
    std::map<std::string, UsdShadeMaterial> materials;
    auto materialForShape = [&](const ShapeData& shape) -> UsdShadeMaterial {
        if (shape.textures.empty() && shape.shader.empty() && shape.alpha.empty()) {
            return UsdShadeMaterial();
        }
        json keyJson{{"textures", shape.textures}, {"shader", shape.shader}, {"alpha", shape.alpha}};
        const std::string key = keyJson.dump();
        auto existing = materials.find(key);
        if (existing != materials.end()) {
            return existing->second;
        }

        const std::string name = "Material_" + HexHash(Fnv1a64(key)).substr(0, 12);
        UsdShadeMaterial material = UsdShadeMaterial::Define(stage, materialsPath.AppendChild(TfToken(name)));
        material.GetPrim().SetCustomDataByKey(TfToken("brnifly:material"), VtValue(key));
        if (!shape.textures.empty()) {
            material.GetPrim().SetCustomDataByKey(TfToken("brnifly:textures"), VtValue(json(shape.textures).dump()));
        }
        UsdShadeShader shader = UsdShadeShader::Define(stage, material.GetPath().AppendChild(TfToken("PreviewSurface")));
        shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        if (shape.shader.contains("lightingShader") && shape.shader["lightingShader"].contains("alpha")) {
            shader.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(shape.shader["lightingShader"]["alpha"].get<float>());
        } else if (shape.alpha.contains("threshold")) {
            shader.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(1.0f);
        }
        material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
        materials.emplace(key, material);
        return material;
    };

    size_t emittedMeshes = 0;
    std::set<std::string> usedShapePrimNames;
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const ShapeData& shape = shapes[shapeIndex];
        const std::string primName = MakeUniqueName(shape.name.empty() ? "Shape_" + std::to_string(shapeIndex) : shape.name, usedShapePrimNames);
        const SdfPath parentPath = shape.parentBlockId >= 0 ? ensureNodePath(shape.parentBlockId) : SdfPath("/BRNifly");
        const SdfPath meshPath = parentPath.AppendChild(TfToken(primName));
        UsdGeomMesh mesh = UsdGeomMesh::Define(stage, meshPath);
        if (!mesh) {
            AddDiagnostic(diagnostics, "warning", "Failed to define USD mesh for shape '" + shape.name + "'.");
            continue;
        }
        ApplyTransform(mesh.GetPrim(), shape.transform);
        mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:blockId"), VtValue(shape.blockId));
        if (!shape.blockName.empty()) {
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:blockName"), VtValue(shape.blockName));
        }
        mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:transform"), VtValue(TransformJson(shape.transform).dump()));

        VtArray<GfVec3f> points;
        points.reserve(shape.positions.size() / 3u);
        for (size_t i = 0; i + 2 < shape.positions.size(); i += 3) {
            points.push_back(GfVec3f(shape.positions[i], shape.positions[i + 1], shape.positions[i + 2]));
        }
        mesh.CreatePointsAttr(VtValue(points));

        VtArray<int> faceVertexCounts;
        VtArray<int> faceVertexIndices;
        faceVertexCounts.reserve(shape.triangles.size() / 3u);
        faceVertexIndices.reserve(shape.triangles.size());
        for (size_t i = 0; i + 2 < shape.triangles.size(); i += 3) {
            faceVertexCounts.push_back(3);
            faceVertexIndices.push_back(static_cast<int>(shape.triangles[i]));
            faceVertexIndices.push_back(static_cast<int>(shape.triangles[i + 1]));
            faceVertexIndices.push_back(static_cast<int>(shape.triangles[i + 2]));
        }
        mesh.CreateFaceVertexCountsAttr(VtValue(faceVertexCounts));
        mesh.CreateFaceVertexIndicesAttr(VtValue(faceVertexIndices));
        mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
        if ((shape.shader.value("shaderFlags2", 0u) & (1u << 4)) != 0u) {
            mesh.CreateDoubleSidedAttr(VtValue(true));
        }

        if (shape.normals.size() == shape.positions.size()) {
            VtArray<GfVec3f> normals;
            normals.reserve(shape.normals.size() / 3u);
            for (size_t i = 0; i + 2 < shape.normals.size(); i += 3) {
                normals.push_back(GfVec3f(shape.normals[i], shape.normals[i + 1], shape.normals[i + 2]));
            }
            mesh.CreateNormalsAttr(VtValue(normals));
            mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
        }

        UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
        if (shape.tangents.size() == points.size() * 4u) {
            VtArray<GfVec4f> tangents;
            tangents.reserve(points.size());
            for (size_t i = 0; i + 3 < shape.tangents.size(); i += 4) {
                tangents.push_back(GfVec4f(shape.tangents[i], shape.tangents[i + 1], shape.tangents[i + 2], shape.tangents[i + 3]));
            }
            UsdGeomPrimvar tangentPrimvar = primvars.CreatePrimvar(TfToken("brnifly:tangents"), SdfValueTypeNames->Float4Array, UsdGeomTokens->vertex);
            tangentPrimvar.Set(tangents);
        }

        if (shape.uvs.size() == points.size() * 2u) {
            VtArray<GfVec2f> uvValues;
            uvValues.reserve(points.size());
            for (size_t i = 0; i + 1 < shape.uvs.size(); i += 2) {
                uvValues.push_back(GfVec2f(shape.uvs[i], 1.0f - shape.uvs[i + 1]));
            }
            UsdGeomPrimvar st = primvars.CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->vertex);
            st.Set(uvValues);
        }

        if (shape.colors.size() == points.size() * 4u) {
            VtArray<GfVec3f> displayColors;
            VtArray<float> displayOpacity;
            displayColors.reserve(points.size());
            displayOpacity.reserve(points.size());
            for (size_t i = 0; i + 3 < shape.colors.size(); i += 4) {
                displayColors.push_back(GfVec3f(shape.colors[i], shape.colors[i + 1], shape.colors[i + 2]));
                displayOpacity.push_back(shape.colors[i + 3]);
            }
            primvars.CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->vertex).Set(displayColors);
            primvars.CreatePrimvar(TfToken("displayOpacity"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).Set(displayOpacity);
        }

        if (!shape.jointIndices.empty() && !shape.jointWeights.empty()) {
            VtArray<int> jointIndices(shape.jointIndices.begin(), shape.jointIndices.end());
            VtArray<float> jointWeights(shape.jointWeights.begin(), shape.jointWeights.end());
            UsdGeomPrimvar jointIndexPrimvar = primvars.CreatePrimvar(TfToken("brnifly:jointIndices"), SdfValueTypeNames->IntArray, UsdGeomTokens->vertex);
            jointIndexPrimvar.SetElementSize(4);
            jointIndexPrimvar.Set(jointIndices);
            UsdGeomPrimvar jointWeightPrimvar = primvars.CreatePrimvar(TfToken("brnifly:jointWeights"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex);
            jointWeightPrimvar.SetElementSize(4);
            jointWeightPrimvar.Set(jointWeights);
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:jointNames"), VtValue(json(shape.boneNames).dump()));
            if (shape.boneSourceIndices.size() == shape.boneNames.size()) {
                mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:jointSourceIndices"), VtValue(json(shape.boneSourceIndices).dump()));
            }
            if (shape.skinToBoneTransforms.size() == shape.boneNames.size() * 13u) {
                mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:skinToBoneTransforms"), VtValue(json(shape.skinToBoneTransforms).dump()));
            }
        }

        if (!shape.textures.empty()) {
            json textureMetadata = shape.textures;
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:textures"), VtValue(textureMetadata.dump()));
        }
        if (!shape.shader.empty()) {
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:shader"), VtValue(shape.shader.dump()));
        }
        if (!shape.alpha.empty()) {
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:alphaProperty"), VtValue(shape.alpha.dump()));
        }
        if (!shape.segmentation.empty()) {
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:segmentation"), VtValue(shape.segmentation.dump()));
        }
        if (!shape.extraData.empty()) {
            mesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:extraData"), VtValue(shape.extraData.dump()));
        }

        if (UsdShadeMaterial material = materialForShape(shape)) {
            UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(material);
        }

        ++emittedMeshes;
    }

    if (!collisionProxies.empty()) {
        const SdfPath collisionRoot("/BRNifly/Collision");
        UsdGeomXform::Define(stage, collisionRoot);
        std::set<std::string> usedCollisionNames;
        for (size_t proxyIndex = 0; proxyIndex < collisionProxies.size(); ++proxyIndex) {
            const CollisionProxyData& proxy = collisionProxies[proxyIndex];
            const std::string name = MakeUniqueName(proxy.name.empty() ? "Collision_" + std::to_string(proxyIndex) : proxy.name, usedCollisionNames);
            const SdfPath proxyPath = collisionRoot.AppendChild(TfToken(name));
            VtArray<GfVec3f> points;
            points.reserve(proxy.positions.size() / 3u);
            for (size_t i = 0; i + 2 < proxy.positions.size(); i += 3) {
                points.push_back(GfVec3f(proxy.positions[i], proxy.positions[i + 1], proxy.positions[i + 2]));
            }
            if (!proxy.triangles.empty()) {
                UsdGeomMesh collisionMesh = UsdGeomMesh::Define(stage, proxyPath);
                collisionMesh.CreatePointsAttr(VtValue(points));
                VtArray<int> faceVertexCounts;
                VtArray<int> faceVertexIndices;
                for (size_t i = 0; i + 2 < proxy.triangles.size(); i += 3) {
                    faceVertexCounts.push_back(3);
                    faceVertexIndices.push_back(static_cast<int>(proxy.triangles[i]));
                    faceVertexIndices.push_back(static_cast<int>(proxy.triangles[i + 1]));
                    faceVertexIndices.push_back(static_cast<int>(proxy.triangles[i + 2]));
                }
                collisionMesh.CreateFaceVertexCountsAttr(VtValue(faceVertexCounts));
                collisionMesh.CreateFaceVertexIndicesAttr(VtValue(faceVertexIndices));
                collisionMesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
                collisionMesh.GetPrim().SetCustomDataByKey(TfToken("brnifly:collision"), VtValue(json{{"sourceNodeId", proxy.sourceNodeId}, {"bodyId", proxy.bodyId}, {"shapeId", proxy.shapeId}, {"blockName", proxy.blockName}}.dump()));
            } else {
                UsdGeomPoints collisionPoints = UsdGeomPoints::Define(stage, proxyPath);
                collisionPoints.CreatePointsAttr(VtValue(points));
                collisionPoints.GetPrim().SetCustomDataByKey(TfToken("brnifly:collision"), VtValue(json{{"sourceNodeId", proxy.sourceNodeId}, {"bodyId", proxy.bodyId}, {"shapeId", proxy.shapeId}, {"blockName", proxy.blockName}}.dump()));
            }
        }
    }

    if (emittedMeshes == 0 && nodes.empty() && collisionProxies.empty() && rootExtraData.empty()) {
        AddDiagnostic(diagnostics, "error", "No USD-representable data was emitted from the NIF.");
        return std::nullopt;
    }

    std::string usdText;
    if (!rootLayer->ExportToString(&usdText)) {
        AddDiagnostic(diagnostics, "error", "OpenUSD failed to export the generated layer.");
        return std::nullopt;
    }

    AddDiagnostic(diagnostics, "info", "Converted " + std::to_string(emittedMeshes) + " NIF shape(s), " + std::to_string(nodes.size()) + " node(s), and " + std::to_string(collisionProxies.size()) + " collision proxy/proxies to USD.");
    return usdText;
}

json ConvertUsdJson(const char* argv0, const fs::path& nifPath)
{
    std::vector<Diagnostic> diagnostics;
    json response;
    response["status"] = "error";
    response["sourcePath"] = nifPath.string();

    auto api = LoadNiflyApi(argv0, diagnostics);
    if (!api) {
        response["message"] = "Unable to load niflyDLL.";
        response["diagnostics"] = json::array();
        for (const Diagnostic& diagnostic : diagnostics) {
            response["diagnostics"].push_back(DiagnosticJson(diagnostic));
        }
        return response;
    }

#ifdef _WIN32
    if (api->clearMessageLog) {
        api->clearMessageLog();
    }

    void* nifHandle = api->load(nifPath.string().c_str());
    if (!nifHandle) {
        AddDiagnostic(diagnostics, "error", "niflyDLL failed to load '" + nifPath.string() + "'. " + GetMessageLog(*api));
        response["message"] = "niflyDLL failed to load the NIF file.";
        response["diagnostics"] = json::array();
        for (const Diagnostic& diagnostic : diagnostics) {
            response["diagnostics"].push_back(DiagnosticJson(diagnostic));
        }
        return response;
    }

    std::string gameName = "unknown";
    if (api->getGameName) {
        gameName = ReadCString([&](char* buffer, int size) { return api->getGameName(nifHandle, buffer, size); }).value_or("unknown");
    }

    std::vector<NodeData> nodes = ReadNodes(*api, nifHandle);
    std::vector<ShapeData> shapes = ReadShapes(*api, nifHandle, diagnostics);
    json rootExtraData = ReadExtraDataList(*api, nifHandle, nullptr);
    std::vector<CollisionProxyData> collisionProxies;
    api->destroy(nifHandle);

    auto usdText = ConvertShapesToUsd(shapes, std::move(nodes), rootExtraData, collisionProxies, nifPath, gameName, diagnostics);
    if (!usdText) {
        response["message"] = "NIF-to-USD conversion failed.";
        response["diagnostics"] = json::array();
        for (const Diagnostic& diagnostic : diagnostics) {
            response["diagnostics"].push_back(DiagnosticJson(diagnostic));
        }
        return response;
    }

    const std::string hash = HexHash(Fnv1a64(std::string(kUsdContentIdentityVersion) + "\n" + *usdText));
    response["status"] = "ok";
    response["protocolVersion"] = "1.0";
    response["sourcePath"] = nifPath.string();
    response["sourceIdentifier"] = nifPath.string() + "#brnifly=" + hash;
    response["contentHash"] = hash;
    response["rootLayerText"] = *usdText;
    response["dependencies"] = json::array();
    response["textureSearchRoots"] = json::array({nifPath.parent_path().string()});
    response["diagnostics"] = json::array();
    for (const Diagnostic& diagnostic : diagnostics) {
        response["diagnostics"].push_back(DiagnosticJson(diagnostic));
    }
    return response;
#else
    response["message"] = "BRNifly conversion is currently implemented for Windows only.";
    return response;
#endif
}

int PrintUsage()
{
    std::cerr << "Usage:\n"
        << "  BRNifly --describe-services\n"
        << "  BRNifly --shader-flags-json <file.nif>\n"
        << "  BRNifly --shader-flags-json-file <file.nif> <response.json>\n"
        << "  BRNifly --convert-usd-json <file.nif>\n"
        << "  BRNifly --convert-usd-json-file <file.nif> <response.json>\n"
        << "  BRNifly --convert <file.nif> --out <file.usda>\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    CrashLog::Install();
    if (argc >= 2) {
        CrashLog::SetContext(argv[1], argc >= 3 ? argv[2] : "");
    } else {
        CrashLog::SetContext("startup");
    }
#endif

    spdlog::set_level(spdlog::level::warn);

    if (argc == 2 && std::string(argv[1]) == "--describe-services") {
#ifdef _WIN32
        CrashLog::SetContext("--describe-services");
#endif
        std::cout << DescribeServicesJson(argv[0]).dump(2) << std::endl;
        return 0;
    }

    if (argc == 3 && std::string(argv[1]) == "--convert-usd-json") {
#ifdef _WIN32
        CrashLog::SetContext("--convert-usd-json", argv[2]);
#endif
        std::cout << ConvertUsdJson(argv[0], fs::path(argv[2])).dump(2) << std::endl;
        return 0;
    }

    if (argc == 3 && std::string(argv[1]) == "--shader-flags-json") {
#ifdef _WIN32
        CrashLog::SetContext("--shader-flags-json", argv[2]);
#endif
        std::cout << ShaderFlagsJson(argv[0], fs::path(argv[2])).dump(2) << std::endl;
        return 0;
    }

    if (argc == 4 && std::string(argv[1]) == "--shader-flags-json-file") {
#ifdef _WIN32
        CrashLog::SetContext("--shader-flags-json-file", argv[2]);
#endif
        json response = ShaderFlagsJson(argv[0], fs::path(argv[2]));
        std::ofstream out(argv[3], std::ios::binary);
        if (!out) {
            std::cerr << "Failed to open response file: " << argv[3] << std::endl;
            return 1;
        }
        out << response.dump(2);
        std::cout << "{\"status\":\"ok\",\"responseFile\":" << json(argv[3]).dump() << "}" << std::endl;
        return 0;
    }

    if (argc == 4 && std::string(argv[1]) == "--convert-usd-json-file") {
#ifdef _WIN32
        CrashLog::SetContext("--convert-usd-json-file", argv[2]);
#endif
        json response = ConvertUsdJson(argv[0], fs::path(argv[2]));
        std::ofstream out(argv[3], std::ios::binary);
        if (!out) {
            std::cerr << "Failed to open response file: " << argv[3] << std::endl;
            return 1;
        }
        out << response.dump(2);
        std::cout << "{\"status\":\"ok\",\"responseFile\":" << json(argv[3]).dump() << "}" << std::endl;
        return 0;
    }

    if (argc == 5 && std::string(argv[1]) == "--convert" && std::string(argv[3]) == "--out") {
#ifdef _WIN32
        CrashLog::SetContext("--convert", argv[2]);
#endif
        json response = ConvertUsdJson(argv[0], fs::path(argv[2]));
        if (response.value("status", "") != "ok") {
            std::cerr << response.dump(2) << std::endl;
            return 1;
        }
        std::ofstream out(argv[4], std::ios::binary);
        out << response.value("rootLayerText", "");
        return 0;
    }

    return PrintUsage();
}
