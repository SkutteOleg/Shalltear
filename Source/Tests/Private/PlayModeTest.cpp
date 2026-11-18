#include "TestsPCH.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorFrameClock.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PlayModeController.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace Shalltear;

namespace {

	class PmStatsComponent : public Component {
	public:
		f32 power = 0.0f;
		str label = "";
		Handle<SceneObject> buddy;

		SLT_REFLECT_COMPONENT(PmStatsComponent, Component)
		SLT_REFLECT_FIELD(power)
		SLT_REFLECT_FIELD(label)
		SLT_REFLECT_FIELD(buddy)
		SLT_END_REFLECT()

		PmStatsComponent() = default;
	};

	SLT_REGISTER_TYPE(PmStatsComponent)

	class PmRosterComponent : public Component {
	public:
		Vector<Handle<SceneObject>> members{GetMemoryResource(MemTag::Scene)};

		SLT_REFLECT_COMPONENT(PmRosterComponent, Component)
		SLT_REFLECT_ARRAY(members)
		SLT_END_REFLECT()

		PmRosterComponent() = default;
	};

	SLT_REGISTER_TYPE(PmRosterComponent)

	class PmCountingImporter : public IAssetImporter {
	public:
		int imports = 0;

		StringHash GetAssetType() const override { return StringHash("PmCountedAsset"); }
		Asset* Import(const Path& sourcePath) override {
			++imports;
			TextAsset* asset = New<TextAsset>(MemTag::Asset);
			asset->Import(sourcePath);
			return asset;
		}
	};

	class PmAssetRefComponent : public Component {
	public:
		Handle<TextAsset> art;

		SLT_REFLECT_COMPONENT(PmAssetRefComponent, Component)
		SLT_REFLECT_FIELD(art)
		SLT_END_REFLECT()

		PmAssetRefComponent() = default;
	};

	SLT_REGISTER_TYPE(PmAssetRefComponent)

	class PlayModeFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		size_t _sceneBytesBaseline = 0;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_playmode_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();

			SceneSystem::Initialize();
			SceneSystem::Terminate();
			_sceneBytesBaseline = MemoryManager::GetBytesForTag(MemTag::Scene);
			SceneSystem::Initialize();
		}

		void TearDown() override {
			SceneSystem::Terminate();
			AssetDatabase::CollectGarbage();
			EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scene), _sceneBytesBaseline);
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		Handle<PmStatsComponent> Stats(Handle<SceneObject> object) {
			return object.Get()->GetComponent<PmStatsComponent>();
		}
	};

} // anonymous namespace

TEST_F(PlayModeFixture, SnapshotRestoresStateHierarchyAndReferences) {
	Handle<Scene> scene = SceneSystem::CreateScene("Playground");
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", scene, parent);
	child.Get()->SetLocalPosition(Vec3{1.0f, 2.0f, 3.0f});

	Handle<PmStatsComponent> stats = parent.Get()->AddComponent<PmStatsComponent>();
	stats->power = 42.0f;
	stats->label = "editor-state";
	stats->buddy = child;

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(scene, snapshot));
	ASSERT_TRUE(snapshot);

	stats->power = -1.0f;
	stats->label = "mutated";
	SceneSystem::Destroy(child);
	SceneSystem::CreateObject("PlaySpawn", scene);
	SceneSystem::EndFrame();

	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 0u);

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());
	EXPECT_EQ(restored.Get()->name, "Playground");
	ASSERT_EQ(restored.Get()->GetRootCount(), 1u);

	Handle<SceneObject> restoredParent = restored.Get()->FindObjectByName("Parent");
	Handle<SceneObject> restoredChild = restored.Get()->FindObjectByName("Child");
	ASSERT_TRUE(restoredParent.IsValid());
	ASSERT_TRUE(restoredChild.IsValid());
	EXPECT_EQ(restoredChild.Get()->GetParent().Get(), restoredParent.Get());
	EXPECT_EQ(restoredChild.Get()->GetLocalPosition(), (Vec3{1.0f, 2.0f, 3.0f}));
	EXPECT_FALSE(restored.Get()->FindObjectByName("PlaySpawn").IsValid());

	Handle<PmStatsComponent> restoredStats = Stats(restoredParent);
	ASSERT_TRUE(restoredStats.IsValid());
	EXPECT_EQ(restoredStats->power, 42.0f);
	EXPECT_EQ(restoredStats->label, "editor-state");
	EXPECT_EQ(restoredStats->buddy.Get(), restoredChild.Get());
}

TEST_F(PlayModeFixture, ContainerOfReferencesSurvivesPlayStopCycle) {
	Handle<Scene> scene = SceneSystem::CreateScene("Roster");
	Handle<SceneObject> owner = SceneSystem::CreateObject("Owner", scene);
	Handle<SceneObject> first = SceneSystem::CreateObject("First", scene);
	Handle<SceneObject> second = SceneSystem::CreateObject("Second", scene);

	Handle<PmRosterComponent> roster = owner.Get()->AddComponent<PmRosterComponent>();
	roster->members.push_back(first);
	roster->members.push_back(second);

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(scene, snapshot));
	ASSERT_TRUE(snapshot);

	roster->members.clear();

	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 0u);

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());

	Handle<SceneObject> restoredOwner = restored.Get()->FindObjectByName("Owner");
	ASSERT_TRUE(restoredOwner.IsValid());
	Handle<PmRosterComponent> restoredRoster = restoredOwner.Get()->GetComponent<PmRosterComponent>();
	ASSERT_TRUE(restoredRoster.IsValid());

	ASSERT_EQ(restoredRoster->members.size(), 2u);
	EXPECT_EQ(restoredRoster->members[0].Get(), restored.Get()->FindObjectByName("First").Get());
	EXPECT_EQ(restoredRoster->members[1].Get(), restored.Get()->FindObjectByName("Second").Get());
}

