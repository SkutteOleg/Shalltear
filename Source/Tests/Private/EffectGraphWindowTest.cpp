#include "TestsPCH.h"

#ifdef SHALLTEAR_SHARED

#include "ShadingHarness.h"
#include "UITestUtil.h"

#include "EffectGraphEditOps.h"
#include "GraphCurves.h"
#include "GraphCustomHLSL.h"
#include "GraphDefinitions.h"
#include "GraphEffect.h"
#include "GraphWindowParts.h"
#include "NodeShaderDocuments.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/ShaderCompileQueue.h"
#include "Shalltear/Core/Input/InputEvent.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/CurveEditor.h"
#include "Shalltear/Editor/EditorAssetWatch.h"
#include "Shalltear/Editor/EditorDocuments.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorGizmoOps.h"
#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/EditorPanel.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/GraphDocument.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/SceneViewEffects.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "Shalltear/VisualEffects/VisualEffect.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;
using namespace ShadingTest;
using namespace UITest;

namespace {

	constexpr Vec2I Parked = {-20000, -20000};

	str Fountain(std::string_view tint) {
		return str(R"({"_type":"EffectGraph","version":2,"nextId":20,"settings":{},
"properties":[{"id":1,"name":"Tint","reference":"tint","type":"Color","default":[)") +
			   str(tint) + R"(]}],
"nodes":[{"id":10,"def":"Input/Property","x":-320,"y":820,"settings":{"property":1}}],
"contexts":[
{"id":2,"def":"Context/Spawn","x":0,"y":0,"blocks":[{"id":3,"def":"Block/Burst","in":{"Count":{"value":[1]}}}]},
{"id":4,"def":"Context/Initialize","x":0,"y":220,"settings":{"Capacity":4},"flow":[[2,"In"]],"blocks":[
{"id":5,"def":"Block/SetAttribute","settings":{"Attribute":"position"},"in":{"Position":{"value":[0,0,0.5]}}},
{"id":6,"def":"Block/SetAttribute","settings":{"Attribute":"size"},"in":{"Size":{"value":[1]}}}]},
{"id":7,"def":"Context/Update","x":0,"y":560,"blocks":[]},
{"id":8,"def":"Context/OutputParticlePrimitive","x":0,"y":760,"flow":[[4,"In"]],"blocks":[
{"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"from":[10,"Out"]}}}]}],
"groups":[],"notes":[]}
)";
	}

	constexpr const char* GRAPH_NAME = "Fountain.sleffect";
	constexpr const char* GRAPH_PATH = "project://Assets/Fountain.sleffect";

	const GraphContext* ContextOf(const GraphDocument& document, u32 id) {
		for (const GraphContext& context : document.contexts) {
			if (context.node.id == id) {
				return &context;
			}
		}
		return nullptr;
	}

	class EffectGraphWindowFixture : public MaterialReadbackFixture {
	protected:
		ModuleHandle _module;
		const void* _moduleBase = nullptr;
		Handle<Window> _main;
		MaterialAsset* _anyMaterial = nullptr;
		Asset* _imported = nullptr;
		ShadingStage _stage;
		bool _staged = false;

		void SetUp() override {
			MaterialReadbackFixture::SetUp();
			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			UISystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			SceneAnnotations::Initialize();
			PanelManager::Initialize();
			PropertyDrawers::Initialize();
			EditorGizmos::Initialize();
			EditorAssetWatch::Initialize();
			ModuleSystem::Initialize();
			_module = ModuleSystem::Load(Path("VisualEffects" SLT_TEST_DLL_SUFFIX));
			if (ModuleSystem::IsLoaded(_module)) {
				_moduleBase = ModuleSystem::GetNativeHandle(_module);
				EditorExtensions::Load(_moduleBase);
			}

			WindowDesc desc;
			desc.title = "EffectGraphHost";
			desc.clientSize = {1400, 900};
			desc.position = Parked;
			desc.flags = WindowFlags::Resizable;
			_main = WindowSystem::Create(desc);
			ASSERT_TRUE(_main.IsValid());
			ASSERT_NE(UISystem::CreateDockManager(_main), nullptr);
			PanelManager::SetDefaultWindow(_main);
		}

		void TearDown() override {
			if (_staged) {
				TearDownStage(_stage);
			}
			if (_imported) {
				AssetDatabase::UnloadAsset(_imported);
			}
			if (_anyMaterial) {
				Delete(_anyMaterial);
			}
			PanelManager::Terminate();
			Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Debug)};
			UISystem::GetDockWindows(windows);
			for (Handle<Window> window : windows) {
				WindowSystem::RequestDestroy(window);
			}
			WindowSystem::PumpEvents();
			if (_moduleBase) {
				EditorExtensions::Unload(_moduleBase);
			}
			ModuleSystem::Terminate();
			EditorAssetWatch::Terminate();
			EditorGizmos::Terminate();
			PropertyDrawers::Terminate();
			SceneViewEffects::Terminate();
			EditorDocuments::Terminate();
			SceneAnnotations::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			UISystem::Terminate();
			MaterialReadbackFixture::TearDown();
		}

		static void Frame() {
			WindowSystem::PumpEvents();
			PanelManager::TickAll(0.016f);
			UISystem::Update();
			FrameStages::Run(FrameStage::LateUpdate, {});
			FrameStages::Run(FrameStage::EndFrame, {});
			PanelManager::SubmitViews(0.016f);
			RenderSystem::RenderAll();
		}

		Guid StageGraph(const str& text) {
			std::filesystem::create_directories(_root / "Assets");
			std::ofstream out(_root / "Assets" / GRAPH_NAME, std::ios::binary);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.close();
			AssetDatabase::ScanProjectDirectory();
			return AssetDatabase::LookupGuid(Path(GRAPH_PATH));
		}

		static const TypeInfo* WindowType() { return TypeRegistry::FindType(StringHash("EffectGraphWindow")); }

		static Vector<EditorPanel*> Windows() {
			Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
			PanelManager::GetOpenPanels(panels);
			Vector<EditorPanel*> out{GetMemoryResource(MemTag::Editor)};
			for (const Handle<EditorPanel>& panel : panels) {
				if (panel.Get() && WindowType() && panel.Get()->GetType() == WindowType()) {
					out.push_back(panel.Get());
				}
			}
			return out;
		}

		static EditorPanel* Open() {
			if (!AssetOpeners::Open(Path(GRAPH_PATH))) {
				return nullptr;
			}
			for (u32 frame = 0; frame < 4; ++frame) {
				Frame();
			}
			const Vector<EditorPanel*> windows = Windows();
			return windows.empty() ? nullptr : windows.back();
		}

		static Vector<UI::Context*> CanvasContexts() {
			Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Debug)};
			UISystem::GetDockWindows(windows);
			Vector<UI::Context*> out{GetMemoryResource(MemTag::Debug)};
			for (Handle<Window> window : windows) {
				UI::Context* context = UISystem::GetContext(window);
				if (context && context->IsAlive(FindByClass(*context, context->GetRoot(), EditorTheme::Class::Graph, 0))) {
					out.push_back(context);
				}
			}
			return out;
		}

		static UI::ElementHandle NodeShowing(UI::Context& context, std::string_view label) {
			const UI::ElementHandle header = FindByText(context, context.GetRoot(), label);
			return context.IsAlive(header) ? context.MakeHandle(context.GetElement(header)->parent) : UI::ElementHandle{};
		}

		void OpensLegibly(u32 index, std::span<const std::string_view, 4> labels) {
			StageGraph(EffectGraphEditOps::TemplateDocument(index));
			EditorPanel* window = Open();
			ASSERT_NE(window, nullptr);
			ASSERT_FALSE(CanvasContexts().empty());
			UI::Context& context = *CanvasContexts()[0];
			UI::Rect boxes[4];
			for (u32 i = 0; i < 4; ++i) {
				const UI::ElementHandle node = NodeShowing(context, labels[i]);
				ASSERT_TRUE(context.IsAlive(node)) << labels[i];
				boxes[i] = context.GetLayout(node).borderBox;
			}
			for (u32 a = 0; a < 4; ++a) {
				for (u32 b = a + 1; b < 4; ++b) {
					const bool apart = boxes[a].x + boxes[a].w <= boxes[b].x || boxes[b].x + boxes[b].w <= boxes[a].x || boxes[a].y + boxes[a].h <= boxes[b].y ||
									   boxes[b].y + boxes[b].h <= boxes[a].y;
					EXPECT_TRUE(apart) << labels[a] << " and " << labels[b] << " overlap";
				}
			}
			const UI::ElementHandle initialize = NodeShowing(context, labels[1]);
			const u32 blocks = CountByClass(context, initialize, EditorTheme::Class::GraphBlock);
			EXPECT_EQ(blocks, 3u);
			for (u32 i = 0; i < blocks; ++i) {
				const UI::Rect block = context.GetLayout(FindByClass(context, initialize, EditorTheme::Class::GraphBlock, i)).borderBox;
				EXPECT_LE(block.y + block.h, boxes[1].y + boxes[1].h + 1.0f) << "block " << i;
			}
		}

		static Vec2I Centre(UI::Context& context, UI::ElementHandle element) {
			const UI::Rect box = context.GetLayout(element).borderBox;
			return Vec2I(static_cast<i32>(box.x + box.w * 0.5f), static_cast<i32>(box.y + box.h * 0.5f));
		}

		static void Inject(UI::Context& context, InputEventType type, Vec2I at, MouseButton button = MouseButton::Left) {
			InputEvent event{};
			event.type = type;
			event.button = button;
			event.mousePosPx = at;
			context.InjectInput(event);
			context.Update();
		}

		static void Drag(UI::Context& context, Vec2I from, Vec2I to) {
			Inject(context, InputEventType::MouseMove, from);
			Inject(context, InputEventType::MouseDown, from);
			for (i32 step = 1; step <= 4; ++step) {
				Inject(context, InputEventType::MouseMove, Vec2I(from.x + (to.x - from.x) * step / 4, from.y + (to.y - from.y) * step / 4));
			}
			Inject(context, InputEventType::MouseUp, to);
			Frame();
		}

		static void Key(UI::Context& context, Shalltear::Key key, char32_t character = 0) {
			InputEvent down{};
			down.type = InputEventType::KeyDown;
			down.key = key;
			context.InjectInput(down);
			context.Update();
			if (character != 0) {
				InputEvent text{};
				text.type = InputEventType::Text;
				text.codepoint = character;
				context.InjectInput(text);
				context.Update();
			}
			InputEvent up = down;
			up.type = InputEventType::KeyUp;
			context.InjectInput(up);
			context.Update();
		}

		static void Type(UI::Context& context, std::string_view text) {
			for (const char c : text) {
				InputEvent event{};
				event.type = InputEventType::Text;
				event.codepoint = static_cast<char32_t>(c);
				context.InjectInput(event);
				context.Update();
			}
		}

		static void FocusCanvas(UI::Context& context) {
			const UI::ElementHandle canvas = FindByClass(context, context.GetRoot(), EditorTheme::Class::Graph, 0);
			const UI::Rect box = context.GetLayout(canvas).content;
			ClickElement(context, canvas);
			Inject(context, InputEventType::MouseMove, Vec2I(static_cast<i32>(box.x + 6.0f), static_cast<i32>(box.y + 6.0f)));
			Inject(context, InputEventType::MouseDown, Vec2I(static_cast<i32>(box.x + 6.0f), static_cast<i32>(box.y + 6.0f)));
			Inject(context, InputEventType::MouseUp, Vec2I(static_cast<i32>(box.x + 6.0f), static_cast<i32>(box.y + 6.0f)));
		}

		GraphDocument* Document(const Guid& graph) const { return NodeShaderDocuments::Get(graph); }
	};

} // anonymous namespace

TEST_F(EffectGraphWindowFixture, DoubleClickOpensOneWindow) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_TRUE(graph.IsValid());
	ASSERT_TRUE(AssetOpeners::Has(".sleffect"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_EQ(Windows().size(), 1u);
	EXPECT_EQ(window->GetTitleBase(), "Fountain");
	ASSERT_NE(Open(), nullptr);
	EXPECT_EQ(Windows().size(), 1u);

	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	for (std::string_view label : {"Initialize Particle", "Update Particle", "Output Particle Primitive", "Set Position", "Set Size", "Set Color"}) {
		EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), label))) << label << "'";
	}
	const UI::ElementHandle initialize = NodeShowing(context, "Initialize Particle");
	EXPECT_EQ(CountByClass(context, initialize, EditorTheme::Class::GraphBlock), 2u);
	EXPECT_TRUE(context.IsAlive(FindByText(context, initialize, "Capacity")));
	EXPECT_TRUE(NodeShaderDocuments::IsOpen(graph));
}

