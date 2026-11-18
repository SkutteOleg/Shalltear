#include "TestsPCH.h"

#include "Checkout.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Core/UI/VectorAsset.h"
#include "Shalltear/Editor/EditorIcons.h"
#include <array>

#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PlayModeController.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"
#include "Shalltear/Core/UI/UISystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Shalltear;
using namespace UITest;

namespace {

	class HierTestComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(HierTestComponent, Component)
		SLT_REFLECT_FIELD(payload)
		SLT_END_REFLECT()

		HierTestComponent() = default;

		i32 payload = 0;
	};

	class HierBadgeComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(HierBadgeComponent, Component)
		SLT_END_REFLECT()

		HierBadgeComponent() = default;
	};

	SceneObjectDragData DraggedObjects(std::initializer_list<Handle<SceneObject>> objects) {
		SceneObjectDragData data;
		for (Handle<SceneObject> object : objects) {
			data.objects.push_back(object.GetInstanceId());
		}
		return data;
	}

	class HierarchyPanelTest : public ::testing::Test {
	protected:
		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			HierTestComponent::GetReflectType();
			RegisterBuiltInPanels();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			SceneAnnotations::Initialize();
			PanelManager::Initialize();
			_scene = SceneSystem::CreateScene("Fixture");
		}
		void TearDown() override {
			PanelManager::Terminate();
			SceneAnnotations::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			SceneSystem::Terminate();
		}

		static bool IsSceneRow(UI::Context& context, UI::ElementHandle handle) {
			return context.HasClass(handle, EditorTheme::Class::SectionHeader);
		}

		static UI::ElementHandle NthTreeRow(UI::Context& context, UI::ElementHandle from, u32& index,
											bool wantSceneRows = false) {
			if (context.HasClass(from, StringHash("slt-tree-row")) &&
				IsSceneRow(context, from) == wantSceneRows && index-- == 0) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = NthTreeRow(context, handle, index, wantSceneRows);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
		static UI::ElementHandle RowAt(UI::Context& context, UI::ElementHandle from, u32 index) {
			u32 counter = index;
			return NthTreeRow(context, from, counter, false);
		}
		static UI::ElementHandle SceneRowAt(UI::Context& context, UI::ElementHandle from, u32 index) {
			u32 counter = index;
			return NthTreeRow(context, from, counter, true);
		}

		static InputEvent Mouse(InputEventType type, Vec2 point) {
			InputEvent event{};
			event.type = type;
			event.button = MouseButton::Left;
			event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
			return event;
		}
		static InputEvent RightMouse(InputEventType type, Vec2 point) {
			InputEvent event = Mouse(type, point);
			event.button = MouseButton::Right;
			return event;
		}
		static void RightClickAndTick(UI::Context& context, EditorPanel& panel, Vec2 point) {
			context.InjectInput(Mouse(InputEventType::MouseMove, point));
			context.InjectInput(RightMouse(InputEventType::MouseDown, point));
			context.InjectInput(RightMouse(InputEventType::MouseUp, point));
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}
		static constexpr Vec2 AwayFromRows{300.0f, 760.0f};
		static void Hover(UI::Context& context, Vec2 point) {
			context.InjectInput(Mouse(InputEventType::MouseMove, point));
			context.Update();
		}
		static bool SubtreeSaysAnywhere(UI::Context& context, UI::ElementHandle from, std::string_view text) {
			if (context.GetTextString(from) == text) {
				return true;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (SubtreeSaysAnywhere(context, handle, text)) {
					return true;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return false;
		}
		static UI::ElementHandle MenuRowNamed(UI::Context& context, UI::ElementHandle popup, std::string_view label) {
			const UI::Element* element = context.GetElement(popup);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (context.HasClass(handle, StringHash("slt-menu-item")) &&
					SubtreeSaysAnywhere(context, handle, label)) {
					return handle;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
		static UI::ElementHandle RowNamed(UI::Context& context, UI::ElementHandle from, std::string_view label) {
			if (context.HasClass(from, StringHash("slt-tree-row")) && SubtreeSaysAnywhere(context, from, label)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = RowNamed(context, handle, label);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
		static void StripBeforeImpl(UI::Context& context, UI::ElementHandle from, std::string_view label,
									UI::ElementHandle& lastStrip, UI::ElementHandle& found, bool& done) {
			if (done) {
				return;
			}
			if (context.HasClass(from, EditorTheme::Class::InsertStrip)) {
				lastStrip = from;
			} else if (context.HasClass(from, StringHash("slt-tree-row")) &&
					   SubtreeSaysAnywhere(context, from, label)) {
				found = lastStrip;
				done = true;
				return;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				StripBeforeImpl(context, handle, label, lastStrip, found, done);
				child = context.GetElement(handle)->nextSibling;
			}
		}
		static UI::ElementHandle StripBefore(UI::Context& context, UI::ElementHandle from, std::string_view label) {
			UI::ElementHandle lastStrip;
			UI::ElementHandle found;
			bool done = false;
			StripBeforeImpl(context, from, label, lastStrip, found, done);
			return found;
		}
		static void StripsBetweenImpl(UI::Context& context, UI::ElementHandle from, std::string_view after,
									  std::string_view before, Vector<UI::ElementHandle>& out, bool& started,
									  bool& done) {
			if (done) {
				return;
			}
			if (context.HasClass(from, EditorTheme::Class::InsertStrip)) {
				if (started) {
					out.push_back(from);
				}
			} else if (context.HasClass(from, StringHash("slt-tree-row"))) {
				if (started && SubtreeSaysAnywhere(context, from, before)) {
					done = true;
					return;
				}
				if (SubtreeSaysAnywhere(context, from, after)) {
					started = true;
				}
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				StripsBetweenImpl(context, handle, after, before, out, started, done);
				child = context.GetElement(handle)->nextSibling;
			}
		}
		static void CollectStripsBetween(UI::Context& context, UI::ElementHandle from, std::string_view after,
										 std::string_view before, Vector<UI::ElementHandle>& out) {
			bool started = false;
			bool done = false;
			StripsBetweenImpl(context, from, after, before, out, started, done);
		}

		static UI::ElementHandle NthByClassImpl(UI::Context& context, UI::ElementHandle from, StringHash klass,
												u32& index) {
			if (context.HasClass(from, klass) && index-- == 0) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = NthByClassImpl(context, handle, klass, index);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
		static UI::ElementHandle NthByClass(UI::Context& context, UI::ElementHandle from, StringHash klass, u32 n) {
			u32 counter = n;
			return NthByClassImpl(context, from, klass, counter);
		}
		static UI::ElementHandle NthChild(UI::Context& context, UI::ElementHandle parent, u32 index) {
			const UI::Element* element = context.GetElement(parent);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (index-- == 0) {
					return handle;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
		static void ClickAndTick(UI::Context& context, EditorPanel& panel, Vec2 point) {
			context.InjectInput(Mouse(InputEventType::MouseMove, point));
			context.InjectInput(Mouse(InputEventType::MouseDown, point));
			context.InjectInput(Mouse(InputEventType::MouseUp, point));
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}

		static void ModClickAndTick(UI::Context& context, EditorPanel& panel, Vec2 point, KeyMod mods) {
			InputEvent event = Mouse(InputEventType::MouseMove, point);
			event.mods = mods;
			context.InjectInput(event);
			event.type = InputEventType::MouseDown;
			context.InjectInput(event);
			event.type = InputEventType::MouseUp;
			context.InjectInput(event);
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}

		static Vec2 CenterOf(UI::Context& context, UI::ElementHandle handle) {
			const UI::Rect& box = context.GetLayout(handle).borderBox;
			return {box.x + box.w * 0.5f, box.y + box.h * 0.5f};
		}

		static UI::ElementHandle FirstByClass(UI::Context& context, UI::ElementHandle from, StringHash klass) {
			if (context.HasClass(from, klass)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = FirstByClass(context, handle, klass);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static UI::ElementHandle NextSibling(UI::Context& context, UI::ElementHandle element) {
			const UI::Element* pod = context.IsAlive(element) ? context.GetElement(element) : nullptr;
			if (!pod || pod->nextSibling == UI::INVALID_UI_INDEX) {
				return {};
			}
			return context.MakeHandle(pod->nextSibling);
		}
		static UI::ElementHandle RowGlyph(UI::Context& context, UI::ElementHandle row) {
			return NextSibling(context, FirstByClass(context, row, StringHash("slt-tree-arrow")));
		}
		static UI::ElementHandle ActiveToggle(UI::Context& context, UI::ElementHandle row) {
			const UI::Element* element = context.IsAlive(row) ? context.GetElement(row) : nullptr;
			UI::ElementHandle last;
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				last = context.MakeHandle(child);
				child = context.GetElement(last)->nextSibling;
			}
			return last;
		}

		static u32 CountObjectRows(UI::Context& context, UI::ElementHandle from) {
			u32 count = context.HasClass(from, StringHash("slt-tree-row")) && !IsSceneRow(context, from) ? 1u : 0u;
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				count += CountObjectRows(context, handle);
				child = context.GetElement(handle)->nextSibling;
			}
			return count;
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

		Handle<Scene> _scene;
	};

} // anonymous namespace

TEST_F(HierarchyPanelTest, RowsMirrorTheLoadedScene) {
	SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	SceneSystem::CreateObject("Hidden Child", _scene, b);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EXPECT_EQ(CountObjectRows(context, root), 2u);

	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	EXPECT_EQ(CountObjectRows(context, root2), 2u);
}

TEST_F(HierarchyPanelTest, LongNameKeepsRowOneLineHigh) {
	SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("New Object (Copy) (Copy)", _scene);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {160, 400};
	UI::Context context(desc, &fonts);
	EditorTheme::ApplyToContext(context);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle shortRow = RowNamed(context, root, "A");
	UI::ElementHandle longRow = RowNamed(context, root, "New Object (Copy) (Copy)");
	ASSERT_TRUE(shortRow.IsValid());
	ASSERT_TRUE(longRow.IsValid());

	EXPECT_FLOAT_EQ(context.GetLayout(longRow).borderBox.h, context.GetLayout(shortRow).borderBox.h);
	const UI::Rect& box = context.GetLayout(longRow).borderBox;
	EXPECT_LE(box.x + box.w, 160.0f);
}

TEST_F(HierarchyPanelTest, ChildRowsIndentUnderTheirParent) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Child", _scene, parent);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle parentRow = RowAt(context, root, 0);
	ASSERT_TRUE(parentRow.IsValid());
	UI::ElementHandle caret = FirstByClass(context, parentRow, StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	Vec2 caretPoint = CenterOf(context, caret);
	context.InjectInput(Mouse(InputEventType::MouseDown, caretPoint));
	context.InjectInput(Mouse(InputEventType::MouseUp, caretPoint));
	context.Update();

	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	ASSERT_EQ(CountObjectRows(context, root2), 2u);

	UI::ElementHandle parentCaret = FirstByClass(context, RowAt(context, root2, 0), StringHash("slt-tree-arrow"));
	UI::ElementHandle childCaret = FirstByClass(context, RowAt(context, root2, 1), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(parentCaret.IsValid());
	ASSERT_TRUE(childCaret.IsValid());
	f32 step = context.GetStyleVarFloat("--slt-tree-indent"_hash, 0.0f);
	ASSERT_GT(step, 0.0f);
	EXPECT_FLOAT_EQ(context.GetLayout(childCaret).borderBox.x - context.GetLayout(parentCaret).borderBox.x, step);
}

TEST_F(HierarchyPanelTest, RebuildReflectsSceneMutations) {
	SceneSystem::CreateObject("Only", _scene);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	EXPECT_EQ(CountObjectRows(context, root), 1u);

	SceneSystem::CreateObject("Second", _scene);
	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	EXPECT_EQ(CountObjectRows(context, root2), 2u);
}

TEST_F(HierarchyPanelTest, DirtySceneShowsInTheHeader) {
	SceneSystem::CreateObject("Thing", _scene);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	u32 before = context.GetLiveCount();
	EditorSceneOps::MarkDirty(_scene);
	UI::ElementHandle root2 = context.CreateElement();
	context.AppendChild(context.GetRoot(), root2);
	panel.Get()->BuildContent(context, root2);
	context.Update();
	EXPECT_GT(context.GetLiveCount(), before);
}

TEST_F(HierarchyPanelTest, PressedRowStaysDraggable) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle rowA = RowAt(context, root, 0);
	ASSERT_TRUE(rowA.IsValid());
	Vec2 point = CenterOf(context, rowA);
	ASSERT_EQ(context.HitTest(point), rowA);
	ASSERT_FALSE(EditorSelection::IsSelected(Handle<Object>(a.Get())));

	int drags = 0;
	std::function<void(Vec2)> launch = context.Events(rowA).onPressDrag;
	ASSERT_TRUE(static_cast<bool>(launch));
	context.Events(rowA).onPressDrag = [&drags, launch](Vec2 at) {
		++drags;
		launch(at);
	};

	context.InjectInput(Mouse(InputEventType::MouseMove, point));
	context.InjectInput(Mouse(InputEventType::MouseDown, point));
	context.Update();
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(a.Get())));

	panel.Get()->OnTick(0.016f);
	context.Update();
	ASSERT_TRUE(context.IsAlive(rowA));

	context.InjectInput(Mouse(InputEventType::MouseMove, {point.x + 12.0f, point.y + 4.0f}));
	context.Update();
	EXPECT_EQ(drags, 1);

	context.InjectInput(Mouse(InputEventType::MouseMove, point));
	context.InjectInput(Mouse(InputEventType::MouseUp, point));
	context.Update();
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(a.Get())));
	panel.Get()->OnTick(0.016f);
	context.Update();
	ASSERT_TRUE(context.IsAlive(rowA));
	EXPECT_TRUE(context.HasClass(rowA, StringHash("slt-selected")));
	EXPECT_FALSE(context.HasClass(RowAt(context, root, 1), StringHash("slt-selected")));
}

TEST_F(HierarchyPanelTest, DraggingRowElsewhereLeavesSelectionAlone) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	EditorSelection::Select(Handle<Object>(a.Get()));
	panel.Get()->OnTick(0.016f);
	context.Update();

	UI::ElementHandle rowB = RowAt(context, root, 1);
	ASSERT_TRUE(rowB.IsValid());
	Vec2 from = CenterOf(context, rowB);
	Vec2 to = CenterOf(context, RowAt(context, root, 0));

	context.InjectInput(Mouse(InputEventType::MouseMove, from));
	context.InjectInput(Mouse(InputEventType::MouseDown, from));
	context.InjectInput(Mouse(InputEventType::MouseMove, to));
	context.InjectInput(Mouse(InputEventType::MouseUp, to));
	context.Update();

	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(a.Get())));
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(b.Get())));
}

TEST_F(HierarchyPanelTest, DoubleClickFramesNoRename) {
	Handle<SceneObject> object = SceneSystem::CreateObject("A", _scene);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	class FollowWatcher : public Object {
	public:
		u32 count = 0;
		u64 last = 0;
		void OnFollow(const EditorFollowRequestedEvent& event) {
			++count;
			last = event.object;
		}
	};
	FollowWatcher* watcher = New<FollowWatcher>(MemTag::Editor);
	EditorEvents::FollowRequested.Subscribe(EventBus::GetGlobal(), Handle<FollowWatcher>(watcher),
											&FollowWatcher::OnFollow);

	Vec2 point = CenterOf(context, RowAt(context, root, 0));
	for (int click = 0; click < 2; ++click) {
		context.InjectInput(Mouse(InputEventType::MouseMove, point));
		context.InjectInput(Mouse(InputEventType::MouseDown, point));
		context.InjectInput(Mouse(InputEventType::MouseUp, point));
		context.Update();
		panel.Get()->OnTick(0.016f);
		context.Update();
	}

	EXPECT_EQ(watcher->count, 1u);
	EXPECT_EQ(watcher->last, object.GetInstanceId());
	EXPECT_EQ(CountByClass(context, root, StringHash("slt-textfield")), 0u);
	EXPECT_EQ(CountObjectRows(context, root), 1u);
	Delete(watcher);
}

