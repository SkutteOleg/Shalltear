#include "TestsPCH.h"

#ifdef SHALLTEAR_SHARED

#include "CSharpRuntimeFixture.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/ProfilerModules.h"

#include <chrono>
#include <format>
#include <optional>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpTests;
using Shalltear::CSharpConformance::View;

TEST_F(CSharpRuntimeFixture, StartsAndAttaches) {
	ASSERT_FALSE(_hooks->findSDKRoot().empty());
	ASSERT_TRUE(Attach()) << _error;
	EXPECT_TRUE(_hooks->isAttached());

	const SltManagedAPI& managed = _hooks->getManaged();
	EXPECT_EQ(managed.abiVersion, static_cast<uint32_t>(SLT_BRIDGE_ABI_VERSION));
	EXPECT_EQ(managed.layoutHash, SLT_BRIDGE_LAYOUT_HASH);
	EXPECT_NE(managed.loadUserAssemblies, nullptr);
	EXPECT_NE(managed.unloadUserAssemblies, nullptr);
	EXPECT_NE(managed.shutdown, nullptr);
	EXPECT_EQ(managed.getConformanceTestCount(), 0);

	_hooks->detach();
	EXPECT_FALSE(_hooks->isAttached());
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, SecondHostStartIsRefused) {
	ASSERT_NE(Host(), nullptr) << _error;

	str error;
	EXPECT_EQ(_hooks->startHost(_hooks->findSDKRoot(), _hooks->runtimeVersion, _hooks->getRuntimeAssemblyPath(), error), nullptr);
	EXPECT_NE(error.find("already started"), str::npos) << error;
	EXPECT_NE(_hooks->getHostEntry(), nullptr);
}

TEST_F(CSharpRuntimeFixture, MissingSDKNamesTheLibrary) {
	ASSERT_EQ(_hooks->getHostEntry(), nullptr);
	str error;
	EXPECT_EQ(_hooks->startHost("Z:/no/such/sdk", _hooks->runtimeVersion, _hooks->getRuntimeAssemblyPath(), error), nullptr);
	EXPECT_NE(error.find("Z:/no/such/sdk/host/fxr/"), str::npos) << error;
	EXPECT_EQ(_hooks->getHostEntry(), nullptr);

	ASSERT_TRUE(Attach()) << _error;
}

TEST_F(CSharpRuntimeFixture, PackageUnloadsAfterStringsCross) {
	SceneSystem::Initialize();
	SceneSystem::CreateScene("World");
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	{
		const Time::FrameScope frame(CSharpConformance::MakeFrame());
		str message;
		ASSERT_EQ(Run("ObjectTests.InstantiateCopiesFields", message), SLT_OK) << message;
		ASSERT_EQ(Run("BindingTests.NativeThrowCarriesItsMessage", message), SLT_OK) << message;
	}
	SceneSystem::Terminate();
	_hooks->detach();
	ModuleSystem::Terminate();

	ModuleSystem::Initialize();
	_package = ModuleSystem::Load(Path(str("CSharp") + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace);
	ASSERT_TRUE(ModuleSystem::IsLoaded(_package));
	const auto get = reinterpret_cast<CSharpImpl::GetScriptTestHooksFn>(
		ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), CSharpImpl::SCRIPT_TEST_HOOKS_SYMBOL));
	ASSERT_NE(get, nullptr);
	_hooks = get();
	EXPECT_EQ(_hooks->getHostEntry(), nullptr);
}

TEST_F(CSharpRuntimeFixture, MismatchedLayoutNamesBothSides) {
	const SltInitializeFn entry = Host();
	ASSERT_NE(entry, nullptr) << _error;

	SltEngineAPI engine = _hooks->makeEngineAPI();
	engine.layoutHash = 0x0bad0bad0bad0badull;
	str error;
	EXPECT_FALSE(_hooks->attachAs(engine, entry, error));
	EXPECT_FALSE(_hooks->isAttached());
	EXPECT_NE(error.find("layout 0bad0bad0bad0bad"), str::npos) << error;
	EXPECT_NE(error.find(std::format("runtime ABI {} layout {:016x}", SLT_BRIDGE_ABI_VERSION, SLT_BRIDGE_LAYOUT_HASH)), str::npos) << error;

	engine = _hooks->makeEngineAPI();
	engine.abiVersion = 77;
	error.clear();
	EXPECT_FALSE(_hooks->attachAs(engine, entry, error));
	EXPECT_NE(error.find("bridge ABI 77"), str::npos) << error;

	EXPECT_TRUE(Attach()) << _error;
}

