#include "TestsPCH.h"

#ifdef SLT_TEST_CSHARP_AOT

#include "CSharpConformance.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "NativeAOTHost.h"
#include "ScriptRuntime.h"

#include "Platform/EditorPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"

#include <cstdio>
#include <filesystem>
#include <map>

using namespace Shalltear;
using Shalltear::CSharpConformance::View;

namespace {

	std::filesystem::path AOTDirectory() {
		return std::filesystem::path(FileSystem::GetExecutablePath().ToString()).parent_path() / "CSharp" / "aot";
	}

	struct HostRun {
		int exitCode = -1;
		str output;
		std::map<str, std::pair<int, str>> cases;

		bool Has(std::string_view text) const { return output.find(text) != str::npos; }
	};

	HostRun RunHost(const std::filesystem::path& library) {
		HostRun run;
		const str commandLine = "\"" + FileSystem::GetExecutablePath().ToString() + "\" " + TestHelpers::CSHARP_AOT_HOST_FLAG + " \"" +
								library.generic_string() + "\"";
		run.output = TestPlatform::RunCommand(commandLine, run.exitCode);
		size_t start = 0;
		while (start < run.output.size()) {
			size_t end = run.output.find('\n', start);
			if (end == str::npos) {
				end = run.output.size();
			}
			const std::string_view line(run.output.data() + start, end - start);
			int result = 0;
			char name[160] = {};
			if (std::sscanf(str(line).c_str(), "CASE %d %159s", &result, name) == 2) {
				const size_t bar = line.find(" | ");
				str message = bar == std::string_view::npos ? str() : str(line.substr(bar + 3));
				while (!message.empty() && (message.back() == '\r' || message.back() == ' ')) {
					message.pop_back();
				}
				run.cases[name] = {result, message};
			}
			start = end + 1;
		}
		return run;
	}

} // namespace

int Shalltear::TestHelpers::RunCSharpAOTHost(const char* library) {
	JobSystem::Initialize();
	LogManager::Initialize({true, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	str error;
	if (!library || !CSharpImpl::NativeAOTHost::Start(library, error)) {
		std::printf("START FAILED %s\n", error.c_str());
		LogManager::Terminate();
		JobSystem::Terminate();
		return 2;
	}
	const SltManagedAPI& managed = CSharpImpl::ScriptRuntime::GetManaged();
	std::printf("ENTRIES load=%d unload=%d\n", managed.loadUserAssemblies != nullptr, managed.unloadUserAssemblies != nullptr);
	const int32_t count = managed.getConformanceTestCount();
	const Time::FrameScope frame(CSharpConformance::MakeFrame());
	for (int32_t index = 0; index < count; ++index) {
		SltString name{};
		managed.getConformanceTestName(index, &name);
		const str caseName(View(name));
		SltString message{};
		const int32_t result = managed.runConformanceTest(index, &message);
		std::printf("CASE %d %s | %.*s\n", result, caseName.c_str(), static_cast<int>(View(message).size()), View(message).data());
		std::fflush(stdout);
	}
	CSharpImpl::ScriptRuntime::Detach();
	LogManager::Terminate();
	JobSystem::Terminate();
	std::printf("DONE %d\n", count);
	return 0;
}

TEST(CSharpNativeAOTTest, ConformanceSuitePasses) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;
	EXPECT_TRUE(run.Has("ENTRIES load=0 unload=0")) << run.output;

	int ran = 0;
	for (const auto& [name, outcome] : run.cases) {
		if (CSharpConformance::IsProbe(name)) {
			continue;
		}
		EXPECT_EQ(outcome.first, SLT_OK) << name << ": " << outcome.second;
		++ran;
	}
	EXPECT_GE(ran, 29) << run.output;
	EXPECT_TRUE(run.Has(std::format("DONE {}", run.cases.size()))) << run.output;
}

TEST(CSharpNativeAOTTest, FailuresAndExceptionsAreReported) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;

	ASSERT_TRUE(run.cases.contains("Probe.Fails")) << run.output;
	EXPECT_EQ(run.cases.at("Probe.Fails").first, 1);
	EXPECT_EQ(run.cases.at("Probe.Fails").second, "failed as asked");

	ASSERT_TRUE(run.cases.contains("Probe.ThrowsOnCallingThread")) << run.output;
	EXPECT_EQ(run.cases.at("Probe.ThrowsOnCallingThread").first, -1);
	EXPECT_TRUE(run.Has("System.InvalidOperationException: thrown on the calling thread")) << run.output;

	ASSERT_TRUE(run.cases.contains("Probe.ThrowsOnStartedThread")) << run.output;
	EXPECT_EQ(run.cases.at("Probe.ThrowsOnStartedThread").first, SLT_OK);
	EXPECT_TRUE(run.Has("System.InvalidOperationException: thrown on a started thread")) << run.output;
}

