#pragma once

namespace Shalltear::CSharpEditor {

	bool RegisterProfilerModule(const void* owner);
	void ReleaseProfilerModule();

	void PumpProfilerModule();

} // namespace Shalltear::CSharpEditor
