#pragma once

#include "Shalltear/Core/Types.h"

#include <vector>

namespace Shalltear::CSharpImpl::ScriptProfile {

	struct Row {
		str type;
		i32 hook = 0;
		u32 calls = 0;
		f64 milliseconds = 0.0;
		u64 bytes = 0;
	};

	struct Collections {
		u32 counts[3] = {};
		f64 pauseMilliseconds = 0.0;
		u64 heapBytes = 0;
	};

	void SetEnabled(bool enabled);
	bool IsEnabled();

	// costs + collections since last Read
	void Read(std::vector<Row>& rows, Collections& collections);

	const char* GetHookName(i32 hook);

	void Reset();

} // namespace Shalltear::CSharpImpl::ScriptProfile
