#include "TestsPCH.h"

#include "CSharpComponentScenarios.h"
#include "CSharpFieldScenarios.h"
#include "CSharpScenarioUtil.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PlayModeController.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>

#ifndef SHALLTEAR_SHARED
#include "Shalltear/Animation/AnimationClip.h"
#include "Shalltear/Animation/Animator.h"

SLT_DECLARE_LINKED_PACKAGE(Animation)
#endif

using namespace Shalltear;

namespace {

	using namespace Shalltear::CSharpScenarios;

	u64 Bits(f32 value) {
		u64 bits = 0;
		std::memcpy(&bits, &value, sizeof(value));
		return bits;
	}

	constexpr const char* HOLDER_DEFAULTS =
		"holder True -5 200 -3000 60000 -7 3735928559 -5000000000 18364758544493064720 2.5 0.125 0.25 0.75 1 2 3 4 5 6 7 0.5 -0.5 0.25 0.75 0.5 "
		"0.25 0.125 1 7.5 9 4 11 12 13";

	void DefaultsComeFromInitializers(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("FieldHolder");
		ASSERT_TRUE(holder.IsValid());
		const TypeInfo* type = holder->GetType();
		EXPECT_EQ(type->fieldCount, 22u);
		EXPECT_NE(type->fieldSyncFn, nullptr);
		EXPECT_EQ(type->size % 16, 0u);

		EXPECT_EQ(Read<bool>(holder, "flag"), true);
		EXPECT_EQ(Read<i8>(holder, "tiny"), -5);
		EXPECT_EQ(Read<u8>(holder, "small"), 200);
		EXPECT_EQ(Read<i16>(holder, "shorter"), -3000);
		EXPECT_EQ(Read<u16>(holder, "word"), 60000);
		EXPECT_EQ(Read<i32>(holder, "count"), -7);
		EXPECT_EQ(Read<u32>(holder, "mask"), 0xdeadbeefu);
		EXPECT_EQ(Read<i64>(holder, "big"), -5000000000ll);
		EXPECT_EQ(Read<u64>(holder, "huge"), 0xfedcba9876543210ull);
		EXPECT_FLOAT_EQ(Read<f32>(holder, "speed"), 2.5f);
		EXPECT_DOUBLE_EQ(Read<f64>(holder, "ratio"), 0.125);
		EXPECT_TRUE(Read<Vec2>(holder, "uv") == Vec2(0.25f, 0.75f));
		EXPECT_TRUE(Read<Vec3>(holder, "offset") == Vec3(1.0f, 2.0f, 3.0f));
		EXPECT_TRUE(Read<Vec4>(holder, "plane") == Vec4(4.0f, 5.0f, 6.0f, 7.0f));
		EXPECT_TRUE(Read<Quat>(holder, "turn") == Quat(0.5f, -0.5f, 0.25f, 0.75f));
		EXPECT_TRUE(Read<Color>(holder, "tint") == Color(0.5f, 0.25f, 0.125f, 1.0f));
		EXPECT_FLOAT_EQ(Read<f32>(holder, "secret"), 7.5f);
		EXPECT_EQ(Read<i32>(holder, "shared"), 9);
		EXPECT_EQ(Read<i32>(holder, "passing"), 13);

		for (const char* skipped : {"counter", "frozen", "LIMIT", "unmarked", "Property", "changed", "lookup"}) {
			EXPECT_EQ(FindField(type, StringHash(skipped)), nullptr) << skipped;
		}
		EXPECT_EQ(Row(Frame(host), "holder"), HOLDER_DEFAULTS);

		Handle<Component> aligned = world.Add("FieldAligned", "Aligned");
		ASSERT_TRUE(aligned.IsValid());
		for (const char* wide : {"wide", "turn"}) {
			const FieldInfo* field = FindField(aligned->GetType(), StringHash(wide));
			ASSERT_NE(field, nullptr) << wide;
			EXPECT_EQ(reinterpret_cast<uintptr_t>(field->addrFn(aligned.Get())) % 16, 0u) << wide;
		}
		EXPECT_TRUE(Read<Vec4>(aligned, "wide") == Vec4(1.0f, 2.0f, 3.0f, 4.0f));
		Write(aligned, "middle", static_cast<u8>(9));
		Write(aligned, "turn", Quat(0.0f, 0.0f, 0.0f, 1.0f));
		EXPECT_EQ(Row(Frame(host), "aligned"), "aligned 1 4 9 1 3");
	}

