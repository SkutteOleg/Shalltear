#include "TestsPCH.h"

#include "ZoneProbe.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Window.h"

#include "Graphics/RenderGraph.h"

#include <chrono>
#include <filesystem>

using namespace Shalltear;
using namespace Shalltear::GraphicsInternal;

using ZoneProbe::ZoneCalls;
using ZoneProbe::ZoneScope;

namespace {

	RHI::TextureHandle FakeTexture(u32 index) {
		RHI::TextureHandle handle;
		handle.index = index;
		handle.generation = 1;
		return handle;
	}

	void NeverRecorded(GraphExecution&, const GraphPass&) {
		ADD_FAILURE() << "mechanism tier pass recorded";
	}

	struct StatePeek {
		RHI::Device* device = nullptr;
		RHI::TextureHandle texture;
		RHI::ResourceState seen = RHI::ResourceState::Undefined;
	};

	StatePeek* _statePeek = nullptr;

	void RecordNothing(GraphExecution&, const GraphPass&) {}

	void RecordStatePeek(GraphExecution&, const GraphPass&) {
		_statePeek->seen = _statePeek->device->GetTextureState(_statePeek->texture);
	}

	void DeclarePass(RenderGraph& graph, const char* name) {
		graph.AddPass(name, 0, &NeverRecorded, PassWork{});
	}

	u32 LivePassCount(const RenderGraph& graph) {
		u32 live = 0;
		for (u32 i = 0; i < graph.GetPassCount(); ++i) {
			if (graph.IsPassLive(i)) {
				++live;
			}
		}
		return live;
	}

} // anonymous namespace

TEST(RenderGraphMechanismTest, PassWhoseOutputNothingReadsIsCulled) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId unread = graph.CreateVirtual();
	GraphResourceId presented = graph.CreateVirtual(true);

	DeclarePass(graph, "Unread");
	graph.Write(unread, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Sunk");
	graph.Write(presented, RHI::ResourceState::Undefined);
	graph.Compile();

	EXPECT_FALSE(graph.IsPassLive(0));
	EXPECT_TRUE(graph.IsPassLive(1));
	EXPECT_EQ(graph.GetCulledPassCount(), 1u);
}

