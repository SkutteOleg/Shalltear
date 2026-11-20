#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl::ScriptEvents {

	// must match Interop/ScriptEvents.cs
	enum class Event : i32 {
		SceneLoaded,
		SceneUnloading,
		Count,
	};

	void SetListening(i32 event, bool listening);

	void Reset();

} // namespace Shalltear::CSharpImpl::ScriptEvents