TEST_F(HierarchyPanelTest, ComponentDropMovesComponent) {
	Handle<SceneObject> source = SceneSystem::CreateObject("Source", _scene);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	Handle<HierTestComponent> component = source.Get()->AddComponent<HierTestComponent>();
	component.Get()->payload = 5;

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle targetRow = RowAt(context, root, 1);
	ASSERT_TRUE(targetRow.IsValid());

	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(Handle<Component>(component.Get()).GetInstanceId());
	ASSERT_TRUE(context.DeliverDrop(payload, CenterOf(context, targetRow)));
	SceneSystem::EndFrame();

	EXPECT_FALSE(source.Get()->GetComponent<HierTestComponent>().IsValid());
	Handle<HierTestComponent> moved = target.Get()->GetComponent<HierTestComponent>();
	ASSERT_TRUE(moved.IsValid());
	EXPECT_EQ(moved.Get()->payload, 5);
}

TEST_F(HierarchyPanelTest, RowDeclinesComponentItAlreadyOwns) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Owner", _scene);
	Handle<HierTestComponent> component = object.Get()->AddComponent<HierTestComponent>();

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(Handle<Component>(component.Get()).GetInstanceId());
	EXPECT_FALSE(context.DeliverDrop(payload, CenterOf(context, RowAt(context, root, 0))));
	EXPECT_EQ(object.Get()->GetComponentCount(), 1u);
}

TEST_F(HierarchyPanelTest, SceneFoldHidesObjects) {
	SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	ASSERT_EQ(CountObjectRows(context, root), 2u);

	auto sceneCaret = [&context, root]() {
		UI::ElementHandle sceneRow = SceneRowAt(context, root, 0);
		return sceneRow.IsValid() ? FirstByClass(context, sceneRow, StringHash("slt-tree-arrow"))
								  : UI::ElementHandle{};
	};
	ASSERT_TRUE(sceneCaret().IsValid());

	ClickAndTick(context, *panel.Get(), CenterOf(context, sceneCaret()));
	EXPECT_EQ(CountObjectRows(context, root), 0u);
	EXPECT_TRUE(SceneRowAt(context, root, 0).IsValid());

	ClickAndTick(context, *panel.Get(), CenterOf(context, sceneCaret()));
	EXPECT_EQ(CountObjectRows(context, root), 2u);
}

TEST_F(HierarchyPanelTest, SceneRowClickToggles) {
	SceneSystem::CreateObject("A", _scene);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle sceneRow = SceneRowAt(context, root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	Vec2 body = CenterOf(context, sceneRow);
	ASSERT_EQ(context.HitTest(body), sceneRow);

	ClickAndTick(context, *panel.Get(), body);
	EXPECT_EQ(CountObjectRows(context, root), 0u);
}

TEST_F(HierarchyPanelTest, EmptySceneRowClickKeepsNext) {
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle sceneRow = SceneRowAt(context, root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	ClickAndTick(context, *panel.Get(), CenterOf(context, sceneRow));

	SceneSystem::CreateObject("A", _scene);
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_EQ(CountObjectRows(context, root), 1u);
}

TEST_F(HierarchyPanelTest, ScenesCollapseIndependently) {
	Handle<Scene> second = SceneSystem::CreateScene("Second");
	SceneSystem::CreateObject("First object", _scene);
	Handle<SceneObject> kept = SceneSystem::CreateObject("Second object", second);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();
	ASSERT_EQ(CountObjectRows(context, root), 2u);
	ASSERT_TRUE(SceneRowAt(context, root, 1).IsValid());

	UI::ElementHandle firstCaret = FirstByClass(context, SceneRowAt(context, root, 0), StringHash("slt-tree-arrow"));
	ClickAndTick(context, *panel.Get(), CenterOf(context, firstCaret));

	ASSERT_EQ(CountObjectRows(context, root), 1u);
	const UI::Element* survivor = context.GetElement(RowAt(context, root, 0));
	ASSERT_NE(survivor, nullptr);
	EXPECT_EQ(survivor->userTag, kept.GetInstanceId());
	EXPECT_EQ(CountByClass(context, root, EditorTheme::Class::SectionHeader), 2u);
}

TEST_F(HierarchyPanelTest, ObjectRowsIndentUnderTheirSceneRow) {
	SceneSystem::CreateObject("A", _scene);

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
	ASSERT_TRUE(panel.IsValid());

	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {600, 800};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	panel.Get()->BuildContent(context, root);
	context.Update();

	UI::ElementHandle sceneCaret = FirstByClass(context, SceneRowAt(context, root, 0), StringHash("slt-tree-arrow"));
	UI::ElementHandle objectCaret = FirstByClass(context, RowAt(context, root, 0), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(sceneCaret.IsValid());
	ASSERT_TRUE(objectCaret.IsValid());
	f32 step = context.GetStyleVarFloat("--slt-tree-indent"_hash, 0.0f);
	ASSERT_GT(step, 0.0f);
	EXPECT_FLOAT_EQ(context.GetLayout(objectCaret).borderBox.x - context.GetLayout(sceneCaret).borderBox.x, step);
}

namespace {

	struct PanelRig {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;
		Handle<EditorPanel> panel;

		explicit PanelRig(UI::ContextDesc desc)
			: context(desc, &fonts) {
			EditorTheme::ApplyToContext(context);
			panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HierarchyPanel")));
			root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			panel.Get()->BuildContent(context, root);
			context.Update();
		}

		void BeginDrag() {
			context.SetHoverSuppressed(true);
			panel.Get()->OnTick(0.016f);
			context.Update();
		}
	};

	UI::ContextDesc PanelDesc() {
		UI::ContextDesc desc;
		desc.sizePx = {600, 800};
		return desc;
	}

	struct IconFaceRig {
		bool mounted = false;
		bool available = false;

		IconFaceRig() {
			std::filesystem::path engineAssets = Checkout::Find("EngineAssets");
			if (engineAssets.empty()) {
				return;
			}
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			AssetDatabase::Initialize();
			mounted = true;
			EditorIcons::Shutdown();
			available = EditorIcons::IsAvailable(EditorIcons::Face::Solid);
		}
		~IconFaceRig() {
			if (!mounted) {
				return;
			}
			EditorIcons::Shutdown();
			AssetDatabase::Terminate();
			FileSystem::Unmount("engine");
		}
	};

} // anonymous namespace

TEST_F(HierarchyPanelTest, PickingSwatchFromRowMenuTintsSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	PanelRig rig(PanelDesc());
	ASSERT_TRUE(rig.panel.IsValid());

	UI::ElementHandle row = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(row.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	ASSERT_EQ(rig.context.GetOpenPopupCount(), 1u);

	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	UI::ElementHandle colorRow = MenuRowNamed(rig.context, menu, "Color");
	ASSERT_TRUE(colorRow.IsValid());
	Hover(rig.context, CenterOf(rig.context, colorRow));
	ASSERT_EQ(rig.context.GetOpenPopupCount(), 2u);

	UI::ElementHandle submenu = rig.context.GetOpenPopup(1);
	UI::ElementHandle green = MenuRowNamed(rig.context, submenu, "Green");
	ASSERT_TRUE(green.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, green));

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(a), LabelColor::Green);
	Hover(rig.context, AwayFromRows);
	EditorSelection::Clear();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	UI::ElementHandle tinted = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(tinted.IsValid());
	const UI::Style& style = rig.context.GetComputedStyle(tinted);
	EXPECT_EQ(style.background.type, UI::BrushType::LinearGradient);
	EXPECT_EQ(style.background.gradient0, EditorTheme::GetColor(EditorTheme::ColorToken::LabelGreen));
	EXPECT_EQ(style.background.gradient1.a, 0.0f);
	EXPECT_GT(style.background.gradient0.a, 0.0f);
}

TEST_F(HierarchyPanelTest, ASwatchAppliesToEverySelectedRow) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	EditorSelection::AddToSelection(Handle<Object>(b.Get()));

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowAt(rig.context, rig.root, 0);
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	Hover(rig.context, CenterOf(rig.context, MenuRowNamed(rig.context, menu, "Color")));
	UI::ElementHandle submenu = rig.context.GetOpenPopup(1);
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, MenuRowNamed(rig.context, submenu, "Blue")));

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(a), LabelColor::Blue);
	EXPECT_EQ(SceneAnnotations::GetObjectLabel(b), LabelColor::Blue);
}

TEST_F(HierarchyPanelTest, RightClickingUnselectedRowRetargetsColorMenu) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));

	PanelRig rig(PanelDesc());
	UI::ElementHandle second = RowAt(rig.context, rig.root, 1);
	ASSERT_TRUE(second.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, second));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	Hover(rig.context, CenterOf(rig.context, MenuRowNamed(rig.context, menu, "Color")));
	UI::ElementHandle submenu = rig.context.GetOpenPopup(1);
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, MenuRowNamed(rig.context, submenu, "Red")));

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(b), LabelColor::Red);
	EXPECT_EQ(SceneAnnotations::GetObjectLabel(a), LabelColor::None);
}

TEST_F(HierarchyPanelTest, AColoredRowStillReadsAsSelected) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(a);
	SceneAnnotations::SetObjectLabels({one.data(), one.size()}, LabelColor::Purple);

	PanelRig rig(PanelDesc());
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	UI::ElementHandle row = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(row.IsValid());
	ASSERT_EQ(rig.context.GetComputedStyle(row).background.type, UI::BrushType::LinearGradient);

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	Hover(rig.context, AwayFromRows);
	UI::ElementHandle selected = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(selected.IsValid());
	ASSERT_TRUE(rig.context.HasClass(selected, StringHash("slt-selected")));
	const UI::Style& style = rig.context.GetComputedStyle(selected);
	EXPECT_EQ(style.background.type, UI::BrushType::Solid);
	EXPECT_EQ(style.background.solid, EditorTheme::GetColor(EditorTheme::ColorToken::Selected));
}

TEST_F(HierarchyPanelTest, ALabelChangeAloneRebuildsTheRows) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	PanelRig rig(PanelDesc());

	UI::ElementHandle before = RowAt(rig.context, rig.root, 0);
	ASSERT_EQ(rig.context.GetComputedStyle(before).background.type, UI::BrushType::None);

	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(a);
	SceneAnnotations::SetObjectLabels({one.data(), one.size()}, LabelColor::Orange);

	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	UI::ElementHandle after = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(after.IsValid());
	EXPECT_EQ(rig.context.GetComputedStyle(after).background.type, UI::BrushType::LinearGradient);
}

TEST_F(HierarchyPanelTest, FolderRowAtChildIndent) {
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", _scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene, host);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(child);
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Inner", FolderParent::Under(host.GetInstanceId()),
												{one.data(), one.size()});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	ASSERT_EQ(CountObjectRows(rig.context, rig.root), 1u);
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, FirstByClass(rig.context, RowAt(rig.context, rig.root, 0),
													StringHash("slt-tree-arrow"))));

	UI::ElementHandle hostRow = RowAt(rig.context, rig.root, 0);
	UI::ElementHandle folderRow = RowAt(rig.context, rig.root, 1);
	UI::ElementHandle childRow = RowAt(rig.context, rig.root, 2);
	ASSERT_TRUE(folderRow.IsValid());
	ASSERT_TRUE(childRow.IsValid());
	EXPECT_NE(rig.context.GetElement(folderRow)->userTag, child.GetInstanceId());

	f32 hostIndent =
		rig.context.GetLayout(FirstByClass(rig.context, hostRow, StringHash("slt-tree-indent"))).borderBox.w;
	f32 folderIndent =
		rig.context.GetLayout(FirstByClass(rig.context, folderRow, StringHash("slt-tree-indent"))).borderBox.w;
	f32 childIndent =
		rig.context.GetLayout(FirstByClass(rig.context, childRow, StringHash("slt-tree-indent"))).borderBox.w;
	EXPECT_GT(folderIndent, hostIndent);
	EXPECT_GT(childIndent, folderIndent);
}

TEST_F(HierarchyPanelTest, FoldersSortAboveLooseObjects) {
	Handle<SceneObject> filed = SceneSystem::CreateObject("Filed", _scene);
	SceneSystem::CreateObject("Loose", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(filed);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {one.data(), one.size()}), 0u);

	PanelRig rig(PanelDesc());
	ASSERT_EQ(CountObjectRows(rig.context, rig.root), 3u);
	f32 first =
		rig.context.GetLayout(FirstByClass(rig.context, RowAt(rig.context, rig.root, 0), StringHash("slt-tree-indent")))
			.borderBox.w;
	f32 second =
		rig.context.GetLayout(FirstByClass(rig.context, RowAt(rig.context, rig.root, 1), StringHash("slt-tree-indent")))
			.borderBox.w;
	f32 third =
		rig.context.GetLayout(FirstByClass(rig.context, RowAt(rig.context, rig.root, 2), StringHash("slt-tree-indent")))
			.borderBox.w;
	EXPECT_GT(second, first);
	EXPECT_EQ(third, first);
}

TEST_F(HierarchyPanelTest, FolderRefusedForMixedParents) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene, parent);
	Handle<SceneObject> root = SceneSystem::CreateObject("Root", _scene);
	EditorSelection::Select(Handle<Object>(child.Get()));
	EditorSelection::AddToSelection(Handle<Object>(root.Get()));

	PanelRig rig(PanelDesc());
	UI::ElementHandle rootRow = RowAt(rig.context, rig.root, 1);
	ASSERT_TRUE(rootRow.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, rootRow));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	UI::ElementHandle entry = MenuRowNamed(rig.context, menu, "New Folder from Selection");
	ASSERT_TRUE(entry.IsValid());
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 2u);
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));
	Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
	SceneAnnotations::GetChildFolders(_scene, FolderParent::Roots(), folders);
	EXPECT_TRUE(folders.empty());
}

TEST_F(HierarchyPanelTest, CreatingFolderFromMenuFilesWholeSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	EditorSelection::AddToSelection(Handle<Object>(b.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowAt(rig.context, rig.root, 0)));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	UI::ElementHandle entry = MenuRowNamed(rig.context, menu, "New Folder from Selection");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	u64 folder = SceneAnnotations::GetFolderOf(a);
	EXPECT_NE(folder, 0u);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(b), folder);
	EXPECT_FALSE(a.Get()->GetParent().IsValid());
	EXPECT_FALSE(b.Get()->GetParent().IsValid());
}

TEST_F(HierarchyPanelTest, DroppingObjectOnFolderRowFilesIt) {
	Handle<SceneObject> loose = SceneSystem::CreateObject("Loose", _scene);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}), 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle folderRow = RowNamed(rig.context, rig.root, "Bin");
	ASSERT_TRUE(folderRow.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({loose}));
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, folderRow)));
	EXPECT_NE(SceneAnnotations::GetFolderOf(loose), 0u);
}

TEST_F(HierarchyPanelTest, DroppingObjectOnCollapsedRowOpensIt) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Existing", _scene, parent);
	Handle<SceneObject> loose = SceneSystem::CreateObject("Loose", _scene);
	PanelRig rig(PanelDesc());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Existing").IsValid());

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({loose}));
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, RowNamed(rig.context, rig.root, "Parent"))));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	ASSERT_EQ(loose.Get()->GetParent(), parent);
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Loose").IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Existing").IsValid());
}

TEST_F(HierarchyPanelTest, CreatingObjectInCollapsedSceneRevealsIt) {
	SceneSystem::CreateObject("A", _scene);
	PanelRig rig(PanelDesc());
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, FirstByClass(rig.context, SceneRowAt(rig.context, rig.root, 0),
													StringHash("slt-tree-arrow"))));
	ASSERT_EQ(CountObjectRows(rig.context, rig.root), 0u);

	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, Handle<SceneObject>{}, items);
	ASSERT_FALSE(items.empty());
	ASSERT_EQ(items[0].label, "Create Empty");
	items[0].onSelect();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_EQ(CountObjectRows(rig.context, rig.root), 2u);
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "New Object").IsValid());
}

TEST_F(HierarchyPanelTest, CreatingChildUnderCollapsedRowRevealsIt) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Existing", _scene, parent);
	PanelRig rig(PanelDesc());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Existing").IsValid());

	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, parent, items);
	ASSERT_FALSE(items.empty());
	items[0].onSelect();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(RowNamed(rig.context, rig.root, "New Object").IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Existing").IsValid());
}