TEST_F(PlayModeFixture, MultiScenePlayStopCycleRestoresPrimary) {
	Handle<Scene> a = SceneSystem::CreateScene("A");
	Handle<Scene> b = SceneSystem::CreateScene("B");
	SceneSystem::CreateObject("InA", a);
	SceneSystem::CreateObject("InB", b);
	SceneSystem::SetPrimaryScene(b);

	Vector<Buffer> snapshots{GetMemoryResource(MemTag::Scene)};
	size_t primaryIndex = static_cast<size_t>(-1);
	{
		auto scenes = SceneSystem::GetLoadedScenes();
		for (size_t i = 0; i < scenes.size(); ++i) {
			Buffer buffer;
			ASSERT_TRUE(SceneSystem::SaveSceneToMemory(scenes[i], buffer));
			if (scenes[i] == SceneSystem::GetPrimaryScene()) {
				primaryIndex = i;
			}
			snapshots.push_back(std::move(buffer));
		}
	}
	ASSERT_EQ(snapshots.size(), 2u);
	ASSERT_EQ(primaryIndex, 1u);

	SceneSystem::UnloadAllScenes();
	SceneSystem::EndFrame();
	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 0u);

	Handle<Scene> restoredPrimary;
	for (size_t i = 0; i < snapshots.size(); ++i) {
		Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshots[i]);
		ASSERT_TRUE(restored.IsValid());
		if (i == primaryIndex) {
			restoredPrimary = restored;
		}
	}
	SceneSystem::SetPrimaryScene(restoredPrimary);

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	EXPECT_EQ(SceneSystem::GetPrimaryScene().Get()->name, "B");
	EXPECT_TRUE(SceneSystem::GetPrimaryScene().Get()->FindObjectByName("InB").IsValid());
}

TEST_F(PlayModeFixture, PrefabInstanceOverridesSurviveTheCycle) {
	Handle<Scene> scene = SceneSystem::CreateScene("PrefabPlay");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
	Handle<PmStatsComponent> stats = unit.Get()->AddComponent<PmStatsComponent>();
	stats->label = "base";
	Guid prefabId = SceneSystem::SavePrefab(unit, Path("project://pm_unit.prefab"));
	ASSERT_TRUE(prefabId.IsValid());

	stats->label = "override";

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(scene, snapshot));

	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());
	Handle<SceneObject> restoredUnit = restored.Get()->FindObjectByName("Unit");
	ASSERT_TRUE(restoredUnit.IsValid());
	Handle<PmStatsComponent> restoredStats = Stats(restoredUnit);
	ASSERT_TRUE(restoredStats.IsValid());
	EXPECT_EQ(restoredStats->label, "override");

	ASSERT_TRUE(SceneSystem::RevertInstanceOverrides(restoredUnit));
	Handle<PmStatsComponent> reverted = Stats(restored.Get()->FindObjectByName("Unit"));
	ASSERT_TRUE(reverted.IsValid());
	EXPECT_EQ(reverted->label, "base");
}

TEST_F(PlayModeFixture, CrossSceneReferencesFollowTheNullPolicy) {
	Handle<Scene> a = SceneSystem::CreateScene("A");
	Handle<Scene> b = SceneSystem::CreateScene("B");
	Handle<SceneObject> inA = SceneSystem::CreateObject("InA", a);
	Handle<SceneObject> inB = SceneSystem::CreateObject("InB", b);
	Handle<PmStatsComponent> stats = inA.Get()->AddComponent<PmStatsComponent>();
	stats->buddy = inB;

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(a, snapshot));

	SceneSystem::UnloadScene(a);
	SceneSystem::EndFrame();

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());
	Handle<PmStatsComponent> restoredStats = Stats(restored.Get()->FindObjectByName("InA"));
	ASSERT_TRUE(restoredStats.IsValid());
	EXPECT_FALSE(restoredStats->buddy.IsValid());
}

TEST_F(PlayModeFixture, MalformedSnapshotsAreRejectedCleanly) {
	EXPECT_FALSE(SceneSystem::LoadSceneFromMemory(Buffer{}).IsValid());

	Buffer garbage(64, MemTag::Scene);
	std::memset(garbage.data, 0xAB, garbage.size);
	EXPECT_FALSE(SceneSystem::LoadSceneFromMemory(garbage).IsValid());
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 0u);
}

namespace {

	class PmStateWatcher : public Object {
	public:
		Vector<PlayModeState> states{GetMemoryResource(MemTag::Editor)};
		void OnPlayModeChanged(const EditorPlayModeChangedEvent& event) {
			states.push_back(event.state);
		}
	};

	bool RunFrame(PlayModeController& controller) {
		bool ticked = controller.BeginFrameTick();
		controller.PumpMainThread();
		AssetDatabase::CollectGarbage();
		return ticked;
	}

} // anonymous namespace

