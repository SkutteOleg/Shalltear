#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear {

	inline constexpr i64 BOOT_MANIFEST_FORMAT_VERSION = 2;

	struct BootScene {
		str path;
		Guid guid{};
	};

	struct BootManifest {
		str productName;

		Vector<BootScene> scenes{GetMemoryResource(MemTag::Core)};
	};

	bool MountStagedContent(const str& executableRoot, str& outError);

	void UsePackagedProjectSettings(SettingsPaths& paths);

	void RegisterGameCoreExtensions();

	bool LoadBootScene(const BootScene& scene, str& outWhat);

	bool ReadBootManifest(BootManifest& out, str& outError);

	int PrintThirdPartyNotices(const str& executableRoot);

	void UnpackThirdPartyNotices(const str& executableRoot);

} // namespace Shalltear