TEST_F(HierarchyPanelTest, NewSelectionRevealed) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	Handle<SceneObject> sibling = SceneSystem::CreateObject("Sibling", _scene);
	PanelRig rig(PanelDesc());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());

	EditorSelection::Select(Handle<Object>(hidden.Get()));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "Hidden").IsValid());

	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"),
													StringHash("slt-tree-arrow"))));
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());
	ASSERT_TRUE(EditorSelection::IsSelected(Handle<Object>(hidden.Get())));

	EditorSelection::AddToSelection(Handle<Object>(sibling.Get()));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());
}

TEST_F(HierarchyPanelTest, EmptiedRowHasNoStrip) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene, parent);
	SceneSystem::CreateObject("Sibling", _scene);
	PanelRig rig(PanelDesc());
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"),
													StringHash("slt-tree-arrow"))));
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "Child").IsValid());
	rig.BeginDrag();

	Vector<UI::ElementHandle> strips{GetMemoryResource(MemTag::Editor)};
	CollectStripsBetween(rig.context, rig.root, "Child", "Sibling", strips);
	ASSERT_EQ(strips.size(), 2u);

	child.Get()->SetParent({});
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ASSERT_EQ(RowNamed(rig.context, rig.root, "Child").IsValid(), true);
	strips.clear();
	CollectStripsBetween(rig.context, rig.root, "Parent", "Sibling", strips);
	EXPECT_EQ(strips.size(), 1u);

	SceneSystem::CreateObject("Late", _scene, parent);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Late").IsValid());
}

TEST_F(HierarchyPanelTest, FolderDropNestsUnderObject) {
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", _scene);
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle hostRow = RowNamed(rig.context, rig.root, "Host");
	ASSERT_TRUE(hostRow.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneFolder"_hash;
	payload.data = Variant(SceneFolderDragData{_scene.GetInstanceId(), folder});
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, hostRow)));

	FolderInfo info;
	ASSERT_TRUE(SceneAnnotations::GetFolder(_scene, folder, info));
	EXPECT_EQ(info.parent.kind, FolderParentKind::Object);
	EXPECT_EQ(info.parent.id, host.GetInstanceId());
}

TEST_F(HierarchyPanelTest, DroppingFolderOnInsertStripReordersIt) {
	SceneAnnotations::CreateFolder(_scene, "First", FolderParent::Roots(), {});
	SceneAnnotations::CreateFolder(_scene, "Second", FolderParent::Roots(), {});
	u64 third = SceneAnnotations::CreateFolder(_scene, "Third", FolderParent::Roots(), {});

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	auto names = [this]() {
		Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
		SceneAnnotations::GetChildFolders(_scene, FolderParent::Roots(), folders);
		str out;
		for (const FolderInfo& folder : folders) {
			out += folder.name + " ";
		}
		return out;
	};
	auto dropThirdAbove = [&](std::string_view rowLabel) {
		UI::ElementHandle strip = StripBefore(rig.context, rig.root, rowLabel);
		ASSERT_TRUE(strip.IsValid());
		UI::DragPayload payload;
		payload.type = "SceneFolder"_hash;
		payload.data = Variant(SceneFolderDragData{_scene.GetInstanceId(), third});
		ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, strip)));
		rig.panel.Get()->OnTick(0.016f);
		rig.context.Update();
	};

	dropThirdAbove("First");
	EXPECT_EQ(names(), "Third First Second ");

	dropThirdAbove("Second");
	EXPECT_EQ(names(), "First Third Second ");
}

TEST_F(HierarchyPanelTest, SceneRowDropUnfiles) {
	Handle<SceneObject> filed = SceneSystem::CreateObject("Filed", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(filed);
	u64 outer = SceneAnnotations::CreateFolder(_scene, "Outer", FolderParent::Roots(), {one.data(), one.size()});
	u64 inner = SceneAnnotations::CreateFolder(_scene, "Inner", FolderParent::In(outer), {});
	ASSERT_NE(inner, 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle sceneRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(sceneRow.IsValid());

	UI::DragPayload objectPayload;
	objectPayload.type = "SceneObject"_hash;
	objectPayload.data = Variant(DraggedObjects({filed}));
	ASSERT_TRUE(rig.context.DeliverDrop(objectPayload, CenterOf(rig.context, sceneRow)));
	EXPECT_EQ(SceneAnnotations::GetFolderOf(filed), 0u);

	UI::DragPayload folderPayload;
	folderPayload.type = "SceneFolder"_hash;
	folderPayload.data = Variant(SceneFolderDragData{_scene.GetInstanceId(), inner});
	ASSERT_TRUE(rig.context.DeliverDrop(folderPayload, CenterOf(rig.context, sceneRow)));
	FolderInfo info;
	ASSERT_TRUE(SceneAnnotations::GetFolder(_scene, inner, info));
	EXPECT_EQ(info.parent.kind, FolderParentKind::SceneRoot);
}

TEST_F(HierarchyPanelTest, FolderIsNotDropTargetForSubtree) {
	u64 outer = SceneAnnotations::CreateFolder(_scene, "Outer", FolderParent::Roots(), {});
	u64 inner = SceneAnnotations::CreateFolder(_scene, "Inner", FolderParent::In(outer), {});
	ASSERT_NE(inner, 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle innerRow = RowNamed(rig.context, rig.root, "Inner");
	ASSERT_TRUE(innerRow.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneFolder"_hash;
	payload.data = Variant(SceneFolderDragData{_scene.GetInstanceId(), outer});
	EXPECT_FALSE(rig.context.DeliverDrop(payload, CenterOf(rig.context, innerRow)));
	FolderInfo info;
	ASSERT_TRUE(SceneAnnotations::GetFolder(_scene, outer, info));
	EXPECT_EQ(info.parent.kind, FolderParentKind::SceneRoot);
}

TEST_F(HierarchyPanelTest, PressedFolderStaysDraggable) {
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}), 0u);
	PanelRig rig(PanelDesc());

	UI::ElementHandle row = RowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(row.IsValid());
	u32 drags = 0;
	auto chained = rig.context.Events(row).onPressDrag;
	rig.context.Events(row).onPressDrag = [&drags, chained](Vec2 point) {
		++drags;
		if (chained) {
			chained(point);
		}
	};

	Vec2 start = CenterOf(rig.context, row);
	rig.context.InjectInput(Mouse(InputEventType::MouseMove, start));
	rig.context.InjectInput(Mouse(InputEventType::MouseDown, start));
	rig.context.Update();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ASSERT_TRUE(rig.context.IsAlive(row));
	rig.context.InjectInput(Mouse(InputEventType::MouseMove, {start.x + 20.0f, start.y}));
	rig.context.Update();
	EXPECT_GT(drags, 0u);
}

TEST_F(HierarchyPanelTest, DoubleClickingFolderRowStartsRename) {
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Before", FolderParent::Roots(), {}), 0u);
	PanelRig rig(PanelDesc());

	Vec2 point = CenterOf(rig.context, RowAt(rig.context, rig.root, 0));
	ClickAndTick(rig.context, *rig.panel.Get(), point);
	ClickAndTick(rig.context, *rig.panel.Get(), point);

	UI::ElementHandle field = FirstByClass(rig.context, rig.root, StringHash("slt-textfield"));
	ASSERT_TRUE(field.IsValid());
}

TEST_F(HierarchyPanelTest, DeletingFolderKeepsObjectsItHeld) {
	Handle<SceneObject> filed = SceneSystem::CreateObject("Filed", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(filed);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {one.data(), one.size()}), 0u);

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowAt(rig.context, rig.root, 0)));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Delete Folder");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
	SceneAnnotations::GetChildFolders(_scene, FolderParent::Roots(), folders);
	EXPECT_TRUE(folders.empty());
	EXPECT_TRUE(filed.IsValid());
	EXPECT_EQ(CountObjectRows(rig.context, rig.root), 1u);
}

TEST_F(HierarchyPanelTest, NewFolderLandsWhereSelectionWas) {
	SceneSystem::CreateObject("First", _scene);
	SceneSystem::CreateObject("Second", _scene);
	Handle<SceneObject> third = SceneSystem::CreateObject("Third", _scene);
	SceneSystem::CreateObject("Fourth", _scene);
	EditorSelection::Select(Handle<Object>(third.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Third")));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "New Folder from Selection");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	u64 folder = SceneAnnotations::GetFolderOf(third);
	ASSERT_NE(folder, 0u);
	FolderInfo info;
	ASSERT_TRUE(SceneAnnotations::GetFolder(_scene, folder, info));
	EXPECT_EQ(info.slot, third.Get()->GetSiblingIndex());

	f32 folderY = rig.context.GetLayout(RowNamed(rig.context, rig.root, "New Folder")).borderBox.y;
	EXPECT_LT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "First")).borderBox.y, folderY);
	EXPECT_LT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "Second")).borderBox.y, folderY);
	EXPECT_GT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "Fourth")).borderBox.y, folderY);
}

TEST_F(HierarchyPanelTest, FolderCanBeDroppedBetween2Objects) {
	SceneSystem::CreateObject("First", _scene);
	SceneSystem::CreateObject("Second", _scene);
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}, 0);
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "Second");
	ASSERT_TRUE(strip.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneFolder"_hash;
	payload.data = Variant(SceneFolderDragData{_scene.GetInstanceId(), folder});
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, strip)));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	FolderInfo info;
	ASSERT_TRUE(SceneAnnotations::GetFolder(_scene, folder, info));
	EXPECT_EQ(info.slot, 1u);
	f32 folderY = rig.context.GetLayout(RowNamed(rig.context, rig.root, "Bin")).borderBox.y;
	EXPECT_LT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "First")).borderBox.y, folderY);
	EXPECT_GT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "Second")).borderBox.y, folderY);
}

TEST_F(HierarchyPanelTest, FiledObjectsSwap) {
	Handle<SceneObject> a = SceneSystem::CreateObject("Alpha", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("Beta", _scene);
	Vector<Handle<SceneObject>> both{GetMemoryResource(MemTag::Editor)};
	both.push_back(a);
	both.push_back(b);
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {both.data(), both.size()});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "Alpha");
	ASSERT_TRUE(strip.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({b}));
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, strip)));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_EQ(b.Get()->GetSiblingIndex(), 0u);
	EXPECT_EQ(a.Get()->GetSiblingIndex(), 1u);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(a), folder);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(b), folder);
	EXPECT_GT(rig.context.GetLayout(RowNamed(rig.context, rig.root, "Alpha")).borderBox.y,
			  rig.context.GetLayout(RowNamed(rig.context, rig.root, "Beta")).borderBox.y);
}

TEST_F(HierarchyPanelTest, OuterStripDropUnfiles) {
	Handle<SceneObject> filed = SceneSystem::CreateObject("Filed", _scene);
	Handle<SceneObject> loose = SceneSystem::CreateObject("Loose", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(filed);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {one.data(), one.size()}), 0u);

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "Loose");
	ASSERT_TRUE(strip.IsValid());

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({filed}));
	u32 depth = UndoSystem::GetUndoDepth();
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, strip)));

	EXPECT_EQ(SceneAnnotations::GetFolderOf(filed), 0u);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth + 1);
	(void)loose;
}

TEST_F(HierarchyPanelTest, RenameFieldArrivesFocused) {
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Before", FolderParent::Roots(), {}), 0u);
	PanelRig rig(PanelDesc());

	Vec2 point = CenterOf(rig.context, RowNamed(rig.context, rig.root, "Before"));
	ClickAndTick(rig.context, *rig.panel.Get(), point);
	ClickAndTick(rig.context, *rig.panel.Get(), point);

	UI::ElementHandle field = FirstByClass(rig.context, rig.root, StringHash("slt-textfield"));
	ASSERT_TRUE(field.IsValid());
	EXPECT_EQ(rig.context.GetFocused(), field);
}

TEST_F(HierarchyPanelTest, AnObjectRenameFieldArrivesFocusedToo) {
	SceneSystem::CreateObject("Before", _scene);
	PanelRig rig(PanelDesc());

	Vec2 point = CenterOf(rig.context, RowNamed(rig.context, rig.root, "Before"));
	ClickAndTick(rig.context, *rig.panel.Get(), point);
	ASSERT_TRUE(rig.panel.Get()->OnCommand(PanelCommand::Rename));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	UI::ElementHandle field = FirstByClass(rig.context, rig.root, StringHash("slt-textfield"));
	ASSERT_TRUE(field.IsValid());
	EXPECT_EQ(rig.context.GetFocused(), field);
}

TEST_F(HierarchyPanelTest, MenuCreateKeepsListFocus) {
	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), AwayFromRows);
	UI::ElementHandle popup = rig.context.GetOpenPopup(0);
	ASSERT_TRUE(popup.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, MenuRowNamed(rig.context, popup, "Create Empty")));
	ASSERT_EQ(rig.context.GetOpenPopupCount(), 0u);
	ASSERT_TRUE(EditorSelection::GetActive().IsValid());

	ASSERT_TRUE(rig.panel.Get()->OnCommand(PanelCommand::Rename));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	UI::ElementHandle field = FirstByClass(rig.context, rig.root, StringHash("slt-textfield"));
	ASSERT_TRUE(field.IsValid());
	EXPECT_EQ(rig.context.GetFocused(), field);
}

TEST_F(HierarchyPanelTest, RightClickingRowDoesNotSelectIt) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "B")));

	ASSERT_EQ(rig.context.GetOpenPopupCount(), 1u);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 1u);
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(a.Get())));
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(b.Get())));
}

TEST_F(HierarchyPanelTest, RightClickingLeavesAMultiSelectionIntact) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	SceneSystem::CreateObject("C", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	EditorSelection::AddToSelection(Handle<Object>(b.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "C")));
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);
}

TEST_F(HierarchyPanelTest, EmptySpaceClickDeselects) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	PanelRig rig(PanelDesc());
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	UI::ElementHandle rowA = RowNamed(rig.context, rig.root, "A");
	ASSERT_TRUE(rig.context.HasClass(rowA, StringHash("slt-selected")));
	ASSERT_TRUE(rig.context.HasClass(rig.context.HitTest(AwayFromRows), StringHash("slt-scroll-viewport")));

	auto clickWithoutTick = [&rig](Vec2 point) {
		rig.context.InjectInput(Mouse(InputEventType::MouseMove, point));
		rig.context.InjectInput(Mouse(InputEventType::MouseDown, point));
		rig.context.InjectInput(Mouse(InputEventType::MouseUp, point));
		rig.context.Update();
	};
	clickWithoutTick(AwayFromRows);
	EXPECT_FALSE(rig.context.HasClass(rowA, StringHash("slt-selected")));
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 0u);

	EditorSelection::SelectAsset(Guid(0x1234, 0x5678));
	ASSERT_TRUE(EditorSelection::HasAssetSelection());
	clickWithoutTick(AwayFromRows);
	EXPECT_FALSE(EditorSelection::HasAssetSelection());
}

TEST_F(HierarchyPanelTest, RowMenuActsOnUnselectedRow) {
	Handle<SceneObject> selected = SceneSystem::CreateObject("Selected", _scene);
	Handle<SceneObject> clicked = SceneSystem::CreateObject("Clicked", _scene);
	EditorSelection::Select(Handle<Object>(selected.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Clicked")));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	Hover(rig.context, CenterOf(rig.context, MenuRowNamed(rig.context, menu, "Color")));
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, MenuRowNamed(rig.context, rig.context.GetOpenPopup(1), "Red")));

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(clicked), LabelColor::Red);
	EXPECT_EQ(SceneAnnotations::GetObjectLabel(selected), LabelColor::None);
}

TEST_F(HierarchyPanelTest, RowMenuActsOnSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	EditorSelection::AddToSelection(Handle<Object>(b.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "B")));
	UI::ElementHandle menu = rig.context.GetOpenPopup(0);
	Hover(rig.context, CenterOf(rig.context, MenuRowNamed(rig.context, menu, "Color")));
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, MenuRowNamed(rig.context, rig.context.GetOpenPopup(1), "Teal")));

	EXPECT_EQ(SceneAnnotations::GetObjectLabel(a), LabelColor::Teal);
	EXPECT_EQ(SceneAnnotations::GetObjectLabel(b), LabelColor::Teal);
}