TEST_F(EffectGraphWindowFixture, OperatorSubgraphPlusAddsOutput) {
	ASSERT_NE(_moduleBase, nullptr);
	std::filesystem::create_directories(_root / "Assets");
	std::ofstream(_root / "Assets" / "Range.sleffectop", std::ios::binary) << EffectGraphEditOps::OperatorSubgraphTemplate();
	AssetDatabase::ScanProjectDirectory();
	const Guid range = AssetDatabase::LookupGuid(Path("project://Assets/Range.sleffectop"));
	ASSERT_TRUE(range.IsValid());
	ASSERT_TRUE(AssetOpeners::Has(".sleffectop"));
	ASSERT_TRUE(AssetOpeners::Open(Path("project://Assets/Range.sleffectop")));
	for (u32 frame = 0; frame < 4; ++frame) {
		Frame();
	}
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle output = FindByText(context, context.GetRoot(), "Output");
	ASSERT_TRUE(context.IsAlive(output));
	for (std::string_view label : {"range", "min", "max"}) {
		EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), label))) << label << "'";
	}
	EXPECT_TRUE(context.IsAlive(FindByTextContaining(context, context.GetRoot(), "Subgraph Operator")));

	const UI::Element* title = context.GetElement(output);
	ASSERT_NE(title->nextSibling, UI::INVALID_UI_INDEX);
	ClickElement(context, context.MakeHandle(title->nextSibling));
	const UI::ElementHandle kind = FindByText(context, context.GetRoot(), "Boolean");
	ASSERT_TRUE(context.IsAlive(kind));
	ClickElement(context, kind);
	Frame();
	ASSERT_NE(Document(range), nullptr);
	ASSERT_EQ(Document(range)->properties.size(), 4u);
	EXPECT_TRUE(GraphSubgraphs::IsOutput(Document(range)->properties.back()));
	EXPECT_EQ(Document(range)->properties.back().type, "Boolean");
	UndoSystem::Undo();
	Frame();
	EXPECT_EQ(Document(range)->properties.size(), 3u);
}

TEST_F(EffectGraphWindowFixture, BlockSubgraphOpensFromMenu) {
	ASSERT_NE(_moduleBase, nullptr);
	std::filesystem::create_directories(_root / "Assets");
	std::ofstream(_root / "Assets" / "Sized.sleffectblock", std::ios::binary) << R"({"_type":"VisualEffectSubgraphBlock","version":1,"nextId":9,
"properties":[],"nodes":[],"contexts":[{"id":3,"def":"Context/BlockSubgraph","x":0,"y":0,"blocks":[
{"id":4,"def":"Block/SetAttribute","settings":{"Attribute":"size"},"in":{"Size":{"value":[0.5]}}}]}],"groups":[],"notes":[]})";
	AssetDatabase::ScanProjectDirectory();
	const Guid sized = AssetDatabase::LookupGuid(Path("project://Assets/Sized.sleffectblock"));
	ASSERT_TRUE(sized.IsValid());
	str fountain = Fountain("1,0,0,1");
	const std::string_view last = R"("in":{"Size":{"value":[1]}}})";
	fountain.insert(fountain.find(last) + last.size(), R"(,{"id":11,"def":"Block/Subgraph","settings":{"asset":")" + sized.ToString() + R"("}})");
	StageGraph(fountain);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle block = FindByText(context, context.GetRoot(), "Sized");
	ASSERT_TRUE(context.IsAlive(block));
	RightClickElement(context, context.MakeHandle(context.GetElement(block)->parent));
	const UI::ElementHandle open = FindByText(context, context.GetRoot(), "Open Subgraph");
	ASSERT_TRUE(context.IsAlive(open));
	ClickElement(context, open);
	for (u32 frame = 0; frame < 4; ++frame) {
		Frame();
	}
	ASSERT_EQ(Windows().size(), 2u);
	EXPECT_EQ(Windows().back()->GetTitleBase(), "Sized");
	bool shown = false;
	for (UI::Context* other : CanvasContexts()) {
		shown = shown || (other->IsAlive(FindByText(*other, other->GetRoot(), "Block Subgraph")) &&
						  other->IsAlive(FindByText(*other, other->GetRoot(), "Suitable Contexts")) && other->IsAlive(FindByText(*other, other->GetRoot(), "Set Size")));
	}
	EXPECT_TRUE(shown);
}

TEST_F(EffectGraphWindowFixture, SystemSubgraphLinksAtSlot) {
	ASSERT_NE(_moduleBase, nullptr);
	std::filesystem::create_directories(_root / "Assets");
	std::ofstream(_root / "Assets" / "Spark.sleffect", std::ios::binary) << Fountain("0,1,0,1");
	AssetDatabase::ScanProjectDirectory();
	const Guid spark = AssetDatabase::LookupGuid(Path("project://Assets/Spark.sleffect"));
	ASSERT_TRUE(spark.IsValid());
	const Guid graph = StageGraph(std::format(R"({{"_type":"EffectGraph","version":4,"nextId":20,"settings":{{}},"properties":[],"nodes":[],"contexts":[
{{"id":2,"def":"Context/Event","x":0,"y":0,"settings":{{"Event Name":"Fire"}},"blocks":[]}},
{{"id":3,"def":"Context/Subgraph","x":0,"y":320,"settings":{{"asset":"{}"}},"blocks":[]}}],"groups":[],"notes":[]}})",
											  spark.ToString()));
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle subgraph = NodeShowing(context, "Spark");
	ASSERT_TRUE(context.IsAlive(subgraph));
	EXPECT_TRUE(context.IsAlive(FindByText(context, subgraph, "OnPlay")) && context.IsAlive(FindByText(context, subgraph, "OnStop")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, subgraph, "Tint")));

	const UI::ElementHandle event = NodeShowing(context, "Event");
	ASSERT_TRUE(context.IsAlive(event));
	Drag(context, Centre(context, FindByClass(context, event, EditorTheme::Class::GraphFlowPort, 0)),
		 Centre(context, FindByClass(context, NodeShowing(context, "Spark"), EditorTheme::Class::GraphFlowPort, 0)));
	const GraphContext* linked = ContextOf(*Document(graph), 3);
	ASSERT_NE(linked, nullptr);
	ASSERT_EQ(linked->flows.size(), 1u);
	EXPECT_EQ(linked->flows[0].from, 2u);
	EXPECT_EQ(linked->flows[0].slot, "OnPlay");

	UI::Context& again = *CanvasContexts()[0];
	RightClickElement(again, NodeShowing(again, "Spark"));
	const UI::ElementHandle enter = FindByText(again, again.GetRoot(), "Enter Subgraph");
	ASSERT_TRUE(again.IsAlive(enter));
	EXPECT_FALSE(again.IsAlive(FindByText(again, again.GetRoot(), "Create Block")));
	ClickElement(again, enter);
	for (u32 frame = 0; frame < 4; ++frame) {
		Frame();
	}
	ASSERT_EQ(Windows().size(), 2u);
	EXPECT_EQ(Windows().back()->GetTitleBase(), "Spark");
}

TEST_F(EffectGraphWindowFixture, SpaceOverContextAddsBlock) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle update = NodeShowing(context, "Update Particle");
	ASSERT_TRUE(context.IsAlive(update));

	FocusCanvas(context);
	const UI::Rect box = context.GetLayout(update).borderBox;
	Inject(context, InputEventType::MouseMove, Vec2I(static_cast<i32>(box.x + box.w * 0.5f), static_cast<i32>(box.y + box.h * 0.7f)));
	Key(context, Key::Space, U' ');
	Type(context, "add velocity");
	Key(context, Key::Enter);
	Frame();

	const GraphContext* stack = ContextOf(*Document(graph), 7);
	ASSERT_NE(stack, nullptr);
	ASSERT_EQ(stack->blocks.size(), 1u);
	EXPECT_EQ(stack->blocks[0].def, GraphEffectSetAttribute);
	str composition;
	for (const GraphSetting& setting : stack->blocks[0].settings) {
		composition = setting.key == "Composition" ? setting.value : composition;
	}
	EXPECT_EQ(composition, "\"Add\"");
	EXPECT_TRUE(window->HasUnsavedChanges());

	UndoSystem::Undo();
	Frame();
	EXPECT_TRUE(ContextOf(*Document(graph), 7)->blocks.empty());
}

TEST_F(EffectGraphWindowFixture, SpaceButtonIsOneUndo) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	ASSERT_EQ(CountByClass(context, context.GetRoot(), EditorTheme::Class::GraphSpaceButton), 2u);
	const UI::ElementHandle block = NodeShowing(context, "Set Position");
	ASSERT_TRUE(context.IsAlive(block));
	const UI::ElementHandle button = FindByClass(context, block, EditorTheme::Class::GraphSpaceButton, 0);
	ASSERT_TRUE(context.IsAlive(button));
	EXPECT_TRUE(context.IsAlive(FindByText(context, button, "L")));

	const Vec2I at = Centre(context, button);
	Inject(context, InputEventType::MouseMove, at);
	Inject(context, InputEventType::MouseDown, at);
	Inject(context, InputEventType::MouseUp, at);
	Frame();
	const GraphNode* position = GraphDocumentOps::FindNode(*Document(graph), 5);
	ASSERT_NE(position, nullptr);
	ASSERT_EQ(position->inputs.size(), 1u);
	EXPECT_EQ(position->inputs[0].space, "World");
	EXPECT_EQ(position->inputs[0].value, "[0,0,0.5]");
	EXPECT_TRUE(window->HasUnsavedChanges());
	const UI::ElementHandle switched = FindByClass(context, NodeShowing(context, "Set Position"), EditorTheme::Class::GraphSpaceButton, 0);
	EXPECT_TRUE(context.IsAlive(FindByText(context, switched, "W")));

	UndoSystem::Undo();
	Frame();
	EXPECT_TRUE(GraphDocumentOps::FindNode(*Document(graph), 5)->inputs[0].space.empty());
}

TEST_F(EffectGraphWindowFixture, TemplateContextsDontOverlap) {
	ASSERT_NE(_moduleBase, nullptr);
	constexpr std::string_view Labels[] = {"Spawn", "Initialize Particle", "Update Particle", "Output Particle Primitive"};
	OpensLegibly(1, Labels);
}

TEST_F(EffectGraphWindowFixture, SimpleTrailContextsDontOverlap) {
	ASSERT_NE(_moduleBase, nullptr);
	constexpr std::string_view Labels[] = {"Spawn", "Initialize Particle Strip", "Update Particle", "Output ParticleStrip Quad"};
	OpensLegibly(3, Labels);
}

namespace {

