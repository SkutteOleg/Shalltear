#include "TestsPCH.h"

#ifdef SHALLTEAR_SHARED

#include "CSharpComponentScenarios.h"
#include "CSharpRuntimeFixture.h"

#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/ComponentMenu.h"

using namespace Shalltear;
using namespace Shalltear::CSharpTests;

namespace {

	class CSharpComponentFixture : public CSharpRuntimeFixture {
	protected:
		Handle<Scene> _world;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			SceneSystem::Initialize();
			_hooks->installComponentMenu();
			ASSERT_TRUE(Attach()) << _error;
			ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
			_hooks->takeRecords();
			CSharpScenarios::TakeTwinRecords();
			_world = SceneSystem::CreateScene("World");
		}

		void TearDown() override {
			SceneSystem::Terminate();
			CSharpRuntimeFixture::TearDown();
		}

		static const TypeInfo* Script(const char* name) {
			return TypeRegistry::FindType(StringHash(str("Shalltear.Conformance.") + name));
		}

		Handle<Component> Add(const char* name) {
			const TypeInfo* type = Script(name);
			return type ? SceneSystem::CreateObject(name, _world)->AddComponent(type) : Handle<Component>();
		}

		str Records() { return CSharpScenarios::Join(_hooks->takeRecords()); }
	};

} // anonymous namespace

TEST_F(CSharpComponentFixture, AddComponentByType) {
	const TypeInfo* type = Script("HookRecorder");
	ASSERT_NE(type, nullptr);
	EXPECT_NE(type->flags & TypeFlagComponent, 0u);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(type, Behavior::GetReflectType()));
	EXPECT_EQ(type->ownerModule, _hooks->getOwnerToken());
	EXPECT_STREQ(type->displayName, "HookRecorder");

	Handle<Component> component = Add("HookRecorder");
	ASSERT_TRUE(component.IsValid());
	EXPECT_EQ(component->GetType(), type);
	EXPECT_EQ(Records(), "constructed|attach|enable");
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpComponentFixture, HooksMatchNativeBehavior) {
	CSharpScenarios::RunLifecycle(CSharpScenarios::GetTwinType());
	const str twin = CSharpScenarios::Join(CSharpScenarios::TakeTwinRecords());
	CSharpScenarios::RunLifecycle(Script("HookRecorder"));
	const str script = Records();

	EXPECT_EQ(script, twin);
	for (const char* hook : {"constructed", "attach", "detach", "enable", "disable", "destroy", "|update", "fixed update", "late update"}) {
		EXPECT_NE(script.find(hook), str::npos) << hook;
	}
}

TEST_F(CSharpComponentFixture, OnlyOverriddenHooksCross) {
	const u64 before = _hooks->getCounters().hookCrossings;
	Handle<Component> component = Add("UpdateOnly");
	ASSERT_TRUE(component.IsValid());
	EXPECT_EQ(_hooks->getCounters().hookCrossings, before);

	CSharpScenarios::RunFrames(3);
	EXPECT_EQ(_hooks->getCounters().hookCrossings, before + 3);

	component->SetEnabled(false);
	SceneSystem::Destroy(component);
	SceneSystem::EndFrame();
	EXPECT_EQ(_hooks->getCounters().hookCrossings, before + 3);

	Handle<Component> silent = Add("Silent");
	ASSERT_TRUE(silent.IsValid());
	CSharpScenarios::RunFrames(2);
	EXPECT_EQ(_hooks->getCounters().hookCrossings, before + 3);
}

TEST_F(CSharpComponentFixture, EditModeMatchesNativeBehavior) {
	SceneSystem::UnloadScene(_world);
	SceneSystem::EndFrame();
	SceneSystem::SetPlaying(false);

	CSharpScenarios::RunLifecycle(CSharpScenarios::GetTwinType());
	const str twin = CSharpScenarios::Join(CSharpScenarios::TakeTwinRecords());
	CSharpScenarios::RunLifecycle(Script("HookRecorder"));
	const str script = Records();
	SceneSystem::SetPlaying(true);

	EXPECT_EQ(script, twin);
	EXPECT_TRUE(script.starts_with("constructed")) << script;
	for (const char* hook : {"attach", "detach", "enable", "disable", "destroy"}) {
		EXPECT_EQ(script.find(hook), str::npos) << hook;
	}
}

TEST_F(CSharpComponentFixture, EachInstanceHasItsState) {
	Handle<Component> first = Add("UpdateOnly");
	CSharpScenarios::RunFrames(1);
	const std::vector<str> early = _hooks->takeRecords();
	ASSERT_EQ(early.size(), 1u);
	const str firstName = early[0].substr(0, early[0].find(" ran "));

	Handle<Component> second = Add("UpdateOnly");
	ASSERT_TRUE(first.IsValid());
	ASSERT_TRUE(second.IsValid());
	CSharpScenarios::RunFrames(2);

	const std::vector<str> rows = _hooks->takeRecords();
	ASSERT_EQ(rows.size(), 4u);
	u32 firstRows = 0;
	u32 secondRows = 0;
	for (const str& row : rows) {
		if (row.starts_with(firstName + " ran ")) {
			++firstRows;
			EXPECT_EQ(row, firstName + " ran " + std::to_string(firstRows + 1));
		} else {
			++secondRows;
			EXPECT_TRUE(row.ends_with(" ran " + std::to_string(secondRows))) << row;
		}
	}
	EXPECT_EQ(firstRows, 2u);
	EXPECT_EQ(secondRows, 2u);
}

