#include "TestsPCH.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>

using namespace Shalltear;

namespace {

	constexpr const char* V1_DLL = "ShalltearHotReloadModuleV1" SLT_TEST_DLL_SUFFIX;
	constexpr const char* V2_DLL = "ShalltearHotReloadModuleV2" SLT_TEST_DLL_SUFFIX;
	constexpr const char* BAD_DLL = "ShalltearHotReloadModuleBad" SLT_TEST_DLL_SUFFIX;
	constexpr const char* GONE_DLL = "ShalltearHotReloadModuleGone" SLT_TEST_DLL_SUFFIX;

	const StringHash BehaviorTypeName{"HotReloadBehavior"};

	str HexOfBytes(std::string_view bytes) {
		str out;
		for (unsigned char byte : bytes) {
			out += std::format("{:02x}", byte);
		}
		return out;
	}

	class HrHostComponent : public Component {
	public:
		i32 hostValue = 7;

		SLT_REFLECT_COMPONENT(HrHostComponent, Component)
		SLT_REFLECT_FIELD(hostValue)
		SLT_END_REFLECT()

		HrHostComponent() = default;
	};

	SLT_REGISTER_TYPE(HrHostComponent)

	class HotReloadFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		size_t _sceneBytesBaseline = 0;
		Handle<Scene> _scene;
		ModuleHandle _module;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_hotreload_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();

			SceneSystem::Initialize();
			SceneSystem::Terminate();
			_sceneBytesBaseline = MemoryManager::GetBytesForTag(MemTag::Scene);
			SceneSystem::Initialize();
			ModuleSystem::Initialize();

			_scene = SceneSystem::CreateScene("HotReloadTest");
			_module = ModuleSystem::Load(Path(V1_DLL));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
		}

		void TearDown() override {
			SceneSystem::Terminate();
			ModuleSystem::Terminate();
			AssetDatabase::CollectGarbage();
			EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scene), _sceneBytesBaseline);
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		const TypeInfo* BehaviorType() {
			return TypeRegistry::FindType(BehaviorTypeName);
		}

		Handle<Component> AddBehavior(Handle<SceneObject> owner) {
			const TypeInfo* type = BehaviorType();
			EXPECT_NE(type, nullptr);
			return owner.Get()->AddComponent(type);
		}

		template <typename T>
		void Set(Handle<Component> component, StringHash field, const T& value, const TypeInfo* type = nullptr) {
			Variant boxed{value};
			ASSERT_TRUE(SetField(component.Get(), type ? type : BehaviorType(), field, &boxed));
		}

		template <typename T>
		T Get(Handle<Component> component, StringHash field, const TypeInfo* type = nullptr) {
			Variant out;
			EXPECT_TRUE(GetField(component.Get(), type ? type : BehaviorType(), field, &out)) << std::format("{:016x}", field.GetValue());
			const T* value = out.TryGet<T>();
			return value ? *value : T{};
		}

		i32 Magic(Handle<Component> component) {
			Variant ret;
			EXPECT_TRUE(InvokeMethod(component.Get(), BehaviorType(), StringHash("GetMagic"), nullptr, 0, &ret));
			const i32* value = ret.TryGet<i32>();
			return value ? *value : -1;
		}
	};

} // anonymous namespace

