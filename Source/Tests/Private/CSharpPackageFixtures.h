#pragma once

#include "Checkout.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/Types.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace Shalltear::CSharpTests {

	struct PackageFeed {
		std::filesystem::path root;
		str log;
		bool ready = false;

		std::filesystem::path Feed() const { return root / "feed"; }
		std::filesystem::path Packages() const { return root / "packages"; }

		str MakeUserProps(std::string_view packageIds) const {
			str references;
			for (size_t at = 0; at < packageIds.size();) {
				const size_t end = packageIds.find(';', at);
				const std::string_view id = packageIds.substr(at, end == std::string_view::npos ? end : end - at);
				references += "\t\t<PackageReference Include=\"" + str(id) + "\" Version=\"1.0.0\" />\n";
				at = end == std::string_view::npos ? packageIds.size() : end + 1;
			}
			return "<Project>\n\t<PropertyGroup>\n\t\t<RestoreSources>" + Feed().generic_string() + "</RestoreSources>\n\t\t<RestorePackagesPath>" +
				   Packages().generic_string() + "</RestorePackagesPath>\n\t</PropertyGroup>\n\t<ItemGroup>\n" + references + "\t</ItemGroup>\n</Project>\n";
		}
	};

	inline bool PackFixturePackage(PackageFeed& feed, std::string_view name, const std::filesystem::path& nativeLibrary = {}, std::string_view runtime = {}) {
		namespace fs = std::filesystem;
		const fs::path pinFile = Checkout::Find("Source/CSharp/DotNetPin.json");
		const fs::path fixture = Checkout::Find("Source/Tests/Fixtures/CSharp/Packages") / name;
		std::error_code ec;
		if (pinFile.empty() || !fs::is_directory(fixture, ec)) {
			feed.log += "engine checkout not found\n";
			return false;
		}
		std::ifstream stream(pinFile, std::ios::binary);
		const str pinText{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
		LinearAllocator arena(2 * pinText.size() + 1024, MemTag::Debug);
		const JSONValue pin = JSONParse(pinText, arena);
		const fs::path sdk = Checkout::Root() / "External" / "Prebuilt" / "dotnet" / str(pin["sdkVersion"].AsString());
		const fs::path dotnet = sdk / ("dotnet" SLT_TEST_EXE_SUFFIX);
		if (!fs::is_regular_file(dotnet, ec)) {
			feed.log += "pinned .NET SDK not found\n";
			return false;
		}
		const fs::path work = feed.root / "work" / name;
		fs::create_directories(feed.Feed(), ec);
		fs::create_directories(work, ec);
		fs::copy(fixture, work, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		TestPlatform::SetEnvironment("DOTNET_ROOT", sdk.generic_string());
		TestPlatform::SetEnvironment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
		TestPlatform::SetEnvironment("DOTNET_NOLOGO", "1");
		TestPlatform::SetEnvironment("DOTNET_CLI_UI_LANGUAGE", "en-us");
		str command = "\"" + dotnet.string() + "\" pack \"" + (work / (str(name) + ".csproj")).string() + "\" -c Release -o \"" + feed.Feed().string() +
					  "\" -nologo -v:q -tl:off -nodeReuse:false -p:UseSharedCompilation=false -p:ShalltearTargetFramework=" +
					  str(pin["targetFramework"].AsString()) + " \"-p:RestoreSources=" + feed.Feed().generic_string() + "\" \"-p:RestorePackagesPath=" +
					  feed.Packages().generic_string() + "\"";
		if (!nativeLibrary.empty()) {
			command += " \"-p:FixtureNativeLibrary=" + nativeLibrary.generic_string() + "\" -p:FixtureRuntime=" + str(runtime);
		}
		int exitCode = -1;
		feed.log += TestPlatform::RunCommand(command, exitCode);
		return exitCode == 0 && fs::is_regular_file(feed.Feed() / (str(name) + ".1.0.0.nupkg"), ec);
	}

} // namespace Shalltear::CSharpTests
