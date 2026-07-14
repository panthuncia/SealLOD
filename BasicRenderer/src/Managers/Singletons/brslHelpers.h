#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ObjIdl.h>
#include <Unknwn.h>
#include <wtypes.h>
#include <tree_sitter/api.h>
#include "ThirdParty/DirectX/dxcapi.h"

extern "C" const TSLanguage* tree_sitter_hlsl();

struct Replacement {
    uint32_t startByte;
    uint32_t endByte;
    std::string replacement;
};

struct ShaderPreprocessDiagnostics
{
    bool parseSucceeded = false;
    uint32_t errorNodeCount = 0;
    std::vector<std::string> parseErrorSummaries;
    std::vector<std::string> discoveredFunctionDefinitions;
    std::vector<std::string> unnamedFunctionDefinitions;
    std::vector<std::string> unresolvedInternalCalls;
    bool safeToPrune = false;
    bool usedFallbackRewrite = false;
    bool pruningApplied = false;
    bool finalSourceValid = false;
    std::vector<std::string> notes;
};

struct PreparedShaderSource
{
    std::string sourceBeforeRewrite;
    std::vector<std::string> mandatoryIDs;
    std::vector<std::string> optionalIDs;
    std::unordered_set<std::string> originalDefinedFunctionNames;
    ShaderPreprocessDiagnostics diagnostics;
};

inline constexpr uint32_t kBRSLPreprocessVersion = 2;

static inline void SortAndUniqueStrings(std::vector<std::string>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

static inline size_t GetNormalizedShaderSourceSize(const char* source, size_t sourceSize)
{
    if (!source) {
        return 0;
    }

    while (sourceSize > 0) {
        const unsigned char trailing = static_cast<unsigned char>(source[sourceSize - 1]);
        if (trailing == 0u || trailing == 0x1Au) {
            --sourceSize;
            continue;
        }
        break;
    }

    return sourceSize;
}

static inline bool IsPreprocessedLineDirective(std::string_view line)
{
    size_t position = 0;
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }

    if (position >= line.size() || line[position] != '#') {
        return false;
    }

    ++position;
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }

    const size_t keywordStart = position;
    while (position < line.size() && std::isalpha(static_cast<unsigned char>(line[position])) != 0) {
        ++position;
    }

    if (keywordStart != position) {
        const std::string_view keyword = line.substr(keywordStart, position - keywordStart);
        if (keyword != "line") {
            return false;
        }
    }

    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }

    const size_t numberStart = position;
    while (position < line.size() && std::isdigit(static_cast<unsigned char>(line[position])) != 0) {
        ++position;
    }

    return numberStart != position;
}

// DXC may emit #line directives at macro expansion boundaries inside otherwise
// contiguous syntax such as function parameter and argument lists. Tree-sitter's
// C++-derived list rules do not accept preprocessor nodes in every such position.
// Blank the directives only for parsing while preserving every byte and line
// ending so that syntax-node offsets continue to address the original source.
static inline std::string BuildTreeSitterShaderParseView(
    const char* source,
    size_t sourceSize)
{
    sourceSize = GetNormalizedShaderSourceSize(source, sourceSize);
    if (!source || sourceSize == 0) {
        return {};
    }

    std::string parseView(source, sourceSize);
    size_t lineStart = 0;
    while (lineStart < sourceSize) {
        size_t lineEnd = lineStart;
        while (lineEnd < sourceSize && source[lineEnd] != '\n' && source[lineEnd] != '\r') {
            ++lineEnd;
        }

        if (IsPreprocessedLineDirective(
                std::string_view(source + lineStart, lineEnd - lineStart)))
        {
            std::fill(
                parseView.begin() + static_cast<std::ptrdiff_t>(lineStart),
                parseView.begin() + static_cast<std::ptrdiff_t>(lineEnd),
                ' ');
        }

        if (lineEnd < sourceSize && source[lineEnd] == '\r' &&
            lineEnd + 1 < sourceSize && source[lineEnd + 1] == '\n')
        {
            lineStart = lineEnd + 2;
        }
        else if (lineEnd < sourceSize) {
            lineStart = lineEnd + 1;
        }
        else {
            lineStart = sourceSize;
        }
    }

    return parseView;
}

static inline std::string CanonicalizePreprocessedShaderSourceForHash(
    const char* source,
    size_t sourceSize)
{
    sourceSize = GetNormalizedShaderSourceSize(source, sourceSize);
    if (!source || sourceSize == 0) {
        return {};
    }

    std::string canonical;
    canonical.reserve(sourceSize);

    size_t lineStart = 0;
    while (lineStart < sourceSize) {
        size_t lineEnd = lineStart;
        while (lineEnd < sourceSize && source[lineEnd] != '\n' && source[lineEnd] != '\r') {
            ++lineEnd;
        }

        const std::string_view line(source + lineStart, lineEnd - lineStart);
        if (!IsPreprocessedLineDirective(line)) {
            canonical.append(line.data(), line.size());
            if (lineEnd < sourceSize) {
                if (source[lineEnd] == '\r' && lineEnd + 1 < sourceSize && source[lineEnd + 1] == '\n') {
                    canonical.push_back('\n');
                    lineEnd += 2;
                }
                else {
                    canonical.push_back('\n');
                    ++lineEnd;
                }
            }
        }
        else {
            if (lineEnd < sourceSize && source[lineEnd] == '\r' && lineEnd + 1 < sourceSize && source[lineEnd + 1] == '\n') {
                lineEnd += 2;
            }
            else if (lineEnd < sourceSize) {
                ++lineEnd;
            }
        }

        lineStart = lineEnd;
    }

    return canonical;
}

static inline TSNode FindFunctionNameNodeInDeclarator(TSNode node)
{
    if (ts_node_is_null(node)) {
        return TSNode{};
    }

    const char* type = ts_node_type(node);
    if (strcmp(type, "identifier") == 0 ||
        strcmp(type, "field_identifier") == 0 ||
        strcmp(type, "type_identifier") == 0)
    {
        return node;
    }

    TSNode nameField = ts_node_child_by_field_name(node, "name", static_cast<uint32_t>(strlen("name")));
    if (!ts_node_is_null(nameField)) {
        TSNode found = FindFunctionNameNodeInDeclarator(nameField);
        if (!ts_node_is_null(found)) {
            return found;
        }
    }

    TSNode declaratorField = ts_node_child_by_field_name(node, "declarator", static_cast<uint32_t>(strlen("declarator")));
    if (!ts_node_is_null(declaratorField)) {
        TSNode found = FindFunctionNameNodeInDeclarator(declaratorField);
        if (!ts_node_is_null(found)) {
            return found;
        }
    }

    uint32_t namedChildCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < namedChildCount; ++i) {
        TSNode found = FindFunctionNameNodeInDeclarator(ts_node_named_child(node, i));
        if (!ts_node_is_null(found)) {
            return found;
        }
    }

    return TSNode{};
}

static inline std::optional<std::string> ExtractFunctionNameFromDefinition(
    const char* preprocessedSource,
    const TSNode& functionDefNode)
{
    TSNode topDecl = ts_node_child_by_field_name(
        functionDefNode,
        "declarator",
        static_cast<uint32_t>(strlen("declarator")));
    if (ts_node_is_null(topDecl)) {
        return std::nullopt;
    }

    TSNode nameNode = FindFunctionNameNodeInDeclarator(topDecl);
    if (ts_node_is_null(nameNode)) {
        return std::nullopt;
    }

    uint32_t start = ts_node_start_byte(nameNode);
    uint32_t end = ts_node_end_byte(nameNode);
    return std::string(preprocessedSource + start, end - start);
}

void CollectFunctionDefinitions(const TSNode& node, std::vector<TSNode>& outDefinitions) {
    if (ts_node_is_null(node)) {
        return;
    }

    if (std::string(ts_node_type(node)) == "function_definition") {
        outDefinitions.push_back(node);
    }

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        CollectFunctionDefinitions(ts_node_child(node, i), outDefinitions);
    }
}

void BuildFunctionDefs(std::unordered_map<std::string, std::vector<TSNode>>& functionDefs, const char* preprocessedSource, const TSNode& root) {
    std::vector<TSNode> functionDefinitions;
    CollectFunctionDefinitions(root, functionDefinitions);

    for (const TSNode& node : functionDefinitions) {
        auto fnName = ExtractFunctionNameFromDefinition(preprocessedSource, node);
        if (!fnName.has_value()) {
            continue;
        }

        TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
        if (!functionDefs.contains(*fnName)) {
            functionDefs[*fnName] = std::vector<TSNode>();
        }
        functionDefs[*fnName].push_back(bodyNode);
    }
}