TEST_F(HotReloadFixture, StatePreservedAcrossEditAndRebuild) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<SceneObject> beta = SceneSystem::CreateObject("Beta", _scene);
	Handle<Component> hostComp = alpha.Get()->AddComponent(TypeRegistry::FindType(StringHash("HrHostComponent")));
	Handle<Component> mover = AddBehavior(alpha);
	Handle<Component> buddy = AddBehavior(beta);

	Set<i32>(mover, StringHash("counter"), 42);
	Set<str>(mover, StringHash("label"), "hello");
	Set<f32>(mover, StringHash("speed"), 3.5f);
	Set<Handle<SceneObject>>(mover, StringHash("target"), beta);
	Set<Handle<Component>>(mover, StringHash("buddy"), buddy);
	EXPECT_EQ(Magic(mover), 1);
	i32 disablesBefore = Get<i32>(mover, StringHash("disableCount"));
	i32 enablesBefore = Get<i32>(mover, StringHash("enableCount"));

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	ASSERT_TRUE(mover.IsValid());
	ASSERT_TRUE(buddy.IsValid());
	EXPECT_EQ(alpha.Get()->GetComponentCount(), 2u);
	EXPECT_EQ(alpha.Get()->GetComponentAt(0).Get(), hostComp.Get());
	EXPECT_EQ(alpha.Get()->GetComponentAt(1).Get(), mover.Get());
	EXPECT_EQ(Get<i32>(hostComp, StringHash("hostValue"), TypeRegistry::FindType(StringHash("HrHostComponent"))), 7);

	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), 42);
	EXPECT_EQ(Get<str>(mover, StringHash("label")), "hello");
	EXPECT_EQ(FindField(BehaviorType(), StringHash("speed")), nullptr);
	EXPECT_EQ(Get<Vec3>(mover, StringHash("added")), Vec3{});

	EXPECT_EQ(Get<Handle<SceneObject>>(mover, StringHash("target")).Get(), beta.Get());
	EXPECT_EQ(Get<Handle<Component>>(mover, StringHash("buddy")).Get(), buddy.Get());

	EXPECT_EQ(Magic(mover), 2);

	EXPECT_EQ(Get<i32>(mover, StringHash("disableCount")), disablesBefore + 1);
	EXPECT_EQ(Get<i32>(mover, StringHash("enableCount")), enablesBefore + 1);

	i32 counterBeforeTick = Get<i32>(mover, StringHash("counter"));
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();
	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), counterBeforeTick + 1);
}

TEST_F(HotReloadFixture, StoppedReloadMigratesWithoutHooks) {
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	SceneSystem::SetPlaying(false);
	Handle<Scene> stopped = SceneSystem::CreateScene("Stopped");
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", stopped);
	Handle<Component> mover = AddBehavior(alpha);
	Set<i32>(mover, StringHash("counter"), 42);
	ASSERT_EQ(Get<i32>(mover, StringHash("enableCount")), 0);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	ASSERT_TRUE(mover.IsValid());
	EXPECT_EQ(Magic(mover), 2);
	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), 42);
	EXPECT_EQ(Get<i32>(mover, StringHash("disableCount")), 0);
	EXPECT_EQ(Get<i32>(mover, StringHash("enableCount")), 0);
}

TEST_F(HotReloadFixture, BackToV1RestoresDefaults) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<str>(mover, StringHash("label"), "round-trip");
	Set<f32>(mover, StringHash("speed"), 9.0f);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));
	EXPECT_EQ(Magic(mover), 2);

	Path v1{V1_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));
	EXPECT_EQ(Magic(mover), 1);
	EXPECT_EQ(Get<str>(mover, StringHash("label")), "round-trip");
	EXPECT_EQ(Get<f32>(mover, StringHash("speed")), 1.0f);
}

TEST_F(HotReloadFixture, BadBinaryRollsBackWithStateIntact) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<i32>(mover, StringHash("counter"), 1234);

	Path bad{BAD_DLL};
	EXPECT_FALSE(ModuleSystem::Reload(_module, &bad));

	ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
	ASSERT_TRUE(mover.IsValid());
	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), 1234);
	EXPECT_EQ(Magic(mover), 1);
}

TEST_F(HotReloadFixture, OnDisableDestroyDropsCleanly) {
	Handle<SceneObject> doomed = SceneSystem::CreateObject("Doomed", _scene);
	Handle<Component> poison = AddBehavior(doomed);
	Set<str>(poison, StringHash("label"), "destroy-owner-on-disable");

	Handle<SceneObject> survivor = SceneSystem::CreateObject("Survivor", _scene);
	Handle<Component> normal = AddBehavior(survivor);
	Set<i32>(normal, StringHash("counter"), 77);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	EXPECT_FALSE(doomed.IsValid());
	EXPECT_FALSE(poison.IsValid());
	ASSERT_TRUE(normal.IsValid());
	EXPECT_EQ(Get<i32>(normal, StringHash("counter")), 77);
	EXPECT_EQ(Magic(normal), 2);
}

