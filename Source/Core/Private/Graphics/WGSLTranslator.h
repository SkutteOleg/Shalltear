#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear {

	class WGSLTranslator {
	public:
		static bool IsAvailable();
		static str GetToolPath();
		static u64 GetTranslatorStamp();

		static bool TranslateSpirvToWGSL(std::span<const u8> spirv, Vector<u8>& outWGSL, str& outError);
	};

} // namespace Shalltear
