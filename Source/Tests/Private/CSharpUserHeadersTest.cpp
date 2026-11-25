#include "TestsPCH.h"

#include "Shalltear/Core/Module/ModuleThunkRegistry.h"

#include <atomic>
#include <cstdint>

using namespace Shalltear;

namespace {

	void DummyThunkA() {}
	void DummyThunkB() {}

} // anonymous namespace

TEST(ModuleThunkRegistryTest, RegisterLookupUnregisterAndEpochTracking) {
	ModuleThunkRegistry::Clear();
	const u64 startEpoch = ModuleThunkRegistry::GetEpoch();
	const std::atomic<u64>* epochPtr = ModuleThunkRegistry::GetEpochAddress();
	ASSERT_NE(epochPtr, nullptr);
	EXPECT_EQ(epochPtr->load(std::memory_order_acquire), startEpoch);

	int ownerA = 0;
	int ownerB = 0;
	const void* tableA[] = {reinterpret_cast<const void*>(&DummyThunkA)};
	const void* tableB[] = {reinterpret_cast<const void*>(&DummyThunkA), reinterpret_cast<const void*>(&DummyThunkB)};

	ModuleThunkRegistry::RegisterModuleThunkTable("Alpha", tableA, 1, &ownerA);
	const u64 afterA = ModuleThunkRegistry::GetEpoch();
	EXPECT_GT(afterA, startEpoch);
	EXPECT_EQ(epochPtr->load(std::memory_order_acquire), afterA);

	u32 count = 0;
	u64 observedEpoch = 0;
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Alpha", &count, &observedEpoch), static_cast<const void*>(tableA));
	EXPECT_EQ(count, 1u);
	EXPECT_EQ(observedEpoch, afterA);

	ModuleThunkRegistry::RegisterModuleThunkTable("Beta", tableB, 2, &ownerB);
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Beta", &count, nullptr), static_cast<const void*>(tableB));
	EXPECT_EQ(count, 2u);

	// re-registering replaces previous table and advances epoch
	const u64 beforeReplace = ModuleThunkRegistry::GetEpoch();
	ModuleThunkRegistry::RegisterModuleThunkTable("Alpha", tableB, 2, &ownerA);
	EXPECT_GT(ModuleThunkRegistry::GetEpoch(), beforeReplace);
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Alpha", &count, nullptr), static_cast<const void*>(tableB));
	EXPECT_EQ(count, 2u);

	// unregistering ownerA leaves ownerB untouched
	EXPECT_EQ(ModuleThunkRegistry::UnregisterThunkTablesOwnedBy(&ownerA), 1u);
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Alpha"), nullptr);
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Beta"), static_cast<const void*>(tableB));

	EXPECT_EQ(ModuleThunkRegistry::UnregisterThunkTablesOwnedBy(&ownerB), 1u);
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("Beta"), nullptr);
	ModuleThunkRegistry::Clear();
}

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* SIMLIB_HEADER = R"(#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>

namespace SimLib {

	struct alignas(16) Vec4Aligned {
		float x;
		float y;
		float z;
		float w;
	};

	struct BasePose {
		float posX = 0.0f;
		float posY = 0.0f;
	};

	struct CarPose : public BasePose {
		enum {
			MODE_IDLE = 0,
			MODE_ACTIVE = 1,
		};

		struct {
			float throttle;
			float steer;
		};

		float wheelSpin[4] = {};
		Vec4Aligned orientation = {0.0f, 0.0f, 0.0f, 1.0f};
		int32_t mode = MODE_IDLE;
	};

	struct ConfigSpec {
		float gravity;
		int32_t maxTicks;
	};

	extern const ConfigSpec DEFAULT_CONFIG;

	class Agent {
	public:
		int32_t id = 0;
		CarPose pose = {};

		int32_t GetId() const { return id; }
		CarPose GetPose() const { return pose; }
		void SetPose(const CarPose& next) { pose = next; }
	private:
		Agent() = default;
		friend class World;
	};

	class World {
	public:
		using StepCallback = std::function<void(int32_t, float)>;

		static World* Create();
		~World();

