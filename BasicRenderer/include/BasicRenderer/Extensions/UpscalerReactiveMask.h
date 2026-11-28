#pragma once

#include <memory>

namespace org { class PixelBuffer; }

namespace br::render {

// A reactive mask for the temporal upscaler, supplied by an extension that blends something over the scene colour
// which the frame's motion vectors and depth do not describe (strand hair's composite).
//
// The mask is a single-channel texture at the render resolution, 0 where the upscaler should treat the pixel as usual
// and up to 1 where it should take the current frame over its history. The extension owns the texture, makes it with a
// shader resource view, and writes all of it every frame in a pass before "UpscalingPass". The upscaling pass declares
// it and hands it on: to DLSS Super Resolution as the bias-current-colour hint, and to FSR as the reactive mask. DLSS
// Ray Reconstruction and no upscaling ignore it.
//
// Null (the default) removes it: the upscaler then gets no mask, as before this existed. One mask at a time: a second
// extension replaces the first's. Call it on the render thread, from a pass's Update; the upscaling pass declares
// again in the frame it changes.
void SetUpscalerReactiveMask(std::shared_ptr<org::PixelBuffer> mask);
std::shared_ptr<org::PixelBuffer> GetUpscalerReactiveMask();

} // namespace br::render