void ParseBRSLResourceIdentifiers(std::unordered_set<std::string>& outMandatoryIdentifiers, std::unordered_set<std::string>& outOptionalIdentifiers, const DxcBuffer* pBuffer, const std::string& entryPointName) {
    const char* preprocessedSource = static_cast<const char*>(pBuffer->Ptr);
    size_t sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, pBuffer->Size);

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(parser, nullptr, parseView.data(), static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    std::unordered_map<std::string, std::vector<TSNode>> functionDefs;

    BuildFunctionDefs(functionDefs, preprocessedSource, root);

    // Prepare for call-graph walk
    std::unordered_set<std::string> visited;
    std::vector<std::string> worklist;
    worklist.push_back(entryPointName);

    // Wherever we discover ResourceDescriptorIndex, we record replacements
    std::vector<Replacement> replacements;
    std::unordered_map<std::string, std::string> indexMap;

    // Helper to process one function body
    auto processBody = [&](const TSNode& bodyNode, auto&& processBodyRef) -> void {
        // Walk the subtree of this body looking for:
        //  a) ResourceDescriptorIndex calls -> record replacements
        //  b) Other function calls -> enqueue them if unvisited
        std::function<void(TSNode)> walk = [&](TSNode node) {
            const char* type = ts_node_type(node);

            if (strcmp(type, "call_expression") == 0) {
                // Check for ResourceDescriptorIndex(...)
                TSNode functionNode =
                    ts_node_child_by_field_name(node, "function", static_cast<uint32_t>(strlen("function")));

                if (ts_node_is_null(functionNode)) return;

                // Pull out function name from source
                uint32_t start = ts_node_start_byte(functionNode);
                uint32_t end = ts_node_end_byte(functionNode);

                // Slice out the raw text
                std::string funcName(preprocessedSource + start, end - start);

                // trim whitespace:
                auto l = funcName.find_first_not_of(" \t\n\r");
                auto r = funcName.find_last_not_of(" \t\n\r");
                if (l != std::string::npos && r != std::string::npos)
                    funcName = funcName.substr(l, r - l + 1);

                auto parseBuiltin = [&]() -> std::string {
                    TSNode argList = ts_node_child_by_field_name(node, "arguments", 9);
                    if (ts_node_named_child_count(argList) == 1) {
                        TSNode argNode = ts_node_named_child(argList, 0);

                        uint32_t start = ts_node_start_byte(argNode);
                        uint32_t end = ts_node_end_byte(argNode);

                        std::string rawText(preprocessedSource + start, end - start);

                        // if it's a quoted string literal, strip the quotes
                        if (rawText.size() >= 2 && rawText.front() == '"' && rawText.back() == '"') {
                            rawText = rawText.substr(1, rawText.size() - 2);
                        }

                        return std::move(rawText);
                    }
                    else {
                        throw std::runtime_error("ResourceDescriptorIndex requires exactly one argument");
                    }
                    };

                if (funcName == "ResourceDescriptorIndex") {
                    outMandatoryIdentifiers.insert(parseBuiltin());
                }
                else if (funcName == "OptionalResourceDescriptorIndex") {
                    outOptionalIdentifiers.insert(parseBuiltin());
                }
                else {
                    // Otherwise, a normal function call:
                    // Enqueue it for later processing if we know its definition
                    if (functionDefs.count(funcName) && !visited.count(funcName)) {
                        visited.insert(funcName);
                        worklist.push_back(funcName);
                    }
                }
            }

            // recurse
            uint32_t n = ts_node_child_count(node);
            for (uint32_t i = 0; i < n; ++i) {
                walk(ts_node_child(node, i));
            }
            };

        walk(bodyNode);
        };

    // Traverse the call graph
    while (!worklist.empty()) {
        std::string fn = worklist.back();
        worklist.pop_back();

        auto it = functionDefs.find(fn);
        if (it == functionDefs.end())
            continue;   // no definition in this file

        for (const auto& bodyNode : it->second) {
            processBody(bodyNode, processBody);
        }
    }

    std::string sourceText(preprocessedSource, sourceSize);
    // Emit transformed code
    std::string output;
    size_t cursor = 0;
    for (const auto& r : replacements) {
        output += sourceText.substr(cursor, r.startByte - cursor);
        output += r.replacement;
        cursor = r.endByte;
    }
    output += sourceText.substr(cursor);

    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

std::string
rewriteResourceDescriptorCalls(const char* preprocessedSource,
    size_t       sourceSize,
    const std::string& entryPointName,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, sourceSize);
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());
    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser, nullptr, parseView.data(), static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    std::unordered_map<std::string, std::vector<TSNode>> functionDefs;

    BuildFunctionDefs(functionDefs, preprocessedSource, root);

    // Then do the same call-graph walk, but instead of collecting into a set,
    // build a vector<Replacement> by looking up replacementMap[id].
    // Finally, apply the text splice just as you already have:

    // [parse & build functionDefs, then BFS same as collect]

    std::vector<Replacement> replacements;
    std::unordered_set<std::string> visited;
    std::vector<std::string> worklist;
    worklist.push_back(entryPointName);

    auto processBody = [&](const TSNode& bodyNode, auto&& processBodyRef) -> void {
        // Walk the subtree of this body looking for:
        //  a) ResourceDescriptorIndex calls -> record replacements
        //  b) Other function calls -> enqueue them if unvisited
        std::function<void(TSNode)> walk = [&](TSNode node) {
            const char* type = ts_node_type(node);

            if (strcmp(type, "call_expression") == 0) {
                // Check for ResourceDescriptorIndex(...)
                TSNode functionNode =
                    ts_node_child_by_field_name(node, "function", static_cast<uint32_t>(strlen("function")));

                if (ts_node_is_null(functionNode)) return;

                // Pull out function name from source
                uint32_t start = ts_node_start_byte(functionNode);
                uint32_t end = ts_node_end_byte(functionNode);

                // Slice out the raw text
                std::string funcName(preprocessedSource + start, end - start);

                // trim whitespace:
                auto l = funcName.find_first_not_of(" \t\n\r");
                auto r = funcName.find_last_not_of(" \t\n\r");
                if (l != std::string::npos && r != std::string::npos)
                    funcName = funcName.substr(l, r - l + 1);


                if (funcName == "ResourceDescriptorIndex" || funcName == "OptionalResourceDescriptorIndex") {
                    TSNode argList = ts_node_child_by_field_name(node, "arguments", 9);
                    if (ts_node_named_child_count(argList) == 1) {
                        TSNode argNode = ts_node_named_child(argList, 0);

                        uint32_t start = ts_node_start_byte(argNode);
                        uint32_t end = ts_node_end_byte(argNode);

                        std::string rawText(preprocessedSource + start, end - start);

                        // if it's a quoted string literal, strip the quotes
                        if (rawText.size() >= 2 && rawText.front() == '"' && rawText.back() == '"') {
                            rawText = rawText.substr(1, rawText.size() - 2);
                        }

                        std::string identifier = std::move(rawText);
                        if (replacementMap.count(identifier)) {
                            replacements.push_back({
                            ts_node_start_byte(node),
                            ts_node_end_byte(node),
                            replacementMap.at(identifier)
                                });
                        }
                        else {
                            throw std::runtime_error(
                                "ResourceDescriptorIndex identifier does not have mapped replacement: " + identifier);
                        }
                    }
                }
                else {
                    // Otherwise, a normal function call:
                    // Enqueue it for later processing if we know its definition
                    if (functionDefs.count(funcName) && !visited.count(funcName)) {
                        visited.insert(funcName);
                        worklist.push_back(funcName);
                    }
                }
            }

            // recurse
            uint32_t n = ts_node_child_count(node);
            for (uint32_t i = 0; i < n; ++i) {
                walk(ts_node_child(node, i));
            }
            };

        walk(bodyNode);
        };

    // Traverse the call graph
    while (!worklist.empty()) {
        std::string fn = worklist.back();
        worklist.pop_back();

        auto it = functionDefs.find(fn);
        if (it == functionDefs.end())
            continue;   // no definition in this file

        for (const auto& bodyNode : it->second) {
            processBody(bodyNode, processBody);
        }
    }

    // Sort replacements by startByte
    std::sort(replacements.begin(), replacements.end(),
        [](const Replacement& a, const Replacement& b) {
            return a.startByte < b.startByte;
        });

    std::string source(preprocessedSource, sourceSize);
    std::string out;
    size_t cursor = 0;
    for (auto& r : replacements) {
        out.append(source, cursor, r.startByte - cursor);
        out += r.replacement;
        cursor = r.endByte;
    }
    out.append(source, cursor);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return out;
}

static uint32_t
shrinkToIncludeDecorators(const char* src, uint32_t origStart)
{
    uint32_t newStart = origStart;
    while (newStart > 0) {
        // find the '\n' that ends the *previous* line
        auto prevNL = std::string_view(src, newStart).rfind('\n');
        if (prevNL == std::string::npos) break;
        // prevNL points at the '\n' before the declaration line,
        // so the line we care about runs from (prevNL_of_that + 1) .. prevNL-1
        size_t lineEnd = prevNL;
        size_t lineBegin = std::string_view(src, prevNL).rfind('\n');
        if (lineBegin == std::string_view::npos) lineBegin = 0;
        else                                    lineBegin += 1;

        // extract that line and trim it
        auto   len = lineEnd - lineBegin;
        auto   sv = std::string_view(src + lineBegin, len);
        auto   first = sv.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            // blank line: skip it, but keep going up
            newStart = (uint32_t)lineBegin;
            continue;
        }
        if (sv[first] == '[') {
            // decorator: include this line too
            newStart = (uint32_t)lineBegin;
            continue;
        }
        // otherwise: stop looking
        break;
    }
    return newStart;
}

