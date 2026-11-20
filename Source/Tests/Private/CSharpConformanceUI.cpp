#include "TestsPCH.h"

#include "CSharpConformance.h"
#include "DeviceLossHarness.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/UI/UISystem.h"

namespace Shalltear::CSharpConformance {

	UIRig::UIRig() {
		const std::filesystem::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
		if (!engineAssets.empty() && FileSystem::Resolve(Path("engine://Fonts")).ToString().find("://") != str::npos) {
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			_mounted = true;
		}
		RenderSystem::Initialize();
		UISystem::Initialize();
		UIScreenDesc screen;
		screen.target = UIScreenDesc::Target::Texture;
		screen.sizePx = {640, 360};
		UISystem::SetScreen(screen);
		_ready = UISystem::IsInitialized();
	}

	UIRig::~UIRig() {
		UISystem::Terminate();
		RenderSystem::Terminate();
		if (_mounted) {
			FileSystem::Unmount("engine");
		}
	}

} // namespace Shalltear::CSharpConformance
