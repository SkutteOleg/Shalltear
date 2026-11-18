#pragma once

#include "ScriptBridge.h"
#include "ScriptRuntime.h"

#include <new>
#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl {

	inline std::string_view View(SltString text) noexcept {
		return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
	}

	// C# buffers 4 byte aligned, Vec4/Quat need 16
	inline SltVec4 FromEngine(const Vec4& value) noexcept {
		return SltVec4{value.x, value.y, value.z, value.w};
	}
	inline SltQuat FromEngine(const Quat& value) noexcept {
		return SltQuat{value.x, value.y, value.z, value.w};
	}

} // namespace Shalltear::CSharpImpl
