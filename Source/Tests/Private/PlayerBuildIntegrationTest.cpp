#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/GraphicsProfile.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/PrimitiveMesh.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CookOps.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Asset/PakReader.h"
#include "StaticLinkAnchors.h"

#include "ScopedSettingsStore.h"
#include "StaleSDK.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <iterator>
#include <thread>

using namespace Shalltear;

namespace {

	constexpr f64 COMPILE_TIMEOUT_SECONDS = 300.0;
	constexpr f64 BUILD_TIMEOUT_SECONDS = 600.0;

	constexpr const char* PLAYER_MARKER = "SHALLTEAR_PLAYER_PROBE";

	constexpr const char* HUD_MARKER = "SHALLTEAR_HUD_PROBE";

	constexpr const char* DATA_MARKER = "SHALLTEAR_DATA_PROBE";

	constexpr const char* EXTENSION_MARKER = "SHALLTEAR_EXTENSION_PROBE";

	constexpr const char* EXTENSION_SOURCE = R"(#include "Shalltear.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Module/ModuleEvents.h"

#include "Game/BuildStamp.h"

using namespace Shalltear;

namespace {

	u32 _endFrameRuns = 0;

	class ReadyProbe final : public Object {
	public:
		void OnReady(const EmptyEventPayload&) {
			SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_EXTENSION_PROBE ready");
		}
	};

	void OnEndFrame(const FrameStageArgs&, void*) {
		if (++_endFrameRuns == 1) {
			SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_EXTENSION_PROBE stage");
		}
	}

	void RegisterExtensions(const void* owner) {
		const TypeInfo* component = TypeRegistry::FindType(StringHash("ExampleComponent"));
		SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_EXTENSION_PROBE registered owned={}",
					 component && component->ownerModule == owner ? "yes" : "no");
		FrameStages::Register(FrameStage::EndFrame, FrameStagePosition::Normal, &OnEndFrame, nullptr, owner);
		if (BuildStamp::IsPlayer) {
			ModuleEvents::ExtensionsRegistered.Subscribe(EventBus::GetGlobal(), Handle<ReadyProbe>(New<ReadyProbe>(MemTag::Scene)), &ReadyProbe::OnReady);
		}
	}

}

SLT_IMPLEMENT_CORE_EXTENSIONS(&RegisterExtensions)
)";

	constexpr int HUD_SHEET_WIDTH = 240;
	constexpr int HUD_INLINE_HEIGHT = 64;

	constexpr const char* HUD_SHEET = R"(.hud {
	width: 240px;
	padding: 8px;
	flex-direction: column;
}

.hud-line {
	height: 20px;
}
)";

	constexpr const char* HUD_SLOT_DOCUMENT = R"(<ui version="1">
	<panel class="hud-line">
		<panel id="slot-label">slot</panel>
	</panel>
</ui>
)";

	constexpr const char* HUD_DOCUMENT = R"(<ui version="1">
	<sheet src="project://Assets/UI/Hud.slss"/>
	<template src="project://Assets/UI/HudSlot.slxml" name="Slot"/>
	<panel id="hud-panel" class="hud" style="height: 64px">
		<panel class="hud-line" id="hud-title">Shalltear</panel>
		<instance template="Slot" id="hud-score">
			<attribute-overrides element-id="slot-label">Score 0</attribute-overrides>
		</instance>
		<button id="hud-pause" text="Pause"/>
	</panel>
</ui>
)";

	constexpr const char* HUD_COMPONENT_HEADER = R"(#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UITypes.h"

namespace Shalltear::UI {
	class Context;
}

class HudComponent : public Shalltear::Behavior {
public:
	SLT_REFLECT_COMPONENT(HudComponent, Shalltear::Behavior)
	SLT_REFLECT_FIELD(document)
	SLT_END_REFLECT()

	Shalltear::Handle<Shalltear::UIDocumentAsset> document;

protected:
	void OnEnable() override;
	void OnUpdate() override;

private:
	Shalltear::UI::Context* _context = nullptr;
	Shalltear::UI::ElementHandle _panel;
	Shalltear::UI::ElementHandle _pause;
	Shalltear::u32 _framesSeen = 0;
	bool _reported = false;
};
)";

	constexpr const char* HUD_COMPONENT_SOURCE = R"(#include "HudComponent.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window/WindowSystem.h"

using namespace Shalltear;

SLT_REGISTER_TYPE(HudComponent)

namespace {

	u32 CountSubtree(UI::Context& context, UI::ElementHandle handle) {
		const UI::Element* element = context.GetElement(handle);
		if (!element) {
			return 0;
		}
		u32 count = 1;
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle childHandle = context.MakeHandle(child);
			count += CountSubtree(context, childHandle);
			const UI::Element* resolved = context.GetElement(childHandle);
			child = resolved ? resolved->nextSibling : UI::INVALID_UI_INDEX;
		}
		return count;
	}

}

void HudComponent::OnEnable() {
	Handle<Window> window = WindowSystem::GetPrimaryWindow();
	if (!window.IsValid()) {
		SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_HUD_PROBE no-window");
		return;
	}
	UI::Context* context = UISystem::CreateContext(window);
	if (!context) {
		SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_HUD_PROBE no-context");
		return;
	}

	UIDocumentAsset* document = this->document.Get();
	if (!document) {
		SLT_LOG_ERROR("PlayerProbe", "SHALLTEAR_HUD_PROBE no-document");
		return;
	}

	UIInstantiateDesc desc;
	desc.font = UISystem::GetDefaultFont();
	Vector<UIInstantiateMapping> mapping{GetMemoryResource(MemTag::UI)};
	document->Instantiate(*context, context->GetRoot(), desc, mapping);

	str title;
	str score;
	for (const UIInstantiateMapping& entry : mapping) {
		if (!entry.node) {
			continue;
		}
		if (entry.node->id == "hud-panel") {
			_panel = entry.element;
		} else if (entry.node->id == "hud-title") {
			title = context->GetTextString(entry.element);
		} else if (entry.node->id == "slot-label") {
			score = context->GetTextString(entry.element);
		} else if (entry.node->id == "hud-pause") {
			_pause = entry.element;
		}
	}
	_context = context;
	SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_HUD_PROBE built nodes={} title='{}' score='{}'",
				 mapping.size(), title, score);
}

void HudComponent::OnUpdate() {
	if (_reported || !_context || ++_framesSeen < 3) {
		return;
	}
	_reported = true;
	if (!UISystem::IsContextLive(_context)) {
		SLT_LOG_ERROR("PlayerProbe", "SHALLTEAR_HUD_PROBE context-died");
		return;
	}
	if (!_context->IsAlive(_panel)) {
		SLT_LOG_ERROR("PlayerProbe", "SHALLTEAR_HUD_PROBE no-panel");
		return;
	}

	const UI::LayoutData& layout = _context->GetLayout(_panel);
	u32 pauseSubtree = CountSubtree(*_context, _pause);
	u32 pauseKids = pauseSubtree > 0 ? pauseSubtree - 1 : 0;
	f32 scale = _context->GetContentScale();
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_HUD_PROBE laid-out width={} height={} scale={} elements={} pause-kids={} quads={}",
				 static_cast<i32>(layout.borderBox.w / scale), static_cast<i32>(layout.borderBox.h / scale),
				 scale, CountSubtree(*_context, _panel), pauseKids, UISystem::GetStats().quads);
}
)";

	constexpr const char* POTION_CONFIG_HEADER = R"(#pragma once

#include "Shalltear/Core/Asset/DataAsset.h"

class PotionConfig : public Shalltear::DataAsset {
public:
	SLT_REFLECT_CLASS(PotionConfig)
	SLT_REFLECT_PARENT(Shalltear::DataAsset)
	SLT_CREATE_ASSET_MENU("Potion", ".potion")
	SLT_REFLECT_FIELD(healAmount)
	SLT_REFLECT_FIELD(displayName)
	SLT_END_REFLECT()

	Shalltear::i32 healAmount = 10;
	Shalltear::str displayName = "Potion";
};
)";

	constexpr const char* POTION_CONFIG_SOURCE = R"(#include "PotionConfig.h"

SLT_REGISTER_TYPE(PotionConfig)
)";

	constexpr const char* POTION_DRINKER_HEADER = R"(#pragma once

#include "PotionConfig.h"

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Behavior.h"

class PotionDrinker : public Shalltear::Behavior {
public:
	SLT_REFLECT_COMPONENT(PotionDrinker, Shalltear::Behavior)
	SLT_REFLECT_FIELD(potion)
	SLT_END_REFLECT()

	Shalltear::Handle<PotionConfig> potion;

protected:
	void OnEnable() override;
};
)";

	constexpr const char* POTION_DRINKER_SOURCE = R"(#include "PotionDrinker.h"

#include "Shalltear/Core/Log.h"

using namespace Shalltear;

SLT_REGISTER_TYPE(PotionDrinker)

