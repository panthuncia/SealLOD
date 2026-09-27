#pragma once

#include <cstdint>

enum class TextureUploadAdvanceMode : uint8_t {
    AllowBlockingFallback = 0,
    NonBlocking,
};

struct TextureUploadAdvanceResult {
    bool hasUsableImage = false;
    bool hasPendingWork = false;
    bool bindingChanged = false;
    bool didMainThreadUpload = false;
};
