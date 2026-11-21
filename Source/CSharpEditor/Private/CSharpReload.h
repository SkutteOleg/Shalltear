#pragma once

#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear::CSharpEditor::CSharpReload {

	enum class Outcome : u8 {
		None = 0,
		Loaded,
		RolledBack,
		Failed,
	};

	void Begin(const str& assembly);

	// paths, references before the assemblies using them
	void SetAssemblies(std::span<const str> assemblies);

	void Shutdown();

	void Pump();

	void Request();

	void ReloadNow();

	void SetBuildStarting(bool starting);

#ifdef SLT_CSHARP_TEST_HOOKS
	void SetBuildRunningForTest(bool running);
#endif

	bool IsActive();
	bool IsHeld();
	bool IsPending();
	bool IsPlaying();
	bool IsLoaded();
	bool IsBehind();
	Outcome GetLastOutcome();
	u32 GetReloadCount();
	u32 GetLastMigratedCount();
	f64 GetLastMilliseconds();

} // namespace Shalltear::CSharpEditor::CSharpReload