struct Range { uint32_t start, end; };
std::string
pruneUnusedCode(const char* preprocessedSource,
    size_t       sourceSize,
    const std::string& entryPointName,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, sourceSize);
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());
    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(parser, nullptr, parseView.data(), static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    std::unordered_map<std::string, std::vector<TSNode>> bodyMap, defMap;
    std::vector<TSNode> functionDefinitions;
    CollectFunctionDefinitions(root, functionDefinitions);
    for (const TSNode& node : functionDefinitions) {
        auto fnName = ExtractFunctionNameFromDefinition(preprocessedSource, node);
        if (!fnName.has_value()) {
            continue;
        }

        // store the full def node and its body

        if (!defMap.contains(*fnName)) {
            defMap[*fnName] = {};
        }

        if (!bodyMap.contains(*fnName)) {
            bodyMap[*fnName] = {};
        }

        defMap[*fnName].push_back(node);
        bodyMap[*fnName].push_back(ts_node_child_by_field_name(node, "body", static_cast<uint32_t>(strlen("body"))));
    }

    // BFS from entryPointName to find reachable functions
    std::unordered_set<std::string> visited{ entryPointName };
    std::vector<std::string> work{ entryPointName };

    auto enqueueCalls = [&](TSNode body) {
        std::function<void(TSNode)> walk = [&](TSNode n) {
            if (ts_node_is_null(n)) return;
            if (std::string(ts_node_type(n)) == "call_expression") {
                TSNode fn = ts_node_child_by_field_name(n, "function", static_cast<uint32_t>(strlen("function")));
                if (!ts_node_is_null(fn)) {

                    TSNode args = ts_node_child_by_field_name(n, "arguments", static_cast<uint32_t>(strlen("arguments")));

                    uint32_t start = ts_node_start_byte(fn);
                    uint32_t end = ts_node_end_byte(args);    // include closing ')'

                    std::string callSig(
                        preprocessedSource + start,
                        end - start
                    );

                    uint32_t ss = ts_node_start_byte(fn), ee = ts_node_end_byte(fn);
                    std::string called(preprocessedSource + ss, ee - ss);
                    auto l = called.find_first_not_of(" \t\r\n"),
                        r = called.find_last_not_of(" \t\r\n");
                    if (l != std::string::npos && r != std::string::npos)
                        called = called.substr(l, r - l + 1);
                    if (bodyMap.count(called) && !visited.count(called)) {
                        visited.insert(called);
                        work.push_back(called);
                    }
                }
            }
            uint32_t c = ts_node_child_count(n);
            for (uint32_t i = 0; i < c; ++i) walk(ts_node_child(n, i));
            };
        walk(body);
        };

    while (!work.empty()) {
        auto fn = work.back(); work.pop_back();
        auto it = bodyMap.find(fn);
        if (it != bodyMap.end())
            for (auto& body : it->second) {
                enqueueCalls(body);
            }
    }

    std::vector<Range> removeRanges;
    for (auto const& kv : defMap) {
        if (!visited.count(kv.first)) {
            for (auto& defNode : kv.second) {
                uint32_t origStart = ts_node_start_byte(defNode);
                uint32_t origEnd = ts_node_end_byte(defNode);
                uint32_t adjStart = shrinkToIncludeDecorators( //TODO: Probably not needed with new grammar
                    preprocessedSource, origStart);
                removeRanges.push_back({ adjStart, origEnd });
            }
        }
    }

    // merge overlapping removeRanges
    std::sort(removeRanges.begin(), removeRanges.end(),
        [](auto& a, auto& b) { return a.start < b.start; });
    std::vector<Range> merged;
    for (auto& r : removeRanges) {
        if (merged.empty() || r.start > merged.back().end) {
            merged.push_back(r);
        }
        else {
            merged.back().end = std::max(merged.back().end, r.end);
        }
    }

    // keepRanges as complement of merged removeRanges
    std::vector<Range> keep;
    uint32_t lastEnd = 0;
    for (auto& r : merged) {
        if (lastEnd < r.start)
            keep.push_back({ lastEnd, r.start });
        lastEnd = std::max(lastEnd, r.end);
    }
    if (lastEnd < sourceSize)
        keep.push_back({ lastEnd, (uint32_t)sourceSize });

    // splice keepRanges together
    std::string out;
    out.reserve(sourceSize);
    for (auto& r : keep)
        out.append(preprocessedSource + r.start, r.end - r.start);

    // cleanup
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return out;
}

// SM 6.8 library parsing
struct ShaderEntryPointDesc
{
    std::string functionName;     // e.g. "MyNode"
    std::string shaderAttribute;  // e.g. "node", "compute", "pixel", etc. (value inside Shader("..."))
    uint32_t    functionStartByte = 0; // ts_node_start_byte(function_definition)
    uint32_t    functionEndByte = 0; // ts_node_end_byte(function_definition)
};

struct ShaderLibraryBRSLAnalysis
{
    std::vector<ShaderEntryPointDesc> entryPoints;
    std::unordered_set<std::string> mandatoryIdentifiers;
    std::unordered_set<std::string> optionalIdentifiers;
};