	Vector<UI::ElementHandle> DrawnContexts(UI::Context& context) {
		Vector<UI::ElementHandle> out{GetMemoryResource(MemTag::Editor)};
		const u32 ports = CountByClass(context, context.GetRoot(), EditorTheme::Class::GraphFlowPort);
		for (u32 i = 0; i < ports; ++i) {
			const UI::ElementHandle port = FindByClass(context, context.GetRoot(), EditorTheme::Class::GraphFlowPort, i);
			const UI::ElementHandle node = context.MakeHandle(context.GetElement(port)->parent);
			if (std::find(out.begin(), out.end(), node) == out.end()) {
				out.push_back(node);
			}
		}
		return out;
	}

	UI::ElementHandle DotOf(UI::Context& context, UI::ElementHandle owner, bool output) {
		const UI::Rect box = context.GetLayout(owner).borderBox;
		for (u32 child = context.GetElement(owner)->firstChild; child != UI::INVALID_UI_INDEX;) {
			const UI::ElementHandle handle = context.MakeHandle(child);
			const UI::Rect dot = context.GetLayout(handle).borderBox;
			const bool square = dot.w > 0.0f && dot.w < 24.0f && std::fabs(dot.w - dot.h) < 0.5f;
			const bool right = dot.x + dot.w * 0.5f > box.x + box.w * 0.5f;
			if (square && right == output && context.GetTextString(handle).empty()) {
				return handle;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

} // anonymous namespace

TEST_F(EffectGraphWindowFixture, HeadAndTrailContextsDontOverlap) {
	ASSERT_NE(_moduleBase, nullptr);
	StageGraph(EffectGraphEditOps::TemplateDocument(4));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const Vector<UI::ElementHandle> nodes = DrawnContexts(context);
	ASSERT_EQ(nodes.size(), 8u);
	for (size_t a = 0; a < nodes.size(); ++a) {
		const UI::Rect first = context.GetLayout(nodes[a]).borderBox;
		for (size_t b = a + 1; b < nodes.size(); ++b) {
			const UI::Rect second = context.GetLayout(nodes[b]).borderBox;
			const bool apart = first.x + first.w <= second.x || second.x + second.w <= first.x || first.y + first.h <= second.y ||
							   second.y + second.h <= first.y;
			EXPECT_TRUE(apart) << "contexts " << a << " and " << b << " overlap";
		}
	}
}

TEST_F(EffectGraphWindowFixture, TriggerEvtDragsToGpuEvent) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(str(R"({"_type":"EffectGraph","version":4,"nextId":30,"nodes":[],"contexts":[
		{"id":1,"def":"Context/Initialize","x":0,"y":0,"blocks":[]},
		{"id":2,"def":"Context/Update","x":0,"y":420,"flow":[[1,"In"]],"blocks":[{"id":20,"def":"Block/TriggerEvent","settings":{"Mode":"Always"}}]},
		{"id":6,"def":"Context/GPUEvent","x":520,"y":420,"blocks":[]},
		{"id":3,"def":"Context/Initialize","x":520,"y":620,"flow":[[6,"In"]],"blocks":[]}]})"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle title = FindByText(context, context.GetRoot(), "Trigger Event Always");
	ASSERT_TRUE(context.IsAlive(title));
	const UI::ElementHandle block = context.MakeHandle(context.GetElement(title)->parent);
	const UI::ElementHandle evt = DotOf(context, block, true);
	ASSERT_TRUE(context.IsAlive(evt));
	const UI::ElementHandle event = NodeShowing(context, "GPU Event");
	ASSERT_TRUE(context.IsAlive(event));
	const UI::ElementHandle input = DotOf(context, event, false);
	ASSERT_TRUE(context.IsAlive(input));

	Drag(context, Centre(context, evt), Centre(context, input));
	const GraphNode* wired = GraphDocumentOps::FindNode(*Document(graph), 6);
	ASSERT_NE(wired, nullptr);
	const auto fed = std::find_if(wired->inputs.begin(), wired->inputs.end(), [](const GraphInput& held) { return held.port == GraphEffectEvt; });
	ASSERT_NE(fed, wired->inputs.end());
	EXPECT_EQ(fed->fromNode, 20u);
	EXPECT_EQ(fed->fromPort, GraphEffectEvt);
	UndoSystem::Undo();
	Frame();
	wired = GraphDocumentOps::FindNode(*Document(graph), 6);
	EXPECT_TRUE(std::none_of(wired->inputs.begin(), wired->inputs.end(), [](const GraphInput& held) { return held.fromNode != 0; }));
}

TEST_F(EffectGraphWindowFixture, FlowPortDraggedOntoAnotherLinksContexts) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle initialize = NodeShowing(context, "Initialize Particle");
	const UI::ElementHandle update = NodeShowing(context, "Update Particle");
	ASSERT_TRUE(context.IsAlive(update));
	const UI::ElementHandle initializeOut = FindByClass(context, initialize, EditorTheme::Class::GraphFlowPort, 1);
	const UI::ElementHandle updateIn = FindByClass(context, update, EditorTheme::Class::GraphFlowPort, 0);
	ASSERT_TRUE(context.IsAlive(initializeOut));
	ASSERT_TRUE(context.IsAlive(updateIn));

	Drag(context, Centre(context, initializeOut), Centre(context, updateIn));
	const GraphContext* linked = ContextOf(*Document(graph), 7);
	ASSERT_EQ(linked->flows.size(), 1u);
	EXPECT_EQ(linked->flows[0].from, 4u);
	EXPECT_EQ(linked->flows[0].slot, GraphEffectIn);
	const UI::ElementHandle spawn = NodeShowing(context, "Spawn");
	Drag(context, Centre(context, FindByClass(context, spawn, EditorTheme::Class::GraphFlowPort, 2)),
		 Centre(context, FindByClass(context, NodeShowing(context, "Update Particle"), EditorTheme::Class::GraphFlowPort, 0)));
	EXPECT_EQ(ContextOf(*Document(graph), 7)->flows.size(), 1u);
	EXPECT_EQ(ContextOf(*Document(graph), 7)->flows[0].from, 4u);
}

TEST_F(EffectGraphWindowFixture, BlockDraggedToAnotherContextMovesThere) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle initialize = NodeShowing(context, "Initialize Particle");
	const UI::ElementHandle size = FindByClass(context, initialize, EditorTheme::Class::GraphBlock, 1);
	ASSERT_TRUE(context.IsAlive(size));
	const UI::Rect title = context.GetLayout(FindByText(context, size, "Set Size")).borderBox;
	const UI::Rect update = context.GetLayout(NodeShowing(context, "Update Particle")).borderBox;

	Drag(context, Vec2I(static_cast<i32>(title.x + title.w * 0.5f), static_cast<i32>(title.y + title.h * 0.5f)),
		 Vec2I(static_cast<i32>(update.x + update.w * 0.5f), static_cast<i32>(update.y + update.h * 0.75f)));
	const GraphDocument* document = Document(graph);
	EXPECT_EQ(ContextOf(*document, 4)->blocks.size(), 1u);
	ASSERT_EQ(ContextOf(*document, 7)->blocks.size(), 1u);
	EXPECT_EQ(ContextOf(*document, 7)->blocks[0].id, 6u);
}

TEST_F(EffectGraphWindowFixture, BlockDuplicatedAloneLandsBelowItselfSelected) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto block = [&](u32 index) { return FindByClass(context, NodeShowing(context, "Initialize Particle"), EditorTheme::Class::GraphBlock, index); };
	const size_t before = ContextOf(*Document(graph), 4)->blocks.size();
	ASSERT_GE(before, 2u);
	const GraphNode first = ContextOf(*Document(graph), 4)->blocks[0];
	ClickElement(context, FindByText(context, block(0), "Set Position"));
	Frame();
	ASSERT_TRUE(window->OnCommand(PanelCommand::Duplicate));
	Frame();
	const GraphContext* initialize = ContextOf(*Document(graph), 4);
	ASSERT_EQ(initialize->blocks.size(), before + 1);
	EXPECT_EQ(initialize->blocks[0].id, first.id);
	EXPECT_NE(initialize->blocks[1].id, first.id);
	EXPECT_EQ(initialize->blocks[1].def, first.def);
	EXPECT_EQ(initialize->blocks[2].id, 6u);
	EXPECT_EQ(context.GetStyle(block(1)).borderColor, EditorTheme::GetColor(EditorTheme::ColorToken::SelectionWire));
	EXPECT_NE(context.GetStyle(block(0)).borderColor, EditorTheme::GetColor(EditorTheme::ColorToken::SelectionWire));
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(ContextOf(*Document(graph), 4)->blocks.size(), before);
}

TEST_F(EffectGraphWindowFixture, UndoneDuplicateLeavesInspectorOnAssetsSettings) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto inspectsAsset = [&]() { return context.IsAlive(FindByText(context, context.GetRoot(), "Update mode")); };
	ASSERT_TRUE(inspectsAsset());
	const UI::ElementHandle first = FindByClass(context, NodeShowing(context, "Initialize Particle"), EditorTheme::Class::GraphBlock, 0);
	ClickElement(context, FindByText(context, first, "Set Position"));
	Frame();
	ASSERT_FALSE(inspectsAsset());
	const size_t before = ContextOf(*Document(graph), 4)->blocks.size();
	ASSERT_TRUE(window->OnCommand(PanelCommand::Duplicate));
	Frame();
	ASSERT_EQ(ContextOf(*Document(graph), 4)->blocks.size(), before + 1);
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	ASSERT_EQ(ContextOf(*Document(graph), 4)->blocks.size(), before);
	EXPECT_TRUE(inspectsAsset());
}

TEST_F(EffectGraphWindowFixture, InspectorFollowsRemainingSelection) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto inspectsAsset = [&]() { return context.IsAlive(FindByText(context, context.GetRoot(), "Update mode")); };
	const auto title = [&](u32 index, std::string_view label) {
		return FindByText(context, FindByClass(context, NodeShowing(context, "Initialize Particle"), EditorTheme::Class::GraphBlock, index), label);
	};
	ClickElement(context, title(0, "Set Position"));
	Frame();
	const UI::Rect second = context.GetLayout(title(1, "Set Size")).borderBox;
	ASSERT_GT(second.w, 0.0f);
	InputEvent press{};
	press.button = MouseButton::Left;
	press.mods = KeyMod::Ctrl;
	press.mousePosPx = Vec2I(static_cast<i32>(second.x + second.w * 0.5f), static_cast<i32>(second.y + second.h * 0.5f));
	for (const InputEventType type : {InputEventType::MouseMove, InputEventType::MouseDown, InputEventType::MouseUp}) {
		press.type = type;
		context.InjectInput(press);
		context.Update();
	}
	Frame();
	ASSERT_FALSE(inspectsAsset());

	NodeShaderDocuments::BeginEdit(graph, "Delete");
	const u32 taken[] = {6};
	ASSERT_EQ(EffectGraphEditOps::Delete(*Document(graph), taken), 1u);
	NodeShaderDocuments::EndEdit(graph);
	Frame();
	Frame();
	EXPECT_FALSE(inspectsAsset());
}

TEST_F(EffectGraphWindowFixture, NodeSettingTypedInPlace) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle label = FindByText(context, NodeShowing(context, "Initialize Particle"), "Capacity");
	ASSERT_TRUE(context.IsAlive(label));
	ClickElement(context, context.MakeHandle(context.GetElement(label)->parent));
	for (u32 erase = 0; erase < 4; ++erase) {
		Key(context, Key::Backspace);
	}
	Type(context, "512");
	Key(context, Key::Enter);
	Frame();
	str capacity;
	for (const GraphSetting& setting : ContextOf(*Document(graph), 4)->node.settings) {
		capacity = setting.key == "Capacity" ? setting.value : capacity;
	}
	EXPECT_EQ(capacity, "512");
}

