#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/EditorConsole.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* PROCEDURAL_SCRIPT = R"(using System;
using Shalltear;

namespace ProceduralTests;

public sealed class MarkerComponent : Behavior {
	public int code = 7;
}

public sealed class ProceduralProbe : Behavior {
	public int vertexCount;
	public int indexCount;
	public int submeshCount;
	public int textureWidth;
	public int textureHeight;
	public int pixelByteCount;
	public bool meshDestroyedToNull;
	public bool textureDestroyedToNull;
	public bool materialDestroyedToNull;
	public bool tryGetBeforeAdd;
	public bool hasBeforeAdd;
	public bool tryGetAfterAdd;
	public bool hasAfterAdd;
	public int markerCode;
	bool _ran;

	protected override void OnUpdate() {
		if (_ran) {
			return;
		}
		_ran = true;
		tryGetBeforeAdd = TryGetComponent<MarkerComponent>(out MarkerComponent? missing);
		hasBeforeAdd = HasComponent<MarkerComponent>() || missing is not null;

		MarkerComponent added = AddComponent<MarkerComponent>();
		tryGetAfterAdd = TryGetComponent<MarkerComponent>(out MarkerComponent? found);
		hasAfterAdd = HasComponent<MarkerComponent>();
		markerCode = found is not null ? found.code : -1;
		if (!ReferenceEquals(added, found)) {
			markerCode = -2;
		}

		Mesh mesh = Asset.CreateTransient<Mesh>("Quad");
		Span<MeshVertex> verts = stackalloc MeshVertex[3];
		verts[0] = new MeshVertex { position = new Vec3(0f, 0f, 0f), normal = Vec3.Up, u = 0f, v = 0f };
		verts[1] = new MeshVertex { position = new Vec3(1f, 0f, 0f), normal = Vec3.Up, u = 1f, v = 0f };
		verts[2] = new MeshVertex { position = new Vec3(0f, 1f, 0f), normal = Vec3.Up, u = 0f, v = 1f };
		mesh.SetVertices(verts);

		Span<uint> indices = stackalloc uint[3] { 0u, 1u, 2u };
		mesh.SetIndices(indices);

		Span<SubMesh> submeshes = stackalloc SubMesh[1];
		submeshes[0] = new SubMesh {
			firstIndex = 0u,
			indexCount = 3u,
			materialSlot = 0u,
			bounds = new AABB { min = Vec3.Zero, max = new Vec3(1f, 1f, 0f) }
		};
		mesh.SetSubmeshes(submeshes);

		vertexCount = mesh.Vertices.Length;
		indexCount = mesh.Indices.Length;
		submeshCount = mesh.SubmeshCount;

		Asset.DestroyTransient(mesh);
		meshDestroyedToNull = mesh == null;

		Texture texture = Asset.CreateTransient<Texture>("Checker");
		texture.Size = new Vec2I(2, 2);
		Span<byte> pixels = stackalloc byte[16] {
			255, 0, 0, 255,   0, 255, 0, 255,
			0, 0, 255, 255,   255, 255, 255, 255
		};
		texture.SetPixels(pixels);
		Vec2I size = texture.Size;
		textureWidth = size.x;
		textureHeight = size.y;
		pixelByteCount = texture.Pixels.Length;

		Asset.DestroyTransient(texture);
		textureDestroyedToNull = texture == null;

		Material material = Asset.CreateTransient<Material>("RuntimeMat");
		material.SetColor("baseColor", new Color(0.25f, 0.5f, 0.75f, 1f));
		Asset.DestroyTransient(material);
		materialDestroyedToNull = material == null;
	}
}
)";

	class CSharpProceduralAssetsTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		Handle<Scene> _scene;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			ASSERT_TRUE(Attach()) << _error;

			_root = fs::temp_directory_path() /
					("slt_cs_proc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "ProceduralGame";
			fs::create_directories(_project / "Assets" / "Scripts");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownReload();
			}
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			FileSystem::Unmount("engine");
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		bool CompileAndLoad(const str& source, fs::path& outAssembly) {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return false;
			}
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
			files.projectName = "ProceduralGame";
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(files, written)) {
				return false;
			}
			std::ofstream(_project / "Assets" / "Scripts" / "ProceduralProbe.cs", std::ios::binary) << source;
			CompileOptions options;
			options.projectPath = _project.generic_string();
			options.projectFile = "ProceduralGame.Scripts.csproj";
			options.muxer = (fs::path(sdk) / _editor->muxerName).generic_string();
			options.pinStamp = "procedural";
			options.buildServers = false;
			_editor->configureCompile(options);
			if (!_editor->requestCompile()) {
				return false;
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpCompile();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (_editor->getCompile().GetState() != CompileState::Succeeded) {
				return false;
			}
			outAssembly = fs::path(_editor->getCompileOutput());
			const str pathStr = outAssembly.generic_string();
			const SltString paths[] = {SltString{pathStr.data(), static_cast<int32_t>(pathStr.size())}};
			return _hooks->getManaged().loadUserAssemblies(paths, 1) == SLT_OK;
		}
	};

} // anonymous namespace

TEST_F(CSharpProceduralAssetsTest, TransientAssetsSpanUploadsAndComponentHelpers) {
	if (_hooks->findSDKRoot().empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	fs::path assembly;
	ASSERT_TRUE(CompileAndLoad(PROCEDURAL_SCRIPT, assembly)) << _log.Delivered().Joined();

	const TypeInfo* probeType = TypeRegistry::FindType(StringHash("ProceduralTests.ProceduralProbe"));
	ASSERT_NE(probeType, nullptr);

	SceneSystem::SetPlaying(true);
	_scene = SceneSystem::CreateScene("ProceduralWorld");
	Handle<SceneObject> object = SceneSystem::CreateObject("Actor", _scene);
	ASSERT_TRUE(object.IsValid());
	Handle<Component> component = object->AddComponent(probeType);
	ASSERT_TRUE(component.IsValid());
	SceneSystem::BeginFrame();
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();

	auto readInt = [probeType, &component](const char* name) -> i32 {
		Variant out;
		EXPECT_TRUE(GetField(component.Get(), probeType, StringHash(name), &out)) << name;
		return out.As<i32>();
	};
	auto readBool = [probeType, &component](const char* name) -> bool {
		Variant out;
		EXPECT_TRUE(GetField(component.Get(), probeType, StringHash(name), &out)) << name;
		return out.As<bool>();
	};

	EXPECT_EQ(readInt("vertexCount"), 3);
	EXPECT_EQ(readInt("indexCount"), 3);
	EXPECT_EQ(readInt("submeshCount"), 1);
	EXPECT_EQ(readInt("textureWidth"), 2);
	EXPECT_EQ(readInt("textureHeight"), 2);
	EXPECT_EQ(readInt("pixelByteCount"), 16);
	EXPECT_TRUE(readBool("meshDestroyedToNull"));
	EXPECT_TRUE(readBool("textureDestroyedToNull"));
	EXPECT_TRUE(readBool("materialDestroyedToNull"));
	EXPECT_FALSE(readBool("tryGetBeforeAdd"));
	EXPECT_FALSE(readBool("hasBeforeAdd"));
	EXPECT_TRUE(readBool("tryGetAfterAdd"));
	EXPECT_TRUE(readBool("hasAfterAdd"));
	EXPECT_EQ(readInt("markerCode"), 7);
}

#endif
