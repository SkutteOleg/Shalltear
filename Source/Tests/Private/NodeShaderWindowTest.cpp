#include "TestsPCH.h"

#include "ShadingHarness.h"
#include "TestHelperProcess.h"
#include "UITestUtil.h"

#include "GraphDefinitions.h"
#include "GraphPreview.h"
#include "GraphSurface.h"
#include "NodeShader.h"
#include "NodeShaderDocuments.h"
#include "NodeShaderEditOps.h"
#include "NodeShaderStackOps.h"
#include "NodeShaderImporter.h"
#include "NodeShaderWindow.h"
#include "ShaderSubgraphImporter.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/ShaderCompileQueue.h"
#include "Shalltear/Core/Input/InputEvent.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/EditorDocuments.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/GraphDocument.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <thread>

using namespace Shalltear;
using namespace UITest;

namespace {

	using namespace ShadingTest;
	namespace Ops = NodeShaderEditOps;
	namespace Stack = NodeShaderStackOps;

	constexpr Vec2I Parked = {-20000, -20000};
	const int OPENER_TOKEN = 0;

	class NodeShaderWindowFixture : public MaterialShadingFixture {
	protected:
		Handle<Window> _main;

		void SetUp() override {
			MaterialShadingFixture::SetUp();
			TypeRegistry::InitializePrimitives();
			NodeShaderWindow::GetReflectType();
			UISystem::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			PanelManager::Initialize();
			NodeShaderWindowRegistry::RegisterOpener(&OPENER_TOKEN);

			WindowDesc desc;
			desc.title = "NodeShaderHost";
			desc.clientSize = {1200, 800};
			desc.position = Parked;
			desc.flags = WindowFlags::Resizable;
			_main = WindowSystem::Create(desc);
			ASSERT_TRUE(_main.IsValid());
			ASSERT_NE(UISystem::CreateDockManager(_main), nullptr);
			PanelManager::SetDefaultWindow(_main);
		}

		void TearDown() override {
			PanelManager::Terminate();
			Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Debug)};
			UISystem::GetDockWindows(windows);
			for (Handle<Window> window : windows) {
				WindowSystem::RequestDestroy(window);
			}
			WindowSystem::PumpEvents();
			AssetOpeners::UnregisterOwnedBy(&OPENER_TOKEN);
			EditorDocuments::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			UISystem::Terminate();
			MaterialShadingFixture::TearDown();
		}

		static void Frame() {
			WindowSystem::PumpEvents();
			PanelManager::TickAll(0.016f);
			UISystem::Update();
			PanelManager::SubmitViews(0.016f);
			RenderSystem::RenderAll();
		}

		Guid StageGraph(const char* name, u32 templateIndex = 0) { return StageGraphText(name, NodeShaderImporter::TemplateDocument(templateIndex)); }

		Guid StageGraphText(const char* name, const str& text) {
			std::filesystem::create_directories(_root / "Assets");
			std::ofstream out(_root / "Assets" / name, std::ios::binary);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.close();
			AssetDatabase::ScanProjectDirectory();
			return AssetDatabase::LookupGuid(Path(str("project://Assets/") + name));
		}

		str ReadGraph(const char* name) const {
			std::ifstream in(_root / "Assets" / name, std::ios::binary);
			return str((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		static NodeShaderWindow* Open(const Guid& graph) {
			NodeShaderWindow* window = NodeShaderWindowRegistry::Open(graph);
			Frame();
			Frame();
			return window;
		}

		static u32 WindowCount() {
			Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
			PanelManager::GetOpenPanels(panels);
			u32 count = 0;
			for (const Handle<EditorPanel>& panel : panels) {
				count += panel.Get() && panel.Get()->GetType() == NodeShaderWindow::GetReflectType() ? 1u : 0u;
			}
			return count;
		}

		static u32 Pick(NodeShaderWindow& window, std::string_view name, Vec2 at, const GraphPortRef* from = nullptr) {
			Vector<GraphSearchEntry> entries{GetMemoryResource(MemTag::Editor)};
			window.GetModel().GetSearchEntries(from, entries);
			const GraphDocument* before = window.GetDocument();
			const u32 next = before ? before->nextId : 0;
			for (const GraphSearchEntry& entry : entries) {
				if (entry.name == name) {
					window.GetModel().OnSearchPick(entry, at, from);
					return next;
				}
			}
			return 0;
		}

		static u32 NodeCount(const NodeShaderWindow& window) {
			const GraphDocument* document = window.GetDocument();
			return document ? static_cast<u32>(document->nodes.size()) : 0u;
		}

		static bool SettleCompiles(NodeShaderWindow& window) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			Frame();
			while (window.GetPreviews().IsCompiling()) {
				if (std::chrono::steady_clock::now() > deadline) {
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				Frame();
			}
			Frame();
			return true;
		}
	};

} // namespace

TEST_F(NodeShaderWindowFixture, DoubleClickOpensOwnWindow) {
	const Guid dissolve = StageGraph("Dissolve.slshader");
	const Guid glow = StageGraph("Glow.slshader", 1);
	ASSERT_TRUE(dissolve.IsValid());
	ASSERT_TRUE(glow.IsValid());

	ASSERT_TRUE(AssetOpeners::Has(".slshader"));
	ASSERT_TRUE(AssetOpeners::Open(Path("project://Assets/Dissolve.slshader")));
	Frame();
	ASSERT_EQ(WindowCount(), 1u);
	NodeShaderWindow* first = NodeShaderWindowRegistry::Open(dissolve);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first->GetGraph(), dissolve);
	EXPECT_EQ(first->GetTitleBase(), "Dissolve");
	ASSERT_NE(first->GetDocument(), nullptr);
	EXPECT_EQ(first->GetDocument()->contexts.size(), 2u);

	AssetOpeners::Open(Path("project://Assets/Dissolve.slshader"));
	Frame();
	EXPECT_EQ(WindowCount(), 1u);
	NodeShaderWindow* second = Open(glow);
	ASSERT_NE(second, nullptr);
	EXPECT_NE(second, first);
	EXPECT_EQ(WindowCount(), 2u);
}

TEST_F(NodeShaderWindowFixture, OldGraphGainsVertexStack) {
	GraphDocument old;
	str error;
	ASSERT_TRUE(GraphDocumentOps::Read(NodeShaderImporter::TemplateDocument(0), old, error)) << error;
	std::erase_if(old.contexts, [](const GraphContext& context) { return context.node.def == "Context/Vertex"; });
	ASSERT_EQ(old.contexts.size(), 1u);
	const Guid graph = StageGraphText("Old.slshader", GraphDocumentOps::Write(old));
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_NE(window->GetDocument(), nullptr);
	EXPECT_NE(Stack::VertexContext(*window->GetDocument()), 0u);
	EXPECT_FALSE(window->HasUnsavedChanges());

	window->Edit("Move", [](GraphDocument& document) {
		document.contexts.back().node.x += 10;
		return true;
	});
	ASSERT_TRUE(window->HasUnsavedChanges());
	ASSERT_TRUE(window->Save());
	EXPECT_NE(ReadGraph("Old.slshader").find("Context/Vertex"), str::npos);
}

TEST_F(NodeShaderWindowFixture, DocumentStoreRunsOnlyOpenersUpgrade) {
	GraphDocument old;
	str error;
	ASSERT_TRUE(GraphDocumentOps::Read(NodeShaderImporter::TemplateDocument(0), old, error)) << error;
	std::erase_if(old.contexts, [](const GraphContext& context) { return context.node.def == "Context/Vertex"; });
	const Guid graph = StageGraphText("Plain.slshader", GraphDocumentOps::Write(old));

	ASSERT_TRUE(NodeShaderDocuments::Open(graph, nullptr, error)) << error;
	ASSERT_NE(NodeShaderDocuments::Get(graph), nullptr);
	EXPECT_EQ(Stack::VertexContext(*NodeShaderDocuments::Get(graph)), 0u);
	NodeShaderDocuments::Close(graph);
	ASSERT_FALSE(NodeShaderDocuments::IsOpen(graph));

	ASSERT_TRUE(NodeShaderDocuments::Open(graph, nullptr, error, &Stack::UpgradeOnOpen)) << error;
	EXPECT_NE(Stack::VertexContext(*NodeShaderDocuments::Get(graph)), 0u);
	EXPECT_FALSE(NodeShaderDocuments::HasUnsavedChanges(graph));
	NodeShaderDocuments::Get(graph)->contexts.back().node.x += 10;
	ASSERT_TRUE(NodeShaderDocuments::Discard(graph));
	EXPECT_NE(Stack::VertexContext(*NodeShaderDocuments::Get(graph)), 0u);
	EXPECT_FALSE(NodeShaderDocuments::HasUnsavedChanges(graph));
	NodeShaderDocuments::Close(graph);
}

