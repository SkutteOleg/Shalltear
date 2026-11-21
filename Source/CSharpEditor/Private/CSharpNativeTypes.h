#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

namespace Shalltear::CSharpEditor::CSharpNativeTypes {

	inline constexpr std::string_view GeneratedFolder = "Library/CSharp/Generated/Native";
	inline constexpr std::string_view GeneratedFile = "Library/CSharp/Generated/Native/NativeTypes.g.cs";

	void Collect(const void* moduleBase, Vector<const TypeInfo*>& out);

	str Render(std::span<const TypeInfo* const> types);

	bool Write(const str& projectPath, std::span<const TypeInfo* const> types);

	bool Refresh(const str& projectPath);

} // namespace Shalltear::CSharpEditor::CSharpNativeTypes