TEST_F(PlayModeFixture, PauseStepInertInEditMode) {
	PlayModeController controller;
	SceneSystem::CreateScene("Edit");

	EXPECT_FALSE(controller.IsPlaying());
	EXPECT_FALSE(controller.IsPaused());

	controller.RequestPause();
	controller.RequestStep();
	EXPECT_FALSE(RunFrame(controller));
	EXPECT_FALSE(controller.IsPaused());
	EXPECT_FALSE(RunFrame(controller));
}

TEST_F(PlayModeFixture, PauseFreezesGameplayAndResumeRestoresIt) {
	PlayModeController controller;
	SceneSystem::CreateScene("Play");

	controller.RequestToggle();
	ASSERT_FALSE(RunFrame(controller));
	ASSERT_TRUE(controller.IsPlaying());
	EXPECT_TRUE(RunFrame(controller));

	controller.RequestPause();
	EXPECT_TRUE(RunFrame(controller));
	EXPECT_TRUE(controller.IsPaused());
	EXPECT_FALSE(RunFrame(controller));
	EXPECT_FALSE(RunFrame(controller));

	controller.RequestPause();
	RunFrame(controller);
	EXPECT_FALSE(controller.IsPaused());
	EXPECT_TRUE(RunFrame(controller));

	controller.RequestToggle();
	RunFrame(controller);
	EXPECT_FALSE(controller.IsPlaying());
}

TEST_F(PlayModeFixture, StepGrantsOneGameplayFrame) {
	PlayModeController controller;
	SceneSystem::CreateScene("Play");

	controller.RequestToggle();
	RunFrame(controller);
	controller.RequestPause();
	RunFrame(controller);
	ASSERT_TRUE(controller.IsPaused());
	ASSERT_FALSE(RunFrame(controller));

	controller.RequestStep();
	RunFrame(controller);
	EXPECT_TRUE(controller.BeginFrameTick());
	EXPECT_FALSE(controller.BeginFrameTick());
	EXPECT_TRUE(controller.IsPaused());
}

TEST_F(PlayModeFixture, StepAdvancesOneFixedStep) {
	PlayModeController controller;
	EditorFrameClock clock;
	constexpr f64 PAUSED_EDITOR_FRAME = 0.0015;
	FrameTime policy;
	policy.fixedDeltaTime = 1.0f / 60.0f;
	policy.maximumDeltaTime = 0.1f;
	Time::FrameScope installed{policy};

	auto frame = [&controller, &clock](f64 wallSeconds) {
		const bool tick = controller.BeginFrameTick();
		const u32 steps = clock.BeginFrame(EditorFrameClock::KindFromGate(tick, controller.IsPaused()), wallSeconds);
		controller.PumpMainThread();
		return steps;
	};

	SceneSystem::CreateScene("Play");
	controller.RequestToggle();
	frame(PAUSED_EDITOR_FRAME);
	ASSERT_TRUE(controller.IsPlaying());
	controller.RequestPause();
	frame(PAUSED_EDITOR_FRAME);
	ASSERT_TRUE(controller.IsPaused());
	ASSERT_EQ(frame(PAUSED_EDITOR_FRAME), 0u);

	for (u32 press = 0; press < 5; ++press) {
		controller.RequestStep();
		ASSERT_EQ(frame(PAUSED_EDITOR_FRAME), 0u);
		EXPECT_EQ(frame(PAUSED_EDITOR_FRAME), 1u) << "press " << press;
		EXPECT_EQ(frame(PAUSED_EDITOR_FRAME), 0u);
	}

	controller.RequestToggle();
	frame(PAUSED_EDITOR_FRAME);
}

TEST_F(PlayModeFixture, StepWhileRunningPausesFirst) {
	PlayModeController controller;
	SceneSystem::CreateScene("Play");

	controller.RequestToggle();
	RunFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());
	ASSERT_FALSE(controller.IsPaused());

	controller.RequestStep();
	RunFrame(controller);
	EXPECT_TRUE(controller.IsPaused());
	EXPECT_TRUE(controller.BeginFrameTick());
	EXPECT_FALSE(controller.BeginFrameTick());
}

TEST_F(PlayModeFixture, PlayRefusedInPrefabScene) {
	EditorSceneOps::Initialize();
	SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", SceneSystem::GetPrimaryScene());
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Prefabs/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	Handle<Scene> prefabScene = EditorSceneOps::OpenPrefab(guid);
	ASSERT_TRUE(prefabScene.IsValid());

	PlayModeController controller;
	controller.RequestToggle();
	RunFrame(controller);
	EXPECT_FALSE(controller.IsPlaying());
	EXPECT_FALSE(RunFrame(controller));

	SceneSystem::UnloadScene(prefabScene);
	SceneSystem::EndFrame();
	controller.RequestToggle();
	RunFrame(controller);
	EXPECT_TRUE(controller.IsPlaying());

	controller.RequestToggle();
	RunFrame(controller);
	EditorSceneOps::Terminate();
}