TEST_F(NodeShaderWindowFixture, AFileSpelledAnotherWayOpensClean) {
	auto respell = [](const str& text) {
		str out;
		for (char c : text) {
			out += c == '\t' ? str("  ") : str(1, c);
		}
		return out;
	};
	const str graphText = respell(NodeShaderImporter::TemplateDocument(0));
	const str subgraphText = respell(ShaderSubgraphImporter::TemplateDocument());
	const Guid graph = StageGraphText("Spelled.slshader", graphText);
	const Guid subgraph = StageGraphText("Spelled.slsubgraph", subgraphText);
	for (const Guid& asset : {graph, subgraph}) {
		NodeShaderWindow* window = Open(asset);
		ASSERT_NE(window, nullptr);
		ASSERT_NE(window->GetDocument(), nullptr);
		const str written = GraphDocumentOps::Write(*window->GetDocument());
		ASSERT_NE(written, asset == graph ? graphText : subgraphText);
		EXPECT_FALSE(window->HasUnsavedChanges());

		ASSERT_TRUE(window->Edit("Move", [](GraphDocument& document) {
			document.contexts.back().node.x += 10;
			return true;
		}));
		EXPECT_TRUE(window->HasUnsavedChanges());
		ASSERT_TRUE(UndoSystem::Undo());
		Frame();
		EXPECT_FALSE(window->HasUnsavedChanges());
	}
}

TEST_F(NodeShaderWindowFixture, SearcherListsInterpolators) {
	const Guid graph = StageGraph("Wind.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	u32 block = 0;
	window->Edit("Add Block", [&](GraphDocument& document) {
		block = Stack::AddBlock(document, Stack::VertexContext(document), "Block/CustomInterpolator");
		return block != 0 && Stack::RenameInterpolator(document, block, "Wind");
	});
	ASSERT_NE(block, 0u);
	Frame();
	const u32 reader = Pick(*window, "Wind", {-300.0f, 0.0f});
	ASSERT_NE(reader, 0u);
	Frame();
	EXPECT_EQ(Stack::InterpolatorOf(*window->GetDocument(), reader), block);
	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetNodes(drawn);
	bool labelled = false;
	for (const GraphNodeDesc& node : drawn) {
		labelled = labelled || (node.id == NodeShaderWindow::CanvasId(reader) && node.label == "Wind (Custom Interpolator)");
	}
	EXPECT_TRUE(labelled);
}

TEST_F(NodeShaderWindowFixture, NonGraphOpensNothing) {
	std::filesystem::create_directories(_root / "Assets");
	WriteProjectFile("Assets/Plain.hlsl", "float4 PSMain() : SV_Target { return 1; }\n");
	AssetDatabase::ScanProjectDirectory();
	const Guid plain = AssetDatabase::LookupGuid(Path("project://Assets/Plain.hlsl"));
	ASSERT_TRUE(plain.IsValid());
	EXPECT_EQ(NodeShaderWindowRegistry::Open(plain), nullptr);
	Frame();
	EXPECT_EQ(WindowCount(), 0u);
}

TEST_F(NodeShaderWindowFixture, EditIsOneUndoMarksUnsaved) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	EXPECT_FALSE(window->HasUnsavedChanges());
	const u32 depth = UndoSystem::GetUndoDepth();

	const u32 add = Pick(*window, "Add", {-200.0f, 0.0f});
	ASSERT_NE(add, 0u);
	ASSERT_EQ(NodeCount(*window), 1u);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), depth + 1);
	EXPECT_EQ(UndoSystem::GetUndoLabel(), "Create Node");
	EXPECT_TRUE(window->HasUnsavedChanges());
	Frame();
	EXPECT_EQ(window->GetCanvas().GetSelection().size(), 1u);

	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(NodeCount(*window), 0u);
	EXPECT_FALSE(window->HasUnsavedChanges());
	ASSERT_TRUE(UndoSystem::Redo());
	Frame();
	EXPECT_EQ(NodeCount(*window), 1u);
}

TEST_F(NodeShaderWindowFixture, WiresFollowCompiler) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	const u32 noise = Pick(*window, "Simple Noise", {-400.0f, 0.0f});
	const u32 sample = Pick(*window, "Sample Texture 2D", {-400.0f, 200.0f});
	ASSERT_NE(noise, 0u);
	ASSERT_NE(sample, 0u);
	const u32 fragment = Stack::FragmentContext(*window->GetDocument());
	ASSERT_NE(fragment, 0u);

	GraphViewModel& model = window->GetModel();
	const GraphPortRef noiseOut{NodeShaderWindow::CanvasId(noise), GraphPortSide::Output, 0};
	const GraphPortRef textureIn{NodeShaderWindow::CanvasId(sample), GraphPortSide::Input, 0};
	const GraphPortRef baseColor{NodeShaderWindow::CanvasId(fragment), GraphPortSide::Input, 0};
	EXPECT_FALSE(model.CanConnect(noiseOut, textureIn));
	EXPECT_TRUE(model.CanConnect(noiseOut, baseColor));

	model.Connect(noiseOut, baseColor);
	const GraphContext* stack = nullptr;
	for (const GraphContext& context : window->GetDocument()->contexts) {
		stack = context.node.id == fragment ? &context : stack;
	}
	ASSERT_NE(stack, nullptr);
	const GraphNode* block = GraphDocumentOps::FindNode(*window->GetDocument(), stack->blocks[0].id);
	ASSERT_NE(block, nullptr);
	ASSERT_EQ(block->inputs.size(), 1u);
	EXPECT_EQ(block->inputs[0].fromNode, noise);

	Vector<GraphWireDesc> wires{GetMemoryResource(MemTag::Editor)};
	model.GetWires(wires);
	ASSERT_EQ(wires.size(), 1u);
	EXPECT_EQ(wires[0].to, baseColor);
}

TEST_F(NodeShaderWindowFixture, ASettingAddsAndRemovesItsPorts) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	const u32 sample = Pick(*window, "Sample Texture 2D", {-400.0f, 0.0f});
	ASSERT_NE(sample, 0u);
	auto inputs = [&]() {
		Vector<GraphNodeDesc> nodes{GetMemoryResource(MemTag::Editor)};
		window->GetModel().GetNodes(nodes);
		Vector<str> labels{GetMemoryResource(MemTag::Editor)};
		for (const GraphNodeDesc& node : nodes) {
			if (node.id == NodeShaderWindow::CanvasId(sample)) {
				for (const GraphPortDesc& port : node.inputs) {
					labels.push_back(port.label);
				}
			}
		}
		return labels;
	};
	ASSERT_EQ(inputs().size(), 2u);
	window->Edit("Node Setting", [&](GraphDocument& document) { return Ops::SetNodeSetting(document, sample, "Mip Sampling Mode", "\"LOD\""); });
	const Vector<str> lod = inputs();
	ASSERT_EQ(lod.size(), 3u);
	EXPECT_EQ(lod[2], "LOD");

	const u32 one = Pick(*window, "One Minus", {-700.0f, 0.0f});
	window->GetModel().Connect({NodeShaderWindow::CanvasId(one), GraphPortSide::Output, 0}, {NodeShaderWindow::CanvasId(sample), GraphPortSide::Input, 2});
	const GraphNode* node = GraphDocumentOps::FindNode(*window->GetDocument(), sample);
	ASSERT_NE(node, nullptr);
	bool fed = false;
	for (const GraphInput& input : node->inputs) {
		fed = fed || (input.port == "LOD" && input.fromNode == one);
	}
	EXPECT_TRUE(fed);
	Vector<GraphWireDesc> wires{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetWires(wires);
	ASSERT_EQ(wires.size(), 1u);
	EXPECT_EQ(wires[0].to.port, 2u);

	window->Edit("Node Setting", [&](GraphDocument& document) { return Ops::SetNodeSetting(document, sample, "Mip Sampling Mode", "\"Bias\""); });
	const Vector<str> bias = inputs();
	ASSERT_EQ(bias.size(), 3u);
	EXPECT_EQ(bias[2], "Bias");
	window->GetModel().GetWires(wires);
	EXPECT_TRUE(wires.empty());

	window->GetModel().Connect({NodeShaderWindow::CanvasId(one), GraphPortSide::Output, 0}, {NodeShaderWindow::CanvasId(sample), GraphPortSide::Input, 2});
	node = GraphDocumentOps::FindNode(*window->GetDocument(), sample);
	ASSERT_NE(node, nullptr);
	bool biased = false;
	for (const GraphInput& input : node->inputs) {
		biased = biased || (input.port == "Bias" && input.fromNode == one);
	}
	EXPECT_TRUE(biased);
	window->GetModel().GetWires(wires);
	ASSERT_EQ(wires.size(), 1u);
	EXPECT_EQ(wires[0].to.port, 2u);
}