TEST_F(HierarchyPanelTest, MenuDeleteTakesClickedRow) {
	Handle<SceneObject> selected = SceneSystem::CreateObject("Selected", _scene);
	Handle<SceneObject> clicked = SceneSystem::CreateObject("Clicked", _scene);
	EditorSelection::Select(Handle<Object>(selected.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Clicked")));
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Delete")));
	SceneSystem::EndFrame();

	EXPECT_FALSE(clicked.IsValid());
	EXPECT_TRUE(selected.IsValid());
}

TEST_F(HierarchyPanelTest, MenuDuplicateCopiesSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	EditorSelection::Select(Handle<Object>(a.Get()));
	EditorSelection::AddToSelection(Handle<Object>(b.Get()));

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "B")));
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Duplicate")));
	SceneSystem::EndFrame();

	ASSERT_EQ(_scene.Get()->GetRootCount(), 4u);
	EXPECT_EQ(_scene.Get()->GetRoot(1).Get()->name, "A (1)");
	EXPECT_EQ(_scene.Get()->GetRoot(3).Get()->name, "B (1)");
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), _scene.Get()->GetRoot(3).GetInstanceId());
}

TEST_F(HierarchyPanelTest, HoverDoesNotOverrideASelectedRow) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	PanelRig rig(PanelDesc());

	UI::ElementHandle row = RowNamed(rig.context, rig.root, "A");
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	row = RowNamed(rig.context, rig.root, "A");
	ASSERT_TRUE(rig.context.HasClass(row, StringHash("slt-selected")));

	Hover(rig.context, CenterOf(rig.context, row));
	ASSERT_EQ(rig.context.GetHovered(), row);
	const UI::Style& style = rig.context.GetComputedStyle(row);
	EXPECT_EQ(style.background.solid, EditorTheme::GetColor(EditorTheme::ColorToken::Selected));
	EXPECT_NE(style.background.solid, EditorTheme::GetColor(EditorTheme::ColorToken::RowHover));
}

TEST_F(HierarchyPanelTest, HoverDoesNotOverrideALabeledRow) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(a);
	SceneAnnotations::SetObjectLabels({one.data(), one.size()}, LabelColor::Purple);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "A");
	ASSERT_TRUE(row.IsValid());
	Hover(rig.context, CenterOf(rig.context, row));
	ASSERT_EQ(rig.context.GetHovered(), row);

	const UI::Style& style = rig.context.GetComputedStyle(row);
	EXPECT_EQ(style.background.type, UI::BrushType::LinearGradient);
	EXPECT_EQ(style.background.gradient0, EditorTheme::GetColor(EditorTheme::ColorToken::LabelPurple));
}

TEST_F(HierarchyPanelTest, LabeledRowShowsSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Vector<Handle<SceneObject>> one{GetMemoryResource(MemTag::Editor)};
	one.push_back(a);
	SceneAnnotations::SetObjectLabels({one.data(), one.size()}, LabelColor::Orange);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "A");
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	row = RowNamed(rig.context, rig.root, "A");
	ASSERT_TRUE(rig.context.HasClass(row, StringHash("slt-selected")));

	Hover(rig.context, AwayFromRows);
	EXPECT_EQ(rig.context.GetComputedStyle(row).background.solid,
			  EditorTheme::GetColor(EditorTheme::ColorToken::Selected));
	Hover(rig.context, CenterOf(rig.context, row));
	EXPECT_EQ(rig.context.GetComputedStyle(row).background.solid,
			  EditorTheme::GetColor(EditorTheme::ColorToken::Selected));
}

TEST_F(HierarchyPanelTest, AFolderRowsLabelSurvivesHoverToo) {
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);
	SceneAnnotations::SetFolderLabel(_scene, folder, LabelColor::Blue);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Bin");
	ASSERT_TRUE(row.IsValid());
	Hover(rig.context, CenterOf(rig.context, row));
	ASSERT_EQ(rig.context.GetHovered(), row);
	const UI::Style& style = rig.context.GetComputedStyle(row);
	EXPECT_EQ(style.background.type, UI::BrushType::LinearGradient);
	EXPECT_EQ(style.background.gradient0, EditorTheme::GetColor(EditorTheme::ColorToken::LabelBlue));
}

TEST_F(HierarchyPanelTest, GlyphBetweenGutterAndName) {
	SceneSystem::CreateObject("Cube", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Cube");
	ASSERT_TRUE(row.IsValid());

	UI::ElementHandle glyph = RowGlyph(rig.context, row);
	ASSERT_TRUE(rig.context.IsAlive(glyph));
	EXPECT_EQ(rig.context.GetElement(glyph)->flags & UI::ElementFlags::Hoverable, UI::ElementFlags::None);

	UI::ElementHandle label = NextSibling(rig.context, glyph);
	ASSERT_TRUE(rig.context.IsAlive(label));
	EXPECT_EQ(rig.context.GetTextString(label), "Cube");

	UI::ElementHandle eye = ActiveToggle(rig.context, row);
	ASSERT_TRUE(eye.IsValid());
	EXPECT_GT(rig.context.GetLayout(eye).borderBox.x, rig.context.GetLayout(label).borderBox.x);
}

TEST_F(HierarchyPanelTest, EachRowKindDrawsDistinctGlyph) {
	IconFaceRig face;
	if (!face.available) {
		GTEST_SKIP() << "icon font not found";
	}

	Handle<SceneObject> object = SceneSystem::CreateObject("Cube", _scene);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}), 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle objectRow = RowNamed(rig.context, rig.root, "Cube");
	ASSERT_TRUE(objectRow.IsValid());
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, objectRow)),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::SceneObject));
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, RowNamed(rig.context, rig.root, "Bin"))),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::FolderOpen));
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, SceneRowAt(rig.context, rig.root, 0))),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::AssetScene));

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowGlyph(rig.context, objectRow)));
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(object.Get())));
}

TEST_F(HierarchyPanelTest, RowShowsFirstComponentBadge) {
	IconFaceRig face;
	if (!face.available) {
		GTEST_SKIP() << "icon font not found";
	}

	static const u8 owner = 0;
	struct Sweep {
		~Sweep() { EditorIcons::ComponentIcons::UnregisterOwnedBy(&owner); }
	} sweep;
	EditorIcons::ComponentIcons::Register(HierTestComponent::GetReflectType()->name, EditorIcons::Mark::Camera, &owner);
	EditorIcons::ComponentIcons::Register(HierBadgeComponent::GetReflectType()->name, EditorIcons::Mark::Light, &owner);
	Handle<VectorAsset> camera = EditorIcons::GetVector(EditorIcons::Mark::Camera);
	Handle<VectorAsset> light = EditorIcons::GetVector(EditorIcons::Mark::Light);
	ASSERT_TRUE(camera.IsValid() && light.IsValid() && camera != light);
	const Handle<VectorAsset> cube = EditorIcons::GetGlyph(EditorIcons::Glyph::SceneObject);

	Handle<SceneObject> lamp = SceneSystem::CreateObject("Lamp", _scene);
	lamp.Get()->AddComponent<HierTestComponent>();
	lamp.Get()->AddComponent<HierBadgeComponent>();
	Handle<SceneObject> empty = SceneSystem::CreateObject("Empty", _scene);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}), 0u);

	PanelRig rig(PanelDesc());
	auto row = [&](std::string_view name) { return RowNamed(rig.context, rig.root, name); };
	auto pictureOf = [&](std::string_view name) { return rig.context.GetIconAsset(RowGlyph(rig.context, row(name))); };
	auto tick = [&]() {
		rig.panel.Get()->OnTick(0.016f);
		rig.context.Update();
	};

	ASSERT_TRUE(row("Lamp").IsValid() && row("Empty").IsValid() && row("Bin").IsValid());
	EXPECT_EQ(pictureOf("Lamp"), camera);
	EXPECT_EQ(pictureOf("Empty"), cube);

	auto labelX = [&](std::string_view name) {
		return rig.context.GetLayout(NextSibling(rig.context, RowGlyph(rig.context, row(name)))).borderBox.x;
	};
	auto rowH = [&](std::string_view name) { return rig.context.GetLayout(row(name)).borderBox.h; };
	EXPECT_EQ(labelX("Lamp"), labelX("Empty"));
	EXPECT_EQ(labelX("Lamp"), labelX("Bin"));
	EXPECT_EQ(rowH("Lamp"), rowH("Empty"));
	EXPECT_EQ(rowH("Lamp"), rowH("Bin"));

	ASSERT_TRUE(EditorSceneOps::DropComponentOn(lamp.Get()->GetComponentAt(1), lamp, 0));
	tick();
	EXPECT_EQ(pictureOf("Lamp"), light);

	empty.Get()->AddComponent<HierTestComponent>();
	tick();
	EXPECT_EQ(pictureOf("Empty"), camera);

	SceneSystem::Destroy(lamp.Get()->GetComponentAt(0));
	SceneSystem::EndFrame();
	tick();
	EXPECT_EQ(pictureOf("Lamp"), camera);
	SceneSystem::Destroy(lamp.Get()->GetComponentAt(0));
	SceneSystem::EndFrame();
	tick();
	EXPECT_EQ(pictureOf("Lamp"), cube);
}

TEST_F(HierarchyPanelTest, ClickingTheEyeTogglesActiveThroughUndo) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Cube", _scene);
	ASSERT_TRUE(object.Get()->IsActiveSelf());

	PanelRig rig(PanelDesc());
	UI::ElementHandle eye = ActiveToggle(rig.context, RowNamed(rig.context, rig.root, "Cube"));
	ASSERT_TRUE(eye.IsValid());

	u32 depth = UndoSystem::GetUndoDepth();
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, eye));
	EXPECT_FALSE(object.Get()->IsActiveSelf());
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth + 1);
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(object.Get())));

	UndoSystem::Undo();
	EXPECT_TRUE(object.Get()->IsActiveSelf());

	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, ActiveToggle(rig.context, RowNamed(rig.context, rig.root, "Cube"))));
	EXPECT_FALSE(object.Get()->IsActiveSelf());
}

TEST_F(HierarchyPanelTest, EyeShowsOnHoverOrOff) {
	Handle<SceneObject> shown = SceneSystem::CreateObject("Shown", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene);
	hidden.Get()->SetActive(false);

	PanelRig rig(PanelDesc());
	Hover(rig.context, AwayFromRows);

	UI::ElementHandle shownRow = RowNamed(rig.context, rig.root, "Shown");
	UI::ElementHandle shownEye = ActiveToggle(rig.context, shownRow);
	ASSERT_TRUE(shownEye.IsValid());
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(shownEye).opacity, 0.0f);

	UI::ElementHandle hiddenEye = ActiveToggle(rig.context, RowNamed(rig.context, rig.root, "Hidden"));
	ASSERT_TRUE(hiddenEye.IsValid());
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(hiddenEye).opacity, 1.0f);

	Hover(rig.context, CenterOf(rig.context, shownRow));
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(shownEye).opacity, 1.0f);

	Hover(rig.context, CenterOf(rig.context, shownEye));
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(shownEye).opacity, 1.0f);

	Hover(rig.context, AwayFromRows);
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(shownEye).opacity, 0.0f);
	(void)shown;
}

TEST_F(HierarchyPanelTest, PressedEyeStaysVisible) {
	Handle<SceneObject> object = SceneSystem::CreateObject("Cube", _scene);
	ASSERT_TRUE(object.Get()->IsActiveSelf());

	PanelRig rig(PanelDesc());
	UI::ElementHandle eye = ActiveToggle(rig.context, RowNamed(rig.context, rig.root, "Cube"));
	ASSERT_TRUE(eye.IsValid());
	Vec2 point = CenterOf(rig.context, eye);

	Hover(rig.context, point);
	ASSERT_FLOAT_EQ(rig.context.GetComputedStyle(eye).opacity, 1.0f);

	rig.context.InjectInput(Mouse(InputEventType::MouseDown, point));
	rig.context.Update();
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(eye).opacity, 1.0f);

	rig.context.InjectInput(Mouse(InputEventType::MouseUp, point));
	rig.context.Update();
	EXPECT_FLOAT_EQ(rig.context.GetComputedStyle(eye).opacity, 1.0f);
	EXPECT_FALSE(object.Get()->IsActiveSelf());
}

class HierarchyStripScaleTest : public HierarchyPanelTest, public ::testing::WithParamInterface<f32> {
protected:
	UI::ContextDesc ScaledDesc() const {
		UI::ContextDesc desc = PanelDesc();
		desc.contentScale = GetParam();
		return desc;
	}
};

INSTANTIATE_TEST_SUITE_P(AtBothContentScales, HierarchyStripScaleTest, ::testing::Values(1.0f, 1.25f));

TEST_P(HierarchyStripScaleTest, RowsFlushStripsTakeNoSpace) {
	SceneSystem::CreateObject("First", _scene);
	SceneSystem::CreateObject("Second", _scene);

	PanelRig rig(ScaledDesc());
	UI::Rect first = rig.context.GetLayout(RowNamed(rig.context, rig.root, "First")).borderBox;
	UI::Rect second = rig.context.GetLayout(RowNamed(rig.context, rig.root, "Second")).borderBox;
	ASSERT_GT(second.y, first.y);
	EXPECT_FLOAT_EQ(second.y, first.y + first.h);

	UI::Rect strip = rig.context.GetLayout(StripBefore(rig.context, rig.root, "Second")).borderBox;
	EXPECT_GT(strip.h, 0.0f);
	EXPECT_FLOAT_EQ(strip.y + strip.h, second.y);
	EXPECT_GE(strip.y, first.y);
}

TEST_F(HierarchyPanelTest, BoundaryStripsTile) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Child", _scene, parent);
	SceneSystem::CreateObject("Sibling", _scene);

	PanelRig rig(PanelDesc());
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"),
													StringHash("slt-tree-arrow"))));
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "Child").IsValid());

	UI::ElementHandle child = RowNamed(rig.context, rig.root, "Child");
	Vector<UI::ElementHandle> strips{GetMemoryResource(MemTag::Editor)};
	CollectStripsBetween(rig.context, rig.root, "Child", "Sibling", strips);
	ASSERT_GE(strips.size(), 2u);

	UI::Rect childRow = rig.context.GetLayout(child).borderBox;
	f32 previousY = 0.0f;
	for (size_t index = 0; index < strips.size(); ++index) {
		UI::Rect band = rig.context.GetLayout(strips[index]).borderBox;
		EXPECT_GT(band.h, 0.0f);
		EXPECT_LE(band.y + band.h, childRow.y + childRow.h + 0.5f);
		if (index > 0) {
			EXPECT_GT(band.y, previousY);
		}
		previousY = band.y;
	}
}

TEST_P(HierarchyStripScaleTest, StripInertUntilDrag) {
	Handle<SceneObject> first = SceneSystem::CreateObject("First", _scene);
	SceneSystem::CreateObject("Second", _scene);

	PanelRig rig(ScaledDesc());
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "Second");
	ASSERT_TRUE(strip.IsValid());
	Vec2 overTheBand = CenterOf(rig.context, strip);

	ClickAndTick(rig.context, *rig.panel.Get(), overTheBand);
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(first.Get())));
	Hover(rig.context, overTheBand);
	EXPECT_EQ(rig.context.GetHovered(), RowNamed(rig.context, rig.root, "First"));

	rig.BeginDrag();
	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({first}));
	EXPECT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, StripBefore(rig.context, rig.root, "Second"))));
}

namespace {

	class HierarchyAssetDropTest : public HierarchyPanelTest {
	protected:
		std::filesystem::path _projectRoot;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_hierdrop_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_projectRoot / "Assets");
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			AssetDatabase::Initialize();
			HierarchyPanelTest::SetUp();
		}

		void TearDown() override {
			HierarchyPanelTest::TearDown();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		AssetDragData WriteSceneAsset(const str& path, const str& objectName) {
			Handle<Scene> source = SceneSystem::CreateScene("Source");
			SceneSystem::CreateObject(objectName, source);
			EXPECT_TRUE(SceneSystem::SaveScene(source, Path(path)));
			SceneSystem::UnloadScene(source);
			SceneSystem::EndFrame();

			AssetDragData data;
			data.guid = AssetDatabase::LookupGuid(Path(path));
			data.assetTypeHash = StringHash("SceneAsset").GetValue();
			return data;
		}

		static UI::DragPayload AssetPayload(const AssetDragData& data) {
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(data);
			return payload;
		}
	};

} // namespace