TEST_F(CSharpRuntimeFixture, NamelessTypeNeedsItsHash) {
	ASSERT_TRUE(Attach()) << _error;
	const SltEngineAPI engine = _hooks->makeEngineAPI();

	SltEnumConstant constants[2]{};
	constants[0].nameHash = StringHash("Low").GetValue();
	constants[0].value = 2;
	constants[1].nameHash = StringHash("High").GetValue();
	constants[1].value = 5;
	SltEnumDesc gear{};
	gear.nameHash = StringHash("Bare.Gear").GetValue();
	gear.size = 4;
	gear.constants = constants;
	gear.constantCount = 2;
	ASSERT_EQ(engine.registerManagedEnum(&gear), SLT_OK);
	const TypeInfo* registered = TypeRegistry::FindType(StringHash("Bare.Gear"));
	ASSERT_NE(registered, nullptr);
	EXPECT_EQ(str(registered->displayName), std::format("{:016x}", gear.nameHash));
	ASSERT_EQ(registered->enumConstantCount, 2u);
	EXPECT_EQ(registered->enumConstants[1].displayName, nullptr);
	const EnumConstantInfo* high = FindEnumConstantByName(registered, "High");
	ASSERT_NE(high, nullptr);
	EXPECT_EQ(high->value, 5);

	SltFieldDesc grip{};
	grip.nameHash = StringHash("grip").GetValue();
	grip.kind = SLT_KIND_F32;
	SltValueDesc tuning{};
	tuning.nameHash = StringHash("Bare.Tuning").GetValue();
	tuning.fields = &grip;
	tuning.fieldCount = 1;
	ASSERT_EQ(engine.registerManagedValue(&tuning), SLT_OK);
	const TypeInfo* packed = TypeRegistry::FindType(StringHash("Bare.Tuning"));
	ASSERT_NE(packed, nullptr);
	EXPECT_EQ(packed->fieldCount, 1u);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("")), nullptr);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();

	SltEnumDesc nameless{};
	nameless.size = 4;
	EXPECT_EQ(engine.registerManagedEnum(&nameless), SLT_ERR_REFUSED);
	SltTypeDesc type{};
	EXPECT_EQ(engine.registerManagedType(&type), SLT_ERR_REFUSED);
	SltValueDesc value{};
	EXPECT_EQ(engine.registerManagedValue(&value), SLT_ERR_REFUSED);

	const str wrongName = "Slow";
	SltEnumConstant wrong{};
	wrong.name = SltString{wrongName.data(), static_cast<int32_t>(wrongName.size())};
	wrong.nameHash = StringHash("Fast").GetValue();
	const str paceName = "Bare.Pace";
	SltEnumDesc pace{};
	pace.name = SltString{paceName.data(), static_cast<int32_t>(paceName.size())};
	pace.nameHash = StringHash(paceName).GetValue();
	pace.size = 4;
	pace.constants = &wrong;
	pace.constantCount = 1;
	EXPECT_EQ(engine.registerManagedEnum(&pace), SLT_ERR_REFUSED);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Bare.Pace")), nullptr);
	EXPECT_EQ(_log.Delivered().Count("Rebuild the scripts"), 4u) << _log.Delivered().Joined();

	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Bare.Gear")), nullptr);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Bare.Tuning")), nullptr);
}

