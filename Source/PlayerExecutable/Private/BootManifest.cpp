#include "BootManifest.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Module/ModuleSystem.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include <format>

namespace Shalltear {

	bool MountStagedContent(const str& executableRoot, str& outError) {
		Path packagePath(executableRoot + "/Data/Content.slpak");

		return AssetDatabase::MountPackage(packagePath, outError);
	}

	void UsePackagedProjectSettings(SettingsPaths& paths) {
		paths.project = {};
		AssetDatabase::ReadPackageDocument(PakDocumentKind::ProjectSettings, paths.projectText);
	}

	void RegisterGameCoreExtensions() {
		void* image = ModuleSystem::GetExecutableImage();
		auto entry = reinterpret_cast<CoreExtensionRegisterFn>(
			ModuleSystem::FindSymbol(image, SLT_CORE_EXTENSION_REGISTER_SYMBOL));
		if (entry) {
			entry(image);
		}
		ModuleEvents::ExtensionsRegistered.Publish(EventBus::GetGlobal(), {});
	}

	bool LoadBootScene(const BootScene& scene, str& outWhat) {
		outWhat = scene.path.empty() ? scene.guid.ToString() : scene.path;
		return SceneSystem::LoadSceneByGuid(scene.guid).IsValid();
	}

	bool ReadBootManifest(BootManifest& out, str& outError) {
		constexpr std::string_view What = "the package's boot manifest";

		str text;
		if (!AssetDatabase::ReadPackageDocument(PakDocumentKind::BootManifest, text)) {
			outError = std::format("{} is missing (build was cooked incompletely)", What);
			return false;
		}

		LinearAllocator arena(text.size() + 64, MemTag::Core);
		JSONValue root = JSONParse(text, arena);
		if (!root.IsObject()) {
			outError = std::format("{} isn't valid JSON", What);
			return false;
		}

		const JSONValue& version = root["formatVersion"];
		if (!version.IsNumber()) {
			outError = std::format("{} has no formatVersion (not a Shalltear boot manifest)", What);
			return false;
		}
		if (version.AsInt() != BOOT_MANIFEST_FORMAT_VERSION) {
			outError = std::format("{} is format {} (this player reads format {})", What, version.AsInt(),
								   BOOT_MANIFEST_FORMAT_VERSION);
			return false;
		}

		BootManifest parsed;
		parsed.productName = str(root["productName"].AsString());

		const JSONValue& scenes = root["scenes"];
		if (!scenes.IsNull() && !scenes.IsArray()) {
			outError = std::format("{}: \"scenes\" is present but isn't an array", What);
			return false;
		}
		for (size_t i = 0; i < scenes.Size(); ++i) {
			const JSONValue& entry = scenes[i];

			BootScene scene;
			if (!entry.IsObject()) {
				outError = std::format("{}: scene {} isn't an object (expected {{ \"path\", \"guid\" }})",
									   What, i);
				return false;
			}
			scene.path = str(entry["path"].AsString());
			scene.guid = Guid::FromString(entry["guid"].AsString());
			if (!scene.guid.IsValid()) {
				outError = std::format("{}: scene {} ('{}') has no guid",
									   What, i, scene.path);
				return false;
			}
			parsed.scenes.push_back(std::move(scene));
		}

		out = std::move(parsed);
		return true;
	}

} // namespace Shalltear