	void AttributesReachFieldInfo(const FieldHost&) {
		const TypeInfo* type = Script("FieldHolder");
		ASSERT_NE(type, nullptr);
		const FieldInfo* ranged = FindField(type, "ranged"_hash);
		ASSERT_NE(ranged, nullptr);
		EXPECT_STREQ(ranged->displayName, "ranged");
		EXPECT_EQ(ranged->fieldType, GetStaticType<f32>());
		EXPECT_EQ(ranged->getFn, nullptr);
		EXPECT_EQ(ranged->flags & (FieldFlagEditorVisible | FieldFlagReadOnly | FieldFlagTransient), static_cast<u32>(FieldFlagEditorVisible));
		const AttributeInfo* range = FindFieldAttribute(ranged, "Range"_hash);
		ASSERT_NE(range, nullptr);
		ASSERT_TRUE(range->value.Is<AttrRangeData>());
		EXPECT_FLOAT_EQ(range->value.As<AttrRangeData>().min, 0.0f);
		EXPECT_FLOAT_EQ(range->value.As<AttrRangeData>().max, 10.0f);
		const AttributeInfo* tooltip = FindFieldAttribute(ranged, "Tooltip"_hash);
		ASSERT_NE(tooltip, nullptr);
		EXPECT_EQ(tooltip->value.As<str>(), "Units per second");
		const AttributeInfo* header = FindFieldAttribute(ranged, "Header"_hash);
		ASSERT_NE(header, nullptr);
		EXPECT_EQ(header->value.As<str>(), "Motion");

		const FieldInfo* speed = FindField(type, "speed"_hash);
		ASSERT_NE(speed, nullptr);
		EXPECT_EQ(speed->attrCount, 0u);
		const FieldInfo* unseen = FindField(type, "unseen"_hash);
		const FieldInfo* locked = FindField(type, "locked"_hash);
		const FieldInfo* passing = FindField(type, "passing"_hash);
		ASSERT_TRUE(unseen && locked && passing);
		EXPECT_EQ(unseen->flags & FieldFlagEditorVisible, 0u);
		EXPECT_NE(locked->flags & FieldFlagReadOnly, 0u);
		EXPECT_NE(passing->flags & FieldFlagTransient, 0u);
		EXPECT_EQ(Script("UpdateOnly")->fieldSyncFn, nullptr);
	}

	void SceneRoundTrip(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("FieldHolder");
		ASSERT_TRUE(holder.IsValid());
		Write(holder, "flag", false);
		Write(holder, "tiny", static_cast<i8>(100));
		Write(holder, "small", static_cast<u8>(7));
		Write(holder, "shorter", static_cast<i16>(-1));
		Write(holder, "word", static_cast<u16>(65535));
		Write(holder, "count", 123456);
		Write(holder, "mask", 1u);
		Write(holder, "big", static_cast<i64>(1) << 40);
		Write(holder, "huge", static_cast<u64>(~0ull));
		Write(holder, "speed", -0.5f);
		Write(holder, "ratio", 1e100);
		Write(holder, "uv", Vec2(8.0f, 9.0f));
		Write(holder, "offset", Vec3(-1.0f, -2.0f, -3.0f));
		Write(holder, "plane", Vec4(0.0f, 1.0f, 0.0f, 2.0f));
		Write(holder, "turn", Quat(0.0f, 0.0f, 1.0f, 0.0f));
		Write(holder, "tint", Color(1.0f, 0.0f, 0.5f, 0.25f));
		Write(holder, "secret", 0.25f);
		Write(holder, "shared", -9);
		Write(holder, "ranged", 10.0f);
		Write(holder, "unseen", 110);
		Write(holder, "locked", 120);
		Write(holder, "passing", 130);

		Buffer snapshot;
		ASSERT_TRUE(SceneSystem::SaveSceneToMemory(world.scene, snapshot));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
		ASSERT_TRUE(restored.IsValid());

		Handle<Component> loaded = ScriptOn(restored->FindObjectByName("Host"), "FieldHolder");
		ASSERT_TRUE(loaded.IsValid());
		EXPECT_EQ(Row(Frame(host), "holder"),
				  "holder False 100 7 -1 65535 123456 1 1099511627776 18446744073709551615 -0.5 1E+100 8 9 -1 -2 -3 0 1 0 2 0 0 1 0 1 0 0.5 0.25 "
				  "0.25 -9 10 110 120 13");
		EXPECT_EQ(Read<i64>(loaded, "big"), static_cast<i64>(1) << 40);
		EXPECT_TRUE(Read<Quat>(loaded, "turn") == Quat(0.0f, 0.0f, 1.0f, 0.0f));
		EXPECT_EQ(Read<i32>(loaded, "passing"), 13);
	}