TEST_F(PlayModeFixture, StoppingClearsPauseAndPendingSteps) {
	PlayModeController controller;
	SceneSystem::CreateScene("Play");

	controller.RequestToggle();
	RunFrame(controller);
	controller.RequestPause();
	RunFrame(controller);
	ASSERT_TRUE(controller.IsPaused());

	controller.RequestToggle();
	RunFrame(controller);
	EXPECT_FALSE(controller.IsPlaying());
	EXPECT_FALSE(controller.IsPaused());
	EXPECT_FALSE(controller.BeginFrameTick());

	controller.RequestToggle();
	controller.RequestPause();
	controller.RequestStep();
	RunFrame(controller);
	EXPECT_TRUE(controller.IsPlaying());
	EXPECT_FALSE(controller.IsPaused());
	EXPECT_TRUE(controller.BeginFrameTick());
}

TEST_F(PlayModeFixture, TransitionsPublishTheEditorPlayModeEvent) {
	PmStateWatcher* watcher = New<PmStateWatcher>(MemTag::Editor);
	Handle<PmStateWatcher> handle(watcher);
	EditorEvents::PlayModeChanged.Subscribe(EventBus::GetGlobal(), handle, &PmStateWatcher::OnPlayModeChanged);

	PlayModeController controller;
	SceneSystem::CreateScene("Play");

	controller.RequestToggle();
	RunFrame(controller);
	controller.RequestPause();
	RunFrame(controller);
	controller.RequestPause();
	RunFrame(controller);
	controller.RequestToggle();
	RunFrame(controller);

	ASSERT_EQ(watcher->states.size(), 4u);
	EXPECT_EQ(watcher->states[0], PlayModeState::Playing);
	EXPECT_EQ(watcher->states[1], PlayModeState::Paused);
	EXPECT_EQ(watcher->states[2], PlayModeState::Playing);
	EXPECT_EQ(watcher->states[3], PlayModeState::Editing);

	EventBus::GetGlobal().UnsubscribeAll(watcher->GetInstanceId());
	Delete(watcher);
}

namespace {

	class PmStartWatcher : public Object {
	public:
		u32 starts = 0;

		void OnPlayStarting(const EditorPlayStartingEvent&) {
			++starts;
			for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
				Handle<SceneObject> holder = scene->FindObjectByName("Holder");
				if (holder.IsValid()) {
					holder->GetComponent<PmStatsComponent>()->label = "starting";
				}
			}
		}
	};

} // anonymous namespace

TEST_F(PlayModeFixture, PlayStartingComesBeforeTheSnapshot) {
	PmStartWatcher* watcher = New<PmStartWatcher>(MemTag::Editor);
	EditorEvents::PlayStarting.Subscribe(EventBus::GetGlobal(), Handle<PmStartWatcher>(watcher), &PmStartWatcher::OnPlayStarting);

	PlayModeController controller;
	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	SceneSystem::CreateObject("Holder", scene)->AddComponent<PmStatsComponent>()->label = "edited";

	controller.RequestToggle();
	RunFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());
	EXPECT_EQ(watcher->starts, 1u);
	Handle<SceneObject> playing = SceneSystem::GetLoadedScenes()[0]->FindObjectByName("Holder");
	ASSERT_TRUE(playing.IsValid());
	EXPECT_EQ(Stats(playing)->label, "starting");
	Stats(playing)->label = "played";

	controller.RequestToggle();
	RunFrame(controller);
	ASSERT_FALSE(controller.IsPlaying());
	EXPECT_EQ(watcher->starts, 1u);
	Handle<SceneObject> restored = SceneSystem::GetLoadedScenes()[0]->FindObjectByName("Holder");
	ASSERT_TRUE(restored.IsValid());
	EXPECT_EQ(Stats(restored)->label, "starting");

	EventBus::GetGlobal().UnsubscribeAll(watcher->GetInstanceId());
	Delete(watcher);
}

namespace {

	class PmTickCounter : public Behavior {
	public:
		i32 updates = 0;
		i32 fixedUpdates = 0;
		i32 lateUpdates = 0;

		SLT_REFLECT_COMPONENT(PmTickCounter, Behavior)
		SLT_END_REFLECT()
	protected:
		void OnUpdate() override { ++updates; }
		void OnFixedUpdate() override { ++fixedUpdates; }
		void OnLateUpdate() override { ++lateUpdates; }
	};

	SLT_REGISTER_TYPE(PmTickCounter)

	void RunEditorFrame(PlayModeController& controller) {
		SceneSystem::BeginFrame();
		if (controller.BeginFrameTick()) {
			SceneSystem::FixedUpdateAll();
			SceneSystem::UpdateAll();
			SceneSystem::LateUpdateAll();
		}
		SceneSystem::EndFrame();
		controller.PumpMainThread();
	}

} // anonymous namespace

TEST_F(PlayModeFixture, BehaviorsDoNotTickInEditMode) {
	SceneSystem::CreateScene("Edit");
	Handle<SceneObject> node = SceneSystem::CreateObject("Node");
	Handle<PmTickCounter> ticker = node->AddComponent<PmTickCounter>();

	PlayModeController controller;
	for (u32 i = 0; i < 4; ++i) {
		RunEditorFrame(controller);
	}
	EXPECT_EQ(ticker->updates, 0);
	EXPECT_EQ(ticker->fixedUpdates, 0);
	EXPECT_EQ(ticker->lateUpdates, 0);

	controller.RequestToggle();
	RunEditorFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());
	EXPECT_FALSE(ticker.IsValid());
	Handle<SceneObject> played = SceneSystem::GetPrimaryScene()->FindObjectByName("Node");
	ASSERT_TRUE(played.IsValid());
	Handle<PmTickCounter> playedTicker = played->GetComponent<PmTickCounter>();
	ASSERT_TRUE(playedTicker.IsValid());
	RunEditorFrame(controller);
	EXPECT_EQ(playedTicker->updates, 1);
	EXPECT_EQ(playedTicker->fixedUpdates, 1);
	EXPECT_EQ(playedTicker->lateUpdates, 1);
}