TEST_F(HotReloadFixture, PrefabInstanceSurvivesReload) {
	Handle<SceneObject> root = SceneSystem::CreateObject("Unit", _scene);
	Handle<Component> mover = AddBehavior(root);
	Set<str>(mover, StringHash("label"), "prefab-sourced");
	Guid prefabId = SceneSystem::SavePrefab(root, Path("project://hr_unit.prefab"));
	ASSERT_NE(prefabId, Guid{});

	u64 moverIdBefore = mover.GetInstanceId();

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	ASSERT_TRUE(mover.IsValid());
	EXPECT_EQ(mover.GetInstanceId(), moverIdBefore);
	EXPECT_EQ(Get<str>(mover, StringHash("label")), "prefab-sourced");

	SceneSystem::NotifyPrefabChanged(prefabId);
	SceneSystem::EndFrame();

	ASSERT_TRUE(root.IsValid());
	u32 behaviorSlots = 0;
	for (u32 i = 0; i < root.Get()->GetComponentCount(); ++i) {
		Handle<Component> at = root.Get()->GetComponentAt(i);
		if (at.Get() && TypeRegistry::IsDerivedFrom(TypeRegistry::FindType(BehaviorTypeName), Component::GetReflectType()) &&
			at.Get()->GetInstanceId() == mover.GetInstanceId()) {
			++behaviorSlots;
		}
	}
	EXPECT_EQ(root.Get()->GetComponentCount(), 1u);
	EXPECT_EQ(behaviorSlots, 1u);
	EXPECT_TRUE(mover.IsValid());
}

TEST_F(HotReloadFixture, RepeatedReloadCyclesAreLeakFree) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<str>(mover, StringHash("label"), "cycler");

	Path v1{V1_DLL};
	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));
	SceneSystem::EndFrame();
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));
	SceneSystem::EndFrame();
	size_t sceneBytes = MemoryManager::GetBytesForTag(MemTag::Scene);
	size_t coreBytes = MemoryManager::GetBytesForTag(MemTag::Core);

	for (int cycle = 0; cycle < 3; ++cycle) {
		ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));
		SceneSystem::EndFrame();
		ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));
		SceneSystem::EndFrame();
	}

	ASSERT_TRUE(mover.IsValid());
	EXPECT_EQ(Get<str>(mover, StringHash("label")), "cycler");
	EXPECT_EQ(Magic(mover), 1);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scene), sceneBytes);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Core), coreBytes);
}

TEST_F(HotReloadFixture, ComponentlessReloadSwapsCode) {
	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));
	ASSERT_NE(TypeRegistry::FindType(BehaviorTypeName), nullptr);

	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	EXPECT_EQ(Magic(mover), 2);
}

namespace {

	class UnloadingRecorder : public Object {
	public:
		std::vector<const void*> images;
		void OnImageUnloading(const ImageUnloadingEvent& event) { images.push_back(event.image); }
	};

} // anonymous namespace

TEST_F(HotReloadFixture, ReloadAnnouncesReplacedImage) {
	auto recorder = MakeShared<UnloadingRecorder>();
	ModuleEvents::ImageUnloading.Subscribe(EventBus::GetGlobal(), Handle<UnloadingRecorder>(recorder.get()),
										   &UnloadingRecorder::OnImageUnloading);
	const void* before = ModuleSystem::GetNativeHandle(_module);
	Path v2{V2_DLL};
	const bool reloaded = ModuleSystem::Reload(_module, &v2);
	EventBus::GetGlobal().UnsubscribeAll(recorder->GetInstanceId());

	ASSERT_TRUE(reloaded);
	ASSERT_EQ(recorder->images.size(), 1u);
	EXPECT_EQ(recorder->images[0], before);
}

TEST_F(HotReloadFixture, PlaySnapshotSurvivesReload) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<str>(mover, StringHash("label"), "played");
	Set<f32>(mover, StringHash("speed"), 8.0f);

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());
	Handle<SceneObject> restoredAlpha = restored.Get()->FindObjectByName("Alpha");
	ASSERT_TRUE(restoredAlpha.IsValid());
	ASSERT_EQ(restoredAlpha.Get()->GetComponentCount(), 1u);
	Handle<Component> restoredMover = restoredAlpha.Get()->GetComponentAt(0);

	EXPECT_EQ(Get<str>(restoredMover, StringHash("label")), "played");
	EXPECT_EQ(Magic(restoredMover), 2);
	EXPECT_EQ(FindField(BehaviorType(), StringHash("speed")), nullptr);
}

