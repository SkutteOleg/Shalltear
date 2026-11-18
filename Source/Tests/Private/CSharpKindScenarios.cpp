#include "TestsPCH.h"

#include "CSharpScenarioUtil.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>

using namespace Shalltear;

namespace {

	using namespace Shalltear::CSharpScenarios;

	str Part(const str& row, std::string_view key) {
		const str prefix = str(key) + "=";
		for (const str& word : Words(row)) {
			if (word.starts_with(prefix)) {
				return word.substr(prefix.size());
			}
		}
		return "<no " + str(key) + ">";
	}

	str Kinds(const FieldHost& host) {
		return Row(Frame(host), "kinds");
	}

	const FieldInfo* Field(Handle<Component> component, const char* name) {
		return component.IsValid() ? FindField(component->GetType(), StringHash(name)) : nullptr;
	}

	void* At(Handle<Component> component, const char* name) {
		const FieldInfo* field = Field(component, name);
		if (!field) {
			ADD_FAILURE() << "no field " << name;
			return nullptr;
		}
		return field->addrFn(EditTarget{component}.Resolve());
	}

	size_t Count(Handle<Component> component, const char* name) {
		const FieldInfo* field = Field(component, name);
		void* at = At(component, name);
		return field && field->container && at ? field->container->sizeFn(at) : ~size_t{0};
	}

	void DescribeHolder(const FieldHost&) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		ASSERT_TRUE(holder.IsValid());
		const TypeInfo* type = holder->GetType();
		for (const char* skipped : {"nested", "map", "bare", "ignored"}) {
			EXPECT_EQ(FindField(type, StringHash(skipped)), nullptr) << skipped;
		}
		EXPECT_EQ(type->fieldCount, 25u);

		for (const char* text : {"title", "note", "hidden"}) {
			const FieldInfo* field = Field(holder, text);
			ASSERT_NE(field, nullptr) << text;
			EXPECT_EQ(field->fieldType, GetStaticType<str>()) << text;
			EXPECT_EQ(field->size, sizeof(str)) << text;
		}

		const FieldInfo* pace = Field(holder, "pace");
		ASSERT_NE(pace, nullptr);
		const TypeInfo* paceType = pace->fieldType;
		ASSERT_NE(paceType, nullptr);
		EXPECT_EQ(paceType, TypeRegistry::FindType(StringHash("Shalltear.Conformance.Pace")));
		EXPECT_NE(pace->flags & FieldFlagEnum, 0u);
		EXPECT_STREQ(paceType->displayName, "Pace");
		EXPECT_EQ(paceType->size, 4u);
		ASSERT_EQ(paceType->enumConstantCount, 3u);
		EXPECT_STREQ(paceType->enumConstants[2].displayName, "Run");
		EXPECT_EQ(paceType->enumConstants[2].value, 10);
		ASSERT_NE(Field(holder, "tiny"), nullptr);
		EXPECT_EQ(Field(holder, "tiny")->fieldType->size, 1u);
		const FieldInfo* reach = Field(holder, "reach");
		ASSERT_NE(reach, nullptr);
		EXPECT_EQ(reach->fieldType->size, 8u);
		EXPECT_EQ(reach->fieldType->enumConstants[0].value, -5);
		EXPECT_EQ(reach->fieldType->enumConstants[1].value, 5000000000ll);

		const auto pointee = [&](const char* name) {
			const FieldInfo* field = Field(holder, name);
			return field && field->fieldType && (field->flags & FieldFlagHandle) ? HandlePointeeName(*field->fieldType) : StringHash();
		};
		EXPECT_EQ(pointee("target"), SceneObject::GetReflectType()->name);
		EXPECT_EQ(Field(holder, "target")->fieldType->name, GetStaticType<Handle<SceneObject>>()->name);
		ASSERT_NE(Script("Carrier"), nullptr);
		EXPECT_EQ(pointee("carrier"), Script("Carrier")->name);
		EXPECT_EQ(pointee("text"), TextAsset::GetReflectType()->name);

		const FieldInfo* stats = Field(holder, "stats");
		ASSERT_NE(stats, nullptr);
		ASSERT_NE(stats->fieldType, nullptr);
		EXPECT_NE(stats->flags & FieldFlagSubobject, 0u);
		EXPECT_EQ(stats->fieldType, TypeRegistry::FindType(StringHash("Shalltear.Conformance.Stats")));
		EXPECT_STREQ(stats->fieldType->displayName, "Stats");
		EXPECT_EQ(stats->fieldType->fieldCount, 6u);
		const FieldInfo* wide = Field(holder, "wide");
		ASSERT_NE(wide, nullptr);
		const FieldInfo* turn = FindField(wide->fieldType, "turn"_hash);
		ASSERT_NE(turn, nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(turn->addrFn(wide->addrFn(holder.Get()))) % 16, 0u);
		const FieldInfo* loadout = Field(holder, "loadout");
		ASSERT_NE(loadout, nullptr);
		EXPECT_EQ(loadout->fieldType->fieldCount, 5u);
		const FieldInfo* mods = FindField(loadout->fieldType, "mods"_hash);
		ASSERT_NE(mods, nullptr);
		EXPECT_NE(mods->flags & FieldFlagContainer, 0u);
		const FieldInfo* chain = Field(holder, "chain");
		ASSERT_NE(chain, nullptr);
		EXPECT_EQ(chain->fieldType->fieldCount, 1u);