TEST_F(EffectGraphWindowFixture, EmptySelectionEditsUpdateMode) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto fieldAfter = [&context](std::string_view label) {
		const UI::Element* held = context.GetElement(FindByText(context, context.GetRoot(), label));
		return held && held->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(held->nextSibling) : UI::ElementHandle{};
	};
	const auto stored = [&](std::string_view key) {
		str value;
		for (const GraphSetting& setting : Document(graph)->settings) {
			value = setting.key == key ? setting.value : value;
		}
		return value;
	};
	ASSERT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Update mode")));
	const auto box = [&](std::string_view label) { return FindByClass(context, fieldAfter(label), StringHash("slt-checkbox"), 0); };
	ASSERT_TRUE(context.IsAlive(box("Exact Fixed Time")));
	ClickElement(context, box("Exact Fixed Time"));
	Frame();
	EXPECT_EQ(stored(GraphEffectExactFixedTime), "true");
	ClickElement(context, box("Fixed Delta Time"));
	Frame();
	EXPECT_EQ(stored(GraphEffectFixedDeltaTime), "false");
	ASSERT_TRUE(context.IsAlive(box("Exact Fixed Time")));
	ClickElement(context, box("Exact Fixed Time"));
	Frame();
	EXPECT_EQ(stored(GraphEffectExactFixedTime), "true");

	const UI::ElementHandle total = FindByClass(context, fieldAfter("PreWarm Total Time"), StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(total));
	ClickElement(context, total);
	InputEvent all{};
	all.type = InputEventType::KeyDown;
	all.key = Shalltear::Key::A;
	all.mods = KeyMod::Ctrl;
	context.InjectInput(all);
	context.Update();
	Type(context, "2");
	Key(context, Shalltear::Key::Enter);
	Frame();
	Frame();
	const GraphEffectTiming timing = GraphEffectTarget::Timing(*Document(graph));
	EXPECT_EQ(timing.prewarmStepCount, 1u);
	EXPECT_EQ(timing.prewarmDeltaTime, 2.0f);
	EXPECT_TRUE(context.IsAlive(FindByText(context, fieldAfter("PreWarm Step Count"), "1")));
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(GraphEffectTarget::Timing(*Document(graph)).prewarmStepCount, 0u);
	EXPECT_EQ(stored(GraphEffectFixedDeltaTime), "false");
}

TEST_F(EffectGraphWindowFixture, EmptySelectionEditsCulling) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto popup = [&context]() {
		const UI::Element* label = context.GetElement(FindByText(context, context.GetRoot(), "Culling Flags"));
		const UI::ElementHandle field = label && label->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(label->nextSibling) : UI::ElementHandle{};
		return FindByClass(context, field, StringHash("slt-dropdown"), 0);
	};
	const auto flags = [&]() { return GraphEffectTarget::Timing(*Document(graph)).cullingFlags; };
	const auto pick = [&](u32 choice) {
		ClickElement(context, popup());
		ASSERT_EQ(context.GetOpenPopupCount(), 1u);
		ClickElement(context, NthChildOf(context, context.GetOpenPopup(0), choice));
		Frame();
	};
	ASSERT_TRUE(context.IsAlive(popup()));
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup(), "Simulate and update bounds only while visible")));

	pick(1);
	EXPECT_EQ(flags(), GRAPH_EFFECT_CULL_SIMULATION);
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup(), "Always update bounds, simulate only while visible")));
	pick(2);
	EXPECT_EQ(flags(), 0u);
	pick(0);
	EXPECT_EQ(flags(), GRAPH_EFFECT_CULL_DEFAULT);
	for (const GraphSetting& setting : Document(graph)->settings) {
		EXPECT_NE(setting.key, GraphEffectCullingFlags);
	}
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(flags(), 0u);
}

TEST_F(EffectGraphWindowFixture, WarnedBlockMarked) {
	ASSERT_NE(_moduleBase, nullptr);
	str idle = Fountain("1,0,0,1");
	const str update = R"({"id":7,"def":"Context/Update","x":0,"y":560,"blocks":[]})";
	idle.replace(idle.find(update), update.size(),
				 R"({"id":7,"def":"Context/Update","x":0,"y":560,"flow":[[4,"In"]],"blocks":[{"id":12,"def":"Block/CollisionShape","settings":{"Behavior":"None"}}]})");
	const str output = R"("flow":[[4,"In"]],"blocks":[
{"id":9)";
	idle.replace(idle.find(output), output.size(), R"("flow":[[7,"In"]],"blocks":[
{"id":9)");
	const Guid graph = StageGraph(idle);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto block = [&]() { return FindByClass(context, NodeShowing(context, "Update Particle"), EditorTheme::Class::GraphBlock, 0); };
	const auto border = [&](UI::ElementHandle element) { return context.GetStyle(element).borderColor; };
	const auto deselect = [&]() {
		FocusCanvas(context);
		Frame();
	};
	const Color warn = EditorTheme::GetColor(EditorTheme::ColorToken::StatusWarn);
	const Color edge = EditorTheme::GetColor(EditorTheme::ColorToken::CardEdge);
	ASSERT_TRUE(context.IsAlive(block()));
	EXPECT_EQ(border(block()), warn);
	EXPECT_EQ(border(NodeShowing(context, "Initialize Particle")), warn);
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "2 warnings")));

	ClickElement(context, FindByText(context, block(), "Trigger Shape Plane"));
	Frame();
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(),
										   "This block does nothing (its behavior is None, it writes no collision attributes).")));
	ClickElement(context, FindByText(context, context.GetRoot(), "Ignore"));
	Frame();
	const GraphNode* ignored = GraphDocumentOps::FindNode(*Document(graph), 12);
	ASSERT_NE(ignored, nullptr);
	EXPECT_TRUE(GraphEffectRules::IgnoresError(*ignored, "collision-does-nothing"));
	deselect();
	EXPECT_EQ(border(block()), edge);
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Unsaved - 1 warning")));

	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(border(block()), warn);

	ASSERT_TRUE(UndoSystem::Redo());
	Frame();
	EXPECT_EQ(border(block()), edge);
	RightClickElement(context, FindByText(context, block(), "Trigger Shape Plane"));
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	const UI::ElementHandle clear = FindByText(context, context.GetOpenPopup(0), "Clear Ignored Errors");
	ASSERT_TRUE(context.IsAlive(clear));
	ClickElement(context, clear);
	Frame();
	EXPECT_FALSE(GraphEffectRules::HasIgnoredErrors(*GraphDocumentOps::FindNode(*Document(graph), 12)));
	deselect();
	EXPECT_EQ(border(block()), warn);
}

TEST_F(EffectGraphWindowFixture, AutoBoundsForceAlwaysRecompute) {
	ASSERT_NE(_moduleBase, nullptr);
	str automatic = Fountain("1,0,0,1");
	const str capacity = R"("settings":{"Capacity":4})";
	automatic.replace(automatic.find(capacity), capacity.size(), R"("settings":{"Capacity":4,"Bounds Setting Mode":"Automatic"})");
	const Guid graph = StageGraph(automatic);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto popup = [&context]() {
		const UI::Element* label = context.GetElement(FindByText(context, context.GetRoot(), "Culling Flags"));
		const UI::ElementHandle field = label && label->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(label->nextSibling) : UI::ElementHandle{};
		return FindByClass(context, field, StringHash("slt-dropdown"), 0);
	};
	ASSERT_TRUE(context.IsAlive(popup()));
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup(), "Always simulate and update bounds")));
	ClickElement(context, popup());
	if (context.GetOpenPopupCount() == 1) {
		ClickElement(context, NthChildOf(context, context.GetOpenPopup(0), 0));
	}
	Frame();
	for (const GraphSetting& setting : Document(graph)->settings) {
		EXPECT_NE(setting.key, GraphEffectCullingFlags);
	}
}

TEST_F(EffectGraphWindowFixture, WithNothingSelectedInspectorEditsAssetsInstancing) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto fieldAfter = [&context](std::string_view label) {
		const UI::Element* held = context.GetElement(FindByText(context, context.GetRoot(), label));
		return held && held->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(held->nextSibling) : UI::ElementHandle{};
	};
	const auto popup = [&]() { return FindByClass(context, fieldAfter("Instancing Mode"), StringHash("slt-dropdown"), 0); };
	const auto held = [&]() { return GraphEffectTarget::Instancing(*Document(graph)); };
	const auto pick = [&](u32 choice) {
		ClickElement(context, popup());
		ASSERT_EQ(context.GetOpenPopupCount(), 1u);
		ClickElement(context, NthChildOf(context, context.GetOpenPopup(0), choice));
		Frame();
	};
	ASSERT_TRUE(context.IsAlive(popup()));
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup(), "Automatic batch capacity")));
	EXPECT_FALSE(context.IsAlive(FindByText(context, context.GetRoot(), "Max Batch Capacity")));

	pick(2);
	EXPECT_EQ(held().mode, GRAPH_EFFECT_INSTANCING_CUSTOM);
	const UI::ElementHandle capacity = FindByClass(context, fieldAfter("Max Batch Capacity"), StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(capacity));
	ClickElement(context, capacity);
	InputEvent all{};
	all.type = InputEventType::KeyDown;
	all.key = Shalltear::Key::A;
	all.mods = KeyMod::Ctrl;
	context.InjectInput(all);
	context.Update();
	Type(context, "12");
	Key(context, Shalltear::Key::Enter);
	Frame();
	Frame();
	EXPECT_EQ(held().capacity, 12u);

	pick(0);
	EXPECT_EQ(held().mode, GRAPH_EFFECT_INSTANCING_DISABLED);
	EXPECT_FALSE(context.IsAlive(FindByText(context, context.GetRoot(), "Max Batch Capacity")));
	pick(1);
	EXPECT_EQ(held().mode, GRAPH_EFFECT_INSTANCING_AUTOMATIC);
	for (const GraphSetting& setting : Document(graph)->settings) {
		EXPECT_NE(setting.key, GraphEffectInstancingMode);
	}
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(held().mode, GRAPH_EFFECT_INSTANCING_DISABLED);
}

TEST_F(EffectGraphWindowFixture, GpuEventHoldsInstancingPopupAtDisabled) {
	ASSERT_NE(_moduleBase, nullptr);
	str evented = Fountain("1,0,0,1");
	const str contexts = R"("contexts":[)";
	ASSERT_NE(evented.find(contexts), str::npos);
	evented.replace(evented.find(contexts), contexts.size(), R"("contexts":[{"id":90,"def":"Context/GPUEvent","x":900,"y":0,"blocks":[]},)");
	const Guid graph = StageGraph(evented);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::Element* label = context.GetElement(FindByText(context, context.GetRoot(), "Instancing Mode"));
	const UI::ElementHandle field = label && label->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(label->nextSibling) : UI::ElementHandle{};
	const UI::ElementHandle popup = FindByClass(context, field, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(popup));
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Instancing is off for this effect:")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "- it uses GPU events")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup, "Disabled")));
	ClickElement(context, popup);
	if (context.GetOpenPopupCount() == 1) {
		ClickElement(context, NthChildOf(context, context.GetOpenPopup(0), 2));
	}
	Frame();
	for (const GraphSetting& setting : Document(graph)->settings) {
		EXPECT_NE(setting.key, GraphEffectInstancingMode);
	}
}