static inline bool IsIdentChar(char c)
{
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static std::string TrimCopy(std::string_view sv)
{
    size_t l = 0;
    while (l < sv.size() && (sv[l] == ' ' || sv[l] == '\t' || sv[l] == '\r' || sv[l] == '\n')) ++l;
    size_t r = sv.size();
    while (r > l && (sv[r - 1] == ' ' || sv[r - 1] == '\t' || sv[r - 1] == '\r' || sv[r - 1] == '\n')) --r;
    return std::string(sv.substr(l, r - l));
}

static bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// Remove //... and /*...*/ from a small snippet (decorator block).
static std::string StripComments(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/')
        {
            // line comment
            i += 2;
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*')
        {
            // block comment
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            if (i + 1 < s.size()) i += 2;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

static std::optional<std::string> ExtractFunctionName(
    const char* preprocessedSource,
    const TSNode& functionDefNode)
{
    return ExtractFunctionNameFromDefinition(preprocessedSource, functionDefNode);
}

// Returns the string inside Shader("...") if present; otherwise nullopt.
static std::optional<std::string> TryParseShaderDecorator(std::string_view decoratorBlockRaw)
{
    // Make false-positives less likely by removing comments first.
    std::string cleaned = StripComments(decoratorBlockRaw);
    std::string_view s(cleaned);

    // scan for word "shader" (case-insensitive), with identifier boundaries
    for (size_t i = 0; i + 6 <= s.size(); ++i)
    {
        if (!IEquals(s.substr(i, 6), "shader")) continue;

        // word boundary
        if (i > 0 && IsIdentChar(s[i - 1])) continue;
        if (i + 6 < s.size() && IsIdentChar(s[i + 6])) continue;

        size_t p = i + 6;

        // skip whitespace
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
        if (p >= s.size() || s[p] != '(') continue;
        ++p;

        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
        if (p >= s.size() || s[p] != '"') continue;

        // parse a basic string literal (supports \" escaping)
        ++p;
        std::string value;
        while (p < s.size())
        {
            char c = s[p++];
            if (c == '\\' && p < s.size())
            {
                char next = s[p++];
                // keep simple: accept \" and \\ and \n \t
                if (next == '"' || next == '\\') value.push_back(next);
                else if (next == 'n') value.push_back('\n');
                else if (next == 't') value.push_back('\t');
                else value.push_back(next);
                continue;
            }
            if (c == '"') break;
            value.push_back(c);
        }

        // We don't strictly need to verify closing ) and ] here.
        if (!value.empty())
            return value;

        return std::string{}; // allow empty Shader("")
    }

    return std::nullopt;
}

static std::vector<ShaderEntryPointDesc> ExtractShaderLibraryEntryPoints(
    const char* preprocessedSource,
    size_t sourceSize,
    const TSNode& root)
{
    std::vector<ShaderEntryPointDesc> out;

    std::vector<TSNode> functionDefinitions;
    CollectFunctionDefinitions(root, functionDefinitions);
    out.reserve(functionDefinitions.size());

    for (const TSNode& node : functionDefinitions)
    {

        auto nameOpt = ExtractFunctionName(preprocessedSource, node);
        if (!nameOpt.has_value()) {
            continue;
        }

        uint32_t fnStart = ts_node_start_byte(node);
        uint32_t fnEnd = ts_node_end_byte(node);

        // decorator expansion:
        std::string_view decorBlock(preprocessedSource + fnStart, fnEnd - fnStart);

        auto shaderAttr = TryParseShaderDecorator(decorBlock);
        if (!shaderAttr.has_value())
            continue; // not a library entrypoint

        ShaderEntryPointDesc ep;
        ep.functionName = std::move(*nameOpt);
        ep.shaderAttribute = std::move(*shaderAttr);
        ep.functionStartByte = fnStart;
        ep.functionEndByte = fnEnd;
        out.push_back(std::move(ep));
    }

    // stable ordering by source position
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        return a.functionStartByte < b.functionStartByte;
        });

    // TODO: dedupe function names? This would probably break anyway

    return out;
}

static void CollectBRSLIdentifiersFromRoots(
    std::unordered_set<std::string>& outMandatory,
    std::unordered_set<std::string>& outOptional,
    const char* preprocessedSource,
    const std::unordered_map<std::string, std::vector<TSNode>>& functionDefs,
    const std::vector<std::string>& rootFunctions)
{
    std::unordered_set<std::string> visited;
    std::vector<std::string> worklist;
    worklist.reserve(rootFunctions.size() * 2);

    for (auto& r : rootFunctions)
    {
        if (visited.insert(r).second)
            worklist.push_back(r);
    }

    auto parseSingleBuiltinArg = [&](TSNode callExprNode) -> std::string
        {
            TSNode argList = ts_node_child_by_field_name(
                callExprNode, "arguments",
                static_cast<uint32_t>(strlen("arguments")));
            if (ts_node_is_null(argList) || ts_node_named_child_count(argList) != 1)
                throw std::runtime_error("ResourceDescriptorIndex requires exactly one argument");

            TSNode argNode = ts_node_named_child(argList, 0);
            uint32_t start = ts_node_start_byte(argNode);
            uint32_t end = ts_node_end_byte(argNode);

            std::string raw(preprocessedSource + start, end - start);

            // strip quotes if it's a string literal
            if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                raw = raw.substr(1, raw.size() - 2);

            return raw;
        };

    auto trimInPlace = [](std::string& s)
        {
            auto l = s.find_first_not_of(" \t\r\n");
            auto r = s.find_last_not_of(" \t\r\n");
            if (l == std::string::npos) { s.clear(); return; }
            s = s.substr(l, r - l + 1);
        };

    // Walk a function body: record builtins, enqueue calls.
    auto processBody = [&](const TSNode& bodyNode)
        {
            std::function<void(TSNode)> walk = [&](TSNode node)
                {
                    if (ts_node_is_null(node)) return;

                    if (strcmp(ts_node_type(node), "call_expression") == 0)
                    {
                        TSNode fnNode = ts_node_child_by_field_name(
                            node, "function",
                            static_cast<uint32_t>(strlen("function")));
                        if (!ts_node_is_null(fnNode))
                        {
                            uint32_t s = ts_node_start_byte(fnNode);
                            uint32_t e = ts_node_end_byte(fnNode);
                            std::string funcName(preprocessedSource + s, e - s);
                            trimInPlace(funcName);

                            if (funcName == "ResourceDescriptorIndex")
                            {
                                outMandatory.insert(parseSingleBuiltinArg(node));
                            }
                            else if (funcName == "OptionalResourceDescriptorIndex")
                            {
                                outOptional.insert(parseSingleBuiltinArg(node));
                            }
                            else
                            {
                                // normal call -> enqueue if we have its definition
                                auto it = functionDefs.find(funcName);
                                if (it != functionDefs.end())
                                {
                                    if (visited.insert(funcName).second)
                                        worklist.push_back(funcName);
                                }
                            }
                        }
                    }

                    uint32_t n = ts_node_child_count(node);
                    for (uint32_t i = 0; i < n; ++i)
                        walk(ts_node_child(node, i));
                };

            walk(bodyNode);
        };

    while (!worklist.empty())
    {
        std::string fn = std::move(worklist.back());
        worklist.pop_back();

        auto it = functionDefs.find(fn);
        if (it == functionDefs.end())
            continue;

        for (const TSNode& body : it->second)
            processBody(body);
    }
}

ShaderLibraryBRSLAnalysis AnalyzePreprocessedShaderLibrary(
    const DxcBuffer* pPreprocessedBuffer)
{
    const char* preprocessedSource = static_cast<const char*>(pPreprocessedBuffer->Ptr);
    size_t sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, pPreprocessedBuffer->Size);

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser, nullptr, parseView.data(),
        static_cast<uint32_t>(parseView.size()));

    TSNode root = ts_tree_root_node(tree);

    // Build function name -> bodies
    std::unordered_map<std::string, std::vector<TSNode>> functionDefs;
    BuildFunctionDefs(functionDefs, preprocessedSource, root);

    ShaderLibraryBRSLAnalysis result;
    result.entryPoints = ExtractShaderLibraryEntryPoints(preprocessedSource, sourceSize, root);

    // Convert entrypoint descs to just root function names
    std::vector<std::string> roots;
    roots.reserve(result.entryPoints.size());
    for (auto& ep : result.entryPoints)
        roots.push_back(ep.functionName);

    // Union of BRSL ids over all entrypoints
    CollectBRSLIdentifiersFromRoots(
        result.mandatoryIdentifiers,
        result.optionalIdentifiers,
        preprocessedSource,
        functionDefs,
        roots);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return result;
}

static inline void TrimInPlace(std::string& s)
{
    auto l = s.find_first_not_of(" \t\n\r");
    auto r = s.find_last_not_of(" \t\n\r");
    if (l == std::string::npos) { s.clear(); return; }
    s = s.substr(l, r - l + 1);
}

struct ResourceDescriptorCallSite
{
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    bool isOptional = false;
    std::string identifier;
};

static inline bool IsValidUtf8(std::string_view bytes)
{
    size_t i = 0;
    while (i < bytes.size()) {
        const uint8_t c0 = static_cast<uint8_t>(bytes[i]);
        if (c0 <= 0x7F) {
            ++i;
            continue;
        }

        auto isContinuation = [&](size_t idx) -> bool {
            return idx < bytes.size() && (static_cast<uint8_t>(bytes[idx]) & 0xC0u) == 0x80u;
            };

        if (c0 >= 0xC2 && c0 <= 0xDF) {
            if (!isContinuation(i + 1)) {
                return false;
            }
            i += 2;
            continue;
        }

        if (c0 == 0xE0) {
            if (i + 2 >= bytes.size()) {
                return false;
            }
            const uint8_t c1 = static_cast<uint8_t>(bytes[i + 1]);
            if (!(c1 >= 0xA0 && c1 <= 0xBF) || !isContinuation(i + 2)) {
                return false;
            }
            i += 3;
            continue;
        }

        if ((c0 >= 0xE1 && c0 <= 0xEC) || (c0 >= 0xEE && c0 <= 0xEF)) {
            if (!isContinuation(i + 1) || !isContinuation(i + 2)) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 == 0xED) {
            if (i + 2 >= bytes.size()) {
                return false;
            }
            const uint8_t c1 = static_cast<uint8_t>(bytes[i + 1]);
            if (!(c1 >= 0x80 && c1 <= 0x9F) || !isContinuation(i + 2)) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 == 0xF0) {
            if (i + 3 >= bytes.size()) {
                return false;
            }
            const uint8_t c1 = static_cast<uint8_t>(bytes[i + 1]);
            if (!(c1 >= 0x90 && c1 <= 0xBF) || !isContinuation(i + 2) || !isContinuation(i + 3)) {
                return false;
            }
            i += 4;
            continue;
        }

        if (c0 >= 0xF1 && c0 <= 0xF3) {
            if (!isContinuation(i + 1) || !isContinuation(i + 2) || !isContinuation(i + 3)) {
                return false;
            }
            i += 4;
            continue;
        }

        if (c0 == 0xF4) {
            if (i + 3 >= bytes.size()) {
                return false;
            }
            const uint8_t c1 = static_cast<uint8_t>(bytes[i + 1]);
            if (!(c1 >= 0x80 && c1 <= 0x8F) || !isContinuation(i + 2) || !isContinuation(i + 3)) {
                return false;
            }
            i += 4;
            continue;
        }

        return false;
    }

    return true;
}

static inline void ValidateUtf8OrThrow(std::string_view bytes, std::string_view label)
{
    if (!IsValidUtf8(bytes)) {
        throw std::runtime_error(std::string(label) + " is not valid UTF-8");
    }
}

static inline bool IsIdentifierLikeChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

static inline size_t SkipQuotedLiteral(std::string_view source, size_t index, char quote)
{
    ++index;
    while (index < source.size()) {
        const char c = source[index];
        if (c == '\\') {
            index += 2;
            continue;
        }
        ++index;
        if (c == quote) {
            break;
        }
    }
    return index;
}

static inline void SkipWhitespaceAndComments(std::string_view source, size_t& index)
{
    while (index < source.size()) {
        const char c = source[index];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++index;
            continue;
        }

        if (c == '/' && index + 1 < source.size()) {
            if (source[index + 1] == '/') {
                index += 2;
                while (index < source.size() && source[index] != '\n') {
                    ++index;
                }
                continue;
            }

            if (source[index + 1] == '*') {
                index += 2;
                while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) {
                    ++index;
                }
                if (index + 1 < source.size()) {
                    index += 2;
                }
                continue;
            }
        }

        break;
    }
}

static inline bool StartsWithTokenAtBoundary(
    std::string_view source,
    size_t offset,
    std::string_view token)
{
    if (offset + token.size() > source.size()) {
        return false;
    }

    if (source.substr(offset, token.size()) != token) {
        return false;
    }

    if (offset > 0 && IsIdentifierLikeChar(source[offset - 1])) {
        return false;
    }

    const size_t end = offset + token.size();
    if (end < source.size() && IsIdentifierLikeChar(source[end])) {
        return false;
    }

    return true;
}

