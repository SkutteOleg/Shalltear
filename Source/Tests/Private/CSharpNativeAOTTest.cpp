#include "TestsPCH.h"

#ifdef SLT_TEST_CSHARP_AOT

#include "CSharpComponentScenarios.h"
#include "CSharpConformance.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "BindingProbe.h"
#include "ManagedTypes.h"
#include "NativeAOTHost.h"
#include "ScriptRuntime.h"

#include "Platform/EditorPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>

using namespace Shalltear;
using Shalltear::CSharpConformance::View;

SLT_DECLARE_LINKED_PACKAGE(CSharp)

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

	HostRun RunHost(const std::filesystem::path& library, const char* flag = TestHelpers::CSHARP_AOT_HOST_FLAG) {
		HostRun run;
		const str commandLine = "\"" + FileSystem::GetExecutablePath().ToString() + "\" " + flag + " \"" + library.generic_string() + "\"";
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

int Shalltear::TestHelpers::RunCSharpAOTComponents(const char* library) {
	JobSystem::Initialize();
	LogManager::Initialize({true, false});
	TypeRegistry::RegisterPendingTypes();
	static char packages = 0;
	CSharpConformance::StandUpPackages(&packages);
	SceneSystem::Initialize();
	str error;
	if (!library || !CSharpImpl::NativeAOTHost::Start(library, error)) {
		std::printf("START FAILED %s\n", error.c_str());
		SceneSystem::Terminate();
		LogManager::Terminate();
		JobSystem::Terminate();
		return 2;
	}
	const auto script = [](const char* name) { return TypeRegistry::FindType(StringHash(str("Shalltear.Conformance.") + name)); };

	CSharpScenarios::RunLifecycle(CSharpScenarios::GetTwinType());
	std::printf("TWIN %s\n", CSharpScenarios::Join(CSharpScenarios::TakeTwinRecords()).c_str());
	CSharpImpl::BindingProbe::TakeRecords();
	CSharpScenarios::RunLifecycle(script("HookRecorder"));
	std::printf("SCRIPT %s\n", CSharpScenarios::Join(CSharpImpl::BindingProbe::TakeRecords()).c_str());

	Handle<Scene> world = SceneSystem::CreateScene("World");
	const u64 before = CSharpImpl::ManagedTypes::GetCounters().hookCrossings;
	Handle<Component> only = SceneSystem::CreateObject("Only", world)->AddComponent(script("UpdateOnly"));
	Handle<Component> silent = SceneSystem::CreateObject("Silent", world)->AddComponent(script("Silent"));
	CSharpScenarios::RunFrames(3);
	std::printf("CROSSINGS %d %d %llu\n", only.IsValid(), silent.IsValid(),
				static_cast<unsigned long long>(CSharpImpl::ManagedTypes::GetCounters().hookCrossings - before));

	for (const char* name : {"HookRecorder", "UpdateOnly", "Silent", "LateReporter", "HolderOfInt"}) {
		Handle<Component> component = SceneSystem::CreateObject(name, world)->AddComponent(script(name));
		std::printf("TICKS %s %d\n", name, component.IsValid() ? static_cast<int>(static_cast<Behavior*>(component.Get())->GetTickMask()) : -1);
	}
	const TypeInfo* parent = script("EnableCounter");
	const TypeInfo* child = script("LateReporter");
	std::printf("PARENT %d %d\n", parent && child && child->parent == parent, parent && (parent->flags & TypeFlagAbstract) != 0);

	CSharpImpl::BindingProbe::TakeRecords();
	Handle<Component> thrower = SceneSystem::CreateObject("Thrower", world)->AddComponent(script("ThrowsInConstructor"));
	CSharpScenarios::RunFrames(1);
	std::printf("INERT %d\n", thrower.IsValid());
	std::fflush(stdout);

	SceneSystem::Terminate();
	CSharpImpl::ScriptRuntime::Detach();
	LogManager::Terminate();
	JobSystem::Terminate();
	std::printf("DONE\n");
	return 0;
}

int Shalltear::TestHelpers::RunCSharpAOTAudio(const char* library) {
	JobSystem::Initialize();
	LogManager::Initialize({true, false});
	TypeRegistry::InitializePrimitives();
	TypeRegistry::RegisterPendingTypes();
	int32_t count = -1;
	{
		const CSharpConformance::AudioStore store;
		static char packages = 0;
		CSharpConformance::StandUpAudio(&packages);
		str error;
		if (library && CSharpImpl::NativeAOTHost::Start(library, error)) {
			SceneSystem::Initialize();
			{
				const CSharpConformance::AudioRig rig(&CSharpImpl::BindingProbe::SetDriver, &CSharpImpl::BindingProbe::SetAssetFinder);
				std::printf("ASSETS staged=%d\n", rig.IsStaged());
				SceneSystem::CreateScene("World");
				const SltManagedAPI& managed = CSharpImpl::ScriptRuntime::GetManaged();
				count = managed.getConformanceTestCount();
				for (int32_t index = 0; index < count; ++index) {
					SltString name{};
					managed.getConformanceTestName(index, &name);
					const str caseName(View(name));
					SltString message{};
					const int32_t result = managed.runConformanceTest(index, &message);
					std::printf("CASE %d %s | %.*s\n", result, caseName.c_str(), static_cast<int>(View(message).size()), View(message).data());
					std::fflush(stdout);
				}
				SceneSystem::Terminate();
			}
			CSharpImpl::ScriptRuntime::Detach();
		} else {
			std::printf("START FAILED %s\n", error.c_str());
		}
		CSharpConformance::ShutDownAudio();
		FrameStages::UnregisterOwnedBy(&packages);
	}
	LogManager::Terminate();
	JobSystem::Terminate();
	std::printf("DONE %d\n", count);
	return count < 0 ? 2 : 0;
}

int Shalltear::TestHelpers::RunCSharpAOTHost(const char* library) {
	JobSystem::Initialize();
	LogManager::Initialize({true, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	TypeRegistry::RegisterPendingTypes();
	static char packages = 0;
	CSharpConformance::StandUpPackages(&packages);
	str error;
	if (!library || !CSharpImpl::NativeAOTHost::Start(library, error)) {
		std::printf("START FAILED %s\n", error.c_str());
		LogManager::Terminate();
		JobSystem::Terminate();
		return 2;
	}
	SceneSystem::Initialize();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	std::printf("ASSETS staged=%d\n", assets->IsStaged());
	std::optional<CSharpConformance::InputRig> input(std::in_place, &CSharpImpl::BindingProbe::SetDriver);
	std::printf("INPUT ready=%d\n", input->IsReady());
	SceneSystem::CreateScene("World");
	const SltManagedAPI& managed = CSharpImpl::ScriptRuntime::GetManaged();
	std::printf("ENTRIES load=%d unload=%d\n", managed.loadUserAssemblies != nullptr, managed.unloadUserAssemblies != nullptr);
	const int32_t count = managed.getConformanceTestCount();
	const Time::FrameScope frame(CSharpConformance::MakeFrame());
	int32_t fillsCache = -1;
	int32_t reportsCache = -1;
	for (int32_t index = 0; index < count; ++index) {
		SltString name{};
		managed.getConformanceTestName(index, &name);
		const str caseName(View(name));
		fillsCache = caseName == "ObjectProbe.FillsCache" ? index : fillsCache;
		reportsCache = caseName == "ObjectProbe.ReportsCacheCount" ? index : reportsCache;
		SltString message{};
		const int32_t result = managed.runConformanceTest(index, &message);
		std::printf("CASE %d %s | %.*s\n", result, caseName.c_str(), static_cast<int>(View(message).size()), View(message).data());
		std::fflush(stdout);
	}

	char token = 0;
	SLT_CALL_LINKED_PACKAGE(CSharp, &token);
	const auto cached = [&] {
		SltString message{};
		managed.runConformanceTest(reportsCache, &message);
		return std::atoi(str(View(message)).c_str());
	};
	for (u32 pass = 0; pass < 8; ++pass) {
		FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	}
	const int live = cached();
	SltString ignored{};
	managed.runConformanceTest(fillsCache, &ignored);
	const int filled = cached() - live;
	FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	const int swept = cached() - live;
	FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	std::printf("SWEEP %d %d %d live %d\n", filled, swept, cached() - live, live);
	FrameStages::UnregisterOwnedBy(&token);

	SceneSystem::Terminate();
	input.reset();
	assets.reset();
	CSharpImpl::ScriptRuntime::Detach();
	LogManager::Terminate();
	JobSystem::Terminate();
	std::printf("DONE %d\n", count);
	return 0;
}

TEST(CSharpNativeAOTTest, ConformanceSuitePasses) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;
	EXPECT_TRUE(run.Has("ASSETS staged=1")) << run.output;
	EXPECT_TRUE(run.Has("INPUT ready=1")) << run.output;
	EXPECT_TRUE(run.Has("ENTRIES load=0 unload=0")) << run.output;

	int ran = 0;
	for (const auto& [name, outcome] : run.cases) {
		if (!CSharpConformance::IsInPassSet(name)) {
			continue;
		}
		EXPECT_EQ(outcome.first, SLT_OK) << name << ": " << outcome.second;
		++ran;
	}
	EXPECT_GE(ran, CSharpConformance::PASS_SET_SIZE) << run.output;
	EXPECT_TRUE(run.Has(std::format("DONE {}", run.cases.size()))) << run.output;
}

TEST(CSharpNativeAOTTest, AudioConformanceSuitePasses) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAudioAOT.dll", TestHelpers::CSHARP_AOT_AUDIO_FLAG);
	ASSERT_EQ(run.exitCode, 0) << run.output;
	EXPECT_TRUE(run.Has("ASSETS staged=1")) << run.output;
	for (const auto& [name, outcome] : run.cases) {
		EXPECT_EQ(outcome.first, SLT_OK) << name << ": " << outcome.second;
	}
	EXPECT_EQ(static_cast<int32_t>(run.cases.size()), CSharpConformance::AUDIO_CASES) << run.output;
	EXPECT_TRUE(run.Has(std::format("DONE {}", run.cases.size()))) << run.output;
	EXPECT_FALSE(run.Has("[ERROR]")) << run.output;
}

TEST(CSharpNativeAOTTest, FrameEndSweepsTheCache) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;
	EXPECT_TRUE(run.Has("\nSWEEP 300 44 0")) << run.output;
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

	ASSERT_TRUE(run.cases.contains("ObjectProbe.AddsAFailingScript")) << run.output;
	EXPECT_EQ(run.cases.at("ObjectProbe.AddsAFailingScript").first, SLT_OK) << run.cases.at("ObjectProbe.AddsAFailingScript").second;
	EXPECT_TRUE(run.Has("System.InvalidOperationException: thrown by a constructor")) << run.output;
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

TEST(CSharpNativeAOTTest, PlayersDrawNoDebugLines) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll");
	ASSERT_EQ(run.exitCode, 0) << run.output;
	ASSERT_TRUE(run.cases.contains("RenderingProbe.CountsDrawnLines")) << run.output;
	EXPECT_EQ(run.cases.at("RenderingProbe.CountsDrawnLines").second, "lines 0");
}