TEST_F(NodeShaderWindowFixture, WindowsShareOneDocument) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* first = Open(graph);
	ASSERT_NE(first, nullptr);
	Handle<EditorPanel> panel = PanelManager::OpenPanel(NodeShaderWindow::GetReflectType(), _main);
	auto* second = static_cast<NodeShaderWindow*>(panel.Get());
	ASSERT_NE(second, nullptr);
	ASSERT_TRUE(second->OpenGraph(graph));
	Frame();
	EXPECT_EQ(first->GetDocument(), second->GetDocument());

	ASSERT_NE(Pick(*first, "Multiply", {-300.0f, 0.0f}), 0u);
	Frame();
	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	second->GetModel().GetNodes(drawn);
	EXPECT_EQ(drawn.size(), 3u);
	EXPECT_TRUE(second->HasUnsavedChanges());

	PanelManager::ClosePanel(PanelManager::FindPanelByDockId(first->GetDockId()));
	Frame();
	Frame();
	ASSERT_NE(second->GetDocument(), nullptr);
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(NodeCount(*second), 0u);
}

TEST_F(NodeShaderWindowFixture, SaveWritesDiscardReads) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	const str original = ReadGraph("Dissolve.slshader");

	ASSERT_NE(Pick(*window, "One Minus", {-250.0f, 40.0f}), 0u);
	EXPECT_EQ(ReadGraph("Dissolve.slshader"), original);
	EXPECT_NE(window->GetSaveChangesMessage().find("Dissolve.slshader"), str::npos);
	ASSERT_TRUE(window->Save());
	EXPECT_FALSE(window->HasUnsavedChanges());
	const str saved = ReadGraph("Dissolve.slshader");
	EXPECT_EQ(saved, GraphDocumentOps::Write(*window->GetDocument()));
	EXPECT_NE(saved.find("Math/OneMinus"), str::npos);

	ASSERT_NE(Pick(*window, "Saturate", {-250.0f, 140.0f}), 0u);
	EXPECT_TRUE(window->HasUnsavedChanges());
	window->DiscardChanges();
	Frame();
	EXPECT_FALSE(window->HasUnsavedChanges());
	EXPECT_EQ(NodeCount(*window), 1u);
}

TEST_F(NodeShaderWindowFixture, ThePreviewMeshIsAGraphSetting) {
	const Guid graph = StageGraph("Dissolve.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	EXPECT_EQ(window->GetPreviews().GetMesh(), NodeShaderPreviewMesh::Sphere);
	window->Edit("Change Preview Mesh", [](GraphDocument& document) { return Ops::SetGraphSetting(document, NodeShaderPreviews::MeshSetting, "\"cube\""); });
	EXPECT_EQ(window->GetPreviews().GetMesh(), NodeShaderPreviewMesh::Cube);
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	EXPECT_EQ(window->GetPreviews().GetMesh(), NodeShaderPreviewMesh::Sphere);
}

TEST_F(NodeShaderWindowFixture, PasteCarriesProperty) {
	const Guid from = StageGraph("Dissolve.slshader");
	const Guid to = StageGraph("Glow.slshader");
	NodeShaderWindow* source = Open(from);
	NodeShaderWindow* target = Open(to);
	ASSERT_NE(source, nullptr);
	ASSERT_NE(target, nullptr);

	u32 property = 0;
	source->Edit("Add Property", [&](GraphDocument& document) {
		property = Ops::AddProperty(document, "Float");
		return property != 0;
	});
	u32 reader = 0;
	source->Edit("Create Property Node", [&](GraphDocument& document) {
		reader = Ops::AddPropertyNode(document, property, {-300.0f, 0.0f});
		return reader != 0;
	});
	Frame();
	const GraphItem selected{GraphItemKind::Node, NodeShaderWindow::CanvasId(reader)};
	source->GetCanvas().SetSelection({&selected, 1});
	ASSERT_TRUE(source->GetModel().OnGraphCommand(GraphCommand::Copy));
	ASSERT_TRUE(target->GetModel().OnGraphCommand(GraphCommand::Paste));
	Frame();

	const GraphDocument* pasted = target->GetDocument();
	ASSERT_EQ(pasted->properties.size(), 1u);
	ASSERT_EQ(pasted->nodes.size(), 1u);
	EXPECT_EQ(Ops::PropertyOf(*pasted, pasted->nodes[0].id), pasted->properties[0].id);
	EXPECT_TRUE(source->GetDocument()->contexts.size() == 2u && pasted->contexts.size() == 2u);
}

TEST_F(NodeShaderWindowFixture, AnUndoneDuplicateLeavesNothingInspected) {
	const Guid graph = StageGraph("Copied.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	const u32 noise = Pick(*window, "Simple Noise", {-300.0f, 0.0f});
	ASSERT_NE(noise, 0u);
	Frame();
	ASSERT_EQ(window->GetInspected(), noise);
	ASSERT_TRUE(window->OnCommand(PanelCommand::Duplicate));
	Frame();
	const std::span<const GraphItem> selected = window->GetCanvas().GetSelection();
	ASSERT_EQ(selected.size(), 1u);
	ASSERT_NE(static_cast<u32>(selected.back().id), noise);
	EXPECT_EQ(window->GetInspected(), static_cast<u32>(selected.back().id));
	ASSERT_TRUE(UndoSystem::Undo());
	Frame();
	ASSERT_TRUE(window->GetCanvas().GetSelection().empty());
	EXPECT_EQ(window->GetInspected(), 0u);
}

TEST_F(NodeShaderWindowFixture, DroppedPropertyInspectsNode) {
	const Guid graph = StageGraph("Dropped.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	window->Edit("Add Property", [](GraphDocument& document) {
		const u32 property = Ops::AddProperty(document, "Float");
		return property != 0 && Ops::RenameProperty(document, property, "Glint");
	});
	Frame();
	Frame();
	UI::Context* context = nullptr;
	UI::ElementHandle row;
	Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Debug)};
	UISystem::GetDockWindows(windows);
	for (Handle<Window> held : windows) {
		UI::Context* in = UISystem::GetContext(held);
		if (in && in->IsAlive(FindByText(*in, in->GetRoot(), "Glint"))) {
			context = in;
			row = FindByText(*in, in->GetRoot(), "Glint");
		}
	}
	ASSERT_NE(context, nullptr);
	ClickElement(*context, row);
	Frame();
	ASSERT_EQ(window->GetInspected(), 0u);
	const u32 made = window->GetDocument()->nextId;
	const u32 before = NodeCount(*window);
	const UI::Rect from = context->GetLayout(row).borderBox;
	InputEvent drag{};
	drag.button = MouseButton::Left;
	for (i32 step = 0; step <= 4; ++step) {
		drag.type = step == 0 ? InputEventType::MouseDown : InputEventType::MouseMove;
		drag.mousePosPx = Vec2I(static_cast<i32>(from.x + from.w * 0.5f), static_cast<i32>(from.y + from.h * 0.5f) + 6 * step);
		context->InjectInput(drag);
		context->Update();
	}
	const UI::DragPayload* began = UISystem::GetDragPayload();
	const UI::DragPayload payload = began ? *began : UI::DragPayload{};
	UISystem::CancelDragPayload();
	drag.type = InputEventType::MouseUp;
	context->InjectInput(drag);
	context->Update();
	ASSERT_NE(payload.type.GetValue(), 0u);
	const UI::Rect onto = context->GetLayout(window->GetCanvas().GetElement()).content;
	EXPECT_TRUE(context->DeliverDrop(payload, Vec2(onto.x + onto.w * 0.3f, onto.y + onto.h * 0.5f)));
	Frame();
	ASSERT_EQ(NodeCount(*window), before + 1);
	EXPECT_EQ(window->GetInspected(), made);
}

