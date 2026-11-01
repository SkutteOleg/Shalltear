#include "TestsPCH.h"

#include "Shalltear/Editor/EditorIcons.h"

#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/FullScreenPass.h"
#include "Shalltear/Core/Graphics/ObjectPass.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/ReflectionProbe.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/ComponentMenu.h"
#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PlayModeController.h"
#include "Shalltear/Editor/ProjectOps.h"

#include <chrono>
#include <filesystem>
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/PropertyRows.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/TypeNameOps.h"
#include "Shalltear/Editor/UndoSystem.h"

using namespace Shalltear;
using namespace UITest;

namespace {

	std::filesystem::path InspectorRepoDir(const char* relative) {
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

	class InspTestComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspTestComponent, Component)
		SLT_REFLECT_FIELD(speed, SLT_RANGE(0.0f, 10.0f))
		SLT_REFLECT_FIELD(count, SLT_TOOLTIP("How many charges the ability starts with"))
		SLT_REFLECT_FIELD(title)
		SLT_REFLECT_FIELD(offset)
		SLT_REFLECT_FIELD(flag)
		SLT_REFLECT_FIELD(targetRef)
		SLT_REFLECT_FIELD(secret, SLT_HIDDEN)
		SLT_REFLECT_FIELD(fixed, SLT_READONLY)
		SLT_END_REFLECT()

		InspTestComponent() = default;

		f32 speed = 1.0f;
		i32 count = 2;
		str title = "hello";
		Vec3 offset{};
		bool flag = false;
		Handle<SceneObject> targetRef;
		i32 secret = 0;
		f32 fixed = 3.0f;
	};

	class InspGoneComponent : public Component {
	public:
		i32 charges = 5;

		SLT_REFLECT_COMPONENT(InspGoneComponent, Component)
		SLT_REFLECT_FIELD(charges)
		SLT_END_REFLECT()

		InspGoneComponent() = default;
	};

	class InspVecTestComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspVecTestComponent, Component)
		SLT_REFLECT_FIELD(gridSize)
		SLT_END_REFLECT()

		InspVecTestComponent() = default;

		Vec2I gridSize{4, 3};
	};

	class InspQuatTestComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspQuatTestComponent, Component)
		SLT_REFLECT_FIELD(tilt)
		SLT_END_REFLECT()

		InspQuatTestComponent() = default;

		Quat tilt = Quat::FromEuler(Vec3{-88.0f, 30.0f, 0.0f});
	};

	class InspBareComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspBareComponent, Component)
		SLT_REFLECT_FIELD(internal, SLT_HIDDEN)
		SLT_END_REFLECT()

		InspBareComponent() = default;

		i32 internal = 0;
	};

	bool AnyTextContains(UI::Context& context, UI::ElementHandle from, std::string_view needle) {
		if (context.GetTextString(from).find(needle) != str::npos) {
			return true;
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return false;
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			if (AnyTextContains(context, handle, needle)) {
				return true;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return false;
	}

	class InspectorTest : public ::testing::Test {
	protected:
		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			InspTestComponent::GetReflectType();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorAssetOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			PanelManager::Initialize();
			_scene = SceneSystem::CreateScene("InspectorFixture");
		}
		void TearDown() override {
			PanelManager::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			EditorAssetOps::Terminate();
			SceneSystem::Terminate();
		}

		static u32 CountByClass(UI::Context& context, UI::ElementHandle from, StringHash klass) {
			u32 count = context.HasClass(from, klass) ? 1u : 0u;
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return count;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				count += CountByClass(context, handle, klass);
				child = context.GetElement(handle)->nextSibling;
			}
			return count;
		}

		static void CollectDropTargets(UI::Context& context, UI::ElementHandle from,
									   Vector<UI::ElementHandle>& out) {
			if (context.Events(from).onDrop) {
				out.push_back(from);
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectDropTargets(context, handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static void CollectDragSources(UI::Context& context, UI::ElementHandle from,
									   Vector<UI::ElementHandle>& out) {
			if (context.Events(from).onPressDrag) {
				out.push_back(from);
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectDragSources(context, handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static void CollectByClass(UI::Context& context, UI::ElementHandle from, StringHash klass,
								   Vector<UI::ElementHandle>& out) {
			if (context.HasClass(from, klass)) {
				out.push_back(from);
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectByClass(context, handle, klass, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static f32 WorstRightOverflow(UI::Context& context, UI::ElementHandle from, f32 rightEdge) {
			const UI::Rect& box = context.GetLayout(from).borderBox;
			f32 worst = box.w > 0.0f ? (box.x + box.w) - rightEdge : 0.0f;
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return worst;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				worst = std::max(worst, WorstRightOverflow(context, handle, rightEdge));
				child = context.GetElement(handle)->nextSibling;
			}
			return worst;
		}

		static str FirstTextFieldText(UI::Context& context, UI::ElementHandle from) {
			if (context.HasClass(from, StringHash("slt-textfield"))) {
				return context.GetTextString(from);
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return {};
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				str found = FirstTextFieldText(context, handle);
				if (!found.empty()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		Handle<EditorPanel> OpenInspector() {
			return PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
		}

		static UI::ElementHandle CardKebab(UI::Context& context, UI::ElementHandle from) {
			const Handle<VectorAsset> morePicture = EditorIcons::GetGlyph(EditorIcons::Glyph::More);
			Vector<UI::ElementHandle> cards{GetMemoryResource(MemTag::Editor)};
			CollectByClass(context, from, EditorTheme::Class::ComponentCard, cards);
			for (UI::ElementHandle card : cards) {
				Vector<UI::ElementHandle> buttons{GetMemoryResource(MemTag::Editor)};
				CollectByClass(context, card, StringHash("slt-button"), buttons);
				for (UI::ElementHandle button : buttons) {
					if (UITest::FindByIcon(context, button, morePicture).IsValid()) {
						return button;
					}
				}
			}
			return {};
		}

		static UI::ElementHandle MenuEntry(UI::Context& context, std::string_view label) {
			Vector<UI::ElementHandle> entries{GetMemoryResource(MemTag::Editor)};
			CollectByClass(context, context.GetRoot(), StringHash("slt-menu-item"), entries);
			for (UI::ElementHandle entry : entries) {
				if (AnyTextContains(context, entry, label)) {
					return entry;
				}
			}
			return {};
		}

		Handle<Scene> _scene;
	};

	class GestureWatcher : public Object {
	public:
		u32 begins = 0;
		u32 ends = 0;
		Vector<Handle<SceneObject>> lastObjects{GetMemoryResource(MemTag::Editor)};

		void OnGesture(const EditorTransformGestureEvent& event) {
			if (event.objects.empty()) {
				++ends;
				lastObjects.clear();
				return;
			}
			++begins;
			lastObjects.assign(event.objects.begin(), event.objects.end());
		}
	};

	class PingWatcher : public Object {
	public:
		u32 count = 0;
		EditorPingTarget last;

		void OnPing(const EditorPingRequestedEvent& event) {
			++count;
			last = event.target;
		}
	};

} // anonymous namespace

TEST_F(InspectorTest, ScrubbingTransformRowBracketsTransformGesture) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	GestureWatcher* watcher = New<GestureWatcher>(MemTag::Editor);
	EditorEvents::TransformGesture.Subscribe(EventBus::GetGlobal(), Handle<GestureWatcher>(watcher),
											 &GestureWatcher::OnGesture);

	UI::ElementHandle scrubLabel = UITest::FindByClass(context, root, StringHash("slt-number-label"), 0);
	ASSERT_TRUE(scrubLabel.IsValid());
	const UI::Rect& box = context.GetLayout(scrubLabel).borderBox;
	const i32 x = static_cast<i32>(box.x + box.w * 0.5f);
	const i32 y = static_cast<i32>(box.y + box.h * 0.5f);

	InputEvent event{};
	event.type = InputEventType::MouseMove;
	event.mousePosPx = {x, y};
	context.InjectInput(event);
	event.type = InputEventType::MouseDown;
	event.button = MouseButton::Left;
	context.InjectInput(event);
	EXPECT_EQ(watcher->begins, 0u);

	event.type = InputEventType::MouseMove;
	event.mousePosPx = {x + 20, y};
	context.InjectInput(event);
	context.Update();
	EXPECT_EQ(watcher->begins, 1u);
	ASSERT_EQ(watcher->lastObjects.size(), 1u);
	EXPECT_EQ(watcher->lastObjects.front(), object);
	EXPECT_EQ(watcher->ends, 0u);

	event.type = InputEventType::MouseUp;
	context.InjectInput(event);
	context.Update();
	EXPECT_EQ(watcher->ends, 1u);
	EXPECT_EQ(watcher->begins, 1u);

	EventBus::GetGlobal().UnsubscribeAll(watcher->GetInstanceId());
	Delete(watcher);
}

TEST_F(InspectorTest, RowsHonorAttributesAndRebuildIdempotently) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);

	u32 numbers[2]{}, sliders[2]{}, checks[2]{}, toggles[2]{}, texts[2]{};
	for (int build = 0; build < 2; ++build) {
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();
		numbers[build] = CountByClass(context, root, StringHash("slt-numberfield"));
		sliders[build] = CountByClass(context, root, StringHash("slt-slider-track"));
		checks[build] = CountByClass(context, root, StringHash("slt-checkbox"));
		toggles[build] = CountByClass(context, root, StringHash("slt-toggle"));
		texts[build] = CountByClass(context, root, StringHash("slt-textfield"));
	}

	EXPECT_EQ(numbers[0], 14u);
	EXPECT_EQ(sliders[0], 1u);
	EXPECT_EQ(checks[0], 1u);
	EXPECT_EQ(toggles[0], 2u);
	EXPECT_EQ(texts[0], numbers[0] + 2u);

	EXPECT_EQ(numbers[0], numbers[1]);
	EXPECT_EQ(sliders[0], sliders[1]);
	EXPECT_EQ(checks[0], checks[1]);
	EXPECT_EQ(toggles[0], toggles[1]);
	EXPECT_EQ(texts[0], texts[1]);
}

TEST_F(InspectorTest, TooltipFieldShowsOnHover) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	FontAsset* font = New<FontAsset>(MemTag::Asset);
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	desc.tooltipFont = Handle<FontAsset>(font);
	desc.tooltipDelaySeconds = 0.0f;
	UI::Context context(desc, &fonts);

	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle labelElement = FindByText(context, root, "count");
	ASSERT_TRUE(labelElement.IsValid());
	UI::ElementHandle row = context.MakeHandle(context.GetElement(labelElement)->parent);
	const UI::Rect& box = context.GetLayout(row).borderBox;

	InputEvent move{};
	move.type = InputEventType::MouseMove;
	move.mousePosPx = {static_cast<i32>(box.x + box.w * 0.5f), static_cast<i32>(box.y + box.h * 0.5f)};
	context.InjectInput(move);
	context.Update();

	UI::ElementHandle card = FindByClass(context, context.GetRoot(), StringHash("slt-tooltip"), 0);
	ASSERT_TRUE(card.IsValid());
	EXPECT_EQ(context.GetTextString(card), "How many charges the ability starts with");
	Delete(font);
}

TEST_F(InspectorTest, NothingOverflowsANarrowPanel) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	const UI::Rect& panelBox = context.GetLayout(root).borderBox;
	ASSERT_NEAR(panelBox.w, 420.0f, 0.01f);
	f32 rightEdge = panelBox.x + panelBox.w;
	EXPECT_LE(WorstRightOverflow(context, root, rightEdge), 0.01f);

	Vector<UI::ElementHandle> tracks{GetMemoryResource(MemTag::UI)};
	CollectByClass(context, root, StringHash("slt-scroll-track"), tracks);
	ASSERT_EQ(tracks.size(), 1u);
	const UI::Rect& trackBox = context.GetLayout(tracks[0]).borderBox;
	EXPECT_NEAR(trackBox.x + trackBox.w, rightEdge, 0.01f);

	Vector<UI::ElementHandle> numbers{GetMemoryResource(MemTag::UI)};
	CollectByClass(context, root, StringHash("slt-numberfield"), numbers);
	ASSERT_GE(numbers.size(), 3u);
	f32 previousRight = 0.0f;
	for (u32 i = 0; i < 3; ++i) {
		const UI::Rect& box = context.GetLayout(numbers[i]).borderBox;
		EXPECT_GT(box.w, 0.0f) << "component " << i;
		EXPECT_LE(box.x + box.w, trackBox.x + 0.01f) << "component " << i;
		EXPECT_GE(box.x, previousRight - 0.01f) << "component " << i;
		previousRight = box.x + box.w;
	}
}

TEST_F(InspectorTest, StructureChangesGrowTheComponentList) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);

	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	u32 before = CountByClass(context, root, StringHash("slt-checkbox"));
	u32 beforeToggles = CountByClass(context, root, StringHash("slt-toggle"));

	object.Get()->AddComponent<InspTestComponent>();
	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	EXPECT_EQ(CountByClass(context, root2, StringHash("slt-checkbox")), before + 1);
	EXPECT_EQ(CountByClass(context, root2, StringHash("slt-toggle")), beforeToggles + 1);
}

TEST_F(InspectorTest, LockPinsTheTargetAcrossSelectionChanges) {
	Handle<SceneObject> first = SceneSystem::CreateObject("First", _scene);
	Handle<SceneObject> second = SceneSystem::CreateObject("Second", _scene);
	EditorSelection::Select(Handle<Object>(first.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	const TypeInfo* panelType = panel.Get()->GetType();

	Variant lockedValue(true);
	Variant pinnedId(first.GetInstanceId());
	ASSERT_TRUE(SetField(panel.Get(), panelType, "locked"_hash, &lockedValue));
	ASSERT_TRUE(SetField(panel.Get(), panelType, "lockedTargetId"_hash, &pinnedId));

	EditorSelection::Select(Handle<Object>(second.Get()));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	EXPECT_EQ(FirstTextFieldText(context, root), "First");

	Variant unlocked(false);
	SetField(panel.Get(), panelType, "locked"_hash, &unlocked);
	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	EXPECT_EQ(FirstTextFieldText(context, root2), "Second");
}

TEST_F(InspectorTest, ComponentCopyCommandSeedsReflectedState) {
	Handle<SceneObject> source = SceneSystem::CreateObject("Source", _scene);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	Handle<InspTestComponent> component = source.Get()->AddComponent<InspTestComponent>();
	component.Get()->speed = 7.5f;
	component.Get()->title = "copied";

	str stateJSON = TextSerializer::Serialize(component.Get(), component.Get()->GetType());
	UndoSystem::Push(MakeUnique<AddComponentCommand>(target, StringHash("InspTestComponent"), stateJSON));

	Handle<InspTestComponent> copy = target.Get()->GetComponent<InspTestComponent>();
	ASSERT_TRUE(copy.IsValid());
	EXPECT_FLOAT_EQ(copy.Get()->speed, 7.5f);
	EXPECT_EQ(copy.Get()->title, "copied");

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(target.Get()->GetComponent<InspTestComponent>().IsValid());
}

TEST_F(InspectorTest, HandleFieldRawCommandRoundTrips) {
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", _scene);
	Handle<SceneObject> pointee = SceneSystem::CreateObject("Pointee", _scene);
	Handle<InspTestComponent> component = host.Get()->AddComponent<InspTestComponent>();

	UndoSystem::Push(MakeUnique<SetRawFieldCommand>(
		Handle<Object>(component.Get()), "targetRef"_hash,
		0, pointee.GetInstanceId(), 8, "Set targetRef"));
	EXPECT_EQ(component.Get()->targetRef.GetInstanceId(), pointee.GetInstanceId());
	EXPECT_TRUE(component.Get()->targetRef.IsValid());

	UndoSystem::Undo();
	EXPECT_FALSE(component.Get()->targetRef.IsValid());
	UndoSystem::Redo();
	EXPECT_TRUE(component.Get()->targetRef.IsValid());
}

TEST_F(InspectorTest, MeshRendererUsesTheCustomInspector) {
	EXPECT_NE(PropertyDrawers::FindComponentInspector(StringHash("MeshRenderer")), nullptr);
	EXPECT_EQ(PropertyDrawers::FindComponentInspector(StringHash("NoSuchComponent")), nullptr);
}

TEST_F(InspectorTest, ReferenceFieldTakesDrop) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	ASSERT_NE(object.Get()->AddComponent<MeshRenderer>().Get(), nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ObjectField, fields);
	ASSERT_GE(fields.size(), 2u);

	Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
	CollectDropTargets(context, root, targets);
	for (UI::ElementHandle target : targets) {
		if (context.Events(target).onDragOver && !context.HasClass(target, EditorTheme::Class::ObjectField)) {
			continue;
		}
		EXPECT_TRUE(context.HasClass(target, EditorTheme::Class::ObjectField));
	}
	for (UI::ElementHandle field : fields) {
		EXPECT_TRUE(static_cast<bool>(context.Events(field).onDrop));
		EXPECT_TRUE(static_cast<bool>(context.Events(field).onDragOver));
	}

	UI::ElementHandle picker = FindByClass(context, fields[0], EditorTheme::Class::ObjectFieldButton, 0);
	ASSERT_TRUE(picker.IsValid());
	ClickElement(context, picker);
	context.Update();
	EXPECT_TRUE(AnyTextContains(context, context.GetRoot(), "None"));
}

TEST_F(InspectorTest, MeshRendererRowsTakeDrops) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	MeshAsset mesh;
	MaterialAsset material;

	Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
	CollectDropTargets(context, root, targets);
	ASSERT_GE(targets.size(), 2u);

	UI::DragPayload meshPayload;
	meshPayload.type = "Asset"_hash;
	meshPayload.display = "Cube.primitive";
	bool droppedMesh = false;
	for (UI::ElementHandle row : targets) {
		auto& events = context.Events(row);
		if (!events.onDrop) {
			continue;
		}
		UI::DragPayload objectPayload;
		objectPayload.type = "Component"_hash;
		objectPayload.data = Variant(mesh.GetInstanceId());
		if (events.onDragOver && events.onDragOver(objectPayload)) {
			events.onDrop(objectPayload, {});
			droppedMesh = true;
			break;
		}
	}
	ASSERT_TRUE(droppedMesh);
	EXPECT_EQ(renderer->GetMesh().GetInstanceId(), mesh.GetInstanceId());

	UndoSystem::Undo();
	EXPECT_FALSE(renderer->GetMesh().IsValid());
	UndoSystem::Redo();
	EXPECT_EQ(renderer->GetMesh().GetInstanceId(), mesh.GetInstanceId());
}

TEST_F(InspectorTest, MaterialSlotAssignableAndUndoable) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	MaterialAsset material;
	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(material.GetInstanceId());

	Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
	CollectDropTargets(context, root, targets);
	bool dropped = false;
	for (UI::ElementHandle row : targets) {
		auto& events = context.Events(row);
		if (events.onDragOver && events.onDrop && events.onDragOver(payload)) {
			events.onDrop(payload, {});
			dropped = true;
			break;
		}
	}
	ASSERT_TRUE(dropped);

	EXPECT_EQ(renderer->GetMaterial(0).GetInstanceId(), material.GetInstanceId());
	UndoSystem::Undo();
	EXPECT_FALSE(renderer->GetMaterial(0).IsValid());
}

TEST_F(InspectorTest, EveryMaterialSlotHasRow) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	MeshAsset mesh;
	SubMesh first;
	first.firstIndex = 0;
	first.indexCount = 3;
	first.materialSlot = 0;
	SubMesh second;
	second.firstIndex = 3;
	second.indexCount = 3;
	second.materialSlot = 1;
	mesh.submeshes.push_back(first);
	mesh.submeshes.push_back(second);
	renderer->SetMesh(Handle<MeshAsset>(&mesh));

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	MaterialAsset material;
	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(material.GetInstanceId());

	Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
	CollectDropTargets(context, root, targets);
	Vector<UI::ElementHandle> accepting{GetMemoryResource(MemTag::Editor)};
	for (UI::ElementHandle row : targets) {
		auto& events = context.Events(row);
		if (events.onDragOver && events.onDrop && events.onDragOver(payload)) {
			accepting.push_back(row);
		}
	}
	ASSERT_GE(accepting.size(), 2u);

	context.Events(accepting[1]).onDrop(payload, {});
	EXPECT_EQ(renderer->GetMaterial(1).GetInstanceId(), material.GetInstanceId());
	EXPECT_FALSE(renderer->GetMaterial(0).IsValid());

	UndoSystem::Undo();
	EXPECT_FALSE(renderer->GetMaterial(1).IsValid());
	EXPECT_TRUE(renderer->GetMaterialCount() == 0);

	renderer->SetMesh({});
}

TEST_F(InspectorTest, AssigningMeshGrowsSlotRowsWithoutReselecting) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	MaterialAsset material;
	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(material.GetInstanceId());

	auto countMaterialRows = [&]() {
		Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
		CollectDropTargets(context, root, targets);
		u32 accepting = 0;
		for (UI::ElementHandle row : targets) {
			auto& events = context.Events(row);
			if (events.onDragOver && events.onDrop && events.onDragOver(payload)) {
				++accepting;
			}
		}
		return accepting;
	};

	EXPECT_EQ(countMaterialRows(), 1u);

	MeshAsset mesh;
	for (u32 i = 0; i < 4; ++i) {
		SubMesh submesh;
		submesh.firstIndex = i * 3;
		submesh.indexCount = 3;
		submesh.materialSlot = i;
		mesh.submeshes.push_back(submesh);
	}
	renderer->SetMesh(Handle<MeshAsset>(&mesh));

	panel.Get()->OnTick(0.016f);
	context.Update();

	EXPECT_EQ(countMaterialRows(), 4u);

	renderer->SetMesh({});
}

TEST_F(InspectorTest, SponzaSlotsBindable) {
	static constexpr u32 MATERIAL_COUNT = 25;
	static constexpr u32 SUBMESH_COUNT = 103;

	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	MeshAsset mesh;
	for (u32 i = 0; i < SUBMESH_COUNT; ++i) {
		SubMesh submesh;
		submesh.firstIndex = i * 3;
		submesh.indexCount = 3;
		submesh.materialSlot = i % MATERIAL_COUNT;
		mesh.submeshes.push_back(submesh);
	}
	renderer->SetMesh(Handle<MeshAsset>(&mesh));

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 4000};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	MaterialAsset material;
	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(material.GetInstanceId());

	Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
	CollectDropTargets(context, root, targets);
	Vector<UI::ElementHandle> accepting{GetMemoryResource(MemTag::Editor)};
	for (UI::ElementHandle row : targets) {
		auto& events = context.Events(row);
		if (events.onDragOver && events.onDrop && events.onDragOver(payload)) {
			accepting.push_back(row);
		}
	}
	ASSERT_EQ(accepting.size(), MATERIAL_COUNT);

	context.Events(accepting[MATERIAL_COUNT - 1]).onDrop(payload, {});
	EXPECT_EQ(renderer->GetMaterial(MATERIAL_COUNT - 1).GetInstanceId(), material.GetInstanceId());
	EXPECT_EQ(renderer->GetMaterialCount(), MATERIAL_COUNT);

	UndoSystem::Undo();
	EXPECT_FALSE(renderer->GetMaterial(MATERIAL_COUNT - 1).IsValid());
	EXPECT_TRUE(renderer->GetMaterialCount() == 0);

	renderer->SetMesh({});
}

TEST_F(InspectorTest, SkinnedRendererRowPerBlendShape) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> rendererHandle = object.Get()->AddComponent<SkinnedMeshRenderer>();
	SkinnedMeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	MeshAsset mesh;
	SubMesh first;
	first.firstIndex = 0;
	first.indexCount = 3;
	first.materialSlot = 0;
	SubMesh second;
	second.firstIndex = 3;
	second.indexCount = 3;
	second.materialSlot = 1;
	mesh.submeshes.push_back(first);
	mesh.submeshes.push_back(second);

	MeshBlendShape raise;
	raise.name = "Raise";
	MeshBlendShape smile;
	smile.name = "Smile";
	mesh.blendShapes.push_back(std::move(raise));
	mesh.blendShapes.push_back(std::move(smile));
	renderer->SetMesh(Handle<MeshAsset>(&mesh));
	renderer->SetBlendShapeWeightCount(0);

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "Material 0"));
	EXPECT_TRUE(AnyTextContains(context, root, "Material 1"));
	EXPECT_TRUE(AnyTextContains(context, root, "Skin"));
	EXPECT_TRUE(AnyTextContains(context, root, "Skeleton"));

	EXPECT_TRUE(AnyTextContains(context, root, "BlendShapes"));
	EXPECT_TRUE(AnyTextContains(context, root, "Raise"));
	EXPECT_TRUE(AnyTextContains(context, root, "Smile"));
	EXPECT_FALSE(AnyTextContains(context, root, "Element 0"));
}