namespace {

	std::vector<std::pair<u64, str>> _pmHooks;

	class PmHookScript : public Behavior {
	public:
		SLT_REFLECT_COMPONENT(PmHookScript, Behavior)
		SLT_END_REFLECT()
	protected:
		void OnAttach() override { _pmHooks.emplace_back(GetInstanceId(), "attach"); }
		void OnEnable() override { _pmHooks.emplace_back(GetInstanceId(), "enable"); }
		void OnDisable() override { _pmHooks.emplace_back(GetInstanceId(), "disable"); }
		void OnDetach() override { _pmHooks.emplace_back(GetInstanceId(), "detach"); }
		void OnDestroy() override { _pmHooks.emplace_back(GetInstanceId(), "destroy"); }
	};

	class PmOriginScript : public Behavior {
	public:
		Vec3 origin{-1.0f, -1.0f, -1.0f};

		SLT_REFLECT_COMPONENT(PmOriginScript, Behavior)
		SLT_END_REFLECT()
	protected:
		void OnEnable() override { origin = GetOwner()->GetLocalPosition(); }
	};

	SLT_REGISTER_TYPE(PmHookScript)
	SLT_REGISTER_TYPE(PmOriginScript)

	std::vector<str> HooksOf(u64 instanceId) {
		std::vector<str> hooks;
		for (const auto& [id, hook] : _pmHooks) {
			if (id == instanceId) {
				hooks.push_back(hook);
			}
		}
		return hooks;
	}

} // anonymous namespace

TEST_F(PlayModeFixture, HooksRunOnlyInPlayWorld) {
	_pmHooks.clear();
	SceneSystem::SetPlaying(false);
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> node = SceneSystem::CreateObject("Node", scene);
	Handle<PmHookScript> edited = node->AddComponent<PmHookScript>();
	u64 editedId = edited.GetInstanceId();
	EXPECT_TRUE(_pmHooks.empty());

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	Handle<PmHookScript> played = SceneSystem::GetPrimaryScene()->FindObjectByName("Node")->GetComponent<PmHookScript>();
	ASSERT_TRUE(played.IsValid());
	u64 playedId = played.GetInstanceId();
	EXPECT_EQ(HooksOf(editedId), std::vector<str>{});
	EXPECT_EQ(HooksOf(playedId), (std::vector<str>{"attach", "enable"}));

	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(controller.IsPlaying());
	Handle<PmHookScript> restored = SceneSystem::GetPrimaryScene()->FindObjectByName("Node")->GetComponent<PmHookScript>();
	ASSERT_TRUE(restored.IsValid());
	EXPECT_EQ(HooksOf(playedId), (std::vector<str>{"attach", "enable", "disable", "detach", "destroy"}));
	EXPECT_EQ(HooksOf(restored.GetInstanceId()), std::vector<str>{});
}

TEST_F(PlayModeFixture, PlayStartsFromCurrentScene) {
	SceneSystem::SetPlaying(false);
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> platform = SceneSystem::CreateObject("Platform", scene);
	platform->AddComponent<PmOriginScript>();
	platform->SetLocalPosition(Vec3{5.0f, 0.0f, 0.0f});

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	Handle<PmOriginScript> played = SceneSystem::GetPrimaryScene()->FindObjectByName("Platform")->GetComponent<PmOriginScript>();
	ASSERT_TRUE(played.IsValid());
	EXPECT_EQ(played->origin, (Vec3{5.0f, 0.0f, 0.0f}));
}

TEST_F(PlayModeFixture, SecondPlaySessionStartsClockAtZero) {
	FrameTime unbounded;
	unbounded.maximumDeltaTime = 1000.0f;
	Time::FrameScope clock{unbounded};
	SceneSystem::CreateScene("Play");
	PlayModeController controller;

	FrameStageArgs args;

	Time::BeginFrame(0.25);
	controller.RequestToggle();
	Time::BeginFrame(0.25);
	RunEditorFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());
	ASSERT_EQ(Time::GetFrameCount(), 0u);

	for (u32 i = 0; i < 4; ++i) {
		Time::BeginFrame(0.25);
		FrameStages::Run(FrameStage::Update, args);
		RunEditorFrame(controller);
	}
	Time::SetTimeScale(0.2f);
	ASSERT_DOUBLE_EQ(Time::GetTime(), 1.0);
	ASSERT_DOUBLE_EQ(Time::GetUnscaledTime(), 1.0);
	ASSERT_EQ(Time::GetFrameCount(), 4u);

	controller.RequestToggle();
	Time::BeginFrame(0.25);
	RunEditorFrame(controller);
	ASSERT_FALSE(controller.IsPlaying());

	controller.RequestToggle();
	Time::BeginFrame(0.25);
	RunEditorFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());

	EXPECT_DOUBLE_EQ(Time::GetTime(), 0.0);
	EXPECT_DOUBLE_EQ(Time::GetUnscaledTime(), 0.0);
	EXPECT_EQ(Time::GetFrameCount(), 0u);
	EXPECT_FLOAT_EQ(Time::GetTimeScale(), 1.0f);

	controller.RequestToggle();
	RunEditorFrame(controller);
}