TEST_F(NodeShaderWindowFixture, NoGraphCompileRunsOnMainThread) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "no compiler";
	}
	const Guid graph = StageGraph("Dissolve.slshader");
	Frame();
	Frame();
	const ShaderCompileStats before = ShaderCompileQueue::GetStats();

	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_TRUE(SettleCompiles(*window));

	const u32 noise = Pick(*window, "Simple Noise", {-500.0f, 0.0f});
	const u32 step = Pick(*window, "Step", {-250.0f, 0.0f});
	ASSERT_NE(noise, 0u);
	ASSERT_NE(step, 0u);
	GraphViewModel& model = window->GetModel();
	model.Connect({NodeShaderWindow::CanvasId(noise), GraphPortSide::Output, 0}, {NodeShaderWindow::CanvasId(step), GraphPortSide::Input, 1});
	ASSERT_TRUE(SettleCompiles(*window));
	const f32 edge[] = {0.4f};
	model.SetInlineValue({NodeShaderWindow::CanvasId(step), GraphPortSide::Input, 0}, edge);
	ASSERT_TRUE(SettleCompiles(*window));

	const ShaderCompileStats after = ShaderCompileQueue::GetStats();
	EXPECT_EQ(after.compilesElsewhere, before.compilesElsewhere);
	EXPECT_GT(after.submitted, before.submitted);
	EXPECT_TRUE(window->GetPreviews().GetDiagnostics().empty())
		<< window->GetPreviews().GetDiagnostics()[0].message;
	EXPECT_FALSE(window->GetPreviews().SurfaceFailed());
	EXPECT_GE(window->GetPreviews().TileOf(step), 0);

	const u64 submitted = ShaderCompileQueue::GetStats().submitted;
	model.OnMoveBegin();
	model.MoveNode(NodeShaderWindow::CanvasId(noise), {-600.0f, 20.0f});
	model.OnMoveEnd();
	ASSERT_TRUE(SettleCompiles(*window));
	EXPECT_EQ(ShaderCompileQueue::GetStats().submitted, submitted);
}

namespace {

	const GraphNodeDesc* Drawn(const Vector<GraphNodeDesc>& drawn, u32 id) {
		for (const GraphNodeDesc& node : drawn) {
			if (node.id == NodeShaderWindow::CanvasId(id)) {
				return &node;
			}
		}
		return nullptr;
	}

} // namespace

TEST_F(NodeShaderWindowFixture, SubgraphOpensInWindowWithOutputNode) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	ASSERT_TRUE(ripple.IsValid());
	ASSERT_TRUE(AssetOpeners::Has(".slsubgraph"));
	ASSERT_TRUE(AssetOpeners::Open(Path("project://Assets/Ripple.slsubgraph")));
	Frame();
	NodeShaderWindow* window = NodeShaderWindowRegistry::Open(ripple);
	ASSERT_NE(window, nullptr);
	ASSERT_EQ(WindowCount(), 1u);
	ASSERT_NE(window->GetDocument(), nullptr);
	const u32 output = GraphSubgraphs::OutputContext(*window->GetDocument());
	ASSERT_NE(output, 0u);

	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetNodes(drawn);
	const GraphNodeDesc* stack = Drawn(drawn, output);
	ASSERT_NE(stack, nullptr);
	EXPECT_EQ(stack->label, "Output");
	ASSERT_EQ(stack->inputs.size(), 1u);
	EXPECT_EQ(stack->inputs[0].label, "Out_Vector4");

	ASSERT_TRUE(window->Edit("Add Output", [&](GraphDocument& document) { return Ops::AddSubgraphOutput(document, output, "Float") != 0; }));
	Frame();
	window->GetModel().GetNodes(drawn);
	stack = Drawn(drawn, output);
	ASSERT_NE(stack, nullptr);
	ASSERT_EQ(stack->inputs.size(), 2u);
	EXPECT_EQ(stack->inputs[1].label, "Out_Float");

	ASSERT_TRUE(SettleCompiles(*window));
	EXPECT_FALSE(window->GetPreviews().SurfaceFailed());
	EXPECT_NE(window->GetPreviews().GetSurfaceText().find("main preview"), str::npos);
}

TEST_F(NodeShaderWindowFixture, SearcherListsProjectSubgraphs) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	const Guid pond = StageGraph("Pond.slshader");
	ASSERT_TRUE(ripple.IsValid());
	ASSERT_TRUE(pond.IsValid());
	NodeShaderWindow* window = Open(pond);
	ASSERT_NE(window, nullptr);
	Vector<GraphSearchEntry> entries{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetSearchEntries(nullptr, entries);
	const auto listed = std::find_if(entries.begin(), entries.end(), [](const GraphSearchEntry& entry) { return entry.name == "Ripple"; });
	ASSERT_NE(listed, entries.end());
	EXPECT_EQ(listed->category, GraphSubgraphDefaultCategory);

	const u32 node = Pick(*window, "Ripple", {-300.0f, 0.0f});
	ASSERT_NE(node, 0u);
	Frame();
	const GraphNode* made = GraphDocumentOps::FindNode(*window->GetDocument(), node);
	ASSERT_NE(made, nullptr);
	EXPECT_EQ(made->def, GraphSubgraphNode);
	EXPECT_EQ(GraphSubgraphs::AssetOf(*made), ripple);
	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetNodes(drawn);
	const GraphNodeDesc* desc = Drawn(drawn, node);
	ASSERT_NE(desc, nullptr);
	EXPECT_EQ(desc->label, "Ripple");
	ASSERT_EQ(desc->outputs.size(), 1u);
	EXPECT_EQ(desc->outputs[0].label, "Out_Vector4");

	NodeShaderWindow* own = Open(ripple);
	ASSERT_NE(own, nullptr);
	own->GetModel().GetSearchEntries(nullptr, entries);
	EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [](const GraphSearchEntry& entry) { return entry.name == "Ripple"; }));
}

TEST_F(NodeShaderWindowFixture, SavingSubgraphReachesOpenGraphUsingIt) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	const Guid pond = StageGraph("Pond.slshader");
	NodeShaderWindow* user = Open(pond);
	ASSERT_NE(user, nullptr);
	const u32 node = Pick(*user, "Ripple", {-300.0f, 0.0f});
	ASSERT_NE(node, 0u);
	NodeShaderWindow* editor = Open(ripple);
	ASSERT_NE(editor, nullptr);
	ASSERT_TRUE(editor->Edit("Add Output", [](GraphDocument& document) {
		return Ops::AddSubgraphOutput(document, GraphSubgraphs::OutputContext(document), "Float") != 0;
	}));
	Frame();
	auto outputs = [&]() {
		Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
		user->GetModel().GetNodes(drawn);
		const GraphNodeDesc* desc = Drawn(drawn, node);
		return desc ? desc->outputs.size() : size_t{0};
	};
	EXPECT_EQ(outputs(), 1u);
	ASSERT_TRUE(editor->Save());
	Frame();
	EXPECT_EQ(outputs(), 2u);
}

TEST_F(NodeShaderWindowFixture, DoubleClickingSubgraphNodeOpensSubgraph) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	const Guid pond = StageGraph("Pond.slshader");
	NodeShaderWindow* window = Open(pond);
	ASSERT_NE(window, nullptr);
	const u32 node = Pick(*window, "Ripple", {-300.0f, 0.0f});
	ASSERT_NE(node, 0u);
	window->GetModel().OnNodeDoubleClick(NodeShaderWindow::CanvasId(node));
	Frame();
	EXPECT_EQ(WindowCount(), 2u);
	NodeShaderWindow* opened = NodeShaderWindowRegistry::Open(ripple);
	ASSERT_NE(opened, nullptr);
	EXPECT_NE(opened, window);
	EXPECT_EQ(WindowCount(), 2u);
}

TEST_F(NodeShaderWindowFixture, SubgraphsPropertyShowsNoRowMaterialReads) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	const Guid pond = StageGraph("Pond.slshader");
	ASSERT_TRUE(ripple.IsValid());
	ASSERT_TRUE(pond.IsValid());
	auto add = [](NodeShaderWindow& window, std::string_view type, std::string_view name) {
		return window.Edit("Add Property", [&](GraphDocument& document) {
			const u32 property = Ops::AddProperty(document, type);
			return property != 0 && Ops::RenameProperty(document, property, name);
		});
	};
	auto select = [](std::string_view name) -> UI::Context* {
		Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Editor)};
		UISystem::GetDockWindows(windows);
		for (Handle<Window> window : windows) {
			UI::Context* context = UISystem::GetContext(window);
			const UI::ElementHandle row = context ? FindByText(*context, context->GetRoot(), name) : UI::ElementHandle{};
			if (context && context->IsAlive(row)) {
				ClickElement(*context, row);
				Frame();
				return context;
			}
		}
		return nullptr;
	};
	auto shows = [](UI::Context& context, std::string_view text) { return context.IsAlive(FindByText(context, context.GetRoot(), text)); };

	NodeShaderWindow* graph = Open(pond);
	NodeShaderWindow* subgraph = Open(ripple);
	ASSERT_NE(graph, nullptr);
	ASSERT_NE(subgraph, nullptr);
	ASSERT_TRUE(add(*graph, "Color", "Glow"));
	ASSERT_TRUE(add(*subgraph, "Color", "Tint"));
	ASSERT_TRUE(add(*subgraph, "Texture2D", "Ripples"));
	ASSERT_TRUE(add(*subgraph, "BooleanKeyword", "Wet"));
	Frame();

	UI::Context* context = select("Glow");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Color Property"));
	EXPECT_TRUE(shows(*context, "Reference"));
	EXPECT_TRUE(shows(*context, "Mode"));
	EXPECT_TRUE(shows(*context, "Show In Inspector"));

	context = select("Tint");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Color Property"));
	EXPECT_TRUE(shows(*context, "Default"));
	EXPECT_FALSE(shows(*context, "Reference"));
	EXPECT_FALSE(shows(*context, "Mode"));
	EXPECT_FALSE(shows(*context, "Show In Inspector"));
	EXPECT_FALSE(shows(*context, "Group"));

	context = select("Ripples");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Texture2D Property"));
	EXPECT_FALSE(shows(*context, "Mode"));

	context = select("Wet");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Boolean Keyword Property"));
	EXPECT_TRUE(shows(*context, "Reference"));
}