TEST_F(CSharpRuntimeFixture, DataAssetTypesAreChecked) {
	ASSERT_TRUE(Attach()) << _error;
	const SltEngineAPI engine = _hooks->makeEngineAPI();
	const auto describe = [](std::string_view name, std::string_view extension, uint64_t parent, uint32_t flags) {
		SltTypeDesc desc{};
		desc.name = SltString{name.data(), static_cast<int32_t>(name.size())};
		desc.nameHash = StringHash(name).GetValue();
		desc.parentHash = parent;
		desc.flags = flags;
		desc.assetMenu = SltString{"Bare/Note", 9};
		desc.assetExtension = SltString{extension.data(), static_cast<int32_t>(extension.size())};
		return desc;
	};
	const SltTypeDesc dotless = describe("Bare.Dotless", "note", 0, SLT_TYPE_DATA_ASSET);
	EXPECT_EQ(engine.registerManagedType(&dotless), SLT_ERR_REFUSED);
	const SltTypeDesc note = describe("Bare.Note", ".note", 0, SLT_TYPE_DATA_ASSET);
	ASSERT_EQ(engine.registerManagedType(&note), SLT_OK);
	const IAssetImporter* importer = AssetImporterRegistry::FindByExtension(".note");
	ASSERT_NE(importer, nullptr);
	EXPECT_EQ(importer->GetCreateMenuLabel(), "Bare/Note");
	EXPECT_EQ(importer->GetCreateDefaultFileName(), "New Note.note");
	const TypeInfo* registered = TypeRegistry::FindType(StringHash("Bare.Note"));
	ASSERT_NE(registered, nullptr);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(registered, DataAsset::GetReflectType()));
	EXPECT_EQ(registered->flags & TypeFlagComponent, 0u);
	const SltTypeDesc copy = describe("Bare.Copy", ".note", 0, SLT_TYPE_DATA_ASSET);
	EXPECT_EQ(engine.registerManagedType(&copy), SLT_ERR_REFUSED);
	const SltTypeDesc mixed = describe("Bare.Mixed", "", note.nameHash, 0);
	EXPECT_EQ(engine.registerManagedType(&mixed), SLT_ERR_REFUSED);
	const SltTypeDesc menuless = describe("Bare.Menuless", "", note.nameHash, SLT_TYPE_DATA_ASSET);
	EXPECT_EQ(engine.registerManagedType(&menuless), SLT_OK);
	EXPECT_EQ(_log.Delivered().Count("C# data asset 'Bare.Dotless' has extension 'note'. Use a dot + a name, like .level"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("C# data asset 'Bare.Copy' has extension .note (taken by another importer). Pick another extension"), 1u);
	EXPECT_EQ(_log.Delivered().Count("C# type 'Bare.Mixed' and its parent are not both data assets. Rebuild the scripts"), 1u);

	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(AssetImporterRegistry::FindByExtension(".note"), nullptr);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Bare.Note")), nullptr);
}

TEST_F(CSharpRuntimeFixture, DetachEndsLoadedDataAssets) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	Asset* soup = AssetDatabase::LoadAsset(Path("project://Assets/Soup.recipe"));
	ASSERT_NE(soup, nullptr);
	const u64 id = soup->GetInstanceId();
	SceneSystem::Terminate();

	_hooks->detach();
	EXPECT_EQ(ObjectRegistry::Resolve(id), nullptr);
	assets.reset();
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, ConformanceSuitePasses) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	ASSERT_NE(AssetDatabase::LoadAsset(Path("project://Assets/Dot.png")), nullptr);
	std::optional<CSharpConformance::InputRig> input(std::in_place, _hooks->setDriver);
	ASSERT_TRUE(input->IsReady());
	std::optional<CSharpConformance::UIRig> ui(std::in_place);
	ASSERT_TRUE(ui->IsReady());
	SceneSystem::CreateScene("World");

	const Time::FrameScope frame(CSharpConformance::MakeFrame());
	const SltManagedAPI& managed = _hooks->getManaged();
	const int32_t count = managed.getConformanceTestCount();
	int32_t ran = 0;
	for (int32_t index = 0; index < count; ++index) {
		SltString name{};
		ASSERT_EQ(managed.getConformanceTestName(index, &name), SLT_OK);
		const str caseName(View(name));
		if (!CSharpConformance::IsInPassSet(caseName)) {
			continue;
		}
		SltString message{};
		EXPECT_EQ(managed.runConformanceTest(index, &message), SLT_OK) << caseName << ": " << View(message);
		++ran;
	}
	EXPECT_GE(ran, CSharpConformance::PASS_SET_SIZE);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();

	SceneSystem::Terminate();
	ui.reset();
	input.reset();
	assets.reset();
	EXPECT_EQ(managed.unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(managed.getConformanceTestCount(), 0);
}