static inline std::optional<ResourceDescriptorCallSite> TryParseResourceDescriptorCallAt(
    std::string_view source,
    size_t functionStart)
{
    static constexpr std::string_view optionalName = "OptionalResourceDescriptorIndex";
    static constexpr std::string_view mandatoryName = "ResourceDescriptorIndex";

    std::string_view functionName;
    bool isOptional = false;

    if (StartsWithTokenAtBoundary(source, functionStart, optionalName)) {
        functionName = optionalName;
        isOptional = true;
    }
    else if (StartsWithTokenAtBoundary(source, functionStart, mandatoryName)) {
        functionName = mandatoryName;
    }
    else {
        return std::nullopt;
    }

    size_t cursor = functionStart + functionName.size();
    SkipWhitespaceAndComments(source, cursor);
    if (cursor >= source.size() || source[cursor] != '(') {
        return std::nullopt;
    }

    const size_t argumentStart = cursor + 1;
    size_t argumentEnd = argumentStart;
    bool sawTopLevelComma = false;
    int depth = 1;
    ++cursor;

    while (cursor < source.size()) {
        const char c = source[cursor];

        if (c == '"' || c == '\'') {
            cursor = SkipQuotedLiteral(source, cursor, c);
            continue;
        }

        if (c == '/' && cursor + 1 < source.size()) {
            if (source[cursor + 1] == '/') {
                cursor += 2;
                while (cursor < source.size() && source[cursor] != '\n') {
                    ++cursor;
                }
                continue;
            }

            if (source[cursor + 1] == '*') {
                cursor += 2;
                while (cursor + 1 < source.size() && !(source[cursor] == '*' && source[cursor + 1] == '/')) {
                    ++cursor;
                }
                if (cursor + 1 < source.size()) {
                    cursor += 2;
                }
                continue;
            }
        }

        if (c == '(') {
            ++depth;
            ++cursor;
            continue;
        }

        if (c == ')') {
            --depth;
            if (depth == 0) {
                argumentEnd = cursor;
                ++cursor;
                break;
            }
            ++cursor;
            continue;
        }

        if (c == ',' && depth == 1) {
            sawTopLevelComma = true;
        }

        ++cursor;
    }

    if (depth != 0) {
        throw std::runtime_error("Unterminated ResourceDescriptorIndex call in shader source");
    }

    if (sawTopLevelComma) {
        throw std::runtime_error("ResourceDescriptorIndex requires exactly one argument");
    }

    std::string identifier(source.substr(argumentStart, argumentEnd - argumentStart));
    TrimInPlace(identifier);

    if (identifier.size() >= 2 && identifier.front() == '"' && identifier.back() == '"') {
        identifier = identifier.substr(1, identifier.size() - 2);
    }

    ResourceDescriptorCallSite callSite;
    callSite.startByte = static_cast<uint32_t>(functionStart);
    callSite.endByte = static_cast<uint32_t>(cursor);
    callSite.isOptional = isOptional;
    callSite.identifier = std::move(identifier);
    return callSite;
}

static inline std::vector<ResourceDescriptorCallSite> CollectResourceDescriptorCallsFromText(
    std::string_view source)
{
    std::vector<ResourceDescriptorCallSite> callSites;
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (c == '"' || c == '\'') {
            i = SkipQuotedLiteral(source, i, c);
            continue;
        }

        if (c == '/' && i + 1 < source.size()) {
            if (source[i + 1] == '/') {
                i += 2;
                while (i < source.size() && source[i] != '\n') {
                    ++i;
                }
                continue;
            }

            if (source[i + 1] == '*') {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
                    ++i;
                }
                if (i + 1 < source.size()) {
                    i += 2;
                }
                continue;
            }
        }

        auto callSite = TryParseResourceDescriptorCallAt(source, i);
        if (callSite.has_value()) {
            callSites.push_back(*callSite);
            i = callSite->endByte;
            continue;
        }

        ++i;
    }

    return callSites;
}

static inline void CollectResourceDescriptorIdentifiersFromText(
    std::unordered_set<std::string>& outMandatory,
    std::unordered_set<std::string>& outOptional,
    std::string_view source)
{
    const auto callSites = CollectResourceDescriptorCallsFromText(source);
    for (const auto& callSite : callSites) {
        if (callSite.isOptional) {
            outOptional.insert(callSite.identifier);
        }
        else {
            outMandatory.insert(callSite.identifier);
        }
    }
}

static inline std::string RewriteResourceDescriptorCallsInText(
    std::string_view source,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    const auto callSites = CollectResourceDescriptorCallsFromText(source);
    if (callSites.empty()) {
        return std::string(source);
    }

    std::string out;
    out.reserve(source.size() + callSites.size() * 8);

    size_t cursor = 0;
    for (const auto& callSite : callSites) {
        out.append(source.substr(cursor, callSite.startByte - cursor));

        auto replacementIt = replacementMap.find(callSite.identifier);
        if (replacementIt == replacementMap.end()) {
            throw std::runtime_error(
                "ResourceDescriptorIndex identifier does not have mapped replacement: " + callSite.identifier);
        }

        out += replacementIt->second;
        cursor = callSite.endByte;
    }

    out.append(source.substr(cursor));
    return out;
}

static inline bool ContainsTokenOutsideCommentsAndStrings(
    std::string_view source,
    std::string_view token)
{
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (c == '"' || c == '\'') {
            i = SkipQuotedLiteral(source, i, c);
            continue;
        }

        if (c == '/' && i + 1 < source.size()) {
            if (source[i + 1] == '/') {
                i += 2;
                while (i < source.size() && source[i] != '\n') {
                    ++i;
                }
                continue;
            }

            if (source[i + 1] == '*') {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
                    ++i;
                }
                if (i + 1 < source.size()) {
                    i += 2;
                }
                continue;
            }
        }

        if (i + token.size() <= source.size() && source.substr(i, token.size()) == token) {
            return true;
        }

        ++i;
    }

    return false;
}

static inline std::string SanitizeSourceSnippet(std::string_view snippet)
{
    std::string sanitized;
    sanitized.reserve((std::min)(snippet.size(), size_t(160)));

    bool previousWasSpace = false;
    for (char c : snippet) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }

        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!previousWasSpace && !sanitized.empty()) {
                sanitized.push_back(' ');
            }
            previousWasSpace = true;
            continue;
        }

        previousWasSpace = false;
        sanitized.push_back(c);
        if (sanitized.size() >= 160) {
            sanitized += "...";
            break;
        }
    }

    if (!sanitized.empty() && sanitized.back() == ' ') {
        sanitized.pop_back();
    }

    return sanitized;
}

static inline std::string BuildParseErrorSummary(const char* source, size_t sourceSize, const TSNode& errorNode)
{
    const TSPoint startPoint = ts_node_start_point(errorNode);
    const TSPoint endPoint = ts_node_end_point(errorNode);
    const uint32_t startByte = ts_node_start_byte(errorNode);
    const uint32_t endByte = ts_node_end_byte(errorNode);

    const size_t contextStart = startByte > 48u ? startByte - 48u : 0u;
    const size_t contextEnd = (std::min)(sourceSize, static_cast<size_t>(endByte) + 48u);
    std::string snippet = SanitizeSourceSnippet(
        std::string_view(source + contextStart, contextEnd - contextStart));

    std::ostringstream stream;
    stream << "ERROR@" << (startPoint.row + 1) << ":" << (startPoint.column + 1)
           << "-" << (endPoint.row + 1) << ":" << (endPoint.column + 1);

    TSNode parentNode = ts_node_parent(errorNode);
    if (!ts_node_is_null(parentNode)) {
        stream << " parent=" << ts_node_type(parentNode);
    }

    TSNode previousSibling = ts_node_prev_sibling(errorNode);
    if (!ts_node_is_null(previousSibling)) {
        stream << " prev=" << ts_node_type(previousSibling);
    }

    TSNode nextSibling = ts_node_next_sibling(errorNode);
    if (!ts_node_is_null(nextSibling)) {
        stream << " next=" << ts_node_type(nextSibling);
    }

    stream << " snippet=\"" << snippet << "\"";
    return stream.str();
}

static inline void CollectParseErrorSummaries(
    const char* source,
    size_t sourceSize,
    const TSNode& node,
    std::vector<std::string>& outSummaries,
    size_t maxSummaries)
{
    if (ts_node_is_null(node) || outSummaries.size() >= maxSummaries) {
        return;
    }

    if (strcmp(ts_node_type(node), "ERROR") == 0) {
        outSummaries.push_back(BuildParseErrorSummary(source, sourceSize, node));
        if (outSummaries.size() >= maxSummaries) {
            return;
        }
    }

    const uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        CollectParseErrorSummaries(source, sourceSize, ts_node_child(node, i), outSummaries, maxSummaries);
        if (outSummaries.size() >= maxSummaries) {
            return;
        }
    }
}

static inline void CountParseErrorNodes(const TSNode& node, uint32_t& errorNodeCount)
{
    if (ts_node_is_null(node)) {
        return;
    }

    if (strcmp(ts_node_type(node), "ERROR") == 0) {
        ++errorNodeCount;
    }

    const uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        CountParseErrorNodes(ts_node_child(node, i), errorNodeCount);
    }
}