TEST_F(HierarchyAssetDropTest, NewSceneHeaderShowsFileName) {
	ASSERT_TRUE(AssetDatabase::CreateAsset(Path("project://Assets/Scenes/New Scene.scene"),
										   StringHash("SceneAsset"))
					.IsValid());

	EditorSelection::Clear();
	UndoSystem::Clear();
	SceneSystem::UnloadAllScenes();
	SceneSystem::EndFrame();
	ASSERT_TRUE(EditorSceneOps::OpenScene(Path("project://Assets/Scenes/New Scene.scene")).IsValid());

	PanelRig rig(PanelDesc());
	UI::ElementHandle sceneRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	UI::ElementHandle label = NextSibling(rig.context, RowGlyph(rig.context, sceneRow));
	ASSERT_TRUE(rig.context.IsAlive(label));
	EXPECT_EQ(rig.context.GetTextString(label), "New Scene");
}

TEST_F(HierarchyAssetDropTest, ComponentAssetDroppedOnRowJoinsObject) {
	static char owner = 0;
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	ASSERT_TRUE(ComponentDrops::Register(StringHash("ProbeScript"), [](const Guid&) { return Light::GetReflectType(); }, &owner));
	AssetDragData data;
	data.guid = Guid{3, 9};
	data.assetTypeHash = StringHash("ProbeScript").GetValue();
	const u32 rootsBefore = _scene->GetRootCount();

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Target");
	ASSERT_TRUE(row.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, row)));
	ASSERT_EQ(target->GetComponentCount(), 1u);
	EXPECT_EQ(target->GetComponentAt(0)->GetType(), Light::GetReflectType());
	EXPECT_EQ(target->GetChildCount(), 0u);
	EXPECT_EQ(_scene->GetRootCount(), rootsBefore);

	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(target->GetComponentCount(), 0u);
	EXPECT_EQ(ComponentDrops::UnregisterOwnedBy(&owner), 1u);
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, row)));
	EXPECT_EQ(target->GetComponentCount(), 0u);
}

TEST_F(HierarchyAssetDropTest, SceneDropLoadsAdditively) {
	AssetDragData data = WriteSceneAsset("project://Assets/second.scene", "Arrival");
	ASSERT_TRUE(data.guid.IsValid());
	u64 primaryBefore = SceneSystem::GetPrimaryScene().GetInstanceId();
	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle sceneRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, sceneRow)));

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	EXPECT_EQ(SceneSystem::GetPrimaryScene().GetInstanceId(), primaryBefore);
	Handle<Scene> added = SceneSystem::GetLoadedScenes()[1];
	EXPECT_TRUE(added->FindObjectByName("Arrival").IsValid());
}

TEST_F(HierarchyAssetDropTest, SceneDropOnFolderFilesNothing) {
	AssetDragData data = WriteSceneAsset("project://Assets/second.scene", "Arrival");
	ASSERT_TRUE(data.guid.IsValid());
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle folderRow = RowNamed(rig.context, rig.root, "Bin");
	ASSERT_TRUE(folderRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, folderRow)));

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	Handle<Scene> added = SceneSystem::GetLoadedScenes()[1];
	Handle<SceneObject> arrival = added->FindObjectByName("Arrival");
	ASSERT_TRUE(arrival.IsValid());
	EXPECT_EQ(SceneAnnotations::GetFolderOf(arrival), 0u);
}

TEST_F(HierarchyAssetDropTest, AnObjectRowRefusesASceneAsset) {
	AssetDragData data = WriteSceneAsset("project://Assets/second.scene", "Arrival");
	ASSERT_TRUE(data.guid.IsValid());
	SceneSystem::CreateObject("Host", _scene);
	Handle<SceneObject> dragged = SceneSystem::CreateObject("Dragged", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle objectRow = RowNamed(rig.context, rig.root, "Host");
	ASSERT_TRUE(objectRow.IsValid());
	Vec2 point = CenterOf(rig.context, objectRow);

	UI::DragPayload objectPayload;
	objectPayload.type = "SceneObject"_hash;
	objectPayload.data = Variant(DraggedObjects({dragged}));
	ASSERT_TRUE(rig.context.DeliverDrop(objectPayload, point));

	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), point));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(HierarchyAssetDropTest, TheInsertionStripRefusesASceneAsset) {
	AssetDragData data = WriteSceneAsset("project://Assets/second.scene", "Arrival");
	ASSERT_TRUE(data.guid.IsValid());
	SceneSystem::CreateObject("Host", _scene);
	Handle<SceneObject> dragged = SceneSystem::CreateObject("Dragged", _scene);

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "Host");
	ASSERT_TRUE(strip.IsValid());
	Vec2 point = CenterOf(rig.context, strip);

	UI::DragPayload objectPayload;
	objectPayload.type = "SceneObject"_hash;
	objectPayload.data = Variant(DraggedObjects({dragged}));
	ASSERT_TRUE(rig.context.DeliverDrop(objectPayload, point));

	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), point));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(HierarchyAssetDropTest, AssetOfAnotherTypeIsRefusedEverywhere) {
	AssetDragData data = WriteSceneAsset("project://Assets/second.scene", "Arrival");
	ASSERT_TRUE(data.guid.IsValid());
	data.assetTypeHash = StringHash("MaterialAsset").GetValue();
	SceneSystem::CreateObject("Host", _scene);
	ASSERT_NE(SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {}), 0u);

	PanelRig rig(PanelDesc());
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0))));
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, RowNamed(rig.context, rig.root, "Bin"))));
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, RowNamed(rig.context, rig.root, "Host"))));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(HierarchyAssetDropTest, SceneAssetWithNoPathIsRefused) {
	AssetDragData data;
	data.guid = Guid{0x1234u, 0x5678u};
	data.assetTypeHash = StringHash("SceneAsset").GetValue();

	PanelRig rig(PanelDesc());
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0))));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

namespace {

	class HierarchyPrefabDropTest : public HierarchyAssetDropTest {
	protected:
		AssetDragData MakePrefabAsset(const str& path = "project://Assets/unit.prefab") {
			Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
			SceneSystem::CreateObject("Body", _scene, unit);
			Guid guid = SceneSystem::SavePrefab(unit, Path(path));
			EXPECT_TRUE(guid.IsValid());
			SceneSystem::Destroy(unit);
			SceneSystem::EndFrame();

			AssetDragData data;
			data.guid = guid;
			data.assetTypeHash = StringHash("PrefabAsset").GetValue();
			return data;
		}

		static bool IsLinkedInstance(Handle<SceneObject> root) {
			return SceneSystem::RevertInstanceOverrides(root);
		}

		AssetDragData StageModelAsset() {
			std::error_code ec;
			std::filesystem::path source;
			std::filesystem::path probe = std::filesystem::current_path(ec);
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				std::filesystem::path candidate = probe / "Source/Tests/Fixtures/Graphics/hierarchy.gltf";
				if (std::filesystem::exists(candidate, ec)) {
					source = candidate;
					break;
				}
				std::filesystem::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			AssetDragData data;
			if (source.empty()) {
				return data;
			}
			std::filesystem::copy_file(source, _projectRoot / "Assets" / "hierarchy.gltf",
									   std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				return data;
			}
			AssetDatabase::ScanProjectDirectory();
			data.guid = AssetDatabase::LookupGuid(Path("project://Assets/hierarchy.gltf"));
			IAssetImporter* importer = AssetImporterRegistry::FindByExtension(".gltf");
			data.assetTypeHash = importer ? importer->GetAssetType().GetValue() : 0;
			return data;
		}
	};

} // namespace

TEST_F(HierarchyPrefabDropTest, PrefabDropMakesChild) {
	AssetDragData data = MakePrefabAsset();
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle hostRow = RowNamed(rig.context, rig.root, "Host");
	ASSERT_TRUE(hostRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, hostRow)));

	ASSERT_EQ(host->GetChildCount(), 1u);
	Handle<SceneObject> instance = host->GetChild(0);
	EXPECT_EQ(instance->name, "Unit");
	EXPECT_TRUE(IsLinkedInstance(instance));

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_EQ(host->GetChildCount(), 0u);

	UndoSystem::Redo();
	ASSERT_EQ(host->GetChildCount(), 1u);
	EXPECT_TRUE(IsLinkedInstance(host->GetChild(0)));
}

TEST_F(HierarchyPrefabDropTest, PrefabDropOnSceneMakesRoot) {
	AssetDragData data = MakePrefabAsset();
	u32 rootsBefore = _scene->GetRootCount();

	PanelRig rig(PanelDesc());
	UI::ElementHandle sceneRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, sceneRow)));

	ASSERT_EQ(_scene->GetRootCount(), rootsBefore + 1);
	Handle<SceneObject> instance = _scene->FindObjectByName("Unit");
	ASSERT_TRUE(instance.IsValid());
	EXPECT_FALSE(instance->GetParent().IsValid());
	EXPECT_TRUE(IsLinkedInstance(instance));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
}

TEST_F(HierarchyPrefabDropTest, DroppingAModelFileInstantiatesItsHierarchy) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}
	EXPECT_EQ(StringHash{data.assetTypeHash}, StringHash("PrefabAsset"));

	PanelRig rig(PanelDesc());
	UI::ElementHandle sceneRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(sceneRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, sceneRow)));

	Handle<SceneObject> instance = _scene->FindObjectByName("hierarchy");
	ASSERT_TRUE(instance.IsValid());
	ASSERT_EQ(instance->GetChildCount(), 4u);
	EXPECT_EQ(instance->GetChild(0)->name, "Body");
	EXPECT_EQ(instance->GetChild(1)->name, "Muzzle");
	EXPECT_TRUE(IsLinkedInstance(instance));

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(_scene->FindObjectByName("hierarchy").IsValid());
}

TEST_F(HierarchyPrefabDropTest, PrefabDropOnFolderFiles) {
	AssetDragData data = MakePrefabAsset();
	u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	UI::ElementHandle folderRow = RowNamed(rig.context, rig.root, "Bin");
	ASSERT_TRUE(folderRow.IsValid());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, folderRow)));

	Handle<SceneObject> instance = _scene->FindObjectByName("Unit");
	ASSERT_TRUE(instance.IsValid());
	EXPECT_FALSE(instance->GetParent().IsValid());
	EXPECT_EQ(SceneAnnotations::GetFolderOf(instance), folder);

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(_scene->FindObjectByName("Unit").IsValid());
}

namespace {

	class HierarchyRegisteredDropTest : public HierarchyAssetDropTest {
	protected:
		static inline char owner = 0;

		void TearDown() override {
			SceneAssetDrops::UnregisterOwnedBy(&owner);
			SceneAssetDrops::Terminate();
			HierarchyAssetDropTest::TearDown();
		}

		static bool Register() {
			return SceneAssetDrops::Register(StringHash("UIDocumentAsset"), StringHash("UIPanelRenderer"), StringHash("_document"), &owner);
		}

		AssetDragData WriteDocument() {
			{
				std::ofstream file(_projectRoot / "Assets" / "Hud.slxml", std::ios::binary);
				file << "<ui version=\"1\">\n\t<panel id=\"root\"/>\n</ui>\n";
			}
			AssetDatabase::ScanProjectDirectory();
			AssetDragData data;
			data.guid = AssetDatabase::LookupGuid(Path("project://Assets/Hud.slxml"));
			IAssetImporter* importer = AssetImporterRegistry::FindByExtension(".slxml");
			data.assetTypeHash = importer ? importer->GetAssetType().GetValue() : 0;
			return data;
		}

		static Guid DocumentOf(Handle<SceneObject> object) {
			Handle<UIPanelRenderer> renderer = object.IsValid() ? object->GetComponent<UIPanelRenderer>() : Handle<UIPanelRenderer>{};
			const UIDocumentAsset* document = renderer.IsValid() ? renderer->GetDocument().Get() : nullptr;
			return document ? document->assetId : Guid{};
		}

		static bool IsActive(Handle<SceneObject> object) {
			return object.IsValid() && EditorSelection::GetActive().GetInstanceId() == object.GetInstanceId();
		}
	};

} // namespace

TEST_F(HierarchyRegisteredDropTest, RegisteredDropMakesPlayingChild) {
	AssetDragData data = WriteDocument();
	ASSERT_TRUE(data.guid.IsValid());
	ASSERT_EQ(StringHash{data.assetTypeHash}, StringHash("UIDocumentAsset"));
	ASSERT_TRUE(Register());
	EXPECT_FALSE(Register());
	Handle<SceneObject> host = SceneSystem::CreateObject("Host", _scene);

	PanelRig rig(PanelDesc());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Host"))));
	ASSERT_EQ(host->GetChildCount(), 1u);
	Handle<SceneObject> made = host->GetChild(0);
	EXPECT_EQ(made->name, "Hud");
	EXPECT_EQ(DocumentOf(made), data.guid);
	EXPECT_TRUE(IsActive(made));

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_EQ(host->GetChildCount(), 0u);
	UndoSystem::Redo();
	ASSERT_EQ(host->GetChildCount(), 1u);
	EXPECT_EQ(DocumentOf(host->GetChild(0)), data.guid);

	EXPECT_EQ(SceneAssetDrops::UnregisterOwnedBy(&owner), 1u);
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Host"))));
}

TEST_F(HierarchyRegisteredDropTest, RegisteredDropRootOrFiled) {
	AssetDragData data = WriteDocument();
	ASSERT_TRUE(data.guid.IsValid());
	ASSERT_TRUE(Register());
	const u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);

	PanelRig rig(PanelDesc());
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0))));
	ASSERT_EQ(_scene->GetRootCount(), 1u);
	Handle<SceneObject> root = _scene->GetRoot(0);
	EXPECT_EQ(DocumentOf(root), data.guid);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(root), 0u);

	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Bin"))));
	ASSERT_EQ(_scene->GetRootCount(), 2u);
	Handle<SceneObject> filed = _scene->GetRoot(1);
	EXPECT_EQ(DocumentOf(filed), data.guid);
	EXPECT_FALSE(filed->GetParent().IsValid());
	EXPECT_EQ(SceneAnnotations::GetFolderOf(filed), folder);
	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_EQ(_scene->GetRootCount(), 1u);
}

namespace {

	struct PrefabIconFace {
		bool mounted = false;
		bool available = false;

		PrefabIconFace() {
			std::filesystem::path engineAssets = Checkout::Find("EngineAssets");
			if (engineAssets.empty()) {
				return;
			}
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			mounted = true;
			EditorIcons::Shutdown();
			available = EditorIcons::IsAvailable(EditorIcons::Face::Solid);
		}
		~PrefabIconFace() {
			if (!mounted) {
				return;
			}
			EditorIcons::Shutdown();
			FileSystem::Unmount("engine");
		}
	};

} // namespace

TEST_F(HierarchyPrefabDropTest, PrefabInstanceRowsWearThePrefabGlyph) {
	PrefabIconFace face;
	if (!face.available) {
		GTEST_SKIP() << "icon font not found";
	}
	AssetDragData data = MakePrefabAsset();
	SceneSystem::CreateObject("Plain", _scene);
	Handle<SceneObject> instance = SceneSystem::Instantiate(data.guid, _scene);
	ASSERT_TRUE(instance.IsValid());

	PanelRig rig(PanelDesc());
	UI::ElementHandle instanceRow = RowNamed(rig.context, rig.root, "Unit");
	UI::ElementHandle plainRow = RowNamed(rig.context, rig.root, "Plain");
	ASSERT_TRUE(instanceRow.IsValid());
	ASSERT_TRUE(plainRow.IsValid());

	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, instanceRow)),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::AssetPrefab));
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, plainRow)),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::SceneObject));
}

TEST_F(HierarchyPrefabDropTest, TreeRepaintsWhenObjectBecomesInstance) {
	PrefabIconFace face;
	if (!face.available) {
		GTEST_SKIP() << "icon font not found";
	}
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	EditorSceneOps::MarkDirty(_scene);

	PanelRig rig(PanelDesc());
	ASSERT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, RowNamed(rig.context, rig.root, "Unit"))),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::SceneObject));

	ASSERT_TRUE(EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Prefabs/Unit.prefab")).IsValid());
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, RowNamed(rig.context, rig.root, "Unit"))),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::AssetPrefab));
}

namespace {

	class HierarchyPrefabSceneTest : public HierarchyPrefabDropTest {
	protected:
		static constexpr const char* PREFAB_PATH = "project://Assets/unit.prefab";