TEST(CSharpNativeAOTTest, ComponentsRunTheirHooks) {
	const HostRun run = RunHost(AOTDirectory() / "Shalltear.ConformanceAOT.dll", TestHelpers::CSHARP_AOT_COMPONENTS_FLAG);
	ASSERT_EQ(run.exitCode, 0) << run.output;
	ASSERT_TRUE(run.Has("\nDONE")) << run.output;

	const str output = "\n" + run.output;
	const auto line = [&output](std::string_view prefix) {
		const size_t at = output.find(str("\n") + str(prefix));
		if (at == str::npos) {
			return str();
		}
		const size_t begin = at + 1 + prefix.size();
		size_t end = output.find('\n', begin);
		end = end == str::npos ? output.size() : end;
		str text = output.substr(begin, end - begin);
		while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
			text.pop_back();
		}
		return text;
	};
	const str twin = line("TWIN ");
	EXPECT_FALSE(twin.empty()) << run.output;
	EXPECT_EQ(line("SCRIPT "), twin) << run.output;
	EXPECT_NE(twin.find("late update"), str::npos) << twin;
	EXPECT_NE(twin.find("destroy"), str::npos) << twin;

	EXPECT_EQ(line("CROSSINGS "), "1 1 3") << run.output;
	EXPECT_EQ(line("TICKS HookRecorder "), "7") << run.output;
	EXPECT_EQ(line("TICKS UpdateOnly "), "1") << run.output;
	EXPECT_EQ(line("TICKS Silent "), "0") << run.output;
	EXPECT_EQ(line("TICKS LateReporter "), "4") << run.output;
	EXPECT_EQ(line("TICKS HolderOfInt "), "2") << run.output;
	EXPECT_EQ(line("PARENT "), "1 1") << run.output;
	EXPECT_EQ(line("INERT "), "1") << run.output;
	EXPECT_TRUE(run.Has("System.InvalidOperationException: thrown by a constructor")) << run.output;
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
	bool repeatable = false;
	bool symbolsByName = false;
	size_t start = 0;
	while (start < inputs.size()) {
		size_t end = inputs.find_first_of("\r\n", start);
		if (end == str::npos) {
			end = inputs.size();
		}
		str token = inputs.substr(start, end - start);
		start = end + 1;
		withoutDefaults = withoutDefaults || token == "/NODEFAULTLIB";
		repeatable = repeatable || token == "/Brepro";
		symbolsByName = symbolsByName || token == "/PDBALTPATH:%_PDB%";
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
	EXPECT_TRUE(repeatable);
	EXPECT_TRUE(symbolsByName);
}

#endif