TEST_F(EffectGraphWindowFixture, NodeShaderPortsFollowSave) {
	ASSERT_NE(_moduleBase, nullptr);
	const auto shading = [](std::string_view reference, bool vfx) {
		return std::format(R"({{"_type":"NodeShader","version":1,"nextId":20,"settings":{{"target":"unlit"{}}},
"properties":[{{"id":1,"name":"Glow","reference":"{}","type":"Color","default":[1,1,1,1]}}],"nodes":[],
"contexts":[{{"id":5,"def":"Context/Fragment","blocks":[{{"id":6,"def":"Block/BaseColor"}}]}}],"groups":[],"notes":[]}})",
						   vfx ? R"(,"supportEffects":true)" : "", reference);
	};
	std::filesystem::create_directories(_root / "Assets");
	std::ofstream(_root / "Assets" / "Glowing.slshader", std::ios::binary) << shading("_Glow", true);
	std::ofstream(_root / "Assets" / "Plain.slshader", std::ios::binary) << shading("_Plain", false);
	str text = Fountain("1,0,0,1");
	const std::string_view output = R"("def":"Context/OutputParticlePrimitive")";
	text.replace(text.find(output), output.size(), R"("def":"Context/OutputParticleNodeShaderPrimitive")");
	const Guid graph = StageGraph(text);
	const Guid glowing = AssetDatabase::LookupGuid(Path("project://Assets/Glowing.slshader"));
	ASSERT_TRUE(glowing.IsValid());
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const auto outputNode = [&]() { return NodeShowing(context, "Output Particle NodeShader Primitive"); };
	ASSERT_TRUE(context.IsAlive(outputNode()));
	EXPECT_TRUE(context.IsAlive(FindByText(context, outputNode(), "EffectDefault")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, outputNode(), "_Color")));

	const UI::ElementHandle label = FindByText(context, outputNode(), "Node Shader");
	ASSERT_TRUE(context.IsAlive(label));
	ClickElement(context, context.MakeHandle(context.GetElement(label)->parent));
	context.Update();
	EXPECT_FALSE(context.IsAlive(FindByText(context, context.GetRoot(), "Plain")));
	const UI::ElementHandle pick = FindByText(context, context.GetRoot(), "Glowing");
	ASSERT_TRUE(context.IsAlive(pick));
	ClickElement(context, pick);
	for (u32 i = 0; i < 3; ++i) {
		Frame();
	}
	str stored;
	for (const GraphSetting& setting : ContextOf(*Document(graph), 8)->node.settings) {
		stored = setting.key == GraphEffectNodeShaderSetting ? setting.value : stored;
	}
	EXPECT_EQ(stored, "\"" + glowing.ToString() + "\"");
	EXPECT_TRUE(context.IsAlive(FindByText(context, outputNode(), "_Glow")));
	EXPECT_FALSE(context.IsAlive(FindByText(context, outputNode(), "_Color")));

	str error;
	ASSERT_TRUE(NodeShaderDocuments::Open(glowing, nullptr, error)) << error;
	NodeShaderDocuments::BeginEdit(glowing, "Rename");
	NodeShaderDocuments::Get(glowing)->properties[0].reference = "_Bloom";
	NodeShaderDocuments::EndEdit(glowing);
	ASSERT_TRUE(NodeShaderDocuments::Save(glowing));
	NodeShaderDocuments::Close(glowing);
	for (u32 i = 0; i < 3; ++i) {
		Frame();
	}
	EXPECT_TRUE(context.IsAlive(FindByText(context, outputNode(), "_Bloom")));
	EXPECT_FALSE(context.IsAlive(FindByText(context, outputNode(), "_Glow")));
}

TEST_F(EffectGraphWindowFixture, CurveFieldEditIsOneUndo) {
	ASSERT_NE(_moduleBase, nullptr);
	str text = Fountain("1,0,0,1");
	const str block = R"({"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"from":[10,"Out"]}}})";
	text.replace(text.find(block), block.size(),
				 R"({"id":9,"def":"Block/AttributeFromCurve","settings":{"Attribute":"size"}},)"
				 R"({"id":11,"def":"Block/AttributeFromCurve","settings":{"Attribute":"color","Composition":"Multiply"}})");
	const Guid graph = StageGraph(text);
	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];

	const UI::ElementHandle title = FindByText(context, context.GetRoot(), "Set Size Over Life");
	ASSERT_TRUE(context.IsAlive(title));
	const UI::Rect blockBox = context.GetLayout(context.MakeHandle(context.GetElement(title)->parent)).borderBox;
	UI::ElementHandle field;
	for (u32 index = 0; index < CountByClass(context, context.GetRoot(), EditorTheme::Class::GraphInlineField); ++index) {
		const UI::ElementHandle candidate = FindByClass(context, context.GetRoot(), EditorTheme::Class::GraphInlineField, index);
		const Vec2I at = Centre(context, candidate);
		if (at.x > blockBox.x && at.x < blockBox.x + blockBox.w && at.y > blockBox.y && at.y < blockBox.y + blockBox.h) {
			field = candidate;
		}
	}
	ASSERT_TRUE(context.IsAlive(field));
	ClickElement(context, field);
	Frame();
	ASSERT_GE(context.GetOpenPopupCount(), 1u);
	const UI::ElementHandle popup = context.GetOpenPopup(context.GetOpenPopupCount() - 1);
	const UI::ElementHandle plot = FindByClass(context, popup, EditorTheme::Class::CurveEditorPlot, 0);
	ASSERT_TRUE(context.IsAlive(plot));

	AnimationCurve line;
	str error;
	ASSERT_TRUE(GraphCurves::Parse(GraphCurves::DefaultJSON, line, error)) << error;
	const CurveEditor::View view = CurveEditor::Frame(line);
	const UI::Rect box = context.GetLayout(plot).borderBox;
	const Vec2I middle(static_cast<i32>(box.x + (0.5f - view.timeMin) / (view.timeMax - view.timeMin) * box.w),
					   static_cast<i32>(box.y + (1.0f - (0.5f - view.valueMin) / (view.valueMax - view.valueMin)) * box.h));
	for (u32 click = 0; click < 2; ++click) {
		Inject(context, InputEventType::MouseMove, middle);
		Inject(context, InputEventType::MouseDown, middle);
		Inject(context, InputEventType::MouseUp, middle);
	}
	Frame();
	const auto stored = [&]() {
		for (const GraphNode& held : ContextOf(*Document(graph), 8)->blocks) {
			for (const GraphInput& input : held.inputs) {
				if (held.id == 9 && input.port == "Size") {
					return input.value;
				}
			}
		}
		return str();
	};
	AnimationCurve edited;
	ASSERT_TRUE(GraphCurves::Parse(stored(), edited, error)) << stored();
	EXPECT_EQ(edited.keys.size(), 3u);
	EXPECT_TRUE(window->HasUnsavedChanges());

	UndoSystem::Undo();
	Frame();
	EXPECT_TRUE(stored().empty());

	context.ClosePopup(popup);
	Frame();
	const UI::ElementHandle color = FindByText(context, context.GetRoot(), "Multiply Color Over Life");
	ASSERT_TRUE(context.IsAlive(color));
	ClickElement(context, color);
	Frame();
	Frame();
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Alpha 4")));
}

TEST_F(EffectGraphWindowFixture, CustomHLSLCodeMakesPorts) {
	ASSERT_NE(_moduleBase, nullptr);
	str text = Fountain("1,0,0,1");
	const str block = R"({"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"from":[10,"Out"]}}})";
	text.replace(text.find(block), block.size(),
				 R"({"id":9,"def":"Block/CustomHLSL","settings":{"HLSL Code":"void Grow(inout EffectAttributes attributes) { attributes.size = 1; }"}})");
	const Guid graph = StageGraph(text);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Grow")));

	const UI::ElementHandle title = FindByText(context, context.GetRoot(), "Custom HLSL");
	ASSERT_TRUE(context.IsAlive(title));
	ClickElement(context, title);
	Frame();
	Frame();
	const auto fieldAfter = [&context](std::string_view label) {
		const UI::Element* held = context.GetElement(FindByText(context, context.GetRoot(), label));
		return held && held->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(held->nextSibling) : UI::ElementHandle{};
	};
	ASSERT_TRUE(context.IsAlive(fieldAfter("HLSL Code")));
	EXPECT_TRUE(context.IsAlive(fieldAfter("Shader File")));
	EXPECT_TRUE(context.IsAlive(fieldAfter("Block Name")));

	ClickElement(context, fieldAfter("HLSL Code"));
	InputEvent all{};
	all.type = InputEventType::KeyDown;
	all.key = Shalltear::Key::A;
	all.mods = KeyMod::Ctrl;
	context.InjectInput(all);
	context.Update();
	Type(context, "void Spread(inout EffectAttributes attributes, float spread)");
	Key(context, Shalltear::Key::Enter);
	Type(context, "{ attributes.size = spread; }");
	ClickElement(context, fieldAfter("Block Name"));
	Frame();
	Frame();
	str code;
	for (const GraphNode& held : ContextOf(*Document(graph), 8)->blocks) {
		for (const GraphSetting& setting : held.settings) {
			if (held.id == 9 && setting.key == GraphCustomHLSLCode) {
				LinearAllocator arena(setting.value.size() + 64, MemTag::Editor);
				code = str(JSONParse(setting.value, arena).AsString());
			}
		}
	}
	EXPECT_EQ(code, "void Spread(inout EffectAttributes attributes, float spread)\n{ attributes.size = spread; }");
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Spread")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "spread")));

	UndoSystem::Undo();
	Frame();
	Frame();
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Grow")));
}

TEST_F(EffectGraphWindowFixture, DiskEditReshapesBlock) {
	ASSERT_NE(_moduleBase, nullptr);
	const std::filesystem::path file = _root / "Assets" / "Spread.hlsl";
	std::filesystem::create_directories(file.parent_path());
	const auto write = [&file](std::string_view code) {
		std::ofstream out(file, std::ios::binary);
		out.write(code.data(), static_cast<std::streamsize>(code.size()));
	};
	write("void Spread(inout EffectAttributes attributes, float amount) { attributes.size = amount; }\n");
	AssetDatabase::ScanProjectDirectory();
	const Guid shader = AssetDatabase::LookupGuid(Path("project://Assets/Spread.hlsl"));
	ASSERT_TRUE(shader.IsValid());
	str text = Fountain("1,0,0,1");
	const str block = R"({"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"from":[10,"Out"]}}})";
	text.replace(text.find(block), block.size(), std::format(R"({{"id":9,"def":"Block/CustomHLSL","settings":{{"Shader File":"{}"}}}})", shader.ToString()));
	StageGraph(text);
	EditorAssetWatch::Initialize();
	EditorAssetWatch::PollChanges();
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "amount")));

	write("void Spread(inout EffectAttributes attributes, float width) { attributes.size = width; }\n");
	EditorAssetWatch::NotifyChanged(Path("project://Assets/Spread.hlsl"));
	EditorAssetWatch::PollChanges();
	Frame();
	Frame();
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "width")));
	EXPECT_FALSE(context.IsAlive(FindByText(context, context.GetRoot(), "amount")));
	EditorAssetWatch::PollChanges();
	EditorAssetWatch::Terminate();
}

TEST_F(EffectGraphWindowFixture, WindowsShareOneDocument) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_NE(Open(), nullptr);
	Handle<EditorPanel> second = PanelManager::OpenPanel(WindowType());
	ASSERT_TRUE(second.IsValid());
	const FieldInfo* field = FindField(WindowType(), StringHash("graph"));
	ASSERT_NE(field, nullptr);
	*static_cast<Guid*>(field->addrFn(second.Get())) = graph;
	second.Get()->OnOpen();
	Frame();
	Frame();
	ASSERT_EQ(Windows().size(), 2u);

	NodeShaderDocuments::BeginEdit(graph, "Rename");
	NodeShaderDocuments::Get(graph)->properties[0].name = "Glow";
	NodeShaderDocuments::EndEdit(graph);
	Frame();
	Frame();
	u32 canvases = 0;
	for (UI::Context* context : CanvasContexts()) {
		const u32 here = CountByClass(*context, context->GetRoot(), EditorTheme::Class::Graph);
		canvases += here;
		u32 shown = 0;
		for (u32 n = 0; n < here; ++n) {
			const UI::ElementHandle canvas = FindByClass(*context, context->GetRoot(), EditorTheme::Class::Graph, n);
			shown += context->IsAlive(FindByText(*context, canvas, "Glow")) ? 1u : 0u;
		}
		EXPECT_EQ(shown, here);
	}
	EXPECT_EQ(canvases, 2u);
	PanelManager::ClosePanel(second);
	Frame();
	EXPECT_TRUE(NodeShaderDocuments::IsOpen(graph));
}