	void BoxedEditReachesScript(const FieldHost& host) {
		World world;
		Handle<Component> mover = world.Add("FieldMover");
		ASSERT_TRUE(mover.IsValid());
		std::vector<str> first = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(first.size(), 6u);
		EXPECT_EQ(first[1], "1");
		EXPECT_EQ(first[2], "2.5");
		EXPECT_EQ(first[3], "2");

		Write(mover, "speed", 10.0f);
		const std::vector<str> second = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(second.size(), 6u);
		EXPECT_EQ(second[1], "2");
		EXPECT_EQ(second[2], "12.5");
		EXPECT_EQ(second[3], "4");
		EXPECT_EQ(second[4], "10");
	}

	void ScriptChangeReachesReflection(const FieldHost& host) {
		World world;
		Handle<Component> mover = world.Add("FieldMover");
		ASSERT_TRUE(mover.IsValid());
		Frame(host, 3);
		EXPECT_EQ(Read<i32>(mover, "ticks"), 3);
		EXPECT_FLOAT_EQ(Read<Vec3>(mover, "position").x, 7.5f);
		EXPECT_FLOAT_EQ(Read<f32>(mover, "charge"), 8.0f);

		Buffer snapshot;
		ASSERT_TRUE(SceneSystem::SaveSceneToMemory(world.scene, snapshot));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
		ASSERT_TRUE(restored.IsValid());
		const std::vector<str> row = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(row.size(), 6u);
		EXPECT_EQ(row[1], "4");
		EXPECT_EQ(row[2], "10");
		EXPECT_EQ(row[3], "16");
	}

	void PrefabOverrideRoundTrip(const FieldHost& host) {
		World world;
		Handle<Component> authored = world.Add("FieldMover", "Unit");
		ASSERT_TRUE(authored.IsValid());
		Write(authored, "speed", 4.0f);
		Handle<SceneObject> source = world.scene->FindObjectByName("Unit");
		const Guid prefab = SceneSystem::SavePrefab(source, Path("project://mover.prefab"));
		ASSERT_TRUE(prefab.IsValid());
		SceneSystem::Destroy(source);
		SceneSystem::EndFrame();

		Handle<SceneObject> instance = SceneSystem::Instantiate(prefab, world.scene);
		ASSERT_TRUE(instance.IsValid());
		Handle<Component> live = ScriptOn(instance, "FieldMover");
		ASSERT_TRUE(live.IsValid());
		EXPECT_FLOAT_EQ(Read<f32>(live, "speed"), 4.0f);
		Write(live, "ticks", 100);

		ASSERT_TRUE(SceneSystem::SaveScene(world.scene, Path("project://mover.scene")));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		Handle<Scene> reloaded = SceneSystem::LoadScene(Path("project://mover.scene"));
		ASSERT_TRUE(reloaded.IsValid());

		const std::vector<str> row = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(row.size(), 6u);
		EXPECT_EQ(row[1], "101");
		EXPECT_EQ(row[2], "4");
		EXPECT_EQ(row[4], "4");
		EXPECT_TRUE(SceneSystem::RevertInstanceOverrides(reloaded->FindObjectByName("Unit")));
		SceneSystem::EndFrame();
		EXPECT_EQ(Read<i32>(ScriptOn(reloaded->FindObjectByName("Unit"), "FieldMover"), "ticks"), 0);
	}