TEST_F(InspectorTest, MeshRendererDrawsRenderingLayerMaskRow) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Plain", _scene);
	ASSERT_NE(object.Get()->AddComponent<MeshRenderer>().Get(), nullptr);

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "renderingLayerMask"));
}

TEST_F(InspectorTest, ShapelessSkinnedKeepsLayerMask) {
	MeshAsset bare;
	SubMesh only;
	only.firstIndex = 0;
	only.indexCount = 3;
	only.materialSlot = 0;
	bare.submeshes.push_back(only);

	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> renderer = object.Get()->AddComponent<SkinnedMeshRenderer>();
	ASSERT_NE(renderer.Get(), nullptr);
	renderer.Get()->SetMesh(Handle<MeshAsset>(&bare));
	ASSERT_TRUE(bare.blendShapes.empty());

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "renderingLayerMask"));
}

TEST_F(InspectorTest, ShapelessMeshHasNoShapeSection) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> rendererHandle = object.Get()->AddComponent<SkinnedMeshRenderer>();
	ASSERT_NE(rendererHandle.Get(), nullptr);
	MeshAsset mesh;
	SubMesh only;
	only.indexCount = 3;
	mesh.submeshes.push_back(only);
	rendererHandle.Get()->SetMesh(Handle<MeshAsset>(&mesh));

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "Material 0"));
	EXPECT_FALSE(AnyTextContains(context, root, "BlendShapes"));
}

TEST_F(InspectorTest, ShapeRowGrowsWeightList) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> rendererHandle = object.Get()->AddComponent<SkinnedMeshRenderer>();
	SkinnedMeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	MeshAsset mesh;
	SubMesh only;
	only.indexCount = 3;
	mesh.submeshes.push_back(only);
	MeshBlendShape up;
	up.name = "Up";
	MeshBlendShape down;
	down.name = "Down";
	mesh.blendShapes.push_back(std::move(up));
	mesh.blendShapes.push_back(std::move(down));
	renderer->SetMesh(Handle<MeshAsset>(&mesh));
	renderer->SetBlendShapeWeightCount(0);
	ASSERT_TRUE(renderer->GetBlendShapeWeightCount() == 0);

	Handle<Object> target(static_cast<Object*>(renderer));
	f32 before = 0.0f;
	f32 after = 65.0f;
	u64 beforeBits = 0;
	u64 afterBits = 0;
	std::memcpy(&beforeBits, &before, sizeof(f32));
	std::memcpy(&afterBits, &after, sizeof(f32));
	UndoSystem::Push(MakeUnique<SetContainerElementCommand>(
		target, StringHash("_blendShapeWeights"), 1, beforeBits, afterBits, 0, "Set Down"));

	ASSERT_EQ(renderer->GetBlendShapeWeightCount(), 2u);
	EXPECT_FLOAT_EQ(renderer->GetBlendShapeWeight(static_cast<u32>(1)), 65.0f);
	EXPECT_FLOAT_EQ(renderer->GetBlendShapeWeight(static_cast<u32>(0)), 0.0f);

	UndoSystem::Undo();
	EXPECT_TRUE(renderer->GetBlendShapeWeightCount() == 0);
}

TEST_F(InspectorTest, TypedShapeWeightWrites) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> rendererHandle = object.Get()->AddComponent<SkinnedMeshRenderer>();
	SkinnedMeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);

	MeshAsset mesh;
	SubMesh only;
	only.indexCount = 3;
	mesh.submeshes.push_back(only);
	MeshBlendShape up;
	up.name = "Up";
	mesh.blendShapes.push_back(std::move(up));
	renderer->SetMesh(Handle<MeshAsset>(&mesh));
	renderer->SetBlendShapeWeightCount(0);

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> inputs{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, StringHash("slt-number-input"), inputs);
	ASSERT_FALSE(inputs.empty());
	UI::ElementHandle field = inputs.back();

	ClickElement(context, field);
	InputEvent selectAll{};
	selectAll.type = InputEventType::KeyDown;
	selectAll.key = Key::A;
	selectAll.mods = KeyMod::Ctrl;
	context.InjectInput(selectAll);
	for (char c : std::string_view("60")) {
		InputEvent typed{};
		typed.type = InputEventType::Text;
		typed.codepoint = static_cast<char32_t>(c);
		context.InjectInput(typed);
	}
	InputEvent commit{};
	commit.type = InputEventType::KeyDown;
	commit.key = Key::Enter;
	context.InjectInput(commit);
	context.Update();

	ASSERT_EQ(renderer->GetBlendShapeWeightCount(), 1u);
	EXPECT_FLOAT_EQ(renderer->GetBlendShapeWeight(static_cast<u32>(0)), 60.0f);

	UndoSystem::Undo();
	EXPECT_TRUE(renderer->GetBlendShapeWeightCount() == 0);
}

TEST_F(InspectorTest, BindDefaultsRebindsDangling) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Character", _scene);
	Handle<SkinnedMeshRenderer> rendererHandle = object.Get()->AddComponent<SkinnedMeshRenderer>();
	SkinnedMeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);
	ASSERT_TRUE(renderer->GetMaterialCount() == 0);

	MeshAsset mesh;
	SubMesh only;
	only.indexCount = 3;
	only.materialSlot = 0;
	mesh.submeshes.push_back(only);
	mesh.defaultMaterials.push_back(Guid{});
	renderer->SetMesh(Handle<MeshAsset>(&mesh));

	Handle<Object> target(static_cast<Object*>(renderer));

	EXPECT_EQ(EditorSceneOps::BindDefaultMaterials(target, mesh.GetInstanceId()), 0u);

	MaterialAsset live;
	renderer->SetMaterial(0, Handle<MaterialAsset>(&live));
	EXPECT_EQ(EditorSceneOps::BindDefaultMaterials(target, mesh.GetInstanceId()), 0u);
	EXPECT_EQ(renderer->GetMaterial(static_cast<u32>(0)).Get(), &live);

	{
		MaterialAsset doomed;
		renderer->SetMaterial(static_cast<u32>(0), Handle<MaterialAsset>(&doomed));
		ASSERT_TRUE(renderer->GetMaterial(static_cast<u32>(0)).IsValid());
	}
	ASSERT_NE(renderer->GetMaterial(static_cast<u32>(0)).GetInstanceId(), 0u);
	EXPECT_FALSE(renderer->GetMaterial(static_cast<u32>(0)).IsValid());
}

TEST_F(InspectorTest, SkinnedMeshRendererUsesTheCustomInspector) {
	EXPECT_NE(PropertyDrawers::FindComponentInspector(StringHash("SkinnedMeshRenderer")), nullptr);
}

TEST_F(InspectorTest, NothingOverflowsADockedPanel) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {1920, 1030};
	desc.contentScale = 1.25f;
	UI::Context context(desc, &fonts);

	UI::Style rootStyle;
	rootStyle.flexDirection = UI::FlexDirection::Column;
	rootStyle.alignItems = UI::Align::Stretch;
	context.SetStyle(context.GetRoot(), rootStyle);

	UI::ElementHandle menuBar = context.CreateElement();
	context.AppendChild(context.GetRoot(), menuBar);
	UI::Style menuStyle;
	menuStyle.height = UI::Length::Pixel(30.0f);
	menuStyle.width = UI::Length::Percent(1.0f);
	context.SetStyle(menuBar, menuStyle);

	UI::ElementHandle host = context.CreateElement();
	context.AppendChild(context.GetRoot(), host);
	UI::Style hostStyle;
	hostStyle.height = UI::Length::Pixel(0.0f);
	hostStyle.flexGrow = 1.0f;
	hostStyle.alignItems = UI::Align::Stretch;
	context.SetStyle(host, hostStyle);

	UI::DockManager dock(context, Handle<FontAsset>{});
	UI::ElementHandle panelRoot;
	dock.RegisterPanel(StringHash("left"), "Left", [](UI::Context&, UI::ElementHandle) {});
	dock.RegisterPanel(StringHash("inspector"), "Inspector",
					   [&panelRoot, &panel](UI::Context& c, UI::ElementHandle parent) {
						   panelRoot = parent;
						   panel.Get()->BuildContent(c, parent);
					   });
	dock.DockPanel(StringHash("left"), dock.GetRootNode(), UI::DockZone::Center);
	dock.DockPanel(StringHash("inspector"), dock.GetRootNode(), UI::DockZone::Right);
	Vector<UI::DockNodeDesc> layout{GetMemoryResource(MemTag::UI)};
	dock.SaveLayout(layout);
	ASSERT_GE(layout.size(), 1u);
	layout[0].ratio = 0.78f;
	ASSERT_TRUE(dock.RestoreLayout({layout.data(), layout.size()}));
	dock.Build(host);
	context.Update();

	ASSERT_TRUE(panelRoot.IsValid());
	const UI::Rect& leaf = context.GetLayout(panelRoot).content;
	ASSERT_GT(leaf.w, 0.0f);
	ASSERT_LT(leaf.w, 460.0f);
	EXPECT_LE(WorstRightOverflow(context, panelRoot, leaf.x + leaf.w), 0.01f)
		<< "leaf " << leaf.x << " w " << leaf.w;

	Vector<UI::ElementHandle> numbers{GetMemoryResource(MemTag::UI)};
	CollectByClass(context, panelRoot, StringHash("slt-numberfield"), numbers);
	ASSERT_GE(numbers.size(), 3u);
	for (u32 i = 0; i < 3; ++i) {
		const UI::Rect& box = context.GetLayout(numbers[i]).borderBox;
		EXPECT_GT(box.w, 0.0f) << "component " << i;
		EXPECT_LE(box.x + box.w, leaf.x + leaf.w + 0.01f) << "component " << i;
	}
}

namespace {

	constexpr u32 ADD_COMPONENT_CAP_FOR_TEST = 24;

	UI::ElementHandle FindElementByText(UI::Context& context, UI::ElementHandle from, std::string_view text) {
		if (context.GetTextString(from) == text) {
			return from;
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return {};
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			UI::ElementHandle found = FindElementByText(context, handle, text);
			if (context.IsAlive(found)) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	UI::ElementHandle FindElementByTextContaining(UI::Context& context, UI::ElementHandle from,
												  std::string_view needle) {
		if (context.GetTextString(from).find(needle) != std::string_view::npos) {
			return from;
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return {};
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			UI::ElementHandle found = FindElementByTextContaining(context, handle, needle);
			if (context.IsAlive(found)) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	void CollectTexts(UI::Context& context, UI::ElementHandle from, Vector<str>& out) {
		str text = context.GetTextString(from);
		if (!text.empty()) {
			out.push_back(text);
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return;
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			CollectTexts(context, handle, out);
			child = context.GetElement(handle)->nextSibling;
		}
	}

	bool ContainsText(const Vector<str>& texts, std::string_view needle) {
		for (const str& text : texts) {
			if (text.find(needle) != str::npos) {
				return true;
			}
		}
		return false;
	}

	bool ExactText(const Vector<str>& texts, std::string_view whole) {
		for (const str& text : texts) {
			if (text == whole) {
				return true;
			}
		}
		return false;
	}

	InputEvent InspectorMouseDown(Vec2 point) {
		InputEvent event{};
		event.type = InputEventType::MouseDown;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
		return event;
	}

	InputEvent InspectorMouseUp(Vec2 point) {
		InputEvent event{};
		event.type = InputEventType::MouseUp;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
		return event;
	}

	void PressAndRelease(UI::Context& context, UI::ElementHandle element) {
		const UI::Rect& box = context.GetLayout(element).borderBox;
		Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
		context.InjectInput(InspectorMouseDown(center));
		context.InjectInput(InspectorMouseUp(center));
		context.Update();
	}

	void PressAway(UI::Context& context, UI::ElementHandle root) {
		UI::ElementHandle strip = FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
		const UI::Rect& box = context.GetLayout(strip).borderBox;
		Vec2 point{box.x + box.w - 4.0f, box.y + box.h * 0.5f};
		context.InjectInput(InspectorMouseDown(point));
		context.InjectInput(InspectorMouseUp(point));
		context.Update();
	}

} // anonymous namespace

TEST_F(InspectorTest, AddComponentGroupsUntruncated) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	const UI::Element* labelElement = context.GetElement(addLabel);
	ASSERT_NE(labelElement, nullptr);
	UI::ElementHandle addButton = context.MakeHandle(labelElement->parent);

	PressAndRelease(context, addButton);
	context.Update();

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, context.GetRoot(), texts);

	EXPECT_TRUE(ContainsText(texts, "Engine"));

	u32 componentTypes = 0;
	TypeRegistry::ForEachType(
		[](const TypeInfo* type, void* user) {
			if ((type->flags & TypeFlagComponent) == 0 || (type->flags & TypeFlagAbstract) != 0 || !type->createFn) {
				return;
			}
			if (!ComponentMenu::CategoryFor(type).empty()) {
				return;
			}
			++*static_cast<u32*>(user);
		},
		&componentTypes);
	EXPECT_EQ(ContainsText(texts, "more (keep typing"), componentTypes > ADD_COMPONENT_CAP_FOR_TEST) << componentTypes;
}

TEST_F(InspectorTest, AddComponentTakesTypingMomentItOpens) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Typed", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	PressAndRelease(context, context.MakeHandle(context.GetElement(addLabel)->parent));
	context.Update();
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	UI::ElementHandle search = FindByClass(context, context.GetOpenPopup(0), StringHash("slt-textfield"), 0);
	ASSERT_TRUE(context.IsAlive(search));

	for (char32_t ch : std::u32string_view(U"Insp")) {
		InputEvent typed{};
		typed.type = InputEventType::Text;
		typed.codepoint = ch;
		context.InjectInput(typed);
	}
	context.Update();
	EXPECT_EQ(context.GetTextString(search), "Insp");
}

TEST_F(InspectorTest, AddComponentEmptyStateOutranksFilterWording) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	UI::ElementHandle addButton = context.MakeHandle(context.GetElement(addLabel)->parent);
	PressAndRelease(context, addButton);
	context.Update();

	UI::ElementHandle search;
	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-textfield"), fields);
	for (UI::ElementHandle field : fields) {
		if (context.GetTextString(field).empty()) {
			search = field;
		}
	}
	ASSERT_TRUE(context.IsAlive(search));
	ASSERT_TRUE(static_cast<bool>(context.Events(search).onTextChange));
	context.Events(search).onTextChange("zzzznotacomponent");
	context.Update();

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, context.GetRoot(), texts);
	EXPECT_TRUE(ContainsText(texts, "no C++ module"));
	EXPECT_FALSE(ContainsText(texts, "(no matching components)"));
}

namespace {

	class ScriptedProjectFixture : public InspectorTest {
	protected:
		std::filesystem::path _projectRoot;

		void MountTempProject() {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_inspproj_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot / "Assets");

			Config projectFile;
			projectFile.SetString("Project", "name", "MenuProbe");
			projectFile.Save(Path(_projectRoot.generic_string() + "/Project.ini"));

			ASSERT_TRUE(ProjectOps::MountProject(_projectRoot.generic_string()));

			std::filesystem::path engineAssets = InspectorRepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			}
		}

		void UnmountTempProject() {
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		void CollectPopupTexts(UI::Context& context, Vector<str>& out) {
			Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
			EditorSelection::Select(Handle<Object>(object.Get()));

			Handle<EditorPanel> panel = OpenInspector();
			ASSERT_TRUE(panel.IsValid());
			UI::ElementHandle root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			panel.Get()->BuildContent(context, root);
			context.Update();

			UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
			ASSERT_TRUE(context.IsAlive(addLabel));
			PressAndRelease(context, context.MakeHandle(context.GetElement(addLabel)->parent));
			context.Update();
			CollectTexts(context, context.GetRoot(), out);
		}
	};

} // anonymous namespace

TEST_F(ScriptedProjectFixture, EmptyStateReportsWhileFiltering) {
	MountTempProject();

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectPopupTexts(context, texts);

	UI::ElementHandle search;
	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-textfield"), fields);
	for (UI::ElementHandle field : fields) {
		if (context.GetTextString(field).empty()) {
			search = field;
		}
	}
	ASSERT_TRUE(context.IsAlive(search));
	ASSERT_TRUE(static_cast<bool>(context.Events(search).onTextChange));
	context.Events(search).onTextChange("CharacterController");
	context.Update();

	texts.clear();
	CollectTexts(context, context.GetRoot(), texts);

	EXPECT_TRUE(ContainsText(texts, "no C++ module"));
	EXPECT_TRUE(ContainsText(texts, "Create C++ Module"));
	EXPECT_FALSE(ContainsText(texts, "(no matching components)"));

	UnmountTempProject();
}

TEST_F(ScriptedProjectFixture, ScaffoldedProjectIsRecognizedByScriptingSurfaces) {
	MountTempProject();

	str projectPath = _projectRoot.generic_string();
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MenuProbe";
	options.includeMachineScopedFiles = false;
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(projectPath, options, scaffolded));

	EXPECT_EQ(ProjectOps::GetMountedProjectPath(), projectPath);
	EXPECT_TRUE(ScriptProjectOps::HasScriptProject(ProjectOps::GetMountedProjectPath()));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectPopupTexts(context, texts);

	EXPECT_FALSE(ContainsText(texts, "no C++ module"));
	EXPECT_TRUE(ContainsText(texts, "New C++ Component..."));
	EXPECT_TRUE(ContainsText(texts, "New C# Component..."));

	UnmountTempProject();
}

TEST_F(ScriptedProjectFixture, CreateRowRuledOff) {
	MountTempProject();

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "RuleProbe";
	options.includeMachineScopedFiles = false;
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(_projectRoot.generic_string(), options, scaffolded));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	EditorTheme::ApplyToContext(context);

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectPopupTexts(context, texts);
	ASSERT_TRUE(ContainsText(texts, "New C++ Component..."));

	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	UI::ElementHandle card = context.GetOpenPopup(0);
	const UI::Element* cardElement = context.GetElement(card);
	ASSERT_NE(cardElement, nullptr);

	UI::ElementHandle previous;
	UI::ElementHandle beforeThat;
	UI::ElementHandle create;
	for (u32 child = cardElement->firstChild; child != UI::INVALID_UI_INDEX;) {
		UI::ElementHandle handle = context.MakeHandle(child);
		if (context.GetTextString(handle) == "New C++ Component...") {
			create = handle;
			break;
		}
		beforeThat = previous;
		previous = handle;
		child = context.GetElement(handle)->nextSibling;
	}
	ASSERT_TRUE(context.IsAlive(create));
	ASSERT_TRUE(context.IsAlive(previous));
	EXPECT_TRUE(context.HasClass(previous, StringHash("slt-separator")));
	EXPECT_TRUE(context.IsAlive(beforeThat) && context.GetElement(beforeThat)->firstChild != UI::INVALID_UI_INDEX);

	UnmountTempProject();
}

TEST_F(ScriptedProjectFixture, NewComponentDialogFocused) {
	MountTempProject();
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "FocusProbe";
	options.includeMachineScopedFiles = false;
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(_projectRoot.generic_string(), options, scaffolded));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectPopupTexts(context, texts);
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	UI::ElementHandle create = FindElementByText(context, context.GetOpenPopup(0), "New C++ Component...");
	ASSERT_TRUE(context.IsAlive(create));
	PressAndRelease(context, create);
	context.Update();
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);

	UI::ElementHandle nameField = FindByClass(context, context.GetOpenPopup(0), StringHash("slt-textfield"), 0);
	ASSERT_TRUE(context.IsAlive(nameField));
	for (char32_t ch : std::u32string_view(U"Coin")) {
		InputEvent typed{};
		typed.type = InputEventType::Text;
		typed.codepoint = ch;
		context.InjectInput(typed);
	}
	context.Update();
	EXPECT_EQ(context.GetTextString(nameField), "Coin");

	UnmountTempProject();
}

namespace {

	constexpr const char* RELOAD_MODULE_V1 = "ShalltearHotReloadModuleV1.dll";
	const StringHash ReloadBehavior{"HotReloadBehavior"};

	constexpr i32 PROBE_COUNTER = 4242;

	InputEvent InspectorTextChar(char32_t codepoint) {
		InputEvent event{};
		event.type = InputEventType::Text;
		event.codepoint = codepoint;
		return event;
	}

	InputEvent InspectorKeyPress(Key key, KeyMod mods = KeyMod::None) {
		InputEvent event{};
		event.type = InputEventType::KeyDown;
		event.key = key;
		event.mods = mods;
		return event;
	}

	class InspectorModuleReloadFixture : public InspectorTest {
	protected:
		std::filesystem::path _root;
		ModuleHandle _module;

		void SetUp() override {
			InspectorTest::SetUp();

			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_insprel_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			ModuleSystem::Initialize();

			_module = ModuleSystem::Load(Path(RELOAD_MODULE_V1));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
		}

		void TearDown() override {
			InspectorTest::TearDown();
			ModuleSystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		const TypeInfo* BehaviorType() const { return TypeRegistry::FindType(ReloadBehavior); }

		i32 LiveCounter(Handle<Component> probe) const {
			Object* object = probe.Get();
			EXPECT_NE(object, nullptr);
			Variant out;
			EXPECT_TRUE(object && GetField(object, object->GetType(), StringHash("counter"), &out));
			const i32* value = out.TryGet<i32>();
			return value ? *value : -1;
		}
	};

} // anonymous namespace

TEST_F(InspectorModuleReloadFixture, NumericRowsCommitAfterTheirModuleReloads) {
	const TypeInfo* behavior = BehaviorType();
	ASSERT_NE(behavior, nullptr);

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<Component> probe = object.Get()->AddComponent(behavior);
	ASSERT_TRUE(probe.IsValid());
	Variant seed{PROBE_COUNTER};
	ASSERT_TRUE(SetField(probe.Get(), behavior, StringHash("counter"), &seed));
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle field = FindElementByText(context, root, std::to_string(PROBE_COUNTER));
	ASSERT_TRUE(context.IsAlive(field));
	ASSERT_TRUE(context.HasClass(field, StringHash("slt-textfield")));

	Path v1{RELOAD_MODULE_V1};
	ASSERT_TRUE(ModuleSystem::Reload(_module, &v1));
	ASSERT_TRUE(probe.IsValid());
	EXPECT_EQ(LiveCounter(probe), PROBE_COUNTER);

	PressAndRelease(context, field);
	ASSERT_EQ(context.GetFocused(), field);
	context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
	context.InjectInput(InspectorTextChar(U'7'));
	context.InjectInput(InspectorTextChar(U'7'));
	context.Update();
	ASSERT_EQ(context.GetTextString(field), "77");

	PressAway(context, root);
	EXPECT_NE(context.GetFocused(), field);

	EXPECT_EQ(LiveCounter(probe), 77);

	ASSERT_TRUE(UndoSystem::CanUndo());
	UndoSystem::Undo();
	EXPECT_EQ(LiveCounter(probe), PROBE_COUNTER);
}

namespace {

	UI::DragPayload ComponentPayload(Handle<Component> component) {
		UI::DragPayload payload;
		payload.type = "Component"_hash;
		payload.data = Variant(component.GetInstanceId());
		payload.display = "component";
		return payload;
	}

	Vec2 CenterOfElement(UI::Context& context, UI::ElementHandle handle) {
		const UI::Rect& box = context.GetLayout(handle).borderBox;
		return {box.x + box.w * 0.5f, box.y + box.h * 0.5f};
	}

} // anonymous namespace

TEST_F(InspectorTest, DroppingComponentInInspectorNeverClonesIt) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> cards{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ComponentCard, cards);
	ASSERT_FALSE(cards.empty());

	UI::DragPayload payload = ComponentPayload(Handle<Component>(component.Get()));
	EXPECT_FALSE(context.DeliverDrop(payload, CenterOfElement(context, cards[0])));
	EXPECT_EQ(object.Get()->GetComponentCount(), 1u);
}

