#include "TestsPCH.h"

#include "DeviceLossHarness.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleSystem.h"
#include "Shalltear/Core/Module/TypeReplacement.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"

#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;

namespace {

	char _swapToken = 0;
	char _otherToken = 0;
	char _unknownBase = 0;

	class SwapProbe final : public Behavior {
	public:
		const TypeInfo* GetType() const override { return type; }

		const TypeInfo* type = nullptr;
		f32 speed = 2.5f;
		i32 count = 3;
		i32 bonus = 5;
		str label = "fresh";
		Handle<SceneObject> target;
	};

	Object* CreateSwapProbe(const TypeInfo* type, MemTag tag) {
		SwapProbe* probe = New<SwapProbe>(tag);
		probe->type = type;
		return probe;
	}

	template <typename T>
	void AddProbeField(ReflectBuilder& builder, const char* name, void* (*address)(void*)) {
		builder.AddField(StringHash(name), name, GetStaticType<T>(), sizeof(T), alignof(T), nullptr, nullptr, address, FieldFlagDefault, {});
	}

	const TypeInfo* RegisterProbe(const char* name, const void* token, bool secondShape) {
		ReflectBuilder builder(StringHash(name), sizeof(SwapProbe), alignof(SwapProbe));
		builder.SetDisplayName(name);
		builder.SetParent(Behavior::GetReflectType());
		builder.SetFlags(TypeFlagComponent);
		builder.SetCreateTypedFn(&CreateSwapProbe);
		AddProbeField<str>(builder, "label", +[](void* instance) -> void* { return &static_cast<SwapProbe*>(instance)->label; });
		if (secondShape) {
			AddProbeField<i32>(builder, "bonus", +[](void* instance) -> void* { return &static_cast<SwapProbe*>(instance)->bonus; });
		} else {
			AddProbeField<i32>(builder, "count", +[](void* instance) -> void* { return &static_cast<SwapProbe*>(instance)->count; });
		}
		AddProbeField<f32>(builder, "speed", +[](void* instance) -> void* { return &static_cast<SwapProbe*>(instance)->speed; });
		AddProbeField<Handle<SceneObject>>(builder, "target", +[](void* instance) -> void* { return &static_cast<SwapProbe*>(instance)->target; });
		return builder.FinalizeOwned(token);
	}

	struct Swap {
		bool registersSecondShape = true;
		bool result = true;
		u32 calls = 0;
		u32 aliveInside = 0;
	};

	bool RunSwap(void* user) {
		Swap& swap = *static_cast<Swap*>(user);
		++swap.calls;
		TypeRegistry::UnregisterOwnedBy(&_swapToken);
		if (swap.registersSecondShape) {
			RegisterProbe("SwapProbe", &_swapToken, true);
		}
		return swap.result;
	}

	class TypeReplacementTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		Handle<Scene> _scene;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() / ("shalltear_replace_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			TypeRegistry::InitializePrimitives();
			_scene = SceneSystem::CreateScene("Swap");
		}

		void TearDown() override {
			SceneSystem::Terminate();
			TypeRegistry::UnregisterOwnedBy(&_swapToken);
			TypeRegistry::UnregisterOwnedBy(&_otherToken);
			AssetDatabase::CollectGarbage();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		static SwapProbe* ProbeOf(Handle<SceneObject> object, const char* typeName = "SwapProbe") {
			for (u32 index = 0; index < object->GetComponentCount(); ++index) {
				Component* component = object->GetComponentAt(index).Get();
				if (component && component->GetType()->name == StringHash(typeName)) {
					return static_cast<SwapProbe*>(component);
				}
			}
			return nullptr;
		}
	};

} // anonymous namespace