TEST_F(CSharpRuntimeFixture, ThrowingSceneHandlerIsReported) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	str message;
	ASSERT_EQ(Run("SceneProbe.ThrowsThenListens", message), SLT_OK) << message;
	_hooks->takeRecords();

	SceneSystem::CreateScene("Loud");
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by a scene handler"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"heard Loud"}));
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, ViewFieldCopiesEndAtUnload) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const size_t before = _hooks->countKeptFields();
	str message;
	ASSERT_EQ(Run("FieldProbe.LeavesAViewField", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->countKeptFields(), before + 1);
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(_hooks->countKeptFields(), 0u);
}

TEST_F(CSharpRuntimeFixture, ThrowingContinuationsAreReported) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	{
		const Time::FrameScope frame(CSharpConformance::MakeFrame());
		str message;
		EXPECT_EQ(Run("AwaitProbe.ThrowsInContinuations", message), SLT_OK) << message;
	}
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown after an await"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by a routine"), 1u);
	EXPECT_EQ(_log.Delivered().Count("Coroutine yielded Int32. Yield null, a Coroutines wait or an IEnumerator"), 1u);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"resumed after the throw"}));
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, HookKindsAreProfilerZones) {
#ifdef SHALLTEAR_CONFIG_RELEASE
	GTEST_SKIP() << "zones are compiled out of Release";
#endif
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");

	Profiler::SetEnabled(true);
	Profiler::NextFrame();
	str message;
	EXPECT_EQ(Run("ProfileProbe.RunsTwoScripts", message), SLT_OK) << message;
	Profiler::NextFrame();
	const Profiler::FrameResults results = Profiler::GetFrameResults();
	u32 zones = 0;
	u32 updates = 0;
	for (u32 index = 0; index < results.count; ++index) {
		const std::string_view name = results.zones[index].name;
		zones += name.starts_with("C# ") ? 1u : 0u;
		updates += name == "C# OnUpdate" ? results.zones[index].calls : 0u;
	}
	Profiler::SetEnabled(false);
	EXPECT_EQ(zones, 8u);
	EXPECT_EQ(updates, 6u);
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, ProfilerModuleListsHookCosts) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");
	static char owner = 0;
	ASSERT_TRUE(_hooks->registerProfilerModule(&owner));
	const ProfilerModule* module = ProfilerModules::Find(StringHash("CSharp"));
	ASSERT_NE(module, nullptr);
	EXPECT_EQ(module->title, "C# Scripts");

	std::vector<CSharpImpl::ScriptProfile::Row> rows;
	CSharpImpl::ScriptProfile::Collections collections;
	str message;
	ASSERT_EQ(Run("ProfileProbe.RunsTwoScripts", message), SLT_OK) << message;
	Profiler::SetEnabled(true);
	_hooks->pumpProfilerModule();
	_hooks->readProfile(rows, collections);
	EXPECT_TRUE(rows.empty());

	ASSERT_EQ(Run("ProfileProbe.RunsTwoScripts", message), SLT_OK) << message;
	_hooks->readProfile(rows, collections);
	const auto find = [&rows](std::string_view type) -> const CSharpImpl::ScriptProfile::Row* {
		for (const CSharpImpl::ScriptProfile::Row& row : rows) {
			if (row.type == type && row.hook == SLT_HOOK_UPDATE) {
				return &row;
			}
		}
		return nullptr;
	};
	const CSharpImpl::ScriptProfile::Row* hoarder = find("Shalltear.Conformance.Hoarder");
	const CSharpImpl::ScriptProfile::Row* idler = find("Shalltear.Conformance.Idler");
	ASSERT_NE(hoarder, nullptr);
	ASSERT_NE(idler, nullptr);
	EXPECT_EQ(hoarder->calls, 3u);
	EXPECT_GE(hoarder->bytes, 3000u);
	EXPECT_LT(hoarder->bytes, 4000u);
	EXPECT_EQ(idler->calls, 3u);
	EXPECT_EQ(idler->bytes, 0u);
	EXPECT_GT(collections.heapBytes, 0u);
	_hooks->readProfile(rows, collections);
	EXPECT_TRUE(rows.empty());

	ASSERT_EQ(Run("ProfileProbe.RunsTwoScripts", message), SLT_OK) << message;
	module->sample();
	UITest::MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {800, 600};
	{
		UI::Context context(desc, &fonts);
		const UI::ElementHandle host = context.CreateElement();
		context.AppendChild(context.GetRoot(), host);
		module->build(context, host);
		module->refresh(context, host);
		EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, host, "Shalltear.Conformance.Hoarder")));
		EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, host, "OnUpdate")));
		module->refresh(context, host);
		EXPECT_FALSE(context.IsAlive(UITest::FindByText(context, host, "Shalltear.Conformance.Hoarder")));
	}

	Profiler::SetEnabled(false);
	_hooks->pumpProfilerModule();
	ASSERT_EQ(Run("ProfileProbe.RunsTwoScripts", message), SLT_OK) << message;
	Profiler::SetEnabled(true);
	_hooks->pumpProfilerModule();
	_hooks->readProfile(rows, collections);
	EXPECT_TRUE(rows.empty());
	Profiler::SetEnabled(false);
	_hooks->pumpProfilerModule();
	ProfilerModules::UnregisterOwnedBy(&owner);
	_hooks->releaseProfilerModule();
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, ThrowingExportIsReported) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	str message;
	EXPECT_EQ(Run("FunctionProbe.CallsThrowingExport", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by an export"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"returned after the throw"}));
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, ThrowingPhysicsListenerIsReported) {
#ifdef SHALLTEAR_PHYSICS_BACKEND_STUB
	GTEST_SKIP();
#endif
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::InputRig> rig(std::in_place, _hooks->setDriver);
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	str message;
	ASSERT_EQ(Run("PhysicsProbe.ListenerThrows", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by a physics listener"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"second heard 1"}));
	SceneSystem::Terminate();
	rig.reset();
}

TEST_F(CSharpRuntimeFixture, ThrowingAnimationHandlerIsReported) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	str message;
	ASSERT_EQ(Run("AnimationProbe.HandlerThrows", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by an animation handler"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"heard 0.5"}));
	SceneSystem::Terminate();
	assets.reset();
}

TEST_F(CSharpRuntimeFixture, ThrowingUIHandlerIsReported) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	std::optional<CSharpConformance::InputRig> input(std::in_place, _hooks->setDriver);
	std::optional<CSharpConformance::UIRig> ui(std::in_place);
	ASSERT_TRUE(ui->IsReady());
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	str message;
	EXPECT_EQ(Run("UIProbe.HandlerThrows", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by a UI handler"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"heard MouseUp"}));
	SceneSystem::Terminate();
	ui.reset();
	input.reset();
	assets.reset();
}

TEST_F(CSharpRuntimeFixture, UIHandlerChangesApplyAtFrameEnd) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	std::optional<CSharpConformance::InputRig> input(std::in_place, _hooks->setDriver);
	std::optional<CSharpConformance::UIRig> ui(std::in_place);
	ASSERT_TRUE(ui->IsReady());
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();

	str message;
	EXPECT_EQ(Run("UIProbe.SwapsInsideAHandler", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"swapped"}));
	FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	EXPECT_EQ(Run("UIProbe.ClicksSwappersButton", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"heard Click"}));
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
	SceneSystem::Terminate();
	ui.reset();
	input.reset();
	assets.reset();
}

TEST_F(CSharpRuntimeFixture, ScriptsUnloadAfterTheirBridge) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");
	str message;
	ASSERT_EQ(Run("AnimationProbe.KeepsListening", message), SLT_OK) << message;

	ASSERT_TRUE(CSharpConformance::UnloadBridge("AnimationCSharp"));
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
	SceneSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, CamerasAreListed) {
	WindowSystem::Initialize();
	RenderSystem::Initialize();
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	SceneSystem::CreateScene("World");
	_hooks->takeRecords();
	str message;
	EXPECT_EQ(Run("RenderingProbe.ListsCameras", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"cameras 1 then 0"}));
	SceneSystem::Terminate();
	RenderSystem::Terminate();
	WindowSystem::Terminate();
}