TEST_F(InspectorTest, DroppingComponentOnInsertStripReordersIt) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> first = object.Get()->AddComponent<InspTestComponent>();
	Handle<MeshRenderer> second = object.Get()->AddComponent<MeshRenderer>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	ASSERT_EQ(object.Get()->GetComponentAt(0), Handle<Component>(first.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	context.SetHoverSuppressed(true);
	panel.Get()->OnTick(0.0f);
	context.Update();

	Vector<UI::ElementHandle> strips{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::InsertStrip, strips);
	ASSERT_EQ(strips.size(), 3u);

	UI::DragPayload payload = ComponentPayload(Handle<Component>(second.Get()));
	ASSERT_TRUE(context.DeliverDrop(payload, CenterOfElement(context, strips[0])));
	EXPECT_EQ(object.Get()->GetComponentCount(), 2u);
	EXPECT_EQ(object.Get()->GetComponentAt(0), Handle<Component>(second.Get()));

	UndoSystem::Undo();
	EXPECT_EQ(object.Get()->GetComponentAt(0), Handle<Component>(first.Get()));
}

TEST_F(InspectorTest, OccupiedSlotsNotDropTargets) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> first = object.Get()->AddComponent<InspTestComponent>();
	object.Get()->AddComponent<MeshRenderer>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> strips{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::InsertStrip, strips);
	ASSERT_EQ(strips.size(), 3u);

	UI::DragPayload payload = ComponentPayload(Handle<Component>(first.Get()));
	EXPECT_FALSE(context.Events(strips[0]).onDragOver(payload));
	EXPECT_FALSE(context.Events(strips[1]).onDragOver(payload));
	EXPECT_TRUE(context.Events(strips[2]).onDragOver(payload));
}

TEST_F(InspectorTest, ComponentDropMovesState) {
	Handle<SceneObject> source = SceneSystem::CreateObject("Source", _scene);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	Handle<InspTestComponent> component = source.Get()->AddComponent<InspTestComponent>();
	component.Get()->speed = 7.5f;
	component.Get()->title = "carried";
	EditorSelection::Select(Handle<Object>(target.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::DragPayload payload = ComponentPayload(Handle<Component>(component.Get()));
	ASSERT_TRUE(context.DeliverDrop(payload, CenterOfElement(context, root)));
	SceneSystem::EndFrame();

	EXPECT_FALSE(source.Get()->GetComponent<InspTestComponent>().IsValid());
	Handle<InspTestComponent> moved = target.Get()->GetComponent<InspTestComponent>();
	ASSERT_TRUE(moved.IsValid());
	EXPECT_FLOAT_EQ(moved.Get()->speed, 7.5f);
	EXPECT_EQ(moved.Get()->title, "carried");

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(target.Get()->GetComponent<InspTestComponent>().IsValid());
	EXPECT_TRUE(source.Get()->GetComponent<InspTestComponent>().IsValid());
	EXPECT_EQ(target.Get()->GetComponentCount(), 0u);
}

TEST_F(InspectorTest, MissingComponentRowIsInert) {
	std::filesystem::path root = std::filesystem::temp_directory_path() /
								 ("shalltear_inspmissing_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
	std::filesystem::create_directories(root);
	FileSystem::Mount("project", Path(root.generic_string()));

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	TypeRegistry::Register(const_cast<TypeInfo*>(InspGoneComponent::GetReflectType()));
	object.Get()->AddComponent<InspGoneComponent>();
	TypeNameOps::Refresh();

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	TypeRegistry::Unregister(InspGoneComponent::GetReflectType()->name);

	_scene = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(_scene.IsValid());
	Handle<SceneObject> reopened = _scene->FindObjectByName("Subject");
	ASSERT_TRUE(reopened.IsValid());
	ASSERT_EQ(reopened->GetComponentCount(), 2u);
	EditorSelection::Select(Handle<Object>(reopened.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root2, "Missing: InspGoneComponent"));
	EXPECT_TRUE(AnyTextContains(context, root2, "saved unchanged"));

	EXPECT_TRUE(AnyTextContains(context, root2, "1 component in this scene couldn't be loaded"));
	EXPECT_TRUE(AnyTextContains(context, root2, "InspGoneComponent."));

	EXPECT_EQ(CountByClass(context, root2, StringHash("slt-checkbox")), 1u);
	EXPECT_EQ(CountByClass(context, root2, StringHash("slt-toggle")), 2u);
	EXPECT_EQ(CountByClass(context, root2, StringHash("slt-numberfield")), 14u);

	FileSystem::Unmount("project");
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

static void ClickInspectorElement(UI::Context& context, UI::ElementHandle element) {
	const UI::Rect& box = context.GetLayout(element).borderBox;
	InputEvent down{};
	down.type = InputEventType::MouseDown;
	down.button = MouseButton::Left;
	down.mousePosPx = {static_cast<i32>(box.x + box.w * 0.5f), static_cast<i32>(box.y + box.h * 0.5f)};
	InputEvent up = down;
	up.type = InputEventType::MouseUp;
	context.InjectInput(down);
	context.InjectInput(up);
	context.Update();
}

TEST_F(InspectorTest, MissingComponentRemovalAsks) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	TypeRegistry::Register(const_cast<TypeInfo*>(InspGoneComponent::GetReflectType()));
	object.Get()->AddComponent<InspGoneComponent>();

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	TypeRegistry::Unregister(InspGoneComponent::GetReflectType()->name);

	_scene = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(_scene.IsValid());
	Handle<SceneObject> reopened = _scene->FindObjectByName("Subject");
	ASSERT_TRUE(reopened.IsValid());
	ASSERT_EQ(reopened->GetComponentCount(), 1u);
	ASSERT_EQ(reopened->GetComponentAt(0).Get()->GetType(), UnresolvedComponent::GetReflectType());
	EditorSelection::Select(Handle<Object>(reopened.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle kebab = CardKebab(context, root);
	ASSERT_TRUE(context.IsAlive(kebab));

	ClickInspectorElement(context, kebab);
	context.Update();
	Vector<UI::ElementHandle> entries{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-menu-item"), entries);
	UI::ElementHandle remove{};
	for (UI::ElementHandle entry : entries) {
		if (AnyTextContains(context, entry, "Remove Component")) {
			remove = entry;
			break;
		}
	}
	ASSERT_TRUE(context.IsAlive(remove));

	ClickInspectorElement(context, remove);
	SceneSystem::EndFrame();
	context.Update();
	ASSERT_EQ(reopened->GetComponentCount(), 1u);
	EXPECT_TRUE(AnyTextContains(context, context.GetRoot(), "cannot be recovered"));

	Vector<UI::ElementHandle> popupButtons{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-button"), popupButtons);
	UI::ElementHandle accept{};
	for (UI::ElementHandle button : popupButtons) {
		if (AnyTextContains(context, button, "Remove")) {
			accept = button;
			break;
		}
	}
	ASSERT_TRUE(context.IsAlive(accept));
	ClickInspectorElement(context, accept);
	SceneSystem::EndFrame();

	EXPECT_EQ(reopened->GetComponentCount(), 0u);
	EXPECT_TRUE(EditorSceneOps::IsDirty(_scene));
}

TEST_F(InspectorTest, BannerIsAbsentWhenNothingIsMissing) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_FALSE(AnyTextContains(context, root, "couldn't be loaded"));

	EXPECT_EQ(EditorSceneOps::SurveyPreservedComponents(_scene).rows, 0u);
}

namespace {

	struct PreservedRowScene {
		Handle<SceneObject> carrier;
		Handle<SceneObject> plain;
	};

	PreservedRowScene ReloadWithAPreservedRow(Handle<Scene>& scene) {
		Handle<SceneObject> carrier = SceneSystem::CreateObject("Carrier", scene);
		TypeRegistry::Register(const_cast<TypeInfo*>(InspGoneComponent::GetReflectType()));
		carrier->AddComponent<InspGoneComponent>();
		SceneSystem::CreateObject("Plain", scene);

		Buffer snapshot;
		SceneSystem::SaveSceneToMemory(scene, snapshot);
		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
		TypeRegistry::Unregister(InspGoneComponent::GetReflectType()->name);

		scene = SceneSystem::LoadSceneFromMemory(snapshot);
		PreservedRowScene out;
		if (scene.IsValid()) {
			out.carrier = scene->FindObjectByName("Carrier");
			out.plain = scene->FindObjectByName("Plain");
		}
		return out;
	}

	void ExpectTheBannerClearsAfter(Handle<Scene>& scene, const std::function<void()>& publish) {
		PreservedRowScene objects = ReloadWithAPreservedRow(scene);
		ASSERT_TRUE(objects.plain.IsValid());
		ASSERT_EQ(objects.carrier->GetComponentCount(), 1u);
		EditorSelection::Select(Handle<Object>(objects.plain.Get()));

		Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
		ASSERT_TRUE(panel.IsValid());
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {500, 900};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();
		ASSERT_TRUE(AnyTextContains(context, root, "couldn't be loaded"));

		ASSERT_TRUE(EditorSceneOps::RemovePreservedComponent(objects.carrier->GetComponentAt(0)));
		SceneSystem::EndFrame();

		publish();
		panel.Get()->OnTick(1.0f / 60.0f);
		EXPECT_FALSE(AnyTextContains(context, root, "couldn't be loaded"));
	}

} // namespace

TEST_F(InspectorTest, BannerFollowsReload) {
	ExpectTheBannerClearsAfter(_scene, []() {
		EditorEvents::ModuleReloadEnded.Publish(EventBus::GetGlobal(), {true, nullptr});
	});
}

TEST_F(InspectorTest, BannerFollowsCompileState) {
	ExpectTheBannerClearsAfter(_scene, []() {
		EditorCompileStateChangedEvent event;
		EditorEvents::CompileStateChanged.Publish(EventBus::GetGlobal(), event);
	});
}

TEST_F(InspectorTest, OnlyPreservedRowIsRemovableWithoutUndo) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<Component> ordinary = object.Get()->AddComponent(InspTestComponent::GetReflectType());
	EXPECT_FALSE(EditorSceneOps::RemovePreservedComponent(ordinary));
	EXPECT_FALSE(EditorSceneOps::RemovePreservedComponent(Handle<Component>{}));
	EXPECT_EQ(object.Get()->GetComponentCount(), 1u);
}

TEST_F(InspectorTest, MissingComponentOffersNoLossyControl) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	TypeRegistry::Register(const_cast<TypeInfo*>(InspGoneComponent::GetReflectType()));
	object.Get()->AddComponent<InspGoneComponent>();

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	TypeRegistry::Unregister(InspGoneComponent::GetReflectType()->name);

	_scene = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(_scene.IsValid());
	Handle<SceneObject> reopened = _scene->FindObjectByName("Subject");
	ASSERT_TRUE(reopened.IsValid());
	ASSERT_EQ(reopened->GetComponentCount(), 1u);
	ASSERT_EQ(reopened->GetComponentAt(0).Get()->GetType(), UnresolvedComponent::GetReflectType());
	EditorSelection::Select(Handle<Object>(reopened.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_EQ(CountByClass(context, root, StringHash("slt-checkbox")), 0u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-toggle")), 1u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), 9u);

	Vector<UI::ElementHandle> cards{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ComponentCard, cards);
	UI::ElementHandle missingCard{};
	for (UI::ElementHandle card : cards) {
		if (AnyTextContains(context, card, "Missing: ")) {
			missingCard = card;
			break;
		}
	}
	ASSERT_TRUE(context.IsAlive(missingCard));
	Vector<UI::ElementHandle> dragSources{GetMemoryResource(MemTag::Editor)};
	CollectDragSources(context, missingCard, dragSources);
	EXPECT_TRUE(dragSources.empty());
}

namespace {

	class PrefabInspectorTest : public InspectorTest {
	protected:
		std::filesystem::path _projectRoot;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_prefabinsp_" + std::to_string(stamp) + "_" +
							std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot / "Assets");
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			AssetDatabase::Initialize();
			InspectorTest::SetUp();
		}

		void TearDown() override {
			InspectorTest::TearDown();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		void ImportModelFixture(Guid& out) {
			out = {};
			std::filesystem::path source = InspectorRepoDir("Source/Tests/Fixtures/Graphics/hierarchy.gltf");
			if (source.empty()) {
				return;
			}
			std::error_code ec;
			std::filesystem::copy_file(source, _projectRoot / "Assets" / "hierarchy.gltf",
									   std::filesystem::copy_options::overwrite_existing, ec);
			ASSERT_FALSE(ec);
			Asset* model = AssetDatabase::LoadAsset(Path("project://Assets/hierarchy.gltf"));
			ASSERT_NE(model, nullptr);
			out = model->assetId;
			AssetDatabase::UnloadAsset(model);
		}

		static UI::ElementHandle ButtonNamed(UI::Context& context, UI::ElementHandle from, std::string_view label) {
			if (context.HasClass(from, StringHash("slt-button")) && AnyTextContains(context, from, label)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = ButtonNamed(context, handle, label);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
	};

} // namespace

TEST_F(PrefabInspectorTest, PrefabOffersOpenButton) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle open = ButtonNamed(context, root, "Open Prefab");
	ASSERT_TRUE(open.IsValid());

	const UI::Rect& box = context.GetLayout(open).borderBox;
	Vec2 point{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
	for (InputEventType type : {InputEventType::MouseMove, InputEventType::MouseDown, InputEventType::MouseUp}) {
		InputEvent event{};
		event.type = type;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
		context.InjectInput(event);
	}
	context.Update();

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	Handle<Scene> opened = SceneSystem::GetLoadedScenes()[1];
	EXPECT_EQ(EditorSceneOps::GetPrefabTarget(opened), guid);
	EXPECT_EQ(opened->GetRootCount(), 1u);
}

TEST_F(PrefabInspectorTest, NonPrefabAssetOffersNoOpenButton) {
	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/notes.txt")), "hello");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/notes.txt"));
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_FALSE(ButtonNamed(context, root, "Open Prefab").IsValid());
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(PrefabInspectorTest, ModelPrefabExplainsItself) {
	Guid guid;
	ImportModelFixture(guid);
	if (!guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_FALSE(ButtonNamed(context, root, "Open Prefab").IsValid());
	EXPECT_TRUE(AnyTextContains(context, context.GetRoot(), "rebuilt from the file on import"));
	EXPECT_TRUE(AnyTextContains(context, context.GetRoot(), "Prefab Variant"));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

namespace {

	void WriteTwoAssets(Guid& first, Guid& second) {
		FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/one.txt")), "one");
		FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/two.txt")), "two");
		AssetDatabase::ScanProjectDirectory();
		first = AssetDatabase::LookupGuid(Path("project://Assets/one.txt"));
		second = AssetDatabase::LookupGuid(Path("project://Assets/two.txt"));
	}

	void BuildAndCollect(Handle<EditorPanel> panel, UI::Context& context, Vector<str>& out) {
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();
		out.clear();
		CollectTexts(context, context.GetRoot(), out);
	}

} // namespace

TEST_F(PrefabInspectorTest, MultiSelectAnnouncesCount) {
	Guid one, two;
	WriteTwoAssets(one, two);
	ASSERT_TRUE(one.IsValid() && two.IsValid());

	const Guid run[] = {one, two};
	EditorSelection::SelectAssets(run);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_TRUE(ContainsText(texts, "2 assets selected"));
	EXPECT_TRUE(ContainsText(texts, "two.txt"));
	EXPECT_FALSE(ContainsText(texts, "one.txt"));
}

TEST_F(PrefabInspectorTest, OneSelectedAssetSaysNothingAboutCount) {
	Guid one, two;
	WriteTwoAssets(one, two);
	EditorSelection::SelectAsset(one);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_FALSE(ContainsText(texts, "assets selected"));
	EXPECT_TRUE(ContainsText(texts, "one.txt"));
}

TEST_F(PrefabInspectorTest, WiderSelectionRedraws) {
	Guid one, two;
	WriteTwoAssets(one, two);

	EditorSelection::SelectAsset(two);
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);
	ASSERT_FALSE(ContainsText(texts, "assets selected"));

	const Guid run[] = {one, two};
	EditorSelection::SelectAssets(run);
	panel.Get()->OnTick(0.016f);
	texts.clear();
	CollectTexts(context, context.GetRoot(), texts);

	EXPECT_TRUE(ContainsText(texts, "2 assets selected"));
	EXPECT_TRUE(ContainsText(texts, "two.txt"));
}

TEST_F(PrefabInspectorTest, RenamingTheSelectedAssetRedrawsItsName) {
	Guid one, two;
	WriteTwoAssets(one, two);

	EditorSelection::SelectAsset(one);
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);
	ASSERT_TRUE(ContainsText(texts, "one.txt"));

	ASSERT_TRUE(AssetBrowserOps::RenameEntry(Path("project://Assets/one.txt"), "uno.txt"));
	panel.Get()->OnTick(0.016f);
	texts.clear();
	CollectTexts(context, context.GetRoot(), texts);

	EXPECT_TRUE(ContainsText(texts, "uno.txt"));
	EXPECT_FALSE(ContainsText(texts, "one.txt"));
}

TEST_F(PrefabInspectorTest, LockHoldsAsset) {
	Guid one, two;
	WriteTwoAssets(one, two);
	Handle<SceneObject> object = SceneSystem::CreateObject("Elsewhere", _scene);
	EditorSelection::SelectAsset(one);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	auto redraw = [&]() {
		panel.Get()->OnTick(0.0f);
		context.Update();
		texts.clear();
		CollectTexts(context, root, texts);
	};
	auto pressLock = [&]() {
		UI::ElementHandle strip = FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
		ClickInspectorElement(context, FindByClass(context, strip, EditorTheme::Class::ToolButton, 0));
		redraw();
	};
	redraw();
	ASSERT_TRUE(ContainsText(texts, "one.txt"));
	pressLock();

	EditorSelection::Clear();
	redraw();
	EXPECT_TRUE(ContainsText(texts, "one.txt"));

	EditorSelection::SelectAsset(two);
	redraw();
	EXPECT_TRUE(ContainsText(texts, "one.txt"));
	EXPECT_FALSE(ContainsText(texts, "two.txt"));

	EditorSelection::Select(Handle<Object>(object.Get()));
	redraw();
	EXPECT_TRUE(ContainsText(texts, "one.txt"));

	pressLock();
	EXPECT_FALSE(ContainsText(texts, "one.txt"));
	EXPECT_EQ(FirstTextFieldText(context, root), "Elsewhere");
}

TEST_F(PrefabInspectorTest, SelectingPrefabDrawsRootsInspectorBody) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	unit->AddComponent<InspTestComponent>();
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), 14u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-slider-track")), 1u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-checkbox")), 1u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-toggle")), 2u);
	EXPECT_TRUE(AnyTextContains(context, root, "Transform"));
	EXPECT_TRUE(AnyTextContains(context, root, "Unit"));
	EXPECT_TRUE(AnyTextContains(context, root, "Guid"));
	EXPECT_TRUE(ButtonNamed(context, root, "Open Prefab").IsValid());

	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(PrefabInspectorTest, TheStructuralControlsAreDrawnButDisabled) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	unit->AddComponent<InspTestComponent>();
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle add = ButtonNamed(context, root, "Add Component");
	ASSERT_TRUE(add.IsValid());
	EXPECT_TRUE(context.HasClass(add, EditorTheme::Class::Disabled));

	u32 before = CountByClass(context, context.GetRoot(), EditorTheme::Class::PopupCardCompact);
	ClickInspectorElement(context, add);
	EXPECT_EQ(CountByClass(context, context.GetRoot(), EditorTheme::Class::PopupCardCompact), before);

	Vector<UI::ElementHandle> strips{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::InsertStrip, strips);
	EXPECT_TRUE(strips.empty());
	Vector<UI::ElementHandle> cards{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ComponentCard, cards);
	ASSERT_FALSE(cards.empty());
	Vector<UI::ElementHandle> dragSources{GetMemoryResource(MemTag::Editor)};
	CollectDragSources(context, cards[0], dragSources);
	for (UI::ElementHandle source : dragSources) {
		EXPECT_EQ(context.GetTextString(source).find("InspTestComponent"), str::npos);
	}
}

TEST_F(PrefabInspectorTest, TogglingActiveInBodyWritesPrefab) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> switches{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, StringHash("slt-toggle"), switches);
	ASSERT_GE(switches.size(), 1u);
	ClickInspectorElement(context, switches[0]);

	PanelManager::TickAll(0.016f);

	Handle<SceneObject> fresh = SceneSystem::Instantiate(guid, _scene);
	ASSERT_TRUE(fresh.IsValid());
	EXPECT_FALSE(fresh->IsActiveSelf());
}

TEST_F(PrefabInspectorTest, StagedPrefabShowsStageRoot) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();

	Handle<Scene> stage = EditorSceneOps::OpenPrefab(guid);
	ASSERT_TRUE(stage.IsValid());
	Handle<SceneObject> stageRoot = stage->GetRoot(0);
	ASSERT_TRUE(stageRoot.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> switches{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, StringHash("slt-toggle"), switches);
	ASSERT_GE(switches.size(), 1u);
	ClickInspectorElement(context, switches[0]);
	PanelManager::TickAll(0.016f);

	EXPECT_FALSE(stageRoot->IsActiveSelf());
	EXPECT_TRUE(EditorSceneOps::IsDirty(stage));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
}

TEST_F(PrefabInspectorTest, ModelFileNamedAsModel) {
	Guid guid;
	ImportModelFixture(guid);
	if (!guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_TRUE(ExactText(texts, "Model Prefab"));
	for (const str& text : texts) {
		EXPECT_EQ(text.find("PrefabAsset"), str::npos);
	}
	EXPECT_TRUE(ExactText(texts, "Path"));
	EXPECT_TRUE(ExactText(texts, "Guid"));
	EXPECT_TRUE(ExactText(texts, ".gltf"));
}

TEST_F(PrefabInspectorTest, AuthoredPrefabNamedPrefab) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_TRUE(ExactText(texts, "Prefab"));
	EXPECT_FALSE(ExactText(texts, "Model Prefab"));
	for (const str& text : texts) {
		EXPECT_EQ(text.find("PrefabAsset"), str::npos) << "'" << text << "'";
	}
}

TEST_F(PrefabInspectorTest, AVariantIsNamedPrefabVariant) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(base.IsValid());
	Guid variant = EditorSceneOps::CreatePrefabVariantOf(base, Path("project://Assets/Unit Variant.prefab"));
	ASSERT_TRUE(variant.IsValid());
	EditorSelection::SelectAsset(variant);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_TRUE(ExactText(texts, "Prefab Variant"));
	EXPECT_FALSE(ExactText(texts, "Prefab"));
	EXPECT_FALSE(ExactText(texts, "Model Prefab"));
	EXPECT_TRUE(ExactText(texts, "Open Prefab"));
	EXPECT_TRUE(ExactText(texts, "Unit"));
}

TEST_F(PrefabInspectorTest, UninspectedAssetNamesType) {
	MeshAsset::GetReflectType();
	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/shape.primitive")), "");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/shape.primitive"));
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	EXPECT_TRUE(ExactText(texts, "Type"));
	EXPECT_TRUE(ExactText(texts, "MeshAsset"));
}

namespace {

	class MaterialShaderRowTest : public InspectorTest {
	protected:
		std::filesystem::path _projectRoot;
		bool _mountedEngine = false;
		Guid _shaderGuid;
		Guid _materialGuid;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_matrow_" + std::to_string(stamp) + "_" +
							std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot);
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			std::filesystem::path engineAssets = InspectorRepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
				_mountedEngine = true;
			}
			AssetDatabase::Initialize();
			InspectorTest::SetUp();
		}

		void TearDown() override {
			InspectorTest::TearDown();
			AssetDatabase::Terminate();
			if (_mountedEngine) {
				FileSystem::Unmount("engine");
			}
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		bool MakeShaderAndMaterial() {
			if (!_mountedEngine) {
				return false;
			}
			_shaderGuid = AssetDatabase::CreateAsset(Path("project://New Shader.hlsl"), StringHash("ShaderAsset"));
			if (!_shaderGuid.IsValid()) {
				return false;
			}
			Asset* probe = AssetDatabase::LoadAssetByGuid(_shaderGuid);
			if (!probe) {
				return false;
			}
			AssetDatabase::UnloadAsset(probe);
			_materialGuid = AssetDatabase::CreateAsset(Path("project://New Material.mat"), StringHash("MaterialAsset"));
			return _materialGuid.IsValid();
		}

		bool DropShaderOnItsRow(UI::Context& context, UI::ElementHandle root) {
			AssetDragData data;
			data.guid = _shaderGuid;
			data.assetTypeHash = StringHash("ShaderAsset").GetValue();
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.display = "New Shader.hlsl";
			payload.data = Variant(data);

			AssetDragData otherData = data;
			otherData.assetTypeHash = StringHash("MeshAsset").GetValue();
			UI::DragPayload otherPayload = payload;
			otherPayload.data = Variant(otherData);

			Vector<UI::ElementHandle> targets{GetMemoryResource(MemTag::Editor)};
			CollectDropTargets(context, root, targets);
			for (UI::ElementHandle row : targets) {
				auto& events = context.Events(row);
				if (!events.onDragOver || !events.onDragOver(payload) || events.onDragOver(otherPayload)) {
					continue;
				}
				events.onDrop(payload, {});
				return true;
			}
			return false;
		}

		Handle<EditorPanel> BuildInspector(UI::Context& context, UI::ElementHandle& outRoot) {
			Handle<EditorPanel> panel = OpenInspector();
			if (!panel.IsValid()) {
				return panel;
			}
			outRoot = context.CreateElement();
			context.AppendChild(context.GetRoot(), outRoot);
			panel.Get()->BuildContent(context, outRoot);
			context.Update();
			return panel;
		}
	};

} // namespace