TEST(RenderGraphMechanismTest, CullingWalksBackwardThroughAWholeChain) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId first = graph.CreateVirtual();
	GraphResourceId second = graph.CreateVirtual();
	GraphResourceId unread = graph.CreateVirtual();

	DeclarePass(graph, "Source");
	graph.Write(first, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Middle");
	graph.Read(first, RHI::ResourceState::Undefined);
	graph.Write(second, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Tail");
	graph.Read(second, RHI::ResourceState::Undefined);
	graph.Write(unread, RHI::ResourceState::Undefined);
	graph.Compile();

	EXPECT_EQ(LivePassCount(graph), 0u);
	EXPECT_EQ(graph.GetCulledPassCount(), 3u);
}

TEST(RenderGraphMechanismTest, SameChainSurvivesWhenItReachesSink) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId first = graph.CreateVirtual();
	GraphResourceId second = graph.CreateVirtual();
	GraphResourceId sunk = graph.CreateVirtual(true);

	DeclarePass(graph, "Source");
	graph.Write(first, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Middle");
	graph.Read(first, RHI::ResourceState::Undefined);
	graph.Write(second, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Tail");
	graph.Read(second, RHI::ResourceState::Undefined);
	graph.Write(sunk, RHI::ResourceState::Undefined);
	graph.Compile();

	EXPECT_EQ(LivePassCount(graph), 3u);
	EXPECT_EQ(graph.GetCulledPassCount(), 0u);
}

TEST(RenderGraphMechanismTest, DisablingCullingRedensTheCullingGate) {
	RenderGraph graph;
	graph.SetCullingDisabled(true);
	graph.Begin(nullptr);
	GraphResourceId unread = graph.CreateVirtual();
	GraphResourceId presented = graph.CreateVirtual(true);

	DeclarePass(graph, "Unread");
	graph.Write(unread, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Sunk");
	graph.Write(presented, RHI::ResourceState::Undefined);
	graph.Compile();

	EXPECT_TRUE(graph.IsPassLive(0));
	EXPECT_EQ(graph.GetCulledPassCount(), 0u);
}

TEST(RenderGraphMechanismTest, OneTextureImportedTwiceIsOneResource) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId written = graph.Import(FakeTexture(7));
	GraphResourceId sunk = graph.Import(FakeTexture(9), true, RHI::ResourceState::Present);

	DeclarePass(graph, "Producer");
	graph.Write(written, RHI::ResourceState::RenderTarget);

	GraphResourceId again = graph.Import(FakeTexture(7));
	EXPECT_EQ(written, again);

	DeclarePass(graph, "Consumer");
	graph.Read(again, RHI::ResourceState::ShaderResource);
	graph.Write(sunk, RHI::ResourceState::RenderTarget);
	graph.Compile();

	EXPECT_TRUE(graph.IsPassLive(0));
	EXPECT_TRUE(graph.IsPassLive(1));
}

TEST(RenderGraphMechanismTest, CompileRemovesPassesWithoutReorderingThem) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId sunk = graph.CreateVirtual(true);
	GraphResourceId unread = graph.CreateVirtual();

	DeclarePass(graph, "First");
	graph.Write(sunk, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Dropped");
	graph.Write(unread, RHI::ResourceState::Undefined);
	DeclarePass(graph, "Last");
	graph.Write(sunk, RHI::ResourceState::Undefined);
	graph.Compile();

	ASSERT_EQ(graph.GetPassCount(), 3u);
	EXPECT_STREQ(graph.GetPass(0).name, "First");
	EXPECT_STREQ(graph.GetPass(1).name, "Dropped");
	EXPECT_STREQ(graph.GetPass(2).name, "Last");
	EXPECT_FALSE(graph.IsPassLive(1));
}

namespace {

	RHI::BufferHandle FakeBuffer(u32 index) {
		RHI::BufferHandle handle;
		handle.index = index;
		handle.generation = 1;
		return handle;
	}

	bool OrderedAfterUnordered(const RenderGraph& graph, u32 pass, u16 access) {
		return graph.GetAccess(graph.GetPass(pass).firstAccess + access).unorderedBarrier;
	}

} // anonymous namespace

TEST(RenderGraphMechanismTest, BufferSinkKeepsWriter) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId kept = graph.ImportBuffer(FakeBuffer(1), true);
	GraphResourceId dropped = graph.ImportBuffer(FakeBuffer(2));
	DeclarePass(graph, "Simulate");
	graph.Write(kept, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "Orphan");
	graph.Write(dropped, RHI::ResourceState::UnorderedAccess);
	graph.Compile();

	EXPECT_TRUE(graph.IsPassLive(0));
	EXPECT_FALSE(graph.IsPassLive(1));
	EXPECT_EQ(graph.GetPass(0).buffers[0], FakeBuffer(1));
	EXPECT_FALSE(graph.GetPass(0).textures[0].IsValid());
}

TEST(RenderGraphMechanismTest, OneBufferImportedTwiceIsOneResource) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId first = graph.ImportBuffer(FakeBuffer(4));
	GraphResourceId again = graph.ImportBuffer(FakeBuffer(4), true);
	EXPECT_EQ(first, again);
	DeclarePass(graph, "Writer");
	graph.Write(first, RHI::ResourceState::UnorderedAccess);
	graph.Compile();
	EXPECT_TRUE(graph.IsPassLive(0));
}