TEST_F(TypeReplacementTest, InstanceIdsSurvive) {
	const TypeInfo* first = RegisterProbe("SwapProbe", &_swapToken, false);
	const TypeInfo* bystander = RegisterProbe("SwapBystander", &_otherToken, false);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(bystander, nullptr);
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _scene);
	Handle<SceneObject> aim = SceneSystem::CreateObject("Aim", _scene);
	Handle<Component> probe = holder->AddComponent(first);
	Handle<Component> other = holder->AddComponent(bystander);
	Handle<Component> far = aim->AddComponent(first);
	ASSERT_TRUE(probe.IsValid());
	SwapProbe* before = static_cast<SwapProbe*>(probe.Get());
	before->speed = 7.5f;
	before->count = 9;
	before->label = "kept";
	before->target = aim;
	static_cast<SwapProbe*>(far.Get())->speed = 1.5f;
	const u64 id = probe.GetInstanceId();
	const Component* otherBefore = other.Get();

	Swap swap;
	ASSERT_TRUE(TypeReplacement::Replace(&_swapToken, &RunSwap, &swap));
	EXPECT_EQ(swap.calls, 1u);

	ASSERT_TRUE(probe.IsValid());
	EXPECT_EQ(probe.GetInstanceId(), id);
	const TypeInfo* second = TypeRegistry::FindType(StringHash("SwapProbe"));
	ASSERT_NE(second, nullptr);
	const SwapProbe* after = static_cast<SwapProbe*>(probe.Get());
	EXPECT_EQ(after->GetType(), second);
	EXPECT_NE(FindField(second, "bonus"_hash), nullptr);
	EXPECT_EQ(FindField(second, "count"_hash), nullptr);
	EXPECT_FLOAT_EQ(after->speed, 7.5f);
	EXPECT_EQ(after->label, "kept");
	EXPECT_EQ(after->target, aim);
	EXPECT_EQ(after->bonus, 5);
	EXPECT_EQ(after->count, 3);
	ASSERT_TRUE(far.IsValid());
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(far.Get())->speed, 1.5f);

	EXPECT_EQ(other.Get(), otherBefore);
	ASSERT_EQ(holder->GetComponentCount(), 2u);
	EXPECT_EQ(holder->GetComponentAt(0), probe);
	EXPECT_EQ(holder->GetComponentAt(1), other);

	ASSERT_TRUE(TypeReplacement::Replace(&_swapToken, &RunSwap, &swap));
	EXPECT_EQ(probe.GetInstanceId(), id);
	ASSERT_TRUE(probe.IsValid());
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(probe.Get())->speed, 7.5f);
	EXPECT_EQ(static_cast<SwapProbe*>(probe.Get())->label, "kept");
}

TEST_F(TypeReplacementTest, FailedSwapLeavesUnresolvedComponents) {
	const TypeInfo* first = RegisterProbe("SwapProbe", &_swapToken, false);
	ASSERT_NE(first, nullptr);
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _scene);
	Handle<Component> probe = holder->AddComponent(first);
	static_cast<SwapProbe*>(probe.Get())->speed = 7.5f;
	static_cast<SwapProbe*>(probe.Get())->label = "kept";

	Swap gone;
	gone.registersSecondShape = false;
	gone.result = false;
	EXPECT_FALSE(TypeReplacement::Replace(&_swapToken, &RunSwap, &gone));
	EXPECT_EQ(gone.calls, 1u);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("SwapProbe")), nullptr);
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	const Component* preserved = holder->GetComponentAt(0).Get();
	ASSERT_NE(preserved, nullptr);
	ASSERT_EQ(preserved->GetType(), UnresolvedComponent::GetReflectType());
	EXPECT_EQ(static_cast<const UnresolvedComponent*>(preserved)->GetUnresolvedType(), StringHash("SwapProbe"));

	Swap back;
	ASSERT_TRUE(TypeReplacement::Replace(&_swapToken, &RunSwap, &back));
	const SwapProbe* restored = ProbeOf(holder);
	ASSERT_NE(restored, nullptr);
	EXPECT_FLOAT_EQ(restored->speed, 7.5f);
	EXPECT_EQ(restored->label, "kept");
	EXPECT_EQ(holder->GetComponentCount(), 1u);
}