TEST_F(CSharpRuntimeFixture, DebugLinesReachTheSceneView) {
	EditorGizmos::Initialize();
	char owner = 0;
	ASSERT_TRUE(_hooks->registerDebugLineGizmo(&owner));
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	Vector<DebugLine> lines;
	Vector<DebugLine> overlay;
	const auto draw = [&] {
		lines.clear();
		overlay.clear();
		GizmoDrawContext context;
		context.lines = &lines;
		context.overlayLines = &overlay;
		EditorGizmos::DrawSceneGizmos(context);
	};
	FrameTime frame = CSharpConformance::MakeFrame();
	str message;
	{
		const Time::FrameScope scope(frame);
		EXPECT_EQ(Run("RenderingProbe.DrawsLines", message), SLT_OK) << message;
		FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	}
	draw();
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(overlay.size(), 1u);
	if (lines.size() == 1 && overlay.size() == 1) {
		EXPECT_EQ(lines[0].a, Vec3(0.0f, 0.0f, 0.0f));
		EXPECT_EQ(lines[0].b, Vec3(1.0f, 2.0f, 3.0f));
		EXPECT_EQ(lines[0].color, Color32(255, 0, 0, 255));
		EXPECT_EQ(overlay[0].b, Vec3(0.0f, 1.0f, 0.0f));
		EXPECT_EQ(overlay[0].color, Color32(0, 255, 0, 255));
	}

	++frame.frameCount;
	{
		const Time::FrameScope scope(frame);
		FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	}
	draw();
	EXPECT_EQ(lines.size(), 0u);
	EXPECT_EQ(overlay.size(), 1u);

	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	++frame.frameCount;
	{
		const Time::FrameScope scope(frame);
		FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
	}
	draw();
	EXPECT_EQ(lines.size(), 0u);
	EXPECT_EQ(overlay.size(), 0u);
	EXPECT_NE(Run("RenderingProbe.CountsDrawnLines", message), SLT_OK);
	EXPECT_EQ(message, "lines 2");
	EditorGizmos::UnregisterOwnedBy(&owner);
	EditorGizmos::Terminate();
}