namespace {

	UI::Context* ContextShowing(std::string_view text) {
		Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Editor)};
		UISystem::GetDockWindows(windows);
		for (Handle<Window> window : windows) {
			UI::Context* context = UISystem::GetContext(window);
			if (context && context->IsAlive(FindByText(*context, context->GetRoot(), text))) {
				return context;
			}
		}
		return nullptr;
	}

	UI::ElementHandle FieldAfter(UI::Context& context, std::string_view label) {
		const UI::Element* held = context.GetElement(FindByText(context, context.GetRoot(), label));
		return held && held->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(held->nextSibling) : UI::ElementHandle{};
	}

	void TypeInto(UI::Context& context, std::string_view text) {
		for (const char c : text) {
			InputEvent event{};
			event.type = InputEventType::Text;
			event.codepoint = static_cast<char32_t>(c);
			context.InjectInput(event);
			context.Update();
		}
	}

	void PressIn(UI::Context& context, Key key) {
		InputEvent down{};
		down.type = InputEventType::KeyDown;
		down.key = key;
		context.InjectInput(down);
		context.Update();
		InputEvent up = down;
		up.type = InputEventType::KeyUp;
		context.InjectInput(up);
		context.Update();
	}

} // namespace

TEST_F(NodeShaderWindowFixture, CustomFunctionIsEditedInInspector) {
	const Guid graph = StageGraph("Function.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	const u32 node = Pick(*window, "Custom Function", {-300.0f, 0.0f});
	ASSERT_NE(node, 0u);
	Frame();
	Frame();
	ASSERT_EQ(GraphDocumentOps::FindNode(*window->GetDocument(), node)->def, GraphCustomFunctionNode);
	UI::Context* context = ContextShowing("Inputs");
	ASSERT_NE(context, nullptr);

	const UI::ElementHandle plus = FieldAfter(*context, "Inputs");
	ASSERT_TRUE(context->IsAlive(plus));
	ClickElement(*context, plus);
	const UI::ElementHandle kind = FindByText(*context, context->GetRoot(), "Vector3");
	ASSERT_TRUE(context->IsAlive(kind));
	ClickElement(*context, kind);
	Frame();
	Frame();
	auto inputs = [&]() {
		Vector<GraphCustomFunctionPort> out{GetMemoryResource(MemTag::Editor)};
		str error;
		GraphCustomFunctions::ReadPorts(*GraphDocumentOps::FindNode(*window->GetDocument(), node), GraphCustomFunctionInputs, out, error);
		return out;
	};
	ASSERT_EQ(inputs().size(), 1u);
	EXPECT_EQ(inputs()[0].name, "New");
	EXPECT_EQ(GraphCustomFunctionPortTypes[inputs()[0].type], "Vector3");

	TypeInto(*context, "Tint");
	PressIn(*context, Key::Enter);
	Frame();
	Frame();
	ASSERT_EQ(inputs().size(), 1u);
	EXPECT_EQ(inputs()[0].name, "Tint");
	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetNodes(drawn);
	const GraphNodeDesc* desc = Drawn(drawn, node);
	ASSERT_NE(desc, nullptr);
	ASSERT_EQ(desc->inputs.size(), 1u);
	EXPECT_EQ(desc->inputs[0].label, "Tint");

	context = ContextShowing("Source");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FieldAfter(*context, "Type"));
	const UI::ElementHandle string = FindByText(*context, context->GetRoot(), "String");
	ASSERT_TRUE(context->IsAlive(string));
	ClickElement(*context, string);
	Frame();
	Frame();

	context = ContextShowing("Body");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FieldAfter(*context, "Body"));
	TypeInto(*context, "Out = 1.0;");
	PressIn(*context, Key::Enter);
	TypeInto(*context, "Out = 2.0;");
	ClickElement(*context, FieldAfter(*context, "Name"));
	Frame();
	const GraphNode* held = GraphDocumentOps::FindNode(*window->GetDocument(), node);
	str body;
	for (const GraphSetting& setting : held->settings) {
		if (setting.key == GraphCustomFunctionBody) {
			LinearAllocator arena(setting.value.size() + 64, MemTag::Editor);
			const JSONValue value = JSONParse(setting.value, arena);
			body = str(value.AsString());
		}
	}
	EXPECT_EQ(body, "Out = 1.0;\nOut = 2.0;");
}