static inline void BuildFunctionCatalogDetailed(
    const char* source,
    const TSNode& root,
    std::unordered_map<std::string, std::vector<TSNode>>& outDefinitionNodes,
    std::unordered_map<std::string, std::vector<TSNode>>& outBodyNodes,
    std::unordered_set<std::string>& outDefinedFunctionNames,
    ShaderPreprocessDiagnostics& diagnostics)
{
    std::vector<TSNode> functionDefinitions;
    CollectFunctionDefinitions(root, functionDefinitions);
    diagnostics.discoveredFunctionDefinitions.reserve(functionDefinitions.size());

    for (const TSNode& definitionNode : functionDefinitions) {
        auto functionName = ExtractFunctionNameFromDefinition(source, definitionNode);
        if (!functionName.has_value()) {
            diagnostics.unnamedFunctionDefinitions.push_back(
                "function_definition@" + std::to_string(ts_node_start_byte(definitionNode)));
            continue;
        }

        diagnostics.discoveredFunctionDefinitions.push_back(*functionName);
        outDefinedFunctionNames.insert(*functionName);
        outDefinitionNodes[*functionName].push_back(definitionNode);

        TSNode bodyNode = ts_node_child_by_field_name(
            definitionNode,
            "body",
            static_cast<uint32_t>(strlen("body")));
        if (!ts_node_is_null(bodyNode)) {
            outBodyNodes[*functionName].push_back(bodyNode);
        }
    }

    SortAndUniqueStrings(diagnostics.discoveredFunctionDefinitions);
    SortAndUniqueStrings(diagnostics.unnamedFunctionDefinitions);
}

static inline std::vector<std::string> CollectUndefinedInternalCalls(
    const char* source,
    const TSNode& root,
    const std::unordered_set<std::string>& originalDefinedFunctionNames)
{
    std::unordered_map<std::string, std::vector<TSNode>> definitionNodes;
    std::unordered_map<std::string, std::vector<TSNode>> bodyNodes;
    std::unordered_set<std::string> currentlyDefinedFunctionNames;
    ShaderPreprocessDiagnostics ignoredDiagnostics;
    BuildFunctionCatalogDetailed(
        source,
        root,
        definitionNodes,
        bodyNodes,
        currentlyDefinedFunctionNames,
        ignoredDiagnostics);

    std::vector<std::string> unresolved;

    std::function<void(TSNode)> walk = [&](TSNode node) {
        if (ts_node_is_null(node)) {
            return;
        }

        if (strcmp(ts_node_type(node), "call_expression") == 0) {
            TSNode functionNode = ts_node_child_by_field_name(
                node,
                "function",
                static_cast<uint32_t>(strlen("function")));
            if (!ts_node_is_null(functionNode)) {
                uint32_t start = ts_node_start_byte(functionNode);
                uint32_t end = ts_node_end_byte(functionNode);
                std::string functionName(source + start, end - start);
                TrimInPlace(functionName);

                if (originalDefinedFunctionNames.contains(functionName) &&
                    !currentlyDefinedFunctionNames.contains(functionName))
                {
                    unresolved.push_back(functionName);
                }
            }
        }

        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i) {
            walk(ts_node_child(node, i));
        }
        };

    walk(root);

    SortAndUniqueStrings(unresolved);
    return unresolved;
}