TEST_F(MaterialShaderRowTest, AssignmentSurvivesBuildThatOfferedIt) {
	if (!MakeShaderAndMaterial()) {
		GTEST_SKIP() << "EngineAssets or dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
	}
	EditorSelection::SelectAsset(_materialGuid);

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root;
	ASSERT_TRUE(BuildInspector(context, root).IsValid());

	ASSERT_TRUE(DropShaderOnItsRow(context, root));

	Asset* materialAsset = AssetDatabase::LoadAssetByGuid(_materialGuid);
	ASSERT_NE(materialAsset, nullptr);
	MaterialAsset* material = static_cast<MaterialAsset*>(materialAsset);
	EXPECT_EQ(material->shaderGuid, _shaderGuid);
	ShaderAsset* assigned = material->shader.Get();
	ASSERT_NE(assigned, nullptr);
	EXPECT_EQ(AssetDatabase::LookupPath(assigned->assetId).ToString(), "project://New Shader.hlsl");
	AssetDatabase::UnloadAsset(materialAsset);
}

TEST_F(MaterialShaderRowTest, DroppingShaderRedrawsRowThatTookIt) {
	if (!MakeShaderAndMaterial()) {
		GTEST_SKIP() << "EngineAssets or dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
	}
	EditorSelection::SelectAsset(_materialGuid);

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root;
	ASSERT_TRUE(BuildInspector(context, root).IsValid());

	ASSERT_TRUE(AnyTextContains(context, root, "Lit"));
	ASSERT_FALSE(AnyTextContains(context, root, "New Shader"));

	ASSERT_TRUE(DropShaderOnItsRow(context, root));

	PanelManager::TickAll(1.0f / 60.0f);
	context.Update();
	EXPECT_TRUE(AnyTextContains(context, root, "New Shader"));
	EXPECT_FALSE(AnyTextContains(context, root, "Lit"));
}

TEST_F(MaterialShaderRowTest, ClosingPanelReleasesAssetItWasShowing) {
	if (!MakeShaderAndMaterial()) {
		GTEST_SKIP() << "EngineAssets or dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
	}
	EditorSelection::SelectAsset(_materialGuid);

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root;
	Handle<EditorPanel> panel = BuildInspector(context, root);
	ASSERT_TRUE(panel.IsValid());

	Asset* whileOpen = AssetDatabase::LoadAssetByGuid(_materialGuid);
	ASSERT_NE(whileOpen, nullptr);
	u64 openInstance = whileOpen->GetInstanceId();
	AssetDatabase::UnloadAsset(whileOpen);

	PanelManager::ClosePanel(panel);

	AssetDatabase::CollectGarbage();
	Asset* afterClose = AssetDatabase::LoadAssetByGuid(_materialGuid);
	ASSERT_NE(afterClose, nullptr);
	EXPECT_NE(afterClose->GetInstanceId(), openInstance);
	AssetDatabase::UnloadAsset(afterClose);
}

TEST_F(InspectorTest, ReflectedEnumDrawsAsNamedDropdown) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Lamp", _scene);
	Light* light = object.Get()->AddComponent<Light>().Get();
	ASSERT_NE(light, nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle picker = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(picker));
	UI::ElementHandle label = NthChildOf(context, picker, 0);
	EXPECT_EQ(context.GetTextString(label), "Directional");

	ClickElement(context, picker);
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	ClickElement(context, NthChildOf(context, context.GetOpenPopup(0), 2));
	EXPECT_EQ(light->GetLightType(), LightType::Spot);
	context.Update();
	EXPECT_EQ(context.GetTextString(label), "Spot");

	UndoSystem::Undo();
	EXPECT_EQ(light->GetLightType(), LightType::Directional);
	PanelManager::TickAll(1.0f / 60.0f);
	context.Update();
	EXPECT_EQ(context.GetTextString(label), "Directional");
}

TEST_F(InspectorTest, EnumValueNoConstantNamesDrawsBlank) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Lamp", _scene);
	Light* light = object.Get()->AddComponent<Light>().Get();
	ASSERT_NE(light, nullptr);
	light->SetLightType(static_cast<LightType>(9));
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle picker = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(picker));
	EXPECT_EQ(context.GetTextString(NthChildOf(context, picker, 0)), "");
	EXPECT_EQ(static_cast<u8>(light->GetLightType()), 9u);
}

TEST_F(InspectorTest, LockButtonPinsTarget) {
	Handle<SceneObject> first = SceneSystem::CreateObject("First", _scene);
	Handle<SceneObject> second = SceneSystem::CreateObject("Second", _scene);
	EditorSelection::Select(Handle<Object>(first.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	const TypeInfo* panelType = panel.Get()->GetType();

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle strip = FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
	ASSERT_TRUE(context.IsAlive(strip));
	ASSERT_EQ(CountByClass(context, strip, EditorTheme::Class::ToolButton), 1u);
	UI::ElementHandle lock = FindByClass(context, strip, EditorTheme::Class::ToolButton, 0);
	EXPECT_FALSE(context.HasClass(lock, EditorTheme::Class::ToolButtonOn));

	ClickInspectorElement(context, lock);
	panel.Get()->OnTick(0.0f);
	context.Update();

	Variant lockedValue;
	ASSERT_TRUE(GetField(panel.Get(), panelType, "locked"_hash, &lockedValue));
	EXPECT_TRUE(lockedValue.As<bool>());
	Variant pinned;
	ASSERT_TRUE(GetField(panel.Get(), panelType, "lockedTargetId"_hash, &pinned));
	EXPECT_EQ(pinned.As<u64>(), first.GetInstanceId());
	EXPECT_TRUE(context.HasClass(FindByClass(context, root, EditorTheme::Class::ToolButton, 0),
								 EditorTheme::Class::ToolButtonOn));

	EditorSelection::Select(Handle<Object>(second.Get()));
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_EQ(FirstTextFieldText(context, root), "First");

	ClickInspectorElement(context, FindByClass(context, root, EditorTheme::Class::ToolButton, 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	ASSERT_TRUE(GetField(panel.Get(), panelType, "locked"_hash, &lockedValue));
	EXPECT_FALSE(lockedValue.As<bool>());
	EXPECT_EQ(FirstTextFieldText(context, root), "Second");
}

TEST_F(InspectorTest, TitleIsDragHandle) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	EditorTheme::ApplyToContext(context);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle card = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	ASSERT_TRUE(context.IsAlive(card));
	UI::ElementHandle header = NthChildOf(context, card, 0);
	ASSERT_TRUE(context.IsAlive(header));
	Vector<UI::ElementHandle> sources{GetMemoryResource(MemTag::Editor)};
	CollectDragSources(context, header, sources);
	ASSERT_EQ(sources.size(), 1u);
	UI::ElementHandle title = sources[0];

	Vector<str> offenders{GetMemoryResource(MemTag::Editor)};
	CollectCursorOffenders(context, root, offenders);
	EXPECT_TRUE(offenders.empty()) << ReportCursorOffenders(offenders);

	const UI::Rect& box = context.GetLayout(title).borderBox;
	auto moveTo = [&](i32 x, i32 y) {
		InputEvent event{};
		event.type = InputEventType::MouseMove;
		event.mousePosPx = {x, y};
		context.InjectInput(event);
		context.Update();
	};
	auto fill = [&]() { return context.GetComputedStyle(title).background.solid; };
	moveTo(static_cast<i32>(box.x + box.w + 40.0f), static_cast<i32>(box.y + box.h + 300.0f));
	const Color rest = fill();
	moveTo(static_cast<i32>(box.x + box.w * 0.5f), static_cast<i32>(box.y + box.h * 0.5f));
	ASSERT_EQ(context.HitTest({box.x + box.w * 0.5f, box.y + box.h * 0.5f}), title);
	EXPECT_NE(fill(), rest);
	moveTo(static_cast<i32>(box.x + box.w + 40.0f), static_cast<i32>(box.y + box.h + 300.0f));
	EXPECT_EQ(fill(), rest);
}

TEST_F(InspectorTest, FoldedCardKeepsHeader) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	const u32 openNumbers = CountByClass(context, root, StringHash("slt-numberfield"));
	ASSERT_GT(openNumbers, 9u);
	ASSERT_EQ(CountByClass(context, root, StringHash("slt-toggle")), 2u);

	UI::ElementHandle card = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	ASSERT_TRUE(context.IsAlive(card));
	UI::ElementHandle header = NthChildOf(context, card, 0);
	ASSERT_TRUE(context.IsAlive(header));
	ClickInspectorElement(context, NthChildOf(context, header, 1));
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), openNumbers);

	UI::ElementHandle open = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	UI::ElementHandle openHeader = NthChildOf(context, open, 0);
	ClickInspectorElement(context, NthChildOf(context, openHeader, 0));
	panel.Get()->OnTick(0.0f);
	context.Update();

	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), 9u);
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-toggle")), 2u);

	UI::ElementHandle folded = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	UI::ElementHandle foldedHeader = NthChildOf(context, folded, 0);
	ClickInspectorElement(context, NthChildOf(context, foldedHeader, 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), openNumbers);
}

TEST_F(InspectorTest, EmptyComponentHasNoCaret) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspBareComponent>();
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	auto card = [&](u32 index) { return FindByClass(context, root, EditorTheme::Class::ComponentCard, index); };
	auto headerOf = [&](u32 index) { return NthChildOf(context, card(index), 0); };
	ASSERT_TRUE(context.IsAlive(card(0)));
	ASSERT_TRUE(context.IsAlive(card(1)));

	UI::ElementHandle fullCaret = NthChildOf(context, headerOf(1), 0);
	ASSERT_TRUE(context.HasIcon(fullCaret) || !context.GetTextString(fullCaret).empty());

	UI::ElementHandle bareSlot = NthChildOf(context, headerOf(0), 0);
	ASSERT_TRUE(context.IsAlive(bareSlot));
	EXPECT_FALSE(context.HasIcon(bareSlot));
	EXPECT_TRUE(context.GetTextString(bareSlot).empty());

	ClickInspectorElement(context, NthChildOf(context, headerOf(1), 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	ASSERT_FALSE(context.IsAlive(NthChildOf(context, card(1), 1)));
	EXPECT_EQ(context.GetLayout(headerOf(0)).borderBox.h, context.GetLayout(headerOf(1)).borderBox.h);

	const f32 headerBefore = context.GetLayout(headerOf(0)).borderBox.h;
	ClickInspectorElement(context, NthChildOf(context, headerOf(0), 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_FALSE(context.IsAlive(NthChildOf(context, card(0), 1)));
	EXPECT_EQ(context.GetLayout(headerOf(0)).borderBox.h, headerBefore);
}

TEST_F(InspectorTest, FoldSurvivesRebuildItDidNotCause) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<SceneObject> other = SceneSystem::CreateObject("Elsewhere", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle card = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	ASSERT_TRUE(context.IsAlive(card));
	ClickInspectorElement(context, NthChildOf(context, NthChildOf(context, card, 0), 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	const u32 folded = CountByClass(context, root, StringHash("slt-numberfield"));
	ASSERT_EQ(folded, 9u);

	EditorSelection::Select(Handle<Object>(other.Get()));
	panel.Get()->OnTick(0.0f);
	context.Update();
	ASSERT_EQ(FirstTextFieldText(context, root), "Elsewhere");

	EditorSelection::Select(Handle<Object>(object.Get()));
	panel.Get()->OnTick(0.0f);
	context.Update();
	ASSERT_EQ(FirstTextFieldText(context, root), "Subject");
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), folded);
}

TEST_F(InspectorTest, AFoldSurvivesPlayAndStop) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	const u32 open = CountByClass(context, root, StringHash("slt-numberfield"));
	UI::ElementHandle card = FindByClass(context, root, EditorTheme::Class::ComponentCard, 0);
	ASSERT_TRUE(context.IsAlive(card));
	ClickInspectorElement(context, NthChildOf(context, NthChildOf(context, card, 0), 0));
	panel.Get()->OnTick(0.0f);
	context.Update();
	const u32 folded = CountByClass(context, root, StringHash("slt-numberfield"));
	ASSERT_LT(folded, open);

	auto componentId = []() -> u64 {
		Object* selected = EditorSelection::GetActive().Get();
		SceneObject* owner = selected ? static_cast<SceneObject*>(selected) : nullptr;
		return owner ? owner->GetComponent<InspTestComponent>().GetInstanceId() : 0;
	};
	const u64 before = componentId();
	PlayModeController playMode;
	for (const char* phase : {"Play", "Stop"}) {
		playMode.RequestToggle();
		playMode.PumpMainThread();
		panel.Get()->OnTick(0.0f);
		context.Update();
		ASSERT_EQ(FirstTextFieldText(context, root), "Subject") << phase;
		ASSERT_NE(componentId(), before);
		EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), folded) << phase;
	}
	SceneSystem::EndFrame();
}

TEST_F(InspectorTest, CardsRuledOff) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	EditorTheme::ApplyToContext(context);

	UI::ElementHandle root;
	auto rulesAndCards = [&](u32& rules, u32& cards) {
		root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();
		rules = CountByClass(context, root, StringHash("slt-separator"));
		cards = CountByClass(context, root, EditorTheme::Class::ComponentCard);
	};

	u32 rules = 0;
	u32 cards = 0;
	rulesAndCards(rules, cards);
	EXPECT_EQ(cards, 0u);
	EXPECT_EQ(rules, 1u);

	object.Get()->AddComponent<InspTestComponent>();
	rulesAndCards(rules, cards);
	EXPECT_EQ(cards, 1u);
	EXPECT_EQ(rules, 2u);

	object.Get()->AddComponent<Light>();
	rulesAndCards(rules, cards);
	EXPECT_EQ(cards, 2u);
	EXPECT_EQ(rules, 3u);

	Vector<UI::ElementHandle> column{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ComponentCard, column);
	ASSERT_EQ(column.size(), 2u);
	UI::ElementHandle last = column.back();
	bool pastLast = false;
	u32 trailing = 0;
	const UI::Element* stack = context.GetElement(context.MakeHandle(context.GetElement(last)->parent));
	for (u32 child = stack ? stack->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
		UI::ElementHandle handle = context.MakeHandle(child);
		if (pastLast && context.HasClass(handle, StringHash("slt-separator"))) {
			++trailing;
		}
		pastLast = pastLast || handle.index == last.index;
		child = context.GetElement(handle)->nextSibling;
	}
	EXPECT_EQ(trailing, 1u);

	for (UI::ElementHandle card : column) {
		EXPECT_EQ(context.GetComputedStyle(card).boxShadowCount, 0u);
	}
}

namespace {

	class InspDataAsset : public Asset {
	public:
		SLT_REFLECT_CLASS(InspDataAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_REFLECT_FIELD(damage)
		SLT_REFLECT_FIELD(title)
		SLT_REFLECT_FIELD(secret, SLT_HIDDEN)
		SLT_END_REFLECT()

		i32 damage = 11;
		str title = "sword";
		i32 secret = 0;

		StringHash GetAssetTypeId() const override { return InspDataAsset::GetReflectType()->name; }

		bool Import(const Path& sourcePath) override {
			return TextSerializer::Deserialize(this, GetReflectType(),
											   FileSystem::ReadTextFile(sourcePath, MemTag::Asset));
		}
		bool Export(const Path& destPath) const override {
			return FileSystem::WriteTextFile(destPath, TextSerializer::Serialize(this, GetReflectType()));
		}
	};

	class InspFieldlessAsset : public Asset {
	public:
		SLT_REFLECT_CLASS(InspFieldlessAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_END_REFLECT()

		StringHash GetAssetTypeId() const override { return InspFieldlessAsset::GetReflectType()->name; }
		bool Import(const Path&) override { return true; }
	};

	class InspDataAssetImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("InspDataAsset"); }

		Asset* Import(const Path& sourcePath) override {
			auto* asset = New<InspDataAsset>(MemTag::Asset);
			if (!asset->Import(sourcePath)) {
				Delete(asset);
				return nullptr;
			}
			return asset;
		}

		bool CanCreateAsset() const override { return true; }
		std::string_view GetCreateMenuLabel() const override { return "Insp Data"; }
		std::string_view GetCreateDefaultFileName() const override { return "New Insp Data.idata"; }
		Asset* CreateAsset() override { return New<InspDataAsset>(MemTag::Asset); }
	};

	class InspFieldlessAssetImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("InspFieldlessAsset"); }
		Asset* Import(const Path&) override { return New<InspFieldlessAsset>(MemTag::Asset); }
	};

	size_t TextIndex(const Vector<str>& texts, std::string_view whole) {
		for (size_t i = 0; i < texts.size(); ++i) {
			if (texts[i] == whole) {
				return i;
			}
		}
		return str::npos;
	}

	UI::ElementHandle FindButtonNamed(UI::Context& context, UI::ElementHandle from, std::string_view label) {
		if (context.HasClass(from, StringHash("slt-button")) && AnyTextContains(context, from, label)) {
			return from;
		}
		const UI::Element* element = context.GetElement(from);
		for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			UI::ElementHandle found = FindButtonNamed(context, handle, label);
			if (found.IsValid()) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	class DataAssetInspectorTest : public InspectorTest {
	protected:
		std::filesystem::path _projectRoot;
		InspDataAssetImporter _importer;
		InspFieldlessAssetImporter _fieldlessImporter;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_datainsp_" + std::to_string(stamp) + "_" +
							std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot / "Assets");
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			AssetDatabase::Initialize();
			InspDataAsset::GetReflectType();
			InspFieldlessAsset::GetReflectType();
			AssetImporterRegistry::Register(StringHash("InspDataAsset"), &_importer, {".idata"});
			AssetImporterRegistry::Register(StringHash("InspFieldlessAsset"), &_fieldlessImporter, {".ifield"});
			InspectorTest::SetUp();
		}

		void TearDown() override {
			InspectorTest::TearDown();
			AssetImporterRegistry::Unregister(&_fieldlessImporter);
			AssetImporterRegistry::Unregister(&_importer);
			AssetDatabase::CollectGarbage();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		Guid CreateProbeAsset(const char* virtualPath = "project://Probe.idata") {
			Guid guid = AssetDatabase::CreateAsset(Path(virtualPath), StringHash("InspDataAsset"));
			EXPECT_TRUE(guid.IsValid());
			return guid;
		}

		i32 LiveDamage(const Guid& guid) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			EXPECT_NE(asset, nullptr);
			if (!asset) {
				return -1;
			}
			Variant out;
			EXPECT_TRUE(GetField(asset, asset->GetType(), StringHash("damage"), &out));
			AssetDatabase::UnloadAsset(asset);
			const i32* value = out.TryGet<i32>();
			return value ? *value : -1;
		}

		str FileText(const Guid& guid) {
			Path file = AssetDatabase::LookupPath(guid);
			EXPECT_FALSE(file.ToString().empty());
			return FileSystem::ReadTextFile(FileSystem::Resolve(file), MemTag::Editor);
		}

		u64 LiveInstanceId(const Guid& guid) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			EXPECT_NE(asset, nullptr);
			if (!asset) {
				return 0;
			}
			u64 id = asset->GetInstanceId();
			AssetDatabase::UnloadAsset(asset);
			return id;
		}

		void TypeDamage(UI::Context& context, UI::ElementHandle root, std::string_view from, std::string_view to) {
			UI::ElementHandle field = FindElementByText(context, root, from);
			ASSERT_TRUE(context.IsAlive(field)) << from << "'";
			PressAndRelease(context, field);
			ASSERT_EQ(context.GetFocused(), field);
			context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
			for (char digit : to) {
				context.InjectInput(InspectorTextChar(static_cast<char32_t>(digit)));
			}
			context.Update();
			ASSERT_EQ(context.GetTextString(field), str(to));
			PressAway(context, root);
		}

		void SelectAndSettle(const Guid& guid, UI::Context& context) {
			EditorSelection::SelectAsset(guid);
			PanelManager::TickAll(1.0f / 60.0f);
			context.Update();
			AssetDatabase::CollectGarbage();
		}
	};

} // anonymous namespace

TEST_F(DataAssetInspectorTest, FieldsUnderInfoBlock) {
	Guid guid = CreateProbeAsset();
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);

	ASSERT_NE(TextIndex(texts, "Path"), str::npos);
	ASSERT_NE(TextIndex(texts, "Guid"), str::npos);
	EXPECT_NE(TextIndex(texts, "InspDataAsset"), str::npos);
	EXPECT_NE(TextIndex(texts, ".idata"), str::npos);

	size_t damage = TextIndex(texts, "damage");
	ASSERT_NE(damage, str::npos);
	ASSERT_NE(TextIndex(texts, "title"), str::npos);
	EXPECT_LT(TextIndex(texts, "Guid"), damage);
	EXPECT_EQ(CountByClass(context, context.GetRoot(), EditorTheme::Class::SectionCard), 1u);

	EXPECT_EQ(TextIndex(texts, "secret"), str::npos);
	EXPECT_EQ(TextIndex(texts, "assetId"), str::npos);
	EXPECT_EQ(TextIndex(texts, "virtualPath"), str::npos);
}

TEST_F(DataAssetInspectorTest, FieldRowCommitsAndUndoes) {
	Guid guid = CreateProbeAsset();
	ASSERT_TRUE(guid.IsValid());
	ASSERT_EQ(LiveDamage(guid), 11);
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle field = FindElementByText(context, root, "11");
	ASSERT_TRUE(context.IsAlive(field));
	ASSERT_TRUE(context.HasClass(field, StringHash("slt-textfield")));

	PressAndRelease(context, field);
	ASSERT_EQ(context.GetFocused(), field);
	context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
	context.InjectInput(InspectorTextChar(U'4'));
	context.InjectInput(InspectorTextChar(U'2'));
	context.Update();
	ASSERT_EQ(context.GetTextString(field), "42");

	PressAway(context, root);

	EXPECT_EQ(LiveDamage(guid), 42);
	ASSERT_TRUE(UndoSystem::CanUndo());
	UndoSystem::Undo();
	EXPECT_EQ(LiveDamage(guid), 11);
}

TEST_F(DataAssetInspectorTest, SaveWritesEditedInstanceToFile) {
	Guid guid = CreateProbeAsset();
	ASSERT_TRUE(guid.IsValid());
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle field = FindElementByText(context, root, "11");
	ASSERT_TRUE(context.IsAlive(field));
	PressAndRelease(context, field);
	context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
	context.InjectInput(InspectorTextChar(U'9'));
	context.InjectInput(InspectorTextChar(U'9'));
	context.Update();
	PressAway(context, root);
	ASSERT_EQ(LiveDamage(guid), 99);

	Path file = AssetDatabase::LookupPath(guid);
	ASSERT_FALSE(file.ToString().empty());
	str before = FileSystem::ReadTextFile(FileSystem::Resolve(file), MemTag::Editor);
	EXPECT_EQ(before.find("99"), str::npos);

	UI::ElementHandle save = FindButtonNamed(context, root, "Save");
	ASSERT_TRUE(save.IsValid());
	PressAndRelease(context, save);

	str after = FileSystem::ReadTextFile(FileSystem::Resolve(file), MemTag::Editor);
	EXPECT_NE(after.find("99"), str::npos) << after << "'";
}

