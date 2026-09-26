#include <BasicRenderer/Diagnostics/PipelineControl.h>

int main() {
    return br::diagnostics::GetPipelineEpoch() == 0;
}