	void InstantiateCopiesFields(const FieldHost& host) {
		World world;
		Handle<Component> mover = world.Add("FieldMover", "Original");
		ASSERT_TRUE(mover.IsValid());
		Write(mover, "speed", 6.0f);
		Frame(host, 2);

		Handle<SceneObject> clone = SceneSystem::Instantiate(world.scene->FindObjectByName("Original"));
		ASSERT_TRUE(clone.IsValid());
		Handle<Component> copied = ScriptOn(clone, "FieldMover");
		ASSERT_TRUE(copied.IsValid());
		EXPECT_NE(copied.GetInstanceId(), mover.GetInstanceId());
		EXPECT_EQ(Read<i32>(copied, "ticks"), 2);
		EXPECT_FLOAT_EQ(Read<f32>(copied, "charge"), 4.0f);

		const std::vector<str> rows = Frame(host);
		for (u32 index = 0; index < 2; ++index) {
			const std::vector<str> row = Words(Row(rows, "mover", index));
			ASSERT_EQ(row.size(), 6u) << index;
			EXPECT_EQ(row[1], "3");
			EXPECT_EQ(row[2], "18");
			EXPECT_EQ(row[3], "8");
			EXPECT_EQ(row[4], "6");
		}
	}

	void InheritedFieldsFollowParents(const FieldHost& host) {
		World world;
		Handle<Component> derived = world.Add("FieldDerived");
		ASSERT_TRUE(derived.IsValid());
		const TypeInfo* type = derived->GetType();
		const TypeInfo* base = Script("FieldBase");
		ASSERT_NE(base, nullptr);
		EXPECT_EQ(type->parent, base);
		EXPECT_EQ(type->fieldCount, 1u);
		EXPECT_EQ(base->fieldCount, 2u);

		const FieldInfo* rank = FindField(type, "rank"_hash);
		const FieldInfo* seed = FindField(type, "seed"_hash);
		const FieldInfo* level = FindField(type, "level"_hash);
		ASSERT_TRUE(rank && seed && level);
		EXPECT_EQ(rank->ownerType, base);
		EXPECT_EQ(level->ownerType, type);
		EXPECT_LT(rank->addrFn(derived.Get()), seed->addrFn(derived.Get()));
		EXPECT_LT(seed->addrFn(derived.Get()), level->addrFn(derived.Get()));
		EXPECT_EQ(Row(Frame(host), "derived"), "derived 21 22 23");

		Write(derived, "rank", 1);
		Write(derived, "seed", 2);
		Write(derived, "level", 3);
		EXPECT_EQ(Row(Frame(host), "derived"), "derived 1 2 3");

		Handle<Component> generic = world.Add("HolderOfInt", "Generic");
		ASSERT_TRUE(generic.IsValid());
		EXPECT_EQ(generic->GetType()->fieldCount, 2u);
		Write(generic, "held", 5);
		Write(generic, "spare", 6);
		EXPECT_EQ(Row(Frame(host), "generic fixed update"), "generic fixed update 5 6");
	}

	void SyncsStayBounded(const FieldHost& host) {
		World world;
		CSharpImpl::ManagedTypes::Counters& counters = host.getCounters();
		CSharpImpl::ManagedTypes::Counters before = counters;
		Handle<Component> silent = world.Add("UpdateOnly", "Plain");
		ASSERT_TRUE(silent.IsValid());
		EXPECT_EQ(counters.pushes, before.pushes);

		Handle<Component> mover = world.Add("FieldMover");
		ASSERT_TRUE(mover.IsValid());
		EXPECT_EQ(counters.pushes, before.pushes + 1);
		EXPECT_EQ(counters.pulls, before.pulls);

		Frame(host, 3);
		EXPECT_EQ(counters.pushes, before.pushes + 1);
		EXPECT_EQ(counters.pulls, before.pulls);

		before = counters;
		Write(mover, "speed", 1.0f);
		Write(mover, "ticks", 5);
		Read<i32>(mover, "ticks");
		EXPECT_EQ(counters.pushes, before.pushes + 1);
		EXPECT_EQ(counters.pulls, before.pulls);
		Frame(host, 2);
		EXPECT_EQ(counters.pushes, before.pushes + 1);
		EXPECT_EQ(counters.pulls, before.pulls + 1);
		EXPECT_EQ(Read<i32>(mover, "ticks"), 7);
		EXPECT_EQ(counters.pushes, before.pushes + 2);
	}