		Agent* AddAgent(int32_t id);
		const std::vector<Agent*>& GetAgents() const { return _agents; }
		const std::unordered_set<Agent*>& GetActiveAgents() const { return _active; }
		void SetStepCallback(StepCallback callback);
		void Step(int32_t ticks = 1);
		int32_t GetTotalTicks() const { return _ticks; }
		float SumSpan(std::span<const float> values) const;
		int32_t LoadPath(const std::filesystem::path& path) const;
	private:
		World() = default;
		std::vector<Agent*> _agents;
		std::unordered_set<Agent*> _active;
		StepCallback _callback;
		int32_t _ticks = 0;
	};

	// lifecycle / ignored functions that ExposurePolicy must exclude
	void Initialize();
	void Shutdown();

	[[clang::annotate("slt_script_ignore")]]
	int32_t HiddenSecret();

	class PolicyBlockedType {
	public:
		int32_t secret = 1;
	};
	int32_t PolicyBlockedFunc();

	class TrackedResource {
	public:
		static TrackedResource* Create();
		~TrackedResource();
	private:
		TrackedResource() = default;
	};

	void RecordMainThread();
	int32_t GetDestroyedCount();
	int32_t GetDestroyedOnMainThreadCount();
	void StoreRetainedCallback(World::StepCallback callback);
	void FireRetainedCallback(int32_t ticks);
	void ClearRetainedCallback();

} // namespace SimLib
)";

	constexpr const char* SIMLIB_SOURCE = R"(#include "SimLib.h"

#include <atomic>

namespace SimLib {

	namespace {

		std::thread::id _mainThreadId{};
		std::atomic<int32_t> _destroyedCount{0};
		std::atomic<int32_t> _destroyedOnMainCount{0};
		World::StepCallback _retainedCallback;

	} // anonymous namespace

	const ConfigSpec DEFAULT_CONFIG = {-9.81f, 120};

	World* World::Create() {
		return new World();
	}

	World::~World() {
		for (Agent* agent : _agents) {
			delete agent;
		}
	}

	Agent* World::AddAgent(int32_t id) {
		Agent* agent = new Agent();
		agent->id = id;
		agent->pose.posX = static_cast<float>(id) * 2.0f;
		agent->pose.posY = 3.5f;
		agent->pose.throttle = 0.75f;
		agent->pose.steer = -0.25f;
		agent->pose.wheelSpin[2] = 9.5f;
		agent->pose.orientation = {1.0f, 2.0f, 3.0f, 4.0f};
		agent->pose.mode = CarPose::MODE_ACTIVE;
		_agents.push_back(agent);
		_active.insert(agent);
		return agent;
	}

	void World::SetStepCallback(StepCallback callback) {
		_callback = std::move(callback);
	}

	void World::Step(int32_t ticks) {
		_ticks += ticks;
		if (_callback) {
			_callback(_ticks, static_cast<float>(_ticks) * 0.5f);
		}
	}

	float World::SumSpan(std::span<const float> values) const {
		float total = 0.0f;
		for (float value : values) {
			total += value;
		}
		return total;
	}

	int32_t World::LoadPath(const std::filesystem::path& path) const {
		return static_cast<int32_t>(path.filename().string().size());
	}

	void Initialize() {}
	void Shutdown() {}
	int32_t HiddenSecret() { return 999; }
	int32_t PolicyBlockedFunc() { return 42; }

	TrackedResource* TrackedResource::Create() {
		return new TrackedResource();
	}

	TrackedResource::~TrackedResource() {
		_destroyedCount.fetch_add(1, std::memory_order_relaxed);
		if (std::this_thread::get_id() == _mainThreadId) {
			_destroyedOnMainCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void RecordMainThread() {
		_mainThreadId = std::this_thread::get_id();
		_destroyedCount.store(0, std::memory_order_relaxed);
		_destroyedOnMainCount.store(0, std::memory_order_relaxed);
	}

	int32_t GetDestroyedCount() {
		return _destroyedCount.load(std::memory_order_relaxed);
	}

	int32_t GetDestroyedOnMainThreadCount() {
		return _destroyedOnMainCount.load(std::memory_order_relaxed);
	}

	void StoreRetainedCallback(World::StepCallback callback) {
		_retainedCallback = std::move(callback);
	}

	void FireRetainedCallback(int32_t ticks) {
		if (_retainedCallback) {
			_retainedCallback(ticks, static_cast<float>(ticks) * 0.25f);
		}
	}

	void ClearRetainedCallback() {
		_retainedCallback = {};
	}

} // namespace SimLib

#if defined(SIMLIB_EXPORT_DLL)
#define SIMLIB_EXPORT __declspec(dllexport)
#else
#define SIMLIB_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
SIMLIB_EXPORT void SimLibTestFireAndClearRetained(int32_t ticks) {
	SimLib::FireRetainedCallback(ticks);
	SimLib::ClearRetainedCallback();
}

SIMLIB_EXPORT int32_t SimLibTestGetDestroyedCount() {
	return SimLib::GetDestroyedCount();
}

SIMLIB_EXPORT int32_t SimLibTestGetDestroyedOnMainCount() {
	return SimLib::GetDestroyedOnMainThreadCount();
}
}
)";

	constexpr const char* SIM_PROBE_CS = R"(using System;
