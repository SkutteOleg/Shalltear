#pragma once

#include "CSharpCompile.h"

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor::CSharpAuthoring {

	struct Options {
		str projectPath;
		str projectFile;
		str sdkRoot;
		bool buildServers = true;
		bool watchSources = true;
		f64 debounceSeconds = 0.25;
	};

	void Begin(const Options& options, const void* owner);

	void Pump();

	void Shutdown();

	bool IsActive();

	const CSharpCompile& GetCompile();

	void RequestCompile();

	void NotifySourceChanged();

} // namespace Shalltear::CSharpEditor::CSharpAuthoring
