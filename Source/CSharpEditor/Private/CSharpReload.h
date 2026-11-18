#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor::CSharpReload {

	enum class Outcome : u8 {
		None = 0,
		Loaded,
		RolledBack,
		Failed,
	};

	void Begin(const str& assembly);

	void Shutdown();

	void Pump();

	void Request();

	void ReloadNow();

#ifdef SLT_CSHARP_TEST_HOOKS
	void SetBuildRunningForTest(bool running);
#endif

	bool IsActive();
	bool IsHeld();
	bool IsLoaded();
	Outcome GetLastOutcome();
	u32 GetReloadCount();
	u32 GetLastMigratedCount();
	f64 GetLastMilliseconds();

} // namespace Shalltear::CSharpEditor::CSharpReload