TEST(RenderGraphMechanismTest, UnorderedAccessesAreOrderedUnlessBothRead) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId lists = graph.ImportBuffer(FakeBuffer(1), true);
	GraphResourceId arguments = graph.ImportBuffer(FakeBuffer(2), true);
	GraphResourceId counts = graph.ImportBuffer(FakeBuffer(3), true);
	GraphResourceId orphan = graph.ImportBuffer(FakeBuffer(4));

	DeclarePass(graph, "Initialize");
	graph.Write(lists, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "Update");
	graph.Read(arguments, RHI::ResourceState::IndirectArgument);
	graph.Write(lists, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "Culled");
	graph.Read(lists, RHI::ResourceState::UnorderedAccess);
	graph.Write(orphan, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "Arguments");
	graph.Read(lists, RHI::ResourceState::UnorderedAccess);
	graph.Write(arguments, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "Peek");
	graph.Read(lists, RHI::ResourceState::UnorderedAccess);
	graph.Write(counts, RHI::ResourceState::UnorderedAccess);
	DeclarePass(graph, "PeekAgain");
	graph.Read(lists, RHI::ResourceState::UnorderedAccess);
	graph.Write(counts, RHI::ResourceState::UnorderedAccess);
	graph.Compile();
	ASSERT_FALSE(graph.IsPassLive(2));

	EXPECT_TRUE(OrderedAfterUnordered(graph, 0, 0));
	EXPECT_FALSE(OrderedAfterUnordered(graph, 1, 0));
	EXPECT_TRUE(OrderedAfterUnordered(graph, 1, 1));
	EXPECT_TRUE(OrderedAfterUnordered(graph, 3, 0));
	EXPECT_FALSE(OrderedAfterUnordered(graph, 3, 1));
	EXPECT_FALSE(OrderedAfterUnordered(graph, 4, 0));
	EXPECT_TRUE(OrderedAfterUnordered(graph, 4, 1));
	EXPECT_TRUE(OrderedAfterUnordered(graph, 5, 1));
	EXPECT_FALSE(OrderedAfterUnordered(graph, 5, 0));
}

TEST(RenderGraphMechanismTest, ExtraBuffersBarriered) {
	RenderGraph graph;
	graph.Begin(nullptr);
	GraphResourceId color = graph.Import(FakeTexture(1), true);
	constexpr u32 BUFFERS = 12;
	GraphResourceId buffers[BUFFERS];
	for (u32 i = 0; i < BUFFERS; ++i) {
		buffers[i] = graph.ImportBuffer(FakeBuffer(10 + i), true);
	}
	DeclarePass(graph, "Simulate");
	for (u32 i = 0; i < BUFFERS; ++i) {
		graph.Write(buffers[i], RHI::ResourceState::UnorderedAccess);
	}
	DeclarePass(graph, "Scene");
	graph.Write(color, RHI::ResourceState::RenderTarget);
	for (u32 i = 0; i < BUFFERS; ++i) {
		graph.Read(buffers[i], i % 3 == 0 ? RHI::ResourceState::IndirectArgument : RHI::ResourceState::ShaderResource);
	}
	graph.Compile();

	ASSERT_TRUE(graph.IsPassLive(0));
	ASSERT_TRUE(graph.IsPassLive(1));
	const GraphPass& scene = graph.GetPass(1);
	ASSERT_EQ(scene.accessCount, 1 + BUFFERS);
	EXPECT_EQ(scene.textures[0], FakeTexture(1));
	EXPECT_EQ(scene.buffers[1], FakeBuffer(10));
	for (u32 i = 0; i < BUFFERS; ++i) {
		EXPECT_EQ(graph.GetAccessBuffer(scene.firstAccess + 1 + i), FakeBuffer(10 + i)) << i;
	}
	EXPECT_FALSE(graph.GetAccessBuffer(scene.firstAccess).IsValid());

	const GraphPass& simulate = graph.GetPass(0);
	ASSERT_EQ(simulate.accessCount, BUFFERS);
	for (u32 i = 0; i < BUFFERS; ++i) {
		EXPECT_TRUE(graph.GetAccess(simulate.firstAccess + i).unorderedBarrier);
	}
}

TEST(RenderGraphMechanismTest, BeginClearsFrameItDoesNotAccumulate) {
	RenderGraph graph;
	for (u32 frame = 0; frame < 3; ++frame) {
		graph.Begin(nullptr);
		GraphResourceId sunk = graph.CreateVirtual(true);
		DeclarePass(graph, "Only");
		graph.Write(sunk, RHI::ResourceState::Undefined);
		graph.Compile();
		ASSERT_EQ(graph.GetPassCount(), 1u) << "frame " << frame;
		ASSERT_EQ(graph.GetCulledPassCount(), 0u) << "frame " << frame;
	}
}

namespace {

	class UIOnlyOverlay : public IWindowOverlay {
	public:
		Vector<Handle<Window>> windows{GetMemoryResource(MemTag::Graphics)};

		void CollectOverlayWindows(Vector<Handle<Window>>& out) override {
			for (Handle<Window> window : windows) {
				out.push_back(window);
			}
		}

		u32 RecordOverlay(Handle<Window>, RHI::CommandList&, RHI::TextureHandle, Vec2I) override {
			return 0;
		}
	};