TEST_F(PlayModeFixture, AsyncLoadsRunWhenStopped) {
	JobSystem::Initialize();
	{
		Handle<Scene> authored = SceneSystem::CreateScene("Authored");
		SceneSystem::CreateObject("Alpha", authored);
		ASSERT_TRUE(SceneSystem::SaveScene(authored, Path("project://editmode.scene")));
		SceneSystem::UnloadAllScenes();
		SceneSystem::EndFrame();

		PlayModeController controller;
		ASSERT_FALSE(controller.IsPlaying());

		Handle<SceneLoadOperation> operation = SceneSystem::LoadSceneAsync(Path("project://editmode.scene"));
		ASSERT_TRUE(operation.IsValid());
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!operation->IsDone() && std::chrono::steady_clock::now() < deadline) {
			RunEditorFrame(controller);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_EQ(operation->GetState(), SceneLoadState::Done);
		ASSERT_TRUE(operation->GetScene().IsValid());
		EXPECT_TRUE(operation->GetScene()->FindObjectByName("Alpha").IsValid());

		SceneSystem::UnloadAllScenes();
		SceneSystem::EndFrame();
		controller.RequestToggle();
		RunEditorFrame(controller);
		controller.RequestPause();
		RunEditorFrame(controller);
		ASSERT_TRUE(controller.IsPaused());

		Handle<SceneLoadOperation> paused = SceneSystem::LoadSceneAsync(Path("project://editmode.scene"));
		ASSERT_TRUE(paused.IsValid());
		deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!paused->IsDone() && std::chrono::steady_clock::now() < deadline) {
			ASSERT_FALSE(controller.BeginFrameTick());
			RunEditorFrame(controller);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		EXPECT_EQ(paused->GetState(), SceneLoadState::Done);

		SceneSystem::CancelAsyncLoads();
	}
	SceneSystem::Terminate();
	SceneSystem::Initialize();
	JobSystem::Terminate();
}

TEST_F(PlayModeFixture, SelectionSurvivesThePlayAndStopSwaps) {
	EditorSelection::Initialize();
	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	Handle<SceneObject> alpha = SceneSystem::CreateObject("Alpha", scene);
	Handle<SceneObject> beta = SceneSystem::CreateObject("Beta", scene);
	beta->AddComponent<PmStatsComponent>();

	EditorSelection::Select(Handle<Object>(alpha.Get()));
	EditorSelection::AddToSelection(Handle<Object>(beta.Get()));

	auto expectSelectionFollowed = [](u64 alphaBefore, const char* swap) {
		ASSERT_EQ(EditorSelection::GetSelectionCount(), 2u) << swap;
		std::span<const Handle<Object>> selection = EditorSelection::GetSelection();
		Object* firstSelected = selection[0].Get();
		ASSERT_NE(firstSelected, nullptr);
		EXPECT_NE(selection[0].GetInstanceId(), alphaBefore) << swap;
		EXPECT_EQ(static_cast<SceneObject*>(firstSelected)->name, "Alpha");
		EXPECT_EQ(static_cast<SceneObject*>(selection[1].Get())->name, "Beta");
		EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), selection[1].GetInstanceId());
	};

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	ASSERT_FALSE(alpha.IsValid());
	u64 alphaInPlay = EditorSelection::GetSelection()[0].GetInstanceId();
	expectSelectionFollowed(alpha.GetInstanceId(), "Play");

	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(controller.IsPlaying());
	expectSelectionFollowed(alphaInPlay, "Stop");

	EditorSelection::Terminate();
}

TEST_F(PlayModeFixture, SceneAnnotationsSurviveTheStopSwap) {
	EditorSelection::Initialize();
	UndoSystem::Initialize();
	EditorSceneOps::Initialize();
	SceneAnnotations::Initialize();

	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", scene, host);

	Vector<Handle<SceneObject>> members{GetMemoryResource(MemTag::Editor)};
	members.push_back(child);
	u64 folder = SceneAnnotations::CreateFolder(scene, "Inner", FolderParent::Under(host.GetInstanceId()),
												{members.data(), members.size()});
	ASSERT_NE(folder, 0u);
	SceneAnnotations::SetObjectLabels({members.data(), members.size()}, LabelColor::Purple);

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(controller.IsPlaying());
	ASSERT_FALSE(child.IsValid());

	Handle<Scene> restored = SceneSystem::GetPrimaryScene();
	ASSERT_TRUE(restored.IsValid());
	Handle<SceneObject> newHost;
	Handle<SceneObject> newChild;
	struct Search {
		Handle<SceneObject>* host;
		Handle<SceneObject>* child;
		static void Visit(SceneObject& node, void* user) {
			Search& search = *static_cast<Search*>(user);
			if (node.name == "Host") {
				*search.host = Handle<SceneObject>(&node);
			} else if (node.name == "Child") {
				*search.child = Handle<SceneObject>(&node);
			}
		}
	} search{&newHost, &newChild};
	restored.Get()->ForEachObject(&Search::Visit, &search);
	ASSERT_TRUE(newChild.IsValid());

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(newChild), LabelColor::Purple);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(newChild), folder);
	EXPECT_EQ(SceneAnnotations::GetFolderAnchor(restored, folder), newHost);

	SceneAnnotations::Terminate();
	EditorSceneOps::Terminate();
	UndoSystem::Terminate();
	EditorSelection::Terminate();
}