void PotionDrinker::OnEnable() {
	PotionConfig* resolved = potion.Get();
	if (!resolved) {
		SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_DATA_PROBE no-potion");
		return;
	}
	SLT_LOG_INFO("PlayerProbe", "SHALLTEAR_DATA_PROBE heal={} name='{}'", resolved->healAmount, resolved->displayName);
}
)";

	std::filesystem::path RepoDir(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	str RunCommand(const str& commandLine, int& outExitCode) {
		return TestPlatform::RunCommand(commandLine, outExitCode);
	}

	str ReadAllBytes(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary);
		return str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	void WriteSolidTGA(const std::filesystem::path& path, u32 edge, const u8 (&rgba)[4]) {
		str bytes(18, '\0');
		bytes[2] = 2;
		bytes[12] = static_cast<char>(edge & 0xFF);
		bytes[13] = static_cast<char>(edge >> 8);
		bytes[14] = static_cast<char>(edge & 0xFF);
		bytes[15] = static_cast<char>(edge >> 8);
		bytes[16] = 32;
		bytes[17] = 0x28;
		for (u32 i = 0; i < edge * edge; ++i) {
			bytes += static_cast<char>(rgba[2]);
			bytes += static_cast<char>(rgba[1]);
			bytes += static_cast<char>(rgba[0]);
			bytes += static_cast<char>(rgba[3]);
		}
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << bytes;
	}

	class PlayerBuildIntegrationTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		str _config;
		std::unique_ptr<ScopedUserDataDirectory> _userData;

		void SetUp() override {
			_sdkRoot = ScriptProjectOps::GetSDKRoot();
			if (_sdkRoot.empty()) {
				GTEST_SKIP() << "no Shalltear CMake package found";
			}
			str linkMode = ReadPackageLinkMode(_sdkRoot);
			if (linkMode != "STATIC") {
				GTEST_SKIP() << "this SDK is " << (linkMode.empty() ? "unknown" : linkMode) << ". Player links the game statically";
			}

			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = _sdkRoot;
			CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);
			if (!toolchain.IsComplete()) {
				GTEST_SKIP() << "toolchain incomplete:\n"
							 << CppToolchain::Describe(toolchain);
			}
			_cmakePath = toolchain.cmake.path;

			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_player_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);

			_userData = std::make_unique<ScopedUserDataDirectory>(_root / "UserData");

			FileSystem::Mount("project", Path(_root.generic_string()));

			_engineAssets = RepoDir("EngineAssets");
			ASSERT_FALSE(_engineAssets.empty());
			FileSystem::Mount("engine", Path(_engineAssets.generic_string()));

			_config = str(ScriptProjectOps::GetActiveConfigName());
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			ModuleSystem::Initialize();
		}

		void TearDown() override {
			if (IsSkipped()) {
				return;
			}
			Settings::Terminate();
			SceneSystem::Terminate();
			ModuleSystem::Terminate();
			AssetDatabase::Terminate();
			BuildStepRegistry::Shutdown();
			BuildTargetRegistry::Shutdown();
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			_userData.reset();
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		str Root() const { return _root.generic_string(); }
		const str& SDKRoot() const { return _sdkRoot; }
		const std::filesystem::path& EngineAssets() const { return _engineAssets; }

		void WriteFile(const str& relative, const str& contents) {
			std::filesystem::path target = _root / relative;
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			std::ofstream file(target, std::ios::binary | std::ios::trunc);
			file << contents;
		}

		str RunCMake(const str& arguments, int& outExitCode) const {
			return RunCommand("\"" + _cmakePath + "\" " + arguments, outExitCode);
		}

		bool RunCompile(CompileService& service) const {
			CompileRequest request;
			request.projectPath = Root();
			if (!service.RequestCompile(request)) {
				return false;
			}
			auto deadline = std::chrono::steady_clock::now() +
							std::chrono::duration<f64>(COMPILE_TIMEOUT_SECONDS);
			while (service.IsBusy() && std::chrono::steady_clock::now() < deadline) {
				service.PumpMainThread();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			service.PumpMainThread();
			return service.GetState() == CompileState::Succeeded;
		}

		void WriteProfile(const str& name, const str& target, const Vector<str>& scenes,
						  std::initializer_list<std::pair<const char*, const char*>> settings = {}) const {
			BuildProfile profile;
			profile.name = name;
			profile.target = target;
			profile.SetList("scenes", scenes);
			profile.SetString("configuration", _config);
			for (const auto& [key, value] : settings) {
				profile.SetString(key, value);
			}
			ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(Root(), name), profile));
		}

		BuildResult Build(const Vector<str>& profiles, const str& projectPath = {}) const {
			BuildPipeline pipeline;
			BuildRequest request;
			request.projectPath = projectPath.empty() ? Root() : projectPath;
			request.profiles = profiles;
			str refusal;
			EXPECT_TRUE(pipeline.Request(request, refusal)) << refusal;

			auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(BUILD_TIMEOUT_SECONDS);
			while (pipeline.IsBusy() && std::chrono::steady_clock::now() < deadline) {
				pipeline.PumpMainThread();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			EXPECT_FALSE(pipeline.IsBusy()) << BUILD_TIMEOUT_SECONDS << "s";
			return pipeline.GetResult();
		}

		static str DescribeFailure(const BuildResult& result) {
			str out;
			for (const BuildProfileResult& entry : result.profiles) {
				out += entry.profile + ": " + entry.reason + "\n";
				for (const BuildIssue& issue : entry.issues) {
					out += "  " + issue.key + ": " + issue.message + "\n";
				}
			}
			return out;
		}

		str EditorExecutable() const {
			return Path(ScriptProjectOps::GetEditorExecutablePath()).GetDirectory() +
				   "/Shalltear" SLT_TEST_EXE_SUFFIX;
		}
	private:
		static str ReadPackageLinkMode(const str& sdkRoot) {
			for (const char* relative : {"ShalltearConfig.cmake", "Lib/cmake/Shalltear/ShalltearConfig.cmake"}) {
				std::filesystem::path candidate = std::filesystem::path(sdkRoot) / relative;
				std::ifstream file(candidate);
				if (!file) {
					continue;
				}
				str text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				size_t at = text.find("set(SHALLTEAR_LINK_MODE \"");
				if (at == str::npos) {
					continue;
				}
				at += std::strlen("set(SHALLTEAR_LINK_MODE \"");
				size_t end = text.find('"', at);
				return end == str::npos ? str() : text.substr(at, end - at);
			}
			return {};
		}

		str _sdkRoot;
		str _cmakePath;
		std::filesystem::path _engineAssets;
	};

} // anonymous namespace