	class InstalledOverlay {
	public:
		explicit InstalledOverlay(IWindowOverlay* overlay) { RenderSystem::SetWindowOverlay(overlay); }
		~InstalledOverlay() { RenderSystem::SetWindowOverlay(nullptr); }

		InstalledOverlay(const InstalledOverlay&) = delete;
		InstalledOverlay& operator=(const InstalledOverlay&) = delete;
	};

	std::filesystem::path RepoDir(const char* relative) {
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

	class RenderGraphFrameFixture : public ::testing::Test {
	protected:
		bool _mounted = false;

		void SetUp() override {
			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
				_mounted = true;
			}
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			SceneSystem::Initialize();
		}

		void TearDown() override {
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			if (_mounted) {
				FileSystem::Unmount("engine");
			}
		}

		Handle<SkinnedMeshRenderer> AddACharacter(Handle<Scene> scene) {
			Handle<SceneObject> object = SceneSystem::CreateObject("Character", scene);
			return object->AddComponent<SkinnedMeshRenderer>();
		}
	};

} // anonymous namespace

TEST_F(RenderGraphFrameFixture, UnreadSkinningCulled) {
#if defined(SHALLTEAR_CONFIG_RELEASE)
	GTEST_SKIP() << "zones compiled out in Release";
#else
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<Scene> scene = SceneSystem::CreateScene("Culled");
	ASSERT_TRUE(AddACharacter(scene).IsValid());

	WindowDesc windowDesc;
	windowDesc.title = "RenderGraphCulling";
	windowDesc.clientSize = {128, 128};
	windowDesc.flags = WindowFlags::Hidden;
	Handle<Window> window = WindowSystem::Create(windowDesc);
	ASSERT_TRUE(window.IsValid());

	UIOnlyOverlay overlay;
	overlay.windows.push_back(window);
	InstalledOverlay installed{&overlay};

	for (u32 frame = 0; frame < 3; ++frame) {
		WindowSystem::PumpEvents();
		RenderSystem::RenderAll();
	}

	ZoneScope zones;
	WindowSystem::PumpEvents();
	RenderSystem::RenderAll();
	Profiler::NextFrame();
	EXPECT_EQ(ZoneCalls("Render.Skinning"), 0u);
	EXPECT_EQ(ZoneCalls("Render.Window.Clear"), 1u);

	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
#endif
}

TEST_F(RenderGraphFrameFixture, SkinningPassRunsWhenViewReadsIt) {
#if defined(SHALLTEAR_CONFIG_RELEASE)
	GTEST_SKIP() << "zones compiled out in Release";
#else
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<Scene> scene = SceneSystem::CreateScene("Read");
	ASSERT_TRUE(AddACharacter(scene).IsValid());

	WindowDesc windowDesc;
	windowDesc.title = "RenderGraphCullingControl";
	windowDesc.clientSize = {128, 128};
	windowDesc.flags = WindowFlags::Hidden;
	Handle<Window> window = WindowSystem::Create(windowDesc);
	ASSERT_TRUE(window.IsValid());

	UIOnlyOverlay overlay;
	overlay.windows.push_back(window);
	InstalledOverlay installed{&overlay};

	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);

	for (u32 frame = 0; frame < 3; ++frame) {
		WindowSystem::PumpEvents();
		RenderSystem::RenderAll();
	}

	ZoneScope zones;
	ViewDesc desc;
	desc.projection = Mat4::Perspective(60.0f * Math::DEG_TO_RAD, 1.0f, 0.1f, 100.0f);
	RenderSystem::SubmitView(target, desc);
	WindowSystem::PumpEvents();
	RenderSystem::RenderAll();
	Profiler::NextFrame();
	EXPECT_EQ(ZoneCalls("Render.Skinning"), 1u);
	EXPECT_EQ(ZoneCalls("Render.View.Scene"), 1u);

	RenderSystem::DestroyViewTarget(target);
	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
#endif
}