TEST_F(EffectGraphWindowFixture, UnsavedEditReachesPlayingEffect) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device";
	}
	str joined = Fountain("1,0,0,1");
	const Guid graph = StageGraph(joined);
	_imported = AssetDatabase::LoadAsset(Path(GRAPH_PATH));
	if (!_imported) {
		GTEST_SKIP() << "import failed (dxcompiler.dll unavailable?)";
	}
	_anyMaterial = New<MaterialAsset>(MemTag::Asset);
	_anyMaterial->shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"))));
	_staged = BuildStage(_anyMaterial, _stage);
	ASSERT_TRUE(_staged);
	_stage.quad->SetActive(false);
	Handle<SceneObject> object = SceneSystem::CreateObject("Effect", _stage.scene);
	Handle<Component> component = object->AddComponent(TypeRegistry::FindType(StringHash("VisualEffect")));
	ASSERT_TRUE(component.IsValid());
	static_cast<VisualEffect*>(component.Get())->asset = Handle<VisualEffectAsset>(static_cast<VisualEffectAsset*>(_imported));
	SceneSystem::EndFrame();

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	auto frame = [&]() {
		Frame();
		return RenderStage(_stage, pixels, size);
	};
	ASSERT_TRUE(frame());
	ASSERT_GT(CenterTexel(pixels, size).x, 150.0f);

	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	const str saved = FileSystem::ReadTextFile(Path(GRAPH_PATH), MemTag::Editor);

	const u64 before = ShaderCompileQueue::GetStats().submitted;
	NodeShaderDocuments::BeginEdit(graph, "Move");
	GraphDocumentOps::MoveNode(*Document(graph), 7, Vec2(80.0f, 600.0f));
	NodeShaderDocuments::EndEdit(graph);
	for (u32 i = 0; i < 3; ++i) {
		frame();
	}
	EXPECT_EQ(ShaderCompileQueue::GetStats().submitted, before);

	NodeShaderDocuments::BeginEdit(graph, "Default");
	Document(graph)->properties[0].defaultValue = "[0,1,0,1]";
	NodeShaderDocuments::EndEdit(graph);
	bool reached = false;
	for (u32 attempt = 0; attempt < 400 && !reached; ++attempt) {
		reached = frame() && CenterTexel(pixels, size).y > 150.0f && CenterTexel(pixels, size).x < 60.0f;
		if (!reached) {
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
	}
	EXPECT_TRUE(reached);
	EXPECT_GT(ShaderCompileQueue::GetStats().submitted, before);
	EXPECT_EQ(FileSystem::ReadTextFile(Path(GRAPH_PATH), MemTag::Editor), saved);
	EXPECT_TRUE(window->HasUnsavedChanges());
}

TEST_F(EffectGraphWindowFixture, UnsavedSettingsReachPlayingEffect) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device";
	}
	const Guid graph = StageGraph(R"({"_type":"EffectGraph","version":2,"nextId":20,"settings":{},"properties":[],"nodes":[],
"contexts":[
{"id":2,"def":"Context/Spawn","x":0,"y":0,"blocks":[{"id":3,"def":"Block/Burst","in":{"Count":{"value":[1]}}}]},
{"id":4,"def":"Context/Initialize","x":0,"y":220,"settings":{"Capacity":4},"flow":[[2,"In"]],"blocks":[
{"id":5,"def":"Block/SetAttribute","settings":{"Attribute":"position"},"in":{"Position":{"value":[0,0,0.5]}}},
{"id":6,"def":"Block/SetAttribute","settings":{"Attribute":"size"},"in":{"Size":{"value":[1]}}},
{"id":12,"def":"Block/SetAttribute","settings":{"Attribute":"velocity"},"in":{"Velocity":{"value":[0,4,0]}}}]},
{"id":7,"def":"Context/Update","x":0,"y":560,"flow":[[4,"In"]],"blocks":[]},
{"id":8,"def":"Context/OutputParticlePrimitive","x":0,"y":760,"flow":[[7,"In"]],"blocks":[
{"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"value":[1,0,0]}}}]}],
"groups":[],"notes":[]}
)");
	_imported = AssetDatabase::LoadAsset(Path(GRAPH_PATH));
	if (!_imported) {
		GTEST_SKIP() << "import failed (dxcompiler.dll unavailable?)";
	}
	_anyMaterial = New<MaterialAsset>(MemTag::Asset);
	_anyMaterial->shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"))));
	_staged = BuildStage(_anyMaterial, _stage);
	ASSERT_TRUE(_staged);
	_stage.quad->SetActive(false);
	Handle<SceneObject> object = SceneSystem::CreateObject("Effect", _stage.scene);
	Handle<Component> component = object->AddComponent(TypeRegistry::FindType(StringHash("VisualEffect")));
	ASSERT_TRUE(component.IsValid());
	static_cast<VisualEffect*>(component.Get())->asset = Handle<VisualEffectAsset>(static_cast<VisualEffectAsset*>(_imported));
	SceneSystem::EndFrame();

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	auto frame = [&]() {
		Frame();
		return RenderStage(_stage, pixels, size);
	};
	ASSERT_NE(Open(), nullptr);
	for (u32 i = 0; i < 3; ++i) {
		ASSERT_TRUE(frame());
	}
	ASSERT_GT(CenterTexel(pixels, size).x, 150.0f);

	NodeShaderDocuments::BeginEdit(graph, "Prewarm");
	EffectGraphEditOps::SetPrewarm(*Document(graph), EffectGraphEditOps::PrewarmField::TotalTime, 1.0);
	EffectGraphEditOps::SetPrewarm(*Document(graph), EffectGraphEditOps::PrewarmField::StepCount, 10.0);
	NodeShaderDocuments::EndEdit(graph);
	bool reached = false;
	for (u32 attempt = 0; attempt < 400 && !reached; ++attempt) {
		reached = frame() && CenterTexel(pixels, size).x < 60.0f;
		if (!reached) {
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
	}
	EXPECT_TRUE(reached);
}

namespace {

	class VisualEffectLiveEditFixture : public EffectGraphWindowFixture {
	protected:
		Guid _graph{};
		EditorPanel* _window = nullptr;
		Vector<u8> _pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I _size{};

		bool Shown() {
			EditorAssetWatch::PollChanges();
			Frame();
			return RenderStage(_stage, _pixels, _size);
		}

		bool ShowsGreen() { return CenterTexel(_pixels, _size).y > 150.0f && CenterTexel(_pixels, _size).x < 60.0f; }
		bool ShowsRed() { return CenterTexel(_pixels, _size).x > 150.0f && CenterTexel(_pixels, _size).y < 60.0f; }

		template <typename Seen>
		bool Until(Seen seen) {
			for (u32 attempt = 0; attempt < 400; ++attempt) {
				if (Shown() && seen()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
			}
			return false;
		}

		bool PlayGreenUnsaved(const str& text) {
			if (!RenderSystem::IsRenderingAvailable()) {
				return false;
			}
			_graph = StageGraph(text);
			_imported = AssetDatabase::LoadAsset(Path(GRAPH_PATH));
			if (!_imported) {
				return false;
			}
			_anyMaterial = New<MaterialAsset>(MemTag::Asset);
			_anyMaterial->shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"))));
			_staged = BuildStage(_anyMaterial, _stage);
			if (!_staged) {
				return false;
			}
			_stage.quad->SetActive(false);
			Handle<SceneObject> object = SceneSystem::CreateObject("Effect", _stage.scene);
			Handle<Component> component = object->AddComponent(TypeRegistry::FindType(StringHash("VisualEffect")));
			if (!component.IsValid()) {
				return false;
			}
			static_cast<VisualEffect*>(component.Get())->asset = Handle<VisualEffectAsset>(static_cast<VisualEffectAsset*>(_imported));
			SceneSystem::EndFrame();
			EditorAssetWatch::PollChanges();
			_window = Open();
			if (!_window || !Until([this]() { return ShowsRed(); })) {
				return false;
			}
			NodeShaderDocuments::BeginEdit(_graph, "Default");
			Document(_graph)->properties[0].defaultValue = "[0,1,0,1]";
			NodeShaderDocuments::EndEdit(_graph);
			return Until([this]() { return ShowsGreen(); });
		}

		void Wait(f64 seconds) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (std::chrono::steady_clock::now() < until) {
				Shown();
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
			}
		}
	};

} // anonymous namespace

TEST_F(VisualEffectLiveEditFixture, DiskEditKeepsWindowEditPlaying) {
	ASSERT_NE(_moduleBase, nullptr);
	const std::filesystem::path file = _root / "Assets" / "Grow.hlsl";
	std::filesystem::create_directories(file.parent_path());
	const auto write = [&file](std::string_view code) {
		std::ofstream out(file, std::ios::binary);
		out.write(code.data(), static_cast<std::streamsize>(code.size()));
	};
	write("void Grow(inout EffectAttributes attributes) { attributes.size = 1; }\n");
	AssetDatabase::ScanProjectDirectory();
	const Guid shader = AssetDatabase::LookupGuid(Path("project://Assets/Grow.hlsl"));
	ASSERT_TRUE(shader.IsValid());
	str text = Fountain("1,0,0,1");
	const str tinted = R"({"id":9,"def":"Block/SetAttribute","settings":{"Attribute":"color"},"in":{"Color":{"from":[10,"Out"]}}})";
	ASSERT_NE(text.find(tinted), str::npos);
	text.insert(text.find(tinted) + tinted.size(), std::format(R"(,{{"id":11,"def":"Block/CustomHLSL","settings":{{"Shader File":"{}"}}}})", shader.ToString()));
	if (!PlayGreenUnsaved(text)) {
		GTEST_SKIP();
	}

	write("void Grow(inout EffectAttributes attributes) { attributes.size = 1.0; }\n");
	EditorAssetWatch::NotifyChanged(Path("project://Assets/Grow.hlsl"));
	Wait(1.0);
	EXPECT_TRUE(ShowsGreen());
	EXPECT_TRUE(_window->HasUnsavedChanges());
}

TEST_F(VisualEffectLiveEditFixture, ImportUnderWindowRestoresGraph) {
	ASSERT_NE(_moduleBase, nullptr);
	const str text = Fountain("1,0,0,1");
	if (!PlayGreenUnsaved(text)) {
		GTEST_SKIP();
	}
	std::ofstream out(_root / "Assets" / GRAPH_NAME, std::ios::binary);
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	out.close();
	EditorAssetWatch::NotifyChanged(Path(GRAPH_PATH));
	ASSERT_TRUE(Until([this]() { return ShowsRed(); }));

	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	const UI::ElementHandle compile = FindByText(context, context.GetRoot(), "Compile");
	ASSERT_TRUE(context.IsAlive(compile));
	ClickElement(context, compile);
	EXPECT_TRUE(Until([this]() { return ShowsGreen(); }));
}

namespace {

	str SentFountain() {
		return str(R"({"_type":"EffectGraph","version":2,"nextId":20,"settings":{},"properties":[],"nodes":[],
"contexts":[
{"id":11,"def":"Context/Event","x":0,"y":780,"settings":{"Event Name":"Burst"},"blocks":[]},
{"id":2,"def":"Context/Spawn","x":0,"y":0,"flow":[[11,"Start"]],"blocks":[{"id":3,"def":"Block/Burst","in":{"Count":{"value":[1]}}}]},
{"id":4,"def":"Context/Initialize","x":0,"y":220,"settings":{"Capacity":4},"flow":[[2,"In"]],"blocks":[
{"id":5,"def":"Block/SetAttribute","settings":{"Attribute":"lifetime"},"in":{"Lifetime":{"value":[100]}}}]},
{"id":8,"def":"Context/OutputParticlePrimitive","x":0,"y":560,"flow":[[4,"In"]],"blocks":[]}],
"groups":[],"notes":[]}
)");
	}

	bool ShowsButton(UI::Context& context, UI::ElementHandle root, std::string_view word) {
		const UI::ElementHandle label = FindByText(context, root, word);
		const UI::Element* element = label.IsValid() ? context.GetElement(label) : nullptr;
		if (!element || element->parent == UI::INVALID_UI_INDEX) {
			return false;
		}
		return context.GetStyle(context.MakeHandle(element->parent)).display != UI::Display::None;
	}

	class EffectGraphAttachFixture : public EffectGraphWindowFixture {
	protected:
		Handle<SceneObject> Effect(std::string_view name, bool playsGraph = true) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _stage.scene);
			Handle<Component> component = object->AddComponent(TypeRegistry::FindType(StringHash("VisualEffect")));
			if (component.IsValid() && playsGraph) {
				static_cast<VisualEffect*>(component.Get())->asset = Handle<VisualEffectAsset>(static_cast<VisualEffectAsset*>(_imported));
			}
			SceneSystem::EndFrame();
			return object;
		}

		bool Ready(const str& graph) {
			if (!RenderSystem::IsRenderingAvailable()) {
				return false;
			}
			StageGraph(graph);
			_imported = AssetDatabase::LoadAsset(Path(GRAPH_PATH));
			if (!_imported) {
				return false;
			}
			_anyMaterial = New<MaterialAsset>(MemTag::Asset);
			_anyMaterial->shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"))));
			_staged = BuildStage(_anyMaterial, _stage);
			return _staged;
		}

		static void Select(Handle<SceneObject> object) {
			EditorSelection::Select(Handle<Object>(static_cast<Object*>(object.Get())));
			Frame();
			Frame();
		}

		static UI::Context* Host() {
			const Vector<UI::Context*> contexts = CanvasContexts();
			return contexts.empty() ? nullptr : contexts[0];
		}

		static UI::ElementHandle Board(UI::Context& context) { return FindByClass(context, context.GetRoot(), EditorTheme::Class::GraphBoard, 0); }

		static bool BoardSays(UI::Context& context, std::string_view text) { return context.IsAlive(FindByText(context, Board(context), text)); }

		static str TextsUnder(UI::Context& context, UI::ElementHandle root) {
			const UI::Element* pod = context.IsAlive(root) ? context.GetElement(root) : nullptr;
			if (!pod) {
				return {};
			}
			str texts(context.GetTextString(root));
			for (u32 child = pod->firstChild; child != UI::INVALID_UI_INDEX;) {
				const UI::ElementHandle handle = context.MakeHandle(child);
				const str inner = TextsUnder(context, handle);
				texts += !texts.empty() && !inner.empty() ? " | " + inner : inner;
				child = context.GetElement(handle)->nextSibling;
			}
			return texts;
		}
	};

} // anonymous namespace