TEST_F(PlayerBuildIntegrationTest, PipelineRunsStaticPlayer) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PlayerProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions scaffoldOptions;
	scaffoldOptions.moduleName = "PlayerProbe";
	scaffoldOptions.sdkRoot = SDKRoot();
	scaffoldOptions.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), scaffoldOptions, scaffolded));

	str gitignore = FileSystem::ReadTextFile(Path(Root() + "/.gitignore"), MemTag::Editor);
	EXPECT_NE(gitignore.find("/Builds/"), str::npos) << gitignore;

	ASSERT_TRUE(FileSystem::WriteTextFile(Path(Root() + "/ProjectSettings.ini"),
										  str("[Graphics]\nadapter = 99\n")));

	str source = FileSystem::ReadTextFile(Path(Root() + "/Source/ExampleComponent.cpp"), MemTag::Editor);
	ASSERT_FALSE(source.empty());
	source = StringUtil::Replace(source, "#include \"ExampleComponent.h\"",
								 "#include \"ExampleComponent.h\"\n#include \"Game/BuildStamp.h\"");
	ASSERT_NE(source.find("Game/BuildStamp.h"), str::npos) << source;
	source = StringUtil::Replace(source, "SLT_LOG_INFO(\"PlayerProbe\", \"ExampleComponent enabled\");",
								 str("SLT_LOG_INFO(\"PlayerProbe\", \"") + PLAYER_MARKER +
									 " isPlayer={} target={} number={} version={} profile={}\", "
									 "BuildStamp::IsPlayer, BuildStamp::TargetId, BuildStamp::Number, "
									 "BuildStamp::Version, BuildStamp::ProfileName);");
	ASSERT_NE(source.find(PLAYER_MARKER), str::npos) << source;
	WriteFile("Source/ExampleComponent.cpp", source);

	WriteFile("Assets/UI/Hud.slss", HUD_SHEET);
	WriteFile("Assets/UI/HudSlot.slxml", HUD_SLOT_DOCUMENT);
	WriteFile("Assets/UI/Hud.slxml", HUD_DOCUMENT);
	WriteFile("Assets/Strings.csv", "key,en\ngreeting,Hello\n");
	WriteFile("Assets/Data.csv", "id,value\n1,2\n");
	WriteFile("Source/HudComponent.h", HUD_COMPONENT_HEADER);
	WriteFile("Source/HudComponent.cpp", HUD_COMPONENT_SOURCE);

	WriteFile("Source/PotionConfig.h", POTION_CONFIG_HEADER);
	WriteFile("Source/PotionConfig.cpp", POTION_CONFIG_SOURCE);
	WriteFile("Source/PotionDrinker.h", POTION_DRINKER_HEADER);
	WriteFile("Source/PotionDrinker.cpp", POTION_DRINKER_SOURCE);

	WriteFile("Source/ExtensionProbe.cpp", EXTENSION_SOURCE);

	CompileService service;
	service.RefreshToolchain();
	if (!RunCompile(service)) {
		str diagnostics;
		for (const CompilerDiagnostic& diagnostic : service.GetDiagnostics()) {
			diagnostics += diagnostic.raw + "\n";
		}
		FAIL() << diagnostics;
	}

	str modulePath = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ModuleHandle module = ModuleSystem::Load(Path(modulePath));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));

	const TypeInfo* probeType = TypeRegistry::FindType(StringHash("ExampleComponent"));
	ASSERT_NE(probeType, nullptr);

	const TypeInfo* hudType = TypeRegistry::FindType(StringHash("HudComponent"));
	ASSERT_NE(hudType, nullptr);

	std::error_code sceneDirEc;
	std::filesystem::create_directories(_root / "Assets" / "Scenes", sceneDirEc);
	ASSERT_FALSE(sceneDirEc) << sceneDirEc.message();

	Handle<Scene> boot = SceneSystem::CreateScene("Boot");
	Handle<SceneObject> cameraObject = SceneSystem::CreateObject("Camera", boot);
	cameraObject.Get()->AddComponent<Camera>();
	SceneSystem::CreateObject("Sky", boot).Get()->AddComponent<Shalltear::Environment>();
	Handle<SceneObject> probeObject = SceneSystem::CreateObject("Probe", boot);
	ASSERT_TRUE(probeObject.Get()->AddComponent(probeType).IsValid());
	Handle<SceneObject> hudObject = SceneSystem::CreateObject("Hud", boot);
	Handle<Component> hudComponent = hudObject.Get()->AddComponent(hudType);
	ASSERT_TRUE(hudComponent.IsValid());

	{
		AssetDatabase::ScanProjectDirectory();
		Asset* hudDocument = AssetDatabase::LoadAsset(Path("project://Assets/UI/Hud.slxml"));
		ASSERT_NE(hudDocument, nullptr);
		const FieldInfo* field = FindField(hudType, StringHash("document"));
		ASSERT_NE(field, nullptr);
		ASSERT_NE(field->addrFn, nullptr);
		*reinterpret_cast<u64*>(field->addrFn(hudComponent.Get())) = hudDocument->GetInstanceId();
	}

	{
		const TypeInfo* potionType = TypeRegistry::FindType(StringHash("PotionConfig"));
		ASSERT_NE(potionType, nullptr);
		const TypeInfo* drinkerType = TypeRegistry::FindType(StringHash("PotionDrinker"));
		ASSERT_NE(drinkerType, nullptr);

		std::error_code dataDirEc;
		std::filesystem::create_directories(_root / "Assets" / "Data", dataDirEc);
		const Path potionPath("project://Assets/Data/Healing.potion");
		ASSERT_TRUE(AssetDatabase::CreateAsset(potionPath, StringHash("PotionConfig")).IsValid());
		Asset* potion = AssetDatabase::LoadAsset(potionPath);
		ASSERT_NE(potion, nullptr);
		const FieldInfo* heal = FindField(potionType, StringHash("healAmount"));
		const FieldInfo* name = FindField(potionType, StringHash("displayName"));
		ASSERT_TRUE(heal && heal->addrFn && name && name->addrFn);
		*reinterpret_cast<i32*>(heal->addrFn(potion)) = 42;
		*reinterpret_cast<str*>(name->addrFn(potion)) = "Elixir";
		AssetDatabase::SaveAsset(potion);

		Handle<Component> drinker = SceneSystem::CreateObject("Drinker", boot).Get()->AddComponent(drinkerType);
		ASSERT_TRUE(drinker.IsValid());
		const FieldInfo* field = FindField(drinkerType, StringHash("potion"));
		ASSERT_TRUE(field && field->addrFn);
		*reinterpret_cast<u64*>(field->addrFn(drinker.Get())) = potion->GetInstanceId();
	}

	ASSERT_TRUE(SceneSystem::SaveScene(boot, Path("project://Assets/Scenes/Boot.scene")));

	Handle<Scene> secret = SceneSystem::CreateScene("FinalBoss");
	SceneSystem::CreateObject("Spoiler", secret);
	ASSERT_TRUE(SceneSystem::SaveScene(secret, Path("project://Assets/Scenes/FinalBoss.scene")));

	Handle<Scene> standalone = SceneSystem::CreateScene("Standalone");
	SceneSystem::CreateObject("Camera", standalone).Get()->AddComponent<Camera>();
	ASSERT_TRUE(SceneSystem::SaveScene(standalone, Path("project://Assets/Scenes/Standalone.scene")));

	SceneSystem::UnloadScene(boot);
	SceneSystem::UnloadScene(secret);
	SceneSystem::UnloadScene(standalone);
	SceneSystem::EndFrame();
	AssetDatabase::ScanProjectDirectory();

	{
		GraphicsProfile* authored = New<GraphicsProfile>(MemTag::Asset);
		authored->msaa = 4;
		authored->tonemap = TonemapCurve::Neutral;
		ASSERT_TRUE(authored->Export(Path("project://Assets/High.slprofile")));
		Delete(authored);
	}
	AssetDatabase::ScanProjectDirectory();

	const Guid qualityProfileGuid = AssetDatabase::LookupGuid(Path("project://Assets/High.slprofile"));
	ASSERT_TRUE(qualityProfileGuid.IsValid());
	const Guid stringTableGuid = AssetDatabase::LookupGuid(Path("project://Assets/Strings.csv"));
	const Guid dataGuid = AssetDatabase::LookupGuid(Path("project://Assets/Data.csv"));
	ASSERT_TRUE(stringTableGuid.IsValid());
	ASSERT_TRUE(dataGuid.IsValid());

	ASSERT_TRUE(FileSystem::WriteTextFile(Path(Root() + "/ProjectSettings.ini"),
										  str("[Graphics]\nadapter = 99\nqualityProfiles = ") +
											  qualityProfileGuid.ToString() + "\nqualityLevel = 0\n"));

	Settings::Initialize(Settings::GetDefaultPaths());

	WriteProfile("Windows", SLT_TEST_HOST_TARGET, {"Assets/Scenes/Boot.scene"},
				 {{"version", "1.0.0"}, {"buildNumber", "${AUTO}"}});

	BuildResult result = Build({"Windows"});
	ASSERT_EQ(result.profiles.size(), 1u);
	ASSERT_EQ(result.profiles[0].outcome, BuildOutcome::Succeeded) << DescribeFailure(result);
	u32 notTables = 0;
	for (const BuildIssue& issue : result.profiles[0].issues) {
		if (issue.severity == BuildIssueSeverity::Warning &&
			issue.message == "'project://Assets/Data.csv' isn't a string table (not cooked)") {
			++notTables;
		}
	}
	EXPECT_EQ(notTables, 1u);

	EXPECT_EQ(result.profiles[0].buildNumber, 1u);

	std::filesystem::path staging = _root / "Builds" / "Windows";
	EXPECT_EQ(result.profiles[0].outputPath, staging.generic_string());

	std::filesystem::path playerExe = staging / "PlayerProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << playerExe.generic_string();
	std::printf("[player] PlayerProbe.exe %llu B\n", static_cast<unsigned long long>(std::filesystem::file_size(playerExe)));

	EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(modulePath)));

	EXPECT_TRUE(std::filesystem::exists(staging / "Data" / "Content.slpak"));
	EXPECT_FALSE(std::filesystem::exists(staging / "Boot.json"));

	{
		PakReader shipped;
		str packageError;
		ASSERT_TRUE(shipped.Mount(Path((staging / "Data" / "Content.slpak").generic_string()), packageError))
			<< packageError;
		const PakSuperblock& sb = shipped.GetSuperblock();
		std::printf("[cook] Content.slpak %llu B, payload %llu B, %u regions, %u pages, %u assets\n",
					static_cast<unsigned long long>(sb.fileSize), static_cast<unsigned long long>(sb.payloadSize),
					sb.regionCount, sb.pageCount, sb.assetCount);
		EXPECT_GT(sb.regionCount, 0u);
		for (u32 i = 0; i < sb.regionCount; ++i) {
			const PakRegion* region = shipped.GetRegion(i);
			ASSERT_NE(region, nullptr);
			EXPECT_EQ(region->codec, static_cast<u8>(PakCodec::None));
		}

		EXPECT_EQ(shipped.GetDocument(PakDocumentKind::ProjectSettings),
				  FileSystem::ReadTextFile(Path(Root() + "/ProjectSettings.ini"), MemTag::Editor));
		EXPECT_FALSE(shipped.GetDocument(PakDocumentKind::BootManifest).empty());

		{
			str reportPath = BuildPipeline::GetCookReportPath(Root(), "Windows");
			str reportJSON = FileSystem::ReadTextFile(Path(reportPath), MemTag::Editor);
			ASSERT_FALSE(reportJSON.empty()) << reportPath;

			CookOps::CookReport report;
			str reportError;
			ASSERT_TRUE(CookOps::ReadReportJSON(reportJSON, report, reportError)) << reportError;

			EXPECT_EQ(report.fileBytes, sb.fileSize);
			EXPECT_EQ(report.fileBytes, std::filesystem::file_size(staging / "Data" / "Content.slpak"));
			EXPECT_EQ(report.tocBytes, sb.tocSizeRaw);
			EXPECT_EQ(report.payloadBytes, sb.payloadSize);
			EXPECT_EQ(report.regionCount, sb.regionCount);
			EXPECT_EQ(report.pageCount, sb.pageCount);
			ASSERT_EQ(report.assets.size(), sb.assetCount);
			EXPECT_EQ(report.profileName, "Windows");
			EXPECT_GE(report.candidateCount, report.assets.size());

			for (u32 i = 0; i < sb.assetCount; ++i) {
				const PakAssetEntry* entry = shipped.GetAsset(i);
				ASSERT_NE(entry, nullptr);
				EXPECT_EQ(report.assets[i].guid, entry->guid) << "report row " << i << ' ' << i;
				EXPECT_EQ(report.assets[i].descriptorBytes, entry->descSize);
			}

			u32 roots = 0;
			for (const CookOps::CookReportAsset& line : report.assets) {
				roots += line.isRoot ? 1u : 0u;
			}
			EXPECT_GE(roots, AssetDatabase::GetEngineBuiltinRoots().size() + 1);

			{
				const CookOps::CookReportAsset* profileRow = nullptr;
				for (const CookOps::CookReportAsset& line : report.assets) {
					if (line.guid == qualityProfileGuid) {
						profileRow = &line;
					}
				}
				ASSERT_NE(profileRow, nullptr);
				EXPECT_TRUE(profileRow->isRoot);
				EXPECT_TRUE(profileRow->rootPath.empty());
			}

			{
				const CookOps::CookReportAsset* tableRow = nullptr;
				for (const CookOps::CookReportAsset& line : report.assets) {
					if (line.guid == stringTableGuid) {
						tableRow = &line;
					}
					EXPECT_NE(line.guid, dataGuid);
				}
				ASSERT_NE(tableRow, nullptr);
				EXPECT_TRUE(tableRow->isRoot);
			}
		}
	}

	EXPECT_FALSE(std::filesystem::exists(staging / "Assets"));

	EXPECT_FALSE(std::filesystem::exists(staging / ".assets.db"));
	{
		u32 sidecars = 0;
		std::error_code walkEc;
		for (std::filesystem::recursive_directory_iterator it(staging, walkEc), end; !walkEc && it != end;
			 it.increment(walkEc)) {
			if (it->is_regular_file(walkEc) && it->path().extension() == ".meta") {
				++sidecars;
			}
		}
		EXPECT_EQ(sidecars, 0u);
	}

	EXPECT_FALSE(std::filesystem::exists(staging / "dxcompiler.dll"));

	EXPECT_FALSE(std::filesystem::exists(staging / "EngineAssets"));

	EXPECT_FALSE(std::filesystem::exists(staging / "ProjectSettings.ini"));

	{
		std::filesystem::path notices = staging / "THIRD-PARTY-NOTICES.txt";
		ASSERT_TRUE(std::filesystem::exists(notices));
		std::ifstream stream(notices, std::ios::binary);
		const str text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
		EXPECT_NE(text.find("FreeType"), str::npos);
		EXPECT_NE(text.find("Inter"), str::npos);
		EXPECT_EQ(text.find("ufbx"), str::npos);
	}

	str artifactKind;
	EXPECT_TRUE(TestPlatform::IsArtifactKind(playerExe.generic_string(), true, artifactKind)) << artifactKind;
	EXPECT_TRUE(TestPlatform::ReadsTextAsUTF8(playerExe.generic_string(), artifactKind)) << artifactKind;
	EXPECT_EQ(TestPlatform::FindForeignDependency(playerExe.generic_string()), "");

	int exitCode = 0;
	str playerLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 8", exitCode);
	ASSERT_EQ(exitCode, 0) << playerLog;

	EXPECT_NE(playerLog.find("forced adapter idx 99"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find(PLAYER_MARKER), str::npos) << playerLog;

	auto expectGameExtensions = [](const str& log) {
		const size_t registered = log.find(str(EXTENSION_MARKER) + " registered owned=yes");
		EXPECT_NE(registered, str::npos) << log;
		EXPECT_LT(registered, log.find(PLAYER_MARKER)) << log;
		EXPECT_EQ(log.find(str(EXTENSION_MARKER) + " registered", registered + 1), str::npos) << log;
		const size_t ready = log.find(str(EXTENSION_MARKER) + " ready");
		ASSERT_NE(ready, str::npos) << log;
		EXPECT_LT(registered, ready) << log;
		EXPECT_LT(ready, log.find(PLAYER_MARKER)) << log;
		EXPECT_EQ(log.find(str(EXTENSION_MARKER) + " ready", ready + 1), str::npos) << log;
		EXPECT_NE(log.find(str(EXTENSION_MARKER) + " stage"), str::npos) << log;
	};
	expectGameExtensions(playerLog);

	EXPECT_EQ(playerLog.find("unloaded type"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find(str(DATA_MARKER) + " heal=42 name='Elixir'"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find(str(HUD_MARKER) + " built"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find("score='Score 0'"), str::npos) << playerLog;
	EXPECT_NE(playerLog.find("title='Shalltear'"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find(std::format("laid-out width={} height={}", HUD_SHEET_WIDTH, HUD_INLINE_HEIGHT)),
			  str::npos)
		<< playerLog;

	EXPECT_NE(playerLog.find("pause-kids=1"), str::npos) << playerLog;

	EXPECT_EQ(playerLog.find("quads=0"), str::npos) << playerLog;

	BuildResult rebuilt = Build({"Windows"});
	ASSERT_EQ(rebuilt.profiles.size(), 1u);
	ASSERT_EQ(rebuilt.profiles[0].outcome, BuildOutcome::Succeeded) << DescribeFailure(rebuilt);
	EXPECT_EQ(rebuilt.profiles[0].buildNumber, 2u);

	str secondLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 8", exitCode);
	ASSERT_EQ(exitCode, 0) << secondLog;
	EXPECT_NE(secondLog.find(PLAYER_MARKER), str::npos) << secondLog;
	EXPECT_EQ(secondLog.find("could not be resolved"), str::npos) << secondLog;
	EXPECT_NE(secondLog.find("number=2"), str::npos) << secondLog;

	str lostLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 12 --lose-device 4", exitCode);
	ASSERT_EQ(exitCode, 0) << lostLog;
	EXPECT_NE(lostLog.find("device loss simulated"), str::npos) << lostLog;
	EXPECT_NE(lostLog.find("device rebuilt in"), str::npos) << lostLog;
	EXPECT_EQ(lostLog.find("Recreate it"), str::npos) << lostLog;
	EXPECT_NE(lostLog.find(PLAYER_MARKER), str::npos) << lostLog;
	auto errors = [](const str& log) {
		size_t count = 0;
		for (size_t at = log.find("[ERROR]"); at != str::npos; at = log.find("[ERROR]", at + 1)) {
			++count;
		}
		return count;
	};
	EXPECT_EQ(errors(lostLog), errors(secondLog)) << lostLog;
	auto memoryReport = [](const str& log) {
		const size_t from = log.find("MEMORY REPORT");
		const size_t total = log.find("TOTAL", from);
		return from == str::npos || total == str::npos ? str() : log.substr(from, log.find('\n', total) - from);
	};
	EXPECT_FALSE(memoryReport(lostLog).empty()) << lostLog;
	EXPECT_EQ(memoryReport(lostLog), memoryReport(secondLog));

	str blindLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 12 --lose-device 4 --fail-rebuilds 1000", exitCode);
	ASSERT_EQ(exitCode, 0) << blindLog;
	EXPECT_NE(blindLog.find("couldn't rebuild device (simulated, 1 attempt(s) in 0 s). Retrying"), str::npos) << blindLog;
	EXPECT_EQ(blindLog.find("device rebuilt in"), str::npos) << blindLog;
	EXPECT_NE(blindLog.find(PLAYER_MARKER), str::npos) << blindLog;
	EXPECT_EQ(errors(blindLog), errors(secondLog)) << blindLog;
	EXPECT_EQ(memoryReport(blindLog), memoryReport(secondLog));

	str ninjaFile = FileSystem::ReadTextFile(
		Path(BuildPipeline::GetIntermediateDirectory(Root(), "Windows") + "/build.ninja"), MemTag::Editor);
	ASSERT_FALSE(ninjaFile.empty());
	constexpr std::string_view WholeArchive = "/WHOLEARCHIVE:";
	constexpr std::string_view GnuWholeArchive = "-Wl,--whole-archive ";
	size_t at = ninjaFile.find(WholeArchive);
	if (at != str::npos) {
		at += WholeArchive.size();
	} else {
		at = ninjaFile.find(GnuWholeArchive);
		ASSERT_NE(at, str::npos);
		at += GnuWholeArchive.size();
	}
	size_t end = ninjaFile.find_first_of(" \r\n", at);
	str archivePath = ninjaFile.substr(at, end == str::npos ? str::npos : end - at);
	str archiveBytes = FileSystem::ReadTextFile(Path(archivePath), MemTag::Editor);
	EXPECT_NE(archiveBytes.find(PLAYER_MARKER), str::npos);

	EXPECT_NE(playerLog.find("isPlayer=true"), str::npos) << playerLog;
	EXPECT_NE(playerLog.find("target=" SLT_TEST_HOST_TARGET " "), str::npos) << playerLog;
	EXPECT_NE(playerLog.find("number=1"), str::npos) << playerLog;
	EXPECT_NE(playerLog.find("version=1.0.0"), str::npos) << playerLog;
	EXPECT_NE(playerLog.find("profile=Windows"), str::npos) << playerLog;

	EXPECT_NE(playerLog.find("PlayerProbe 1.0.0 (build 1"), str::npos) << playerLog;

	size_t reportAt = playerLog.find("MEMORY REPORT");
	ASSERT_NE(reportAt, str::npos) << playerLog;
	EXPECT_EQ(playerLog.find("Editor", reportAt), str::npos) << playerLog.substr(reportAt);

	std::ifstream binary(playerExe, std::ios::binary);
	str bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
	EXPECT_EQ(bytes.find("Shalltear Editor - "), str::npos);

	{
		std::error_code ec;
		std::filesystem::remove_all(staging / "Logs", ec);
		std::filesystem::remove_all(staging / "Library", ec);

		ASSERT_TRUE(TestPlatform::DenyDirectoryWrites(staging.generic_string()));

		{
			std::ofstream probe(staging / "acl-probe.tmp");
			bool writable = probe.is_open();
			probe.close();
			std::filesystem::remove(staging / "acl-probe.tmp", ec);
			if (writable) {
				TestPlatform::AllowDirectoryWrites(staging.generic_string());
				GTEST_FAIL();
			}
		}

		str readOnlyLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 8", exitCode);

		TestPlatform::AllowDirectoryWrites(staging.generic_string());

		ASSERT_EQ(exitCode, 0) << readOnlyLog;

		EXPECT_NE(readOnlyLog.find(PLAYER_MARKER), str::npos) << readOnlyLog;
		EXPECT_EQ(readOnlyLog.find("could not be resolved"), str::npos) << readOnlyLog;

		EXPECT_EQ(readOnlyLog.find("quads=0"), str::npos) << readOnlyLog;
		EXPECT_NE(readOnlyLog.find(str(HUD_MARKER) + " built"), str::npos) << readOnlyLog;

		EXPECT_EQ(readOnlyLog.find("dxcompiler.dll is unavailable"), str::npos) << readOnlyLog;

		EXPECT_FALSE(std::filesystem::exists(staging / "Library"));
		EXPECT_FALSE(std::filesystem::exists(staging / "Logs"));
		EXPECT_FALSE(std::filesystem::exists(staging / "Assets"));
		EXPECT_FALSE(std::filesystem::exists(staging / ".assets.db"));
		(void)ec;
	}

	{
		std::filesystem::path packagePath = staging / "Data" / "Content.slpak";
		str originalBytes;
		{
			std::ifstream in(packagePath, std::ios::binary);
			ASSERT_TRUE(in.is_open()) << packagePath.generic_string();
			originalBytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
		ASSERT_GT(originalBytes.size(), sizeof(PakSuperblock)) << packagePath.generic_string();

		str stampedBytes = originalBytes;
		PakSuperblock sb{};
		std::memcpy(&sb, stampedBytes.data(), sizeof(PakSuperblock));
		const u64 readerBuild = sb.engineHash;
		sb.engineHash ^= 0xA5A5A5A5ull;
		const u64 cookedBy = sb.engineHash;
		sb.crc32c = PakCrc32c(&sb, 4092);
		std::memcpy(stampedBytes.data(), &sb, sizeof(PakSuperblock));
		{
			std::ofstream out(packagePath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(out.is_open());
			out.write(stampedBytes.data(), static_cast<std::streamsize>(stampedBytes.size()));
		}

		str refusedLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 8", exitCode);

		{
			std::ofstream out(packagePath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(out.is_open());
			out.write(originalBytes.data(), static_cast<std::streamsize>(originalBytes.size()));
		}

		EXPECT_NE(exitCode, 0) << refusedLog;

		EXPECT_NE(refusedLog.find(std::format("{:016x}", cookedBy)), str::npos) << refusedLog;
		EXPECT_NE(refusedLog.find(std::format("{:016x}", readerBuild)), str::npos) << refusedLog;
		EXPECT_NE(refusedLog.find("CONFIGURE time"), str::npos) << refusedLog;

		EXPECT_EQ(refusedLog.find("WindowSystem initialized"), str::npos) << refusedLog;
		EXPECT_EQ(refusedLog.find("[Graphics]"), str::npos) << refusedLog;

		EXPECT_EQ(refusedLog.find("could not be resolved"), str::npos) << refusedLog;

		{
			PakReader restored;
			str restoreError;
			ASSERT_TRUE(restored.Mount(Path(packagePath.generic_string()), restoreError)) << restoreError;
		}
	}

	WriteProfile("Windows Server", SLT_TEST_HOST_TARGET "Server", {"Assets/Scenes/Boot.scene"},
				 {{"version", "1.0.0"}, {"buildNumber", "${AUTO}"}});

	BuildResult serverResult = Build({"Windows Server"});
	ASSERT_EQ(serverResult.profiles.size(), 1u);
	ASSERT_EQ(serverResult.profiles[0].outcome, BuildOutcome::Succeeded) << DescribeFailure(serverResult);

	EXPECT_EQ(serverResult.profiles[0].buildNumber, 1u);

	std::filesystem::path serverExe = _root / "Builds" / "Windows Server" / "PlayerProbeServer" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(serverExe)) << serverExe.generic_string();
	EXPECT_NE(serverExe.filename(), playerExe.filename());

	EXPECT_TRUE(TestPlatform::IsArtifactKind(serverExe.generic_string(), false, artifactKind)) << artifactKind;
	EXPECT_TRUE(TestPlatform::ReadsTextAsUTF8(serverExe.generic_string(), artifactKind)) << artifactKind;
	EXPECT_EQ(TestPlatform::FindForeignDependency(serverExe.generic_string()), "");

	str serverLog = RunCommand("\"" + serverExe.generic_string() + "\" --frames 4", exitCode);
	ASSERT_EQ(exitCode, 0) << serverLog;

	EXPECT_NE(serverLog.find(PLAYER_MARKER), str::npos) << serverLog;
	EXPECT_EQ(serverLog.find("could not be resolved"), str::npos) << serverLog;
	EXPECT_NE(serverLog.find("headless"), str::npos) << serverLog;
	EXPECT_NE(serverLog.find("target=" SLT_TEST_HOST_TARGET "Server"), str::npos) << serverLog;
	expectGameExtensions(serverLog);

	EXPECT_EQ(serverLog.find("WindowSystem initialized"), str::npos) << serverLog;
	EXPECT_EQ(serverLog.find("[Graphics]"), str::npos) << serverLog;

	{
		Guid hudGuid = AssetDatabase::LookupGuid(Path("project://Assets/UI/Hud.slxml"));
		ASSERT_TRUE(hudGuid.IsValid());

		std::filesystem::path batchOutput = _root / "BatchBuilds";
		str withCodeLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + Root() +
										 "\" --build Windows --output \"" + batchOutput.generic_string() + "\"",
									 exitCode);
		ASSERT_EQ(exitCode, 0) << withCodeLog;

		EXPECT_EQ(withCodeLog.find("kept verbatim"), str::npos) << withCodeLog;

		PakReader batched;
		str batchError;
		std::filesystem::path batchPackage = batchOutput / "Windows" / "Data" / "Content.slpak";
		ASSERT_TRUE(batched.Mount(Path(batchPackage.generic_string()), batchError)) << batchError;
		EXPECT_NE(batched.FindAsset(hudGuid), PAK_INVALID_INDEX) << withCodeLog;

		PakReader staged;
		str stagedError;
		ASSERT_TRUE(staged.Mount(Path((staging / "Data" / "Content.slpak").generic_string()), stagedError))
			<< stagedError;
		EXPECT_EQ(batched.GetSuperblock().assetCount, staged.GetSuperblock().assetCount) << withCodeLog;
	}

	std::filesystem::path contentOnly = _root / "ContentOnly";
	std::filesystem::create_directories(contentOnly / "Assets" / "Scenes");
	{
		Config contentProject;
		contentProject.SetString("Project", "name", "Standalone");
		ASSERT_TRUE(contentProject.Save(Path(contentOnly.generic_string() + "/Project.ini")));
	}
	std::error_code copyEc;
	for (const char* name : {"Standalone.scene", "Standalone.scene.meta"}) {
		std::filesystem::copy_file(_root / "Assets" / "Scenes" / name, contentOnly / "Assets" / "Scenes" / name,
								   std::filesystem::copy_options::overwrite_existing, copyEc);
		ASSERT_FALSE(copyEc) << "staging " << name << " failed: " << copyEc.message();
	}
	std::filesystem::copy_file(_root / ".assets.db", contentOnly / ".assets.db",
							   std::filesystem::copy_options::overwrite_existing, copyEc);
	ASSERT_FALSE(copyEc) << copyEc.message();
	ASSERT_FALSE(ScriptProjectOps::HasScriptProject(contentOnly.generic_string()));

	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{str("Assets/Scenes/Standalone.scene")});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(contentOnly.generic_string(), "Windows"), profile));
	}

	str batchLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + contentOnly.generic_string() +
								  "\" --list-profiles --build Windows",
							  exitCode);
	ASSERT_EQ(exitCode, 0) << batchLog;
	EXPECT_NE(batchLog.find("SUCCEEDED"), str::npos) << batchLog;
	EXPECT_NE(batchLog.find("1 profile(s)"), str::npos) << batchLog;

	EXPECT_TRUE(std::filesystem::exists(contentOnly / "Library" / "Player" / "Windows" / "CMakeLists.txt"));
	EXPECT_FALSE(std::filesystem::exists(contentOnly / "CMakeLists.txt"));

	std::filesystem::path standaloneExe = contentOnly / "Builds" / "Windows" / "Standalone" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(standaloneExe)) << batchLog;

	str standaloneLog = RunCommand("\"" + standaloneExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << standaloneLog;
	EXPECT_EQ(standaloneLog.find("could not be resolved"), str::npos) << standaloneLog;

	str standaloneBytes = ReadAllBytes(standaloneExe);
	ASSERT_FALSE(standaloneBytes.empty());
	for (const char* typeName : {"TextureAsset", "UIDocumentAsset", "StyleSheetAsset"}) {
		EXPECT_NE(standaloneBytes.find(typeName), str::npos);
	}

	str missingLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + contentOnly.generic_string() +
									"\" --build NoSuchProfile",
								exitCode);
	EXPECT_NE(exitCode, 0) << missingLog;
}

