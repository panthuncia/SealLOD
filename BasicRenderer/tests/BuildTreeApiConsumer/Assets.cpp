#include <BasicRenderer/Assets/ImportedAsset.h>

int main() {
    br::import::ImportedAssetPayload payload;
    return static_cast<int>(payload.parts.size());
}
