#include "CSharpSettings.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/SettingsPresentation.h"

namespace Shalltear::CSharpEditor::CSharpSettings {

	namespace {

		template <typename T>
		const T* FindLive(std::string_view path) {
			Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Editor)};
			Settings::GetCategories(categories);
			for (const SettingsCategoryInfo& category : categories) {
				if (category.path == path && category.type == T::GetReflectType()) {
					return static_cast<const T*>(category.live);
				}
			}
			return nullptr;
		}

		void Field(std::string_view category, std::string_view name, std::string_view label, std::string_view tooltip, std::string_view keywords,
				   const void* owner) {
			SettingsFieldPresentation field;
			field.label = str(label);
			field.tooltip = str(tooltip);
			field.keywords = str(keywords);
			SettingsPresentation::RegisterField(category, name, std::move(field), owner);
		}

	} // anonymous namespace

	void Declare() {
		Settings::DeclareCategory<CSharpProjectSettings>(ProjectCategory, SettingScope::Project);
		Settings::DeclareCategory<CSharpUserSettings>(UserCategory, SettingScope::User);
	}

	void RegisterPage(const void* owner) {
		SettingsCategoryPresentation project;
		project.label = "C#";
		project.tooltip = "How C# scripts compile in the editor/in built games.";
		project.audience = SettingsAudience::EditorAndPlayer;
		project.keywords = "csharp dotnet script unsafe globalization culture rename obfuscate linker native";
		project.order = 16;
		SettingsPresentation::RegisterCategory(ProjectCategory, std::move(project), owner);

		Field(ProjectCategory, "allowUnsafeCode", "Allow Unsafe Code", "Lets scripts use pointers/unsafe blocks.", "pointer fixed stackalloc",
			  owner);
		Field(ProjectCategory, "invariantGlobalization", "Invariant Globalization",
			  "Same text formatting/comparison on all machines (built games have no culture data).", "culture locale icu", owner);
		Field(ProjectCategory, "renameScriptNames", "Rename Script Names", "Replaces class/enum names of scripts in Release builds.",
			  "obfuscate strip names", owner);
		Field(ProjectCategory, "windowsLinker", "Windows Linker",
			  "Linker for Windows builds. \"Bundled\" needs nothing installed, \"Visual Studio\" uses installed C++ tools.", "link lld msvc", owner);

		SettingsCategoryPresentation user;
		user.label = "C#";
		user.tooltip = "Where the .NET SDK is + how scripts compile in the editor.";
		user.audience = SettingsAudience::EditorOnly;
		user.keywords = "csharp dotnet sdk path debug release optimization";
		user.order = 21;
		SettingsPresentation::RegisterCategory(UserCategory, std::move(user), owner);

		Field(UserCategory, "sdkPath", "SDK Path", "Override the discovered .NET SDK folder. Empty uses the downloaded one.", "dotnet toolchain",
			  owner);
		Field(UserCategory, "codeOptimization", "Code Optimization",
			  "\"Debug\" lets a debugger see all locals, \"Release\" runs scripts faster in the editor.", "debug release speed", owner);
		Field(UserCategory, "autoCompile", "Auto Compile", "Compiles the project's C# when a script changes. When off, compile with the Compile button (F7).",
			  "save watch", owner);
		Field(UserCategory, "aotAnalysis", "AOT Analysis",
			  "Runs the AOT/trim analyzers at each compile (slower). When off, player builds/the IDE report the same warnings.",
			  "native trim analyzer warning", owner);
	}

	const CSharpProjectSettings& GetProject() {
		static const CSharpProjectSettings defaults;
		const CSharpProjectSettings* live = FindLive<CSharpProjectSettings>(ProjectCategory);
		return live ? *live : defaults;
	}

	const CSharpUserSettings& GetUser() {
		static const CSharpUserSettings defaults;
		const CSharpUserSettings* live = FindLive<CSharpUserSettings>(UserCategory);
		return live ? *live : defaults;
	}

} // namespace Shalltear::CSharpEditor::CSharpSettings