TEST_F(EffectGraphAttachFixture, SelectionAttachesLockHolds) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!Ready(Fountain("1,0,0,1"))) {
		GTEST_SKIP();
	}
	Handle<SceneObject> alpha = Effect("Alpha");
	Handle<SceneObject> beta = Effect("Beta");
	Handle<SceneObject> plain = Effect("Plain", false);
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(context->IsAlive(Board(*context)));
	EXPECT_TRUE(BoardSays(*context, "Select an object running this effect"));

	Select(alpha);
	EXPECT_TRUE(BoardSays(*context, "Alpha"));
	EXPECT_TRUE(ShowsButton(*context, context->GetRoot(), "Detach"));
	Select(beta);
	EXPECT_TRUE(BoardSays(*context, "Beta"));
	Select(plain);
	EXPECT_TRUE(BoardSays(*context, "Beta"));

	ClickElement(*context, FindByText(*context, context->GetRoot(), "Detach"));
	Frame();
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Select an object running this effect"));
	EXPECT_TRUE(ShowsButton(*context, context->GetRoot(), "Attach to Selection"));
	Select(beta);
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Detach"));
	Frame();
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Select an object running this effect"));

	ClickElement(*context, FindByText(*context, context->GetRoot(), "Attach to Selection"));
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Beta"));
	ClickElement(*context, FindByClass(*context, context->GetRoot(), StringHash("slt-checkbox"), 1));
	Select(alpha);
	EXPECT_TRUE(BoardSays(*context, "Beta"));
	ClickElement(*context, FindByClass(*context, context->GetRoot(), StringHash("slt-checkbox"), 1));

	SceneSystem::Destroy(beta);
	SceneSystem::EndFrame();
	Frame();
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Select an object running this effect"));
	EditorSelection::Clear();
}

TEST_F(EffectGraphAttachFixture, RecordedSystemWarnsUntilWindowIsAttached) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!Ready(Fountain("1,0,0,1"))) {
		GTEST_SKIP();
	}
	Handle<SceneObject> alpha = Effect("Alpha");
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	const Color warn = EditorTheme::GetColor(EditorTheme::ColorToken::StatusWarn);
	const auto initialize = [&]() { return context->GetStyle(NodeShowing(*context, "Initialize Particle")).borderColor; };
	EXPECT_EQ(initialize(), warn);
	EXPECT_TRUE(context->IsAlive(FindByText(*context, context->GetRoot(), "1 warning")));

	Select(alpha);
	ASSERT_TRUE(BoardSays(*context, "Alpha"));
	EXPECT_NE(initialize(), warn);

	SceneSystem::Destroy(alpha);
	SceneSystem::EndFrame();
	Frame();
	Frame();
	EXPECT_EQ(initialize(), warn);
	EditorSelection::Clear();
}

TEST_F(EffectGraphAttachFixture, BoardDrivesAttachedEffect) {
	ASSERT_NE(_moduleBase, nullptr);
	str named = Fountain("1,0,0,1");
	const str capacity = R"("settings":{"Capacity":4})";
	named.replace(named.find(capacity), capacity.size(), R"("settings":{"Capacity":4,"System Name":"Sparks"})");
	if (!Ready(named)) {
		GTEST_SKIP();
	}
	Handle<SceneObject> alpha = Effect("Alpha");
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	Select(alpha);
	const UI::ElementHandle board = Board(*context);
	ASSERT_TRUE(BoardSays(*context, "Alpha"));
	EXPECT_TRUE(context->IsAlive(FindByText(*context, board, "OnPlay")));
	EXPECT_TRUE(context->IsAlive(FindByText(*context, board, "Send")));

	UI::ElementHandle count;
	for (u32 frame = 0; frame < 60 && !context->IsAlive(count); ++frame) {
		Frame();
		count = FindByText(*context, Board(*context), "1 / 4, 0 dropped");
	}
	ASSERT_TRUE(context->IsAlive(count));
	const UI::ElementHandle line = context->MakeHandle(context->GetElement(count)->parent);
	EXPECT_TRUE(context->IsAlive(FindByText(*context, line, "Sparks")));

	ASSERT_TRUE(ShowsButton(*context, Board(*context), "Pause"));
	ClickElement(*context, FindByText(*context, Board(*context), "Pause"));
	Frame();
	EXPECT_TRUE(ShowsButton(*context, Board(*context), "Play"));
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Detach"));
	Frame();
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Attach to Selection"));
	Frame();
	EXPECT_TRUE(ShowsButton(*context, Board(*context), "Pause"));
	EditorSelection::Clear();
}

TEST_F(EffectGraphAttachFixture, EventsSendInGraphReachesEveryEffect) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!Ready(SentFountain())) {
		GTEST_SKIP();
	}
	Handle<SceneObject> alpha = Effect("Alpha");
	Handle<SceneObject> beta = Effect("Beta");
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	const auto counts = [&](Handle<SceneObject> object, std::string_view text) {
		Select(object);
		for (u32 frame = 0; frame < 60; ++frame) {
			Frame();
			if (context->IsAlive(FindByText(*context, Board(*context), text))) {
				return testing::AssertionSuccess();
			}
		}
		return testing::AssertionFailure() << TextsUnder(*context, Board(*context));
	};
	EXPECT_TRUE(counts(alpha, "0 / 4, 0 dropped"));

	const UI::ElementHandle send = FindByClass(*context, context->GetRoot(), EditorTheme::Class::GraphSettingAction, 0);
	ASSERT_TRUE(context->IsAlive(send));
	const UI::Rect at = context->GetLayout(send).borderBox;
	const str clicked = std::format("{} at {},{} {}x{} in {}x{}", DescribeElement(*context, send), at.x, at.y, at.w, at.h,
									context->GetLayout(context->GetRoot()).borderBox.w, context->GetLayout(context->GetRoot()).borderBox.h);
	ASSERT_TRUE(context->HitTest(Vec2{at.x + at.w * 0.5f, at.y + at.h * 0.5f}) == send) << clicked;
	ClickElement(*context, send);
	EXPECT_TRUE(counts(alpha, "1 / 4, 0 dropped"));
	EXPECT_TRUE(counts(beta, "1 / 4, 0 dropped"));
	EditorSelection::Clear();
}