static inline ShaderPreprocessDiagnostics AnalyzeShaderSourceCatalog(
    const char* source,
    size_t sourceSize)
{
    sourceSize = GetNormalizedShaderSourceSize(source, sourceSize);
    ValidateUtf8OrThrow(std::string_view(source, sourceSize), "Shader source");

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(source, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        parseView.data(),
        static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    ShaderPreprocessDiagnostics diagnostics;
    diagnostics.parseSucceeded = !ts_node_is_null(root);
    if (diagnostics.parseSucceeded) {
        std::unordered_map<std::string, std::vector<TSNode>> definitionNodes;
        std::unordered_map<std::string, std::vector<TSNode>> bodyNodes;
        std::unordered_set<std::string> definedFunctionNames;

        CountParseErrorNodes(root, diagnostics.errorNodeCount);
        CollectParseErrorSummaries(source, sourceSize, root, diagnostics.parseErrorSummaries, 6);
        if (diagnostics.errorNodeCount > diagnostics.parseErrorSummaries.size()) {
            diagnostics.notes.push_back(
                "Captured " + std::to_string(diagnostics.parseErrorSummaries.size()) +
                " of " + std::to_string(diagnostics.errorNodeCount) + " parse errors.");
        }
        BuildFunctionCatalogDetailed(
            source,
            root,
            definitionNodes,
            bodyNodes,
            definedFunctionNames,
            diagnostics);
        diagnostics.safeToPrune =
            diagnostics.errorNodeCount == 0 &&
            diagnostics.unnamedFunctionDefinitions.empty();
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return diagnostics;
}

static inline bool ValidatePrunedShaderSource(
    const std::string& prunedSource,
    const std::unordered_set<std::string>& originalDefinedFunctionNames,
    ShaderPreprocessDiagnostics& inOutDiagnostics)
{
    ValidateUtf8OrThrow(prunedSource, "Pruned shader source");

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(
        prunedSource.c_str(), prunedSource.size());
    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        parseView.data(),
        static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    uint32_t prunedErrorNodeCount = 0;
    CountParseErrorNodes(root, prunedErrorNodeCount);

    std::vector<std::string> unresolvedInternalCalls =
        CollectUndefinedInternalCalls(prunedSource.c_str(), root, originalDefinedFunctionNames);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    if (prunedErrorNodeCount != 0) {
        inOutDiagnostics.notes.push_back(
            "Pruned source contained parse errors; falling back to unpruned source.");
        return false;
    }

    if (!unresolvedInternalCalls.empty()) {
        inOutDiagnostics.unresolvedInternalCalls = unresolvedInternalCalls;
        inOutDiagnostics.notes.push_back(
            "Pruned source retained calls to definitions that were removed; falling back to unpruned source.");
        return false;
    }

    return true;
}

std::string rewriteResourceDescriptorCallsMultiRoots(
    const char* preprocessedSource,
    size_t sourceSize,
    const std::vector<std::string>& rootFunctionNames,
    const std::unordered_map<std::string, std::string>& replacementMap);

std::string pruneUnusedCodeMultiRoots(
    const char* preprocessedSource,
    size_t sourceSize,
    const std::vector<std::string>& rootFunctionNames);

static inline PreparedShaderSource PrepareShaderSourceForRoots(
    const DxcBuffer& preprocessedBuffer,
    const std::vector<std::string>& rootFunctionNames)
{
    const char* source = static_cast<const char*>(preprocessedBuffer.Ptr);
    const size_t sourceSize = GetNormalizedShaderSourceSize(source, preprocessedBuffer.Size);

    if (!source || sourceSize == 0) {
        throw std::runtime_error("PrepareShaderSourceForRoots: empty preprocessed buffer");
    }

    if (rootFunctionNames.empty()) {
        throw std::runtime_error("PrepareShaderSourceForRoots: no root functions provided");
    }

    ValidateUtf8OrThrow(std::string_view(source, sourceSize), "Preprocessed shader source");

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(source, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        parseView.data(),
        static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    PreparedShaderSource prepared;
    prepared.diagnostics.parseSucceeded = !ts_node_is_null(root);

    std::unordered_map<std::string, std::vector<TSNode>> definitionNodes;
    std::unordered_map<std::string, std::vector<TSNode>> bodyNodes;

    if (prepared.diagnostics.parseSucceeded) {
        CountParseErrorNodes(root, prepared.diagnostics.errorNodeCount);
        CollectParseErrorSummaries(source, sourceSize, root, prepared.diagnostics.parseErrorSummaries, 6);
        if (prepared.diagnostics.errorNodeCount > prepared.diagnostics.parseErrorSummaries.size()) {
            prepared.diagnostics.notes.push_back(
                "Captured " + std::to_string(prepared.diagnostics.parseErrorSummaries.size()) +
                " of " + std::to_string(prepared.diagnostics.errorNodeCount) + " parse errors.");
        }
        BuildFunctionCatalogDetailed(
            source,
            root,
            definitionNodes,
            bodyNodes,
            prepared.originalDefinedFunctionNames,
            prepared.diagnostics);

        prepared.diagnostics.safeToPrune =
            prepared.diagnostics.errorNodeCount == 0 &&
            prepared.diagnostics.unnamedFunctionDefinitions.empty();
    }
    else {
        prepared.diagnostics.notes.push_back("Tree-sitter failed to produce a root node; pruning disabled.");
    }

    for (const std::string& rootFunctionName : rootFunctionNames) {
        if (!prepared.originalDefinedFunctionNames.contains(rootFunctionName)) {
            prepared.diagnostics.safeToPrune = false;
            prepared.diagnostics.notes.push_back(
                "Root function '" + rootFunctionName + "' was not found in the parsed definition catalog.");
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    prepared.sourceBeforeRewrite.assign(source, sourceSize);

    if (prepared.diagnostics.safeToPrune) {
        std::string prunedSource = pruneUnusedCodeMultiRoots(
            prepared.sourceBeforeRewrite.c_str(),
            prepared.sourceBeforeRewrite.size(),
            rootFunctionNames);

        if (ValidatePrunedShaderSource(
            prunedSource,
            prepared.originalDefinedFunctionNames,
            prepared.diagnostics))
        {
            prepared.diagnostics.pruningApplied = prunedSource != prepared.sourceBeforeRewrite;
            prepared.sourceBeforeRewrite = std::move(prunedSource);
        }
        else {
            prepared.diagnostics.usedFallbackRewrite = true;
            prepared.diagnostics.pruningApplied = false;
        }
    }
    else {
        prepared.diagnostics.usedFallbackRewrite = true;
    }

    std::unordered_set<std::string> mandatoryIdentifiers;
    std::unordered_set<std::string> optionalIdentifiers;
    CollectResourceDescriptorIdentifiersFromText(
        mandatoryIdentifiers,
        optionalIdentifiers,
        prepared.sourceBeforeRewrite);

    prepared.mandatoryIDs.assign(mandatoryIdentifiers.begin(), mandatoryIdentifiers.end());
    prepared.optionalIDs.assign(optionalIdentifiers.begin(), optionalIdentifiers.end());
    SortAndUniqueStrings(prepared.mandatoryIDs);
    SortAndUniqueStrings(prepared.optionalIDs);

    return prepared;
}

static inline PreparedShaderSource PrepareShaderSourceForEntryPoint(
    const DxcBuffer& preprocessedBuffer,
    const std::string& entryPointName)
{
    return PrepareShaderSourceForRoots(preprocessedBuffer, std::vector<std::string>{ entryPointName });
}

static inline std::string JoinStrings(
    const std::vector<std::string>& values,
    std::string_view separator)
{
    std::ostringstream stream;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            stream << separator;
        }
        stream << values[i];
    }
    return stream.str();
}

static inline std::string FormatShaderPreprocessDiagnostics(
    const ShaderPreprocessDiagnostics& diagnostics)
{
    std::ostringstream stream;
    stream << "parseSucceeded=" << (diagnostics.parseSucceeded ? "true" : "false")
           << ", errorNodeCount=" << diagnostics.errorNodeCount
           << ", safeToPrune=" << (diagnostics.safeToPrune ? "true" : "false")
           << ", usedFallbackRewrite=" << (diagnostics.usedFallbackRewrite ? "true" : "false")
           << ", pruningApplied=" << (diagnostics.pruningApplied ? "true" : "false")
           << ", finalSourceValid=" << (diagnostics.finalSourceValid ? "true" : "false");

    if (!diagnostics.parseErrorSummaries.empty()) {
        stream << ", parseErrors=[" << JoinStrings(diagnostics.parseErrorSummaries, " || ") << "]";
    }

    if (!diagnostics.unnamedFunctionDefinitions.empty()) {
        stream << ", unnamedFunctionDefinitions=[" << JoinStrings(diagnostics.unnamedFunctionDefinitions, ", ") << "]";
    }

    if (!diagnostics.unresolvedInternalCalls.empty()) {
        stream << ", unresolvedInternalCalls=[" << JoinStrings(diagnostics.unresolvedInternalCalls, ", ") << "]";
    }

    if (!diagnostics.notes.empty()) {
        stream << ", notes=[" << JoinStrings(diagnostics.notes, " | ") << "]";
    }

    return stream.str();
}

static inline std::string FinalizePreparedShaderSource(
    PreparedShaderSource& prepared,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    for (const std::string& identifier : prepared.mandatoryIDs) {
        if (!replacementMap.contains(identifier)) {
            throw std::runtime_error(
                "Missing descriptor replacement for mandatory identifier: " + identifier);
        }
    }

    for (const std::string& identifier : prepared.optionalIDs) {
        if (!replacementMap.contains(identifier)) {
            throw std::runtime_error(
                "Missing descriptor replacement for optional identifier: " + identifier);
        }
    }

    std::string finalSource = RewriteResourceDescriptorCallsInText(
        prepared.sourceBeforeRewrite,
        replacementMap);

    ValidateUtf8OrThrow(finalSource, "Final rewritten shader source");

    const auto remainingDescriptorCalls = CollectResourceDescriptorCallsFromText(finalSource);
    if (!remainingDescriptorCalls.empty()) {
        throw std::runtime_error(
            "Final rewritten shader source still contains unresolved ResourceDescriptorIndex calls");
    }

    if (ContainsTokenOutsideCommentsAndStrings(finalSource, "Builtin::")) {
        throw std::runtime_error(
            "Final rewritten shader source still contains symbolic Builtin:: references");
    }

    if (prepared.diagnostics.pruningApplied) {
        TSParser* parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_hlsl());

        const std::string parseView = BuildTreeSitterShaderParseView(
            finalSource.c_str(), finalSource.size());
        TSTree* tree = ts_parser_parse_string(
            parser,
            nullptr,
            parseView.data(),
            static_cast<uint32_t>(parseView.size()));
        TSNode root = ts_tree_root_node(tree);

        std::vector<std::string> unresolvedInternalCalls =
            CollectUndefinedInternalCalls(finalSource.c_str(), root, prepared.originalDefinedFunctionNames);

        ts_tree_delete(tree);
        ts_parser_delete(parser);

        if (!unresolvedInternalCalls.empty()) {
            prepared.diagnostics.unresolvedInternalCalls = unresolvedInternalCalls;
            throw std::runtime_error(
                "Final rewritten shader source still contains calls to removed helper functions: " +
                JoinStrings(unresolvedInternalCalls, ", "));
        }
    }

    prepared.diagnostics.finalSourceValid = true;
    return finalSource;
}

static std::string ParseSingleStringArgOrThrow(const char* src, TSNode callExprNode)
{
    TSNode argList = ts_node_child_by_field_name(
        callExprNode, "arguments",
        static_cast<uint32_t>(strlen("arguments")));

    if (ts_node_is_null(argList) || ts_node_named_child_count(argList) != 1)
        throw std::runtime_error("ResourceDescriptorIndex requires exactly one argument");

    TSNode argNode = ts_node_named_child(argList, 0);

    uint32_t start = ts_node_start_byte(argNode);
    uint32_t end = ts_node_end_byte(argNode);

    std::string raw(src + start, end - start);

    // If it's a quoted string literal, strip the quotes
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        raw = raw.substr(1, raw.size() - 2);

    return raw;
}

std::string rewriteResourceDescriptorCallsMultiRoots(
    const char* preprocessedSource,
    size_t sourceSize,
    const std::vector<std::string>& rootFunctionNames,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, sourceSize);
    if (!preprocessedSource || sourceSize == 0)
        return {};
    if (rootFunctionNames.empty())
        return std::string(preprocessedSource, sourceSize);

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser, nullptr, parseView.data(), static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    std::unordered_map<std::string, std::vector<TSNode>> functionDefs;
    BuildFunctionDefs(functionDefs, preprocessedSource, root);

    std::vector<Replacement> replacements;
    replacements.reserve(128);

    std::unordered_set<std::string> visited;
    visited.reserve(rootFunctionNames.size() * 2);

    std::vector<std::string> worklist;
    worklist.reserve(rootFunctionNames.size() * 2);

    // Seed all roots
    for (auto& r : rootFunctionNames)
    {
        if (visited.insert(r).second)
            worklist.push_back(r);
    }

    auto processBody = [&](const TSNode& bodyNode)
        {
            std::function<void(TSNode)> walk = [&](TSNode node)
                {
                    if (ts_node_is_null(node)) return;

                    if (strcmp(ts_node_type(node), "call_expression") == 0)
                    {
                        TSNode functionNode = ts_node_child_by_field_name(
                            node, "function",
                            static_cast<uint32_t>(strlen("function")));
                        if (!ts_node_is_null(functionNode))
                        {
                            uint32_t s = ts_node_start_byte(functionNode);
                            uint32_t e = ts_node_end_byte(functionNode);
                            std::string funcName(preprocessedSource + s, e - s);
                            TrimInPlace(funcName);

                            if (funcName == "ResourceDescriptorIndex" ||
                                funcName == "OptionalResourceDescriptorIndex")
                            {
                                std::string identifier = ParseSingleStringArgOrThrow(preprocessedSource, node);

                                auto it = replacementMap.find(identifier);
                                if (it == replacementMap.end())
                                {
                                    throw std::runtime_error(
                                        "ResourceDescriptorIndex identifier does not have mapped replacement: " + identifier);
                                }

                                replacements.push_back(Replacement{
                                    ts_node_start_byte(node),
                                    ts_node_end_byte(node),
                                    it->second
                                    });
                            }
                            else
                            {
                                // Normal function call: enqueue if we have a definition.
                                if (functionDefs.count(funcName) && !visited.count(funcName))
                                {
                                    visited.insert(funcName);
                                    worklist.push_back(funcName);
                                }
                            }
                        }
                    }

                    uint32_t n = ts_node_child_count(node);
                    for (uint32_t i = 0; i < n; ++i)
                        walk(ts_node_child(node, i));
                };

            walk(bodyNode);
        };

    while (!worklist.empty())
    {
        std::string fn = std::move(worklist.back());
        worklist.pop_back();

        auto it = functionDefs.find(fn);
        if (it == functionDefs.end())
            continue; // no definition in this file

        for (const TSNode& bodyNode : it->second)
            processBody(bodyNode);
    }

    // Sort replacements by startByte and sanity-check overlaps
    std::sort(replacements.begin(), replacements.end(),
        [](const Replacement& a, const Replacement& b) { return a.startByte < b.startByte; });

    for (size_t i = 1; i < replacements.size(); ++i)
    {
        if (replacements[i - 1].endByte > replacements[i].startByte)
            throw std::runtime_error("Overlapping replacements detected; rewriting would be ambiguous.");
    }

    // Apply splices
    std::string source(preprocessedSource, sourceSize);
    std::string out;
    out.reserve(source.size() + replacements.size() * 16);

    size_t cursor = 0;
    for (const auto& r : replacements)
    {
        out.append(source, cursor, r.startByte - cursor);
        out += r.replacement;
        cursor = r.endByte;
    }
    out.append(source, cursor);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return out;
}

std::string pruneUnusedCodeMultiRoots(
    const char* preprocessedSource,
    size_t sourceSize,
    const std::vector<std::string>& rootFunctionNames)
{
    sourceSize = GetNormalizedShaderSourceSize(preprocessedSource, sourceSize);
    if (!preprocessedSource || sourceSize == 0)
        return {};
    if (rootFunctionNames.empty())
        return std::string(preprocessedSource, sourceSize);

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_hlsl());

    const std::string parseView = BuildTreeSitterShaderParseView(preprocessedSource, sourceSize);
    TSTree* tree = ts_parser_parse_string(
        parser, nullptr, parseView.data(), static_cast<uint32_t>(parseView.size()));
    TSNode root = ts_tree_root_node(tree);

    // function name -> [function_definition nodes], and [body nodes]
    std::unordered_map<std::string, std::vector<TSNode>> defMap;
    std::unordered_map<std::string, std::vector<TSNode>> bodyMap;

    std::vector<TSNode> functionDefinitions;
    CollectFunctionDefinitions(root, functionDefinitions);
    for (const TSNode& node : functionDefinitions)
    {
        auto fnName = ExtractFunctionNameFromDefinition(preprocessedSource, node);
        if (!fnName.has_value()) {
            continue;
        }

        defMap[*fnName].push_back(node);

        TSNode body = ts_node_child_by_field_name(
            node, "body",
            static_cast<uint32_t>(strlen("body")));
        bodyMap[*fnName].push_back(body);
    }

    // Multi-root BFS/DFS over the call graph
    std::unordered_set<std::string> visited;
    visited.reserve(rootFunctionNames.size() * 2);

    std::vector<std::string> work;
    work.reserve(rootFunctionNames.size() * 2);

    for (auto& r : rootFunctionNames)
    {
        if (visited.insert(r).second)
            work.push_back(r);
    }

    auto enqueueCalls = [&](TSNode body)
        {
            std::function<void(TSNode)> walk = [&](TSNode n)
                {
                    if (ts_node_is_null(n)) return;

                    if (std::string(ts_node_type(n)) == "call_expression")
                    {
                        TSNode fnNode = ts_node_child_by_field_name(
                            n, "function",
                            static_cast<uint32_t>(strlen("function")));

                        if (!ts_node_is_null(fnNode))
                        {
                            uint32_t ss = ts_node_start_byte(fnNode);
                            uint32_t ee = ts_node_end_byte(fnNode);

                            std::string called(preprocessedSource + ss, ee - ss);
                            TrimInPlace(called);

                            if (bodyMap.count(called) && visited.insert(called).second)
                                work.push_back(called);
                        }
                    }

                    uint32_t c = ts_node_child_count(n);
                    for (uint32_t i = 0; i < c; ++i)
                        walk(ts_node_child(n, i));
                };

            walk(body);
        };

    while (!work.empty())
    {
        std::string fn = std::move(work.back());
        work.pop_back();

        auto it = bodyMap.find(fn);
        if (it == bodyMap.end())
            continue;

        for (TSNode body : it->second)
            enqueueCalls(body);
    }

    // Build remove ranges for anything not visited
    std::vector<Range> removeRanges;
    removeRanges.reserve(defMap.size());

    for (auto const& kv : defMap)
    {
        if (visited.count(kv.first))
            continue;

        for (TSNode defNode : kv.second)
        {
            uint32_t origStart = ts_node_start_byte(defNode);
            uint32_t origEnd = ts_node_end_byte(defNode);

			uint32_t adjStart = shrinkToIncludeDecorators(preprocessedSource, origStart); // TODO: Probably not needed with new grammar
            removeRanges.push_back({ adjStart, origEnd });
        }
    }

    if (removeRanges.empty())
    {
        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return std::string(preprocessedSource, sourceSize);
    }

    // Merge overlapping remove ranges
    (std::sort)(removeRanges.begin(), removeRanges.end(),
        [](const Range& a, const Range& b) { return a.start < b.start; });

    std::vector<Range> merged;
    merged.reserve(removeRanges.size());

    for (auto& r : removeRanges)
    {
        if (merged.empty() || r.start > merged.back().end)
        {
            merged.push_back(r);
        }
        else
        {
            merged.back().end = (std::max)(merged.back().end, r.end);
        }
    }

    // Complement => keep ranges
    std::vector<Range> keep;
    keep.reserve(merged.size() + 1);

    uint32_t lastEnd = 0;
    for (auto& r : merged)
    {
        if (lastEnd < r.start)
            keep.push_back({ lastEnd, r.start });
        lastEnd = (std::max)(lastEnd, r.end);
    }
    if (lastEnd < sourceSize)
        keep.push_back({ lastEnd, static_cast<uint32_t>(sourceSize) });

    // Splice kept ranges
    std::string out;
    out.reserve(sourceSize);
    for (auto& r : keep)
        out.append(preprocessedSource + r.start, r.end - r.start);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return out;
}