		str PrefabBytes() const {
			return FileSystem::ReadTextFile(FileSystem::Resolve(Path(PREFAB_PATH)), MemTag::Editor);
		}

		bool AnySceneFileExists() const {
			std::error_code ec;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(_projectRoot, ec)) {
				if (entry.is_regular_file(ec) && entry.path().extension() == ".scene") {
					return true;
				}
			}
			return false;
		}

		static UI::DragPayload ObjectPayload(Handle<SceneObject> object) {
			UI::DragPayload payload;
			payload.type = "SceneObject"_hash;
			payload.data = Variant(DraggedObjects({object}));
			return payload;
		}
	};

} // namespace

TEST_F(HierarchyPrefabSceneTest, PrefabSceneSaveApplies) {
	AssetDragData data = MakePrefabAsset();
	Handle<Scene> prefabScene = EditorSceneOps::OpenPrefab(data.guid);
	ASSERT_TRUE(prefabScene.IsValid());
	str before = PrefabBytes();
	ASSERT_FALSE(before.empty());
	prefabScene->GetRoot(0)->FindChildByName("Body")->SetLocalPosition({7.0f, 0.0f, 0.0f});

	PanelRig rig(PanelDesc());
	UI::ElementHandle prefabRow = SceneRowAt(rig.context, rig.root, 0);
	ASSERT_TRUE(prefabRow.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, prefabRow));
	UI::ElementHandle save = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Save Scene");
	ASSERT_TRUE(save.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, save));

	EXPECT_NE(PrefabBytes(), before);
	EXPECT_FALSE(AnySceneFileExists());
}

TEST_F(HierarchyPrefabSceneTest, SaveAsInertOnPrefabScene) {
	AssetDragData data = MakePrefabAsset();
	ASSERT_TRUE(EditorSceneOps::OpenPrefab(data.guid).IsValid());

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(),
					  CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0)));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Save Scene As...");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	EXPECT_FALSE(AnySceneFileExists());
	rig.context.CloseAllPopups();
	rig.context.Update();
	ASSERT_TRUE(EditorSceneOps::LeavePrefabStage());
	SceneSystem::EndFrame();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	RightClickAndTick(rig.context, *rig.panel.Get(),
					  CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0)));
	UI::ElementHandle ordinary = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Save Scene As...");
	ASSERT_TRUE(ordinary.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, ordinary));
	EXPECT_GT(rig.context.GetOpenPopupCount(), 0u);
}

TEST_F(HierarchyPrefabSceneTest, APrefabSceneRefusesADroppedPrefab) {
	AssetDragData data = MakePrefabAsset();
	Handle<Scene> prefabScene = EditorSceneOps::OpenPrefab(data.guid);
	ASSERT_TRUE(prefabScene.IsValid());

	PanelRig rig(PanelDesc());
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0))));
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data),
										 CenterOf(rig.context, RowNamed(rig.context, rig.root, "Unit"))));
	EXPECT_EQ(prefabScene->GetRootCount(), 1u);
	EXPECT_EQ(prefabScene->GetRoot(0)->GetChildCount(), 1u);

	ASSERT_TRUE(EditorSceneOps::LeavePrefabStage());
	SceneSystem::EndFrame();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_TRUE(rig.context.DeliverDrop(AssetPayload(data),
										CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0))));
}

TEST_F(HierarchyPrefabSceneTest, ClickingTheBreadcrumbLeavesTheStage) {
	AssetDragData data = MakePrefabAsset();
	SceneSystem::CreateObject("Outsider", _scene);
	ASSERT_TRUE(EditorSceneOps::OpenPrefab(data.guid).IsValid());

	PanelRig rig(PanelDesc());
	UI::ElementHandle crumb = NthByClass(rig.context, rig.root, EditorTheme::Class::StageBreadcrumb, 0);
	ASSERT_TRUE(crumb.IsValid());
	EXPECT_FALSE(rig.context.HasClass(crumb, EditorTheme::Class::SectionHeader));

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, crumb));
	SceneSystem::EndFrame();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_FALSE(EditorSceneOps::GetPrefabStage().IsValid());
	EXPECT_FALSE(NthByClass(rig.context, rig.root, EditorTheme::Class::StageBreadcrumb, 0).IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Outsider").IsValid());
	EXPECT_EQ(SceneSystem::GetPrimaryScene().GetInstanceId(), _scene.GetInstanceId());
}

TEST_F(HierarchyPrefabSceneTest, StageRefusesOutsideDrag) {
	AssetDragData data = MakePrefabAsset();
	Handle<SceneObject> outsider = SceneSystem::CreateObject("Outsider", _scene);
	Handle<Scene> prefabScene = EditorSceneOps::OpenPrefab(data.guid);
	ASSERT_TRUE(prefabScene.IsValid());
	Handle<SceneObject> prefabRoot = prefabScene->GetRoot(0);
	Handle<SceneObject> prefabBody = prefabRoot->FindChildByName("Body");
	ASSERT_TRUE(prefabBody.IsValid());

	PanelRig rig(PanelDesc());
	rig.BeginDrag();
	EXPECT_FALSE(RowNamed(rig.context, rig.root, "Outsider").IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Unit").IsValid());

	Vec2 prefabRootRow = CenterOf(rig.context, RowNamed(rig.context, rig.root, "Unit"));
	EXPECT_FALSE(rig.context.DeliverDrop(ObjectPayload(outsider), prefabRootRow));
	EXPECT_FALSE(rig.context.DeliverDrop(ObjectPayload(outsider),
										 CenterOf(rig.context, StripBefore(rig.context, rig.root, "Unit"))));
	EXPECT_FALSE(outsider->GetParent().IsValid());
	EXPECT_EQ(outsider->GetScene().GetInstanceId(), _scene.GetInstanceId());

	EXPECT_TRUE(rig.context.DeliverDrop(ObjectPayload(prefabBody), prefabRootRow));

	ASSERT_TRUE(EditorSceneOps::LeavePrefabStage());
	SceneSystem::EndFrame();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Outsider").IsValid());
	EXPECT_FALSE(RowNamed(rig.context, rig.root, "Unit").IsValid());
}

namespace {

	class HierarchyCreatePrefabTest : public HierarchyPrefabDropTest {
	protected:
		static UI::ElementHandle ButtonNamed(UI::Context& context, UI::ElementHandle from, std::string_view label) {
			if (context.HasClass(from, StringHash("slt-button")) && SubtreeSaysAnywhere(context, from, label)) {
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
		static UI::ElementHandle ButtonInAnyPopup(UI::Context& context, std::string_view label) {
			for (u32 i = 0; i < context.GetOpenPopupCount(); ++i) {
				if (UI::ElementHandle found = ButtonNamed(context, context.GetOpenPopup(i), label); found.IsValid()) {
					return found;
				}
			}
			return {};
		}

		static constexpr std::string_view AcceptLabel = "Create (cannot be undone)";
	};

} // namespace

TEST_F(HierarchyCreatePrefabTest, RowMenuPrefabLinksRow) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	SceneSystem::CreateObject("Body", _scene, unit);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Unit");
	ASSERT_TRUE(row.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Create Prefab...");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	UI::ElementHandle accept = ButtonInAnyPopup(rig.context, AcceptLabel);
	ASSERT_TRUE(accept.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, accept));

	EXPECT_TRUE(std::filesystem::exists(_projectRoot / "Assets" / "Prefabs" / "Unit.prefab"));
	EXPECT_TRUE(SceneSystem::IsPrefabLinked(unit));
	EXPECT_TRUE(EditorSceneOps::IsDirty(_scene));
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 0u);
}

TEST_F(HierarchyCreatePrefabTest, CancellingThePromptCreatesNothing) {
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);

	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(),
					  CenterOf(rig.context, RowNamed(rig.context, rig.root, "Unit")));
	ClickAndTick(rig.context, *rig.panel.Get(),
				 CenterOf(rig.context, MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Create Prefab...")));

	UI::ElementHandle cancel = ButtonInAnyPopup(rig.context, "Cancel");
	ASSERT_TRUE(cancel.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, cancel));

	EXPECT_FALSE(std::filesystem::exists(_projectRoot / "Assets" / "Prefabs"));
	EXPECT_FALSE(SceneSystem::IsPrefabLinked(unit));
}

TEST_F(HierarchyCreatePrefabTest, CreatePrefabInertOnInstance) {
	AssetDragData data = MakePrefabAsset();
	Handle<SceneObject> instance = SceneSystem::Instantiate(data.guid, _scene);
	ASSERT_TRUE(instance.IsValid());

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Unit");
	ASSERT_TRUE(row.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Create Prefab...");
	ASSERT_TRUE(entry.IsValid());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));
	EXPECT_FALSE(ButtonInAnyPopup(rig.context, AcceptLabel).IsValid());
	EXPECT_FALSE(std::filesystem::exists(_projectRoot / "Assets" / "Prefabs"));
}

TEST_F(HierarchyPrefabDropTest, TwoDropsMakeTwoIndependentInstances) {
	AssetDragData data = MakePrefabAsset();

	PanelRig rig(PanelDesc());
	Vec2 sceneRowPoint = CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), sceneRowPoint));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), sceneRowPoint));

	ASSERT_EQ(_scene->GetRootCount(), 2u);
	Handle<SceneObject> a = _scene->GetRoot(0);
	Handle<SceneObject> b = _scene->GetRoot(1);
	EXPECT_NE(a.GetInstanceId(), b.GetInstanceId());
	EXPECT_TRUE(IsLinkedInstance(a));
	EXPECT_TRUE(IsLinkedInstance(b));

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_EQ(_scene->GetRootCount(), 1u);
	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_EQ(_scene->GetRootCount(), 0u);
}

TEST_F(HierarchyCreatePrefabTest, SaveAsSeedsPathInsideContentRoot) {
	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(),
					  CenterOf(rig.context, SceneRowAt(rig.context, rig.root, 0)));
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Save Scene As...");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	UI::ElementHandle accept = ButtonInAnyPopup(rig.context, "OK");
	ASSERT_TRUE(accept.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, accept));

	std::error_code ec;
	Vector<str> written{GetMemoryResource(MemTag::Editor)};
	for (const auto& file : std::filesystem::recursive_directory_iterator(_projectRoot, ec)) {
		if (file.is_regular_file(ec) && file.path().extension() == ".scene") {
			written.push_back(str(std::filesystem::relative(file.path(), _projectRoot, ec).generic_string()));
		}
	}
	ASSERT_EQ(written.size(), 1u);
	EXPECT_TRUE(written[0].starts_with("Assets/Scenes/")) << written[0];
}

TEST_F(HierarchyCreatePrefabTest, RowMenuVariantKeepsLink) {
	AssetDragData data = MakePrefabAsset();
	Handle<SceneObject> instance = SceneSystem::Instantiate(data.guid, _scene);
	ASSERT_TRUE(instance.IsValid());
	EditorSceneOps::MarkClean(_scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Unit");
	ASSERT_TRUE(row.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	UI::ElementHandle popup = rig.context.GetOpenPopup(0);
	UI::ElementHandle prefabItem = MenuRowNamed(rig.context, popup, "Create Prefab...");
	ASSERT_TRUE(prefabItem.IsValid());
	EXPECT_FALSE(static_cast<bool>(rig.context.Events(prefabItem).onClick));
	UI::ElementHandle entry = MenuRowNamed(rig.context, popup, "Create Prefab Variant...");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	UI::ElementHandle accept = ButtonInAnyPopup(rig.context, AcceptLabel);
	ASSERT_TRUE(accept.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, accept));

	EXPECT_TRUE(std::filesystem::exists(_projectRoot / "Assets" / "Prefabs" / "Unit.prefab"));
	Guid variant = AssetDatabase::LookupGuid(Path("project://Assets/Prefabs/Unit.prefab"));
	ASSERT_TRUE(variant.IsValid());
	EXPECT_NE(variant, data.guid);
	EXPECT_TRUE(SceneSystem::IsPrefabInstanceRoot(instance));
	EXPECT_TRUE(EditorSceneOps::IsDirty(_scene));
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 0u);
}

TEST_F(HierarchyCreatePrefabTest, VariantItemIsInertOnPlainRow) {
	SceneSystem::CreateObject("Plain", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Plain");
	ASSERT_TRUE(row.IsValid());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, row));
	UI::ElementHandle popup = rig.context.GetOpenPopup(0);
	UI::ElementHandle entry = MenuRowNamed(rig.context, popup, "Create Prefab Variant...");
	ASSERT_TRUE(entry.IsValid());
	EXPECT_FALSE(static_cast<bool>(rig.context.Events(entry).onClick));
	EXPECT_TRUE(static_cast<bool>(rig.context.Events(MenuRowNamed(rig.context, popup, "Create Prefab...")).onClick));

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));
	EXPECT_FALSE(ButtonInAnyPopup(rig.context, AcceptLabel).IsValid());
	EXPECT_FALSE(std::filesystem::exists(_projectRoot / "Assets" / "Prefabs"));
}

TEST_F(HierarchyPrefabSceneTest, StageHidesPrefabGlyph) {
	PrefabIconFace face;
	if (!face.available) {
		GTEST_SKIP() << "icon font not found";
	}
	AssetDragData data = MakePrefabAsset();
	Handle<SceneObject> instance = SceneSystem::Instantiate(data.guid, _scene);
	ASSERT_TRUE(instance.IsValid());
	{
		PanelRig level(PanelDesc());
		EXPECT_EQ(level.context.GetIconAsset(RowGlyph(level.context, RowNamed(level.context, level.root, "Unit"))),
				  EditorIcons::GetGlyph(EditorIcons::Glyph::AssetPrefab));
	}

	ASSERT_TRUE(EditorSceneOps::OpenPrefab(data.guid).IsValid());
	PanelRig rig(PanelDesc());
	UI::ElementHandle staged = RowNamed(rig.context, rig.root, "Unit");
	ASSERT_TRUE(staged.IsValid());
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, staged)),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::SceneObject));

	ASSERT_TRUE(EditorSceneOps::LeavePrefabStage());
	SceneSystem::EndFrame();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_EQ(rig.context.GetIconAsset(RowGlyph(rig.context, RowNamed(rig.context, rig.root, "Unit"))),
			  EditorIcons::GetGlyph(EditorIcons::Glyph::AssetPrefab));
}

TEST_F(HierarchyPanelTest, ShiftClickSpansFromAnchor) {
	SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	SceneSystem::CreateObject("D", _scene);
	PanelRig rig(PanelDesc());
	ASSERT_TRUE(rig.panel.IsValid());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")));
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 1u);

	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "C")),
					KeyMod::Shift);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 3u);
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), c.GetInstanceId());
}

TEST_F(HierarchyPanelTest, ShiftClickSpansSameRunDraggedUpwards) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	SceneSystem::CreateObject("C", _scene);
	PanelRig rig(PanelDesc());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "C")));
	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")),
					KeyMod::Shift);

	EXPECT_EQ(EditorSelection::GetSelectionCount(), 3u);
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), a.GetInstanceId());
}

TEST_F(HierarchyPanelTest, SecondShiftClickRespansFromAnchor) {
	SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	SceneSystem::CreateObject("C", _scene);
	Handle<SceneObject> d = SceneSystem::CreateObject("D", _scene);
	PanelRig rig(PanelDesc());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")));
	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "D")),
					KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 4u);

	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "B")),
					KeyMod::Shift);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(d.Get())));
}

TEST_F(HierarchyPanelTest, ShiftRangeSkipsFoldedRows) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene);
	hidden.Get()->SetParent(a);
	SceneSystem::CreateObject("Z", _scene);
	PanelRig rig(PanelDesc());

	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")));
	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Z")),
					KeyMod::Shift);

	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(hidden.Get())));
}

TEST_F(HierarchyPanelTest, CtrlWinsOverShift) {
	SceneSystem::CreateObject("A", _scene);
	SceneSystem::CreateObject("B", _scene);
	SceneSystem::CreateObject("C", _scene);
	PanelRig rig(PanelDesc());

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")));
	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "C")),
					KeyMod::Ctrl | KeyMod::Shift);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);

	ModClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "A")),
					KeyMod::Ctrl);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 1u);
}