TEST_F(RenderGraphFrameFixture, WorldViewDoesNotDeformIsolatedPartition) {
#if defined(SHALLTEAR_CONFIG_RELEASE)
	GTEST_SKIP() << "zones compiled out in Release";
#else
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<Scene> world = SceneSystem::CreateScene("World");
	ASSERT_TRUE(AddACharacter(world).IsValid());

	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);
	for (u32 frame = 0; frame < 3; ++frame) {
		WindowSystem::PumpEvents();
		RenderSystem::RenderAll();
	}

	ZoneScope zones;
	ViewDesc desc;
	desc.projection = Mat4::Perspective(60.0f * Math::DEG_TO_RAD, 1.0f, 0.1f, 100.0f);
	RenderSystem::SubmitView(target, desc);
	RenderSystem::RenderAll();
	Profiler::NextFrame();
	EXPECT_EQ(ZoneCalls("Render.Skinning"), 1u);
	EXPECT_EQ(ZoneCalls("Render.Skinning.Isolated"), 0u);

	RenderSystem::DestroyViewTarget(target);
#endif
}

TEST_F(RenderGraphFrameFixture, FramesPassZonesFitProfilersSiteTable) {
#if defined(SHALLTEAR_CONFIG_RELEASE)
	GTEST_SKIP() << "zones compiled out in Release";
#else
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);
	WindowDesc windowDesc;
	windowDesc.title = "RenderGraphZones";
	windowDesc.clientSize = {128, 128};
	windowDesc.flags = WindowFlags::Hidden;
	Handle<Window> window = WindowSystem::Create(windowDesc);
	ASSERT_TRUE(window.IsValid());
	UIOnlyOverlay overlay;
	overlay.windows.push_back(window);
	InstalledOverlay installed{&overlay};

	ZoneScope zones;
	ViewDesc desc;
	desc.projection = Mat4::Perspective(60.0f * Math::DEG_TO_RAD, 1.0f, 0.1f, 100.0f);
	RenderSystem::SubmitView(target, desc);
	WindowSystem::PumpEvents();
	RenderSystem::RenderAll();
	Profiler::NextFrame();

	Profiler::FrameResults frame = Profiler::GetFrameResults();
	EXPECT_EQ(frame.droppedSites, 0u);

	RenderSystem::DestroyViewTarget(target);
	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
#endif
}

namespace {

	u32 LiveTextures() { return RenderSystem::GetStats().liveTextures; }

	ViewDesc AnyView() {
		ViewDesc desc;
		desc.projection = Mat4::Perspective(60.0f * Math::DEG_TO_RAD, 1.0f, 0.1f, 100.0f);
		return desc;
	}

} // anonymous namespace

TEST_F(RenderGraphFrameFixture, ViewTargetNobodySubmitsHoldsOnlyResult) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const u32 before = LiveTextures();
	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);
	RenderSystem::RenderAll();
	EXPECT_EQ(LiveTextures() - before, 1u);

	RenderSystem::DestroyViewTarget(target);
}

TEST_F(RenderGraphFrameFixture, SameSizeViewsShareChain) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const u32 before = LiveTextures();
	ViewTargetId first = RenderSystem::CreateViewTarget({64, 64});
	ViewTargetId second = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(first, INVALID_VIEW_TARGET);
	ASSERT_NE(second, INVALID_VIEW_TARGET);

	RenderSystem::SubmitView(first, AnyView());
	RenderSystem::SubmitView(second, AnyView());
	RenderSystem::RenderAll();

	EXPECT_EQ(LiveTextures() - before, 4u);

	RenderSystem::DestroyViewTarget(first);
	RenderSystem::DestroyViewTarget(second);
}

TEST_F(RenderGraphFrameFixture, DifferentSizeViewsDontShare) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const u32 before = LiveTextures();
	ViewTargetId first = RenderSystem::CreateViewTarget({64, 64});
	ViewTargetId second = RenderSystem::CreateViewTarget({96, 96});
	ASSERT_NE(first, INVALID_VIEW_TARGET);
	ASSERT_NE(second, INVALID_VIEW_TARGET);

	RenderSystem::SubmitView(first, AnyView());
	RenderSystem::SubmitView(second, AnyView());
	RenderSystem::RenderAll();

	EXPECT_EQ(LiveTextures() - before, 6u);

	RenderSystem::DestroyViewTarget(first);
	RenderSystem::DestroyViewTarget(second);
}