TEST_F(NodeShaderWindowFixture, FailedFunctionShownOnNode) {
	std::filesystem::create_directories(_root / "Assets");
	WriteProjectFile("Assets/Broken.hlsl", "void Broken_float(out float4 Out)\n{\n\tOut = Missing;\n}\n");
	AssetDatabase::ScanProjectDirectory();
	const Guid file = AssetDatabase::LookupGuid(Path("project://Assets/Broken.hlsl"));
	ASSERT_TRUE(file.IsValid());
	const Guid graph = StageGraph("Faulty.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	u32 typo = 0;
	u32 broken = 0;
	ASSERT_TRUE(window->Edit("Functions", [&](GraphDocument& document) {
		typo = Ops::AddCustomFunctionNode(document, {-600.0f, 0.0f});
		const u32 typoOut = Ops::AddCustomFunctionPort(document, typo, true, "Vector4");
		Ops::SetNodeSetting(document, typo, GraphCustomFunctionType, Ops::Quote("String"));
		Ops::SetNodeSetting(document, typo, GraphCustomFunctionName, Ops::Quote("Typo"));
		Ops::SetNodeSetting(document, typo, GraphCustomFunctionBody, Ops::Quote("Out = Ou;"));
		broken = Ops::AddCustomFunctionNode(document, {-600.0f, 200.0f});
		const u32 brokenOut = Ops::AddCustomFunctionPort(document, broken, true, "Vector4");
		Ops::SetNodeSetting(document, broken, GraphCustomFunctionName, Ops::Quote("Broken"));
		Ops::SetNodeSetting(document, broken, GraphCustomFunctionSource, Ops::Quote(file.ToString()));
		const u32 add = Ops::AddNode(document, "Math/Add", {-300.0f, 0.0f});
		GraphDocumentOps::Connect(document, typo, std::to_string(typoOut), add, "A");
		GraphDocumentOps::Connect(document, broken, std::to_string(brokenOut), add, "B");
		for (const GraphContext& context : document.contexts) {
			for (const GraphNode& block : context.blocks) {
				if (block.def == "Block/BaseColor") {
					GraphDocumentOps::Connect(document, add, "Out", block.id, "Value");
				}
			}
		}
		return true;
	}));
	ASSERT_TRUE(SettleCompiles(*window));
	auto says = [&](u32 node, std::string_view word) {
		for (const GraphDiagnostic& diagnostic : window->GetPreviews().GetDiagnostics()) {
			if (diagnostic.node == node && diagnostic.message.starts_with("does not compile: ") && diagnostic.message.find(word) != str::npos) {
				return true;
			}
		}
		return false;
	};
	EXPECT_TRUE(says(typo, "Ou"));
	EXPECT_TRUE(says(broken, "Missing"));
}

TEST_F(NodeShaderWindowFixture, SubgraphBadgeFollowsPackage) {
	static const int token = 0;
	RegisterNodeShader(&token);
	EXPECT_EQ(EditorPreviews::BadgeForFileName("Ripple.slsubgraph").icon.glyph, EditorIcons::Glyph::AssetShaderSubgraph.glyph);
	EXPECT_EQ(EditorPreviews::BadgeForFileName("Pond.slshader").icon.glyph, EditorIcons::Glyph::AssetShader.glyph);

	EXPECT_GT(EditorExtensions::Unload(&token), 0u);
	EXPECT_EQ(EditorPreviews::BadgeForFileName("Ripple.slsubgraph").icon.glyph, EditorIcons::Glyph::AssetGeneric.glyph);
	EXPECT_EQ(EditorPreviews::BadgeForFileName("Pond.slshader").icon.glyph, EditorIcons::Glyph::AssetShader.glyph);

	TestHelpers::RestoreNodeShader();
}

TEST_F(NodeShaderWindowFixture, CustomBindingLabelsInput) {
	const Guid ripple = StageGraphText("Ripple.slsubgraph", ShaderSubgraphImporter::TemplateDocument());
	const Guid pond = StageGraph("Pond.slshader");
	ASSERT_TRUE(ripple.IsValid());
	ASSERT_TRUE(pond.IsValid());
	NodeShaderWindow* graph = Open(pond);
	NodeShaderWindow* subgraph = Open(ripple);
	ASSERT_NE(graph, nullptr);
	ASSERT_NE(subgraph, nullptr);
	auto offered = [](NodeShaderWindow& window, std::string_view name) {
		Vector<GraphSearchEntry> entries{GetMemoryResource(MemTag::Editor)};
		window.GetModel().GetSearchEntries(nullptr, entries);
		return std::any_of(entries.begin(), entries.end(), [&](const GraphSearchEntry& entry) { return entry.name == name; });
	};
	EXPECT_TRUE(offered(*subgraph, "Branch On Input Connection"));
	EXPECT_FALSE(offered(*graph, "Branch On Input Connection"));

	auto add = [](NodeShaderWindow& window, std::string_view name) {
		return window.Edit("Add Property", [&](GraphDocument& document) {
			const u32 property = Ops::AddProperty(document, "Float");
			return property != 0 && Ops::RenameProperty(document, property, name);
		});
	};
	ASSERT_TRUE(add(*graph, "Glow"));
	ASSERT_TRUE(add(*subgraph, "Offset"));
	Frame();
	auto select = [](std::string_view name) -> UI::Context* {
		UI::Context* context = ContextShowing(name);
		if (context) {
			ClickElement(*context, FindByText(*context, context->GetRoot(), name));
			Frame();
		}
		return context;
	};
	auto shows = [](UI::Context& context, std::string_view text) { return context.IsAlive(FindByText(context, context.GetRoot(), text)); };
	auto setting = [&](std::string_view key) {
		for (const GraphProperty& property : subgraph->GetDocument()->properties) {
			for (const GraphSetting& held : property.settings) {
				if (property.name == "Offset" && held.key == key) {
					return held.value;
				}
			}
		}
		return str();
	};

	UI::Context* context = select("Glow");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Float Property"));
	EXPECT_FALSE(shows(*context, "Use Custom Binding"));

	context = select("Offset");
	ASSERT_NE(context, nullptr);
	ASSERT_TRUE(shows(*context, "Use Custom Binding"));
	EXPECT_FALSE(shows(*context, "Label"));
	ClickElement(*context, FieldAfter(*context, "Use Custom Binding"));
	Frame();
	EXPECT_EQ(setting("customBinding"), "true");
	context = ContextShowing("Label");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FieldAfter(*context, "Label"));
	TypeInto(*context, "UV");
	PressIn(*context, Key::Enter);
	Frame();
	EXPECT_EQ(setting("bindingLabel"), "\"UV\"");
	ASSERT_TRUE(subgraph->Save());
	Frame();

	const u32 node = Pick(*graph, "Ripple", {-300.0f, 0.0f});
	ASSERT_NE(node, 0u);
	Frame();
	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	graph->GetModel().GetNodes(drawn);
	const GraphNodeDesc* desc = Drawn(drawn, node);
	ASSERT_NE(desc, nullptr);
	ASSERT_EQ(desc->inputs.size(), 1u);
	EXPECT_EQ(desc->inputs[0].inlineValue.kind, GraphInlineKind::Label);
	EXPECT_EQ(desc->inputs[0].inlineValue.text, "UV");
}

TEST_F(NodeShaderWindowFixture, EnumKeywordIsEditedInInspector) {
	const Guid graph = StageGraph("Modes.slshader");
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	u32 property = 0;
	ASSERT_TRUE(window->Edit("Add Property", [&](GraphDocument& document) {
		property = Ops::AddProperty(document, "EnumKeyword");
		return property != 0 && Ops::RenameProperty(document, property, "Quality");
	}));
	u32 node = 0;
	ASSERT_TRUE(window->Edit("Add Node", [&](GraphDocument& document) {
		node = Ops::AddPropertyNode(document, property, {-300.0f, 0.0f});
		return node != 0;
	}));
	Frame();
	UI::Context* context = ContextShowing("Quality");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Quality"));
	Frame();
	context = ContextShowing("Enum Keyword Property");
	ASSERT_NE(context, nullptr);
	auto keyword = [&]() -> const GraphProperty& {
		const GraphDocument* document = window->GetDocument();
		return *std::find_if(document->properties.begin(), document->properties.end(), [&](const GraphProperty& held) { return held.id == property; });
	};

	const UI::ElementHandle pick = FieldAfter(*context, "Default");
	ASSERT_TRUE(context->IsAlive(pick));
	ClickElement(*context, pick);
	const UI::ElementHandle popup = FindByClass(*context, context->GetRoot(), StringHash("slt-menu-popup"), 0);
	ASSERT_TRUE(context->IsAlive(popup));
	const UI::ElementHandle second = FindByText(*context, popup, "B");
	ASSERT_TRUE(context->IsAlive(second));
	ClickElement(*context, second);
	Frame();
	EXPECT_EQ(keyword().defaultValue, "1");

	context = ContextShowing("Entries");
	ASSERT_NE(context, nullptr);
	const UI::ElementHandle plus = FieldAfter(*context, "Entries");
	ASSERT_TRUE(context->IsAlive(plus));
	ClickElement(*context, plus);
	Frame();
	Frame();
	auto entries = [&]() {
		Vector<GraphKeywordEntry> out{GetMemoryResource(MemTag::Editor)};
		str error;
		GraphProperties::ReadEntries(keyword(), out, error);
		return out;
	};
	ASSERT_EQ(entries().size(), 4u);
	EXPECT_EQ(entries()[3].name, "New");
	TypeInto(*context, "Ultra");
	PressIn(*context, Key::Enter);
	Frame();
	Frame();
	ASSERT_EQ(entries().size(), 4u);
	EXPECT_EQ(entries()[3].name, "Ultra");
	EXPECT_EQ(entries()[3].reference, "ULTRA");

	Vector<GraphNodeDesc> drawn{GetMemoryResource(MemTag::Editor)};
	window->GetModel().GetNodes(drawn);
	const GraphNodeDesc* desc = Drawn(drawn, node);
	ASSERT_NE(desc, nullptr);
	ASSERT_EQ(desc->inputs.size(), 4u);
	EXPECT_EQ(desc->inputs[0].label, "A");
	EXPECT_EQ(desc->inputs[3].label, "Ultra");
	EXPECT_EQ(desc->label, "Quality");
}

namespace {

	struct ScaleGraph {
		str text;
		Vector<u32> noises{GetMemoryResource(MemTag::Editor)};
		u32 gain = 0;
	};