TEST_F(HotReloadFixture, ModuleEpochSeparatesTypeFromReplacement) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);

	const TypeInfo* before = BehaviorType();
	ASSERT_NE(before, nullptr);
	const u32 epochBefore = before->moduleEpoch;
	const u64 layoutBefore = before->layoutHash;
	const u64 nameBefore = before->name.GetValue();
	const u32 fieldsBefore = before->fieldCount;
	const u64 instanceBefore = mover.GetInstanceId();

	Path v1{V1_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));

	const TypeInfo* after = BehaviorType();
	ASSERT_NE(after, nullptr);

	EXPECT_EQ(after->name.GetValue(), nameBefore);
	EXPECT_EQ(after->layoutHash, layoutBefore);
	EXPECT_EQ(after->fieldCount, fieldsBefore);
	EXPECT_EQ(mover.GetInstanceId(), instanceBefore);

	EXPECT_GT(after->moduleEpoch, epochBefore);
}

TEST_F(HotReloadFixture, ExportedFunctionRebindsAcrossReload) {
	ScriptFunction<i32()> build = ScriptFunctions::Find<i32()>("HrBuildNumber");
	ASSERT_TRUE(build.IsBound());
	EXPECT_EQ(build(), 1);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));
	EXPECT_EQ(build(), 2);

	ASSERT_TRUE(ModuleSystem::Unload(_module));
	EXPECT_FALSE(build.IsBound());
	EXPECT_EQ(build(), 0);
}

TEST_F(HotReloadFixture, FieldRefResolvesAcrossReload) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<i32>(mover, StringHash("counter"), 11);

	FieldRef ref{BehaviorTypeName, StringHash("counter")};
	const FieldInfo* first = ref.Resolve();
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(ref.Resolve(), first);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	const FieldInfo* after = ref.Resolve();
	ASSERT_NE(after, nullptr);

	ASSERT_NE(after->ownerType, nullptr);
	EXPECT_GT(after->ownerType->moduleEpoch, 0u);
	EXPECT_EQ(after, FindField(BehaviorType(), StringHash("counter")));

	ASSERT_NE(after->getFn, nullptr);
	Variant value;
	after->getFn(mover.Get(), &value);
	EXPECT_EQ(value.As<i32>(), 11);
}

TEST_F(HotReloadFixture, DroppedFieldRefIsNull) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	AddBehavior(alpha);

	FieldRef ref{BehaviorTypeName, StringHash("speed")};
	ASSERT_NE(ref.Resolve(), nullptr);

	Path v2{V2_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v2));

	EXPECT_EQ(ref.Resolve(), nullptr);
	EXPECT_EQ(FindField(BehaviorType(), StringHash("speed")), nullptr);
}

TEST_F(HotReloadFixture, LostTypeFieldRefIsNull) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	AddBehavior(alpha);

	FieldRef ref{BehaviorTypeName, StringHash("counter")};
	ASSERT_NE(ref.Resolve(), nullptr);

	Path gone{GONE_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &gone));

	EXPECT_EQ(ref.Resolve(), nullptr);
	EXPECT_EQ(TypeRegistry::FindType(BehaviorTypeName), nullptr);
}