using System.Runtime.CompilerServices;
using Shalltear;

namespace ZeroGlueGame;

public sealed partial class SimProbe : Behavior {
	public int agentCount;
	public int activeAgentCount;
	public int firstAgentId;
	public float posX;
	public float throttle;
	public float wheel2;
	public float orientW;
	public int modeValue;
	public int totalTicks;
	public int callbackTicks;
	public float callbackHalf;
	public float spanSum;
	public int pathLength;
	public int longPathLength;
	public float defaultGravity;
	public int defaultMaxTicks;
	public int destroyedBeforeTick;

	[Callable]
	public int ComputeBoost(int baseValue) {
		return baseValue * 3;
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	static void AllocateDroppedResource() {
		_ = SimLib.TrackedResource.Create();
	}

	protected override void OnEnable() {
		SimLib.Native.RecordMainThread();
		SimLib.ConfigSpec cfg = SimLib.Native.DEFAULT_CONFIG;
		defaultGravity = cfg.gravity;
		defaultMaxTicks = cfg.maxTicks;

		using SimLib.World world = SimLib.World.Create()!;
		SimLib.Agent a1 = world.AddAgent(10)!;
		_ = world.AddAgent(20);

		SimLib.Agent[] agents = world.GetAgents();
		SimLib.Agent[] active = world.GetActiveAgents();
		agentCount = agents.Length;
		activeAgentCount = active.Length;
		firstAgentId = a1.GetId();

		SimLib.CarPose pose = a1.GetPose();
		posX = pose.posX;
		throttle = pose.throttle;
		unsafe {
			wheel2 = pose.wheelSpin[2];
		}
		orientW = pose.orientation.w;
		modeValue = pose.mode == SimLib.CarPose.MODE_ACTIVE ? 1 : 0;

		world.SetStepCallback((ticks, half) => {
			callbackTicks = ticks;
			callbackHalf = half;
		});
		world.Step();
		world.Step(4);
		totalTicks = world.GetTotalTicks();

		Span<float> values = stackalloc float[3] { 1.5f, 2.5f, 6.0f };
		spanSum = world.SumSpan(values);
		pathLength = world.LoadPath("maps/arena.cmf");
		string longDir = new string('a', 300);
		longPathLength = world.LoadPath(longDir + "/long_arena_map.cmf");

		// drop an un-disposed opaque wrapper and force finalization off the main thread
		AllocateDroppedResource();
		GC.Collect();
		GC.WaitForPendingFinalizers();
		destroyedBeforeTick = SimLib.Native.GetDestroyedCount();

		// retain a callback in native global state to verify safe ALC unload
		SimLib.Native.StoreRetainedCallback((ticks, half) => {
			callbackTicks += ticks;
		});
	}
}
)";

	str ReadAllText(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	class CSharpUserHeadersTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		ModuleHandle _userModule;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			ASSERT_TRUE(Attach()) << _error;

			_root = fs::temp_directory_path() /
					("slt_cs_headers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "ZeroGlueGame";
			fs::create_directories(_project / "Assets" / "Scripts");
			fs::create_directories(_project / "ThirdParty" / "SimLib");
		}

		void TearDown() override {
			if (ModuleSystem::IsLoaded(_userModule)) {
				ModuleSystem::Unload(_userModule);
				_userModule = {};
			}
			if (_editor) {
				_editor->shutdownReload();
			}
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			FileSystem::Unmount("engine");
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}
	};

} // anonymous namespace