TEST_F(PlayerBuildIntegrationTest, TheCloseButtonEndsAShippedGame) {
	std::filesystem::path project = _root / "CloseProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "CloseProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}

	int exitCode = 0;
	str buildLog = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	std::filesystem::path playerExe = project / "Builds" / "Windows" / "CloseProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	struct Game {
		TestPlatform::LaunchedProcess* process = nullptr;
		~Game() { TestPlatform::Terminate(process); }
	} game;
	game.process = TestPlatform::Launch("\"" + playerExe.generic_string() + "\"");
	ASSERT_NE(game.process, nullptr);

	str windowsSeen;
	if (!TestPlatform::WaitForWindow(game.process, "CloseProbe", 30000, windowsSeen)) {
		GTEST_SKIP() << (windowsSeen.empty() ? str("\n    <none>") : windowsSeen);
	}

	i32 gameExitCode = 1;
	ASSERT_FALSE(TestPlatform::WaitForExit(game.process, 1500, gameExitCode));

	ASSERT_TRUE(TestPlatform::CloseWindow(game.process));

	ASSERT_TRUE(TestPlatform::WaitForExit(game.process, 60000, gameExitCode));
	EXPECT_EQ(gameExitCode, 0);
}

TEST_F(PlayerBuildIntegrationTest, LinkMatchesEnabledPackages) {
	constexpr const char* PACKAGE_MARKER = "SHALLTEAR_PACKAGE_PROBE";
	constexpr const char* PACKAGE_ID = "dev.shalltear.samplepackage";

	std::filesystem::path project = _root / "PackageProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "PackageProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}

	int exitCode = 0;
	std::filesystem::path playerExe = project / "Builds" / "Windows" / "PackageProbe" SLT_TEST_PLAYER_SUFFIX;

	str disabledBuild = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << disabledBuild;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << disabledBuild;

	str disabledBytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(disabledBytes.empty());
	EXPECT_EQ(disabledBytes.find(PACKAGE_MARKER), str::npos);

	str disabledRun = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << disabledRun;
	EXPECT_EQ(disabledRun.find(PACKAGE_MARKER), str::npos) << disabledRun;

	{
		Config settings;
		settings.SetString("Packages", "enabled", PACKAGE_ID);
		ASSERT_TRUE(settings.Save(Path(project.generic_string() + "/ProjectSettings.ini")));
	}

	str enabledBuild = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << enabledBuild;

	str enabledBytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(enabledBytes.empty());
	EXPECT_NE(enabledBytes.find(PACKAGE_MARKER), str::npos) << enabledBuild;
	EXPECT_GT(enabledBytes.size(), disabledBytes.size());

	str enabledRun = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << enabledRun;
	EXPECT_NE(enabledRun.find(PACKAGE_MARKER), str::npos) << enabledRun;

	EXPECT_NE(enabledRun.find("runs=4"), str::npos) << enabledRun;

	EXPECT_NE(enabledRun.find("type=registered"), str::npos) << enabledRun;
	EXPECT_EQ(enabledRun.find("type=MISSING"), str::npos) << enabledRun;

	bool sawLiveDelta = false;
	for (size_t at = enabledRun.find("dt="); at != str::npos; at = enabledRun.find("dt=", at + 1)) {
		if (enabledRun.compare(at, 9, "dt=0.0000") != 0) {
			sawLiveDelta = true;
			break;
		}
	}
	EXPECT_TRUE(sawLiveDelta) << enabledRun;

	{
		BuildProfile profile;
		profile.name = "Server";
		profile.target = SLT_TEST_HOST_TARGET "Server";
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Server"), profile));
	}
	str serverBuild = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Server", exitCode);
	ASSERT_EQ(exitCode, 0) << serverBuild;
	std::filesystem::path serverExe = project / "Builds" / "Server" / "PackageProbeServer" SLT_TEST_PLAYER_SUFFIX;
	str serverRun = RunCommand("\"" + serverExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << serverRun;
	EXPECT_NE(serverRun.find(PACKAGE_MARKER), str::npos) << serverRun;
	EXPECT_NE(serverRun.find("type=registered"), str::npos) << serverRun;
	EXPECT_EQ(serverRun.find("type=MISSING"), str::npos) << serverRun;
}