TEST_F(HotReloadFixture, DeletingTypeLeavesRowsPreservedInPlace) {
	const TypeInfo* hostType = TypeRegistry::FindType(StringHash("HrHostComponent"));
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<SceneObject> beta = SceneSystem::CreateObject("Beta", _scene);

	Handle<Component> head = alpha.Get()->AddComponent(hostType);
	Handle<Component> mover = AddBehavior(alpha);
	Handle<Component> tail = alpha.Get()->AddComponent(hostType);
	Handle<Component> second = AddBehavior(beta);
	Set<i32>(mover, StringHash("counter"), 42);
	Set<str>(mover, StringHash("label"), "hello");
	Set<Handle<SceneObject>>(mover, StringHash("target"), beta);

	Path gone{GONE_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &gone));
	ASSERT_EQ(TypeRegistry::FindType(BehaviorTypeName), nullptr);

	ASSERT_EQ(alpha.Get()->GetComponentCount(), 3u);
	EXPECT_EQ(alpha.Get()->GetComponentAt(0).Get(), head.Get());
	EXPECT_EQ(alpha.Get()->GetComponentAt(2).Get(), tail.Get());
	EXPECT_EQ(Get<i32>(head, StringHash("hostValue"), TypeRegistry::FindType(StringHash("HrHostComponent"))), 7);

	ASSERT_TRUE(mover.IsValid());
	ASSERT_TRUE(second.IsValid());
	Component* preserved = alpha.Get()->GetComponentAt(1).Get();
	EXPECT_EQ(preserved, mover.Get());
	ASSERT_EQ(preserved->GetType(), UnresolvedComponent::GetReflectType());
	EXPECT_EQ(static_cast<UnresolvedComponent*>(preserved)->GetUnresolvedType(), BehaviorTypeName);
	EXPECT_EQ(second.Get()->GetType(), UnresolvedComponent::GetReflectType());

	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();
	ASSERT_TRUE(mover.IsValid());

	ASSERT_TRUE(SceneSystem::SaveScene(_scene, Path("project://deleted.scene")));
	str text = FileSystem::ReadTextFile(FileSystem::Resolve(Path("project://deleted.scene")), MemTag::Scene);
	EXPECT_NE(text.find(std::format("{:016x}", BehaviorTypeName.GetValue())), str::npos);
	EXPECT_EQ(text.find(std::format("{:016x}", StringHash("UnresolvedComponent").GetValue())), str::npos);
	EXPECT_EQ(text.find("UnresolvedComponent"), str::npos);
	EXPECT_NE(text.find(HexOfBytes("hello")), str::npos);
}

TEST_F(HotReloadFixture, RestoringTypeRebuildsPreservedRowsInPlace) {
	const TypeInfo* hostType = TypeRegistry::FindType(StringHash("HrHostComponent"));
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<SceneObject> beta = SceneSystem::CreateObject("Beta", _scene);
	Handle<Component> head = alpha.Get()->AddComponent(hostType);
	Handle<Component> mover = AddBehavior(alpha);
	Handle<Component> buddy = AddBehavior(beta);
	Set<i32>(mover, StringHash("counter"), 42);
	Set<str>(mover, StringHash("label"), "hello");
	Set<Handle<SceneObject>>(mover, StringHash("target"), beta);
	Set<Handle<Component>>(mover, StringHash("buddy"), buddy);
	const u64 moverId = mover.GetInstanceId();

	Path gone{GONE_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &gone));
	ASSERT_EQ(mover.Get()->GetType(), UnresolvedComponent::GetReflectType());

	Path v1{V1_DLL};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));

	ASSERT_EQ(alpha.Get()->GetComponentCount(), 2u);
	EXPECT_EQ(alpha.Get()->GetComponentAt(0).Get(), head.Get());
	ASSERT_TRUE(mover.IsValid());
	EXPECT_EQ(mover.GetInstanceId(), moverId);
	ASSERT_EQ(mover.Get()->GetType(), BehaviorType());

	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), 42);
	EXPECT_EQ(Get<str>(mover, StringHash("label")), "hello");
	EXPECT_EQ(Get<Handle<SceneObject>>(mover, StringHash("target")).Get(), beta.Get());
	EXPECT_EQ(Get<Handle<Component>>(mover, StringHash("buddy")).Get(), buddy.Get());
	EXPECT_EQ(Magic(mover), 1);

	i32 before = Get<i32>(mover, StringHash("counter"));
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();
	EXPECT_EQ(Get<i32>(mover, StringHash("counter")), before + 1);
}

