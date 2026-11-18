#pragma once

#include "Shalltear/Core/Types.h"

#include <vector>

namespace Shalltear::CSharpEditor::DotNetToolchain {

	struct PinnedPack {
		str name;
		str sha512;
	};

	struct Pin {
		str sdkVersion;
		str runtimeVersion;
		str targetFramework;
		str hostRid;
		str archiveURL;
		str archiveSHA512;
		std::vector<PinnedPack> packs;
	};

	const Pin& GetPin();

	enum class Tier : u8 {
		None = 0,
		Override,
		Cache,
		Checkout,
		SearchPath
	};

	struct DiscoveryOptions {
		str overrideRoot;
		str cacheRoot;
		str checkoutRoot;
		str searchPath;
	};

	struct Info {
		str root;
		Tier tier = Tier::None;
	};

	DiscoveryOptions GetDefaultDiscovery();

#ifdef SLT_CSHARP_TEST_HOOKS
	void HideSDKForTest(bool hidden);
#endif

	bool HoldsPin(const str& root, const Pin& pin);

	Info Discover(const Pin& pin, const DiscoveryOptions& options);

	str Describe(const Pin& pin, const DiscoveryOptions& options);

	str GetCacheRoot();

	str GetPackagesRoot(const str& cacheRoot, const Pin& pin);

	str GetMuxerPath(const str& sdkRoot);

	str FindSDKRoot();

	str GetRuntimeAssemblyPath();

} // namespace Shalltear::CSharpEditor::DotNetToolchain