	void InspectorEditReachesScript(const FieldHost& host) {
		World world;
		Handle<Component> mover = world.Add("FieldMover");
		ASSERT_TRUE(mover.IsValid());
		const EditTarget target{mover};
		Frame(host);

		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "speed"_hash, Bits(2.5f), Bits(9.0f), 4));
		std::vector<str> row = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(row.size(), 6u);
		EXPECT_EQ(row[1], "2");
		EXPECT_EQ(row[2], "11.5");
		EXPECT_EQ(row[4], "9");

		ASSERT_TRUE(UndoSystem::Undo());
		row = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(row.size(), 6u);
		EXPECT_EQ(row[1], "3");
		EXPECT_EQ(row[2], "14");
		EXPECT_EQ(row[4], "2.5");

		UndoSystem::Push(MakeUnique<PropertySetCommand>(target, "position"_hash, Variant(Vec3(14.0f, 0.0f, 0.0f)), Variant(Vec3(100.0f, 0.0f, 0.0f))));
		row = Words(Row(Frame(host), "mover"));
		ASSERT_EQ(row.size(), 6u);
		EXPECT_EQ(row[1], "4");
		EXPECT_EQ(row[2], "102.5");
		EXPECT_EQ(row[3], "16");
	}

	void InspectorShowsAValueTheScriptChanged(const FieldHost& host) {
		World world;
		Handle<Component> mover = world.Add("FieldMover");
		ASSERT_TRUE(mover.IsValid());
		const EditTarget target{mover};
		const FieldInfo* ticks = FindField(mover->GetType(), "ticks"_hash);
		ASSERT_NE(ticks, nullptr);
		EXPECT_EQ(*static_cast<const i32*>(ticks->addrFn(target.Resolve())), 0);
		Frame(host, 2);
		EXPECT_EQ(*static_cast<const i32*>(ticks->addrFn(mover.Get())), 0);
		EXPECT_EQ(*static_cast<const i32*>(ticks->addrFn(target.Resolve())), 2);
	}

	void InspectorRowsEditScriptFields(const FieldHost& host) {
		World world;
		ASSERT_NE(Script("FieldHolder"), nullptr);
		ASSERT_NE(Script("FieldMover"), nullptr);
		Handle<SceneObject> object = SceneSystem::CreateObject("Subject", world.scene);
		Handle<Component> shown = object->AddComponent(Script("FieldHolder"));
		Handle<Component> ticking = object->AddComponent(Script("FieldMover"));
		ASSERT_TRUE(shown.IsValid());
		ASSERT_TRUE(ticking.IsValid());
		Write(shown, "count", 31337);
		Write(shown, "unseen", 4242);
		Write(shown, "shared", 7373);
		Write(ticking, "ticks", 7000);
		EditorSelection::Select(Handle<Object>(object.Get()));

		Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
		ASSERT_TRUE(panel.IsValid());
		UITest::MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {500, 3000};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel->BuildContent(context, root);
		context.Update();

		EXPECT_EQ(UITest::CountByClass(context, root, StringHash("slt-slider-track")), 1u);
		EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, root, "7373")));
		EXPECT_FALSE(context.IsAlive(UITest::FindByText(context, root, "4242")));

		UI::ElementHandle field = UITest::FindByText(context, root, "31337");
		ASSERT_TRUE(context.IsAlive(field));
		ASSERT_TRUE(context.HasClass(field, StringHash("slt-textfield")));
		UITest::ClickElement(context, field);
		ASSERT_EQ(context.GetFocused(), field);
		InputEvent selectAll{};
		selectAll.type = InputEventType::KeyDown;
		selectAll.key = Key::A;
		selectAll.mods = KeyMod::Ctrl;
		context.InjectInput(selectAll);
		for (char32_t digit : {U'5', U'6'}) {
			InputEvent typed{};
			typed.type = InputEventType::Text;
			typed.codepoint = digit;
			context.InjectInput(typed);
		}
		context.Update();
		UI::ElementHandle header = UITest::FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
		ASSERT_TRUE(context.IsAlive(header));
		UITest::ClickElement(context, header);
		EXPECT_NE(context.GetFocused(), field);

		std::vector<str> row = Words(Row(Frame(host), "holder"));
		ASSERT_GT(row.size(), 6u);
		EXPECT_EQ(row[6], "56");
		ASSERT_TRUE(UndoSystem::CanUndo());
		UndoSystem::Undo();
		row = Words(Row(Frame(host), "holder"));
		ASSERT_GT(row.size(), 6u);
		EXPECT_EQ(row[6], "31337");

		Frame(host);
		panel->OnTick(0.016f);
		context.Update();
		EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, root, "7003")));
		PanelManager::ClosePanel(panel);
		EditorSelection::Clear();
	}

	void PlaySnapshotKeepsValues(const FieldHost& host) {
		SceneSystem::SetPlaying(false);
		{
			World world;
			Handle<Component> mover = world.Add("FieldMover");
			ASSERT_TRUE(mover.IsValid());
			Write(mover, "speed", 3.0f);
			Write(mover, "ticks", 40);

			PlayModeController controller;
			const auto frame = [&controller] {
				SceneSystem::BeginFrame();
				if (controller.BeginFrameTick()) {
					SceneSystem::FixedUpdateAll();
					SceneSystem::UpdateAll();
					SceneSystem::LateUpdateAll();
				}
				SceneSystem::EndFrame();
				controller.PumpMainThread();
			};
			frame();
			EXPECT_TRUE(host.takeRecords().empty());

			controller.RequestToggle();
			frame();
			ASSERT_TRUE(controller.IsPlaying());
			frame();
			frame();
			const std::vector<str> rows = host.takeRecords();
			ASSERT_FALSE(rows.empty());
			const std::vector<str> last = Words(rows.back());
			ASSERT_EQ(last.size(), 6u);
			EXPECT_EQ(last[4], "3");
			EXPECT_GT(std::stoi(last[1]), 40);

			controller.RequestToggle();
			frame();
			ASSERT_FALSE(controller.IsPlaying());
			ASSERT_FALSE(SceneSystem::GetLoadedScenes().empty());
			Handle<Component> back = ScriptOn(SceneSystem::GetLoadedScenes()[0]->FindObjectByName("Host"), "FieldMover");
			ASSERT_TRUE(back.IsValid());
			EXPECT_EQ(Read<i32>(back, "ticks"), 40);
			EXPECT_FLOAT_EQ(Read<f32>(back, "speed"), 3.0f);
			EXPECT_FLOAT_EQ(Read<Vec3>(back, "position").x, 0.0f);
		}
		SceneSystem::SetPlaying(true);
	}