TEST_F(TypeReplacementTest, TerminateForgetsEveryReportHold) {
	DeviceLossTest::ScopedLog log;
	const TypeInfo* first = RegisterProbe("SwapProbe", &_swapToken, false);
	ASSERT_NE(first, nullptr);
	SceneSystem::CreateObject("Holder", _scene)->AddComponent(first);
	Buffer saved;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, saved));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	TypeRegistry::UnregisterOwnedBy(&_swapToken);

	SceneSystem::HoldUnresolvedComponentReports();
	SceneSystem::HoldUnresolvedComponentReports();
	ASSERT_TRUE(SceneSystem::LoadSceneFromMemory(saved).IsValid());
	EXPECT_EQ(log.Delivered().Count("unloaded type(s)"), 0u);
	SceneSystem::Terminate();
	SceneSystem::Initialize();
	const u32 before = log.Delivered().Count("unloaded type(s)");
	ASSERT_TRUE(SceneSystem::LoadSceneFromMemory(saved).IsValid());
	EXPECT_EQ(log.Delivered().Count("unloaded type(s)"), before + 1) << log.Delivered().Joined();
}

TEST_F(TypeReplacementTest, RefusedReplaceTouchesNothing) {
	const TypeInfo* first = RegisterProbe("SwapProbe", &_swapToken, false);
	ASSERT_NE(first, nullptr);
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _scene);
	Handle<SceneObject> doomed = SceneSystem::CreateObject("Doomed", _scene);
	Handle<Component> probe = holder->AddComponent(first);
	const Component* before = probe.Get();

	Swap swap;
	EXPECT_FALSE(TypeReplacement::Replace(nullptr, &RunSwap, &swap));
	EXPECT_FALSE(TypeReplacement::Replace(&_swapToken, nullptr, &swap));
	EXPECT_EQ(swap.calls, 0u);
	EXPECT_EQ(probe.Get(), before);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("SwapProbe")), first);

	EXPECT_TRUE(TypeReplacement::CanReplace());
	SceneSystem::Destroy(doomed);
	EXPECT_FALSE(TypeReplacement::CanReplace());
	EXPECT_FALSE(TypeReplacement::Replace(&_swapToken, &RunSwap, &swap));
	EXPECT_EQ(swap.calls, 0u);
	EXPECT_EQ(probe.Get(), before);

	SceneSystem::EndFrame();
	EXPECT_TRUE(TypeReplacement::CanReplace());
	EXPECT_TRUE(TypeReplacement::Replace(&_swapToken, &RunSwap, &swap));
	EXPECT_EQ(swap.calls, 1u);
	EXPECT_TRUE(probe.IsValid());
}

TEST_F(TypeReplacementTest, UndoOutlivesTokenReplaceClearsForImage) {
	const TypeInfo* first = RegisterProbe("SwapProbe", &_swapToken, false);
	ASSERT_NE(first, nullptr);
	UndoSystem::Initialize();
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _scene);
	Handle<Component> probe = holder->AddComponent(first);
	UndoSystem::Push(MakeUnique<PropertySetCommand>(EditTarget{probe}, "speed"_hash, Variant(2.5f), Variant(8.0f), "Speed"));
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(probe.Get())->speed, 8.0f);
	ASSERT_TRUE(UndoSystem::CanUndo());

	EventBus& bus = EventBus::GetGlobal();
	Swap swap;
	EditorEvents::ModuleReloadBegan.Publish(bus, {&_swapToken});
	ASSERT_TRUE(TypeReplacement::Replace(&_swapToken, &RunSwap, &swap));
	EditorEvents::ModuleReloadEnded.Publish(bus, {true, &_swapToken});

	ASSERT_TRUE(probe.IsValid());
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(probe.Get())->speed, 8.0f);
	ASSERT_TRUE(UndoSystem::CanUndo());
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(probe.Get())->speed, 2.5f);
	ASSERT_TRUE(UndoSystem::Redo());
	EXPECT_FLOAT_EQ(static_cast<SwapProbe*>(probe.Get())->speed, 8.0f);

	ASSERT_TRUE(UndoSystem::CanUndo());
	EditorEvents::ModuleReloadBegan.Publish(bus, {&_unknownBase});
	EXPECT_FALSE(UndoSystem::CanUndo());
	UndoSystem::Terminate();
}

TEST_F(TypeReplacementTest, EditorExtensionsSkipAToken) {
	ASSERT_NE(RegisterProbe("SwapProbe", &_swapToken, false), nullptr);
	EXPECT_TRUE(TypeRegistry::IsOwnerToken(&_swapToken));
	EXPECT_FALSE(EditorExtensions::Load(&_swapToken));
	EXPECT_FALSE(EditorExtensions::Load(nullptr));
}