TEST_F(DataAssetInspectorTest, FieldlessTypeHasNoBody) {
	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/Fieldless.ifield")), "opaque");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/Fieldless.ifield"));
	ASSERT_TRUE(guid.IsValid());
	Asset* probe = AssetDatabase::LoadAssetByGuid(guid);
	ASSERT_NE(probe, nullptr);
	ASSERT_EQ(probe->GetRefCount(), 1);
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_EQ(probe->GetRefCount(), 1);

	EXPECT_FALSE(FindButtonNamed(context, root, "Save").IsValid());
	EXPECT_EQ(CountByClass(context, root, EditorTheme::Class::SectionCard), 0u);
	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, root, texts);
	EXPECT_NE(TextIndex(texts, "Path"), str::npos);
	EXPECT_NE(TextIndex(texts, "InspFieldlessAsset"), str::npos);
	EXPECT_NE(TextIndex(texts, ".ifield"), str::npos);

	AssetDatabase::UnloadAsset(probe);
}

TEST_F(DataAssetInspectorTest, AnUnsavedEditSurvivesSelectingAnotherAsset) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	ASSERT_TRUE(probe.IsValid() && other.IsValid());
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	TypeDamage(context, root, "11", "88");
	ASSERT_EQ(LiveDamage(probe), 88);
	ASSERT_EQ(FileText(probe).find("88"), str::npos);
	u64 instance = LiveInstanceId(probe);
	ASSERT_NE(instance, 0u);

	SelectAndSettle(other, context);
	EXPECT_NE(ObjectRegistry::Resolve(instance), nullptr);

	SelectAndSettle(probe, context);
	EXPECT_EQ(LiveDamage(probe), 88);
	EXPECT_TRUE(context.IsAlive(FindElementByText(context, root, "88")));
	EXPECT_EQ(FileText(probe).find("88"), str::npos);
}

TEST_F(DataAssetInspectorTest, UneditedAssetDroppedOnMove) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	ASSERT_TRUE(context.IsAlive(FindElementByText(context, root, "11")));

	u64 instance = LiveInstanceId(probe);
	ASSERT_NE(instance, 0u);
	SelectAndSettle(other, context);
	EXPECT_EQ(ObjectRegistry::Resolve(instance), nullptr);
}

TEST_F(DataAssetInspectorTest, AssetEditIsNotSceneEdit) {
	Guid probe = CreateProbeAsset();
	EditorSelection::SelectAsset(probe);
	ASSERT_FALSE(EditorAssetOps::AnyDirty());
	ASSERT_FALSE(EditorSceneOps::AnyDirty());

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	TypeDamage(context, root, "11", "88");
	EXPECT_TRUE(EditorAssetOps::AnyDirty());
	EXPECT_FALSE(EditorSceneOps::AnyDirty());

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	UndoSystem::Push(MakeUnique<SetActiveCommand>(object, false));
	EXPECT_TRUE(EditorSceneOps::AnyDirty());
}

TEST_F(DataAssetInspectorTest, SaveClearsDirtyState) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	TypeDamage(context, root, "11", "88");
	ASSERT_TRUE(EditorAssetOps::AnyDirty());
	u64 instance = LiveInstanceId(probe);

	UI::ElementHandle save = FindButtonNamed(context, root, "Save");
	ASSERT_TRUE(save.IsValid());
	PressAndRelease(context, save);

	EXPECT_NE(FileText(probe).find("88"), str::npos);
	EXPECT_FALSE(EditorAssetOps::AnyDirty());

	SelectAndSettle(other, context);
	EXPECT_EQ(ObjectRegistry::Resolve(instance), nullptr);
}

TEST_F(DataAssetInspectorTest, IsDirtyTracksOneAsset) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	Asset* untouched = AssetDatabase::LoadAssetByGuid(other);
	ASSERT_NE(untouched, nullptr);
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Asset* edited = AssetDatabase::FindLoadedByGuid(probe);
	ASSERT_NE(edited, nullptr);
	EXPECT_FALSE(EditorAssetOps::IsDirty(Handle<Asset>(edited)));
	TypeDamage(context, root, "11", "88");
	EXPECT_TRUE(EditorAssetOps::IsDirty(Handle<Asset>(edited)));
	EXPECT_FALSE(EditorAssetOps::IsDirty(Handle<Asset>(untouched)));
	EXPECT_FALSE(EditorAssetOps::IsDirty(Handle<Asset>()));

	UI::ElementHandle save = FindButtonNamed(context, root, "Save");
	ASSERT_TRUE(save.IsValid());
	PressAndRelease(context, save);
	EXPECT_FALSE(EditorAssetOps::IsDirty(Handle<Asset>(edited)));
	AssetDatabase::UnloadAsset(untouched);
}

TEST_F(DataAssetInspectorTest, DiscardingDropsEditAndFileWinsAgain) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	TypeDamage(context, root, "11", "88");
	ASSERT_TRUE(EditorAssetOps::AnyDirty());

	EditorAssetOps::DiscardAllDirty();
	EXPECT_FALSE(EditorAssetOps::AnyDirty());

	SelectAndSettle(other, context);
	SelectAndSettle(probe, context);
	EXPECT_EQ(LiveDamage(probe), 11);
	EXPECT_TRUE(context.IsAlive(FindElementByText(context, root, "11")));
}

TEST_F(DataAssetInspectorTest, DeletedFileEditForgotten) {
	Guid probe = CreateProbeAsset();
	Guid other = CreateProbeAsset("project://Other.idata");
	EditorSelection::SelectAsset(probe);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	TypeDamage(context, root, "11", "88");
	ASSERT_TRUE(EditorAssetOps::AnyDirty());
	const u64 instance = LiveInstanceId(probe);
	const Path path = AssetDatabase::LookupPath(probe);
	ASSERT_TRUE(AssetBrowserOps::DeleteEntry(path));

	EditorAssetOps::SaveAllDirty();
	EXPECT_FALSE(FileSystem::Exists(FileSystem::Resolve(path)));
	EXPECT_FALSE(EditorAssetOps::AnyDirty());
	SelectAndSettle(other, context);
	EXPECT_EQ(ObjectRegistry::Resolve(instance), nullptr);
}

namespace {

	struct InspNestedEntry {
		str name;
		f32 volume = 1.0f;
		bool enabled = true;
		Handle<InspDataAsset> reference;

		SLT_REFLECT_VALUE(InspNestedEntry)
		SLT_REFLECT_FIELD(name)
		SLT_REFLECT_FIELD(volume)
		SLT_REFLECT_FIELD(enabled)
		SLT_REFLECT_FIELD(reference)
		SLT_END_REFLECT()
	};

	struct InspNestedSettings {
		i32 depth = 37;
		Vector<Handle<InspDataAsset>> extras{GetMemoryResource(MemTag::Asset)};

		SLT_REFLECT_VALUE(InspNestedSettings)
		SLT_REFLECT_FIELD(depth)
		SLT_REFLECT_ARRAY(extras)
		SLT_END_REFLECT()
	};

	class InspNestedAsset : public Asset {
	public:
		SLT_REFLECT_CLASS(InspNestedAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_REFLECT_ARRAY(entries)
		SLT_REFLECT_SUBOBJECT(settings)
		SLT_END_REFLECT()

		InspNestedAsset() {
			entries.resize(2);
			entries[0].name = "alpha";
			entries[1].name = "beta";
		}

		Vector<InspNestedEntry> entries{GetMemoryResource(MemTag::Asset)};
		InspNestedSettings settings;

		StringHash GetAssetTypeId() const override { return InspNestedAsset::GetReflectType()->name; }

		bool Import(const Path& sourcePath) override {
			return TextSerializer::Deserialize(this, GetReflectType(),
											   FileSystem::ReadTextFile(sourcePath, MemTag::Asset));
		}
		bool Export(const Path& destPath) const override {
			return FileSystem::WriteTextFile(destPath, TextSerializer::Serialize(this, GetReflectType()));
		}
	};

	class InspNestedAssetImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("InspNestedAsset"); }

		Asset* Import(const Path& sourcePath) override {
			auto* asset = New<InspNestedAsset>(MemTag::Asset);
			if (!asset->Import(sourcePath)) {
				Delete(asset);
				return nullptr;
			}
			return asset;
		}

		bool CanCreateAsset() const override { return true; }
		std::string_view GetCreateMenuLabel() const override { return "Insp Nested"; }
		std::string_view GetCreateDefaultFileName() const override { return "New Insp Nested.inest"; }
		Asset* CreateAsset() override { return New<InspNestedAsset>(MemTag::Asset); }
	};

	class NestedAssetInspectorTest : public DataAssetInspectorTest {
	protected:
		InspNestedAssetImporter _nestedImporter;

		void SetUp() override {
			DataAssetInspectorTest::SetUp();
			InspNestedAsset::GetReflectType();
			AssetImporterRegistry::Register(StringHash("InspNestedAsset"), &_nestedImporter, {".inest"});
		}

		void TearDown() override {
			AssetImporterRegistry::Unregister(&_nestedImporter);
			DataAssetInspectorTest::TearDown();
		}

		struct Snapshot {
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			i32 depth = 0;
			size_t extras = 0;
		};
		Snapshot Live(const Guid& guid) {
			Snapshot out;
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			EXPECT_NE(asset, nullptr);
			if (!asset) {
				return out;
			}
			auto* nested = static_cast<InspNestedAsset*>(asset);
			for (const InspNestedEntry& entry : nested->entries) {
				out.names.push_back(entry.name);
			}
			out.depth = nested->settings.depth;
			out.extras = nested->settings.extras.size();
			AssetDatabase::UnloadAsset(asset);
			return out;
		}

		struct Drawn {
			Handle<EditorPanel> panel;
			UI::ElementHandle root;
		};
		Drawn Draw(const Guid& guid, UI::Context& context) {
			EditorSelection::SelectAsset(guid);
			Drawn drawn;
			drawn.panel = OpenInspector();
			EXPECT_TRUE(drawn.panel.IsValid());
			drawn.root = context.CreateElement();
			context.AppendChild(context.GetRoot(), drawn.root);
			drawn.panel.Get()->BuildContent(context, drawn.root);
			context.Update();
			return drawn;
		}

		void Settle(UI::Context& context) {
			PanelManager::TickAll(1.0f / 60.0f);
			context.Update();
		}

		void Retype(UI::Context& context, UI::ElementHandle root, std::string_view from, std::string_view to) {
			UI::ElementHandle field = FindElementByText(context, root, from);
			ASSERT_TRUE(context.IsAlive(field)) << from << "'";
			PressAndRelease(context, field);
			ASSERT_EQ(context.GetFocused(), field);
			context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
			for (char c : to) {
				context.InjectInput(InspectorTextChar(static_cast<char32_t>(c)));
			}
			context.Update();
			PressAway(context, root);
		}

		Guid CreateNested() {
			Guid guid = AssetDatabase::CreateAsset(Path("project://Nested.inest"), StringHash("InspNestedAsset"));
			EXPECT_TRUE(guid.IsValid());
			return guid;
		}
	};

} // anonymous namespace

TEST_F(NestedAssetInspectorTest, NestedStructsDrawFields) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1600};
	UI::Context context(desc, &fonts);
	EditorSelection::SelectAsset(guid);
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	BuildAndCollect(panel, context, texts);
	for (const char* expected : {"entries", "Element 0", "Element 1", "name", "volume", "enabled", "reference", "settings", "depth"}) {
		EXPECT_NE(TextIndex(texts, expected), str::npos) << expected << "'";
	}
	EXPECT_NE(TextIndex(texts, "alpha"), str::npos);
	EXPECT_NE(TextIndex(texts, "37"), str::npos);
	for (const str& text : texts) {
		EXPECT_EQ(text.find("no drawer"), str::npos) << text << "'";
	}
	EXPECT_NE(TextIndex(texts, "2"), str::npos);
}

TEST_F(NestedAssetInspectorTest, ElementFieldCommitsAndUndoes) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1600};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);

	Retype(context, drawn.root, "beta", "gamma");
	Snapshot edited = Live(guid);
	ASSERT_EQ(edited.names.size(), 2u);
	EXPECT_EQ(edited.names[0], "alpha");
	EXPECT_EQ(edited.names[1], "gamma");

	ASSERT_TRUE(UndoSystem::CanUndo());
	UndoSystem::Undo();
	EXPECT_EQ(Live(guid).names[1], "beta");
}

TEST_F(NestedAssetInspectorTest, ANestedStructsFieldCommitsAndUndoes) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1600};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);

	Retype(context, drawn.root, "37", "12");
	EXPECT_EQ(Live(guid).depth, 12);
	UndoSystem::Undo();
	EXPECT_EQ(Live(guid).depth, 37);
}

TEST_F(NestedAssetInspectorTest, AddRemoveAreOneStepEach) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 2000};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);

	UI::ElementHandle add = FindButtonNamed(context, drawn.root, "Add InspNestedEntry");
	ASSERT_TRUE(add.IsValid());
	PressAndRelease(context, add);
	Settle(context);
	EXPECT_EQ(Live(guid).names.size(), 3u);
	EXPECT_TRUE(context.IsAlive(FindElementByText(context, drawn.root, "Element 2")));

	UndoSystem::Undo();
	Settle(context);
	EXPECT_EQ(Live(guid).names.size(), 2u);
	EXPECT_FALSE(context.IsAlive(FindElementByText(context, drawn.root, "Element 2")));

	UI::ElementHandle remove = FindButtonNamed(context, drawn.root, "-");
	ASSERT_TRUE(remove.IsValid());
	PressAndRelease(context, remove);
	Settle(context);
	Snapshot removed = Live(guid);
	ASSERT_EQ(removed.names.size(), 1u);
	EXPECT_EQ(removed.names[0], "beta");

	UndoSystem::Undo();
	Snapshot restored = Live(guid);
	ASSERT_EQ(restored.names.size(), 2u);
	EXPECT_EQ(restored.names[0], "alpha");
	EXPECT_EQ(restored.names[1], "beta");
}

TEST_F(NestedAssetInspectorTest, NestedListGrowsWithUndo) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 2000};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);
	auto elementRows = [&]() {
		Vector<str> texts{GetMemoryResource(MemTag::Editor)};
		CollectTexts(context, drawn.root, texts);
		return std::count(texts.begin(), texts.end(), str("Element 0"));
	};
	ASSERT_EQ(elementRows(), 1);

	UI::ElementHandle add = FindButtonNamed(context, drawn.root, "Add InspDataAsset");
	ASSERT_TRUE(add.IsValid());
	PressAndRelease(context, add);
	Settle(context);
	EXPECT_EQ(Live(guid).extras, 1u);
	EXPECT_EQ(elementRows(), 2);

	UndoSystem::Undo();
	Settle(context);
	EXPECT_EQ(Live(guid).extras, 0u);
	EXPECT_EQ(elementRows(), 1);
}

TEST_F(NestedAssetInspectorTest, ElementFoldSurvivesRebuild) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1600};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);

	UI::ElementHandle label = FindElementByText(context, drawn.root, "Element 0");
	ASSERT_TRUE(context.IsAlive(label));
	const UI::Element* row = context.GetElement(context.MakeHandle(context.GetElement(label)->parent));
	ASSERT_NE(row, nullptr);
	UI::ElementHandle caret = context.MakeHandle(row->firstChild);
	PressAndRelease(context, caret);
	Settle(context);

	EXPECT_FALSE(context.IsAlive(FindElementByText(context, drawn.root, "alpha")));
	EXPECT_TRUE(context.IsAlive(FindElementByText(context, drawn.root, "beta")));

	Guid other = CreateProbeAsset();
	SelectAndSettle(other, context);
	ASSERT_FALSE(context.IsAlive(FindElementByText(context, drawn.root, "beta")));
	SelectAndSettle(guid, context);
	ASSERT_TRUE(context.IsAlive(FindElementByText(context, drawn.root, "beta")));
	EXPECT_FALSE(context.IsAlive(FindElementByText(context, drawn.root, "alpha")));
}

namespace {

	constexpr const char* DATA_RELOAD_V2 = "ShalltearHotReloadModuleV2.dll";
	constexpr const char* DATA_RELOAD_GONE = "ShalltearHotReloadModuleGone.dll";
	const StringHash DataAssetType{"HotReloadDataAsset"};
	constexpr const char* DATA_ASSET_PATH = "project://InspectorProbe.hrdata";
	constexpr i32 PROBE_DAMAGE = 4242;

	class DataAssetInspectorReloadFixture : public InspectorTest {
	protected:
		std::filesystem::path _root;
		ModuleHandle _module;
		Guid _assetId;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_datainsprel_" + std::to_string(stamp) + "_" +
					 std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			ModuleSystem::Initialize();
			InspectorTest::SetUp();

			_module = ModuleSystem::Load(Path(RELOAD_MODULE_V1));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
			ASSERT_NE(TypeRegistry::FindType(DataAssetType), nullptr);
			_assetId = AssetDatabase::CreateAsset(Path(DATA_ASSET_PATH), DataAssetType);
			ASSERT_TRUE(_assetId.IsValid());
		}

		void TearDown() override {
			InspectorTest::TearDown();
			ModuleSystem::Terminate();
			AssetDatabase::CollectGarbage();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		bool ReloadTo(const char* dll) {
			Path target{dll};
			return ModuleSystem::Reload(_module, &target);
		}

		void SetDamage(i32 value) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(_assetId);
			ASSERT_NE(asset, nullptr);
			Variant boxed{value};
			ASSERT_TRUE(SetField(asset, asset->GetType(), StringHash("damage"), &boxed));
			AssetDatabase::UnloadAsset(asset);
		}

		i32 LiveDamage() {
			Asset* asset = AssetDatabase::LoadAssetByGuid(_assetId);
			EXPECT_NE(asset, nullptr);
			if (!asset) {
				return -1;
			}
			Variant out;
			EXPECT_TRUE(GetField(asset, asset->GetType(), StringHash("damage"), &out));
			AssetDatabase::UnloadAsset(asset);
			const i32* value = out.TryGet<i32>();
			return value ? *value : -1;
		}
	};

} // anonymous namespace

TEST_F(DataAssetInspectorReloadFixture, GenericBodyStillCommitsAfterModuleReloads) {
	SetDamage(PROBE_DAMAGE);
	EditorSelection::SelectAsset(_assetId);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle field = FindElementByText(context, root, std::to_string(PROBE_DAMAGE));
	ASSERT_TRUE(context.IsAlive(field));

	ASSERT_TRUE(ReloadTo(DATA_RELOAD_V2));
	EXPECT_EQ(LiveDamage(), PROBE_DAMAGE);

	PressAndRelease(context, field);
	ASSERT_EQ(context.GetFocused(), field);
	context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
	context.InjectInput(InspectorTextChar(U'7'));
	context.InjectInput(InspectorTextChar(U'7'));
	context.Update();
	PressAway(context, root);

	EXPECT_EQ(LiveDamage(), 77);
}

TEST_F(DataAssetInspectorReloadFixture, FieldReloadAddedGetsRowWithoutReselecting) {
	EditorSelection::SelectAsset(_assetId);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<str> before{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, root, before);
	ASSERT_NE(TextIndex(before, "damage"), str::npos);
	ASSERT_EQ(TextIndex(before, "tier"), str::npos);

	ASSERT_TRUE(ReloadTo(DATA_RELOAD_V2));
	PanelManager::TickAll(1.0f / 60.0f);
	context.Update();

	Vector<str> after{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, root, after);
	EXPECT_NE(TextIndex(after, "tier"), str::npos);
	EXPECT_NE(TextIndex(after, "damage"), str::npos);
}

TEST_F(DataAssetInspectorReloadFixture, LostTypeDropsBody) {
	SetDamage(PROBE_DAMAGE);
	EditorSelection::SelectAsset(_assetId);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1200};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	ASSERT_TRUE(context.IsAlive(FindElementByText(context, root, std::to_string(PROBE_DAMAGE))));

	ASSERT_TRUE(ReloadTo(DATA_RELOAD_GONE));
	PanelManager::TickAll(1.0f / 60.0f);
	context.Update();

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, root, texts);
	EXPECT_EQ(TextIndex(texts, "damage"), str::npos);
	EXPECT_EQ(TextIndex(texts, std::to_string(PROBE_DAMAGE)), str::npos);
	EXPECT_NE(TextIndex(texts, "Path"), str::npos);
	EXPECT_NE(TextIndex(texts, "Guid"), str::npos);
}

namespace {

	class InspDrawerComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspDrawerComponent, Component)
		SLT_REFLECT_FIELD(tag)
		SLT_REFLECT_FIELD(note)
		SLT_END_REFLECT()

		InspDrawerComponent() = default;

		str tag = "alpha";
		str note = "beta";
	};

	class InspDrawerChild : public InspDrawerComponent {
	public:
		SLT_REFLECT_COMPONENT(InspDrawerChild, InspDrawerComponent)
		SLT_END_REFLECT()

		InspDrawerChild() = default;
	};

	class InspDrawerLists : public Component {
	public:
		SLT_REFLECT_COMPONENT(InspDrawerLists, Component)
		SLT_REFLECT_ARRAY(drawn)
		SLT_REFLECT_ARRAY(plain)
		SLT_END_REFLECT()

		InspDrawerLists() = default;

		std::vector<i32> drawn;
		std::vector<i32> plain;
	};

	constexpr const char* DRAWN_CLASS = "insp-drawn-marker";
	constexpr const char* TYPE_DRAWN_CLASS = "insp-type-drawn-marker";

	void MarkRow(UI::Context& context, UI::ElementHandle row, const char* klass) {
		UI::ElementHandle mark = context.CreateElement();
		context.AddClass(mark, StringHash(klass));
		context.AppendChild(row, mark);
	}

	class DrawerCapturingSink : public ILogSink {
	public:
		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			_messages.push_back(str(event.message));
		}
		void Flush() override {}

		u32 CountMatching(std::string_view needle) const {
			std::lock_guard<std::mutex> lock(_mutex);
			u32 count = 0;
			for (const str& message : _messages) {
				if (message.find(needle) != str::npos) {
					++count;
				}
			}
			return count;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
	};

	const u8 _DRAWER_OWNER = 0;
	const u8 _OTHER_OWNER = 0;

	class InspectorDrawerTest : public InspectorTest {
	protected:
		void SetUp() override {
			InspectorTest::SetUp();
			InspDrawerComponent::GetReflectType();
			InspDrawerChild::GetReflectType();
			InspDrawerLists::GetReflectType();
			PropertyDrawers::Initialize();
		}
		void TearDown() override {
			PropertyDrawers::UnregisterOwnedBy(&_DRAWER_OWNER);
			PropertyDrawers::UnregisterOwnedBy(&_OTHER_OWNER);
			InspectorTest::TearDown();
		}

		u32 BuildAndCount(UI::Context& context, const char* klass) {
			Handle<EditorPanel> panel = OpenInspector();
			if (!panel.IsValid()) {
				return 0;
			}
			UI::ElementHandle root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			panel.Get()->BuildContent(context, root);
			context.Update();
			return CountByClass(context, root, StringHash(klass));
		}
	};

} // anonymous namespace

TEST_F(InspectorDrawerTest, FieldDrawerClaimsOnlyItsField) {
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InspDrawerComponent"), StringHash("tag"),
		[](UI::Context& context, UI::ElementHandle row, FieldRef, EditTarget) {
			MarkRow(context, row, DRAWN_CLASS);
		},
		&_DRAWER_OWNER));

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspDrawerComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	EXPECT_EQ(BuildAndCount(context, DRAWN_CLASS), 1u);
}

TEST_F(InspectorDrawerTest, FieldDrawerBeatsTypeDrawer) {
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawer(
		StringHash("str"),
		[](UI::Context& context, UI::ElementHandle row, FieldRef, EditTarget) {
			MarkRow(context, row, TYPE_DRAWN_CLASS);
		},
		&_DRAWER_OWNER));
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InspDrawerComponent"), StringHash("tag"),
		[](UI::Context& context, UI::ElementHandle row, FieldRef, EditTarget) {
			MarkRow(context, row, DRAWN_CLASS);
		},
		&_DRAWER_OWNER));

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspDrawerComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_EQ(CountByClass(context, root, StringHash(DRAWN_CLASS)), 1u);
	EXPECT_EQ(CountByClass(context, root, StringHash(TYPE_DRAWN_CLASS)), 1u);
}

TEST_F(InspectorDrawerTest, LengthDrawerSkipsRebuild) {
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InspDrawerLists"), StringHash("drawn"),
		[](UI::Context& context, UI::ElementHandle row, FieldRef, EditTarget) {
			MarkRow(context, row, DRAWN_CLASS);
		},
		&_DRAWER_OWNER));

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	InspDrawerLists* lists = object.Get()->AddComponent<InspDrawerLists>().Get();
	ASSERT_NE(lists, nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	const UI::ElementHandle mark = FindByClass(context, root, StringHash(DRAWN_CLASS), 0);
	ASSERT_TRUE(context.IsAlive(mark));

	lists->drawn.push_back(1);
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_TRUE(context.IsAlive(mark));

	lists->plain.push_back(1);
	panel.Get()->OnTick(0.0f);
	context.Update();
	EXPECT_FALSE(context.IsAlive(mark));
	EXPECT_EQ(CountByClass(context, root, StringHash(DRAWN_CLASS)), 1u);
}

