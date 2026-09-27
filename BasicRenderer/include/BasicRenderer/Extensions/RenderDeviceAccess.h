#pragma once

#include <rhi.h>

namespace br::extensions {

rhi::Device GetRenderDevice();
rhi::CommandSignatureHandle GetRawDispatchSignature();
rhi::CommandSignatureHandle GetDispatchMeshSignature();

} // namespace br::extensions