TEST_F(PlayerBuildIntegrationTest, ProjectWithoutCSharpShipsNoCSharp) {
	constexpr const char* PACKAGE_ID = "dev.shalltear.csharp";
	constexpr const char* PACKAGE_ARCHIVE = "CSharp.a";
	constexpr const char* PACKAGE_OBJECT = "ScriptRuntime.cpp.o";
	constexpr const char* PACKAGE_SYMBOL = "Shalltear::CSharpImpl::";
	constexpr const char* BRIDGED_PACKAGES = "dev.shalltear.animation;dev.shalltear.audio;dev.shalltear.physics3d;dev.shalltear.network;"
											 "dev.shalltear.postprocessing;dev.shalltear.visualeffects";
	constexpr const char* BRIDGED_OBJECTS[] = {"Animator.cpp.o", "AudioSystem.cpp.o", "PhysicsWorld.cpp.o", "Network.cpp.o", "PostProcessing.cpp.o", "VisualEffects.cpp.o"};
	constexpr const char* BRIDGE_OBJECTS[] = {"AnimationBindings.cpp.o", "AudioBindings.cpp.o", "PhysicsBindings.cpp.o",
											  "NetworkCSharpPackage.cpp.o", "PostProcessingCSharpPackage.cpp.o", "VisualEffectsCSharpPackage.cpp.o"};

	std::filesystem::path project = _root / "ScriptlessProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "Scriptless");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}
	{
		Config settings;
		settings.SetString("Packages", "enabled", BRIDGED_PACKAGES);
		ASSERT_TRUE(settings.Save(Path(project.generic_string() + "/ProjectSettings.ini")));
	}
	const std::filesystem::path staging = project / "Builds" / "Windows";
	const str command = "\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows";
	const auto build = [&](const std::filesystem::path& map, int& exitCode) {
		const str previous = TestPlatform::GetEnvironment("LDFLAGS");
		TestPlatform::SetEnvironment("LDFLAGS", "-Wl,-Map=" + map.generic_string());
		str log = RunCommand(command, exitCode);
		TestPlatform::SetEnvironment("LDFLAGS", previous);
		return log;
	};

	int exitCode = 0;
	const std::filesystem::path plainMap = _root / "scriptless.map";
	const str plainLog = build(plainMap, exitCode);
	ASSERT_EQ(exitCode, 0) << plainLog;
	const str plain = ReadAllBytes(plainMap);
	ASSERT_NE(plain.find("TypeRegistry.cpp.o"), str::npos) << plainLog;
	ASSERT_NE(plain.find("Shalltear::TypeRegistry::"), str::npos) << plainLog;
	for (const char* object : BRIDGED_OBJECTS) {
		EXPECT_NE(plain.find(object), str::npos) << object << plainLog;
	}
	for (const char* object : BRIDGE_OBJECTS) {
		EXPECT_EQ(plain.find(object), str::npos) << object;
	}
	EXPECT_EQ(plain.find(PACKAGE_OBJECT), str::npos);
	EXPECT_EQ(plain.find(PACKAGE_SYMBOL), str::npos);
	EXPECT_EQ(plain.find("NativeAOTHost"), str::npos);
	std::error_code ec;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(staging, ec)) {
		EXPECT_EQ(entry.path().filename().generic_string().find("GameScripts"), str::npos) << entry.path().generic_string();
		EXPECT_EQ(entry.path().filename().generic_string().find("DotNet"), str::npos) << entry.path().generic_string();
	}

	if (!std::filesystem::is_regular_file(std::filesystem::path(SDKRoot()) / "lib" / PACKAGE_ARCHIVE, ec)) {
		return;
	}
	{
		Config settings;
		settings.SetString("Packages", "enabled", str(PACKAGE_ID) + ";" + BRIDGED_PACKAGES);
		ASSERT_TRUE(settings.Save(Path(project.generic_string() + "/ProjectSettings.ini")));
	}
	std::filesystem::remove_all(project / "Library" / "Player", ec);
	const std::filesystem::path scriptedMap = _root / "scripted.map";
	const str scriptedLog = build(scriptedMap, exitCode);
	ASSERT_EQ(exitCode, 0) << scriptedLog;
	const str scripted = ReadAllBytes(scriptedMap);
	EXPECT_NE(scripted.find(PACKAGE_OBJECT), str::npos) << scriptedLog;
	EXPECT_NE(scripted.find(PACKAGE_SYMBOL), str::npos) << scriptedLog;
	for (const char* object : BRIDGE_OBJECTS) {
		EXPECT_NE(scripted.find(object), str::npos) << object << scriptedLog;
	}
}

