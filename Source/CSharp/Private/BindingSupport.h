#pragma once

#include "ManagedTypes.h"
#include "ScriptBridge.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Object.h"

#include <bit>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>

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

	template <typename T>
	T* ResolveObject(uint64_t id) noexcept {
		return static_cast<T*>(ObjectRegistry::Resolve(id));
	}

	template <typename T>
	uint64_t IdOf(const Handle<T>& handle) noexcept {
		return handle.IsValid() ? handle.GetInstanceId() : 0;
	}
	inline uint64_t IdOf(const Object* object) noexcept {
		return object ? object->GetInstanceId() : 0;
	}

} // namespace Shalltear::CSharpImpl
