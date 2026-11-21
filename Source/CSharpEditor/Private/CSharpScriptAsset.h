#pragma once

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor {

	class CSharpScriptAsset final : public Asset {
	public:
		SLT_REFLECT_CLASS(CSharpScriptAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_END_REFLECT()

		bool Import(const Path& sourcePath) override;

		StringHash GetAssetTypeId() const override { return CSharpScriptAsset::GetReflectType()->name; }
	};

	namespace CSharpScripts {

		enum class Template : u8 {
			Component = 0,
			Class,
		};

		void RegisterImporter(const void* owner);

		void GetComponents(const Guid& script, Vector<const TypeInfo*>& out);

		const TypeInfo* ResolveComponent(const Guid& script);

		bool IsValidName(std::string_view name);

		Guid Create(const Path& virtualDir, const str& name, Template kind);

		bool Open(const Guid& script);

		void HandleRenamed(const Path& oldPhysicalPath, const Path& newPhysicalPath);

	} // namespace CSharpScripts

} // namespace Shalltear::CSharpEditor
