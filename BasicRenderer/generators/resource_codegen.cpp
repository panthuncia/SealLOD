#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

// Recursively emit nested structs and collect every type for refl-cpp
static std::string recurse_structs(
    const json& node,
    const std::string& prefix,
    const std::string& indent,
    std::vector<std::string>& types)
{
    std::string out;
    for (auto& [key, child] : node.items()) {
        std::string full = prefix + "::" + key;
        if (child.is_object() && !child.empty()) {
            // record this struct type
            types.push_back(full);

            // emit the struct declaration
            out += indent + "struct " + key + " {\n";
            // recurse into children with increased indent
            out += recurse_structs(child, full, indent + "  ", types);
            out += indent + "};\n";
        }
        else {
            // leaf: emit a static constexpr string_view member
            const std::string value = child.is_string() ? child.get<std::string>() : full;
            out += indent
                + "inline static constexpr std::string_view "
                + key
                + " = \"" + value + "\";\n";
        }
    }
    return out;
}

static std::string recurse_structs_hlsl(
    const json& node,
    const std::string& prefix)
{
    std::string out;
    for (auto& [key, child] : node.items()) {
        std::string full = prefix + "::" + key;
        out += "#define " + full +" "+full+"\n";
    }
    return out;
}


int generate_cpp(json& data, const char* out_path) {
    // will accumulate fully-qualified struct names like "Builtin" or "Builtin::Color"
    std::vector<std::string> types;

    // begin generating the header
    std::string result;
    result += "#pragma once\n\n";
    result += "#include <string_view>\n";
    result += "// GENERATED CODE - DO NOT EDIT\n\n";

    // for each top-level object in the JSON, emit a top-level struct
    for (auto& [nsName, nsObj] : data.items()) {
        // record the top-level struct
        types.push_back(nsName);

        result += "struct " + nsName + " {\n";
        // recurse to fill in nested structs and members
        result += recurse_structs(nsObj, nsName, "  ", types);
        result += "};\n\n";
    }

    // write out to the header file
    std::ofstream ofs{ out_path };
    if (!ofs) {
        std::cerr << "Error: could not open output file for writing: "
            << out_path << "\n";
        return 1;
    }
    ofs << result;
    ofs.close();

    std::cout << "Wrote generated header to: " << out_path << "\n";
    return 0;
}

int generate_hlsl(json& data, const char* out_path) {
    // will accumulate fully-qualified struct names like "Builtin" or "Builtin::Color"
    std::vector<std::string> types;

    // begin generating the header
    std::string result;
    // Genrate include guard
	result += "#ifndef BUILTIN_RESOURCES_H\n";
	result += "#define BUILTIN_RESOURCES_H\n\n";
    result += "// GENERATED CODE - DO NOT EDIT\n\n";

    // for each top-level object in the JSON, emit a top-level struct
    for (auto& [nsName, nsObj] : data.items()) {
        // record the top-level struct
        types.push_back(nsName);

        result += "struct " + nsName + " {\n";
        // recurse to fill in nested structs and members
        result.append(recurse_structs_hlsl(nsObj, nsName));
        result += "};\n\n";
    }

	// End include guard
	result += "\n#endif // BUILTIN_RESOURCES_H\n";

    // write out to the he=ader file
    std::ofstream ofs{ out_path };
    if (!ofs) {
        std::cerr << "Error: could not open output file for writing: "
            << out_path << "\n";
        return 1;
    }
    ofs << result;
    ofs.close();

    std::cout << "Wrote generated header to: " << out_path << "\n";
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: resource_codegen <data.json> <out.h> <out.hlsli>\n";
        return 1;
    }

    // parse the JSON file
    json data = json::parse(std::ifstream{ argv[1] });

	generate_cpp(data, argv[2]);

	//generate_hlsl(data, argv[3]);

    return 0;
}