	ScaleGraph MakeScaleGraph() {
		ScaleGraph out;
		GraphDocument document;
		str error;
		EXPECT_TRUE(GraphDocumentOps::Read(NodeShaderImporter::TemplateDocument(1), document, error)) << error;
		u32 base = 0;
		for (const GraphContext& context : document.contexts) {
			for (const GraphNode& block : context.blocks) {
				base = block.def == "Block/BaseColor" ? block.id : base;
			}
		}
		auto add = [&](std::string_view def, f32 x, f32 y) { return Ops::AddNode(document, def, Vec2(x, y)); };
		auto constant = [&](u32 node, std::string_view port, std::initializer_list<f32> values) {
			EXPECT_TRUE(Ops::SetInputConstant(document, node, port, {values.begin(), values.size()}));
		};
		auto wire = [&](u32 from, u32 to, std::string_view port) { EXPECT_TRUE(GraphDocumentOps::Connect(document, from, "Out", to, port)); };
		Vector<u32> sums{GetMemoryResource(MemTag::Editor)};
		for (u32 k = 0; k < 20; ++k) {
			const f32 y = static_cast<f32>(k) * 220.0f;
			const u32 tiling = add("UV/TilingAndOffset", -2400.0f, y);
			constant(tiling, "Tiling", {1.0f + 0.5f * static_cast<f32>(k), 1.0f + 0.25f * static_cast<f32>(k)});
			const u32 noise = add("Procedural/SimpleNoise", -2150.0f, y);
			constant(noise, "Scale", {10.0f + static_cast<f32>(k)});
			wire(tiling, noise, "UV");
			out.noises.push_back(noise);
			const u32 multiply = add("Math/Multiply", -1900.0f, y);
			wire(noise, multiply, "A");
			const u32 sine = add("Math/Sine", -1650.0f, y);
			wire(multiply, sine, "In");
			const u32 offset = add("Math/Add", -1400.0f, y);
			wire(sine, offset, "A");
			constant(offset, "B", {0.5f});
			const u32 saturate = add("Math/Saturate", -1150.0f, y);
			wire(offset, saturate, "In");
			const u32 power = add("Math/Power", -900.0f, y);
			wire(saturate, power, "A");
			const u32 inverse = add("Math/OneMinus", -650.0f, y);
			wire(power, inverse, "In");
			const u32 lerp = add("Math/Lerp", -400.0f, y);
			wire(inverse, lerp, "A");
			wire(noise, lerp, "B");
			wire(saturate, lerp, "T");
			if (k < 19) {
				sums.push_back(lerp);
			}
		}
		for (f32 x = -150.0f; sums.size() > 1; x += 150.0f) {
			Vector<u32> next{GetMemoryResource(MemTag::Editor)};
			for (size_t i = 0; i + 1 < sums.size(); i += 2) {
				const u32 sum = add("Math/Add", x, static_cast<f32>(i) * 110.0f);
				wire(sums[i], sum, "A");
				wire(sums[i + 1], sum, "B");
				next.push_back(sum);
			}
			if (sums.size() % 2 == 1) {
				next.push_back(sums.back());
			}
			sums = std::move(next);
		}
		out.gain = Ops::AddProperty(document, "Float");
		EXPECT_TRUE(Ops::RenameProperty(document, out.gain, "Gain"));
		EXPECT_TRUE(Ops::SetPropertyDefault(document, out.gain, "0.05"));
		const u32 gain = Ops::AddPropertyNode(document, out.gain, Vec2(0.0f, 300.0f));
		const u32 scale = add("Math/Multiply", 150.0f, 0.0f);
		wire(sums[0], scale, "A");
		wire(gain, scale, "B");
		EXPECT_TRUE(GraphDocumentOps::Connect(document, scale, "Out", base, "Value"));
		EXPECT_EQ(document.nodes.size(), 200u);
		out.text = GraphDocumentOps::Write(document);
		return out;
	}

	f64 MillisecondsSince(std::chrono::steady_clock::time_point start) {
		return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

} // namespace

TEST_F(NodeShaderWindowFixture, BigGraphPreviewsInOneProgram) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const ScaleGraph scale = MakeScaleGraph();
	const Guid graph = StageGraphText("Scale.slshader", scale.text);
	ASSERT_TRUE(graph.IsValid());
	ASSERT_EQ(scale.noises.size(), 20u);
	auto opened = std::chrono::steady_clock::now();
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_TRUE(SettleCompiles(*window));
	const f64 open = MillisecondsSince(opened);
	auto tiled = [&]() {
		u32 tiles = 0;
		for (const GraphNode& node : window->GetDocument()->nodes) {
			tiles += window->GetPreviews().TileOf(node.id) >= 0 ? 1u : 0u;
		}
		return tiles;
	};
	EXPECT_EQ(tiled(), 199u);
	for (const GraphDiagnostic& diagnostic : window->GetPreviews().GetDiagnostics()) {
		ADD_FAILURE() << "node " << diagnostic.node << ": " << diagnostic.message;
	}

	struct Kind {
		const char* name;
		u64 compiles;
		std::function<bool(GraphDocument&, u32)> edit;
	};
	const Kind kinds[] = {
		{"both", 2, [&](GraphDocument& document, u32 i) {
			 const f32 value = 40.0f + static_cast<f32>(i);
			 return Ops::SetInputConstant(document, scale.noises[i * 7], "Scale", {&value, 1});
		 }},
		{"nodes", 1, [&](GraphDocument& document, u32 i) {
			 const f32 value = 60.0f + static_cast<f32>(i);
			 return Ops::SetInputConstant(document, scale.noises.back(), "Scale", {&value, 1});
		 }},
		{"main", 1, [&](GraphDocument& document, u32 i) {
			 return Ops::SetPropertyDefault(document, scale.gain, std::to_string(0.06 + 0.01 * static_cast<f64>(i)));
		 }},
	};
	str measured = std::format("open {:.0f} ms", open);
	for (const Kind& kind : kinds) {
		f64 frame[3] = {};
		f64 landed[3] = {};
		for (u32 i = 0; i < 3; ++i) {
			const u64 submitted = ShaderCompileQueue::GetStats().submitted;
			const auto edited = std::chrono::steady_clock::now();
			ASSERT_TRUE(window->Edit("Scale", [&](GraphDocument& document) { return kind.edit(document, i); }));
			Frame();
			frame[i] = MillisecondsSince(edited);
			ASSERT_TRUE(SettleCompiles(*window)) << kind.name;
			landed[i] = MillisecondsSince(edited);
			EXPECT_EQ(tiled(), 199u) << kind.name;
			EXPECT_EQ(ShaderCompileQueue::GetStats().submitted - submitted, kind.compiles) << kind.name;
		}
		measured += std::format("; {}: frame {:.1f}/{:.1f}/{:.1f} ms, landed {:.0f}/{:.0f}/{:.0f} ms", kind.name, frame[0], frame[1], frame[2],
								landed[0], landed[1], landed[2]);
	}
	RecordProperty("measured", measured);
	std::printf("[scale] %s\n", measured.c_str());
}

TEST_F(NodeShaderWindowFixture, SurfaceConversionAddsVertexStack) {
	const Guid graph = StageGraph("Screen.slshader", 3);
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_NE(window->GetDocument(), nullptr);
	ASSERT_EQ(Ops::GraphSettingText(*window->GetDocument(), "target"), "fullscreen");
	ASSERT_EQ(Stack::VertexContext(*window->GetDocument()), 0u);

	UI::Context* context = ContextShowing("Graph Settings");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Graph Settings"));
	Frame();
	auto shows = [&](std::string_view text) { return context->IsAlive(FindByText(*context, context->GetRoot(), text)); };
	ASSERT_TRUE(shows("Blending Mode"));
	EXPECT_FALSE(shows("Render Face"));
	EXPECT_FALSE(shows("Depth"));
	EXPECT_FALSE(shows("Fragment Normal Space"));
	EXPECT_FALSE(shows("Support Effects"));

	ClickElement(*context, FieldAfter(*context, "Material"));
	const UI::ElementHandle lit = FindByText(*context, context->GetRoot(), "Lit");
	ASSERT_TRUE(context->IsAlive(lit));
	ClickElement(*context, lit);
	Frame();
	Frame();
	EXPECT_EQ(Ops::GraphSettingText(*window->GetDocument(), "target"), "lit");
	EXPECT_NE(Stack::VertexContext(*window->GetDocument()), 0u);
	EXPECT_TRUE(shows("Render Face"));
	EXPECT_TRUE(shows("Support Effects"));
}

TEST_F(NodeShaderWindowFixture, SupportEffectsIsCheckboxOfGraphsSettings) {
	const Guid graph = StageGraph("Particle.slshader", 1);
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_NE(window->GetDocument(), nullptr);
	EXPECT_FALSE(GraphSurface::SupportsEffects(*window->GetDocument()));
	const str before = GraphDocumentOps::Write(*window->GetDocument());

	UI::Context* context = ContextShowing("Graph Settings");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Graph Settings"));
	Frame();
	ASSERT_TRUE(context->IsAlive(FindByText(*context, context->GetRoot(), "Support Effects")));
	ClickElement(*context, FieldAfter(*context, "Support Effects"));
	Frame();
	EXPECT_TRUE(GraphSurface::SupportsEffects(*window->GetDocument()));
	ClickElement(*context, FieldAfter(*context, "Support Effects"));
	Frame();
	EXPECT_FALSE(GraphSurface::SupportsEffects(*window->GetDocument()));
	EXPECT_EQ(GraphDocumentOps::Write(*window->GetDocument()), before);
}