TEST_F(HotReloadFixture, ModuleReclaimsSavedRows) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	Handle<Component> mover = AddBehavior(alpha);
	Set<i32>(mover, StringHash("counter"), 7);
	Set<str>(mover, StringHash("label"), "saved");
	ASSERT_TRUE(SceneSystem::SaveScene(_scene, Path("project://carried.scene")));

	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	ASSERT_TRUE(ModuleSystem::Unload(_module));
	ASSERT_EQ(TypeRegistry::FindType(BehaviorTypeName), nullptr);

	Handle<Scene> opened = SceneSystem::LoadScene(Path("project://carried.scene"));
	ASSERT_TRUE(opened.IsValid());
	_scene = opened;
	Handle<SceneObject> subject = opened->FindObjectByName("Alpha");
	ASSERT_TRUE(subject.IsValid());
	ASSERT_EQ(subject->GetComponentCount(), 1u);
	Handle<Component> preserved = subject->GetComponentAt(0);
	ASSERT_EQ(preserved.Get()->GetType(), UnresolvedComponent::GetReflectType());

	_module = ModuleSystem::Load(Path(V1_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(_module));

	ASSERT_EQ(subject->GetComponentCount(), 1u);
	ASSERT_TRUE(preserved.IsValid());
	ASSERT_EQ(preserved.Get()->GetType(), BehaviorType());
	EXPECT_EQ(Get<i32>(preserved, StringHash("counter")), 7);
	EXPECT_EQ(Get<str>(preserved, StringHash("label")), "saved");

	i32 before = Get<i32>(preserved, StringHash("counter"));
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();
	EXPECT_EQ(Get<i32>(preserved, StringHash("counter")), before + 1);
}

namespace {

	class SceneWarningCapture : public ILogSink {
	public:
		void ReceiveLog(const LogEvent& event) override {
			if (event.level == LogLevel::Warn && event.category == "Scene") {
				std::lock_guard lock(_mutex);
				_messages.push_back(event.message);
			}
		}
		void Flush() override {}

		u32 CountHolding(std::string_view needle) const {
			std::lock_guard lock(_mutex);
			return static_cast<u32>(std::count_if(_messages.begin(), _messages.end(),
												  [needle](const str& message) { return message.find(needle) != str::npos; }));
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
	};

} // namespace

TEST_F(HotReloadFixture, HeldReportsNameOnlyTypesStillMissing) {
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", _scene);
	AddBehavior(alpha);
	ASSERT_TRUE(SceneSystem::SaveScene(_scene, Path("project://held.scene")));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	ASSERT_TRUE(ModuleSystem::Unload(_module));
	ASSERT_EQ(TypeRegistry::FindType(BehaviorTypeName), nullptr);

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto capture = std::make_shared<SceneWarningCapture>();
	LogManager::AddSink(capture);
	const str hex = std::format("{:#018x}", BehaviorTypeName.GetValue());
	auto loadAndCook = [&] {
		_scene = SceneSystem::LoadScene(Path("project://held.scene"));
		ASSERT_TRUE(_scene.IsValid());
		auto* asset = static_cast<SceneAsset*>(AssetDatabase::LoadAsset(Path("project://held.scene")));
		ASSERT_NE(asset, nullptr);
		Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
		Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
		PakStringPool pool;
		CookWriter descriptor(descriptorBytes, pool);
		CookWriter payload(payloadBytes, pool);
		EXPECT_TRUE(asset->Cook(descriptor, payload));
		AssetDatabase::UnloadAsset(asset);
	};

	SceneSystem::HoldUnresolvedComponentReports();
	loadAndCook();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 0u);

	_module = ModuleSystem::Load(Path(V1_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
	SceneSystem::ReleaseUnresolvedComponentReports();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 0u);

	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	ASSERT_TRUE(ModuleSystem::Unload(_module));
	SceneSystem::HoldUnresolvedComponentReports();
	loadAndCook();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 0u);

	SceneSystem::ReleaseUnresolvedComponentReports();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding("'held' loaded with 1 component row(s) of 1 unloaded type(s)"), 1u);
	EXPECT_EQ(capture->CountHolding("converting a document"), 1u);
	EXPECT_EQ(capture->CountHolding(hex), 2u);

	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	SceneSystem::HoldUnresolvedComponentReports();
	SceneSystem::HoldUnresolvedComponentReports();
	loadAndCook();
	SceneSystem::ReleaseUnresolvedComponentReports();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 2u);
	SceneSystem::ReleaseUnresolvedComponentReports();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 4u);
	SceneSystem::ReleaseUnresolvedComponentReports();
	LogManager::DrainBeforeImageUnload();
	EXPECT_EQ(capture->CountHolding(hex), 4u);

	LogManager::RemoveSink(capture);
	LogManager::Terminate();
	JobSystem::Terminate();
}
