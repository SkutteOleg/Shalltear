#pragma once

namespace Shalltear::TestHelpers {

	inline constexpr const char* WINDOW_HOST_FLAG = "--shalltear-window-host";
	inline constexpr const char* WINDOW_HOST_TITLE = "ShalltearForeignWindowProbe";

	int RunWindowHost();

	inline constexpr const char* SHOW_STATE_FLAG = "--shalltear-show-state";

	enum ShowStateResult : int {
		ShowStateOk = 0,
		ShowStateNoSubstitution = 10,
		ShowStateCreateFailed = 11,
		ShowStateCreateNotVisible = 12,
		ShowStateShowDidNotShow = 13,
		ShowStateHiddenWasShown = 14,
		ShowStateNotMaximized = 15,
	};

	int RunShowStateProbe();

	inline constexpr const char* DATA_ASSET_RELOAD_FLAG = "--shalltear-data-asset-reload";

	enum DataAssetReloadResult : int {
		DataAssetSurvived = 0,

		DataAssetSweepAborted = 30,
		DataAssetVtableDangled = 31,
		DataAssetVtableAliased = 34,

		DataAssetHandleLost = 32,
		DataAssetWrongType = 33,
		DataAssetIdChanged = 35,
		DataAssetValueLost = 36,
		DataAssetNewCodeAbsent = 37,

		DataAssetModuleLoadFailed = 40,
		DataAssetTypeMissing = 41,
		DataAssetCreateFailed = 42,
		DataAssetNotModuleOwned = 43,
		DataAssetReloadRefused = 44,
		DataAssetNoProjectRoot = 45,
		DataAssetAbortedInSetup = 46,
		DataAssetNotTheOnlyOne = 47,
	};

	int RunDataAssetReloadProbe(const char* projectRoot);

	inline constexpr const char* REFLECTION_STARTUP_FLAG = "--shalltear-reflection-startup";

	enum ReflectionStartupResult : int {
		ReflectionStartupOk = 0,
		ReflectionStartupFirstMissing = 60,
		ReflectionStartupNoPrimitives = 59,
	};

	int RunReflectionStartupProbe();

	inline constexpr const char* CSHARP_AOT_HOST_FLAG = "--shalltear-csharp-aot-host";

	int RunCSharpAOTHost(const char* library);

	inline constexpr const char* CSHARP_AOT_COMPONENTS_FLAG = "--shalltear-csharp-aot-components";

	int RunCSharpAOTComponents(const char* library);

	inline constexpr const char* CSHARP_AOT_FIELDS_FLAG = "--shalltear-csharp-aot-fields";

	int RunCSharpAOTFields(const char* library);

	void RestoreNodeShader();

} // namespace Shalltear::TestHelpers
