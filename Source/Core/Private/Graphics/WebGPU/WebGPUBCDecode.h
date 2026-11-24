#pragma once

#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear::RHI::WebGPU {

	bool DecodeBlockCompressedLevel(Format format, std::span<const u8> blocks, Vec2I size, std::span<u8> outTexels);

} // namespace Shalltear::RHI::WebGPU
