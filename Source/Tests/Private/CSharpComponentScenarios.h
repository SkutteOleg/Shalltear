#pragma once

#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <vector>

namespace Shalltear::CSharpScenarios {

	const TypeInfo* GetTwinType();
	std::vector<str> TakeTwinRecords();

	void RunFrames(u32 count);
	void RunLifecycle(const TypeInfo* type);

	str Join(const std::vector<str>& records);

} // namespace Shalltear::CSharpScenarios
