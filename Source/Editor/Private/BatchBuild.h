#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	struct BatchBuildOptions {
		bool requested = false;
		bool listProfiles = false;
		bool buildAll = false;
		str projectPath;
		str outputOverride;
		Vector<str> profiles;
	};

	BatchBuildOptions ParseBatchBuildOptions(int argc, char* argv[]);

	int RunBatchBuild(const BatchBuildOptions& options);

	const void* GetBatchModuleBase();

} // namespace Shalltear