TEST_F(EffectGraphAttachFixture, BoardRecordsAndAppliesBounds) {
	ASSERT_NE(_moduleBase, nullptr);
	if (!Ready(Fountain("1,0,0,1"))) {
		GTEST_SKIP();
	}
	const Guid graph = AssetDatabase::LookupGuid(Path(GRAPH_PATH));
	Handle<SceneObject> alpha = Effect("Alpha");
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	Select(alpha);
	ASSERT_TRUE(BoardSays(*context, "Bounds Recording"));
	const auto red = []() {
		Vector<DebugLine> lines{GetMemoryResource(MemTag::Editor)};
		Vector<DebugLine> overlay{GetMemoryResource(MemTag::Editor)};
		GizmoDrawContext gizmo;
		gizmo.lines = &lines;
		gizmo.overlayLines = &overlay;
		EditorGizmos::DrawSceneGizmos(gizmo);
		return static_cast<u32>(std::count_if(lines.begin(), lines.end(), [](const DebugLine& line) {
			return line.color == EditorTheme::GetColor32(EditorTheme::ColorToken::GizmoEffectRecordedBounds);
		}));
	};
	EXPECT_EQ(red(), 0u);

	ClickElement(*context, FindByText(*context, Board(*context), "Record"));
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Recording bounds..."));
	EditorSelection::Clear();
	bool drawn = false;
	for (u32 attempt = 0; attempt < 400 && !drawn; ++attempt) {
		Frame();
		drawn = red() == 12u;
		if (!drawn) {
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
	}
	ASSERT_TRUE(drawn);

	const auto stored = [&](std::string_view port) {
		const GraphContext* initialize = ContextOf(*Document(graph), 4);
		Vec3 value(std::nanf(""), std::nanf(""), std::nanf(""));
		for (const GraphInput& input : initialize ? std::span<const GraphInput>(initialize->node.inputs) : std::span<const GraphInput>()) {
			if (input.port != port) {
				continue;
			}
			LinearAllocator arena(input.value.size() + 64, MemTag::Editor);
			const JSONValue parsed = JSONParse(input.value, arena);
			if (parsed.IsArray() && parsed.Size() == 3) {
				value = Vec3(static_cast<f32>(parsed[0].AsNumber()), static_cast<f32>(parsed[1].AsNumber()), static_cast<f32>(parsed[2].AsNumber()));
			}
		}
		return value;
	};
	ClickElement(*context, FindByText(*context, Board(*context), "Apply Bounds"));
	Frame();
	const Vec3 center = stored(GraphEffectBoundsCenter);
	const Vec3 size = stored(GraphEffectBoundsSize);
	const f32 across = std::sqrt(3.0f);
	for (u32 axis = 0; axis < 3; ++axis) {
		EXPECT_NEAR(center[axis], axis == 2 ? 0.5f : 0.0f, 1e-4f) << axis;
		EXPECT_NEAR(size[axis], across, 1e-4f) << axis;
	}
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_TRUE(std::isnan(stored(GraphEffectBoundsCenter).x));

	ClickElement(*context, FindByText(*context, Board(*context), "Stop Recording"));
	Frame();
	EXPECT_TRUE(BoardSays(*context, "Bounds Recording"));
	EXPECT_EQ(red(), 0u);
}

TEST_F(EffectGraphWindowFixture, AssetsInspectorAndWindowEditOneDocument) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_TRUE(graph.IsValid());
	const auto onDisk = [&]() { return FileSystem::ReadTextFile(Path(GRAPH_PATH), MemTag::Editor).find("ignoreTimeScale") != str::npos; };
	ASSERT_FALSE(onDisk());
	EditorSelection::SelectAsset(graph);
	MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, &fonts);
	EditorTheme::ApplyToContext(context);
	Handle<EditorPanel> inspector = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	const auto rebuild = [&]() {
		PanelManager::TickAll(1.0f / 60.0f);
		context.ClearChildren(root);
		inspector.Get()->BuildContent(context, root);
		context.Update();
	};
	rebuild();
	for (const char* row : {"Update mode", "Fixed Delta Time", "Culling Flags", "Initial state", "PreWarm Step Count", "Instancing", "Instancing Mode"}) {
		EXPECT_TRUE(context.IsAlive(FindByText(context, root, row))) << row;
	}
	const auto ignore = [&]() { return FindByClass(context, root, StringHash("slt-checkbox"), 2); };
	ASSERT_TRUE(context.IsAlive(ignore()));
	ClickElement(context, ignore());
	Frame();
	EXPECT_TRUE(onDisk());
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_FALSE(onDisk());

	EditorPanel* window = Open();
	ASSERT_NE(window, nullptr);
	rebuild();
	ASSERT_TRUE(context.IsAlive(ignore()));
	ClickElement(context, ignore());
	Frame();
	const GraphDocument* document = Document(graph);
	ASSERT_NE(document, nullptr);
	EXPECT_TRUE(GraphEffectTarget::Timing(*document).ignoreTimeScale);
	EXPECT_TRUE(window->HasUnsavedChanges());
	EXPECT_FALSE(onDisk());
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_FALSE(GraphEffectTarget::Timing(*Document(graph)).ignoreTimeScale);
	EditorSelection::Clear();
	PanelManager::ClosePanel(inspector);
}

TEST_F(EffectGraphWindowFixture, AssetsInspectorFollowsEveryChangeOfDocument) {
	ASSERT_NE(_moduleBase, nullptr);
	const Guid graph = StageGraph(Fountain("1,0,0,1"));
	ASSERT_TRUE(graph.IsValid());
	EditorSelection::SelectAsset(graph);
	Handle<EditorPanel> inspector = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	for (u32 frame = 0; frame < 3; ++frame) {
		Frame();
	}
	UI::Context* shown = UISystem::GetContext(_main);
	ASSERT_NE(shown, nullptr);
	UI::Context& context = *shown;
	const auto fieldAfter = [](UI::Context& in, std::string_view label) {
		const UI::Element* held = in.GetElement(FindByText(in, in.GetRoot(), label));
		return held && held->nextSibling != UI::INVALID_UI_INDEX ? in.MakeHandle(held->nextSibling) : UI::ElementHandle{};
	};
	const auto steps = [&](std::string_view count) {
		for (u32 frame = 0; frame < 3; ++frame) {
			Frame();
		}
		return context.IsAlive(FindByText(context, fieldAfter(context, "PreWarm Step Count"), count));
	};
	ASSERT_TRUE(steps("0"));

	const UI::ElementHandle total = FindByClass(context, fieldAfter(context, "PreWarm Total Time"), StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(total));
	ClickElement(context, total);
	InputEvent all{};
	all.type = InputEventType::KeyDown;
	all.key = Shalltear::Key::A;
	all.mods = KeyMod::Ctrl;
	context.InjectInput(all);
	context.Update();
	Type(context, "2");
	Key(context, Shalltear::Key::Enter);
	ASSERT_EQ(GraphEffectTarget::Timing(*Document(graph)).prewarmStepCount, 1u);
	EXPECT_TRUE(steps("1"));
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_TRUE(steps("0"));

	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& canvas = *CanvasContexts()[0];
	const auto popup = [&](UI::Context& in) { return FindByClass(in, fieldAfter(in, "Culling Flags"), StringHash("slt-dropdown"), 0); };
	ClickElement(canvas, popup(canvas));
	ASSERT_EQ(canvas.GetOpenPopupCount(), 1u);
	ClickElement(canvas, NthChildOf(canvas, canvas.GetOpenPopup(0), 2));
	ASSERT_EQ(GraphEffectTarget::Timing(*Document(graph)).cullingFlags, 0u);
	for (u32 frame = 0; frame < 3; ++frame) {
		Frame();
	}
	EXPECT_TRUE(context.IsAlive(FindByText(context, popup(context), "Always simulate and update bounds")));
	EditorSelection::Clear();
	PanelManager::ClosePanel(inspector);
}

TEST_F(EffectGraphWindowFixture, SystemNameRenamesOnBox) {
	ASSERT_NE(_moduleBase, nullptr);
	str twins = Fountain("1,0,0,1");
	const str initialize = R"({"id":4,"def":"Context/Initialize","x":0,"y":220,"settings":{"Capacity":4},)";
	ASSERT_NE(twins.find(initialize), str::npos);
	twins.replace(twins.find(initialize), initialize.size(), R"({"id":4,"def":"Context/Initialize","x":0,"y":220,"settings":{"Capacity":4,"System Name":"Sparks"},)");
	const str tail = "],\n\"groups\":[],\"notes\":[]}";
	ASSERT_NE(twins.find(tail), str::npos);
	twins.insert(twins.find(tail), R"(,{"id":14,"def":"Context/Initialize","x":600,"y":220,"settings":{"Capacity":4,"System Name":"Sparks"},"flow":[[2,"In"]],"blocks":[]},
{"id":15,"def":"Context/OutputParticlePrimitive","x":600,"y":760,"flow":[[14,"In"]],"blocks":[]})");
	const Guid graph = StageGraph(twins);
	ASSERT_NE(Open(), nullptr);
	ASSERT_FALSE(CanvasContexts().empty());
	UI::Context& context = *CanvasContexts()[0];
	Frame();
	ASSERT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Sparks")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Sparks (1)")));

	DoubleClickElement(context, FindByText(context, context.GetRoot(), "Sparks"));
	Frame();
	const UI::ElementHandle field = context.GetFocused();
	ASSERT_TRUE(context.HasTextInput(field));
	Type(context, "Smoke");
	Key(context, Shalltear::Key::Enter);
	Frame();
	const GraphNode* renamed = GraphDocumentOps::FindNode(*Document(graph), 4);
	ASSERT_NE(renamed, nullptr);
	const GraphSetting* name = GraphWindowParts::FindSetting(renamed->settings, "System Name");
	ASSERT_NE(name, nullptr);
	EXPECT_EQ(GraphWindowParts::ReadString(name->value), "Smoke");
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Smoke")));
	EXPECT_TRUE(context.IsAlive(FindByText(context, context.GetRoot(), "Sparks")));
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(GraphWindowParts::ReadString(GraphWindowParts::FindSetting(GraphDocumentOps::FindNode(*Document(graph), 4)->settings, "System Name")->value),
			  "Sparks");
}

TEST_F(EffectGraphAttachFixture, AttachedShapeDragWritesPort) {
	ASSERT_NE(_moduleBase, nullptr);
	str graph = Fountain("1,0,0,1");
	const str update = R"({"id":7,"def":"Context/Update","x":0,"y":560,"blocks":[]})";
	graph.replace(graph.find(update), update.size(),
				  R"({"id":7,"def":"Context/Update","x":0,"y":560,"flow":[[4,"In"]],"blocks":[{"id":12,"def":"Block/CollisionShape","settings":{"Shape":"Plane"}}]})");
	const str output = R"("flow":[[4,"In"]],"blocks":[
{"id":9)";
	graph.replace(graph.find(output), output.size(), R"("flow":[[7,"In"]],"blocks":[
{"id":9)");
	if (!Ready(graph)) {
		GTEST_SKIP();
	}
	Handle<SceneObject> alpha = Effect("Alpha");
	alpha->SetLocalPosition(Vec3(3.0f, 0.0f, 0.0f));
	ASSERT_NE(Open(), nullptr);
	UI::Context* context = Host();
	ASSERT_NE(context, nullptr);
	const StringHash source("VisualEffects.BlockShape");
	const auto shapes = [&]() {
		Vector<OfferedSceneHandle> offered{GetMemoryResource(MemTag::Editor)};
		EditorGizmos::GatherSceneHandles(offered);
		std::erase_if(offered, [&](const OfferedSceneHandle& entry) { return entry.source != source; });
		return offered;
	};
	const auto block = [&]() { return FindByClass(*context, NodeShowing(*context, "Update Particle"), EditorTheme::Class::GraphBlock, 0); };
	ClickElement(*context, FindByText(*context, block(), "Collision Shape Plane"));
	Frame();
	EXPECT_TRUE(shapes().empty());

	Select(alpha);
	ASSERT_TRUE(BoardSays(*context, "Alpha"));
	ClickElement(*context, FindByText(*context, block(), "Collision Shape Plane"));
	Frame();
	const Vector<OfferedSceneHandle> offered = shapes();
	ASSERT_EQ(offered.size(), 2u);
	const auto position = std::find_if(offered.begin(), offered.end(), [](const OfferedSceneHandle& entry) { return entry.handle.kind == SceneHandleKind::Position; });
	ASSERT_NE(position, offered.end());
	EXPECT_NEAR(Vec3::Distance(EditorGizmoOps::SceneHandleOrigin(position->handle), Vec3(3.0f, 0.0f, 0.0f)), 0.0f, 1.0e-4f);

	SceneHandle moved = position->handle;
	moved.position = Vec3(0.0f, 2.0f, 0.0f);
	const Guid guid = AssetDatabase::LookupGuid(Path(GRAPH_PATH));
	const auto held = [&]() {
		const GraphDocument* document = Document(guid);
		const GraphContext* stack = document ? ContextOf(*document, 7) : nullptr;
		const GraphInput* input = stack && !stack->blocks.empty() ? GraphWindowParts::FindInput(stack->blocks[0], "Position") : nullptr;
		return input ? input->value : str();
	};
	UndoSystem::BeginGroup(position->handle.label);
	EXPECT_TRUE(EditorGizmos::WriteSceneHandle(source, moved));
	UndoSystem::EndGroup();
	EXPECT_EQ(held(), "[0,2,0]");
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_TRUE(held().empty() || held() == "[0,0,0]");

	SceneSystem::Destroy(alpha);
	SceneSystem::EndFrame();
	Frame();
	Frame();
	EXPECT_TRUE(shapes().empty());
	EditorSelection::Clear();
}

#endif
