#pragma once

#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor {

	enum class WindowsLinker : u8 {
		Bundled = 0,
		VisualStudio = 1,
	};

	enum class ScriptOptimization : u8 {
		Debug = 0,
		Release = 1,
	};

} // namespace Shalltear::CSharpEditor

SLT_REFLECT_ENUM(::Shalltear::CSharpEditor::WindowsLinker)
SLT_ENUM_VALUE(Bundled)
SLT_ENUM_VALUE(VisualStudio)
SLT_END_ENUM()

SLT_REFLECT_ENUM(::Shalltear::CSharpEditor::ScriptOptimization)
SLT_ENUM_VALUE(Debug)
SLT_ENUM_VALUE(Release)
SLT_END_ENUM()

namespace Shalltear::CSharpEditor {

	struct CSharpProjectSettings {
		SLT_REFLECT_VALUE(CSharpProjectSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(allowUnsafeCode)
		SLT_REFLECT_FIELD(invariantGlobalization)
		SLT_REFLECT_FIELD(renameScriptNames)
		SLT_REFLECT_FIELD(windowsLinker)
		SLT_END_REFLECT()

		bool allowUnsafeCode = false;
		bool invariantGlobalization = true;
		bool renameScriptNames = true;
		WindowsLinker windowsLinker = WindowsLinker::Bundled;
	};

	struct CSharpUserSettings {
		SLT_REFLECT_VALUE(CSharpUserSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(sdkPath, SLT_MUTABILITY(::Shalltear::MutabilityClass::Restart))
		SLT_REFLECT_FIELD(codeOptimization)
		SLT_END_REFLECT()

		str sdkPath;
		ScriptOptimization codeOptimization = ScriptOptimization::Debug;
	};

	namespace CSharpSettings {

		inline constexpr std::string_view ProjectCategory = "C#";
		inline constexpr std::string_view UserCategory = "Editor/C#";

		void Declare();

		void RegisterPage(const void* owner);

		const CSharpProjectSettings& GetProject();

		const CSharpUserSettings& GetUser();

	} // namespace CSharpSettings

} // namespace Shalltear::CSharpEditor
