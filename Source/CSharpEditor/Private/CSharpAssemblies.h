#pragma once

#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpEditor {

	class CSharpAssemblyDefinition final : public DataAsset {
	public:
		SLT_REFLECT_CLASS(CSharpAssemblyDefinition)
		SLT_REFLECT_PARENT(DataAsset)
		SLT_REFLECT_FIELD(assemblyName, SLT_TOOLTIP("Name of the compiled assembly (file name when empty)."))
		SLT_REFLECT_ARRAY(references, SLT_TOOLTIP("Names of assembly definitions used by this one."))
		SLT_REFLECT_FIELD(editorOnly, SLT_TOOLTIP("Left out of built games."))
		SLT_REFLECT_ARRAY(platforms, SLT_TOOLTIP("Build targets including this assembly (all when empty)."))
		SLT_REFLECT_ARRAY(defines, SLT_TOOLTIP("Symbols defined for #if in this assembly."))
		SLT_CREATE_ASSET_MENU("C# Assembly Definition", ".slassembly")
		SLT_END_REFLECT()

		str assemblyName;
		Vector<str> references{GetMemoryResource(MemTag::Asset)};
		bool editorOnly = false;
		Vector<str> platforms{GetMemoryResource(MemTag::Asset)};
		Vector<str> defines{GetMemoryResource(MemTag::Asset)};
	};

	namespace CSharpAssemblies {

		inline constexpr std::string_view Extension = ".slassembly";

		inline constexpr std::string_view NativeFolder = "Library/CSharp/Generated/Native";
		inline constexpr std::string_view NativeSuffix = ".Native";

		struct Definition {
			str name;
			str file;
			str folder;
			Vector<str> references{GetMemoryResource(MemTag::Editor)};
			Vector<str> platforms{GetMemoryResource(MemTag::Editor)};
			Vector<str> defines{GetMemoryResource(MemTag::Editor)};
			Vector<str> nestedFolders{GetMemoryResource(MemTag::Editor)};
			bool editorOnly = false;
		};

		struct Problem {
			str file;
			str message;
		};

		struct Set {
			// references before the definitions using them
			Vector<Definition> definitions{GetMemoryResource(MemTag::Editor)};
			Vector<Problem> problems{GetMemoryResource(MemTag::Editor)};
		};

		// paths in a Set relative to projectPath
		Set Scan(const str& projectPath, std::string_view defaultAssembly);

		bool BuildsFor(const Definition& definition, std::string_view targetId);

		const Definition* Find(const Set& set, std::string_view name);

	} // namespace CSharpAssemblies

} // namespace Shalltear::CSharpEditor
