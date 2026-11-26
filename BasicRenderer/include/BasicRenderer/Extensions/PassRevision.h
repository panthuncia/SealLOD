#pragma once

// Helpers for a typed pass's Revision (org::TypedRenderGraphPass): what its Prepare and Record depend on beyond its bindings.
// While a pass's revision stays the same, a kept recording of its epoch may be submitted again instead of recording anew
// (GraphHost::SetKeptEpochs). So a revision holds every value Record bakes into the list that can change without a rebuild:
// settings read in Prepare, sizes and counts known on the CPU, the pipeline's build. Values that change every frame do not
// belong in it: they reach the shaders through GPU memory (the per-frame buffer) or the pass is recorded at execution.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <BasicRenderer/Pipeline/PipelineDesc.h>

namespace br::render {

// The pipeline's build: an asynchronous build landing or a shader reload replaces the payload in place, and a kept list
// still binds the payload it was recorded with.
inline void AppendPipelineRevision(std::vector<uint64_t>& out, const br::PipelineState& pipeline) {
    out.push_back(reinterpret_cast<uintptr_t>(pipeline.PeekPayload()));
    out.push_back(pipeline.GetGeneration());
}

// A plain value of up to eight bytes: integers, enums, floats and pointers.
template<class T>
    requires (std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(uint64_t))
inline void AppendRevision(std::vector<uint64_t>& out, const T& value) {
    uint64_t word = 0;
    std::memcpy(&word, &value, sizeof(T));
    out.push_back(word);
}

// Rows of a plain type (a table a pass publishes, a list of dispatches) as one word: a hash of their bytes and their count.
// The rows must have no padding whose bytes could differ (value-initialized rows of 4-byte fields are fine).
template<class T>
    requires std::is_trivially_copyable_v<T>
inline void AppendRevisionRows(std::vector<uint64_t>& out, const T* rows, size_t count) {
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(rows);
    for (size_t i = 0, n = count * sizeof(T); i < n; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    out.push_back(hash);
    out.push_back(count);
}

// The pipelines a pass declared, kept for its Revision: a pass's bindings hold its pipelines, but Revision sees only the
// pass. Declare clears it and adds each pipeline as it declares it.
class DeclaredPipelines {
public:
    void Clear() { m_pipelines.clear(); }
    br::PipelineState Add(br::PipelineState pipeline) {
        m_pipelines.push_back(pipeline);
        return pipeline;
    }
    void Append(std::vector<uint64_t>& out) const {
        for (const auto& pipeline : m_pipelines) AppendPipelineRevision(out, pipeline);
    }
private:
    std::vector<br::PipelineState> m_pipelines;
};

} // namespace br::render