TEST_F(PlayModeFixture, StopKeepsUnsavedState) {
	EditorSelection::Initialize();
	UndoSystem::Initialize();
	EditorSceneOps::Initialize();

	Handle<Scene> edited = SceneSystem::CreateScene("Edited");
	Handle<Scene> saved = SceneSystem::CreateScene("Saved");
	SceneSystem::SetPrimaryScene(edited);
	EditorSceneOps::MarkDirty(edited);
	ASSERT_TRUE(EditorSceneOps::IsDirty(edited));

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	ASSERT_FALSE(edited.IsValid());
	EXPECT_TRUE(EditorSceneOps::IsDirty(SceneSystem::GetPrimaryScene()));
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(controller.IsPlaying());

	std::span<const Handle<Scene>> scenes = SceneSystem::GetLoadedScenes();
	ASSERT_EQ(scenes.size(), 2u);
	Handle<Scene> restoredEdited;
	Handle<Scene> restoredSaved;
	for (const Handle<Scene>& scene : scenes) {
		(scene.Get()->name == "Edited" ? restoredEdited : restoredSaved) = scene;
	}
	ASSERT_TRUE(restoredEdited.IsValid());
	ASSERT_TRUE(restoredSaved.IsValid());
	EXPECT_TRUE(EditorSceneOps::IsDirty(restoredEdited));
	EXPECT_FALSE(EditorSceneOps::IsDirty(restoredSaved));
	EXPECT_TRUE(EditorSceneOps::AnyDirty());

	EditorSceneOps::Terminate();
	UndoSystem::Terminate();
	EditorSelection::Terminate();
}

TEST_F(PlayModeFixture, LockedInspectorKeepsPinAcrossStopSwap) {
	EditorSelection::Initialize();
	UndoSystem::Initialize();
	PanelManager::Initialize();
	RegisterBuiltInPanels();

	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	Handle<SceneObject> pinned = SceneSystem::CreateObject("Pinned", scene);
	SceneSystem::CreateObject("Other", scene);

	const TypeInfo* inspectorType = TypeRegistry::FindType(StringHash("InspectorPanel"));
	ASSERT_NE(inspectorType, nullptr);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(inspectorType);
	ASSERT_TRUE(panel.IsValid());
	Variant lockedOn(true);
	Variant pinnedId(pinned.GetInstanceId());
	ASSERT_TRUE(SetField(panel.Get(), inspectorType, StringHash("locked"), &lockedOn));
	ASSERT_TRUE(SetField(panel.Get(), inspectorType, StringHash("lockedTargetId"), &pinnedId));

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(pinned.IsValid());

	Variant after;
	ASSERT_TRUE(GetField(panel.Get(), inspectorType, StringHash("lockedTargetId"), &after));
	Object* target = ObjectRegistry::Resolve(after.As<u64>());
	ASSERT_NE(target, nullptr);
	EXPECT_EQ(static_cast<SceneObject*>(target)->name, "Pinned");

	PanelManager::ClosePanel(panel);
	PanelManager::TickAll(0.0f);
	PanelManager::Terminate();
	UndoSystem::Terminate();
	EditorSelection::Terminate();
}

TEST_F(PlayModeFixture, SelectedComponentAndPlaySpawnsFollowRules) {
	EditorSelection::Initialize();
	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	Handle<SceneObject> node = SceneSystem::CreateObject("Node", scene);
	Handle<PmStatsComponent> stats = node->AddComponent<PmStatsComponent>();
	EditorSelection::Select(Handle<Object>(stats.Get()));

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());

	Handle<SceneObject> spawned = SceneSystem::CreateObject("Spawned", SceneSystem::GetPrimaryScene());
	EditorSelection::AddToSelection(Handle<Object>(spawned.Get()));
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 2u);

	controller.RequestToggle();
	controller.PumpMainThread();

	ASSERT_EQ(EditorSelection::GetSelectionCount(), 1u);
	Object* selected = EditorSelection::GetSelection()[0].Get();
	ASSERT_NE(selected, nullptr);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(selected->GetType(), PmStatsComponent::GetReflectType()));
	EXPECT_EQ(static_cast<Component*>(selected)->GetOwner()->name, "Node");

	EditorSelection::Terminate();
}