TEST_F(HierarchyPanelTest, SceneLessObjectDrawnQuiet) {
	Handle<SceneObject> ship = SceneSystem::CreateObject("Ship", _scene);
	Handle<SceneObject> engine = SceneSystem::CreateObject("Engine", _scene);
	engine.Get()->SetParent(ship);

	PanelRig rig(PanelDesc());

	UI::ElementHandle caret =
		FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Ship"), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, caret));

	auto quietParts = [&](std::string_view name) {
		UI::ElementHandle row = RowNamed(rig.context, rig.root, name);
		EXPECT_TRUE(row.IsValid()) << name;
		return CountByClass(rig.context, row, EditorTheme::Class::RowOff);
	};
	auto quietestOpacity = [&](std::string_view name) {
		UI::ElementHandle row = RowNamed(rig.context, rig.root, name);
		UI::ElementHandle part = FindByClass(rig.context, row, EditorTheme::Class::RowOff, 0);
		return rig.context.IsAlive(part) ? rig.context.GetComputedStyle(part).opacity : 1.0f;
	};

	ASSERT_EQ(quietParts("Ship"), 0u);
	ASSERT_EQ(quietParts("Engine"), 0u);

	ship.Get()->SetActive(false);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_EQ(quietParts("Ship"), 2u);
	EXPECT_EQ(quietParts("Engine"), 2u);
	EXPECT_FALSE(rig.context.HasClass(RowNamed(rig.context, rig.root, "Ship"), EditorTheme::Class::RowOff));
	EXPECT_LT(quietestOpacity("Ship"), 1.0f);
	EXPECT_GT(quietestOpacity("Ship"), 0.0f);

	ship.Get()->SetActive(true);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_EQ(quietParts("Ship"), 0u);
	EXPECT_EQ(quietParts("Engine"), 0u);
}

namespace {

	StringHash RebuildMark() { return StringHash("m45-rebuild-mark"); }

	void MarkRow(UI::Context& context, UI::ElementHandle row) { context.AddClass(row, RebuildMark()); }

	bool RowSurvived(UI::Context& context, UI::ElementHandle row) {
		return context.IsAlive(row) && context.HasClass(row, RebuildMark());
	}

} // namespace

TEST_F(HierarchyPanelTest, ChangeInsideCollapsedBranchRebuildsNothing) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	SceneSystem::CreateObject("Sibling", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle watched = RowNamed(rig.context, rig.root, "Sibling");
	ASSERT_TRUE(watched.IsValid());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());
	MarkRow(rig.context, watched);

	hidden.Get()->name = "Renamed Under A Fold";
	hidden.Get()->SetActive(false);
	SceneSystem::CreateObject("Also Hidden", _scene, hidden);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(RowSurvived(rig.context, watched));

	parent.Get()->name = "Renamed On Screen";
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_FALSE(RowSurvived(rig.context, watched));
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Renamed On Screen").IsValid());
}

TEST_F(HierarchyPanelTest, ReorderAmongSceneRootsRebuildsRows) {
	SceneSystem::CreateObject("First", _scene);
	SceneSystem::CreateObject("Second", _scene);
	SceneSystem::CreateObject("Third", _scene);

	PanelRig rig(PanelDesc());
	ASSERT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 0), "First"));
	ASSERT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 2), "Third"));

	_scene.Get()->GetRoot(2).Get()->SetSiblingIndex(0);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 0), "Third"));
	EXPECT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 1), "First"));
}

TEST_F(HierarchyPanelTest, ReorderAmongOpenRowsChildrenRebuildsRows) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Alpha", _scene, parent);
	SceneSystem::CreateObject("Beta", _scene, parent);

	PanelRig rig(PanelDesc());
	UI::ElementHandle caret =
		FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, caret));
	ASSERT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 1), "Alpha"));

	parent.Get()->GetChild(1).Get()->SetSiblingIndex(0);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 1), "Beta"));
	EXPECT_TRUE(SubtreeSaysAnywhere(rig.context, RowAt(rig.context, rig.root, 2), "Alpha"));
}

TEST_F(HierarchyPanelTest, ChildAddedUnderOpenRowAppears) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("First Child", _scene, parent);

	PanelRig rig(PanelDesc());
	UI::ElementHandle caret =
		FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, caret));
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "First Child").IsValid());

	SceneSystem::CreateObject("Second Child", _scene, parent);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Second Child").IsValid());
}

TEST_F(HierarchyPanelTest, ChildGivesClosedRowCaret) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);

	PanelRig rig(PanelDesc());
	auto caretOf = [&rig]() {
		return FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"), StringHash("slt-tree-arrow"));
	};
	ASSERT_FALSE(rig.context.HasIcon(caretOf()));

	SceneSystem::CreateObject("Newcomer", _scene, parent);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	ASSERT_EQ(rig.context.GetIconAsset(caretOf()), EditorIcons::GetGlyph(EditorIcons::Glyph::CaretRight));

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, caretOf()));
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Newcomer").IsValid());
}

TEST_F(HierarchyPanelTest, DestroyedRowLeavesTreeBeforeEndFrame) {
	SceneSystem::CreateObject("Keeper", _scene);
	Handle<SceneObject> doomed = SceneSystem::CreateObject("Doomed", _scene);

	PanelRig rig(PanelDesc());
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "Doomed").IsValid());

	SceneSystem::Destroy(doomed);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_FALSE(RowNamed(rig.context, rig.root, "Doomed").IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Keeper").IsValid());
	SceneSystem::EndFrame();
}

TEST_F(HierarchyPanelTest, TickAfterRebuildRebuildsNothingMore) {
	SceneSystem::CreateObject("First", _scene);

	PanelRig rig(PanelDesc());

	SceneSystem::CreateObject("Second", _scene);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	UI::ElementHandle rebuilt = RowNamed(rig.context, rig.root, "Second");
	ASSERT_TRUE(rebuilt.IsValid());
	MarkRow(rig.context, rebuilt);

	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(RowSurvived(rig.context, rebuilt));
}

TEST_F(HierarchyPanelTest, ObjectRenamedIntoFilterMatchAppears) {
	SceneSystem::CreateObject("Alpha", _scene);
	Handle<SceneObject> other = SceneSystem::CreateObject("Zulu", _scene);

	PanelRig rig(PanelDesc());
	UI::ElementHandle magnifier = FindByClass(rig.context, rig.root, EditorTheme::Class::ToolButton, 1);
	ASSERT_TRUE(magnifier.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, magnifier));
	UI::ElementHandle search = FirstByClass(rig.context, rig.root, StringHash("slt-textfield"));
	ASSERT_TRUE(search.IsValid());
	rig.context.SetFocus(search);
	for (char letter : std::string_view("Alph")) {
		InputEvent typed{};
		typed.type = InputEventType::Text;
		typed.codepoint = static_cast<u32>(letter);
		rig.context.InjectInput(typed);
	}
	rig.context.Update();
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ASSERT_TRUE(RowNamed(rig.context, rig.root, "Alpha").IsValid());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Zulu").IsValid());

	other.Get()->name = "Alphabet";
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Alphabet").IsValid());
}

TEST_F(HierarchyPanelTest, HierarchiesKeepSeparateFolds) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	SceneSystem::CreateObject("Sibling", _scene);

	PanelRig open(PanelDesc());
	PanelRig closed(PanelDesc());
	ASSERT_NE(open.panel.GetInstanceId(), closed.panel.GetInstanceId());

	UI::ElementHandle caret =
		FirstByClass(open.context, RowNamed(open.context, open.root, "Parent"), StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	ClickAndTick(open.context, *open.panel.Get(), CenterOf(open.context, caret));
	ASSERT_TRUE(RowNamed(open.context, open.root, "Hidden").IsValid());
	ASSERT_FALSE(RowNamed(closed.context, closed.root, "Hidden").IsValid());

	UI::ElementHandle watchedOpen = RowNamed(open.context, open.root, "Hidden");
	UI::ElementHandle watchedClosed = RowNamed(closed.context, closed.root, "Sibling");
	MarkRow(open.context, watchedOpen);
	MarkRow(closed.context, watchedClosed);

	hidden.Get()->name = "Renamed";
	open.panel.Get()->OnTick(0.016f);
	open.context.Update();
	closed.panel.Get()->OnTick(0.016f);
	closed.context.Update();

	EXPECT_FALSE(RowSurvived(open.context, watchedOpen));
	EXPECT_TRUE(RowNamed(open.context, open.root, "Renamed").IsValid());
	EXPECT_TRUE(RowSurvived(closed.context, watchedClosed));
}

TEST_F(HierarchyPanelTest, RenameFieldDiesWithObject) {
	SceneSystem::CreateObject("Keeper", _scene);
	Handle<SceneObject> doomed = SceneSystem::CreateObject("Doomed", _scene);

	PanelRig rig(PanelDesc());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Doomed")));
	ASSERT_TRUE(rig.panel.Get()->OnCommand(PanelCommand::Rename));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Doomed").IsValid());
	ASSERT_TRUE(FirstByClass(rig.context, rig.root, StringHash("slt-textfield")).IsValid());

	SceneSystem::Destroy(doomed);
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_FALSE(FirstByClass(rig.context, rig.root, StringHash("slt-textfield")).IsValid());
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "Keeper").IsValid());
	SceneSystem::EndFrame();
}

TEST_F(HierarchyPanelTest, DisclosureAndSelectionSurvivePlayStopCycle) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene);
	child.Get()->SetParent(parent);

	PanelRig rig(PanelDesc());
	ASSERT_TRUE(rig.panel.IsValid());
	ASSERT_EQ(CountObjectRows(rig.context, rig.root), 1u);

	UI::ElementHandle caret = FirstByClass(rig.context, RowNamed(rig.context, rig.root, "Parent"),
										   StringHash("slt-tree-arrow"));
	ASSERT_TRUE(caret.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, caret));
	ASSERT_EQ(CountObjectRows(rig.context, rig.root), 2u);

	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Child")));
	Hover(rig.context, AwayFromRows);
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 1u);

	PlayModeController playMode;
	playMode.RequestToggle();
	playMode.PumpMainThread();
	ASSERT_TRUE(playMode.IsPlaying());
	playMode.RequestToggle();
	playMode.PumpMainThread();
	ASSERT_FALSE(playMode.IsPlaying());

	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();

	EXPECT_EQ(CountObjectRows(rig.context, rig.root), 2u);
	UI::ElementHandle restored = RowNamed(rig.context, rig.root, "Child");
	ASSERT_TRUE(restored.IsValid());
	ASSERT_EQ(EditorSelection::GetSelectionCount(), 1u);
	EXPECT_TRUE(rig.context.HasClass(restored, StringHash("slt-selected")));
	SceneSystem::EndFrame();
}

TEST_F(HierarchyPanelTest, CreateChildOffersFullList) {
	SceneSystem::CreateObject("Target", _scene);
	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "Target")));

	UI::ElementHandle popup = rig.context.GetOpenPopup(0);
	ASSERT_TRUE(popup.IsValid());
	EXPECT_TRUE(MenuRowNamed(rig.context, popup, "Create Child").IsValid());
	EXPECT_FALSE(MenuRowNamed(rig.context, popup, "Create Empty Child").IsValid());
}

TEST_F(HierarchyPanelTest, EmptySpaceMenuOffersAllKinds) {
	PanelRig rig(PanelDesc());
	RightClickAndTick(rig.context, *rig.panel.Get(), AwayFromRows);

	UI::ElementHandle popup = rig.context.GetOpenPopup(0);
	ASSERT_TRUE(popup.IsValid());
	EXPECT_TRUE(MenuRowNamed(rig.context, popup, "Create Empty").IsValid());
	EXPECT_TRUE(MenuRowNamed(rig.context, popup, "3D Object").IsValid());
	EXPECT_TRUE(MenuRowNamed(rig.context, popup, "Camera").IsValid());
	EXPECT_TRUE(MenuRowNamed(rig.context, popup, "Light").IsValid());
}

TEST_F(HierarchyPanelTest, DecalCreateAddsDecal) {
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, {}, items);
	const UI::Widgets::MenuItem* decal = nullptr;
	for (const UI::Widgets::MenuItem& item : items) {
		if (item.label == "Decal") {
			decal = &item;
		}
	}
	ASSERT_NE(decal, nullptr);
	decal->onSelect();

	Handle<SceneObject> created(static_cast<SceneObject*>(EditorSelection::GetActive().Get()));
	ASSERT_TRUE(created.IsValid());
	EXPECT_EQ(created.Get()->name, "Decal");
	ASSERT_EQ(created.Get()->GetComponentCount(), 1u);
	EXPECT_EQ(created.Get()->GetComponentAt(0).Get()->GetType()->name.GetValue(), StringHash("Decal").GetValue());

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(created.IsValid());
}

TEST_F(HierarchyPanelTest, CameraCreateIsOneUndo) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);

	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, parent, items);
	const UI::Widgets::MenuItem* camera = nullptr;
	for (const UI::Widgets::MenuItem& item : items) {
		if (item.label == "Camera") {
			camera = &item;
		}
	}
	ASSERT_NE(camera, nullptr);
	ASSERT_TRUE(static_cast<bool>(camera->onSelect));
	camera->onSelect();

	ASSERT_EQ(parent.Get()->GetChildCount(), 1u);
	Handle<SceneObject> created = parent.Get()->GetChild(0);
	ASSERT_TRUE(created.IsValid());
	EXPECT_EQ(created.Get()->name, "Camera");
	ASSERT_EQ(created.Get()->GetComponentCount(), 1u);
	EXPECT_EQ(created.Get()->GetComponentAt(0).Get()->GetType()->name.GetValue(), StringHash("Camera").GetValue());
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), created.GetInstanceId());

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	EXPECT_FALSE(created.IsValid());
	EXPECT_TRUE(parent.IsValid());
	EXPECT_FALSE(UndoSystem::CanUndo());
}

TEST_F(HierarchyPanelTest, PrimitiveSubmenuFromEnum) {
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, Handle<SceneObject>{}, items);
	const UI::Widgets::MenuItem* meshes = nullptr;
	for (const UI::Widgets::MenuItem& item : items) {
		if (item.label == "3D Object") {
			meshes = &item;
		}
	}
	ASSERT_NE(meshes, nullptr);
	ASSERT_EQ(meshes->children.size(), static_cast<size_t>(PrimitiveShape::Count));
	for (u8 i = 0; i < static_cast<u8>(PrimitiveShape::Count); ++i) {
		EXPECT_EQ(meshes->children[i].label, str(PrimitiveShapeName(static_cast<PrimitiveShape>(i))));
	}
}

TEST_F(HierarchyPanelTest, LightSubmenuSetsTypeAndShadow) {
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, Handle<SceneObject>{}, items);
	const UI::Widgets::MenuItem* lights = nullptr;
	for (const UI::Widgets::MenuItem& item : items) {
		if (item.label == "Light") {
			lights = &item;
		}
	}
	ASSERT_NE(lights, nullptr);
	ASSERT_EQ(lights->children.size(), 3u);
	EXPECT_EQ(lights->children[0].label, "Directional Light");
	EXPECT_EQ(lights->children[1].label, "Point Light");
	EXPECT_EQ(lights->children[2].label, "Spot Light");

	const std::array<LightType, 3> expectedTypes{LightType::Directional, LightType::Point, LightType::Spot};
	for (size_t i = 0; i < lights->children.size(); ++i) {
		ASSERT_TRUE(static_cast<bool>(lights->children[i].onSelect)) << "entry " << i << " does nothing";
		lights->children[i].onSelect();

		Handle<SceneObject> created(static_cast<SceneObject*>(EditorSelection::GetActive().Get()));
		ASSERT_TRUE(created.IsValid()) << "entry " << i;
		Handle<Light> light = created.Get()->GetComponent<Light>();
		ASSERT_TRUE(light.IsValid()) << "entry " << i;
		EXPECT_EQ(light.Get()->GetLightType(), expectedTypes[i]) << "entry " << i;
		EXPECT_EQ(light.Get()->GetCastShadows(), expectedTypes[i] == LightType::Directional) << "entry " << i << " (" << lights->children[i].label;
	}
}