#ifndef SHALLTEAR_SHARED

	void AnimationClipDrivesField(const FieldHost& host) {
		World world;
		Handle<Component> glow = world.Add("FieldGlow");
		ASSERT_TRUE(glow.IsValid());
		Handle<SceneObject> object = world.scene->FindObjectByName("Host");

		AnimationClip* clip = New<AnimationClip>(MemTag::Asset);
		clip->virtualPath = "project://glow.clip";
		clip->length = 1.0f;
		clip->loop = AnimationLoopMode::Loop;
		AnimationClip::Track whole;
		whole.kind = AnimationTrackKind::Property;
		whole.interp = AnimationInterpolation::Linear;
		whole.componentType = StringHash("Shalltear.Conformance.FieldGlow");
		whole.name = "glow";
		whole.times.push_back(0.0f);
		whole.values.push_back(Vec4(0.875f, 0.0f, 0.0f, 0.0f));
		clip->tracks.push_back(std::move(whole));
		AnimationClip::Track channel;
		channel.kind = AnimationTrackKind::Property;
		channel.interp = AnimationInterpolation::Linear;
		channel.componentType = StringHash("Shalltear.Conformance.FieldGlow");
		channel.name = "shade";
		channel.component = 1;
		channel.times.push_back(0.0f);
		channel.values.push_back(Vec4(0.25f, 0.0f, 0.0f, 0.0f));
		clip->tracks.push_back(std::move(channel));

		{
			Handle<Animator> animator = object->AddComponent<Animator>();
			animator->SetClip(Handle<AnimationClip>(clip));

			CSharpImpl::ManagedTypes::Counters& counters = host.getCounters();
			const CSharpImpl::ManagedTypes::Counters before = counters;
			for (u32 frame = 0; frame < 3; ++frame) {
				FrameTime clockState;
				clockState.fixedDeltaTime = 1.0f / 50.0f;
				clockState.maximumDeltaTime = 1000.0f;
				Time::FrameScope clock{clockState};
				Time::BeginFrame(0.1f);
				SceneSystem::UpdateAll();
				FrameStageArgs args;
				FrameStages::Run(FrameStage::Update, args);
				SceneSystem::LateUpdateAll();
				SceneSystem::EndFrame();
			}
			const std::vector<str> rows = host.takeRecords();
			ASSERT_EQ(rows.size(), 3u);
			EXPECT_EQ(rows[2], "glow 0.875 0.25");
			EXPECT_GE(counters.pulls, before.pulls + 2);
			EXPECT_EQ(counters.pulls - before.pulls, counters.pushes - before.pushes);
			EXPECT_FLOAT_EQ(Read<Color>(glow, "shade").r, 1.0f);
			SceneSystem::Destroy(Handle<Component>(animator.Get()));
			SceneSystem::EndFrame();
		}
		Delete(clip);
	}