TEST_F(PlayModeFixture, PlayStopSkipsReimport) {
	PmCountingImporter importer;
	AssetImporterRegistry::Register(StringHash("PmCountedAsset"), &importer, {".pmcounted"});

	Path artPath("project://Assets/art.pmcounted");
	Path resolved = FileSystem::Resolve(artPath);
	std::filesystem::create_directories(std::filesystem::path(resolved.ToString()).parent_path());
	ASSERT_TRUE(FileSystem::WriteTextFile(resolved, "pixels"));

	Asset* art = AssetDatabase::LoadAsset(artPath);
	ASSERT_NE(art, nullptr);
	ASSERT_EQ(importer.imports, 1);

	Path scenePath("project://Assets/stage.scene");
	{
		Handle<Scene> authored = SceneSystem::CreateScene("Stage");
		Handle<SceneObject> node = SceneSystem::CreateObject("Node", authored);
		node.Get()->AddComponent<PmAssetRefComponent>()->art = Handle<TextAsset>(static_cast<TextAsset*>(art));
		ASSERT_TRUE(SceneSystem::SaveScene(authored, scenePath));
		SceneSystem::UnloadAllScenes();
		SceneSystem::EndFrame();
	}
	u64 authoredInstance = art->GetInstanceId();
	AssetDatabase::UnloadAsset(art);
	ASSERT_EQ(AssetDatabase::CollectGarbage(), 1u);

	auto artInstance = []() -> u64 {
		Handle<Scene> scene = SceneSystem::GetPrimaryScene();
		if (!scene.IsValid()) {
			return 0;
		}
		Handle<SceneObject> node = scene.Get()->FindObjectByName("Node");
		if (!node.IsValid()) {
			return 0;
		}
		Handle<PmAssetRefComponent> ref = node.Get()->GetComponent<PmAssetRefComponent>();
		return ref.IsValid() && ref->art.IsValid() ? ref->art.Get()->GetInstanceId() : 0;
	};

	ASSERT_TRUE(SceneSystem::LoadScene(scenePath).IsValid());
	u64 openedInstance = artInstance();
	ASSERT_NE(openedInstance, 0u);
	ASSERT_NE(openedInstance, authoredInstance);

	PlayModeController controller;
	controller.RequestToggle();
	RunFrame(controller);
	ASSERT_TRUE(controller.IsPlaying());
	EXPECT_EQ(artInstance(), openedInstance);

	controller.RequestToggle();
	RunFrame(controller);
	ASSERT_FALSE(controller.IsPlaying());

	EXPECT_EQ(artInstance(), openedInstance);

	Handle<Scene> restored = SceneSystem::GetPrimaryScene();
	ASSERT_TRUE(restored.IsValid());
	Handle<SceneObject> restoredNode = restored.Get()->FindObjectByName("Node");
	ASSERT_TRUE(restoredNode.IsValid());
	Handle<PmAssetRefComponent> ref = restoredNode.Get()->GetComponent<PmAssetRefComponent>();
	ASSERT_TRUE(ref.IsValid());
	EXPECT_TRUE(ref->art.IsValid());

	Handle<Object> weakArt(ref->art.Get());
	SceneSystem::UnloadAllScenes();
	SceneSystem::EndFrame();
	EXPECT_TRUE(weakArt.IsValid());
	EXPECT_GE(AssetDatabase::CollectGarbage(), 1u);
	EXPECT_FALSE(weakArt.IsValid());

	AssetImporterRegistry::Unregister(&importer);
}

TEST_F(PlayModeFixture, PrefabSelectionFoundByRow) {
	EditorSelection::Initialize();
	Handle<Scene> authoring = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", authoring);
	SceneSystem::CreateObject("Barrel", authoring, unit);
	Guid prefabId = SceneSystem::SavePrefab(unit, Path("project://pm_sel.prefab"));
	ASSERT_TRUE(prefabId.IsValid());
	SceneSystem::UnloadScene(authoring);
	SceneSystem::EndFrame();

	Handle<Scene> scene = SceneSystem::CreateScene("Play");
	SceneSystem::CreateObject("Main Camera", scene);
	SceneSystem::CreateObject("Light", scene);
	ASSERT_TRUE(SceneSystem::Instantiate(prefabId, scene).IsValid());
	ASSERT_TRUE(SceneSystem::Instantiate(prefabId, scene).IsValid());
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://pm_sel.scene")));
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();

	scene = SceneSystem::LoadScene(Path("project://pm_sel.scene"));
	ASSERT_TRUE(scene.IsValid());

	Vector<Handle<SceneObject>> units{GetMemoryResource(MemTag::Editor)};
	struct Search {
		Vector<Handle<SceneObject>>* out;
		static void Visit(SceneObject& node, void* user) {
			if (node.name == "Unit") {
				static_cast<Search*>(user)->out->push_back(Handle<SceneObject>(&node));
			}
		}
	} search{&units};
	scene->ForEachObject(&Search::Visit, &search);
	ASSERT_EQ(units.size(), 2u);
	Handle<SceneObject> barrel = units[1]->FindChildByName("Barrel");
	ASSERT_TRUE(barrel.IsValid());
	EXPECT_NE(barrel->GetLocalId(), 0u);
	EXPECT_NE(barrel->GetPrefabKey(), 0u);

	EditorSelection::Select(Handle<Object>(barrel.Get()));

	PlayModeController controller;
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_TRUE(controller.IsPlaying());
	controller.RequestToggle();
	controller.PumpMainThread();
	ASSERT_FALSE(controller.IsPlaying());
	ASSERT_FALSE(barrel.IsValid());

	ASSERT_EQ(EditorSelection::GetSelectionCount(), 1u);
	Object* selected = EditorSelection::GetSelection()[0].Get();
	ASSERT_NE(selected, nullptr);
	SceneObject* selectedObject = static_cast<SceneObject*>(selected);
	EXPECT_EQ(selectedObject->name, "Barrel");

	Vector<Handle<SceneObject>> restoredUnits{GetMemoryResource(MemTag::Editor)};
	Search restoredSearch{&restoredUnits};
	SceneSystem::GetPrimaryScene()->ForEachObject(&Search::Visit, &restoredSearch);
	ASSERT_EQ(restoredUnits.size(), 2u);
	EXPECT_EQ(selectedObject->GetParent(), restoredUnits[1]);

	EditorSelection::Terminate();
}