TEST_F(InspectorDrawerTest, FieldDrawerReachesSubtypes) {
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InspDrawerComponent"), StringHash("tag"),
		[](UI::Context& context, UI::ElementHandle row, FieldRef, EditTarget) {
			MarkRow(context, row, DRAWN_CLASS);
		},
		&_DRAWER_OWNER));

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspDrawerChild>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	EXPECT_EQ(BuildAndCount(context, DRAWN_CLASS), 1u);
}

TEST_F(InspectorDrawerTest, RowRefreshRunsUntilElementDies) {
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {300, 300};
	UI::Context context(desc, &fonts);
	UI::ElementHandle row = context.CreateElement();
	context.AppendChild(context.GetRoot(), row);

	u32 runs = 0;
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(context, row, [&runs](UI::Context&) { ++runs; }, &_DRAWER_OWNER));

	PropertyDrawers::RefreshRows(context);
	EXPECT_EQ(runs, 1u);
	PropertyDrawers::RefreshRows(context);
	EXPECT_EQ(runs, 2u);

	context.DestroyElement(row);
	context.Update();
	PropertyDrawers::RefreshRows(context);
	EXPECT_EQ(runs, 2u);
	EXPECT_FALSE(PropertyDrawers::DropRow(context, row));
}

TEST_F(InspectorDrawerTest, RowRefreshStaysInContext) {
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {300, 300};
	UI::Context first(desc, &fonts);
	UI::Context second(desc, &fonts);

	UI::ElementHandle firstRow = first.CreateElement();
	first.AppendChild(first.GetRoot(), firstRow);
	UI::ElementHandle secondRow = second.CreateElement();
	second.AppendChild(second.GetRoot(), secondRow);
	ASSERT_EQ(firstRow.index, secondRow.index);
	ASSERT_EQ(firstRow.generation, secondRow.generation);
	ASSERT_NE(firstRow, secondRow);

	u32 firstRuns = 0;
	u32 secondRuns = 0;
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(first, firstRow, [&firstRuns](UI::Context&) { ++firstRuns; }, &_DRAWER_OWNER));
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(second, secondRow, [&secondRuns](UI::Context&) { ++secondRuns; }, &_DRAWER_OWNER));

	PropertyDrawers::RefreshRows(first);
	EXPECT_EQ(firstRuns, 1u);
	EXPECT_EQ(secondRuns, 0u);

	PropertyDrawers::RefreshRows(second);
	EXPECT_EQ(firstRuns, 1u);
	EXPECT_EQ(secondRuns, 1u);
}

TEST_F(InspectorDrawerTest, SweepTakesModuleRows) {
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {300, 300};
	UI::Context context(desc, &fonts);
	UI::ElementHandle mine = context.CreateElement();
	UI::ElementHandle theirs = context.CreateElement();
	context.AppendChild(context.GetRoot(), mine);
	context.AppendChild(context.GetRoot(), theirs);

	u32 mineRuns = 0;
	u32 theirsRuns = 0;
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(context, mine, [&mineRuns](UI::Context&) { ++mineRuns; }, &_DRAWER_OWNER));
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(context, theirs, [&theirsRuns](UI::Context&) { ++theirsRuns; }, &_OTHER_OWNER));
	ASSERT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InspDrawerComponent"), StringHash("tag"),
		[](UI::Context&, UI::ElementHandle, FieldRef, EditTarget) {}, &_DRAWER_OWNER));

	EXPECT_EQ(PropertyDrawers::UnregisterOwnedBy(&_DRAWER_OWNER), 2u);
	EXPECT_EQ(PropertyDrawers::FindFieldDrawerFor(StringHash("InspDrawerComponent"), StringHash("tag")), nullptr);

	PropertyDrawers::RefreshRows(context);
	EXPECT_EQ(mineRuns, 0u);
	EXPECT_EQ(theirsRuns, 1u);
}

TEST_F(InspectorDrawerTest, InRefreshRegistrationRefused) {
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {300, 300};
	UI::Context context(desc, &fonts);
	UI::ElementHandle row = context.CreateElement();
	UI::ElementHandle late = context.CreateElement();
	context.AppendChild(context.GetRoot(), row);
	context.AppendChild(context.GetRoot(), late);

	bool refused = false;
	bool replaced = false;
	ASSERT_TRUE(PropertyDrawers::SetRowRefresh(
		context, row,
		[&refused, &replaced, late, row](UI::Context& ctx) {
			refused = !PropertyDrawers::SetRowRefresh(ctx, late, [](UI::Context&) {}, &_DRAWER_OWNER);
			replaced = PropertyDrawers::SetRowRefresh(ctx, row, [](UI::Context&) {}, &_DRAWER_OWNER);
		},
		&_DRAWER_OWNER));

	PropertyDrawers::RefreshRows(context);
	EXPECT_TRUE(refused);
	EXPECT_TRUE(replaced);
}

TEST(PropertyDrawerRegistryTest, RegistrationBeforeInitializeIsRefusedAndReported) {
	PropertyDrawers::Terminate();

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<DrawerCapturingSink>();
	LogManager::AddSink(sink);

	EXPECT_FALSE(PropertyDrawers::RegisterFieldDrawer(
		StringHash("TooEarly"), [](UI::Context&, UI::ElementHandle, FieldRef, EditTarget) {}));
	EXPECT_FALSE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("TooEarly"), StringHash("field"), [](UI::Context&, UI::ElementHandle, FieldRef, EditTarget) {}));
	EXPECT_FALSE(PropertyDrawers::RegisterComponentInspector(
		StringHash("TooEarly"), [](UI::Context&, UI::ElementHandle, Handle<Component>) {}));
	EXPECT_FALSE(PropertyDrawers::RegisterAssetInspector(
		StringHash("TooEarly"), [](UI::Context&, UI::ElementHandle, const Guid&, Asset&) {}));
	{
		AssetInspectorSectionDesc section;
		section.label = "TooEarly";
		section.draw = [](UI::Context&, UI::ElementHandle, const Guid&, Asset&) {};
		EXPECT_FALSE(PropertyDrawers::RegisterAssetInspectorSection(StringHash("TooEarly"), std::move(section)));
	}

	LogManager::Terminate();
	ASSERT_GT(sink->CountMatching(""), 0u);
	EXPECT_EQ(sink->CountMatching("before PropertyDrawers::Initialize"), 5u);
	sink.reset();
	JobSystem::Terminate();

	PropertyDrawers::Initialize();
	EXPECT_TRUE(PropertyDrawers::RegisterFieldDrawerFor(
		StringHash("InTime"), StringHash("field"), [](UI::Context&, UI::ElementHandle, FieldRef, EditTarget) {}));
	EXPECT_NE(PropertyDrawers::FindFieldDrawerFor(StringHash("InTime"), StringHash("field")), nullptr);
	EXPECT_EQ(PropertyDrawers::FindFieldDrawerFor(StringHash("InTime"), StringHash("other")), nullptr);
	EXPECT_EQ(PropertyDrawers::FindFieldDrawerFor(StringHash("Other"), StringHash("field")), nullptr);
	PropertyDrawers::Terminate();
}

TEST_F(InspectorTest, ClickingReferenceFieldPingsAssetItNames) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	ASSERT_NE(rendererHandle.Get(), nullptr);
	MeshAsset mesh;
	mesh.assetId = Guid{0x1234, 0x5678};
	rendererHandle.Get()->SetMesh(Handle<MeshAsset>(&mesh));
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	PingWatcher* watcher = New<PingWatcher>(MemTag::Editor);
	EditorEvents::PingRequested.Subscribe(EventBus::GetGlobal(), Handle<PingWatcher>(watcher),
										  &PingWatcher::OnPing);

	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ObjectField, fields);
	ASSERT_FALSE(fields.empty());

	UI::ElementHandle caption = FindByClass(context, fields[0], EditorTheme::Class::RowText, 0);
	ASSERT_TRUE(caption.IsValid());
	ASSERT_GT(context.GetLayout(caption).borderBox.w, 0.0f);

	ClickElement(context, caption);
	context.Update();
	ASSERT_EQ(watcher->count, 1u);
	EXPECT_EQ(watcher->last.asset, mesh.assetId);
	EXPECT_EQ(watcher->last.object, 0u);

	MeshAsset other;
	other.assetId = Guid{0xABCD, 0xEF01};
	rendererHandle.Get()->SetMesh(Handle<MeshAsset>(&other));
	ClickElement(context, caption);
	context.Update();
	ASSERT_EQ(watcher->count, 2u);
	EXPECT_EQ(watcher->last.asset, other.assetId);

	rendererHandle.Get()->SetMesh({});
	ClickElement(context, caption);
	context.Update();
	EXPECT_EQ(watcher->count, 2u);

	Delete(watcher);
}

TEST_F(InspectorTest, ReferenceClickPingsObject) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Holder", _scene);
	Handle<SceneObject> named = SceneSystem::CreateObject("Main Camera", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	ASSERT_NE(component.Get(), nullptr);
	component.Get()->targetRef = named;
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	PingWatcher* watcher = New<PingWatcher>(MemTag::Editor);
	EditorEvents::PingRequested.Subscribe(EventBus::GetGlobal(), Handle<PingWatcher>(watcher),
										  &PingWatcher::OnPing);

	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, EditorTheme::Class::ObjectField, fields);
	ASSERT_EQ(fields.size(), 1u);
	UI::ElementHandle caption = FindByClass(context, fields[0], EditorTheme::Class::RowText, 0);
	ASSERT_TRUE(caption.IsValid());
	ASSERT_GT(context.GetLayout(caption).borderBox.w, 0.0f);

	ClickElement(context, caption);
	context.Update();
	ASSERT_EQ(watcher->count, 1u);
	EXPECT_EQ(watcher->last.object, named.GetInstanceId());
	EXPECT_FALSE(watcher->last.asset.IsValid());
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(object.Get())));

	Delete(watcher);
}

TEST_F(InspectorTest, ObjectFieldTakesFirstOfDraggedRun) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Holder", _scene);
	Handle<SceneObject> first = SceneSystem::CreateObject("First", _scene);
	Handle<SceneObject> second = SceneSystem::CreateObject("Second", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	ASSERT_NE(component.Get(), nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	auto dropRun = [&](std::initializer_list<Handle<SceneObject>> run) {
		panel.Get()->OnTick(0.016f);
		context.Update();
		Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
		CollectByClass(context, root, EditorTheme::Class::ObjectField, fields);
		ASSERT_EQ(fields.size(), 1u);
		UI::DragPayload payload;
		payload.type = "SceneObject"_hash;
		SceneObjectDragData data;
		for (Handle<SceneObject> dragged : run) {
			data.objects.push_back(dragged.GetInstanceId());
		}
		payload.data = Variant(std::move(data));
		ASSERT_TRUE(context.DeliverDrop(payload, CenterOfElement(context, fields[0]))) << run.size();
	};

	dropRun({first, second});
	EXPECT_EQ(component.Get()->targetRef, first);
	dropRun({second});
	EXPECT_EQ(component.Get()->targetRef, second);
}

namespace {

	struct InspectorBuild {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;

		explicit InspectorBuild(f32 hostWidthPx) : context(MakeDesc(), &fonts) {
			EditorTheme::ApplyToContext(context);
			UI::ElementHandle host = context.CreateElement();
			UI::Style hostStyle;
			hostStyle.flexDirection = UI::FlexDirection::Column;
			hostStyle.width = UI::Length::Pixel(hostWidthPx);
			hostStyle.height = UI::Length::Percent(1.0f);
			context.SetStyle(host, hostStyle);
			context.AppendChild(context.GetRoot(), host);
			root = context.CreateElement();
			context.AppendChild(host, root);
		}

		static UI::ContextDesc MakeDesc() {
			UI::ContextDesc desc;
			desc.sizePx = {900, 900};
			return desc;
		}
	};

} // namespace

TEST_F(InspectorTest, ScrubHandleKeepsWidthAsPanelNarrows) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	const auto measure = [&panel](f32 hostWidthPx, f32& handleWidth, f32& gapToField) {
		InspectorBuild build{hostWidthPx};
		panel.Get()->BuildContent(build.context, build.root);
		build.context.Update();

		UI::ElementHandle pair = UITest::FindByClass(build.context, build.root, StringHash("slt-numberfield"), 0);
		ASSERT_TRUE(build.context.IsAlive(pair));
		UI::ElementHandle label = UITest::FindByClass(build.context, pair, StringHash("slt-number-label"), 0);
		UI::ElementHandle field = UITest::FindByClass(build.context, pair, StringHash("slt-number-input"), 0);
		ASSERT_TRUE(build.context.IsAlive(label) && build.context.IsAlive(field));

		const UI::Rect& labelBox = build.context.GetLayout(label).borderBox;
		const UI::Rect& fieldBox = build.context.GetLayout(field).borderBox;
		handleWidth = labelBox.w;
		const UI::Rect& letter = build.context.GetLayout(label).content;
		gapToField = fieldBox.x - (letter.x + letter.w);
	};

	f32 wideHandle = 0.0f, wideGap = 0.0f;
	f32 narrowHandle = 0.0f, narrowGap = 0.0f;
	measure(500.0f, wideHandle, wideGap);
	measure(220.0f, narrowHandle, narrowGap);

	ASSERT_GT(wideHandle, 0.0f);
	EXPECT_FLOAT_EQ(narrowHandle, wideHandle);
	EXPECT_GT(wideGap, 0.0f);
	EXPECT_GT(narrowGap, 0.0f);
}

TEST_F(InspectorTest, VectorFieldsFitDockedWidth) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<Light>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	InspectorBuild build{310.0f};
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();

	Vector<UI::ElementHandle> pairs{GetMemoryResource(MemTag::Editor)};
	CollectByClass(build.context, build.root, StringHash("slt-numberfield"), pairs);
	ASSERT_GE(pairs.size(), 13u);

	for (u32 i = 0; i < pairs.size(); ++i) {
		UI::ElementHandle label = UITest::FindByClass(build.context, pairs[i], StringHash("slt-number-label"), 0);
		UI::ElementHandle input = UITest::FindByClass(build.context, pairs[i], StringHash("slt-number-input"), 0);
		ASSERT_TRUE(build.context.IsAlive(input));
		const UI::Rect& pair = build.context.GetLayout(pairs[i]).borderBox;
		const UI::Rect& inputBox = build.context.GetLayout(input).borderBox;
		if (build.context.IsAlive(label)) {
			const UI::Rect& labelBox = build.context.GetLayout(label).borderBox;
			EXPECT_LE(labelBox.x + labelBox.w, inputBox.x + 0.01f) << "component " << i;
			const UI::Rect& letter = build.context.GetLayout(label).content;
			EXPECT_GT(inputBox.x, letter.x + letter.w) << "component " << i;
		}
		EXPECT_LE(inputBox.x + inputBox.w, pair.x + pair.w + 0.01f);
	}

	for (u32 i = 1; i < pairs.size(); ++i) {
		const UI::Rect& previous = build.context.GetLayout(pairs[i - 1]).borderBox;
		const UI::Rect& current = build.context.GetLayout(pairs[i]).borderBox;
		if (current.y != previous.y) {
			continue;
		}
		EXPECT_GT(current.x, previous.x + previous.w);
	}

	UI::ElementHandle color = UITest::FindByClass(build.context, build.root, StringHash("slt-color-field"), 0);
	ASSERT_TRUE(build.context.IsAlive(color));
	UI::ElementHandle row = build.context.MakeHandle(build.context.GetElement(color)->parent);
	Vector<UI::ElementHandle> channels{GetMemoryResource(MemTag::Editor)};
	CollectByClass(build.context, color, StringHash("slt-numberfield"), channels);
	ASSERT_EQ(channels.size(), 4u);
	const UI::Rect& lastChannel = build.context.GetLayout(channels.back()).borderBox;
	const UI::Rect& colorBox = build.context.GetLayout(color).borderBox;
	EXPECT_LE(lastChannel.x + lastChannel.w, colorBox.x + colorBox.w + 0.01f);
	UI::ElementHandle name = UITest::FindByClass(build.context, row, EditorTheme::Class::FieldLabel, 0);
	ASSERT_TRUE(build.context.IsAlive(name));
	const UI::Style nameStyle = build.context.GetComputedStyle(name);
	ASSERT_EQ(nameStyle.minWidth.unit, UI::Length::Unit::Pixel);
	EXPECT_FLOAT_EQ(build.context.GetLayout(name).borderBox.w, nameStyle.minWidth.value) << build.context.GetLayout(name).borderBox.w << ' ' << nameStyle.minWidth.value;
}

TEST_F(InspectorTest, HeaderButtonsDontTouch) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	InspectorBuild build{500.0f};
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle card = UITest::FindByClass(build.context, build.root, EditorTheme::Class::ComponentCard, 0);
	ASSERT_TRUE(build.context.IsAlive(card));
	UI::ElementHandle header = UITest::NthChildOf(build.context, card, 0);
	UI::ElementHandle toggle = UITest::FindByClass(build.context, header, StringHash("slt-toggle"), 0);
	UI::ElementHandle remove = UITest::FindByClass(build.context, header, StringHash("slt-button"), 0);
	ASSERT_TRUE(build.context.IsAlive(toggle) && build.context.IsAlive(remove));

	const UI::Rect& switchBox = build.context.GetLayout(toggle).borderBox;
	const UI::Rect& removeBox = build.context.GetLayout(remove).borderBox;
	ASSERT_LT(switchBox.x, removeBox.x);
	EXPECT_GT(removeBox.x, switchBox.x + switchBox.w);
}

TEST_F(InspectorTest, EnumRowsDropdownFillsValueColumn) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<Light>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	InspectorBuild build{500.0f};
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle dropdown = UITest::FindByClass(build.context, build.root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(build.context.IsAlive(dropdown));
	UI::ElementHandle row = build.context.MakeHandle(build.context.GetElement(dropdown)->parent);
	ASSERT_TRUE(build.context.IsAlive(row));

	const UI::Rect& dropdownBox = build.context.GetLayout(dropdown).borderBox;
	const UI::Rect& area = build.context.GetLayout(row).content;
	EXPECT_NEAR(dropdownBox.x + dropdownBox.w, area.x + area.w, 1.01f);
	UI::ElementHandle label = UITest::NthChildOf(build.context, dropdown, 0);
	ASSERT_TRUE(build.context.IsAlive(label));
	EXPECT_GT(dropdownBox.w, build.context.GetLayout(label).borderBox.w * 2.0f);
}

TEST_F(InspectorTest, SeparatorMarginsFillGap) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	object.Get()->AddComponent<InspTestComponent>();
	object.Get()->AddComponent<Light>();
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	InspectorBuild build{500.0f};
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle body = build.context.MakeHandle(
		build.context.GetElement(UITest::FindByClass(build.context, build.root,
													 EditorTheme::Class::ComponentCard, 0))
			->parent);
	Vector<UI::ElementHandle> flow{GetMemoryResource(MemTag::Editor)};
	for (u32 child = build.context.GetElement(body)->firstChild; child != UI::INVALID_UI_INDEX;) {
		UI::ElementHandle handle = build.context.MakeHandle(child);
		flow.push_back(handle);
		child = build.context.GetElement(handle)->nextSibling;
	}

	u32 checked = 0;
	for (u32 i = 1; i + 1 < flow.size(); ++i) {
		if (!build.context.HasClass(flow[i], StringHash("slt-separator"))) {
			continue;
		}
		const UI::Rect& rule = build.context.GetLayout(flow[i]).borderBox;
		u32 before = i - 1;
		while (before > 0 && build.context.HasClass(flow[before], EditorTheme::Class::InsertStrip)) {
			--before;
		}
		u32 after = i + 1;
		while (after + 1 < flow.size() && build.context.HasClass(flow[after], EditorTheme::Class::InsertStrip)) {
			++after;
		}
		const UI::Rect& above = build.context.GetLayout(flow[before]).borderBox;
		const UI::Rect& below = build.context.GetLayout(flow[after]).borderBox;
		const f32 gapAbove = rule.y - (above.y + above.h);
		const f32 gapBelow = below.y - (rule.y + rule.h);
		EXPECT_GT(gapAbove, 0.0f) << "rule " << i;
		EXPECT_NEAR(gapAbove, gapBelow, 0.51f);
		++checked;
	}
	EXPECT_EQ(checked, 3u);
}

TEST(ComponentMenuTest, GroupWalksBaseChain) {
	ComponentMenu::RegisterBuiltIns();

	EXPECT_EQ(ComponentMenu::CategoryFor(Camera::GetReflectType()), "Rendering");
	EXPECT_EQ(ComponentMenu::CategoryFor(Light::GetReflectType()), "Rendering");
	EXPECT_EQ(ComponentMenu::CategoryFor(MeshRenderer::GetReflectType()), "Rendering");
	EXPECT_EQ(ComponentMenu::CategoryFor(ReflectionProbe::GetReflectType()), "Rendering");

	EXPECT_TRUE(ComponentMenu::CategoryFor(Behavior::GetReflectType()).empty());
	EXPECT_TRUE(ComponentMenu::CategoryFor(Component::GetReflectType()).empty());
	EXPECT_TRUE(ComponentMenu::CategoryFor(SceneObject::GetReflectType()).empty());
	EXPECT_TRUE(ComponentMenu::CategoryFor(nullptr).empty());
}

TEST(ComponentMenuTest, ModuleSweepRestoresOverrides) {
	ComponentMenu::RegisterBuiltIns();
	int moduleA = 0;
	int moduleB = 0;

	ComponentMenu::Register(Camera::GetReflectType()->name, "Cinematics", &moduleA);
	ComponentMenu::Register(Light::GetReflectType()->name, "Lighting", &moduleB);
	EXPECT_EQ(ComponentMenu::CategoryFor(Camera::GetReflectType()), "Cinematics");

	EXPECT_EQ(ComponentMenu::UnregisterOwnedBy(&moduleA), 1u);
	EXPECT_EQ(ComponentMenu::CategoryFor(Camera::GetReflectType()), "Rendering");
	EXPECT_EQ(ComponentMenu::CategoryFor(Light::GetReflectType()), "Lighting");

	EXPECT_EQ(ComponentMenu::UnregisterOwnedBy(nullptr), 0u);
	EXPECT_EQ(ComponentMenu::CategoryFor(Camera::GetReflectType()), "Rendering");
	ComponentMenu::UnregisterOwnedBy(&moduleB);
	EXPECT_EQ(ComponentMenu::CategoryFor(Light::GetReflectType()), "Rendering");
}

TEST_F(InspectorTest, AddComponentNestsGroupedTypes) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	PressAndRelease(context, context.MakeHandle(context.GetElement(addLabel)->parent));
	context.Update();

	auto listTexts = [&context]() {
		Vector<str> texts{GetMemoryResource(MemTag::Editor)};
		CollectTexts(context, context.GetRoot(), texts);
		return texts;
	};

	Vector<str> texts = listTexts();
	EXPECT_FALSE(ContainsText(texts, "Camera"));
	UI::ElementHandle groupRow = FindElementByTextContaining(context, context.GetRoot(), "Rendering");
	ASSERT_TRUE(context.IsAlive(groupRow));

	UI::ElementHandle clickedRow = context.MakeHandle(context.GetElement(groupRow)->parent);
	PressAndRelease(context, clickedRow);
	context.Update();

	EXPECT_FALSE(context.IsAlive(clickedRow));
	texts = listTexts();
	EXPECT_TRUE(ContainsText(texts, "Camera"));
	EXPECT_TRUE(ContainsText(texts, "Light"));

	UI::ElementHandle backRow = FindElementByTextContaining(context, context.GetRoot(), "\xE2\x80\xB9");
	ASSERT_TRUE(context.IsAlive(backRow));
	PressAndRelease(context, context.MakeHandle(context.GetElement(backRow)->parent));
	context.Update();
	texts = listTexts();
	EXPECT_FALSE(ContainsText(texts, "Camera"));
	EXPECT_TRUE(FindElementByTextContaining(context, context.GetRoot(), "Rendering").IsValid());
}