TEST_F(CSharpRuntimeFixture, ScriptsNeedTheirPackagesBindings) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(CSharpConformance::UnloadBridge("Physics3DCSharp"));
	EXPECT_FALSE(LoadConformance());
	EXPECT_EQ(_log.Delivered().Count("Shalltear.Physics3D has no bindings in this engine. Enable the package"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->getManaged().getConformanceTestCount(), 0);
}

TEST_F(CSharpRuntimeFixture, DroppedInputMapsFreedAtTick) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const i32 before = _hooks->getInputMapCount();
	str message;
	ASSERT_EQ(Run("InputProbe.DropsMaps", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->getInputMapCount(), before + 20);
	_hooks->getManaged().tick();
	EXPECT_EQ(_hooks->getInputMapCount(), before);
}

TEST_F(CSharpRuntimeFixture, SecondLoadIsRefused) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const int32_t count = _hooks->getManaged().getConformanceTestCount();
	ASSERT_GT(count, 0);

	EXPECT_FALSE(LoadConformance());
	EXPECT_EQ(_log.Delivered().Count("user assemblies are already loaded"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->getManaged().getConformanceTestCount(), count);

	str message;
	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, FailedCaseCarriesItsMessage) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.Fails", message), CASE_FAILED);
	EXPECT_EQ(message, "failed as asked");
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, ManagedExceptionIsReported) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.ThrowsOnCallingThread", message), MANAGED_FAILED);
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown on the calling thread"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("ThrowsOnCallingThread"), 1u) << _log.Delivered().Joined();

	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, ThreadExceptionIsReported) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.ThrowsOnStartedThread", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown on a started thread"), 1u) << _log.Delivered().Joined();

	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, NativeExceptionBecomesErrorCode) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	SltEngineAPI engine = _hooks->makeEngineAPI();
	ASSERT_NE(engine.throwForTest, nullptr);
	EXPECT_EQ(engine.throwForTest(), SLT_ERR_EXCEPTION);
	SltString text{};
	engine.getLastError(&text);
	EXPECT_EQ(View(text), "thrown for a test");

	str message;
	EXPECT_EQ(Run("BridgeTests.NativeThrowBecomesEngineException", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, MissingAssemblyIsReported) {
	ASSERT_TRUE(Attach()) << _error;

	const str path = "Z:/no/such/Scripts.dll";
	const SltString paths[] = {SltString{path.data(), static_cast<int32_t>(path.size())}};
	EXPECT_EQ(_hooks->getManaged().loadUserAssemblies(paths, 1), MANAGED_FAILED);
	EXPECT_GE(_log.Delivered().Count("Scripts.dll"), 1u) << _log.Delivered().Joined();

	EXPECT_TRUE(LoadConformance()) << _log.Delivered().Joined();
}

#endif