TEST(CSharpNativeAOTTest, LogReachesEngineLog) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;

	ASSERT_TRUE(run.cases.contains("LogProbe.WritesEachLevel")) << run.output;
	EXPECT_EQ(run.cases.at("LogProbe.WritesEachLevel").first, SLT_OK);
	EXPECT_TRUE(run.Has("[Conformance] [INFO] [:0]: info from C#")) << run.output;
	EXPECT_TRUE(run.Has("[Conformance] [WARN] [:0]: warn from C#")) << run.output;
	EXPECT_TRUE(run.Has("[Conformance] [ERROR] [:0]: error from C#")) << run.output;
#ifdef NDEBUG
	const bool debugRows = false;
#else
	const bool debugRows = true;
#endif
	EXPECT_EQ(run.Has("[Conformance] [TRACE] [:0]: trace from C#"), debugRows) << run.output;
	EXPECT_EQ(run.Has("[Conformance] [DEBUG] [:0]: debug from C#"), debugRows) << run.output;

	ASSERT_TRUE(run.cases.contains("LogProbe.WritesOddText")) << run.output;
	EXPECT_EQ(run.cases.at("LogProbe.WritesOddText").first, SLT_OK);
	EXPECT_TRUE(run.Has("xxxx caf\xC3\xA9 end")) << run.output;
	EXPECT_TRUE(run.Has("[Conformance] [INFO] [:0]: info from a started thread")) << run.output;
}

TEST(CSharpNativeAOTTest, ReleaseLibraryDropsThreadChecks) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;

	ASSERT_TRUE(run.cases.contains("FeatureProbe.ReportsThreadChecks")) << run.output;
#ifdef NDEBUG
	EXPECT_EQ(run.cases.at("FeatureProbe.ReportsThreadChecks").second, "thread checks off");
#else
	EXPECT_EQ(run.cases.at("FeatureProbe.ReportsThreadChecks").second, "thread checks on");
#endif
}

TEST(CSharpNativeAOTTest, MissingLibraryNamesTheFile) {
	const HostRun run = RunHost(AOTDirectory() / "NoSuchScripts.dll");
	EXPECT_EQ(run.exitCode, 2) << run.output;
	EXPECT_TRUE(run.Has("START FAILED")) << run.output;
	EXPECT_TRUE(run.Has("NoSuchScripts.dll didn't load")) << run.output;
}

TEST(CSharpWindowsLinkTest, ImportsOnlySystemLibraries) {
	const Path library((AOTDirectory() / "Shalltear.ConformanceAOT.dll").generic_string());
	Vector<str> imported;
	ASSERT_TRUE(EditorPlatform::ReadImportedLibraries(library, imported));
	ASSERT_FALSE(imported.empty());
	for (const str& name : imported) {
		str lowered = name;
		for (char& character : lowered) {
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		const bool system = lowered == "kernel32.dll" || lowered == "advapi32.dll" || lowered == "bcrypt.dll" || lowered == "ole32.dll" ||
							lowered.starts_with("api-ms-win-crt-");
		EXPECT_TRUE(system) << name;
	}
}

TEST(CSharpWindowsLinkTest, LinkNamesNoVisualStudioFile) {
	const str inputs = FileSystem::ReadTextFile(Path((AOTDirectory() / "Shalltear.ConformanceAOT.rsp").generic_string()));
	ASSERT_FALSE(inputs.empty());
	EXPECT_EQ(inputs.find("Microsoft Visual Studio"), str::npos);
	EXPECT_EQ(inputs.find("Windows Kits"), str::npos);
	EXPECT_NE(inputs.find("CRTShim.obj"), str::npos);

	const std::filesystem::path libraries = AOTDirectory().parent_path() / "native" / "libs";
	u32 bare = 0;
	u32 searchPaths = 0;
	bool withoutDefaults = false;
	size_t start = 0;
	while (start < inputs.size()) {
		size_t end = inputs.find_first_of("\r\n", start);
		if (end == str::npos) {
			end = inputs.size();
		}
		str token = inputs.substr(start, end - start);
		start = end + 1;
		withoutDefaults = withoutDefaults || token == "/NODEFAULTLIB";
		if (token.starts_with("/LIBPATH:")) {
			++searchPaths;
			continue;
		}
		if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
			token = token.substr(1, token.size() - 2);
		}
		if (!token.ends_with(".lib") || token.find_first_of("/\\") != str::npos) {
			continue;
		}
		++bare;
		EXPECT_TRUE(std::filesystem::exists(libraries / token)) << token;
	}
	EXPECT_GE(bare, 17u);
	EXPECT_EQ(searchPaths, 1u);
	EXPECT_TRUE(withoutDefaults);
}

#endif