TEST_F(NodeShaderWindowFixture, FullscreenPreviewIsRowOverStage) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	struct Shot {
		Vec3 centre;
		Vec3 corner;
	};
	auto shoot = [](NodeShaderWindow& window, Shot& out) {
		if (!SettleCompiles(window) || !window.GetPreviews().HasMainPreview()) {
			return false;
		}
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		if (!RenderSystem::ReadbackViewTarget(window.GetPreviews().GetMainTarget(), pixels, size) || size.x < 16 || size.y < 16) {
			return false;
		}
		auto texel = [&](i32 x, i32 y) {
			const size_t index = (static_cast<size_t>(y) * size.x + x) * 4;
			return Vec3(static_cast<f32>(pixels[index]), static_cast<f32>(pixels[index + 1]), static_cast<f32>(pixels[index + 2]));
		};
		out.centre = texel(size.x / 2, size.y / 2);
		out.corner = texel(2, 2);
		return true;
	};
	auto near = [](Vec3 a, Vec3 b, f32 tolerance) {
		return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
	};

	NodeShaderWindow* surface = Open(StageGraph("Surface.slshader", 1));
	ASSERT_NE(surface, nullptr);
	Shot subject;
	ASSERT_TRUE(shoot(*surface, subject));
	ASSERT_FALSE(near(subject.centre, subject.corner, 10.0f));

	NodeShaderWindow* screen = Open(StageGraph("Screen.slshader", 3));
	ASSERT_NE(screen, nullptr);
	Shot covered;
	ASSERT_TRUE(shoot(*screen, covered));
	EXPECT_TRUE(near(covered.corner, covered.centre, 2.0f));
	EXPECT_FALSE(near(covered.corner, subject.corner, 10.0f));

	ASSERT_TRUE(screen->Edit("See Through", [](GraphDocument& document) {
		Ops::SetGraphSetting(document, "blend", Ops::Quote("alpha"));
		for (GraphNode& block : document.contexts[0].blocks) {
			if (block.def == "Block/Alpha") {
				GraphInput zero;
				zero.port = "Value";
				zero.value = "[0]";
				block.inputs.push_back(std::move(zero));
			}
		}
		return true;
	}));
	Shot through;
	ASSERT_TRUE(shoot(*screen, through));
	EXPECT_TRUE(near(through.corner, subject.corner, 3.0f));
	EXPECT_FALSE(near(through.centre, through.corner, 10.0f));
}

TEST_F(NodeShaderWindowFixture, FullscreenGraphReadingClockAnimatesMainPreview) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	NodeShaderWindow* window = Open(StageGraph("Pulse.slshader", 3));
	ASSERT_NE(window, nullptr);
	ASSERT_TRUE(window->Edit("Pulse", [](GraphDocument& document) {
		u32 color = 0;
		for (const GraphNode& block : document.contexts[0].blocks) {
			color = block.def == "Block/BaseColor" ? block.id : color;
		}
		GraphNode time;
		time.id = document.nextId++;
		time.def = "Input/Time";
		GraphNode fraction;
		fraction.id = document.nextId++;
		fraction.def = "Math/Fraction";
		const u32 from = time.id;
		const u32 through = fraction.id;
		document.nodes.push_back(std::move(time));
		document.nodes.push_back(std::move(fraction));
		GraphDocumentOps::Connect(document, from, "Time", through, "In");
		GraphDocumentOps::Connect(document, through, "Out", color, "Value");
		return color != 0;
	}));
	ASSERT_TRUE(SettleCompiles(*window));
	ASSERT_TRUE(window->GetPreviews().HasMainPreview());
	auto centre = [&](Vec3& out) {
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		if (!RenderSystem::ReadbackViewTarget(window->GetPreviews().GetMainTarget(), pixels, size) || size.x < 2 || size.y < 2) {
			return false;
		}
		const size_t index = (static_cast<size_t>(size.y / 2) * size.x + size.x / 2) * 4;
		out = Vec3(static_cast<f32>(pixels[index]), static_cast<f32>(pixels[index + 1]), static_cast<f32>(pixels[index + 2]));
		return true;
	};
	Vec3 before;
	ASSERT_TRUE(centre(before));
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
	while (std::chrono::steady_clock::now() < until) {
		Frame();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Vec3 after;
	ASSERT_TRUE(centre(after));
	EXPECT_GT(std::abs(after.x - before.x), 3.0f);
}

TEST_F(NodeShaderWindowFixture, SkyGraphsSettingsAreMaterialAlone) {
	const Guid graph = StageGraph("Sky.slshader", 4);
	NodeShaderWindow* window = Open(graph);
	ASSERT_NE(window, nullptr);
	ASSERT_NE(window->GetDocument(), nullptr);
	ASSERT_EQ(Ops::GraphSettingText(*window->GetDocument(), "target"), "sky");
	ASSERT_EQ(Stack::VertexContext(*window->GetDocument()), 0u);

	UI::Context* context = ContextShowing("Graph Settings");
	ASSERT_NE(context, nullptr);
	ClickElement(*context, FindByText(*context, context->GetRoot(), "Graph Settings"));
	Frame();
	auto shows = [&](std::string_view text) { return context->IsAlive(FindByText(*context, context->GetRoot(), text)); };
	ASSERT_TRUE(context->IsAlive(FieldAfter(*context, "Material")));
	EXPECT_FALSE(shows("Blending Mode"));
	EXPECT_FALSE(shows("Render Face"));
	EXPECT_FALSE(shows("Depth"));
	EXPECT_FALSE(shows("Fragment Normal Space"));
	EXPECT_FALSE(shows("Support Effects"));

	ClickElement(*context, FieldAfter(*context, "Material"));
	const UI::ElementHandle lit = FindByText(*context, context->GetRoot(), "Lit");
	ASSERT_TRUE(context->IsAlive(lit));
	ClickElement(*context, lit);
	Frame();
	Frame();
	EXPECT_EQ(Ops::GraphSettingText(*window->GetDocument(), "target"), "lit");
	EXPECT_NE(Stack::VertexContext(*window->GetDocument()), 0u);
	EXPECT_TRUE(shows("Blending Mode"));
	EXPECT_TRUE(shows("Support Effects"));
}

TEST_F(NodeShaderWindowFixture, SkyGraphsMainPreviewIsStagesSky) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	struct Shot {
		Vec3 centre;
		Vec3 corner;
	};
	auto shoot = [](NodeShaderWindow& window, Shot& out) {
		if (!SettleCompiles(window) || !window.GetPreviews().HasMainPreview()) {
			return false;
		}
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		if (!RenderSystem::ReadbackViewTarget(window.GetPreviews().GetMainTarget(), pixels, size) || size.x < 16 || size.y < 16) {
			return false;
		}
		auto texel = [&](i32 x, i32 y) {
			const size_t index = (static_cast<size_t>(y) * size.x + x) * 4;
			return Vec3(static_cast<f32>(pixels[index]), static_cast<f32>(pixels[index + 1]), static_cast<f32>(pixels[index + 2]));
		};
		out.centre = texel(size.x / 2, size.y / 2);
		out.corner = texel(2, 2);
		return true;
	};
	auto near = [](Vec3 a, Vec3 b, f32 tolerance) {
		return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
	};

	NodeShaderWindow* surface = Open(StageGraph("Surface.slshader", 1));
	ASSERT_NE(surface, nullptr);
	Shot subject;
	ASSERT_TRUE(shoot(*surface, subject));
	ASSERT_FALSE(near(subject.centre, subject.corner, 10.0f));

	NodeShaderWindow* sky = Open(StageGraph("Sky.slshader", 4));
	ASSERT_NE(sky, nullptr);
	ASSERT_TRUE(sky->Edit("Orange", [](GraphDocument& document) {
		bool painted = false;
		for (GraphContext& context : document.contexts) {
			for (GraphNode& block : context.blocks) {
				if (block.def == "Block/BaseColor") {
					GraphInput orange;
					orange.port = "Value";
					orange.value = "[0.8, 0.2, 0.05]";
					block.inputs.push_back(std::move(orange));
					painted = true;
				}
			}
		}
		return painted;
	}));
	Shot covered;
	ASSERT_TRUE(shoot(*sky, covered));
	EXPECT_TRUE(near(covered.corner, covered.centre, 2.0f));
	EXPECT_FALSE(near(covered.corner, subject.corner, 10.0f));
	EXPECT_GT(covered.corner.x, covered.corner.z + 40.0f);

	ASSERT_TRUE(sky->Edit("Unlit", [](GraphDocument& document) {
		Ops::SetGraphSetting(document, "target", Ops::Quote("unlit"));
		Stack::EnsureVertexStack(document);
		return true;
	}));
	Shot back;
	ASSERT_TRUE(shoot(*sky, back));
	EXPECT_TRUE(near(back.corner, subject.corner, 3.0f));
	EXPECT_FALSE(near(back.centre, back.corner, 10.0f));
}