TEST_F(CSharpUserHeadersTest, ZeroSourceLibraryBindingRoundTripsAndEmitsCppView) {
	const str sdk = _hooks->findSDKRoot();
	if (sdk.empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}

	Config projectFile;
	projectFile.SetString("Project", "name", "ZeroGlueGame");
	ASSERT_TRUE(projectFile.Save(Path(_project.generic_string() + "/Project.ini")));

	Config settings;
	settings.SetString("Packages", "enabled", "dev.shalltear.csharp");
	settings.SetBool("C#", "allowUnsafeCode", true);
	ASSERT_TRUE(settings.Save(Path(_project.generic_string() + "/ProjectSettings.ini")));

	std::ofstream(_project / "ThirdParty" / "SimLib" / "SimLib.h", std::ios::binary) << SIMLIB_HEADER;
	std::ofstream(_project / "ThirdParty" / "SimLib" / "SimLib.cpp", std::ios::binary) << SIMLIB_SOURCE;
	std::ofstream(_project / "ThirdParty" / "SimLib" / "CMakeLists.txt", std::ios::binary)
		<< "add_library(SimLib STATIC SimLib.cpp)\n"
		<< "target_include_directories(SimLib PUBLIC \"${CMAKE_CURRENT_SOURCE_DIR}\")\n"
		<< "target_compile_features(SimLib PUBLIC cxx_std_20)\n"
		<< "if(WIN32)\n"
		<< "\ttarget_compile_definitions(SimLib PRIVATE SIMLIB_EXPORT_DLL=1)\n"
		<< "endif()\n";

	std::ofstream(_project / "ExposurePolicy.json", std::ios::binary)
		<< "{\n"
		<< "\t\"excludedFunctions\": [\"PolicyBlockedFunc\"],\n"
		<< "\t\"excludedTypes\": [\"PolicyBlockedType\"]\n"
		<< "}\n";

	std::ofstream(_project / "CMakeLists.txt", std::ios::binary)
		<< "cmake_minimum_required(VERSION 3.24)\n"
		<< "project(ZeroGlueGame LANGUAGES C CXX)\n"
		<< "find_package(Shalltear 0.0.0 REQUIRED)\n"
		<< "add_subdirectory(ThirdParty/SimLib)\n"
		<< "shalltear_add_module(ZeroGlueGame\n"
		<< "\tBIND_HEADERS ThirdParty/SimLib/SimLib.h\n"
		<< "\tBIND_NAMESPACES SimLib\n"
		<< ")\n"
		<< "target_link_libraries(ZeroGlueGame PRIVATE SimLib)\n";

	// verify zero-Source/ project is recognized as a script project
	EXPECT_FALSE(fs::exists(_project / "Source"));
	EXPECT_TRUE(ScriptProjectOps::HasScriptProject(_project.generic_string()));

	// write CMakePresets.json pointing at this build's SDK without creating Source/
	ScriptProjectOps::ScaffoldOptions scaffoldOpts;
	scaffoldOpts.moduleName = "ZeroGlueGame";
	scaffoldOpts.sdkRoot = ScriptProjectOps::GetSDKRoot();
	scaffoldOpts.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	scaffoldOpts.enabledPackages = Vector<str>{"dev.shalltear.csharp"};
	scaffoldOpts.includeStarterComponent = false;
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(_project.generic_string(), scaffoldOpts, scaffolded));
	EXPECT_FALSE(fs::exists(_project / "Source"));

	// compile the zero-source C++ module -> runs Shalltear.HeaderBinder + compiles UserThunks.generated.cpp
	{
		CompileService service;
		service.RefreshToolchain();
		CompileRequest request;
		request.projectPath = _project.generic_string();
		request.enabledPackages = Vector<str>{"dev.shalltear.csharp"};
		ASSERT_TRUE(service.RequestCompile(request));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
		while (service.IsBusy() && std::chrono::steady_clock::now() < deadline) {
			service.PumpMainThread();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		service.PumpMainThread();
		str diagnostics;
		for (const CompilerDiagnostic& diag : service.GetDiagnostics()) {
			diagnostics += diag.raw + "\n";
		}
		ASSERT_EQ(service.GetState(), CompileState::Succeeded) << diagnostics;
	}

	const fs::path bindingsCs = _project / "Library" / "CSharp" / "Generated" / "Native" / "UserBindings.g.cs";
	ASSERT_TRUE(fs::is_regular_file(bindingsCs));
	const str bindingsText = ReadAllText(bindingsCs);
	EXPECT_NE(bindingsText.find("class World"), str::npos);
	EXPECT_NE(bindingsText.find("struct CarPose"), str::npos);
	EXPECT_NE(bindingsText.find("MODE_ACTIVE"), str::npos);
	EXPECT_NE(bindingsText.find("NativeUTF8Buffer"), str::npos);
	EXPECT_EQ(bindingsText.find("[UnmanagedCallersOnly"), str::npos);
	EXPECT_EQ(bindingsText.find("HiddenSecret"), str::npos);
	EXPECT_EQ(bindingsText.find("PolicyBlockedFunc"), str::npos);
	EXPECT_EQ(bindingsText.find("PolicyBlockedType"), str::npos);
	EXPECT_EQ(bindingsText.find("void Initialize("), str::npos);
	EXPECT_EQ(bindingsText.find("void Shutdown("), str::npos);

	// write C# script consuming SimLib bindings + declaring [Callable] method
	std::ofstream(_project / "Assets" / "Scripts" / "SimProbe.cs", std::ios::binary) << SIM_PROBE_CS;

	CSharpProjectFiles::Options csFiles = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
	csFiles.projectName = "ZeroGlueGame";
	csFiles.allowUnsafeCode = true;
	CSharpProjectFiles::Result written;
	ASSERT_TRUE(_editor->writeProjectFiles(csFiles, written));
	ASSERT_TRUE(fs::is_regular_file(_project / "ZeroGlueGame.Native.Scripts.csproj"));
	ASSERT_TRUE(fs::is_regular_file(_project / "ZeroGlueGame.slnx"));

	CompileOptions csOpts;
	csOpts.projectPath = _project.generic_string();
	csOpts.projectFile = "ZeroGlueGame.slnx";
	csOpts.muxer = (fs::path(sdk) / _editor->muxerName).generic_string();
	csOpts.pinStamp = "headers";
	csOpts.buildServers = false;
	_editor->configureCompile(csOpts);
	ASSERT_TRUE(_editor->requestCompile());
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
			_editor->pumpCompile();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		str csDiagnostics;
		for (const CSharpDiagnostics::Diagnostic& diag : _editor->getCompile().GetDiagnostics()) {
			csDiagnostics += diag.file + ":" + std::to_string(diag.line) + " " + diag.code + ": " + diag.message + "\n";
		}
		ASSERT_EQ(_editor->getCompile().GetState(), CompileState::Succeeded) << csDiagnostics << "\n"
																			 << _log.Delivered().Joined();
	}

	// verify C++20 view header emitted for [Callable] method
	const fs::path viewHeader = _project / "Library" / "CSharp" / "Generated" / "Views" / "SimProbe.generated.h";
	ASSERT_TRUE(fs::is_regular_file(viewHeader));
	const str viewText = ReadAllText(viewHeader);
	EXPECT_NE(viewText.find("ComputeBoost"), str::npos) << viewText;

	// locate and load the compiled C++ module DLL (registers its thunk table in ModuleThunkRegistry)
	const fs::path moduleDll(ScriptProjectOps::GetModuleBinaryPath(_project.generic_string(), ScriptProjectOps::GetActiveConfigName()));
	ASSERT_TRUE(fs::is_regular_file(moduleDll)) << "built module DLL not found at " << moduleDll;

	_userModule = ModuleSystem::Load(Path(moduleDll.generic_string()), ModuleLoadPolicy::InPlace);
	ASSERT_TRUE(ModuleSystem::IsLoaded(_userModule));
	EXPECT_NE(ModuleThunkRegistry::GetModuleThunkTable("ZeroGlueGame"), nullptr);

	// load ZeroGlueGame.Native.dll and ZeroGlueGame.Scripts.dll into CoreCLR
	const str nativeDll = (_project / "Library" / "CSharp" / "bin" / "ZeroGlueGame.Native.dll").generic_string();
	const str scriptsDll = (_project / "Library" / "CSharp" / "bin" / "ZeroGlueGame.Scripts.dll").generic_string();
	ASSERT_TRUE(fs::is_regular_file(fs::path(nativeDll)));
	ASSERT_TRUE(fs::is_regular_file(fs::path(scriptsDll)));
	const SltString assemblies[] = {
		SltString{nativeDll.data(), static_cast<int32_t>(nativeDll.size())},
		SltString{scriptsDll.data(), static_cast<int32_t>(scriptsDll.size())},
	};
	ASSERT_EQ(_hooks->getManaged().loadUserAssemblies(assemblies, 2), SLT_OK);

	const TypeInfo* probeType = TypeRegistry::FindType(StringHash("ZeroGlueGame.SimProbe"));
	ASSERT_NE(probeType, nullptr);

	SceneSystem::SetPlaying(true);
	Handle<Scene> scene = SceneSystem::CreateScene("HeaderWorld");
	Handle<SceneObject> actor = SceneSystem::CreateObject("SimActor", scene);
	ASSERT_TRUE(actor.IsValid());
	Handle<Component> comp = actor->AddComponent(probeType);
	ASSERT_TRUE(comp.IsValid());
	SceneSystem::BeginFrame();
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();

	auto readInt = [probeType, &comp](const char* name) -> i32 {
		Variant out;
		EXPECT_TRUE(GetField(comp.Get(), probeType, StringHash(name), &out)) << name;
		return out.As<i32>();
	};
	auto readFloat = [probeType, &comp](const char* name) -> f32 {
		Variant out;
		EXPECT_TRUE(GetField(comp.Get(), probeType, StringHash(name), &out)) << name;
		return out.As<f32>();
	};

	EXPECT_EQ(readInt("agentCount"), 2);
	EXPECT_EQ(readInt("activeAgentCount"), 2);
	EXPECT_EQ(readInt("firstAgentId"), 10);
	EXPECT_FLOAT_EQ(readFloat("posX"), 20.0f);
	EXPECT_FLOAT_EQ(readFloat("throttle"), 0.75f);
	EXPECT_FLOAT_EQ(readFloat("wheel2"), 9.5f);
	EXPECT_FLOAT_EQ(readFloat("orientW"), 4.0f);
	EXPECT_EQ(readInt("modeValue"), 1);
	EXPECT_EQ(readInt("totalTicks"), 5);
	EXPECT_EQ(readInt("callbackTicks"), 5);
	EXPECT_FLOAT_EQ(readFloat("callbackHalf"), 2.5f);
	EXPECT_FLOAT_EQ(readFloat("spanSum"), 10.0f);
	EXPECT_EQ(readInt("pathLength"), 9);
	EXPECT_EQ(readInt("longPathLength"), 18);
	EXPECT_FLOAT_EQ(readFloat("defaultGravity"), -9.81f);
	EXPECT_EQ(readInt("defaultMaxTicks"), 120);

	// finalized TrackedResource must NOT have been deleted on the GC finalizer thread
	EXPECT_EQ(readInt("destroyedBeforeTick"), 0);

	// pump main-thread tick to drain deferred deletes on the main thread
	_hooks->getManaged().tick();
	const auto getDestroyedCount = reinterpret_cast<int32_t (*)()>(
		ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_userModule), "SimLibTestGetDestroyedCount"));
	const auto getDestroyedOnMainCount = reinterpret_cast<int32_t (*)()>(
		ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_userModule), "SimLibTestGetDestroyedOnMainCount"));
	const auto fireAndClearRetained = reinterpret_cast<void (*)(int32_t)>(
		ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_userModule), "SimLibTestFireAndClearRetained"));
	ASSERT_NE(getDestroyedCount, nullptr);
	ASSERT_NE(getDestroyedOnMainCount, nullptr);
	ASSERT_NE(fireAndClearRetained, nullptr);
	EXPECT_EQ(getDestroyedCount(), 1);
	EXPECT_EQ(getDestroyedOnMainCount(), 1);

	// unload scene before leaving play mode and unloading user ALC so no managed Behavior instances remain live
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	SceneSystem::SetPlaying(false);

	// unload user ALC while native SimLib still holds _retainedCallback -> ALC must unload cleanly
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);

	// invoking and releasing the stale native std::function after ALC unload must safely no-op
	fireAndClearRetained(77);

	// unload module and verify thunk table cleanup + epoch bump
	const u64 epochBeforeUnload = ModuleThunkRegistry::GetEpoch();
	ModuleSystem::Unload(_userModule);
	_userModule = {};
	EXPECT_EQ(ModuleThunkRegistry::GetModuleThunkTable("ZeroGlueGame"), nullptr);
	EXPECT_GT(ModuleThunkRegistry::GetEpoch(), epochBeforeUnload);
}

#endif