		struct Listed {
			const char* name;
			const TypeInfo* element;
			u32 size;
			u32 flag;
		};
		const Listed lists[] = {
			{"numbers", GetStaticType<i32>(), 4, 0},
			{"weights", GetStaticType<f32>(), 4, 0},
			{"names", GetStaticType<str>(), static_cast<u32>(sizeof(str)), 0},
			{"tags", GetStaticType<str>(), static_cast<u32>(sizeof(str)), 0},
			{"paces", paceType, 4, FieldFlagEnum},
			{"points", GetStaticType<Vec3>(), 12, 0},
			{"turns", GetStaticType<Quat>(), 16, 0},
			{"targets", GetStaticType<Handle<SceneObject>>(), 8, FieldFlagHandle},
			{"carriers", Field(holder, "carrier")->fieldType, 8, FieldFlagHandle},
			{"history", stats->fieldType, static_cast<u32>(stats->fieldType->size), FieldFlagSubobject},
			{"loadouts", loadout->fieldType, static_cast<u32>(loadout->fieldType->size), FieldFlagSubobject},
		};
		for (const Listed& list : lists) {
			const FieldInfo* field = Field(holder, list.name);
			ASSERT_NE(field, nullptr) << list.name;
			ASSERT_NE(field->container, nullptr) << list.name;
			EXPECT_NE(field->flags & FieldFlagContainer, 0u) << list.name;
			EXPECT_EQ(field->fieldType, nullptr) << list.name;
			ASSERT_NE(field->container->elementType, nullptr) << list.name;
			EXPECT_EQ(field->container->elementType->name, list.element->name) << list.name;
			EXPECT_EQ(field->container->elementSize, list.size) << list.name;
			EXPECT_EQ(field->container->elementFlags & (FieldFlagEnum | FieldFlagHandle | FieldFlagSubobject), list.flag) << list.name;
		}
	}

	constexpr const char* STATS_BASE = "5/1.5/base/0/Idle/0";
	constexpr const char* STATS_EMPTY = "0/0//0/Idle/0";

	void KindDefaultsReachBothSides(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		ASSERT_TRUE(holder.IsValid());

		EXPECT_EQ(*static_cast<const str*>(At(holder, "title")), "start");
		EXPECT_EQ(*static_cast<const str*>(At(holder, "note")), "");
		EXPECT_EQ(*static_cast<const str*>(At(holder, "hidden")), "h");
		EXPECT_EQ(Read<str>(holder, "title"), "start");
		EXPECT_EQ(Read<i64>(holder, "pace"), 1);
		EXPECT_EQ(Read<i64>(holder, "tiny"), 200);
		EXPECT_EQ(*static_cast<const i64*>(At(holder, "reach")), -5);
		EXPECT_EQ(*static_cast<const u64*>(At(holder, "target")), 0u);
		EXPECT_EQ(Count(holder, "numbers"), 3u);
		EXPECT_EQ(Count(holder, "weights"), 2u);
		EXPECT_EQ(Count(holder, "names"), 2u);
		EXPECT_EQ(Count(holder, "tags"), 0u);
		EXPECT_EQ(Count(holder, "history"), 0u);
		const FieldInfo* numbers = Field(holder, "numbers");
		ASSERT_NE(numbers, nullptr);
		EXPECT_EQ(*static_cast<const i32*>(numbers->container->atFn(At(holder, "numbers"), 2)), 3);
		const FieldInfo* names = Field(holder, "names");
		EXPECT_EQ(*static_cast<const str*>(names->container->atFn(At(holder, "names"), 1)), "b");
		const FieldInfo* stats = Field(holder, "stats");
		void* statsAt = At(holder, "stats");
		EXPECT_EQ(*static_cast<const i32*>(FindField(stats->fieldType, "hp"_hash)->addrFn(statsAt)), 5);
		EXPECT_EQ(*static_cast<const str*>(FindField(stats->fieldType, "title"_hash)->addrFn(statsAt)), "base");
		const FieldInfo* loadout = Field(holder, "loadout");
		EXPECT_EQ(*static_cast<const str*>(FindField(loadout->fieldType, "weapon"_hash)->addrFn(At(holder, "loadout"))), "none");
		EXPECT_EQ(*static_cast<const i32*>(FindField(loadout->fieldType, "ammo"_hash)->addrFn(At(holder, "loadout"))), 7);

		const str row = Kinds(host);
		EXPECT_EQ(Part(row, "title"), "start");
		EXPECT_EQ(Part(row, "note"), "");
		EXPECT_EQ(Part(row, "hidden"), "h");
		EXPECT_EQ(Part(row, "pace"), "Walk");
		EXPECT_EQ(Part(row, "tiny"), "On");
		EXPECT_EQ(Part(row, "reach"), "Near");
		EXPECT_EQ(Part(row, "target"), "null");
		EXPECT_EQ(Part(row, "carrier"), "null");
		EXPECT_EQ(Part(row, "text"), "null");
		EXPECT_EQ(Part(row, "stats"), STATS_BASE);
		EXPECT_EQ(Part(row, "wide"), "3/0/1");
		EXPECT_EQ(Part(row, "loadout"), str("none/7/[]/") + STATS_EMPTY + "/null");
		EXPECT_EQ(Part(row, "chain"), "4");
		EXPECT_EQ(Part(row, "numbers"), "[1,2,3]");
		EXPECT_EQ(Part(row, "weights"), "[0.5,1.5]");
		EXPECT_EQ(Part(row, "names"), "[a,b]");
		for (const char* list : {"tags", "points", "turns", "targets", "carriers", "history", "loadouts"}) {
			EXPECT_EQ(Part(row, list), "[]") << list;
		}
		EXPECT_EQ(Part(row, "paces"), "[Run]");
		EXPECT_EQ(Part(row, "ignored"), "9");
	}

	void ExpectChanged(const str& row, const char* object) {
		EXPECT_EQ(Part(row, "title"), "changed_\xC3\xA9");
		EXPECT_EQ(Part(row, "note"), "noted");
		EXPECT_EQ(Part(row, "hidden"), "moved");
		EXPECT_EQ(Part(row, "pace"), "Run");
		EXPECT_EQ(Part(row, "tiny"), "Off");
		EXPECT_EQ(Part(row, "reach"), "Far");
		EXPECT_EQ(Part(row, "target"), object);
		EXPECT_EQ(Part(row, "carrier"), "3");
		EXPECT_EQ(Part(row, "text"), "gold");
		EXPECT_EQ(Part(row, "stats"), "50/2.5/boss/1/Run/77");
		EXPECT_EQ(Part(row, "wide"), "9/1/0");
		EXPECT_EQ(Part(row, "loadout"), str("bow/3/[4,5]/1/0/in/0/Idle/0/") + object);
		EXPECT_EQ(Part(row, "numbers"), "[9,8]");
		EXPECT_EQ(Part(row, "weights"), "[2.5]");
		EXPECT_EQ(Part(row, "names"), "[x,,z\xC3\xA9]");
		EXPECT_EQ(Part(row, "tags"), "[t1,t2]");
		EXPECT_EQ(Part(row, "paces"), "[Idle,Run]");
		EXPECT_EQ(Part(row, "points"), "[6]");
		EXPECT_EQ(Part(row, "turns"), "[1]");
		EXPECT_EQ(Part(row, "targets"), str("[") + object + ",null]");
		EXPECT_EQ(Part(row, "carriers"), "[3]");
		EXPECT_EQ(Part(row, "history"), "[1/0/one/0/Idle/0,2/0/two/0/Idle/0]");
	}

	void ChangeEveryKind(const FieldHost& host, World& world, Handle<Component>& outHolder) {
		Handle<SceneObject> object = SceneSystem::CreateObject("Host", world.scene);
		ASSERT_TRUE(object->AddComponent(Script("Carrier")).IsValid());
		outHolder = object->AddComponent(Script("KindHolder"));
		ASSERT_TRUE(outHolder.IsValid());
		FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://loot.txt")), "gold");
		Asset* text = AssetDatabase::LoadAsset(Path("project://loot.txt"));
		ASSERT_NE(text, nullptr);
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(EditTarget{outHolder}, "text"_hash, 0, text->GetInstanceId(), 8));
		Write(outHolder, "step", 1);
		const str row = Kinds(host);
		ExpectChanged(row, "Host");
		EXPECT_EQ(Part(row, "loadouts"), "[axe/7/[]/0/0/null/0/Idle/0/null,null]");
	}

	void KindsSurviveSaveAndLoad(const FieldHost& host) {
		World world;
		Handle<Component> holder;
		ChangeEveryKind(host, world, holder);
		if (::testing::Test::HasFatalFailure()) {
			return;
		}

		for (const bool binary : {false, true}) {
			ASSERT_TRUE(SceneSystem::SaveScene(world.scene, Path("project://kinds.scene"), binary)) << binary;
			SceneSystem::UnloadScene(world.scene);
			SceneSystem::EndFrame();
			world.scene = SceneSystem::LoadScene(Path("project://kinds.scene"));
			ASSERT_TRUE(world.scene.IsValid()) << binary;

			const str row = Kinds(host);
			ExpectChanged(row, "Host");
			EXPECT_EQ(Part(row, "loadouts"), str("[axe/7/[]/") + STATS_EMPTY + "/null,none/7/[]/" + STATS_EMPTY + "/null]") << binary;
			EXPECT_EQ(Part(row, "chain"), "4") << binary;
		}
	}

	void KindsSurviveInstantiate(const FieldHost& host) {
		World world;
		Handle<Component> holder;
		ChangeEveryKind(host, world, holder);
		if (::testing::Test::HasFatalFailure()) {
			return;
		}
		Handle<SceneObject> clone = SceneSystem::Instantiate(world.scene->FindObjectByName("Host"));
		ASSERT_TRUE(clone.IsValid());
		clone->name = "Copy";
		Handle<Component> copied = ScriptOn(clone, "KindHolder");
		ASSERT_TRUE(copied.IsValid());

		const FieldInfo* numbers = Field(copied, "numbers");
		ASSERT_NE(numbers, nullptr);
		void* list = At(copied, "numbers");
		numbers->container->resizeFn(list, 3);
		*static_cast<i32*>(numbers->container->atFn(list, 2)) = 77;
		EditTarget{copied}.NotifyWritten();

		const std::vector<str> rows = Frame(host);
		ExpectChanged(Row(rows, "kinds", 0), "Host");
		const str second = Row(rows, "kinds", 1);
		EXPECT_EQ(Part(second, "numbers"), "[9,8,77]");
		EXPECT_EQ(Part(second, "target"), "Copy");
		EXPECT_EQ(Part(second, "names"), "[x,,z\xC3\xA9]");
		EXPECT_EQ(Part(second, "history"), "[1/0/one/0/Idle/0,2/0/two/0/Idle/0]");
		EXPECT_EQ(Part(second, "text"), "gold");
	}

	void EditorWritesReachEveryKind(const FieldHost& host) {
		World world;
		Handle<SceneObject> object = SceneSystem::CreateObject("Host", world.scene);
		Handle<Component> carrier = object->AddComponent(Script("Carrier"));
		Handle<Component> holder = object->AddComponent(Script("KindHolder"));
		Handle<SceneObject> other = SceneSystem::CreateObject("Other", world.scene);
		ASSERT_TRUE(carrier.IsValid() && holder.IsValid());
		const EditTarget target{holder};
		Kinds(host);

		Write(holder, "title", str("typed"));
		Write(holder, "pace", static_cast<i64>(10));
		Write(holder, "reach", static_cast<i64>(5000000000ll));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "tiny"_hash, 200, 0, 1));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "target"_hash, 0, other.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "carrier"_hash, 0, carrier.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, "numbers"_hash, 3, 0ull, 42ull, 3, "Add Element"));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, "targets"_hash, 0, 0ull, object.GetInstanceId(), 0, "Add Element"));
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "title"), "typed");
		EXPECT_EQ(Part(row, "pace"), "Run");
		EXPECT_EQ(Part(row, "reach"), "Far");
		EXPECT_EQ(Part(row, "tiny"), "Off");
		EXPECT_EQ(Part(row, "target"), "Other");
		EXPECT_EQ(Part(row, "carrier"), "3");
		EXPECT_EQ(Part(row, "numbers"), "[1,2,3,42]");
		EXPECT_EQ(Part(row, "targets"), "[Host]");
		EXPECT_EQ(Read<i64>(holder, "pace"), 10);
		EXPECT_EQ(Read<str>(holder, "title"), "typed");

		UndoSystem::Push(MakeUnique<RemoveContainerElementCommand>(target, "numbers"_hash, 0, "Remove Element"));
		EXPECT_EQ(Part(Kinds(host), "numbers"), "[2,3,42]");
		ASSERT_TRUE(UndoSystem::Undo());
		ASSERT_TRUE(UndoSystem::Undo());
		ASSERT_TRUE(UndoSystem::Undo());
		row = Kinds(host);
		EXPECT_EQ(Part(row, "numbers"), "[1,2,3]");
		EXPECT_EQ(Part(row, "targets"), "[]");
		EXPECT_EQ(Part(row, "carrier"), "3");
	}

	void UnchangedStringIsNotPushed(const FieldHost& host) {
		World world;
		CSharpImpl::ManagedTypes::Counters& counters = host.getCounters();
		u64 before = counters.textAssigns;
		Handle<Component> texts = world.Add("TextPair");
		ASSERT_TRUE(texts.IsValid());
		EXPECT_EQ(counters.textAssigns, before + 1);
		EXPECT_EQ(Read<str>(texts, "first"), "one");
		EXPECT_EQ(Read<str>(texts, "second"), "");

		before = counters.textAssigns;
		const u64 pushes = counters.pushes;
		for (u32 pass = 0; pass < 5; ++pass) {
			EXPECT_EQ(Read<str>(texts, "first"), "one");
		}
		EXPECT_EQ(counters.pushes, pushes + 5);
		EXPECT_EQ(counters.textAssigns, before);

		Write(texts, "second", str("native"));
		EXPECT_EQ(Row(Frame(host), "texts"), "texts one native 1 False");
		EXPECT_EQ(counters.textAssigns, before);
		EXPECT_EQ(Read<str>(texts, "second"), "native1");
		EXPECT_EQ(counters.textAssigns, before + 1);
		EXPECT_EQ(Read<str>(texts, "second"), "native1");
		EXPECT_EQ(counters.textAssigns, before + 1);

		Write(texts, "second", str("again"));
		EXPECT_EQ(Row(Frame(host), "texts"), "texts one again 2 True");
		before = counters.textAssigns;
		Write(texts, "first", str("edited"));
		EXPECT_EQ(Row(Frame(host), "texts"), "texts edited again2 3 False");
		EXPECT_EQ(Read<str>(texts, "first"), "edited");
		EXPECT_EQ(counters.textAssigns, before + 2);
	}

	void InheritedTextFieldsSyncApart(const FieldHost& host) {
		World world;
		Handle<Component> leaf = world.Add("TextLeaf");
		ASSERT_TRUE(leaf.IsValid());
		EXPECT_EQ(Read<str>(leaf, "root"), "r");
		EXPECT_EQ(Read<str>(leaf, "leaf"), "l");
		EXPECT_EQ(Read<str>(leaf, "twig"), "t");
		Write(leaf, "step", 1);
		EXPECT_EQ(Row(Frame(host), "leaf"), "leaf same1 same1 same1 1");
		EXPECT_EQ(Count(leaf, "marks"), 1u);
		EXPECT_EQ(Read<str>(leaf, "root"), "same1");
		EXPECT_EQ(Read<str>(leaf, "leaf"), "same1");
		EXPECT_EQ(Read<str>(leaf, "twig"), "same1");
		Write(leaf, "root", str("above"));
		EXPECT_EQ(Row(Frame(host), "leaf"), "leaf above same1 same1 1");

		const FieldInfo* words = Field(leaf, "words");
		ASSERT_NE(words, nullptr);
		ASSERT_EQ(Count(leaf, "words"), 1u);
		EXPECT_EQ(*static_cast<const str*>(words->container->atFn(At(leaf, "words"), 0)), "w");
		Write(leaf, "step", 2);
		Frame(host);
		EXPECT_EQ(Read<str>(leaf, "leaf"), "");
		EXPECT_EQ(Read<str>(leaf, "twig"), "same1");
		ASSERT_EQ(Count(leaf, "words"), 1u);
		EXPECT_EQ(*static_cast<const str*>(words->container->atFn(At(leaf, "words"), 0)), "");

		Handle<Component> plain = world.Add("PlainLeaf", "Plain");
		ASSERT_TRUE(plain.IsValid());
		EXPECT_EQ(Row(Frame(host), "plain"), "plain text 1");
		EXPECT_EQ(Count(plain, "marks"), 1u);
	}

	void WideValueStaysAlignedInAList(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("TailHolder");
		ASSERT_TRUE(holder.IsValid());
		const TypeInfo* tail = TypeRegistry::FindType(StringHash("Shalltear.Conformance.Tail"));
		ASSERT_NE(tail, nullptr);
		EXPECT_EQ(tail->alignment, 16u);
		EXPECT_EQ(tail->size, 32u);
		const FieldInfo* many = Field(holder, "many");
		const FieldInfo* turn = FindField(tail, "turn"_hash);
		const FieldInfo* last = FindField(tail, "tail"_hash);
		ASSERT_TRUE(many && many->container && turn && last);
		EXPECT_EQ(many->container->elementSize, 32u);
		void* list = At(holder, "many");
		ASSERT_EQ(many->container->sizeFn(list), 2u);
		void* second = many->container->atFn(list, 1);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(second) % 16, 0u);
		EXPECT_FLOAT_EQ(static_cast<const Quat*>(turn->addrFn(second))->z, 1.0f);
		EXPECT_EQ(*static_cast<const u8*>(last->addrFn(second)), 2);
		EXPECT_EQ(Read<i32>(holder, "after"), 11);

		*static_cast<u8*>(last->addrFn(second)) = 9;
		EditTarget{holder}.NotifyWritten();
		EXPECT_EQ(Row(Frame(host), "tails"), "tails 7 11 2 9 1");
	}

	void NullClassValueIsReplaced(const FieldHost& host) {
		World world;
		Handle<Component> only = world.Add("ChainOnly");
		ASSERT_TRUE(only.IsValid());
		EXPECT_EQ(Row(Frame(host), "only"), "only 4");
		const FieldInfo* chain = Field(only, "chain");
		ASSERT_NE(chain, nullptr);
		EXPECT_EQ(*static_cast<const i32*>(FindField(chain->fieldType, "id"_hash)->addrFn(At(only, "chain"))), 4);
	}

	void WrongComponentTypePullsNull(const FieldHost& host) {
		World world;
		Handle<SceneObject> object = SceneSystem::CreateObject("Host", world.scene);
		Handle<Component> carrier = object->AddComponent(Script("Carrier"));
		Handle<Component> silent = object->AddComponent(Script("Silent"));
		Handle<Component> pointer = object->AddComponent(Script("Pointer"));
		ASSERT_TRUE(carrier.IsValid() && silent.IsValid() && pointer.IsValid());
		const EditTarget target{pointer};
		EXPECT_EQ(Row(Frame(host), "pointer"), "pointer null []");

		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "aimed"_hash, 0, carrier.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, "many"_hash, 0, 0ull, carrier.GetInstanceId(), 0, "Add Element"));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, "many"_hash, 1, 0ull, silent.GetInstanceId(), 1, "Add Element"));
		EXPECT_EQ(Row(Frame(host), "pointer"), "pointer 3 [3,-1]");

		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "aimed"_hash, carrier.GetInstanceId(), silent.GetInstanceId(), 8));
		EXPECT_EQ(Row(Frame(host), "pointer"), "pointer null [3,-1]");
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "aimed"_hash, silent.GetInstanceId(), object.GetInstanceId(), 8));
		EXPECT_EQ(Row(Frame(host), "pointer"), "pointer null [3,-1]");
		Handle<Component> native = object->AddComponent(GetFilledTwinType());
		ASSERT_TRUE(native.IsValid());
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "aimed"_hash, 0, native.GetInstanceId(), 8));
		EXPECT_EQ(Row(Frame(host), "pointer"), "pointer null [3,-1]");

		Handle<Component> holder = object->AddComponent(Script("KindHolder"));
		ASSERT_TRUE(holder.IsValid());
		const EditTarget kinds{holder};
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(kinds, "target"_hash, 0, carrier.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(kinds, "text"_hash, 0, object.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(kinds, "targets"_hash, 0, 0ull, silent.GetInstanceId(), 0, "Add Element"));
		const str row = Kinds(host);
		EXPECT_EQ(Part(row, "target"), "null");
		EXPECT_EQ(Part(row, "text"), "null");
		EXPECT_EQ(Part(row, "targets"), "[null]");
		EXPECT_EQ(*static_cast<const u64*>(At(holder, "target")), 0u);

		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(kinds, "target"_hash, 0, object.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(kinds, "text"_hash, 0, object.GetInstanceId(), 8));
		const str cached = Kinds(host);
		EXPECT_EQ(Part(cached, "target"), "Host");
		EXPECT_EQ(Part(cached, "text"), "null");
	}

	void ReferenceToDestroyedObjectReadsNull(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		Handle<SceneObject> prey = SceneSystem::CreateObject("Prey", world.scene);
		Handle<Component> cargo = prey->AddComponent(Script("Carrier"));
		ASSERT_TRUE(holder.IsValid() && cargo.IsValid());
		const EditTarget target{holder};
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "target"_hash, 0, prey.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "carrier"_hash, 0, cargo.GetInstanceId(), 8));
		UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, "targets"_hash, 0, 0ull, prey.GetInstanceId(), 0, "Add Element"));
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "target"), "Prey");
		EXPECT_EQ(Part(row, "carrier"), "3");
		EXPECT_EQ(Part(row, "targets"), "[Prey]");

		SceneSystem::Destroy(prey);
		SceneSystem::EndFrame();
		row = Kinds(host);
		EXPECT_EQ(Part(row, "target"), "null");
		EXPECT_EQ(Part(row, "carrier"), "null");
		EXPECT_EQ(Part(row, "targets"), "[null]");

		Buffer snapshot;
		ASSERT_TRUE(SceneSystem::SaveSceneToMemory(world.scene, snapshot));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		world.scene = SceneSystem::LoadSceneFromMemory(snapshot);
		ASSERT_TRUE(world.scene.IsValid());
		row = Kinds(host);
		EXPECT_EQ(Part(row, "target"), "null");
		EXPECT_EQ(Part(row, "carrier"), "null");
		EXPECT_EQ(Part(row, "targets"), "[null]");
		EXPECT_EQ(*static_cast<const u64*>(At(ScriptOn(world.scene->FindObjectByName("Host"), "KindHolder"), "target")), 0u);
	}

	void KindOverridesSurvivePrefabReload(const FieldHost& host) {
		World world;
		Handle<SceneObject> source = SceneSystem::CreateObject("Unit", world.scene);
		Handle<Component> authored = source->AddComponent(Script("KindHolder"));
		ASSERT_TRUE(authored.IsValid());
		Write(authored, "title", str("authored"));
		const Guid prefab = SceneSystem::SavePrefab(source, Path("project://kinds.prefab"));
		ASSERT_TRUE(prefab.IsValid());
		SceneSystem::Destroy(source);
		SceneSystem::EndFrame();

		Handle<SceneObject> instance = SceneSystem::Instantiate(prefab, world.scene);
		ASSERT_TRUE(instance.IsValid());
		Handle<Component> live = ScriptOn(instance, "KindHolder");
		ASSERT_TRUE(live.IsValid());
		EXPECT_EQ(Read<str>(live, "title"), "authored");
		const EditTarget target{live};
		Write(live, "note", str("override"));
		Write(live, "pace", static_cast<i64>(10));
		UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "target"_hash, 0, instance.GetInstanceId(), 8));
		const FieldInfo* stats = Field(live, "stats");
		ASSERT_NE(stats, nullptr);
		*static_cast<i32*>(FindField(stats->fieldType, "hp"_hash)->addrFn(At(live, "stats"))) = 90;
		target.NotifyWritten();
		*static_cast<str*>(FindField(stats->fieldType, "title"_hash)->addrFn(At(live, "stats"))) = "raised";
		target.NotifyWritten();

		ASSERT_TRUE(SceneSystem::SaveScene(world.scene, Path("project://kinds-instance.scene")));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		world.scene = SceneSystem::LoadScene(Path("project://kinds-instance.scene"));
		ASSERT_TRUE(world.scene.IsValid());

		str row = Kinds(host);
		EXPECT_EQ(Part(row, "title"), "authored");
		EXPECT_EQ(Part(row, "note"), "override");
		EXPECT_EQ(Part(row, "pace"), "Run");
		EXPECT_EQ(Part(row, "target"), "Unit");
		EXPECT_EQ(Part(row, "stats"), "90/1.5/raised/0/Idle/0");
		EXPECT_EQ(Part(row, "numbers"), "[1,2,3]");
		EXPECT_EQ(Part(row, "weights"), "[0.5,1.5]");

		ASSERT_TRUE(SceneSystem::RevertInstanceOverrides(world.scene->FindObjectByName("Unit")));
		SceneSystem::EndFrame();
		row = Kinds(host);
		EXPECT_EQ(Part(row, "title"), "authored");
		EXPECT_EQ(Part(row, "note"), "");
		EXPECT_EQ(Part(row, "pace"), "Walk");
		EXPECT_EQ(Part(row, "target"), "null");
		EXPECT_EQ(Part(row, "stats"), STATS_BASE);
	}

	struct Inspected {
		UITest::MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;
		Handle<EditorPanel> panel;

		static UI::ContextDesc Tall() {
			UI::ContextDesc desc;
			desc.sizePx = {500, 9000};
			return desc;
		}

		explicit Inspected(Handle<SceneObject> object)
			: context(Tall(), &fonts) {
			EditorSelection::Select(Handle<Object>(object.Get()));
			panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
			root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			if (panel.IsValid()) {
				panel->BuildContent(context, root);
			}
			context.Update();
		}

		~Inspected() {
			if (panel.IsValid()) {
				PanelManager::ClosePanel(panel);
			}
			EditorSelection::Clear();
		}

		void Settle() {
			panel->OnTick(0.016f);
			context.Update();
		}

		UI::ElementHandle RowOf(std::string_view label) {
			UI::ElementHandle text = UITest::FindByText(context, root, label);
			return context.IsAlive(text) ? context.MakeHandle(context.GetElement(text)->parent) : UI::ElementHandle{};
		}

		UI::ElementHandle BodyOf(std::string_view label) {
			UI::ElementHandle row = RowOf(label);
			const UI::Element* element = context.GetElement(row);
			return element && element->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(element->nextSibling) : UI::ElementHandle{};
		}

		UI::ElementHandle Button(UI::ElementHandle from, std::string_view label) {
			for (u32 skip = 0;; ++skip) {
				UI::ElementHandle button = UITest::FindByClass(context, from, StringHash("slt-button"), skip);
				if (!context.IsAlive(button) || context.IsAlive(UITest::FindByText(context, button, label))) {
					return button;
				}
			}
		}

		void CollectTexts(UI::ElementHandle from, std::vector<str>& out) {
			const str text = context.GetTextString(from);
			if (!text.empty()) {
				out.push_back(text);
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectTexts(handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		std::vector<str> Texts(UI::ElementHandle from) {
			std::vector<str> texts;
			CollectTexts(from, texts);
			return texts;
		}

		str Picked(UI::ElementHandle from) {
			UI::ElementHandle picker = UITest::FindByClass(context, from, StringHash("slt-dropdown"), 0);
			return context.IsAlive(picker) ? context.GetTextString(UITest::NthChildOf(context, picker, 0)) : str("<no dropdown>");
		}

		void Pick(UI::ElementHandle from, u32 index) {
			UI::ElementHandle picker = UITest::FindByClass(context, from, StringHash("slt-dropdown"), 0);
			ASSERT_TRUE(context.IsAlive(picker));
			UITest::ClickElement(context, picker);
			ASSERT_EQ(context.GetOpenPopupCount(), 1u);
			UITest::ClickElement(context, UITest::NthChildOf(context, context.GetOpenPopup(0), index));
		}

		void Retype(UI::ElementHandle from, std::string_view shown, std::string_view typed) {
			UI::ElementHandle field = UITest::FindByText(context, from, shown);
			ASSERT_TRUE(context.IsAlive(field)) << shown << "'";
			UITest::ClickElement(context, field);
			ASSERT_EQ(context.GetFocused(), field) << shown << "'";
			InputEvent selectAll{};
			selectAll.type = InputEventType::KeyDown;
			selectAll.key = Key::A;
			selectAll.mods = KeyMod::Ctrl;
			context.InjectInput(selectAll);
			for (const char letter : typed) {
				InputEvent text{};
				text.type = InputEventType::Text;
				text.codepoint = static_cast<char32_t>(letter);
				context.InjectInput(text);
			}
			context.Update();
			UI::ElementHandle header = UITest::FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
			ASSERT_TRUE(context.IsAlive(header));
			UITest::ClickElement(context, header);
			EXPECT_NE(context.GetFocused(), field);
		}
	};

	bool Holds(const std::vector<str>& texts, std::string_view whole) {
		return std::find(texts.begin(), texts.end(), whole) != texts.end();
	}

	void InspectorDrawsEveryKind(const FieldHost& host) {
		World world;
		Handle<Component> holder;
		ChangeEveryKind(host, world, holder);
		if (::testing::Test::HasFatalFailure()) {
			return;
		}
		Inspected view(world.scene->FindObjectByName("Host"));
		ASSERT_TRUE(view.panel.IsValid());
		view.Settle();
		const u64 pushes = host.getCounters().pushes;
		view.Settle();
		EXPECT_EQ(host.getCounters().pushes, pushes + 2);

		const std::vector<str> texts = view.Texts(view.root);
		for (const str& text : texts) {
			EXPECT_EQ(text.find("no drawer"), str::npos) << text << "'";
		}
		for (const char* label : {"step", "title", "note", "hidden", "pace", "tiny", "reach", "target", "carrier", "text", "stats", "wide",
								  "loadout", "chain", "numbers", "weights", "names", "tags", "paces", "points", "turns", "targets",
								  "carriers", "history", "loadouts"}) {
			EXPECT_TRUE(Holds(texts, label)) << label;
		}
		for (const char* skipped : {"nested", "map", "bare", "ignored"}) {
			EXPECT_FALSE(Holds(texts, skipped)) << skipped;
		}
		for (const char* value : {"changed \xC3\xA9", "noted", "moved", "Host", "loot.txt", "boss", "bow", "t1", "t2", "z\xC3\xA9", "one", "two",
								  "axe", "2.5"}) {
			EXPECT_TRUE(Holds(texts, value)) << value;
		}
		EXPECT_EQ(view.Picked(view.RowOf("pace")), "Run");
		EXPECT_EQ(view.Picked(view.RowOf("tiny")), "Off");
		EXPECT_EQ(view.Picked(view.RowOf("reach")), "Far");
		EXPECT_EQ(view.Picked(view.BodyOf("paces")), "Idle");
		EXPECT_EQ(UITest::CountByClass(view.context, view.BodyOf("paces"), StringHash("slt-dropdown")), 2u);
		EXPECT_TRUE(Holds(view.Texts(view.BodyOf("numbers")), "9"));
		EXPECT_TRUE(Holds(view.Texts(view.BodyOf("points")), "3"));
		EXPECT_TRUE(Holds(view.Texts(view.BodyOf("targets")), "Host"));
		EXPECT_TRUE(Holds(view.Texts(view.BodyOf("stats")), "50"));
		EXPECT_TRUE(Holds(view.Texts(view.BodyOf("loadout")), "Add Element"));
	}

	void EnumShowsConstantNames(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		ASSERT_TRUE(holder.IsValid());
		Kinds(host);
		Inspected view(world.scene->FindObjectByName("Host"));
		ASSERT_TRUE(view.panel.IsValid());
		EXPECT_EQ(view.Picked(view.RowOf("pace")), "Walk");
		EXPECT_EQ(view.Picked(view.RowOf("tiny")), "On");
		EXPECT_EQ(view.Picked(view.RowOf("reach")), "Near");

		UI::ElementHandle picker = UITest::FindByClass(view.context, view.RowOf("pace"), StringHash("slt-dropdown"), 0);
		ASSERT_TRUE(view.context.IsAlive(picker));
		UITest::ClickElement(view.context, picker);
		ASSERT_EQ(view.context.GetOpenPopupCount(), 1u);
		const std::vector<str> offered = view.Texts(view.context.GetOpenPopup(0));
		EXPECT_EQ(offered, (std::vector<str>{"Idle", "Walk", "Run"}));
		UITest::ClickElement(view.context, UITest::NthChildOf(view.context, view.context.GetOpenPopup(0), 2));
		EXPECT_EQ(Part(Kinds(host), "pace"), "Run");
		EXPECT_EQ(Read<i64>(holder, "pace"), 10);

		view.Pick(view.RowOf("tiny"), 0);
		view.Pick(view.RowOf("reach"), 1);
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "tiny"), "Off");
		EXPECT_EQ(Part(row, "reach"), "Far");
		view.Settle();
		EXPECT_EQ(view.Picked(view.RowOf("reach")), "Far");

		ASSERT_TRUE(UndoSystem::Undo());
		ASSERT_TRUE(UndoSystem::Undo());
		ASSERT_TRUE(UndoSystem::Undo());
		row = Kinds(host);
		EXPECT_EQ(Part(row, "pace"), "Walk");
		EXPECT_EQ(Part(row, "tiny"), "On");
		EXPECT_EQ(Part(row, "reach"), "Near");
	}

	void ReferencesArePickedInInspector(const FieldHost& host) {
		World world;
		Handle<SceneObject> object = SceneSystem::CreateObject("Host", world.scene);
		Handle<Component> carrier = object->AddComponent(Script("Carrier"));
		Handle<Component> silent = object->AddComponent(Script("Silent"));
		Handle<Component> holder = object->AddComponent(Script("KindHolder"));
		Handle<SceneObject> other = SceneSystem::CreateObject("Other", world.scene);
		ASSERT_TRUE(carrier.IsValid() && silent.IsValid() && holder.IsValid() && other.IsValid());
		Kinds(host);
		Inspected view(object);
		ASSERT_TRUE(view.panel.IsValid());

		const auto pick = [&view](std::string_view row, std::string_view entry) {
			UI::ElementHandle button = UITest::FindByClass(view.context, view.RowOf(row), EditorTheme::Class::ObjectFieldButton, 0);
			ASSERT_TRUE(view.context.IsAlive(button)) << row;
			UITest::ClickElement(view.context, button);
			view.context.Update();
			ASSERT_EQ(view.context.GetOpenPopupCount(), 1u) << row;
			const std::vector<str> offered = view.Texts(view.context.GetOpenPopup(0));
			EXPECT_FALSE(Holds(offered, "Host / Silent")) << row;
			EXPECT_FALSE(Holds(offered, "Host / KindHolder")) << row;
			UI::ElementHandle chosen = UITest::FindByText(view.context, view.context.GetOpenPopup(0), entry);
			ASSERT_TRUE(view.context.IsAlive(chosen)) << entry;
			UITest::ClickElement(view.context, chosen);
			view.context.Update();
		};
		pick("carrier", "Host / Carrier");
		if (::testing::Test::HasFatalFailure()) {
			return;
		}
		pick("target", "Other");
		if (::testing::Test::HasFatalFailure()) {
			return;
		}
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "carrier"), "3");
		EXPECT_EQ(Part(row, "target"), "Other");
		view.Settle();
		view.Settle();
		EXPECT_TRUE(Holds(view.Texts(view.RowOf("target")), "Other"));
		EXPECT_TRUE(Holds(view.Texts(view.RowOf("carrier")), "Carrier"));

		ASSERT_TRUE(UndoSystem::Undo());
		ASSERT_TRUE(UndoSystem::Undo());
		row = Kinds(host);
		EXPECT_EQ(Part(row, "carrier"), "null");
		EXPECT_EQ(Part(row, "target"), "null");
	}

	void ListResizesFromInspector(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		ASSERT_TRUE(holder.IsValid());
		Kinds(host);
		Inspected view(world.scene->FindObjectByName("Host"));
		ASSERT_TRUE(view.panel.IsValid());

		UI::ElementHandle add = view.Button(view.BodyOf("numbers"), "Add Element");
		ASSERT_TRUE(view.context.IsAlive(add));
		UITest::ClickElement(view.context, add);
		view.Settle();
		EXPECT_EQ(Part(Kinds(host), "numbers"), "[1,2,3,0]");
		view.Retype(view.BodyOf("numbers"), "3", "30");
		EXPECT_EQ(Part(Kinds(host), "numbers"), "[1,2,30,0]");
		UI::ElementHandle remove = view.Button(view.BodyOf("numbers"), "-");
		ASSERT_TRUE(view.context.IsAlive(remove));
		UITest::ClickElement(view.context, remove);
		view.Settle();
		EXPECT_EQ(Part(Kinds(host), "numbers"), "[2,30,0]");

		add = view.Button(view.BodyOf("names"), "Add Element");
		ASSERT_TRUE(view.context.IsAlive(add));
		UITest::ClickElement(view.context, add);
		view.Settle();
		view.Retype(view.BodyOf("names"), "b", "bee");
		remove = view.Button(view.BodyOf("names"), "-");
		ASSERT_TRUE(view.context.IsAlive(remove));
		UITest::ClickElement(view.context, remove);
		view.Settle();
		EXPECT_EQ(Part(Kinds(host), "names"), "[bee,]");

		add = view.Button(view.BodyOf("history"), "Add Stats");
		ASSERT_TRUE(view.context.IsAlive(add));
		UITest::ClickElement(view.context, add);
		view.Settle();
		add = view.Button(view.BodyOf("points"), "Add Element");
		ASSERT_TRUE(view.context.IsAlive(add));
		UITest::ClickElement(view.context, add);
		view.Settle();
		add = view.Button(view.BodyOf("paces"), "Add Element");
		ASSERT_TRUE(view.context.IsAlive(add));
		UITest::ClickElement(view.context, add);
		view.Settle();
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "history"), str("[") + STATS_EMPTY + "]");
		EXPECT_EQ(Part(row, "points"), "[0]");
		EXPECT_EQ(Part(row, "paces"), "[Run,Idle]");
		EXPECT_EQ(Count(holder, "history"), 1u);

		for (u32 step = 0; step < 9; ++step) {
			ASSERT_TRUE(UndoSystem::Undo()) << step;
		}
		row = Kinds(host);
		EXPECT_EQ(Part(row, "numbers"), "[1,2,3]");
		EXPECT_EQ(Part(row, "names"), "[a,b]");
		EXPECT_EQ(Part(row, "history"), "[]");
		EXPECT_EQ(Part(row, "points"), "[]");
		EXPECT_EQ(Part(row, "paces"), "[Run]");
	}

	void NestedValueRoundTrip(const FieldHost& host) {
		World world;
		Handle<Component> holder = world.Add("KindHolder");
		ASSERT_TRUE(holder.IsValid());
		Kinds(host);
		{
			Inspected view(world.scene->FindObjectByName("Host"));
			ASSERT_TRUE(view.panel.IsValid());
			view.Retype(view.RowOf("title"), "start", "typed");
			view.Retype(view.BodyOf("stats"), "1.5", "9.5");
			view.Retype(view.BodyOf("stats"), "base", "king");
			view.Retype(view.BodyOf("stats"), "0", "7");
			view.Pick(view.BodyOf("stats"), 2);
			view.Retype(view.BodyOf("loadout"), "none", "sword");
			view.Retype(view.BodyOf("wide"), "3", "12");
		}
		str row = Kinds(host);
		EXPECT_EQ(Part(row, "title"), "typed");
		EXPECT_EQ(Part(row, "stats"), "5/9.5/king/7/Run/0");
		EXPECT_EQ(Part(row, "loadout"), str("sword/7/[]/") + STATS_EMPTY + "/null");
		EXPECT_EQ(Part(row, "wide"), "12/0/1");

		ASSERT_TRUE(SceneSystem::SaveScene(world.scene, Path("project://nested.scene")));
		SceneSystem::UnloadScene(world.scene);
		SceneSystem::EndFrame();
		world.scene = SceneSystem::LoadScene(Path("project://nested.scene"));
		ASSERT_TRUE(world.scene.IsValid());
		row = Kinds(host);
		EXPECT_EQ(Part(row, "stats"), "5/9.5/king/7/Run/0");
		EXPECT_EQ(Part(row, "loadout"), str("sword/7/[]/") + STATS_EMPTY + "/null");
		EXPECT_EQ(Part(row, "wide"), "12/0/1");
	}

	constexpr FieldScenario Scenarios[] = {
		{"DescribeHolder", &DescribeHolder},
		{"KindDefaultsReachBothSides", &KindDefaultsReachBothSides},
		{"KindsSurviveSaveAndLoad", &KindsSurviveSaveAndLoad},
		{"KindsSurviveInstantiate", &KindsSurviveInstantiate},
		{"EditorWritesReachEveryKind", &EditorWritesReachEveryKind},
		{"UnchangedStringIsNotPushed", &UnchangedStringIsNotPushed},
		{"InheritedTextFieldsSyncApart", &InheritedTextFieldsSyncApart},
		{"NullClassValueIsReplaced", &NullClassValueIsReplaced},
		{"WideValueStaysAlignedInAList", &WideValueStaysAlignedInAList},
		{"WrongComponentTypePullsNull", &WrongComponentTypePullsNull},
		{"ReferenceToDestroyedObjectReadsNull", &ReferenceToDestroyedObjectReadsNull},
		{"KindOverridesSurvivePrefabReload", &KindOverridesSurvivePrefabReload},
		{"InspectorDrawsEveryKind", &InspectorDrawsEveryKind},
		{"EnumShowsConstantNames", &EnumShowsConstantNames},
		{"ReferencesArePickedInInspector", &ReferencesArePickedInInspector},
		{"ListResizesFromInspector", &ListResizesFromInspector},
		{"NestedValueRoundTrip", &NestedValueRoundTrip},
	};

} // anonymous namespace

namespace Shalltear::CSharpScenarios {

	std::span<const FieldScenario> GetKindScenarios() {
		return Scenarios;
	}

} // namespace Shalltear::CSharpScenarios
