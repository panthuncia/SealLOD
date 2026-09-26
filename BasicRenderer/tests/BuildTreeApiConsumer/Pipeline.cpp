#include <BasicRenderer/Pipeline/PipelineRecipe.h>

int main() {
    auto recipe = br::pipeline::MakeBasicRendererDemoPipeline();
    (void)recipe;
    return 0;
}