TEST_F(InspectorTest, GroupOnlyListIsNotEmpty) {
	TypeRegistry::ForEachType(
		[](const TypeInfo* type, void*) {
			if ((type->flags & TypeFlagComponent) != 0 && (type->flags & TypeFlagAbstract) == 0 &&
				type->createFn) {
				ComponentMenu::Register(type->name, "Everything");
			}
		},
		nullptr);

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	PressAndRelease(context, context.MakeHandle(context.GetElement(addLabel)->parent));
	context.Update();

	Vector<str> texts{GetMemoryResource(MemTag::Editor)};
	CollectTexts(context, context.GetRoot(), texts);

	ASSERT_TRUE(FindElementByTextContaining(context, context.GetRoot(), "Everything").IsValid());
	for (std::string_view claim : {"no components available", "no matching components", "no C++ module",
								   "compiling. Your components", "the last compile failed",
								   "compiled but failed to load"}) {
		EXPECT_FALSE(ContainsText(texts, claim)) << claim;
	}

	UI::ElementHandle groupRow = FindElementByTextContaining(context, context.GetRoot(), "Everything");
	ASSERT_TRUE(context.IsAlive(groupRow));
	PressAndRelease(context, context.MakeHandle(context.GetElement(groupRow)->parent));
	context.Update();
	texts.clear();
	CollectTexts(context, context.GetRoot(), texts);
	EXPECT_TRUE(ContainsText(texts, "Camera"));

	ComponentMenu::Shutdown();
}

namespace {

	class InspectorPrefabFixture : public InspectorTest {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_inspprefab_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root / "Assets");
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			InspectorTest::SetUp();
			TypeRegistry::Register(const_cast<TypeInfo*>(InspTestComponent::GetReflectType()));
		}

		void TearDown() override {
			InspectorTest::TearDown();
			AssetDatabase::CollectGarbage();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		Guid WritePrefab(bool withComponent) {
			Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
			if (withComponent) {
				unit->AddComponent<InspTestComponent>();
			}
			SceneSystem::CreateObject("Body", _scene, unit);
			Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
			EXPECT_TRUE(guid.IsValid());
			SceneSystem::Destroy(unit);
			SceneSystem::EndFrame();
			return guid;
		}

		struct Built {
			Handle<EditorPanel> panel;
			UI::ElementHandle root;
		};

		Built BuildFor(UI::Context& context, Handle<SceneObject> object) {
			EditorSelection::Select(Handle<Object>(object.Get()));
			Built built;
			built.panel = OpenInspector();
			built.root = context.CreateElement();
			context.AppendChild(context.GetRoot(), built.root);
			built.panel.Get()->BuildContent(context, built.root);
			context.Update();
			return built;
		}

		UI::ElementHandle MarkedRow(UI::Context& context, UI::ElementHandle root, std::string_view label) {
			Vector<UI::ElementHandle> marks{GetMemoryResource(MemTag::Editor)};
			CollectByClass(context, root, EditorTheme::Class::OverrideMark, marks);
			for (UI::ElementHandle mark : marks) {
				UI::ElementHandle row = context.MakeHandle(context.GetElement(mark)->parent);
				if (AnyTextContains(context, row, label)) {
					return row;
				}
			}
			return {};
		}

		UI::ElementHandle OpenKebabEntry(UI::Context& context, UI::ElementHandle root, std::string_view label) {
			UI::ElementHandle kebab = CardKebab(context, root);
			if (!context.IsAlive(kebab)) {
				return {};
			}
			ClickInspectorElement(context, kebab);
			context.Update();
			return MenuEntry(context, label);
		}
	};

} // anonymous namespace

TEST_F(InspectorPrefabFixture, OverriddenRowsMarked) {
	Guid guid = WritePrefab(true);
	Handle<SceneObject> instance = SceneSystem::Instantiate(guid, _scene, {}, Vec3{3.0f, 0.0f, 0.0f});
	ASSERT_TRUE(instance.IsValid());
	Handle<InspTestComponent> stats = instance->GetComponent<InspTestComponent>();
	ASSERT_TRUE(stats.IsValid());
	stats->count = 7;

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Built built = BuildFor(context, instance);

	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 1u);
	EXPECT_TRUE(context.IsAlive(MarkedRow(context, built.root, "count")));
	EXPECT_FALSE(context.IsAlive(MarkedRow(context, built.root, "Position")));

	Handle<SceneObject> body = instance->FindChildByName("Body");
	ASSERT_TRUE(body.IsValid());
	body->SetLocalPosition(Vec3{0.0f, 1.0f, 0.0f});
	EditorSelection::Select(Handle<Object>(body.Get()));
	built.panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 1u);
	EXPECT_TRUE(context.IsAlive(MarkedRow(context, built.root, "Position")));

	Handle<SceneObject> plain = SceneSystem::CreateObject("Plain", _scene);
	plain->AddComponent<InspTestComponent>()->count = 7;
	EditorSelection::Select(Handle<Object>(plain.Get()));
	built.panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 0u);
}

TEST_F(InspectorPrefabFixture, RightClickRevertIsOneUndo) {
	Guid guid = WritePrefab(true);
	Handle<SceneObject> instance = SceneSystem::Instantiate(guid, _scene);
	Handle<InspTestComponent> stats = instance->GetComponent<InspTestComponent>();
	ASSERT_TRUE(stats.IsValid());
	stats->count = 7;

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Built built = BuildFor(context, instance);

	UI::ElementHandle row = MarkedRow(context, built.root, "count");
	ASSERT_TRUE(context.IsAlive(row));
	UI::ElementHandle label = FindElementByTextContaining(context, row, "count");
	ASSERT_TRUE(context.IsAlive(label));
	RightClickElement(context, label);
	UI::ElementHandle revert = MenuEntry(context, "Revert");
	ASSERT_TRUE(context.IsAlive(revert));
	ClickInspectorElement(context, revert);
	EXPECT_EQ(stats->count, 2);

	ASSERT_TRUE(UndoSystem::CanUndo());
	EXPECT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(stats->count, 7);
	EXPECT_TRUE(UndoSystem::Redo());
	EXPECT_EQ(stats->count, 2);

	built.panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 0u);
	EXPECT_TRUE(UndoSystem::Undo());
	built.panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 1u);

	UI::ElementHandle speedLabel = FindElementByTextContaining(context, built.root, "speed");
	ASSERT_TRUE(context.IsAlive(speedLabel));
	RightClickElement(context, speedLabel);
	EXPECT_FALSE(context.IsAlive(MenuEntry(context, "Revert")));
}

TEST_F(InspectorPrefabFixture, ComponentRevertIsOneStep) {
	Guid guid = WritePrefab(true);
	Handle<SceneObject> instance = SceneSystem::Instantiate(guid, _scene);
	Handle<InspTestComponent> stats = instance->GetComponent<InspTestComponent>();
	ASSERT_TRUE(stats.IsValid());
	stats->count = 7;
	stats->speed = 4.0f;
	stats->title = "changed";

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Built built = BuildFor(context, instance);
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 3u);

	UI::ElementHandle entry = OpenKebabEntry(context, built.root, "Revert Modified Component");
	ASSERT_TRUE(context.IsAlive(entry));
	ClickInspectorElement(context, entry);
	EXPECT_EQ(stats->count, 2);
	EXPECT_FLOAT_EQ(stats->speed, 1.0f);
	EXPECT_EQ(stats->title, "hello");

	EXPECT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(stats->count, 7);
	EXPECT_FLOAT_EQ(stats->speed, 4.0f);
	EXPECT_EQ(stats->title, "changed");
	EXPECT_FALSE(UndoSystem::Undo());
}

TEST_F(InspectorPrefabFixture, AddedComponentIsRevertedFromMenu) {
	Guid guid = WritePrefab(false);
	Handle<SceneObject> instance = SceneSystem::Instantiate(guid, _scene);
	ASSERT_TRUE(instance.IsValid());
	instance->AddComponent<InspTestComponent>();

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Built built = BuildFor(context, instance);
	EXPECT_EQ(CountByClass(context, built.root, EditorTheme::Class::OverrideMark), 0u);

	UI::ElementHandle entry = OpenKebabEntry(context, built.root, "Revert Added Component");
	ASSERT_TRUE(context.IsAlive(entry));
	ClickInspectorElement(context, entry);
	SceneSystem::EndFrame();
	EXPECT_EQ(instance->GetComponentCount(), 0u);
	EXPECT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(instance->GetComponentCount(), 1u);
}

TEST_F(InspectorPrefabFixture, RemovedComponentCardRestores) {
	Guid guid = WritePrefab(true);
	Handle<SceneObject> instance = SceneSystem::Instantiate(guid, _scene);
	Handle<InspTestComponent> stats = instance->GetComponent<InspTestComponent>();
	ASSERT_TRUE(stats.IsValid());
	SceneSystem::Destroy(Handle<Component>(stats.Get()));
	SceneSystem::EndFrame();
	ASSERT_EQ(instance->GetComponentCount(), 0u);

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	Built built = BuildFor(context, instance);
	EXPECT_TRUE(AnyTextContains(context, built.root, "(Removed)"));

	UI::ElementHandle entry = OpenKebabEntry(context, built.root, "Revert Removed Component");
	ASSERT_TRUE(context.IsAlive(entry));
	ClickInspectorElement(context, entry);
	SceneSystem::EndFrame();
	ASSERT_EQ(instance->GetComponentCount(), 1u);
	Handle<InspTestComponent> back = instance->GetComponent<InspTestComponent>();
	ASSERT_TRUE(back.IsValid());
	EXPECT_FLOAT_EQ(back->speed, 1.0f);
	EXPECT_NE(back->GetLocalId(), 0u);

	built.panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_FALSE(AnyTextContains(context, built.root, "(Removed)"));

	EXPECT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(instance->GetComponentCount(), 0u);
	EXPECT_TRUE(UndoSystem::Redo());
	SceneSystem::EndFrame();
	EXPECT_EQ(instance->GetComponentCount(), 1u);
}

static Guid StageAVariantWithItsOwnCamera(Handle<Scene> scene) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
	unit->AddComponent<InspTestComponent>();
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	if (!base.IsValid()) {
		return {};
	}
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	Guid variant = EditorSceneOps::CreatePrefabVariantOf(base, Path("project://Assets/Unit Variant.prefab"));
	if (!variant.IsValid()) {
		return {};
	}
	Handle<Scene> stage = EditorSceneOps::OpenPrefab(variant);
	if (!stage.IsValid() || !stage->GetRoot(0)->AddComponent<Camera>().IsValid() ||
		!EditorSceneOps::SavePrefabScene(stage)) {
		return {};
	}
	SceneSystem::EndFrame();
	EditorSceneOps::LeavePrefabStage();
	SceneSystem::EndFrame();
	return variant;
}

TEST_F(PrefabInspectorTest, VariantsComponentDrawsFieldsNotNoDrawer) {
	Guid variant = StageAVariantWithItsOwnCamera(_scene);
	ASSERT_TRUE(variant.IsValid());
	EditorSelection::SelectAsset(variant);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1400};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	ASSERT_TRUE(AnyTextContains(context, root, "Camera"));
	EXPECT_FALSE(AnyTextContains(context, root, "no drawer"));
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-numberfield")), 26u);
}

TEST_F(PrefabInspectorTest, VariantAddedComponentMarked) {
	Guid variant = StageAVariantWithItsOwnCamera(_scene);
	ASSERT_TRUE(variant.IsValid());
	EditorSelection::SelectAsset(variant);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1400};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	ASSERT_TRUE(AnyTextContains(context, root, "Camera"));

	const Handle<VectorAsset> morePicture = EditorIcons::GetGlyph(EditorIcons::Glyph::More);
	Vector<UI::ElementHandle> buttons{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, root, StringHash("slt-button"), buttons);
	UI::ElementHandle kebab{};
	for (UI::ElementHandle button : buttons) {
		if (UITest::FindByIcon(context, button, morePicture).IsValid()) {
			kebab = button;
		}
	}
	ASSERT_TRUE(context.IsAlive(kebab));
	ClickInspectorElement(context, kebab);
	context.Update();
	Vector<UI::ElementHandle> entries{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-menu-item"), entries);
	bool offersRevertAdded = false;
	for (UI::ElementHandle entry : entries) {
		offersRevertAdded = offersRevertAdded || AnyTextContains(context, entry, "Revert Added Component");
	}
	EXPECT_TRUE(offersRevertAdded);
}

TEST_F(PrefabInspectorTest, VariantInspectorNamesBase) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(base.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	Guid variant = EditorSceneOps::CreatePrefabVariantOf(base, Path("project://Assets/Unit Variant.prefab"));
	ASSERT_TRUE(variant.IsValid());

	auto build = [this](const Guid& asset, UI::Context& context, UI::ElementHandle& root) {
		EditorSelection::SelectAsset(asset);
		Handle<EditorPanel> panel = OpenInspector();
		ASSERT_TRUE(panel.IsValid());
		root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();
	};

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	{
		UI::Context context(desc, &fonts);
		UI::ElementHandle root;
		build(base, context, root);
		EXPECT_FALSE(AnyTextContains(context, root, "Base"));
	}

	UI::Context context(desc, &fonts);
	UI::ElementHandle root;
	build(variant, context, root);
	EXPECT_TRUE(AnyTextContains(context, root, "Prefab Variant"));
	EXPECT_TRUE(AnyTextContains(context, root, "Base"));
	UI::ElementHandle reveal = ButtonNamed(context, root, "Unit.prefab");
	ASSERT_TRUE(reveal.IsValid());

	EditorSelection::SelectAsset(variant);
	ClickInspectorElement(context, reveal);
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), base);
}

class InspectorPassListTest : public InspectorTest {
protected:
	void SetUp() override {
		InspectorTest::SetUp();
		static std::atomic<u64> counter{0};
		u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
		_root = std::filesystem::temp_directory_path() /
				("shalltear_passlist_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
		std::filesystem::create_directories(_root / "Assets");
		FileSystem::Mount("project", Path(_root.generic_string()));
		AssetDatabase::Initialize();
	}

	void TearDown() override {
		InspectorTest::TearDown();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::filesystem::path _root;
};

TEST_F(InspectorPassListTest, CameraPassListAddsPicked) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Eye", _scene);
	Handle<Camera> camera = object.Get()->AddComponent<Camera>();
	ASSERT_NE(camera.Get(), nullptr);
	ASSERT_EQ(camera.Get()->GetPassCount(), 0u);
	ASSERT_NE(ObjectPass::GetReflectType(), nullptr);
	ASSERT_NE(FullScreenPass::GetReflectType(), nullptr);

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "Add RenderPass"));

	UI::ElementHandle add = FindByText(context, root, "Add RenderPass");
	ASSERT_TRUE(context.IsAlive(add));
	ClickElement(context, add);
	context.Update();

	Vector<UI::ElementHandle> entries{GetMemoryResource(MemTag::Editor)};
	CollectByClass(context, context.GetRoot(), StringHash("slt-menu-item"), entries);
	UI::ElementHandle objectPassItem{};
	bool sawFullScreen = false;
	for (UI::ElementHandle entry : entries) {
		if (AnyTextContains(context, entry, "ObjectPass")) {
			objectPassItem = entry;
		}
		if (AnyTextContains(context, entry, "FullScreenPass")) {
			sawFullScreen = true;
		}
	}
	ASSERT_TRUE(context.IsAlive(objectPassItem));
	EXPECT_TRUE(sawFullScreen);

	ClickElement(context, objectPassItem);
	ASSERT_EQ(camera.Get()->GetPassCount(), 1u);
	EXPECT_TRUE(camera.Get()->GetPass(0).IsValid());

	Asset* minted = static_cast<Asset*>(camera.Get()->GetPass(0).Get());
	ASSERT_NE(minted, nullptr);
	EXPECT_TRUE(minted->assetId.IsValid());
	EXPECT_EQ(minted->GetType(), ObjectPass::GetReflectType());

	UndoSystem::Undo();
	EXPECT_EQ(camera.Get()->GetPassCount(), 0u);
}

TEST_F(InspectorPassListTest, MiddleRowRemovalClosesGap) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Eye", _scene);
	Handle<Camera> camera = object.Get()->AddComponent<Camera>();
	ASSERT_NE(camera.Get(), nullptr);

	ObjectPass* first = New<ObjectPass>(MemTag::Asset);
	ObjectPass* middle = New<ObjectPass>(MemTag::Asset);
	ObjectPass* last = New<ObjectPass>(MemTag::Asset);
	camera.Get()->SetPass(0, Handle<RenderPass>(first));
	camera.Get()->SetPass(1, Handle<RenderPass>(middle));
	camera.Get()->SetPass(2, Handle<RenderPass>(last));
	ASSERT_EQ(camera.Get()->GetPassCount(), 3u);
	const u64 firstId = camera.Get()->GetPass(0).GetInstanceId();
	const u64 middleId = camera.Get()->GetPass(1).GetInstanceId();
	const u64 lastId = camera.Get()->GetPass(2).GetInstanceId();

	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_TRUE(AnyTextContains(context, root, "passes 2"));

	UndoSystem::Push(MakeUnique<RemoveContainerElementCommand>(
		EditTarget(Handle<Object>(static_cast<Object*>(camera.Get()))), StringHash("_passes"), 1u, "Remove Element"));

	ASSERT_EQ(camera.Get()->GetPassCount(), 2u);
	EXPECT_EQ(camera.Get()->GetPass(0).GetInstanceId(), firstId);
	EXPECT_EQ(camera.Get()->GetPass(1).GetInstanceId(), lastId);

	UndoSystem::Undo();
	ASSERT_EQ(camera.Get()->GetPassCount(), 3u);
	EXPECT_EQ(camera.Get()->GetPass(1).GetInstanceId(), middleId);

	camera.Get()->SetPass(0, {});
	camera.Get()->SetPass(1, {});
	camera.Get()->SetPass(2, {});
	Delete(first);
	Delete(middle);
	Delete(last);
}

TEST_F(InspectorTest, IntegerVectorScrubsWhole) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspVecTestComponent> vec = object.Get()->AddComponent<InspVecTestComponent>();
	ASSERT_TRUE(vec.IsValid());
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1000};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	ASSERT_TRUE(AnyTextContains(context, root, "gridSize"));
	EXPECT_FALSE(AnyTextContains(context, root, "no drawer"));

	UI::ElementHandle x = UITest::FindByClass(context, root, StringHash("slt-number-label"), 9);
	UI::ElementHandle y = UITest::FindByClass(context, root, StringHash("slt-number-label"), 10);
	ASSERT_TRUE(x.IsValid());
	ASSERT_TRUE(y.IsValid());
	EXPECT_FALSE(UITest::FindByClass(context, root, StringHash("slt-number-label"), 11).IsValid());

	const UI::Rect& box = context.GetLayout(x).borderBox;
	const i32 px = static_cast<i32>(box.x + box.w * 0.5f);
	const i32 py = static_cast<i32>(box.y + box.h * 0.5f);

	InputEvent event{};
	event.type = InputEventType::MouseMove;
	event.mousePosPx = {px, py};
	context.InjectInput(event);
	event.type = InputEventType::MouseDown;
	event.button = MouseButton::Left;
	context.InjectInput(event);
	event.type = InputEventType::MouseMove;
	event.mousePosPx = {px + 30, py};
	context.InjectInput(event);
	context.Update();
	event.type = InputEventType::MouseUp;
	context.InjectInput(event);
	context.Update();

	EXPECT_NE(vec.Get()->gridSize.x, 4);
	EXPECT_GT(vec.Get()->gridSize.x, 4);
	EXPECT_EQ(vec.Get()->gridSize.y, 3);

	UndoSystem::Undo();
	EXPECT_EQ(vec.Get()->gridSize.x, 4);
	EXPECT_EQ(vec.Get()->gridSize.y, 3);
}

TEST_F(InspectorTest, QuaternionFieldDrawsAndEditsEulerAngles) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspQuatTestComponent> quat = object.Get()->AddComponent<InspQuatTestComponent>();
	ASSERT_TRUE(quat.IsValid());
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1000};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_FALSE(AnyTextContains(context, root, "no drawer"));
	UI::ElementHandle row = FindElementByTextContaining(context, root, "tilt");
	ASSERT_TRUE(context.IsAlive(row));
	Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
	while (context.IsAlive(row) && fields.size() < 3) {
		row = context.MakeHandle(context.GetElement(row)->parent);
		fields.clear();
		CollectByClass(context, row, StringHash("slt-textfield"), fields);
	}
	ASSERT_EQ(fields.size(), 3u);
	EXPECT_EQ(context.GetTextString(fields[0]), "-88");
	EXPECT_EQ(context.GetTextString(fields[1]), "30");
	EXPECT_EQ(context.GetTextString(fields[2]), "0");

	PressAndRelease(context, fields[1]);
	ASSERT_EQ(context.GetFocused(), fields[1]);
	context.InjectInput(InspectorKeyPress(Key::A, KeyMod::Ctrl));
	for (char digit : std::string_view("45")) {
		context.InjectInput(InspectorTextChar(static_cast<char32_t>(digit)));
	}
	context.Update();
	PressAway(context, root);
	EXPECT_TRUE(quat.Get()->tilt == Quat::FromEuler(Vec3{-88.0f, 45.0f, 0.0f}));

	UndoSystem::Undo();
	EXPECT_TRUE(quat.Get()->tilt == Quat::FromEuler(Vec3{-88.0f, 30.0f, 0.0f}));
}

namespace WideIntegerRow {

	constexpr u64 TwoToThe53 = 9007199254740992ull;
	constexpr u64 TwoToThe53Plus1 = 9007199254740993ull;

	struct Holder {
		u64 seed = 0;
		i64 offset = 0;
		u32 narrow = 0;

		SLT_REFLECT_VALUE(Holder)
		SLT_REFLECT_FIELD(seed)
		SLT_REFLECT_FIELD(offset)
		SLT_REFLECT_FIELD(narrow)
		SLT_END_REFLECT()
	};

	struct Rig {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle row;
		Holder value;

		Rig() : context(Desc(), &fonts) {
			TypeRegistry::Register(const_cast<TypeInfo*>(Holder::GetReflectType()));
			row = context.CreateElement();
			context.AppendChild(context.GetRoot(), row);
		}

		static UI::ContextDesc Desc() {
			UI::ContextDesc desc;
			desc.sizePx = {600, 400};
			return desc;
		}

		PropertyRows::ScalarRow BuildFor(const char* fieldName) {
			const TypeInfo* type = Holder::GetReflectType();
			const FieldInfo* info = FindField(type, StringHash(fieldName));
			EXPECT_NE(info, nullptr) << fieldName;
			PropertyRows::ValueAccess access;
			access.address = [this, info]() -> void* { return info->addrFn(&value); };
			access.commit = [this, info](const PropertyRows::CommittedValue& committed) {
				if (committed.kind == PropertyRows::CommittedValue::Kind::RawBits && info->size <= 8) {
					std::memcpy(info->addrFn(&value), &committed.rawBits, info->size);
				} else if (committed.kind == PropertyRows::CommittedValue::Kind::Number) {
					PropertyRows::WriteNumber(info->fieldType, info->addrFn(&value), committed.number);
				}
			};
			return PropertyRows::Build(context, row, FieldRef{type->name, StringHash(fieldName)}, std::move(access));
		}

		void TypeInto(UI::ElementHandle field, const char* text) {
			context.Update();
			context.ClearFocus();
			context.SetFocus(field);
			for (const char* c = text; *c; ++c) {
				InputEvent typed{};
				typed.type = InputEventType::Text;
				typed.codepoint = static_cast<char32_t>(*c);
				context.InjectInput(typed);
			}
			InputEvent commit{};
			commit.type = InputEventType::KeyDown;
			commit.key = Key::Enter;
			context.InjectInput(commit);
			context.Update();
			context.ClearFocus();
			context.Update();
		}
	};

} // namespace WideIntegerRow

TEST(InspectorWideIntegerTest, U64RoundTripsPastMantissa) {
	WideIntegerRow::Rig rig;
	rig.value.seed = WideIntegerRow::TwoToThe53Plus1;

	PropertyRows::ScalarRow row = rig.BuildFor("seed");
	ASSERT_EQ(row.kind, PropertyRows::ScalarRow::Kind::IntegerField);

	EXPECT_EQ(rig.context.GetTextString(row.element), std::to_string(WideIntegerRow::TwoToThe53Plus1));

	rig.TypeInto(row.element, "18446744073709551615");
	EXPECT_EQ(rig.value.seed, 18446744073709551615ull);

	rig.value.seed = WideIntegerRow::TwoToThe53;
	PropertyRows::Refresh(rig.context, row);
	EXPECT_EQ(rig.context.GetTextString(row.element), std::to_string(WideIntegerRow::TwoToThe53));
}