TEST_F(PlayerBuildIntegrationTest, DisabledPackageLinkRefused) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PackageCallProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions scaffoldOptions;
	scaffoldOptions.moduleName = "PackageCallProbe";
	scaffoldOptions.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), scaffoldOptions, scaffolded));
	const str scaffoldedLists = FileSystem::ReadTextFile(Path(Root() + "/CMakeLists.txt"), MemTag::Editor);
	ASSERT_EQ(scaffoldedLists.find("Physics3D"), str::npos) << scaffoldedLists;

	WriteFile("Source/PackageCall.cpp", R"(#include "Shalltear.h"

u32 PackageCallOverlapCount() {
	Vector<Handle<Collider>> results;
	return Physics::OverlapSphere(Vec3{0.0f, 0.0f, 0.0f}, 1.0f, results);
}
)");
	CppToolchain::DiscoveryOptions discovery;
	discovery.sdkRoot = SDKRoot();
	discovery.probeVersions = false;
	const CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(discovery);
	auto configure = [&](const char* directory, const char* packages, int& outExitCode) {
		return RunCMake("-S \"" + Root() + "\" -B \"" + Root() + "/Library/" + directory + "\" -G Ninja" +
							" -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + toolchain.compiler.path + "\"" +
							" -DCMAKE_MAKE_PROGRAM=\"" + toolchain.ninja.path + "\" -DShalltear_ROOT=\"" + SDKRoot() + "\"" +
							(toolchain.sdkToolchainFile.empty() ? str() : " -DCMAKE_TOOLCHAIN_FILE=\"" + toolchain.sdkToolchainFile + "\"") +
							" -DSHALLTEAR_PLAYER_TARGET=" SLT_TEST_HOST_TARGET " -DSHALLTEAR_ENABLED_PACKAGES=\"" + packages + "\"",
						outExitCode);
	};
	auto build = [&](const char* directory, int& outExitCode) {
		return RunCMake("--build \"" + Root() + "/Library/" + directory + "\"", outExitCode);
	};

	int exitCode = 0;
	const str configured = configure("PlayerOn", "dev.shalltear.physics3d", exitCode);
	ASSERT_EQ(exitCode, 0) << configured;
	const str built = build("PlayerOn", exitCode);
	EXPECT_EQ(exitCode, 0) << built;

	const str configuredOff = configure("PlayerOff", "", exitCode);
	ASSERT_EQ(exitCode, 0) << configuredOff;
	const str builtOff = build("PlayerOff", exitCode);
	EXPECT_NE(exitCode, 0) << builtOff;
	EXPECT_NE(builtOff.find("undeclared identifier 'Physics'"), str::npos) << builtOff;

	WriteFile("CMakeLists.txt", scaffoldedLists + "\ntarget_link_libraries(PackageCallProbe PRIVATE Shalltear::Audio)\n");
	const str refused = configure("PlayerRefused", "dev.shalltear.physics3d", exitCode);
	EXPECT_NE(exitCode, 0) << refused;
	EXPECT_NE(refused.find("dev.shalltear.audio is not"), str::npos) << refused;
}

TEST_F(PlayerBuildIntegrationTest, PlayerHasNoImportPipeline) {
	constexpr const char* PARSER_MARKER = "sparse accessors aren't supported";
	constexpr const char* PREFAB_MARKER = "has shear (importing its closest TRS)";
	constexpr const char* PRIMITIVE_MARKER = "PrimitiveMeshImporter: '{}' has unknown shape '{}'";
	constexpr const char* UFBX_MARKER = "Kaydara FBX Binary";
	constexpr const char* PIPELINE_ID = "dev.shalltear.importpipeline";

	std::filesystem::path project = _root / "ToolStripProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "ToolStrip");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}

	{
		Config settings;
		settings.SetString("Packages", "enabled", PIPELINE_ID);
		ASSERT_TRUE(settings.Save(Path(project.generic_string() + "/ProjectSettings.ini")));
	}

	int exitCode = 0;
	str buildLog = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;

	std::filesystem::path playerExe = project / "Builds" / "Windows" / "ToolStrip" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	str bytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(bytes.empty());

	EXPECT_EQ(bytes.find(PARSER_MARKER), str::npos);
	EXPECT_EQ(bytes.find(PREFAB_MARKER), str::npos);
	EXPECT_EQ(bytes.find(PRIMITIVE_MARKER), str::npos);
	EXPECT_EQ(bytes.find(UFBX_MARKER), str::npos);

	for (const char* typeName : {"TextureAsset", "UIDocumentAsset", "StyleSheetAsset", "MeshAsset", "PrefabAsset"}) {
		EXPECT_NE(bytes.find(typeName), str::npos) << typeName;
	}

	str runLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << runLog;
	EXPECT_EQ(runLog.find("could not be resolved"), str::npos) << runLog;
}