TEST_F(RenderGraphFrameFixture, UnchangedFrameAsksPoolForNothingNew) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);
	for (u32 frame = 0; frame < 3; ++frame) {
		RenderSystem::SubmitView(target, AnyView());
		RenderSystem::RenderAll();
	}
	const u32 settled = LiveTextures();
	for (u32 frame = 0; frame < 30; ++frame) {
		RenderSystem::SubmitView(target, AnyView());
		RenderSystem::RenderAll();
		ASSERT_EQ(LiveTextures(), settled) << "frame " << frame;
	}

	RenderSystem::DestroyViewTarget(target);
}

TEST_F(RenderGraphFrameFixture, UnsubmittedViewReleasesChain) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	WindowDesc windowDesc;
	windowDesc.title = "RenderGraphPoolIdle";
	windowDesc.clientSize = {128, 128};
	windowDesc.flags = WindowFlags::Hidden;
	Handle<Window> window = WindowSystem::Create(windowDesc);
	ASSERT_TRUE(window.IsValid());
	UIOnlyOverlay overlay;
	overlay.windows.push_back(window);
	InstalledOverlay installed{&overlay};

	ViewTargetId target = RenderSystem::CreateViewTarget({64, 64});
	ASSERT_NE(target, INVALID_VIEW_TARGET);
	for (u32 frame = 0; frame < 3; ++frame) {
		WindowSystem::PumpEvents();
		RenderSystem::SubmitView(target, AnyView());
		RenderSystem::RenderAll();
	}
	const u32 withChain = LiveTextures();

	u32 frames = 0;
	constexpr u32 GIVE_UP_AFTER = 400;
	while (frames < GIVE_UP_AFTER && LiveTextures() == withChain) {
		WindowSystem::PumpEvents();
		RenderSystem::RenderAll();
		++frames;
	}
	EXPECT_LT(frames, GIVE_UP_AFTER);
	EXPECT_EQ(withChain - LiveTextures(), 2u);
	EXPECT_TRUE(RenderSystem::GetViewTargetTexture(target).IsValid());

	RenderSystem::DestroyViewTarget(target);
	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
}

namespace {

	struct KernelPush {
		u32 mode = 0;
		u32 count = 0;
		u32 base = 0;
		u32 spin = 0;
		u32 mask = 0;
	};

	struct BufferPassContext {
		RHI::PipelineHandle pipeline;
		u64 size = 0;
		KernelPush pushes[2];
	};
	BufferPassContext* _bufferPass = nullptr;

	void RecordKernelPass(GraphExecution& exec, const GraphPass& pass) {
		const BufferPassContext& context = *_bufferPass;
		const KernelPush& push = context.pushes[pass.work.index];
		exec.cmd->SetPipeline(context.pipeline);
		exec.cmd->BindTransientSet(0, exec.device->CreateTransientSet(context.pipeline, 0).WriteStorageBuffer(0, pass.buffers[0], 0, context.size).Commit());
		exec.cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
		exec.cmd->Dispatch((push.count + 63) / 64);
	}

} // anonymous namespace