TEST_F(HierarchyPanelTest, LightRedoRestoresTypeAndShadow) {
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::BuildCreateMenu(_scene, Handle<SceneObject>{}, items);
	const UI::Widgets::MenuItem* lights = nullptr;
	for (const UI::Widgets::MenuItem& item : items) {
		if (item.label == "Light") {
			lights = &item;
		}
	}
	ASSERT_NE(lights, nullptr);
	lights->children[2].onSelect();
	Handle<SceneObject> created(static_cast<SceneObject*>(EditorSelection::GetActive().Get()));
	ASSERT_TRUE(created.IsValid());

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	ASSERT_FALSE(created.IsValid());

	UndoSystem::Redo();
	SceneSystem::EndFrame();
	ASSERT_EQ(_scene.Get()->GetRootCount(), 1u);
	Handle<SceneObject> rebuilt = _scene.Get()->GetRoot(0);
	ASSERT_TRUE(rebuilt.IsValid());
	Handle<Light> light = rebuilt.Get()->GetComponent<Light>();
	ASSERT_TRUE(light.IsValid());
	EXPECT_EQ(light.Get()->GetLightType(), LightType::Spot);
	EXPECT_FALSE(light.Get()->GetCastShadows());
}

TEST_F(HierarchyPanelTest, PingUnfoldsAndMarks) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	Handle<SceneObject> other = SceneSystem::CreateObject("Other", _scene);
	EditorSelection::Select(Handle<Object>(other.Get()));
	PanelRig rig(PanelDesc());
	ASSERT_FALSE(RowNamed(rig.context, rig.root, "Hidden").IsValid());

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.object = hidden.GetInstanceId()}});
	PanelManager::TickAll(0.016f);
	rig.context.Update();

	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Hidden");
	ASSERT_TRUE(row.IsValid());
	EXPECT_TRUE(rig.context.HasClass(row, EditorTheme::Class::RowPing));
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(other.Get())));
	EXPECT_FALSE(EditorSelection::IsSelected(Handle<Object>(hidden.Get())));

	for (u32 frame = 0; frame < 8; ++frame) {
		PanelManager::TickAll(0.25f);
		rig.context.Update();
	}
	EXPECT_TRUE(rig.context.IsAlive(row));
	EXPECT_FALSE(rig.context.HasClass(row, EditorTheme::Class::RowPing));
}

TEST_F(HierarchyPanelTest, ComponentPingMarksObject) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	Handle<HierTestComponent> component = hidden.Get()->AddComponent<HierTestComponent>();
	ASSERT_NE(component.Get(), nullptr);
	PanelRig rig(PanelDesc());

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.object = component.GetInstanceId()}});
	PanelManager::TickAll(0.016f);
	rig.context.Update();

	UI::ElementHandle row = RowNamed(rig.context, rig.root, "Hidden");
	ASSERT_TRUE(row.IsValid());
	EXPECT_TRUE(rig.context.HasClass(row, EditorTheme::Class::RowPing));
}

TEST_F(HierarchyPanelTest, UnshownPingMovesNothing) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	SceneSystem::CreateObject("Hidden", _scene, parent);
	PanelRig rig(PanelDesc());
	const u32 rows = CountObjectRows(rig.context, rig.root);

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.object = 0xDEADBEEFull}});
	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = Guid{0xDEAD, 0xBEEF}}});
	PanelManager::TickAll(0.016f);
	rig.context.Update();

	EXPECT_EQ(CountObjectRows(rig.context, rig.root), rows);
	EXPECT_FALSE(FirstByClass(rig.context, rig.root, EditorTheme::Class::RowPing).IsValid());
}

TEST_F(HierarchyPanelTest, BothOpenHierarchiesAnswerOnePing) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> hidden = SceneSystem::CreateObject("Hidden", _scene, parent);
	PanelRig first(PanelDesc());
	PanelRig second(PanelDesc());

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.object = hidden.GetInstanceId()}});
	PanelManager::TickAll(0.016f);
	first.context.Update();
	second.context.Update();

	for (PanelRig* rig : {&first, &second}) {
		UI::ElementHandle row = RowNamed(rig->context, rig->root, "Hidden");
		ASSERT_TRUE(row.IsValid());
		EXPECT_TRUE(rig->context.HasClass(row, EditorTheme::Class::RowPing));
	}
}

namespace {

	class HierarchyMultiDragTest : public HierarchyPanelTest {
	protected:
		static UI::DragPayload GrabRow(PanelRig& rig, std::string_view label) {
			UI::ElementHandle row = RowNamed(rig.context, rig.root, label);
			EXPECT_TRUE(row.IsValid()) << label;
			const Vec2 point = CenterOf(rig.context, row);
			UISystem::Initialize();
			rig.context.InjectInput(Mouse(InputEventType::MouseMove, point));
			rig.context.InjectInput(Mouse(InputEventType::MouseDown, point));
			rig.context.Update();
			rig.context.InjectInput(Mouse(InputEventType::MouseMove, {point.x + 12.0f, point.y + 4.0f}));
			rig.context.Update();
			const UI::DragPayload* began = UISystem::GetDragPayload();
			const UI::DragPayload payload = began ? *began : UI::DragPayload{};
			UISystem::CancelDragPayload();
			UISystem::Terminate();
			rig.context.InjectInput(Mouse(InputEventType::MouseMove, AwayFromRows));
			rig.context.InjectInput(Mouse(InputEventType::MouseUp, AwayFromRows));
			rig.context.Update();
			return payload;
		}

		static Vector<u64> Carried(const UI::DragPayload& payload) {
			Vector<u64> ids{GetMemoryResource(MemTag::Editor)};
			if (payload.type == "SceneObject"_hash && payload.data.Is<SceneObjectDragData>()) {
				const Vector<u64>& objects = payload.data.As<SceneObjectDragData>().objects;
				ids.assign(objects.begin(), objects.end());
			}
			return ids;
		}

		static void SelectObjects(std::initializer_list<Handle<SceneObject>> objects) {
			EditorSelection::Clear();
			for (Handle<SceneObject> object : objects) {
				EditorSelection::AddToSelection(Handle<Object>(object.Get()));
			}
		}

		Vector<Handle<SceneObject>> Roots() const {
			Vector<Handle<SceneObject>> roots{GetMemoryResource(MemTag::Editor)};
			for (u32 i = 0; i < _scene.Get()->GetRootCount(); ++i) {
				roots.push_back(_scene.Get()->GetRoot(i));
			}
			return roots;
		}
	};

} // namespace

TEST_F(HierarchyMultiDragTest, SelectionDragKeepsTreeOrder) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> nested = SceneSystem::CreateObject("Nested", _scene, a);
	SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	SelectObjects({c, nested});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "C");
	const Vector<u64> carried = Carried(payload);
	ASSERT_EQ(carried.size(), 2u);
	EXPECT_EQ(carried[0], nested.GetInstanceId());
	EXPECT_EQ(carried[1], c.GetInstanceId());
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 2u);
}

TEST_F(HierarchyMultiDragTest, RowGrabbedOutsideSelectionCarriesOnlyItself) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	SelectObjects({a, b});
	PanelRig rig(PanelDesc());

	const Vector<u64> carried = Carried(GrabRow(rig, "C"));
	ASSERT_EQ(carried.size(), 1u);
	EXPECT_EQ(carried[0], c.GetInstanceId());
}

TEST_F(HierarchyMultiDragTest, SelectionDropIsOneUndo) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	SelectObjects({c, a});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "A");
	const u32 depth = UndoSystem::GetUndoDepth();
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, RowNamed(rig.context, rig.root, "Target"))));

	ASSERT_EQ(target.Get()->GetChildCount(), 2u);
	EXPECT_EQ(target.Get()->GetChild(0), a);
	EXPECT_EQ(target.Get()->GetChild(1), c);
	EXPECT_FALSE(b.Get()->GetParent().IsValid());
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth + 1);

	UndoSystem::Undo();
	EXPECT_EQ(target.Get()->GetChildCount(), 0u);
	const Vector<Handle<SceneObject>> roots = Roots();
	ASSERT_EQ(roots.size(), 4u);
	EXPECT_EQ(roots[0], a);
	EXPECT_EQ(roots[1], b);
	EXPECT_EQ(roots[2], c);
	EXPECT_EQ(roots[3], target);
}

TEST_F(HierarchyMultiDragTest, ChildDraggedWithParentStaysUnderIt) {
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", _scene);
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", _scene, parent);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	SelectObjects({parent, child});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "Parent");
	const Vector<u64> carried = Carried(payload);
	ASSERT_EQ(carried.size(), 1u);
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, RowNamed(rig.context, rig.root, "Target"))));

	EXPECT_EQ(parent.Get()->GetParent(), target);
	EXPECT_EQ(child.Get()->GetParent(), parent);
}

TEST_F(HierarchyMultiDragTest, SelectionDropAllOrNothing) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> target = SceneSystem::CreateObject("Target", _scene);
	SelectObjects({a, target});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "A");
	ASSERT_EQ(Carried(payload).size(), 2u);
	const u32 depth = UndoSystem::GetUndoDepth();
	EXPECT_FALSE(rig.context.DeliverDrop(payload, CenterOf(rig.context, RowNamed(rig.context, rig.root, "Target"))));
	EXPECT_FALSE(a.Get()->GetParent().IsValid());
	EXPECT_FALSE(target.Get()->GetParent().IsValid());
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth);
}

TEST_F(HierarchyMultiDragTest, SelectionDropBetweenRows) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	Handle<SceneObject> d = SceneSystem::CreateObject("D", _scene);
	SelectObjects({a, c});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "C");
	rig.BeginDrag();
	UI::ElementHandle strip = StripBefore(rig.context, rig.root, "D");
	ASSERT_TRUE(strip.IsValid());
	const u32 depth = UndoSystem::GetUndoDepth();
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, strip)));

	const Vector<Handle<SceneObject>> roots = Roots();
	ASSERT_EQ(roots.size(), 4u);
	EXPECT_EQ(roots[0], b);
	EXPECT_EQ(roots[1], a);
	EXPECT_EQ(roots[2], c);
	EXPECT_EQ(roots[3], d);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth + 1);
}

TEST_F(HierarchyMultiDragTest, SelectionDropOnFolderFilesAll) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	const u64 folder = SceneAnnotations::CreateFolder(_scene, "Bin", FolderParent::Roots(), {});
	ASSERT_NE(folder, 0u);
	SelectObjects({a, c});
	PanelRig rig(PanelDesc());

	const UI::DragPayload payload = GrabRow(rig, "A");
	ASSERT_TRUE(rig.context.DeliverDrop(payload, CenterOf(rig.context, RowNamed(rig.context, rig.root, "Bin"))));
	EXPECT_EQ(SceneAnnotations::GetFolderOf(a), folder);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(c), folder);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(b), 0u);

	UndoSystem::Undo();
	EXPECT_EQ(SceneAnnotations::GetFolderOf(a), 0u);
	EXPECT_EQ(SceneAnnotations::GetFolderOf(c), 0u);
}

TEST_F(HierarchyMultiDragTest, MixedParentsLandTogether) {
	Handle<SceneObject> x = SceneSystem::CreateObject("X", _scene);
	Handle<SceneObject> x1 = SceneSystem::CreateObject("X1", _scene, x);
	Handle<SceneObject> x2 = SceneSystem::CreateObject("X2", _scene, x);
	Handle<SceneObject> y = SceneSystem::CreateObject("Y", _scene);
	Handle<SceneObject> y1 = SceneSystem::CreateObject("Y1", _scene, y);
	Handle<SceneObject> z = SceneSystem::CreateObject("Z", _scene);
	Handle<SceneObject> z1 = SceneSystem::CreateObject("Z1", _scene, z);
	Handle<SceneObject> z2 = SceneSystem::CreateObject("Z2", _scene, z);

	const Handle<SceneObject> moving[] = {y1, x2};
	ASSERT_TRUE(EditorSceneOps::MoveObjects({moving, 2}, z, {}, 1));
	ASSERT_EQ(z.Get()->GetChildCount(), 4u);
	EXPECT_EQ(z.Get()->GetChild(0), z1);
	EXPECT_EQ(z.Get()->GetChild(1), x2);
	EXPECT_EQ(z.Get()->GetChild(2), y1);
	EXPECT_EQ(z.Get()->GetChild(3), z2);
	EXPECT_EQ(x.Get()->GetChildCount(), 1u);
	EXPECT_EQ(x.Get()->GetChild(0), x1);
	EXPECT_EQ(y.Get()->GetChildCount(), 0u);
}

namespace {

	class HierarchyEmptyParentTest : public HierarchyMultiDragTest {
	protected:
		Vector<Handle<SceneObject>> LiveRoots() const {
			Vector<Handle<SceneObject>> live{GetMemoryResource(MemTag::Editor)};
			for (Handle<SceneObject> root : Roots()) {
				if (root.Get() && !root.Get()->IsPendingDestroy()) {
					live.push_back(root);
				}
			}
			return live;
		}
	};

} // namespace

TEST_F(HierarchyEmptyParentTest, EmptyParentWrapsSelection) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> c = SceneSystem::CreateObject("C", _scene);
	Handle<SceneObject> d = SceneSystem::CreateObject("D", _scene);
	a.Get()->SetWorldPosition(Vec3(1.0f, 0.0f, 0.0f));
	c.Get()->SetWorldPosition(Vec3(3.0f, 0.0f, 0.0f));
	EditorSelection::Select(Handle<Object>(c.Get()));
	EditorSelection::AddToSelection(Handle<Object>(a.Get()));
	ASSERT_EQ(EditorSelection::GetActive().GetInstanceId(), a.GetInstanceId());
	PanelRig rig(PanelDesc());

	RightClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, RowNamed(rig.context, rig.root, "C")));
	ASSERT_EQ(rig.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle entry = MenuRowNamed(rig.context, rig.context.GetOpenPopup(0), "Create Empty Parent");
	ASSERT_TRUE(entry.IsValid());
	ClickAndTick(rig.context, *rig.panel.Get(), CenterOf(rig.context, entry));

	Vector<Handle<SceneObject>> roots = LiveRoots();
	ASSERT_EQ(roots.size(), 3u);
	Handle<SceneObject> parent = roots[0];
	EXPECT_EQ(roots[1], b);
	EXPECT_EQ(roots[2], d);
	ASSERT_EQ(parent.Get()->GetChildCount(), 2u);
	EXPECT_EQ(parent.Get()->GetChild(0), a);
	EXPECT_EQ(parent.Get()->GetChild(1), c);
	EXPECT_FLOAT_EQ(parent.Get()->GetWorldPosition().x, 1.0f);
	EXPECT_FLOAT_EQ(a.Get()->GetWorldPosition().x, 1.0f);
	EXPECT_FLOAT_EQ(c.Get()->GetWorldPosition().x, 3.0f);
	EXPECT_TRUE(EditorSelection::IsSelected(Handle<Object>(parent.Get())));
	rig.panel.Get()->OnTick(0.016f);
	rig.context.Update();
	EXPECT_TRUE(RowNamed(rig.context, rig.root, "A").IsValid());

	UndoSystem::Undo();
	SceneSystem::EndFrame();
	roots = LiveRoots();
	ASSERT_EQ(roots.size(), 4u);
	EXPECT_EQ(roots[0], a);
	EXPECT_EQ(roots[1], b);
	EXPECT_EQ(roots[2], c);
	EXPECT_EQ(roots[3], d);
	EXPECT_FLOAT_EQ(a.Get()->GetWorldPosition().x, 1.0f);

	UndoSystem::Redo();
	roots = LiveRoots();
	ASSERT_EQ(roots.size(), 3u);
	ASSERT_EQ(roots[0].Get()->GetChildCount(), 2u);
	EXPECT_EQ(a.Get()->GetParent(), roots[0]);
	EXPECT_EQ(c.Get()->GetParent(), roots[0]);
}

TEST_F(HierarchyEmptyParentTest, MixedParentsGetNoParent) {
	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> nested = SceneSystem::CreateObject("Nested", _scene, b);
	const Handle<SceneObject> mixed[] = {a, nested};
	EXPECT_FALSE(EditorSceneOps::CanCreateEmptyParent({mixed, 2}));
	EXPECT_FALSE(EditorSceneOps::CreateEmptyParent({mixed, 2}, a).IsValid());
	const Handle<SceneObject> siblings[] = {a, b};
	EXPECT_TRUE(EditorSceneOps::CanCreateEmptyParent({siblings, 2}));
	EXPECT_FALSE(EditorSceneOps::CanCreateEmptyParent({}));
}