TEST_F(PlayerBuildIntegrationTest, PlayerHasNoBuildMachinePath) {
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "PathStrip");
		ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));
	}
	ScriptProjectOps::ScaffoldOptions scaffoldOptions;
	scaffoldOptions.moduleName = "PathStrip";
	scaffoldOptions.sdkRoot = SDKRoot();
	scaffoldOptions.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), scaffoldOptions, scaffolded));
	WriteProfile("Windows", SLT_TEST_HOST_TARGET, {});

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service));

	int exitCode = 0;
	str buildLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + Root() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	std::filesystem::path playerExe = _root / "Builds" / "Windows" / "PathStrip" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	const str bytes = StringUtil::ToLower(ReadAllBytes(playerExe));
	ASSERT_FALSE(bytes.empty());

	const str checkout = EngineAssets().parent_path().generic_string();
	ASSERT_FALSE(checkout.empty());
	for (const str& root : {checkout, Root()}) {
		const str forward = StringUtil::ToLower(root);
		EXPECT_EQ(bytes.find(forward), str::npos) << root;
		EXPECT_EQ(bytes.find(StringUtil::Replace(forward, "/", "\\")), str::npos) << root;
	}

	for (const char* mapped : {"source/examplecomponent.cpp", "source/core/private/"}) {
		const str forward = mapped;
		EXPECT_TRUE(bytes.find(forward) != str::npos || bytes.find(StringUtil::Replace(forward, "/", "\\")) != str::npos)
			<< mapped;
	}
}

TEST_F(PlayerBuildIntegrationTest, PlayerLinksNoneOfCoresImportTier) {
	constexpr const char* SHADER_IMPORT_MARKER = "ShaderAsset: source '{}' is empty/unreadable";
	constexpr const char* TEXTURE_IMPORT_MARKER = "TextureAsset: '{}' unreadable";
	constexpr const char* COMPILER_MARKER = "dxcompiler.dll";
	constexpr const char* REFLECTION_MARKER = "not a SPIR-V module (bad magic)";
	constexpr const char* DECODER_MARKER = "bad IHDR len";
	constexpr const char* FORWARDER_MARKER = "(no ImportTier::Install in this process)";

	std::filesystem::path project = _root / "TierStripProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "TierStrip");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}

	int exitCode = 0;
	str buildLog = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;

	std::filesystem::path playerExe = project / "Builds" / "Windows" / "TierStrip" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;
	str bytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(bytes.empty());

	EXPECT_EQ(bytes.find(SHADER_IMPORT_MARKER), str::npos);
	EXPECT_EQ(bytes.find(TEXTURE_IMPORT_MARKER), str::npos);
	EXPECT_EQ(bytes.find(COMPILER_MARKER), str::npos);
	EXPECT_EQ(bytes.find(REFLECTION_MARKER), str::npos);
	EXPECT_EQ(bytes.find(DECODER_MARKER), str::npos);
	EXPECT_NE(bytes.find(FORWARDER_MARKER), str::npos);

	str runLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << runLog;
}

TEST_F(PlayerBuildIntegrationTest, PlayerHasNoGraphCode) {
	constexpr const char* COMPILER_MARKER = "has more inputs than the compiler supports";
	constexpr const char* IMPORTER_MARKER = "is a node shader file in a format this engine doesn't import";
	constexpr const char* GENERATOR_MARKER = "ShalltearGraphWorldToTangent";
	constexpr const char* LIBRARY_MARKER = "Sample Texture 2D LOD";

	{
		Config projectFile;
		projectFile.SetString("Project", "name", "GraphStrip");
		ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));
	}
	WriteFile("Assets/Tint.slshader",
			  "{\"_type\":\"NodeShader\",\"version\":1,\"nextId\":5,\"settings\":{\"target\":\"unlit\"},"
			  "\"properties\":[{\"id\":1,\"name\":\"Tint\",\"reference\":\"tint\",\"type\":\"Color\",\"default\":[1,1,1,1]}],"
			  "\"nodes\":[{\"id\":2,\"def\":\"Input/Property\",\"x\":0,\"y\":0,\"settings\":{\"property\":1}}],"
			  "\"contexts\":[{\"id\":3,\"def\":\"Context/Fragment\",\"x\":300,\"y\":0,\"blocks\":["
			  "{\"id\":4,\"def\":\"Block/BaseColor\",\"in\":{\"Value\":{\"from\":[2,\"Out\"]}}}]}],"
			  "\"groups\":[],\"notes\":[]}\n");
	AssetDatabase::ScanProjectDirectory();
	const Guid graph = AssetDatabase::LookupGuid(Path("project://Assets/Tint.slshader"));
	ASSERT_TRUE(graph.IsValid());
	WriteFile("Assets/Tint.mat", "{\n\t\"shader\": \"" + graph.ToString() + "\",\n\t\"colors\": { \"tint\": [0.2, 0.6, 0.4, 1] }\n}\n");
	AssetDatabase::ScanProjectDirectory();
	auto* material = static_cast<MaterialAsset*>(AssetDatabase::LoadAsset(Path("project://Assets/Tint.mat")));
	ASSERT_NE(material, nullptr);
	ASSERT_TRUE(material->shader.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("Graph");
	Handle<SceneObject> quad = SceneSystem::CreateObject("Quad", scene);
	Handle<MeshRenderer> renderer = quad->AddComponent<MeshRenderer>();
	renderer->SetMesh(Handle<MeshAsset>(static_cast<MeshAsset*>(AssetDatabase::LoadAsset(Path(GetPrimitiveMeshPath(PrimitiveShape::Quad))))));
	renderer->SetMaterial(0, Handle<MaterialAsset>(material));
	Handle<SceneObject> camera = SceneSystem::CreateObject("Camera", scene);
	camera->SetLocalPosition(Vec3(0.0f, 0.0f, 3.0f));
	camera->AddComponent<Camera>();
	std::filesystem::create_directories(_root / "Assets" / "Scenes");
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Scenes/Graph.scene")));
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	AssetDatabase::ScanProjectDirectory();
	WriteProfile("Windows", SLT_TEST_HOST_TARGET, {"Assets/Scenes/Graph.scene"});

	int exitCode = 0;
	str buildLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + Root() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	std::filesystem::path playerExe = _root / "Builds" / "Windows" / "GraphStrip" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	{
		PakReader shipped;
		str packageError;
		ASSERT_TRUE(shipped.Mount(Path((_root / "Builds" / "Windows" / "Data" / "Content.slpak").generic_string()), packageError))
			<< packageError;
		u32 index = shipped.FindAsset(graph);
		ASSERT_NE(index, PAK_INVALID_INDEX) << buildLog;
		const PakAssetEntry* entry = shipped.GetAsset(index);
		ASSERT_NE(entry, nullptr);
		Buffer payloadBytes;
		ASSERT_TRUE(shipped.ReadRegion(entry->regionIndex, payloadBytes, packageError)) << packageError;
		CookReader descriptor = shipped.GetDescriptor(index);
		CookReader payload(payloadBytes.data, payloadBytes.size);
		ShaderAsset cooked;
		ASSERT_TRUE(cooked.LoadCooked(descriptor, payload));
		ASSERT_FALSE(cooked.variants.empty());
		EXPECT_FALSE(cooked.variants[SHADER_BASE_VARIANT].pixelSpirv.empty());
	}

	str bytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(bytes.empty());
	EXPECT_EQ(bytes.find(COMPILER_MARKER), str::npos);
	EXPECT_EQ(bytes.find(IMPORTER_MARKER), str::npos);
	EXPECT_EQ(bytes.find(GENERATOR_MARKER), str::npos);
	EXPECT_EQ(bytes.find(LIBRARY_MARKER), str::npos);
	for (const char* typeName : {"ShaderAsset", "MaterialAsset", "MeshRenderer"}) {
		EXPECT_NE(bytes.find(typeName), str::npos) << typeName;
	}

	str runLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << runLog;
	EXPECT_EQ(runLog.find("could not be resolved"), str::npos) << runLog;
}

TEST_F(PlayerBuildIntegrationTest, PlayerHasNoOpusEncoder) {
	constexpr u8 ENCODER_MARKER[] = {0xE2, 0xF7, 0x02, 0xF4, 0x05, 0xFF, 0x08, 0x09, 0x09, 0x08, 0xF3, 0x12, 0xEF, 0xDE, 0xFB, 0x11};
	constexpr u8 DECODER_MARKER[] = {0x08, 0x00, 0x0A, 0x00, 0x0C, 0x00, 0x0E, 0x00, 0x10, 0x00, 0x14, 0x00, 0x18, 0x00, 0x1C,
									 0x00, 0x22, 0x00, 0x28, 0x00, 0x30, 0x00, 0x3C, 0x00, 0x4E, 0x00, 0x64, 0x00};
	const std::string_view encoder(reinterpret_cast<const char*>(ENCODER_MARKER), sizeof(ENCODER_MARKER));
	const std::string_view decoder(reinterpret_cast<const char*>(DECODER_MARKER), sizeof(DECODER_MARKER));

	str archive;
	for (const char* directory : {"lib", "Lib"}) {
		const std::filesystem::path candidate = std::filesystem::path(SDKRoot()) / directory / "Audio.a";
		if (std::filesystem::exists(candidate)) {
			archive = ReadAllBytes(candidate);
			break;
		}
	}
	ASSERT_FALSE(archive.empty()) << SDKRoot();
	ASSERT_NE(archive.find(encoder), str::npos);
	ASSERT_NE(archive.find(decoder), str::npos);

	std::filesystem::path project = _root / "OpusProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "OpusProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}
	{
		Config settings;
		settings.SetString("Packages", "enabled", "dev.shalltear.audio");
		settings.SetString("Audio", "device", "None");
		ASSERT_TRUE(settings.Save(Path(project.generic_string() + "/ProjectSettings.ini")));
	}

	int exitCode = 0;
	str buildLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	std::filesystem::path playerExe = project / "Builds" / "Windows" / "OpusProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	str bytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(bytes.empty());
	EXPECT_NE(bytes.find(decoder), str::npos);
	EXPECT_EQ(bytes.find(encoder), str::npos);

	str runLog = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << runLog;
}