TEST(InspectorWideIntegerTest, I64KeepsSignAndRange) {
	WideIntegerRow::Rig rig;
	PropertyRows::ScalarRow row = rig.BuildFor("offset");
	ASSERT_EQ(row.kind, PropertyRows::ScalarRow::Kind::IntegerField);

	rig.TypeInto(row.element, "-9223372036854775808");
	EXPECT_EQ(rig.value.offset, std::numeric_limits<i64>::min());

	rig.TypeInto(row.element, "9223372036854775807");
	EXPECT_EQ(rig.value.offset, std::numeric_limits<i64>::max());
}

TEST(InspectorWideIntegerTest, OutOfRangeRefused) {
	WideIntegerRow::Rig rig;
	rig.value.seed = 42;
	PropertyRows::ScalarRow row = rig.BuildFor("seed");
	ASSERT_EQ(row.kind, PropertyRows::ScalarRow::Kind::IntegerField);

	rig.TypeInto(row.element, "9999999999999999999999999");
	EXPECT_EQ(rig.value.seed, 42u);

	PropertyRows::Refresh(rig.context, row);
	EXPECT_EQ(rig.context.GetTextString(row.element), "42");
}

TEST(InspectorWideIntegerTest, NarrowerIntegerKeepsNumberFieldAndScrub) {
	WideIntegerRow::Rig rig;
	PropertyRows::ScalarRow row = rig.BuildFor("narrow");
	EXPECT_EQ(row.kind, PropertyRows::ScalarRow::Kind::NumberField);
}

TEST_F(InspectorTest, EmptySelectionPlaceholder) {
	EditorSelection::Clear();
	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle title = FindElementByText(context, root, "Nothing selected");
	ASSERT_TRUE(context.IsAlive(title));
	UI::ElementHandle host = context.MakeHandle(context.GetElement(title)->parent);
	UI::ElementHandle scroll = UITest::FindByClass(context, root, StringHash("slt-scroll-root"), 0);
	ASSERT_TRUE(context.IsAlive(scroll));
	EXPECT_EQ(context.GetComputedStyle(scroll).display, UI::Display::None);

	const UI::Rect hostBox = context.GetLayout(host).borderBox;
	const UI::Rect titleBox = context.GetLayout(title).borderBox;
	EXPECT_GT(hostBox.h, desc.sizePx.y * 0.5f);
	EXPECT_NEAR(titleBox.x + titleBox.w * 0.5f, hostBox.x + hostBox.w * 0.5f, 1.0f);
	const UI::Element* hostPod = context.GetElement(host);
	const UI::Rect first = context.GetLayout(context.MakeHandle(hostPod->firstChild)).borderBox;
	const UI::Rect last = context.GetLayout(context.MakeHandle(hostPod->lastChild)).borderBox;
	EXPECT_NEAR(first.y - hostBox.y, (hostBox.y + hostBox.h) - (last.y + last.h), 1.0f);

	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(context.GetComputedStyle(scroll).display, UI::Display::Flex);
	EXPECT_EQ(context.GetComputedStyle(host).display, UI::Display::None);
	EXPECT_GT(context.GetLayout(scroll).borderBox.h, desc.sizePx.y * 0.5f);

	EditorSelection::Clear();
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(context.GetComputedStyle(host).display, UI::Display::Flex);
	EXPECT_EQ(context.GetComputedStyle(scroll).display, UI::Display::None);
}

namespace {

	class InspResetDerived : public InspTestComponent {
	public:
		SLT_REFLECT_COMPONENT(InspResetDerived, InspTestComponent)
		SLT_END_REFLECT()

		InspResetDerived() { speed = 5.0f; }
	};

	void CollectKebabs(UI::Context& context, UI::ElementHandle from, Vector<UI::ElementHandle>& out) {
		const Handle<VectorAsset> morePicture = EditorIcons::GetGlyph(EditorIcons::Glyph::More);
		Vector<UI::ElementHandle> buttons{GetMemoryResource(MemTag::Editor)};
		std::function<void(UI::ElementHandle)> walk = [&](UI::ElementHandle at) {
			if (context.HasClass(at, StringHash("slt-button")) && UITest::FindByIcon(context, at, morePicture).IsValid()) {
				out.push_back(at);
			}
			const UI::Element* element = context.GetElement(at);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				walk(handle);
				child = context.GetElement(handle)->nextSibling;
			}
		};
		walk(from);
	}

} // anonymous namespace

TEST_F(InspectorTest, ResetRestoresFreshValue) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	ASSERT_TRUE(component.IsValid());
	component->count = 7;
	component->title = "changed";
	component->targetRef = object;
	EditTarget target{Handle<Object>(component.Get())};

	EXPECT_TRUE(EditorSceneOps::DiffersFromDefault(target, "count"_hash));
	EXPECT_FALSE(EditorSceneOps::DiffersFromDefault(target, "speed"_hash));
	const u32 depth = UndoSystem::GetUndoDepth();
	EXPECT_FALSE(EditorSceneOps::ResetToDefault(target, "speed"_hash, {}, "Reset speed"));
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth);

	ASSERT_TRUE(EditorSceneOps::ResetToDefault(target, "count"_hash, {}, "Reset count"));
	EXPECT_EQ(component->count, 2);
	EXPECT_EQ(component->title, "changed");
	EXPECT_EQ(UndoSystem::GetUndoLabel(), "Reset count");
	EXPECT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(component->count, 7);
	EXPECT_TRUE(UndoSystem::Redo());
	EXPECT_EQ(component->count, 2);

	ASSERT_TRUE(EditorSceneOps::ResetToDefault(target, "targetRef"_hash, {}, "Reset targetRef"));
	EXPECT_FALSE(component->targetRef.IsValid());
	EXPECT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(component->targetRef, object);

	Handle<InspResetDerived> derived = object.Get()->AddComponent<InspResetDerived>();
	ASSERT_TRUE(derived.IsValid());
	derived->speed = 1.0f;
	EditTarget derivedTarget{Handle<Object>(derived.Get())};
	EXPECT_TRUE(EditorSceneOps::DiffersFromDefault(derivedTarget, "speed"_hash));
	ASSERT_TRUE(EditorSceneOps::ResetToDefault(derivedTarget, "speed"_hash, {}, "Reset speed"));
	EXPECT_FLOAT_EQ(derived->speed, 5.0f);
}

TEST_F(InspectorTest, ComponentResetIsOneStep) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	ASSERT_TRUE(component.IsValid());
	component->speed = 4.0f;
	component->count = 7;
	component->title = "changed";
	component->offset = Vec3{1.0f, 2.0f, 3.0f};
	component->flag = true;
	component->targetRef = object;
	component->secret = 9;
	component->fixed = 5.0f;
	component->SetEnabled(false);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle kebab = CardKebab(context, root);
	ASSERT_TRUE(context.IsAlive(kebab));
	ClickInspectorElement(context, kebab);
	context.Update();
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);

	EXPECT_FLOAT_EQ(component->speed, 1.0f);
	EXPECT_EQ(component->count, 2);
	EXPECT_EQ(component->title, "hello");
	EXPECT_EQ(component->offset, Vec3{});
	EXPECT_FALSE(component->flag);
	EXPECT_FALSE(component->targetRef.IsValid());
	EXPECT_EQ(component->secret, 0);
	EXPECT_FLOAT_EQ(component->fixed, 3.0f);
	EXPECT_FALSE(component->IsEnabledSelf());
	EXPECT_EQ(UndoSystem::GetUndoLabel(), "Reset InspTestComponent");

	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_FLOAT_EQ(component->speed, 4.0f);
	EXPECT_EQ(component->count, 7);
	EXPECT_EQ(component->title, "changed");
	EXPECT_EQ(component->targetRef, object);
	EXPECT_EQ(component->secret, 9);
	EXPECT_FLOAT_EQ(component->fixed, 5.0f);
	EXPECT_FALSE(UndoSystem::Undo());
}

TEST_F(InspectorTest, RightClickResetRestoresRow) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<InspTestComponent> component = object.Get()->AddComponent<InspTestComponent>();
	ASSERT_TRUE(component.IsValid());
	component->count = 7;
	component->title = "changed";
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle countLabel = FindElementByTextContaining(context, root, "count");
	ASSERT_TRUE(context.IsAlive(countLabel));
	RightClickElement(context, countLabel);
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);
	context.Update();
	EXPECT_FALSE(context.IsAlive(MenuEntry(context, "Reset")));
	EXPECT_EQ(component->count, 2);
	EXPECT_EQ(component->title, "changed");
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(component->count, 7);

	const u32 depth = UndoSystem::GetUndoDepth();
	UI::ElementHandle speedLabel = FindElementByTextContaining(context, root, "speed");
	ASSERT_TRUE(context.IsAlive(speedLabel));
	RightClickElement(context, speedLabel);
	UI::ElementHandle inert = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(inert));
	ClickInspectorElement(context, inert);
	context.Update();
	EXPECT_TRUE(context.IsAlive(MenuEntry(context, "Reset")));
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth);
	EXPECT_FLOAT_EQ(component->speed, 1.0f);
}

TEST_F(InspectorTest, ResettingTransformMovesChildrenWithIt) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Parent", _scene);
	object->SetLocalPosition(Vec3{5.0f, 0.0f, 0.0f});
	object->SetLocalRotation(Quat::FromEuler(Vec3{0.0f, 90.0f, 0.0f}));
	object->SetLocalScale(Vec3{2.0f, 2.0f, 2.0f});
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene, object);
	child->SetLocalPosition(Vec3{1.0f, 0.0f, 0.0f});
	const Vec3 placed = child->GetWorldPosition();
	ASSERT_GT(std::abs(placed.x - 1.0f) + std::abs(placed.z), 0.5f);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	Vector<UI::ElementHandle> kebabs{GetMemoryResource(MemTag::Editor)};
	CollectKebabs(context, root, kebabs);
	ASSERT_EQ(kebabs.size(), 1u);
	ClickInspectorElement(context, kebabs[0]);
	context.Update();
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);

	EXPECT_EQ(object->GetLocalPosition(), Vec3{});
	EXPECT_TRUE(object->GetLocalRotation() == Quat::Identity());
	EXPECT_EQ(object->GetLocalScale(), (Vec3{1.0f, 1.0f, 1.0f}));
	const Vec3 after = child->GetWorldPosition();
	EXPECT_NEAR(after.x, 1.0f, 1e-4f);
	EXPECT_NEAR(after.z, 0.0f, 1e-4f);
	EXPECT_EQ(UndoSystem::GetUndoLabel(), "Reset Transform");

	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(object->GetLocalPosition(), (Vec3{5.0f, 0.0f, 0.0f}));
	EXPECT_EQ(object->GetLocalScale(), (Vec3{2.0f, 2.0f, 2.0f}));
	EXPECT_NEAR(child->GetWorldPosition().x, placed.x, 1e-4f);
	EXPECT_FALSE(UndoSystem::Undo());

	context.Update();
	UI::ElementHandle scaleLabel = FindElementByTextContaining(context, root, "Scale");
	ASSERT_TRUE(context.IsAlive(scaleLabel));
	RightClickElement(context, scaleLabel);
	UI::ElementHandle rowReset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(rowReset));
	ClickInspectorElement(context, rowReset);
	EXPECT_EQ(object->GetLocalScale(), (Vec3{1.0f, 1.0f, 1.0f}));
	EXPECT_EQ(object->GetLocalPosition(), (Vec3{5.0f, 0.0f, 0.0f}));
}

TEST_F(InspectorTest, DuplicateShowsRotationOriginalWasGiven) {
	struct Case {
		Vec3 angles;
		const char* shown[3];
	};
	const Case cases[] = {{Vec3{90.0f, 0.0f, 0.0f}, {"90", "0", "0"}}, {Vec3{-88.0f, 0.0f, 0.0f}, {"-88", "0", "0"}}};
	for (const Case& entry : cases) {
		Handle<SceneObject> original = SceneSystem::CreateObject("Turned", _scene);
		original->SetLocalRotation(Quat::FromEuler(entry.angles));
		EditorSelection::Select(Handle<Object>(original.Get()));
		ASSERT_EQ(EditorSceneOps::DuplicateSelectedObjects(), 1u);
		Object* active = EditorSelection::GetActive().Get();
		ASSERT_TRUE(active != nullptr && active != original.Get());
		Handle<SceneObject> copy(static_cast<SceneObject*>(active));

		Handle<EditorPanel> panel = OpenInspector();
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {500, 900};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		panel.Get()->BuildContent(context, root);
		context.Update();

		UI::ElementHandle row = FindElementByText(context, root, "Rotation");
		ASSERT_TRUE(context.IsAlive(row));
		Vector<UI::ElementHandle> fields{GetMemoryResource(MemTag::Editor)};
		while (context.IsAlive(row) && fields.size() < 3) {
			row = context.MakeHandle(context.GetElement(row)->parent);
			fields.clear();
			CollectByClass(context, row, StringHash("slt-textfield"), fields);
		}
		ASSERT_EQ(fields.size(), 3u);
		for (u32 i = 0; i < 3; ++i) {
			EXPECT_EQ(context.GetTextString(fields[i]), entry.shown[i]) << "axis " << i << " of " << entry.angles.ToString();
		}
		PanelManager::ClosePanel(panel);
		SceneSystem::Destroy(copy);
		SceneSystem::Destroy(original);
		SceneSystem::EndFrame();
	}
}

TEST(InspectorEulerTest, RotationShowsAnglesThatBuiltIt) {
	Vector<Vec3> typed{GetMemoryResource(MemTag::Editor)};
	for (f32 angle = -89.5f; angle <= 89.5f; angle += 0.5f) {
		typed.push_back(Vec3{angle, 0.0f, 0.0f});
	}
	for (f32 angle = -179.5f; angle <= 179.5f; angle += 0.5f) {
		typed.push_back(Vec3{0.0f, angle, 0.0f});
		typed.push_back(Vec3{0.0f, 0.0f, angle});
	}
	u32 state = 12345u;
	auto next = [&state](i32 range) {
		state = state * 1664525u + 1013904223u;
		return static_cast<i32>((state >> 8) % static_cast<u32>(2 * range + 1)) - range;
	};
	for (u32 i = 0; i < 2000; ++i) {
		typed.push_back(Vec3{static_cast<f32>(next(8000)) / 100.0f, static_cast<f32>(next(17900)) / 100.0f,
							 static_cast<f32>(next(17900)) / 100.0f});
	}
	u32 missed = 0;
	str first;
	for (const Vec3& angles : typed) {
		Vec3 shown = EditorSceneOps::EulerForDisplay(Quat::FromEuler(angles));
		if (!(shown == angles)) {
			if (missed++ < 4) {
				first += angles.ToString() + " shown as " + shown.ToString() + "; ";
			}
		}
	}
	EXPECT_EQ(missed, 0u) << "of " << typed.size() << ", first " << first;
}

TEST_F(InspectorTest, CustomInspectorsRowResetsLikeWalkedOne) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", _scene);
	Handle<MeshRenderer> rendererHandle = object.Get()->AddComponent<MeshRenderer>();
	MeshRenderer* renderer = rendererHandle.Get();
	ASSERT_NE(renderer, nullptr);
	MaterialAsset material;
	renderer->SetMaterial(0, Handle<MaterialAsset>(&material));
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle slotLabel = FindElementByTextContaining(context, root, "Material 0");
	ASSERT_TRUE(context.IsAlive(slotLabel));
	RightClickElement(context, slotLabel);
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);
	EXPECT_FALSE(renderer->GetMaterial(0).IsValid());
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(renderer->GetMaterial(0).GetInstanceId(), material.GetInstanceId());
}

TEST_F(NestedAssetInspectorTest, NestedResetRestoresFresh) {
	Guid guid = CreateNested();
	auto* nested = static_cast<InspNestedAsset*>(AssetDatabase::LoadAssetByGuid(guid));
	ASSERT_NE(nested, nullptr);
	EditTarget target{Handle<Object>(nested)};
	nested->entries[1].name = "gamma";
	nested->entries.resize(4);
	nested->entries[3].volume = 0.25f;
	nested->settings.depth = 12;

	using Step = EditorSceneOps::ValueStep;
	const Step secondName[] = {{1, {}}, {-1, "name"_hash}};
	const Step fourthVolume[] = {{3, {}}, {-1, "volume"_hash}};
	const Step depth[] = {{-1, "depth"_hash}};
	const Step fifth[] = {{4, {}}};

	ASSERT_TRUE(EditorSceneOps::ResetToDefault(target, "entries"_hash, secondName, "Reset name"));
	EXPECT_EQ(nested->entries[1].name, "beta");
	EXPECT_EQ(nested->entries[0].name, "alpha");
	ASSERT_TRUE(EditorSceneOps::ResetToDefault(target, "entries"_hash, fourthVolume, "Reset volume"));
	EXPECT_FLOAT_EQ(nested->entries[3].volume, 1.0f);
	EXPECT_EQ(nested->entries.size(), 4u);
	ASSERT_TRUE(EditorSceneOps::ResetToDefault(target, "settings"_hash, depth, "Reset depth"));
	EXPECT_EQ(nested->settings.depth, 37);
	EXPECT_FALSE(EditorSceneOps::DiffersFromDefault(target, "entries"_hash, fifth));

	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(nested->settings.depth, 12);
	EXPECT_FLOAT_EQ(nested->entries[3].volume, 1.0f);
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_FLOAT_EQ(nested->entries[3].volume, 0.25f);
	EXPECT_EQ(nested->entries[1].name, "beta");
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(nested->entries[1].name, "gamma");
	AssetDatabase::UnloadAsset(nested);
}

TEST_F(NestedAssetInspectorTest, NestedRowsRightClickResetsValue) {
	Guid guid = CreateNested();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 1600};
	UI::Context context(desc, &fonts);
	Drawn drawn = Draw(guid, context);

	Retype(context, drawn.root, "37", "12");
	ASSERT_EQ(Live(guid).depth, 12);
	Settle(context);
	UI::ElementHandle depthLabel = FindElementByText(context, drawn.root, "depth");
	ASSERT_TRUE(context.IsAlive(depthLabel));
	RightClickElement(context, depthLabel);
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);
	EXPECT_EQ(Live(guid).depth, 37);
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(Live(guid).depth, 12);
}

TEST_F(PrefabInspectorTest, ResettingInPrefabAssetsBodyWritesPrefab) {
	TypeRegistry::Register(const_cast<TypeInfo*>(InspTestComponent::GetReflectType()));
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	unit->SetLocalScale(Vec3{3.0f, 3.0f, 3.0f});
	unit->AddComponent<InspTestComponent>()->count = 7;
	Guid guid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(guid.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	EditorSelection::SelectAsset(guid);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle kebab = CardKebab(context, root);
	ASSERT_TRUE(context.IsAlive(kebab));
	ClickInspectorElement(context, kebab);
	context.Update();
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);
	PanelManager::TickAll(0.016f);
	context.Update();

	Vector<UI::ElementHandle> kebabs{GetMemoryResource(MemTag::Editor)};
	CollectKebabs(context, root, kebabs);
	UI::ElementHandle cardKebab = CardKebab(context, root);
	UI::ElementHandle transformKebab{};
	for (UI::ElementHandle candidate : kebabs) {
		if (!(candidate == cardKebab)) {
			transformKebab = candidate;
			break;
		}
	}
	ASSERT_TRUE(context.IsAlive(transformKebab));
	ClickInspectorElement(context, transformKebab);
	context.Update();
	UI::ElementHandle transformReset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(transformReset));
	ClickInspectorElement(context, transformReset);
	PanelManager::TickAll(0.016f);

	Handle<SceneObject> fresh = SceneSystem::Instantiate(guid, _scene);
	ASSERT_TRUE(fresh.IsValid());
	Handle<InspTestComponent> composed = fresh->GetComponent<InspTestComponent>();
	ASSERT_TRUE(composed.IsValid());
	EXPECT_EQ(composed->count, 2);
	EXPECT_EQ(fresh->GetLocalScale(), (Vec3{1.0f, 1.0f, 1.0f}));
}

TEST_F(PrefabInspectorTest, VariantRevertWritesVariant) {
	TypeRegistry::Register(const_cast<TypeInfo*>(InspTestComponent::GetReflectType()));
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	unit->AddComponent<InspTestComponent>();
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(base.IsValid());
	unit->GetComponent<InspTestComponent>()->count = 7;
	Guid variant = EditorSceneOps::CreatePrefabVariantFrom(unit, Path("project://Assets/Unit Variant.prefab"));
	ASSERT_TRUE(variant.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();
	EditorSelection::SelectAsset(variant);

	Handle<EditorPanel> panel = OpenInspector();
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle kebab = CardKebab(context, root);
	ASSERT_TRUE(context.IsAlive(kebab));
	ClickInspectorElement(context, kebab);
	context.Update();
	UI::ElementHandle revert = MenuEntry(context, "Revert Modified Component");
	ASSERT_TRUE(context.IsAlive(revert));
	ClickInspectorElement(context, revert);
	PanelManager::TickAll(0.016f);

	Handle<SceneObject> fresh = SceneSystem::Instantiate(variant, _scene);
	ASSERT_TRUE(fresh.IsValid());
	Handle<InspTestComponent> composed = fresh->GetComponent<InspTestComponent>();
	ASSERT_TRUE(composed.IsValid());
	EXPECT_EQ(composed->count, 2);
}

namespace {

	class InspectorDefaultSkyFixture : public InspectorTest {
	protected:
		std::filesystem::path _projectRoot;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_inspsky_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot);
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			const std::filesystem::path engineAssets = InspectorRepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			AssetDatabase::Initialize();
			InspectorTest::SetUp();
		}

		void TearDown() override {
			InspectorTest::TearDown();
			AssetDatabase::Terminate();
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		static MaterialAsset* DefaultSky() {
			return static_cast<MaterialAsset*>(AssetDatabase::LoadAsset(Path("engine://Materials/DefaultSky.mat")));
		}
	};

} // anonymous namespace

TEST_F(InspectorDefaultSkyFixture, AddingEnvironmentFromInspectorDrawsDefaultSky) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle addLabel = FindElementByText(context, root, "Add Component");
	ASSERT_TRUE(context.IsAlive(addLabel));
	PressAndRelease(context, context.MakeHandle(context.GetElement(addLabel)->parent));
	context.Update();
	UI::ElementHandle group = FindElementByTextContaining(context, context.GetRoot(), "Rendering");
	ASSERT_TRUE(context.IsAlive(group));
	PressAndRelease(context, context.MakeHandle(context.GetElement(group)->parent));
	context.Update();
	UI::ElementHandle row = FindElementByText(context, context.GetRoot(), "Environment");
	ASSERT_TRUE(context.IsAlive(row));
	PressAndRelease(context, context.MakeHandle(context.GetElement(row)->parent));

	Handle<Environment> environment = object->GetComponent<Environment>();
	ASSERT_TRUE(environment.IsValid());
	MaterialAsset* expected = DefaultSky();
	ASSERT_NE(expected, nullptr);
	EXPECT_EQ(environment->GetSkyMode(), SkyMode::Material);
	EXPECT_EQ(environment->GetSkyMaterial().Get(), expected);
}

TEST_F(InspectorDefaultSkyFixture, EnvironmentResetRestoresSky) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", _scene);
	Handle<Environment> environment = object->AddComponent<Environment>();
	ASSERT_TRUE(environment.IsValid());
	ASSERT_EQ(environment->GetSkyMode(), SkyMode::Gradient);
	EditorSelection::Select(Handle<Object>(object.Get()));
	Handle<EditorPanel> panel = OpenInspector();
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle kebab = CardKebab(context, root);
	ASSERT_TRUE(context.IsAlive(kebab));
	ClickInspectorElement(context, kebab);
	context.Update();
	UI::ElementHandle reset = MenuEntry(context, "Reset");
	ASSERT_TRUE(context.IsAlive(reset));
	ClickInspectorElement(context, reset);

	EXPECT_EQ(environment->GetSkyMode(), SkyMode::Material);
	EXPECT_EQ(environment->GetSkyMaterial().Get(), DefaultSky());
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_EQ(environment->GetSkyMode(), SkyMode::Gradient);
	EXPECT_FALSE(environment->GetSkyMaterial().IsValid());
}