#endif

	constexpr CSharpScenarios::FieldScenario Scenarios[] = {
		{"DefaultsComeFromInitializers", &DefaultsComeFromInitializers},
		{"AttributesReachFieldInfo", &AttributesReachFieldInfo},
		{"SceneRoundTrip", &SceneRoundTrip},
		{"BoxedEditReachesScript", &BoxedEditReachesScript},
		{"ScriptChangeReachesReflection", &ScriptChangeReachesReflection},
		{"PrefabOverrideRoundTrip", &PrefabOverrideRoundTrip},
		{"InstantiateCopiesFields", &InstantiateCopiesFields},
		{"InheritedFieldsFollowParents", &InheritedFieldsFollowParents},
		{"SyncsStayBounded", &SyncsStayBounded},
		{"InspectorEditReachesScript", &InspectorEditReachesScript},
		{"InspectorShowsAValueTheScriptChanged", &InspectorShowsAValueTheScriptChanged},
		{"InspectorRowsEditScriptFields", &InspectorRowsEditScriptFields},
		{"PlaySnapshotKeepsValues", &PlaySnapshotKeepsValues},
#ifndef SHALLTEAR_SHARED
		{"AnimationClipDrivesField", &AnimationClipDrivesField},
#endif
	};

} // anonymous namespace

namespace Shalltear::CSharpScenarios {

	std::span<const FieldScenario> GetFieldScenarios() {
		static const std::vector<FieldScenario> all = [] {
			std::vector<FieldScenario> joined(std::begin(Scenarios), std::end(Scenarios));
			const std::span<const FieldScenario> kinds = GetKindScenarios();
			joined.insert(joined.end(), kinds.begin(), kinds.end());
			return joined;
		}();
		return all;
	}

	FieldProject::FieldProject() {
		static std::atomic<u64> counter{0};
		const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
		const std::filesystem::path root =
			std::filesystem::temp_directory_path() / ("shalltear_csharp_fields_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
		std::filesystem::create_directories(root);
		_root = root.generic_string();
		FileSystem::Mount("project", Path(_root));
		AssetDatabase::Initialize();
		TypeRegistry::InitializePrimitives();
#ifndef SHALLTEAR_SHARED
		SettingsPaths paths;
		paths.project = Path(_root + "/ProjectSettings.ini");
		paths.user = Path(_root + "/UserSettings.ini");
		paths.userProject = Path(_root + "/UserProjectSettings.ini");
		Settings::Initialize(paths);
#endif
		SceneSystem::Initialize();
#ifndef SHALLTEAR_SHARED
		ModuleSystem::Initialize();
		SLT_CALL_LINKED_PACKAGE(Animation, &_token);
#endif
		RegisterBuiltInPanels();
		EditorSceneOps::Initialize();
		EditorAssetOps::Initialize();
		EditorSelection::Initialize();
		UndoSystem::Initialize();
		PanelManager::Initialize();
	}

	FieldProject::~FieldProject() {
		PanelManager::Terminate();
		UndoSystem::Terminate();
		EditorSelection::Terminate();
		EditorSceneOps::Terminate();
		EditorAssetOps::Terminate();
		SceneSystem::Terminate();
#ifndef SHALLTEAR_SHARED
		FrameStages::UnregisterOwnedBy(&_token);
		ModuleSystem::Terminate();
		Settings::Terminate();
#endif
		AssetDatabase::CollectGarbage();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::path(_root), ec);
	}

} // namespace Shalltear::CSharpScenarios