TEST(RenderGraphBufferDeviceTest, SharedBufferWritesOrdered) {
	RHI::DeviceDesc deviceDesc;
	deviceDesc.headless = true;
	RHI::Device* device = RHI::Device::Create(deviceDesc);
	if (!device) {
		GTEST_SKIP() << "no Vulkan ICD available";
	}
	ShaderAsset kernel;
	const std::filesystem::path source = RepoDir("Source/Tests/Fixtures/Graphics/ComputeFixture.hlsl");
	if (source.empty() || !kernel.Import(Path(source.generic_string()))) {
		RHI::Device::Destroy(device);
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	const Vector<u8>& spirv = kernel.variants[SHADER_BASE_VARIANT].computeSpirv;
	RHI::ComputePipelineDesc pipelineDesc;
	pipelineDesc.spirv = {spirv.data(), spirv.size()};
	pipelineDesc.bytecodeFormat = kernel.bytecodeFormat;
	pipelineDesc.bindings = {kernel.bindings.data(), kernel.bindings.size()};
	pipelineDesc.pushConstantSize = kernel.pushConstantSize;
	RHI::PipelineHandle pipeline = device->CreateComputePipeline(pipelineDesc);
	ASSERT_TRUE(pipeline.IsValid());

	constexpr u32 COUNT = 1u << 16;
	RHI::BufferDesc bufferDesc;
	bufferDesc.size = COUNT * 4;
	bufferDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferSrc;
	RHI::BufferHandle values = device->CreateBufferResource(bufferDesc);
	BufferPassContext context;
	context.pipeline = pipeline;
	context.size = bufferDesc.size;
	context.pushes[0] = {0, COUNT, 1, 2000};
	context.pushes[1] = {1, COUNT};
	_bufferPass = &context;

	RenderGraph graph;
	RHI::CommandList* cmd = device->BeginFrame();
	graph.Begin(device);
	GraphResourceId resource = graph.ImportBuffer(values, true);
	PassWork spin;
	spin.index = 0;
	graph.AddPass("Spin", 0, &RecordKernelPass, spin);
	graph.Write(resource, RHI::ResourceState::UnorderedAccess);
	PassWork doubled;
	doubled.index = 1;
	graph.AddPass("Double", 0, &RecordKernelPass, doubled);
	graph.Write(resource, RHI::ResourceState::UnorderedAccess);
	graph.Compile();
	GraphExecution exec;
	exec.cmd = cmd;
	exec.device = device;
	graph.Execute(exec);
	device->EndFrame();
	device->WaitIdle();
	_bufferPass = nullptr;

	Vector<u32> out{GetMemoryResource(MemTag::Graphics)};
	out.assign(COUNT, 0);
	device->ReadbackBuffer(values, {reinterpret_cast<u8*>(out.data()), out.size() * 4});
	u32 wrong = 0;
	for (u32 i = 0; i < COUNT; ++i) {
		wrong += out[i] == 2 * (1 + i) ? 0u : 1u;
	}
	EXPECT_EQ(wrong, 0u) << "of " << COUNT << ' ' << out[0];
	EXPECT_EQ(graph.GetExecutedPassCount(), 2u);

	graph.DestroyPool();
	device->DestroyResource(pipeline);
	device->DestroyResource(values);
	for (int i = 0; i < 2; ++i) {
		device->BeginFrame();
		device->EndFrame();
	}
	device->WaitIdle();
	RHI::Device::Destroy(device);
}

TEST(RenderGraphTextureDeviceTest, LaterReadRestoresFinalState) {
	RHI::DeviceDesc deviceDesc;
	deviceDesc.headless = true;
	RHI::Device* device = RHI::Device::Create(deviceDesc);
	if (!device) {
		GTEST_SKIP() << "no Vulkan ICD available";
	}
	RHI::TextureDesc desc;
	desc.size = {16, 16};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
	RHI::TextureHandle texture = device->CreateTextureResource(desc);
	ASSERT_TRUE(texture.IsValid());
	StatePeek peek;
	peek.device = device;
	peek.texture = texture;
	_statePeek = &peek;

	RenderGraph graph;
	RHI::CommandList* cmd = device->BeginFrame();
	graph.Begin(device);
	const GraphResourceId resource = graph.Import(texture, true, RHI::ResourceState::ShaderResource);
	graph.AddPass("Draw", 0, &RecordNothing, PassWork{});
	graph.Write(resource, RHI::ResourceState::RenderTarget);
	graph.AddPass("Peek", 0, &RecordStatePeek, PassWork{});
	graph.Write(graph.CreateVirtual(true), RHI::ResourceState::Undefined);
	graph.AddPass("Copy", 0, &RecordNothing, PassWork{});
	graph.Read(resource, RHI::ResourceState::TransferSrc);
	graph.Write(graph.CreateVirtual(true), RHI::ResourceState::Undefined);
	graph.Compile();
	GraphExecution exec;
	exec.cmd = cmd;
	exec.device = device;
	graph.Execute(exec);
	_statePeek = nullptr;
	EXPECT_EQ(graph.GetExecutedPassCount(), 3u);
	EXPECT_EQ(peek.seen, RHI::ResourceState::ShaderResource);
	EXPECT_EQ(device->GetTextureState(texture), RHI::ResourceState::ShaderResource);
	device->EndFrame();
	device->WaitIdle();

	graph.DestroyPool();
	device->DestroyResource(texture);
	for (int i = 0; i < 2; ++i) {
		device->BeginFrame();
		device->EndFrame();
	}
	device->WaitIdle();
	RHI::Device::Destroy(device);
}
