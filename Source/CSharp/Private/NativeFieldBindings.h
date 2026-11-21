#pragma once

#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::NativeFieldBindings {

	// slots must match ScriptFunctionDetail::Slot (enum as i64, object as id)
	void Read(Component& target, u64 type, u64 field, i32 kind, std::span<u64> slots);
	void Write(Component& target, u64 type, u64 field, i32 kind, std::span<const u64> slots);

	std::string_view ReadText(Component& target, u64 type, u64 field);
	void WriteText(Component& target, u64 type, u64 field, std::string_view value);

	void Invoke(Component& target, u64 type, u64 method);

	void Reset();

} // namespace Shalltear::CSharpImpl::NativeFieldBindings