TEST_F(CSharpComponentFixture, TickMaskMatchesOverrides) {
	const std::pair<const char*, BehaviorTick> expected[] = {
		{"HookRecorder", BehaviorTick::Update | BehaviorTick::FixedUpdate | BehaviorTick::LateUpdate},
		{"UpdateOnly", BehaviorTick::Update},
		{"Silent", BehaviorTick::None},
		{"LateReporter", BehaviorTick::LateUpdate},
		{"HolderOfInt", BehaviorTick::FixedUpdate},
	};
	for (const auto& [name, ticks] : expected) {
		Handle<Component> component = Add(name);
		ASSERT_TRUE(component.IsValid()) << name;
		EXPECT_EQ(static_cast<Behavior*>(component.Get())->GetTickMask(), ticks) << name;
	}
	EXPECT_EQ(Script("Holder"), nullptr);
}

TEST_F(CSharpComponentFixture, AbstractParentComesFirst) {
	const TypeInfo* parent = Script("EnableCounter");
	const TypeInfo* child = Script("LateReporter");
	ASSERT_NE(parent, nullptr);
	ASSERT_NE(child, nullptr);
	EXPECT_NE(parent->flags & TypeFlagAbstract, 0u);
	EXPECT_EQ(child->parent, parent);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(child, parent));

	Handle<SceneObject> object = SceneSystem::CreateObject("Abstract", _world);
	EXPECT_FALSE(object->AddComponent(parent).IsValid());

	Handle<Component> reporter = Add("LateReporter");
	ASSERT_TRUE(reporter.IsValid());
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(Records(), "late reporter enabled 1");
}

TEST_F(CSharpComponentFixture, TakenNameIsReported) {
	str message;
	EXPECT_EQ(Run("ComponentProbe.RegistersATakenName", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("C# component 'SceneObject' has a name already used by engine. Rename the class (or give it a namespace)"), 1u)
		<< _log.Delivered().Joined();
	EXPECT_EQ(TypeRegistry::FindType("SceneObject"), SceneObject::GetReflectType());
	EXPECT_NE(SceneObject::GetReflectType()->ownerModule, _hooks->getOwnerToken());
}

TEST_F(CSharpComponentFixture, BrokenDescriptorsAreRefused) {
	str message;
	EXPECT_EQ(Run("ComponentProbe.RegistersAWrongHash", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("C# type 'Shalltear.Conformance.Misnamed' has name hash"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(Script("Misnamed"), nullptr);
	EXPECT_EQ(Script("Other"), nullptr);

	EXPECT_EQ(Run("ComponentProbe.RegistersAnUnknownParent", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("C# type 'Shalltear.Conformance.Orphan' has unregistered parent. Rebuild the scripts"), 1u)
		<< _log.Delivered().Joined();
	EXPECT_EQ(Script("Orphan"), nullptr);
}

TEST_F(CSharpComponentFixture, ScriptsAppearInComponentMenu) {
	EXPECT_EQ(ComponentMenu::CategoryFor(Script("HookRecorder")), "Scripts/Shalltear.Conformance");
	EXPECT_EQ(ComponentMenu::CategoryFor(Script("HolderOfInt")), "Scripts/Shalltear.Conformance");
	const str abstractCategory(ComponentMenu::CategoryFor(Script("EnableCounter")));
	EXPECT_NE(abstractCategory, "Scripts/Shalltear.Conformance");

	const StringHash recorder = Script("HookRecorder")->name;
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(ComponentMenu::UnregisterOwnedBy(_hooks->getOwnerToken()), 0u);
	EXPECT_EQ(TypeRegistry::FindType(recorder), nullptr);
}

TEST_F(CSharpComponentFixture, ConstructorThrowLeavesInertComponent) {
	Handle<Component> component = Add("ThrowsInConstructor");
	ASSERT_TRUE(component.IsValid());
	EXPECT_EQ(component->GetType(), Script("ThrowsInConstructor"));
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by a constructor"), 1u) << _log.Delivered().Joined();

	CSharpScenarios::RunFrames(2);
	EXPECT_EQ(Records(), "");
}

TEST_F(CSharpComponentFixture, HookThrowIsReportedEachFrame) {
	Handle<Component> thrower = Add("ThrowsInUpdate");
	Handle<Component> other = Add("UpdateOnly");
	ASSERT_TRUE(thrower.IsValid());
	ASSERT_TRUE(other.IsValid());
	CSharpScenarios::RunFrames(2);

	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown by an update"), 2u) << _log.Delivered().Joined();
	const std::vector<str> rows = _hooks->takeRecords();
	u32 before = 0;
	u32 late = 0;
	u32 others = 0;
	for (const str& row : rows) {
		before += row == "update before the throw" ? 1u : 0u;
		late += row == "late update after the throw" ? 1u : 0u;
		others += row.starts_with("update only ") ? 1u : 0u;
	}
	EXPECT_EQ(before, 2u);
	EXPECT_EQ(late, 2u);
	EXPECT_EQ(others, 2u);
}

TEST_F(CSharpComponentFixture, UnloadStopsHooksOfLiveComponents) {
	Handle<Component> component = Add("HookRecorder");
	ASSERT_TRUE(component.IsValid());
	const TypeInfo* type = component->GetType();
	const StringHash name = type->name;
	_hooks->takeRecords();

	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(_log.Delivered().Count("1 C# component(s) alive at script unload (hooks stopped)"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(TypeRegistry::FindType(name), nullptr);
	ASSERT_TRUE(component.IsValid());
	EXPECT_EQ(component->GetType(), type);

	CSharpScenarios::RunFrames(2);
	SceneSystem::Destroy(component);
	SceneSystem::EndFrame();
	EXPECT_EQ(Records(), "");
	EXPECT_FALSE(component.IsValid());
}

#endif
