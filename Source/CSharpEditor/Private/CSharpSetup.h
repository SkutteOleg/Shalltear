#pragma once

#include "DotNetAcquire.h"
#include "DotNetToolchain.h"

#include "Shalltear/Core/Types.h"

#include <functional>

namespace Shalltear::CSharpEditor::CSharpSetup {

	enum class State : u8 {
		Idle = 0,
		Acquiring,
		Ready,
		Failed,
		Cancelled
	};

	struct Options {
		DotNetToolchain::DiscoveryOptions discovery;
		AcquireOptions acquire;
		str projectPath;
		std::function<bool(const str& sdkRoot, str& outError)> startRuntime;
		bool authoring = false;
		bool buildServers = true;
	};

	Options GetDefaultOptions();

	void Begin(Options options, const void* owner);

	void Pump();

	void Shutdown();

	State GetState();

	const str& GetSDKRoot();

} // namespace Shalltear::CSharpEditor::CSharpSetup