struct PreprocessedLibraryResult
{
    std::vector<ShaderEntryPointDesc> entryPoints;

    // Stable-ordered (sorted) for determinism
    std::vector<std::string> mandatoryIDs;
    std::vector<std::string> optionalIDs;

    // Maps BRSL identifier string -> replacement token (e.g. "ResourceDescriptorIndex7")
    std::unordered_map<std::string, std::string> replacementMap;

    uint64_t resourceIDsHash = 0;

    // Final transformed source (rewritten + pruned)
    std::string finalSource;

    ShaderPreprocessDiagnostics diagnostics;
};

static inline bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

uint64_t hash_list(const std::vector<std::string>& list) {
    const uint64_t FNV_offset = 146527ULL;
    const uint64_t FNV_prime = 1099511628211ULL;
    uint64_t h = FNV_offset;
    for (auto& s : list) {
        uint32_t L = uint32_t(s.size());
        // mix in the length
        for (int i = 0; i < 4; ++i) {
            h ^= (uint8_t)(L >> (i * 8));
            h *= FNV_prime;
        }
        // mix in each character
        for (unsigned char c : s) {
            h ^= c;
            h *= FNV_prime;
        }
    }
    return h;
}

PreprocessedLibraryResult PreprocessShaderLibrary(
    const DxcBuffer& preprocessedBuffer)
{
    if (!preprocessedBuffer.Ptr || GetNormalizedShaderSourceSize(static_cast<const char*>(preprocessedBuffer.Ptr), preprocessedBuffer.Size) == 0)
        throw std::runtime_error("PreprocessShaderLibrary: empty preprocessed buffer");

    // Parse library once to find [Shader("...")] entrypoints + union of BRSL ids (mandatory/optional)
    ShaderLibraryBRSLAnalysis analysis = AnalyzePreprocessedShaderLibrary(&preprocessedBuffer);

    if (analysis.entryPoints.empty()) {
        throw std::runtime_error("Shader library contains no functions decorated with [Shader(\"...\")]");
    }

    PreprocessedLibraryResult out = {};
    out.entryPoints = std::move(analysis.entryPoints);

    // Roots = function names of decorated entry points
    std::vector<std::string> roots;
    roots.reserve(out.entryPoints.size());
    for (const auto& ep : out.entryPoints) {
        roots.push_back(ep.functionName);
    }

    PreparedShaderSource prepared = PrepareShaderSourceForRoots(preprocessedBuffer, roots);
    out.diagnostics = prepared.diagnostics;
    out.mandatoryIDs = prepared.mandatoryIDs;
    out.optionalIDs = prepared.optionalIDs;

    out.replacementMap.reserve(out.mandatoryIDs.size() + out.optionalIDs.size());
    uint32_t nextIndex = 0;
    for (const auto& id : out.mandatoryIDs) {
        out.replacementMap[id] = "ResourceDescriptorIndex" + std::to_string(nextIndex++);
    }
    for (const auto& id : out.optionalIDs) {
        out.replacementMap[id] = "ResourceDescriptorIndex" + std::to_string(nextIndex++);
    }

    std::vector<std::string> combined;
    combined.reserve(out.mandatoryIDs.size() + out.optionalIDs.size());
    combined.insert(combined.end(), out.mandatoryIDs.begin(), out.mandatoryIDs.end());
    combined.insert(combined.end(), out.optionalIDs.begin(), out.optionalIDs.end());
    out.resourceIDsHash = hash_list(combined);

    out.finalSource = FinalizePreparedShaderSource(prepared, out.replacementMap);
    out.diagnostics = prepared.diagnostics;
    return out;
}