TEST_F(PlayerBuildIntegrationTest, BuildRefusesSDKFromDifferentEngineBuild) {
	Config projectFile;
	projectFile.SetString("Project", "name", "StaleSDKPlayer");

	std::filesystem::path stalePackage = _root / "StaleSDK";
	if (!StaleSDK::Fabricate(SDKRoot(), stalePackage, "0bad0bad0bad")) {
		GTEST_SKIP();
	}
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScopedSettingsStore settings(_root / "SettingsStore",
								 "[Editor/Build]\nstaticSDKRoot = " + stalePackage.generic_string() + "\n");

	std::error_code ec;
	std::filesystem::create_directories(_root / "Assets" / "Scenes", ec);
	Handle<Scene> scene = SceneSystem::CreateScene("Stale");
	SceneSystem::CreateObject("Camera", scene).Get()->AddComponent<Camera>();
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Scenes/Stale.scene")));
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	AssetDatabase::ScanProjectDirectory();

	WriteProfile("Windows", SLT_TEST_HOST_TARGET, {"Assets/Scenes/Stale.scene"});

	BuildResult result = Build({"Windows"});
	ASSERT_EQ(result.profiles.size(), 1u);

	EXPECT_EQ(result.profiles[0].outcome, BuildOutcome::Failed) << DescribeFailure(result);

	bool refusedAtConfigure = false;
	for (const BuildIssue& issue : result.profiles[0].issues) {
		refusedAtConfigure = refusedAtConfigure || issue.message.find("configure failed") != str::npos;
	}
	EXPECT_TRUE(refusedAtConfigure) << DescribeFailure(result);

	EXPECT_FALSE(std::filesystem::exists(_root / "Builds" / "Windows" / "StaleSDKPlayer" SLT_TEST_PLAYER_SUFFIX));
}

TEST_F(PlayerBuildIntegrationTest, PlayerCarriesLoadableCoreTypes) {
	constexpr const char* ALWAYS_PRESENT_CONTROL = "Object";

	std::filesystem::path project = _root / "AnchorProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "AnchorProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	{
		BuildProfile profile;
		profile.name = "Windows";
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		ASSERT_TRUE(BuildProfileOps::SaveProfile(
			BuildProfileOps::GetProfilePath(project.generic_string(), "Windows"), profile));
	}

	int exitCode = 0;
	str build = RunCommand(
		"\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << build;

	std::filesystem::path playerExe = project / "Builds" / "Windows" / "AnchorProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << build;
	str bytes = ReadAllBytes(playerExe);
	ASSERT_FALSE(bytes.empty());

	const auto carries = [&bytes](const char* name) {
		str needle;
		needle.push_back('\0');
		needle += name;
		needle.push_back('\0');
		return bytes.find(needle) != str::npos;
	};

	ASSERT_TRUE(carries(ALWAYS_PRESENT_CONTROL));

#define SLT_EXPECT_CARRIED(TypeName)                                                                              \
	EXPECT_TRUE(carries(#TypeName))                                                                               \
		<< "the player dropped " #TypeName ". Every cooked row naming it loads as an unloaded type. Its owner's " \
		   "list in StaticLinkAnchors.h names it (the owning system's Initialize is no longer in the link)";
	SLT_SCENE_SYSTEM_ANCHORS(SLT_EXPECT_CARRIED)
	SLT_ASSET_DATABASE_ANCHORS(SLT_EXPECT_CARRIED)
	SLT_RENDER_SYSTEM_ANCHORS(SLT_EXPECT_CARRIED)
	SLT_UI_SYSTEM_ANCHORS(SLT_EXPECT_CARRIED)
#undef SLT_EXPECT_CARRIED

	str run = RunCommand("\"" + playerExe.generic_string() + "\" --frames 4", exitCode);
	EXPECT_EQ(exitCode, 0) << run;
	EXPECT_EQ(run.find("unloaded type"), str::npos) << run;
}

TEST_F(PlayerBuildIntegrationTest, BuiltGameShowsIcon) {
	constexpr u8 COLOR[4] = {200, 40, 90, 255};

	std::filesystem::path project = _root / "IconProject";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "IconProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	WriteSolidTGA(project / "Assets" / "Icon.tga", 256, COLOR);
	for (const char* name : {"Iconic", "Plain"}) {
		BuildProfile profile;
		profile.name = name;
		profile.target = SLT_TEST_HOST_TARGET;
		profile.SetList("scenes", Vector<str>{});
		profile.SetString("configuration", _config);
		if (std::string_view(name) == "Iconic") {
			profile.SetString("icon", "Assets/Icon.tga");
		}
		ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(project.generic_string(), name), profile));
	}

	int exitCode = 0;
	str buildLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + project.generic_string() +
								  "\" --build Iconic --build Plain",
							  exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	const std::filesystem::path playerExe = project / "Builds" / "Iconic" / "IconProbe" SLT_TEST_PLAYER_SUFFIX;
	const std::filesystem::path plainExe = project / "Builds" / "Plain" / "IconProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;
	ASSERT_TRUE(std::filesystem::exists(plainExe)) << buildLog;

	const auto isColor = [&COLOR](const TestPlatform::IconImage& image) {
		for (size_t i = 0; i < image.rgba.size(); i += 4) {
			if (std::memcmp(image.rgba.data() + i, COLOR, 4) != 0) {
				return false;
			}
		}
		return !image.rgba.empty();
	};

	const std::vector<TestPlatform::IconImage> fileIcon = TestPlatform::ReadExecutableIcon(playerExe.generic_string());
	EXPECT_TRUE(TestPlatform::ReadExecutableIcon(plainExe.generic_string()).empty());
	if (TestPlatform::ExecutablesCarryIcons()) {
		ASSERT_FALSE(fileIcon.empty());
		EXPECT_EQ(fileIcon.front().size, Vec2I(16, 16));
		EXPECT_EQ(fileIcon.back().size, Vec2I(256, 256));
		for (const TestPlatform::IconImage& image : fileIcon) {
			EXPECT_TRUE(isColor(image)) << image.size.x;
		}
	} else {
		EXPECT_TRUE(fileIcon.empty());
	}

	for (const char* name : {"Iconic", "Plain"}) {
		PakReader package;
		str error;
		ASSERT_TRUE(package.Mount(Path((project / "Builds" / name / "Data" / "Content.slpak").generic_string()), error))
			<< error;
		const bool packaged = !package.GetDocument(PakDocumentKind::Icon).empty();
		EXPECT_EQ(packaged, std::string_view(name) == "Iconic" && !TestPlatform::ExecutablesCarryIcons()) << name;
	}

	struct Game {
		TestPlatform::LaunchedProcess* process = nullptr;
		~Game() { TestPlatform::Terminate(process); }
	} game;
	game.process = TestPlatform::Launch("\"" + playerExe.generic_string() +
										"\" --screen-fullscreen 0 --screen-width 160 --screen-height 120");
	ASSERT_NE(game.process, nullptr);
	str windowsSeen;
	if (!TestPlatform::WaitForWindow(game.process, "IconProbe", 30000, windowsSeen)) {
		GTEST_SKIP() << (windowsSeen.empty() ? str("\n    <none>") : windowsSeen);
	}

	std::vector<TestPlatform::IconImage> windowIcon;
	for (int attempt = 0; attempt < 50 && windowIcon.empty(); ++attempt) {
		windowIcon = TestPlatform::ReadWindowIcon(game.process);
		if (windowIcon.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	ASSERT_FALSE(windowIcon.empty());
	for (const TestPlatform::IconImage& image : windowIcon) {
		EXPECT_TRUE(isColor(image)) << image.size.x;
	}

	ASSERT_TRUE(TestPlatform::CloseWindow(game.process));
	i32 gameExitCode = 1;
	ASSERT_TRUE(TestPlatform::WaitForExit(game.process, 60000, gameExitCode));
	EXPECT_EQ(gameExitCode, 0);
}

TEST_F(PlayerBuildIntegrationTest, ProjectOutsideAsciiBuildsAndRuns) {
	const std::filesystem::path project = _root / u8"Projét Олег 太郎";
	std::filesystem::create_directories(project / "Assets");
	{
		Config projectFile;
		projectFile.SetString("Project", "name", "NamesProbe");
		ASSERT_TRUE(projectFile.Save(Path(project.generic_string() + "/Project.ini")));
	}
	BuildProfile profile;
	profile.name = "Plain";
	profile.target = SLT_TEST_HOST_TARGET;
	profile.SetList("scenes", Vector<str>{});
	profile.SetString("configuration", _config);
	ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(project.generic_string(), "Plain"), profile));

	const ScopedUserDataDirectory userData(_root / u8"appdata é Олег 太郎");
	int exitCode = 0;
	const str buildLog = RunCommand("\"" + EditorExecutable() + "\" --project \"" + project.generic_string() + "\" --build Plain",
									exitCode);
	ASSERT_EQ(exitCode, 0) << buildLog;
	const std::filesystem::path playerExe = project / "Builds" / "Plain" / "NamesProbe" SLT_TEST_PLAYER_SUFFIX;
	ASSERT_TRUE(std::filesystem::exists(playerExe)) << buildLog;

	const str run = RunCommand("\"" + playerExe.generic_string() +
								   "\" --frames 4 --screen-fullscreen 0 --screen-width 160 --screen-height 120",
							   exitCode);
	EXPECT_EQ(exitCode, 0) << run;
	EXPECT_NE(run.find("NamesProbe"), str::npos) << run;
}
