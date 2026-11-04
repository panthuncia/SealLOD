// Generates the CSV, summary and Chrome reports from an async state graph trace capture.
#include "Runtime/StateGraph/RendererGraphTraceFormat.h"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    std::filesystem::path capture;
    std::filesystem::path output;
    br::render::AsyncStateGraphTraceExpandOptions options;
    bool valid = true;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--chrome") options.chromeTrace = true;
        else if (argument == "--out" && index + 1 < argc) output = argv[++index];
        else if (capture.empty() && !argument.starts_with("--")) capture = argument;
        else valid = false;
    }
    if (!valid || capture.empty()) {
        std::cerr << "Usage: GraphTraceReport <" << br::render::kGraphTraceCaptureFileName << "> [--out <directory>] [--chrome]\n"
            << "Writes the events CSV, static group CSV and summary, by default next to the capture.\n"
            << "--chrome also writes a chrome://tracing JSON timeline.\n";
        return 2;
    }
    if (output.empty()) output = capture.parent_path().empty() ? std::filesystem::path(".") : capture.parent_path();
    try {
        const auto report = br::render::ExpandAsyncStateGraphTrace(capture, output, options);
        std::cout << report.capturedEvents << " events, " << report.droppedEvents << " dropped\n"
            << report.summaryMarkdown.string() << "\n" << report.eventsCsv.string() << "\n"
            << report.staticGroupCsv.string() << "\n";
        if (options.chromeTrace) std::cout << report.chromeTraceJson.string() << "\n";
        return report.droppedEvents == 0 ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
