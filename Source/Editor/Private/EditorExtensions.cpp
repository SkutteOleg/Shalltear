#include "Shalltear/Editor/EditorExtensions.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module/ModuleSystem.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/AuthoringDefaults.h"
#include "Shalltear/Editor/MaterialInspectorOps.h"
#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/ComponentMenu.h"
#include "Shalltear/Editor/EditorDocuments.h"
#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorMenus.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorTasks.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/ProfilerModules.h"
#include "Shalltear/Editor/RuntimeViewToolbar.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/SceneViewEffects.h"
#include "Shalltear/Editor/SceneViewOverlays.h"
#include "Shalltear/Editor/SettingsPresentation.h"

namespace Shalltear::EditorExtensions {

	bool Load(const void* moduleBase) {
		if (!moduleBase || TypeRegistry::IsOwnerToken(moduleBase)) {
			return false;
		}
		auto entry = reinterpret_cast<EditorExtensionRegisterFn>(ModuleSystem::FindSymbol(moduleBase, REGISTER_SYMBOL));
		if (!entry) {
			return false;
		}

		entry(moduleBase);
		return true;
	}

	u32 Unload(const void* moduleBase) {
		if (!moduleBase) {
			return 0;
		}

		u32 removed = 0;
		removed += PropertyDrawers::UnregisterOwnedBy(moduleBase);
		removed += EditorPreviews::UnregisterOwnedBy(moduleBase);
		removed += EditorPreviews::AssetBadges::UnregisterOwnedBy(moduleBase);
		removed += EditorGizmos::UnregisterOwnedBy(moduleBase);
		removed += EditorIcons::ComponentIcons::UnregisterOwnedBy(moduleBase);
		removed += ComponentMenu::UnregisterOwnedBy(moduleBase);
		removed += AuthoringDefaults::UnregisterOwnedBy(moduleBase);
		removed += BuildStepRegistry::UnregisterOwnedBy(moduleBase);
		removed += BuildTargetRegistry::UnregisterOwnedBy(moduleBase);
		removed += SettingsPresentation::UnregisterOwnedBy(moduleBase);
		removed += RuntimeViewToolbar::UnregisterOwnedBy(moduleBase);
		removed += SceneViewEffects::UnregisterOwnedBy(moduleBase);
		removed += SceneAssetDrops::UnregisterOwnedBy(moduleBase);
		removed += EditorSceneOps::UnregisterCreateEntriesOwnedBy(moduleBase);
		removed += SceneViewOverlays::UnregisterOwnedBy(moduleBase);
		removed += ProfilerModules::UnregisterOwnedBy(moduleBase);
		removed += AssetOpeners::UnregisterOwnedBy(moduleBase);
		removed += MaterialInspectorOps::UnregisterShaderSourcesOwnedBy(moduleBase);
		removed += EditorDocuments::UnregisterOwnedBy(moduleBase);
		removed += ChildEnvironment::UnregisterOwnedBy(moduleBase);
		removed += EditorTasks::UnregisterOwnedBy(moduleBase);
		removed += CompileStatus::UnregisterOwnedBy(moduleBase);
		removed += EditorMenus::UnregisterOwnedBy(moduleBase);

		EditorPreviewStore::InvalidateAll();

		if (removed > 0) {
			SLT_LOG_INFO("Editor", "swept {} editor extension(s) belonging to the departing module", removed);
		}
		return removed;
	}

} // namespace Shalltear::EditorExtensions
