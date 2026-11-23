#include <gtest/gtest.h>

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/SpriteAsset.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/SceneAsset.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"
#include "Shalltear/Core/UI/UIPanelSettings.h"
#include "Shalltear/Core/UI/WidgetRegistry.h"

#include "Shalltear/Editor/CookOps.h"
#include "Shalltear/Editor/EditorSceneOps.h"

#include "Asset/PakReader.h"

#include "CookHarness.h"
#include "ProductLookup.h"
#include "TestImages.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>

using namespace Shalltear;

namespace {

	class ClProbeComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(ClProbeComponent, Component)
		SLT_REFLECT_FIELD(material)
		SLT_REFLECT_FIELD(other)
		SLT_END_REFLECT()

		Handle<MaterialAsset> material;
		Handle<SceneAsset> other;
	};
	SLT_REGISTER_TYPE(ClProbeComponent)

	struct ClDataBankSlot {
		SLT_REFLECT_VALUE(ClDataBankSlot)
		SLT_REFLECT_FIELD(nested)
		SLT_END_REFLECT()

		Handle<TextAsset> nested;
	};
	SLT_REGISTER_TYPE(ClDataBankSlot)

	class ClDataBank : public DataAsset {
	public:
		SLT_REFLECT_CLASS(ClDataBank)
		SLT_REFLECT_PARENT(DataAsset)
		SLT_REFLECT_CREATABLE()
		SLT_REFLECT_FIELD(primary)
		SLT_REFLECT_ARRAY(layers)
		SLT_REFLECT_SUBOBJECT(slot)
		SLT_END_REFLECT()

		Handle<TextAsset> primary;
		Vector<Handle<TextAsset>> layers{GetMemoryResource(MemTag::Asset)};
		ClDataBankSlot slot;
	};
	SLT_REGISTER_TYPE(ClDataBank)

	class ClDataBankImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return ClDataBank::GetReflectType()->name; }
		Asset* Import(const Path& sourcePath) override {
			ClDataBank* asset = New<ClDataBank>(MemTag::Asset);
			if (!asset->Import(sourcePath)) {
				Delete(asset);
				return nullptr;
			}
			return asset;
		}
	};

	class ClProductImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("TextAsset"); }
		Asset* Import(const Path& sourcePath) override {
			TextAsset* asset = New<TextAsset>(MemTag::Asset);
			asset->Import(sourcePath);
			return asset;
		}
		Asset* ImportWithSettings(const Path& sourcePath, const Path& virtualPath, const JSONValue& settings,
								  AssetImportContext& context) override {
			(void)virtualPath;
			(void)settings;
			TextAsset* product = New<TextAsset>(MemTag::Asset);
			product->content = "the geometry";
			context.AddProduct("Meshes/Body", product);
			return Import(sourcePath);
		}
	};

	constexpr std::string_view ClSpriteOnly[] = {"SpriteAsset"};
	constexpr std::string_view ClTextureOnly[] = {"TextureAsset"};
	constexpr UI::WidgetAttribute ClIconAttributes[] = {{"icon", UI::WidgetAttributeType::Asset, {}, ClSpriteOnly}};
	constexpr UI::WidgetAttribute ClTextureOnlyAttributes[] = {{"icon", UI::WidgetAttributeType::Asset, {}, ClTextureOnly}};
	const int CL_WIDGET_OWNER = 0;
	const Asset* _clIconReceived = nullptr;

	UI::WidgetResult MakeClIcon(UI::Context& context, UI::ElementHandle parent, const UI::WidgetArgs& args) {
		_clIconReceived = args.GetAsset("icon");
		UI::ElementHandle element = context.CreateElement();
		context.AppendChild(parent, element);
		return {element, {}};
	}

	class CookClosureFixture : public ::testing::Test {
	protected:
		void SetUp() override {
			static std::atomic<u64> counter{0};
			std::filesystem::path base = std::filesystem::temp_directory_path() /
										 ("shalltear_cookclosure_" + std::to_string(counter.fetch_add(1)));
			_root = base / "project";
			_engine = base / "engine";
			std::error_code ec;
			std::filesystem::remove_all(base, ec);
			std::filesystem::create_directories(_root / "Assets", ec);
			std::filesystem::create_directories(_engine, ec);

			CookHarness::WriteSyntheticEngineRoots(_engine);

			FileSystem::Mount("project", Path(_root.generic_string()));
			FileSystem::Mount("engine", Path(_engine.generic_string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			AssetImporterRegistry::RegisterExtension(".clprobe", &_productImporter);
		}

		void TearDown() override {
			UI::WidgetRegistry::UnregisterOwnedBy(&CL_WIDGET_OWNER);
			AssetImporterRegistry::Unregister(&_productImporter);
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root.parent_path(), ec);
		}

		void StageText(const char* relative, std::string_view text) const {
			std::filesystem::path at = _root / relative;
			std::error_code ec;
			std::filesystem::create_directories(at.parent_path(), ec);
			ASSERT_TRUE(FileSystem::WriteTextFile(Path(at.generic_string()), str(text)));
		}

		Guid GuidOf(const char* virtualPath) const { return AssetDatabase::LookupGuid(Path(virtualPath)); }

		void Rescan() const { AssetDatabase::ScanProjectDirectory(); }

		Path StageGemAtlas() const {
			std::vector<u8> pixels(16 * 8 * 4, 255);
			std::filesystem::create_directories(_root / "Assets" / "UI");
			TestImages::WriteTGA(_root / "Assets" / "UI" / "Atlas.tga", 16, 8, pixels.data());
			Rescan();
			const Path atlasPath("project://Assets/UI/Atlas.tga");
			JSONValue rect = JSONValue::MakeArray();
			for (f64 value : {8.0, 0.0, 8.0, 8.0}) {
				rect.Push(JSONValue::MakeNumber(value));
			}
			JSONValue sprite = JSONValue::MakeObject();
			sprite.AddMember("id", JSONValue::MakeString("gem"));
			sprite.AddMember("name", JSONValue::MakeString("Gem"));
			sprite.AddMember("rect", rect);
			JSONValue sprites = JSONValue::MakeArray();
			sprites.Push(sprite);
			JSONValue settings = JSONValue::MakeObject();
			settings.AddMember("compression", JSONValue::MakeString("none"));
			settings.AddMember("sprites", sprites);
			EXPECT_TRUE(AssetDatabase::WriteImporterSettings(atlasPath, settings));
			return atlasPath;
		}

		void MountCooked(const Vector<u8>& package, const char* name) const {
			std::filesystem::path file = _root / name;
			Buffer buffer(package.size(), MemTag::Editor);
			std::memcpy(buffer.data, package.data(), package.size());
			ASSERT_TRUE(FileSystem::WriteFile(Path(file.generic_string()), buffer));
			str error;
			ASSERT_TRUE(AssetDatabase::MountPackage(Path(file.generic_string()), error)) << error;
		}

		Vector<Guid> Roots(std::initializer_list<Guid> guids) const {
			Vector<Guid> out{GetMemoryResource(MemTag::Editor)};
			for (const Guid& guid : guids) {
				out.push_back(guid);
			}
			return out;
		}

		static bool Holds(const CookOps::CookClosure& closure, const Guid& guid) {
			return std::find(closure.assets.begin(), closure.assets.end(), guid) != closure.assets.end();
		}

		static const Vector<Guid>* EdgesOf(const CookOps::CookClosure& closure, const Guid& owner) {
			auto it = std::find(closure.assets.begin(), closure.assets.end(), owner);
			if (it == closure.assets.end()) {
				return nullptr;
			}
			return &closure.dependencies[static_cast<size_t>(it - closure.assets.begin())];
		}

		std::filesystem::path _root;
		std::filesystem::path _engine;
		ClProductImporter _productImporter;
	};

	str MaterialJSON(const Guid& shader, const Guid& texture) {
		return std::format(R"({{
	"shader": "{}",
	"floats": {{}},
	"colors": {{}},
	"textures": {{ "_BaseMap": "{}" }}
}})",
						   shader.ToString(), texture.ToString());
	}

} // namespace

TEST_F(CookClosureFixture, ASceneDocumentsReferenceIsAnEdge) {
	StageText("Assets/Leaf.txt", "leaf");
	Rescan();
	StageText("Assets/Mid.mat", MaterialJSON(Guid{}, Guid{}));
	Rescan();

	Guid materialGuid = GuidOf("project://Assets/Mid.mat");
	ASSERT_TRUE(materialGuid.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> root = SceneSystem::CreateObject("Root", scene);
	Asset* material = AssetDatabase::LoadAssetByGuid(materialGuid);
	ASSERT_NE(material, nullptr);
	root->AddComponent<ClProbeComponent>()->material = Handle<MaterialAsset>(static_cast<MaterialAsset*>(material));
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Level.scene"), false));
	AssetDatabase::UnloadAsset(material);
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	Rescan();

	Guid sceneGuid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(sceneGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({sceneGuid}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, sceneGuid));
	EXPECT_TRUE(Holds(closure, materialGuid));

	const Vector<Guid>* edges = EdgesOf(closure, sceneGuid);
	ASSERT_NE(edges, nullptr);
	EXPECT_NE(std::find(edges->begin(), edges->end(), materialGuid), edges->end());
}

TEST_F(CookClosureFixture, MaterialLoadsAreTransitiveEdges) {
	StageText("Assets/Shader.txt", "stands in for a shader");
	StageText("Assets/Tex.txt", "stands in for a texture");
	Rescan();
	Guid shaderish = GuidOf("project://Assets/Shader.txt");
	Guid textureish = GuidOf("project://Assets/Tex.txt");
	ASSERT_TRUE(shaderish.IsValid() && textureish.IsValid());

	StageText("Assets/Mid.mat", MaterialJSON(shaderish, textureish));
	Rescan();
	Guid materialGuid = GuidOf("project://Assets/Mid.mat");
	ASSERT_TRUE(materialGuid.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> root = SceneSystem::CreateObject("Root", scene);
	Asset* material = AssetDatabase::LoadAssetByGuid(materialGuid);
	ASSERT_NE(material, nullptr);
	root->AddComponent<ClProbeComponent>()->material = Handle<MaterialAsset>(static_cast<MaterialAsset*>(material));
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Level.scene"), false));
	AssetDatabase::UnloadAsset(material);
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Level.scene")}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, materialGuid));
	EXPECT_TRUE(Holds(closure, shaderish));
	EXPECT_TRUE(Holds(closure, textureish));

	const Vector<Guid>* edges = EdgesOf(closure, materialGuid);
	ASSERT_NE(edges, nullptr);
	EXPECT_NE(std::find(edges->begin(), edges->end(), shaderish), edges->end());
	EXPECT_NE(std::find(edges->begin(), edges->end(), textureish), edges->end());
}

TEST_F(CookClosureFixture, APrefabInstanceRowIsAnEdge) {
	Handle<Scene> authoring = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", authoring);
	SceneSystem::CreateObject("Body", authoring, unit);
	Guid prefabGuid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(prefabGuid.IsValid());
	SceneSystem::UnloadScene(authoring);
	SceneSystem::EndFrame();
	Rescan();

	Handle<Scene> level = SceneSystem::CreateScene("Level");
	ASSERT_TRUE(SceneSystem::Instantiate(prefabGuid, level).IsValid());
	ASSERT_TRUE(SceneSystem::SaveScene(level, Path("project://Assets/Level.scene"), false));
	SceneSystem::UnloadScene(level);
	SceneSystem::EndFrame();
	Rescan();

	Guid sceneGuid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(sceneGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({sceneGuid}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, prefabGuid));

	const Vector<Guid>* edges = EdgesOf(closure, sceneGuid);
	ASSERT_NE(edges, nullptr);
	EXPECT_NE(std::find(edges->begin(), edges->end(), prefabGuid), edges->end());
}

TEST_F(CookClosureFixture, WalkFollowsPrefabRows) {
	StageText("Assets/Shader.txt", "stands in for a shader");
	StageText("Assets/Tex.txt", "stands in for a texture");
	Rescan();
	Guid shaderish = GuidOf("project://Assets/Shader.txt");
	Guid textureish = GuidOf("project://Assets/Tex.txt");
	ASSERT_TRUE(shaderish.IsValid() && textureish.IsValid());

	StageText("Assets/Skin.mat", MaterialJSON(shaderish, textureish));
	Rescan();
	Guid materialGuid = GuidOf("project://Assets/Skin.mat");
	ASSERT_TRUE(materialGuid.IsValid());

	Handle<Scene> authoring = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", authoring);
	Asset* material = AssetDatabase::LoadAssetByGuid(materialGuid);
	ASSERT_NE(material, nullptr);
	unit->AddComponent<ClProbeComponent>()->material = Handle<MaterialAsset>(static_cast<MaterialAsset*>(material));
	Guid prefabGuid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(prefabGuid.IsValid());
	AssetDatabase::UnloadAsset(material);
	SceneSystem::UnloadScene(authoring);
	SceneSystem::EndFrame();
	Rescan();

	Handle<Scene> level = SceneSystem::CreateScene("Level");
	ASSERT_TRUE(SceneSystem::Instantiate(prefabGuid, level).IsValid());
	ASSERT_TRUE(SceneSystem::SaveScene(level, Path("project://Assets/Level.scene"), false));
	SceneSystem::UnloadScene(level);
	SceneSystem::EndFrame();
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Level.scene")}), closure, error)) << error;

	ASSERT_TRUE(Holds(closure, prefabGuid));
	EXPECT_TRUE(Holds(closure, materialGuid));
	EXPECT_TRUE(Holds(closure, shaderish));
	EXPECT_TRUE(Holds(closure, textureish));

	const Vector<Guid>* edges = EdgesOf(closure, prefabGuid);
	ASSERT_NE(edges, nullptr);
	EXPECT_NE(std::find(edges->begin(), edges->end(), materialGuid), edges->end());
}

TEST_F(CookClosureFixture, OverrideThatNamesAssetIsEdge) {
	StageText("Assets/Base.txt", "stands in for a shader");
	Rescan();
	Guid shaderish = GuidOf("project://Assets/Base.txt");
	StageText("Assets/Stock.mat", MaterialJSON(shaderish, Guid{}));
	StageText("Assets/Swapped.mat", MaterialJSON(shaderish, Guid{}));
	Rescan();
	Guid stockGuid = GuidOf("project://Assets/Stock.mat");
	Guid swappedGuid = GuidOf("project://Assets/Swapped.mat");
	ASSERT_TRUE(stockGuid.IsValid() && swappedGuid.IsValid());

	Handle<Scene> authoring = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", authoring);
	Asset* stock = AssetDatabase::LoadAssetByGuid(stockGuid);
	ASSERT_NE(stock, nullptr);
	unit->AddComponent<ClProbeComponent>()->material = Handle<MaterialAsset>(static_cast<MaterialAsset*>(stock));
	Guid prefabGuid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(prefabGuid.IsValid());
	AssetDatabase::UnloadAsset(stock);
	SceneSystem::UnloadScene(authoring);
	SceneSystem::EndFrame();
	Rescan();

	Handle<Scene> level = SceneSystem::CreateScene("Level");
	Handle<SceneObject> instance = SceneSystem::Instantiate(prefabGuid, level);
	ASSERT_TRUE(instance.IsValid());
	Handle<ClProbeComponent> probe = instance->GetComponent<ClProbeComponent>();
	ASSERT_TRUE(probe.IsValid());
	Asset* swapped = AssetDatabase::LoadAssetByGuid(swappedGuid);
	ASSERT_NE(swapped, nullptr);
	probe->material = Handle<MaterialAsset>(static_cast<MaterialAsset*>(swapped));
	ASSERT_TRUE(SceneSystem::SaveScene(level, Path("project://Assets/Level.scene"), false));
	AssetDatabase::UnloadAsset(swapped);
	SceneSystem::UnloadScene(level);
	SceneSystem::EndFrame();
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Level.scene")}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, swappedGuid));
}

TEST_F(CookClosureFixture, APrefabVariantsBaseIsAnEdge) {
	Handle<Scene> authoring = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", authoring);
	Guid baseGuid = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Base.prefab"));
	ASSERT_TRUE(baseGuid.IsValid());
	SceneSystem::UnloadScene(authoring);
	SceneSystem::EndFrame();

	StageText("Assets/Variant.prefab", std::format(R"({{
	"_type": "Prefab",
	"formatVersion": 1,
	"engineHash": "0",
	"name": "Variant",
	"nextLocalId": 1,
	"prefab": {{ "base": "{}", "overrides": {{}} }},
	"objects": []
}})",
												   baseGuid.ToString()));
	Rescan();
	Guid variantGuid = GuidOf("project://Assets/Variant.prefab");
	ASSERT_TRUE(variantGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({variantGuid}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, baseGuid));

	const Vector<Guid>* edges = EdgesOf(closure, variantGuid);
	ASSERT_NE(edges, nullptr);
	EXPECT_NE(std::find(edges->begin(), edges->end(), baseGuid), edges->end());
}

TEST_F(CookClosureFixture, EngineBuiltinsAreRoots) {
	StageText("Assets/Only.txt", "a project with no references at all");
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Only.txt")}), closure, error)) << error;

	ASSERT_FALSE(AssetDatabase::GetEngineBuiltinRoots().empty());
	for (const Guid& guid : AssetDatabase::GetEngineBuiltinRoots()) {
		EXPECT_TRUE(Holds(closure, guid));
	}
}

TEST_F(CookClosureFixture, UnreachableAssetIsNotInClosure) {
	StageText("Assets/Used.txt", "referenced");
	StageText("Assets/Orphan.txt", "referenced by nothing");
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Used.txt")}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, GuidOf("project://Assets/Used.txt")));
	EXPECT_FALSE(Holds(closure, GuidOf("project://Assets/Orphan.txt")));
}

TEST_F(CookClosureFixture, ACycleTerminates) {
	Handle<Scene> a = SceneSystem::CreateScene("A");
	SceneSystem::CreateObject("Root", a)->AddComponent<ClProbeComponent>();
	ASSERT_TRUE(SceneSystem::SaveScene(a, Path("project://Assets/A.scene"), false));
	Handle<Scene> b = SceneSystem::CreateScene("B");
	SceneSystem::CreateObject("Root", b)->AddComponent<ClProbeComponent>();
	ASSERT_TRUE(SceneSystem::SaveScene(b, Path("project://Assets/B.scene"), false));
	Rescan();

	Guid guidA = GuidOf("project://Assets/A.scene");
	Guid guidB = GuidOf("project://Assets/B.scene");
	ASSERT_TRUE(guidA.IsValid() && guidB.IsValid());

	Asset* assetA = AssetDatabase::LoadAssetByGuid(guidA);
	Asset* assetB = AssetDatabase::LoadAssetByGuid(guidB);
	ASSERT_NE(assetA, nullptr);
	ASSERT_NE(assetB, nullptr);
	a->GetRoot(0)->GetComponent<ClProbeComponent>()->other = Handle<SceneAsset>(static_cast<SceneAsset*>(assetB));
	b->GetRoot(0)->GetComponent<ClProbeComponent>()->other = Handle<SceneAsset>(static_cast<SceneAsset*>(assetA));
	ASSERT_TRUE(SceneSystem::SaveScene(a, Path("project://Assets/A.scene"), false));
	ASSERT_TRUE(SceneSystem::SaveScene(b, Path("project://Assets/B.scene"), false));
	AssetDatabase::UnloadAsset(assetA);
	AssetDatabase::UnloadAsset(assetB);
	SceneSystem::UnloadScene(a);
	SceneSystem::UnloadScene(b);
	SceneSystem::EndFrame();
	AssetDatabase::CollectGarbage();
	Rescan();

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({guidA}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, guidA));
	EXPECT_TRUE(Holds(closure, guidB));
}

TEST_F(CookClosureFixture, DanglingReferenceFailsBuild) {
	StageText("Assets/Tex.txt", "about to be deleted");
	Rescan();
	Guid doomed = GuidOf("project://Assets/Tex.txt");
	ASSERT_TRUE(doomed.IsValid());

	StageText("Assets/Mid.mat", MaterialJSON(Guid{}, doomed));
	Rescan();
	Guid materialGuid = GuidOf("project://Assets/Mid.mat");

	std::error_code ec;
	std::filesystem::remove(_root / "Assets" / "Tex.txt", ec);
	std::filesystem::remove(_root / "Assets" / "Tex.txt.meta", ec);
	AssetDatabase::RebuildDatabase();

	CookOps::CookClosure closure;
	str error;
	EXPECT_FALSE(CookOps::ComputeClosure(Roots({materialGuid}), closure, error));
	EXPECT_NE(error.find(doomed.ToString()), str::npos) << error;
	EXPECT_NE(error.find("Mid.mat"), str::npos) << error;
}

TEST_F(CookClosureFixture, EditorOnlyReferenceFailsBuild) {
	Guid editorOnly = GuidOf("engine://Templates/Scaffold.txt");
	ASSERT_TRUE(editorOnly.IsValid());

	StageText("Assets/Mid.mat", MaterialJSON(editorOnly, Guid{}));
	Rescan();

	CookOps::CookClosure closure;
	str error;
	EXPECT_FALSE(CookOps::ComputeClosure(Roots({GuidOf("project://Assets/Mid.mat")}), closure, error));
	EXPECT_NE(error.find("Scaffold"), str::npos) << error;
	EXPECT_NE(error.find("Mid.mat"), str::npos) << error;
}

TEST_F(CookClosureFixture, EdgesReachCookedPackagesDepsSection) {
	StageText("Assets/Tex.txt", "a leaf");
	Rescan();
	StageText("Assets/Mid.mat", MaterialJSON(Guid{}, GuidOf("project://Assets/Tex.txt")));
	Rescan();

	Guid materialGuid = GuidOf("project://Assets/Mid.mat");
	Guid leafGuid = GuidOf("project://Assets/Tex.txt");

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({materialGuid}), closure, error)) << error;

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;

	std::filesystem::path file = _root / "Content.slpak";
	Buffer buffer(package.size(), MemTag::Editor);
	std::memcpy(buffer.data, package.data(), package.size());
	ASSERT_TRUE(FileSystem::WriteFile(Path(file.generic_string()), buffer));

	PakReader reader;
	ASSERT_TRUE(reader.Mount(Path(file.generic_string()), error)) << error;

	u32 index = reader.FindAsset(materialGuid);
	u32 leafIndex = reader.FindAsset(leafGuid);
	ASSERT_NE(index, PAK_INVALID_INDEX);
	ASSERT_NE(leafIndex, PAK_INVALID_INDEX);

	const PakDepEdge* edges = nullptr;
	u32 count = 0;
	ASSERT_TRUE(reader.GetDependencies(index, edges, count));
	bool found = false;
	for (u32 i = 0; i < count; ++i) {
		found = found || edges[i].targetAsset == leafIndex;
	}
	EXPECT_TRUE(found);
}

TEST_F(CookClosureFixture, ProductReferenceCooksContainer) {
	StageText("Assets/rig.clprobe", "a model standing in for one");
	Rescan();
	Guid sourceGuid = GuidOf("project://Assets/rig.clprobe");
	ASSERT_TRUE(sourceGuid.IsValid());

	Asset* source = AssetDatabase::LoadAsset(Path("project://Assets/rig.clprobe"));
	ASSERT_NE(source, nullptr);
	AssetDatabase::UnloadAsset(source);
	AssetDatabase::CollectGarbage();

	Guid productGuid = ProductLookup::Find(Path("project://Assets/rig.clprobe"), "Meshes/Body");
	ASSERT_TRUE(productGuid.IsValid());
	EXPECT_TRUE(AssetDatabase::LookupPath(productGuid).ToString().empty());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({productGuid}), closure, error)) << error;
	EXPECT_TRUE(Holds(closure, productGuid));
	EXPECT_TRUE(Holds(closure, sourceGuid));

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;

	std::filesystem::path file = _root / "Products.slpak";
	Buffer buffer(package.size(), MemTag::Editor);
	std::memcpy(buffer.data, package.data(), package.size());
	ASSERT_TRUE(FileSystem::WriteFile(Path(file.generic_string()), buffer));

	PakReader reader;
	ASSERT_TRUE(reader.Mount(Path(file.generic_string()), error)) << error;
	EXPECT_NE(reader.FindAsset(productGuid), PAK_INVALID_INDEX);
}

TEST_F(CookClosureFixture, ShaderShipsRequestedVariants) {
	StageText("Assets/Keywords.hlsl",
			  "#pragma shalltear_keyword FEATURE_A\n"
			  "#pragma shalltear_keyword FEATURE_B\n"
			  "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
			  "struct VSOutput { float4 positionCS : SV_Position; };\n"
			  "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
			  "\tVSOutput output;\n"
			  "\toutput.positionCS = float4(float(vertexId), 0, 0, 1);\n"
			  "\treturn output;\n"
			  "}\n"
			  "float4 PSMain(VSOutput input) : SV_Target {\n"
			  "\tfloat3 tint = float3(1, 0, 0);\n"
			  "#ifdef FEATURE_A\n"
			  "\ttint = float3(0, 1, 0);\n"
			  "#endif\n"
			  "#ifdef FEATURE_B\n"
			  "\ttint += float3(0, 0, 1);\n"
			  "#endif\n"
			  "\treturn float4(tint * baseColor.rgb, 1);\n"
			  "}\n");
	Rescan();

	Guid shaderGuid = GuidOf("project://Assets/Keywords.hlsl");
	ASSERT_TRUE(shaderGuid.IsValid());
	{
		Asset* probe = AssetDatabase::LoadAssetByGuid(shaderGuid);
		ASSERT_NE(nullptr, probe);
		bool compiled = !static_cast<ShaderAsset*>(probe)->variants.empty();
		AssetDatabase::UnloadAsset(probe);
		if (!compiled) {
			GTEST_SKIP() << "dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
		}
	}

	StageText("Assets/Featured.mat",
			  std::format("{{\n\t\"shader\": \"{}\",\n\t\"floats\": {{}},\n\t\"colors\": {{}},\n"
						  "\t\"textures\": {{}},\n\t\"keywords\": [ \"FEATURE_A\" ]\n}}\n",
						  shaderGuid.ToString()));
	Rescan();
	Guid materialGuid = GuidOf("project://Assets/Featured.mat");
	ASSERT_TRUE(materialGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({materialGuid}), closure, error)) << error;

	ASSERT_EQ(1u, closure.variantRequests.size());
	EXPECT_EQ(shaderGuid, closure.variantRequests[0].shader);
	ASSERT_EQ(1u, closure.variantRequests[0].keywords.size());
	EXPECT_EQ("FEATURE_A", closure.variantRequests[0].keywords[0]);

	AssetDatabase::CollectGarbage();

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;

	std::filesystem::path file = _root / "Variants.slpak";
	Buffer buffer(package.size(), MemTag::Editor);
	std::memcpy(buffer.data, package.data(), package.size());
	ASSERT_TRUE(FileSystem::WriteFile(Path(file.generic_string()), buffer));

	PakReader reader;
	ASSERT_TRUE(reader.Mount(Path(file.generic_string()), error)) << error;
	u32 index = reader.FindAsset(shaderGuid);
	ASSERT_NE(PAK_INVALID_INDEX, index);

	const PakAssetEntry* entry = reader.GetAsset(index);
	ASSERT_NE(nullptr, entry);
	ASSERT_NE(PAK_NO_REGION, entry->regionIndex);
	Buffer payloadBytes;
	ASSERT_TRUE(reader.ReadRegion(entry->regionIndex, payloadBytes, error)) << error;
	CookReader descriptor = reader.GetDescriptor(index);
	CookReader payload(payloadBytes.data, payloadBytes.size);
	ShaderAsset shipped;
	ASSERT_TRUE(shipped.LoadCooked(descriptor, payload));

	ASSERT_EQ(2u, shipped.variants.size());
	EXPECT_EQ(0u, shipped.variants[SHADER_BASE_VARIANT].keywordHash);
	Vector<str> used{GetMemoryResource(MemTag::Asset)};
	used.push_back(str("FEATURE_A"));
	EXPECT_EQ(1u, shipped.FindVariant(ShaderAsset::KeywordSetHash({used.data(), used.size()})));
	EXPECT_TRUE(shipped.variants[SHADER_BASE_VARIANT].vertexSpirv.size() > 0);
	EXPECT_TRUE(shipped.variants[1].vertexSpirv.size() > 0);
	EXPECT_NE(shipped.variants[SHADER_BASE_VARIANT].pixelSpirv, shipped.variants[1].pixelSpirv);

	ASSERT_EQ(2u, shipped.keywords.size());
	EXPECT_EQ("FEATURE_A", shipped.keywords[0]);
	EXPECT_EQ("FEATURE_B", shipped.keywords[1]);
}

TEST_F(CookClosureFixture, ClippedMaterialCooksImplicitVariant) {
	StageText("Assets/Clip.hlsl",
			  "#pragma shalltear_keyword SHALLTEAR_ALPHA_CLIP\n"
			  "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
			  "struct VSOutput { float4 positionCS : SV_Position; };\n"
			  "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
			  "\tVSOutput output; output.positionCS = float4(float(vertexId), 0, 0, 1); return output;\n"
			  "}\n"
			  "float4 PSMain(VSOutput input) : SV_Target {\n"
			  "\tfloat alpha = baseColor.a;\n"
			  "#ifdef SHALLTEAR_ALPHA_CLIP\n"
			  "\tclip(alpha - 0.5);\n"
			  "#endif\n"
			  "\treturn float4(baseColor.rgb, alpha);\n"
			  "}\n");
	Rescan();

	Guid shaderGuid = GuidOf("project://Assets/Clip.hlsl");
	ASSERT_TRUE(shaderGuid.IsValid());
	{
		Asset* probe = AssetDatabase::LoadAssetByGuid(shaderGuid);
		ASSERT_NE(nullptr, probe);
		bool compiled = !static_cast<ShaderAsset*>(probe)->variants.empty();
		AssetDatabase::UnloadAsset(probe);
		if (!compiled) {
			GTEST_SKIP() << "dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
		}
	}

	StageText("Assets/Cutout.mat",
			  std::format("{{\n\t\"shader\": \"{}\",\n\t\"floats\": {{}},\n\t\"colors\": {{}},\n"
						  "\t\"textures\": {{}},\n\t\"alphaClip\": 0.5\n}}\n",
						  shaderGuid.ToString()));
	Rescan();
	Guid materialGuid = GuidOf("project://Assets/Cutout.mat");
	ASSERT_TRUE(materialGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({materialGuid}), closure, error)) << error;

	ASSERT_EQ(1u, closure.variantRequests.size());
	EXPECT_EQ(shaderGuid, closure.variantRequests[0].shader);
	ASSERT_EQ(1u, closure.variantRequests[0].keywords.size());
	EXPECT_EQ(ShaderAlphaClipKeyword, closure.variantRequests[0].keywords[0]);

	AssetDatabase::CollectGarbage();

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;

	std::filesystem::path file = _root / "Cutout.slpak";
	Buffer buffer(package.size(), MemTag::Editor);
	std::memcpy(buffer.data, package.data(), package.size());
	ASSERT_TRUE(FileSystem::WriteFile(Path(file.generic_string()), buffer));

	PakReader reader;
	ASSERT_TRUE(reader.Mount(Path(file.generic_string()), error)) << error;
	u32 index = reader.FindAsset(shaderGuid);
	ASSERT_NE(PAK_INVALID_INDEX, index);
	const PakAssetEntry* entry = reader.GetAsset(index);
	ASSERT_NE(nullptr, entry);
	ASSERT_NE(PAK_NO_REGION, entry->regionIndex);
	Buffer payloadBytes;
	ASSERT_TRUE(reader.ReadRegion(entry->regionIndex, payloadBytes, error)) << error;
	CookReader descriptor = reader.GetDescriptor(index);
	CookReader payload(payloadBytes.data, payloadBytes.size);
	ShaderAsset shipped;
	ASSERT_TRUE(shipped.LoadCooked(descriptor, payload));

	u32 materialIndex = reader.FindAsset(materialGuid);
	ASSERT_NE(PAK_INVALID_INDEX, materialIndex);
	CookReader materialDescriptor = reader.GetDescriptor(materialIndex);
	CookReader emptyPayload;
	MaterialAsset shippedMaterial;
	ASSERT_TRUE(shippedMaterial.LoadCooked(materialDescriptor, emptyPayload));
	EXPECT_FLOAT_EQ(shippedMaterial.alphaClipThreshold, 0.5f);
	EXPECT_NE(shipped.FindVariant(shippedMaterial.keywordHash), SHADER_BASE_VARIANT);
}

TEST_F(CookClosureFixture, RendererPullsUIDependencies) {
	StageText("Assets/UI/Hud.slss", ".hud { width: 240px; }");
	StageText("Assets/UI/Game.ttf", "stands in for a face");
	Rescan();
	StageText("Assets/UI/Hud.slxml",
			  R"(<ui version="1"><sheet src="project://Assets/UI/Hud.slss"/><panel id="hud" class="hud"/></ui>)");
	Guid fontGuid = GuidOf("project://Assets/UI/Game.ttf");
	ASSERT_TRUE(fontGuid.IsValid());
	StageText("Assets/UI/Hud.slpanel",
			  std::format(R"({{"font": {{"_handle": true, "pointeeType": "FontAsset", "assetId": "{}"}}}})",
						  fontGuid.ToString()));
	Rescan();
	Guid sheetGuid = GuidOf("project://Assets/UI/Hud.slss");
	Guid documentGuid = GuidOf("project://Assets/UI/Hud.slxml");
	Guid settingsGuid = GuidOf("project://Assets/UI/Hud.slpanel");
	ASSERT_TRUE(sheetGuid.IsValid());
	ASSERT_TRUE(documentGuid.IsValid());
	ASSERT_TRUE(settingsGuid.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Asset* document = AssetDatabase::LoadAssetByGuid(documentGuid);
	Asset* settings = AssetDatabase::LoadAssetByGuid(settingsGuid);
	ASSERT_NE(document, nullptr);
	ASSERT_NE(settings, nullptr);
	Handle<UIPanelRenderer> renderer = SceneSystem::CreateObject("HUD", scene)->AddComponent<UIPanelRenderer>();
	ASSERT_TRUE(renderer.IsValid());
	renderer->SetDocument(Handle<UIDocumentAsset>(static_cast<UIDocumentAsset*>(document)));
	renderer->SetPanelSettings(Handle<UIPanelSettings>(static_cast<UIPanelSettings*>(settings)));
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Level.scene"), false));
	AssetDatabase::UnloadAsset(document);
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	Rescan();

	Guid sceneGuid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(sceneGuid.IsValid());
	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({sceneGuid}), closure, error)) << error;

	EXPECT_TRUE(Holds(closure, documentGuid));
	EXPECT_TRUE(Holds(closure, sheetGuid));
	EXPECT_TRUE(Holds(closure, settingsGuid));
	EXPECT_TRUE(Holds(closure, fontGuid));

	const Vector<Guid>* sceneEdges = EdgesOf(closure, sceneGuid);
	ASSERT_NE(sceneEdges, nullptr);
	EXPECT_NE(std::find(sceneEdges->begin(), sceneEdges->end(), documentGuid), sceneEdges->end());
	EXPECT_NE(std::find(sceneEdges->begin(), sceneEdges->end(), settingsGuid), sceneEdges->end());
	const Vector<Guid>* documentEdges = EdgesOf(closure, documentGuid);
	ASSERT_NE(documentEdges, nullptr);
	EXPECT_NE(std::find(documentEdges->begin(), documentEdges->end(), sheetGuid), documentEdges->end());
	const Vector<Guid>* settingsEdges = EdgesOf(closure, settingsGuid);
	ASSERT_NE(settingsEdges, nullptr);
	EXPECT_NE(std::find(settingsEdges->begin(), settingsEdges->end(), fontGuid), settingsEdges->end());

	AssetDatabase::UnloadAsset(settings);
}

TEST_F(CookClosureFixture, ImageSpriteCooksItsTexture) {
	const Path atlasPath = StageGemAtlas();
	StageText("Assets/UI/Hud.slxml", R"(<ui version="1"><panel><image src="project://Assets/UI/Atlas.tga#Gem"/></panel></ui>)");
	Rescan();

	const Guid documentGuid = GuidOf("project://Assets/UI/Hud.slxml");
	const Guid atlasGuid = GuidOf("project://Assets/UI/Atlas.tga");
	Asset* document = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(document, nullptr);
	const Guid spriteGuid = ProductLookup::Find(atlasPath, "Sprites/gem");
	ASSERT_TRUE(spriteGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({documentGuid}), closure, error)) << error;
	EXPECT_TRUE(Holds(closure, spriteGuid));
	EXPECT_TRUE(Holds(closure, atlasGuid));
	const Vector<Guid>* documentEdges = EdgesOf(closure, documentGuid);
	ASSERT_NE(documentEdges, nullptr);
	EXPECT_NE(std::find(documentEdges->begin(), documentEdges->end(), spriteGuid), documentEdges->end());
	const Vector<Guid>* spriteEdges = EdgesOf(closure, spriteGuid);
	ASSERT_NE(spriteEdges, nullptr);
	EXPECT_NE(std::find(spriteEdges->begin(), spriteEdges->end(), atlasGuid), spriteEdges->end());

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;
	AssetDatabase::UnloadAsset(document);
	for (u32 pass = 0; pass < 4 && AssetDatabase::CollectGarbage() > 0; ++pass) {
	}
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(atlasGuid), nullptr);
	MountCooked(package, "Hud.slpak");

	Asset* cooked = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(cooked, nullptr);
	const UIDocumentAsset& restored = *static_cast<UIDocumentAsset*>(cooked);
	ASSERT_EQ(restored.assets.size(), 1u);
	const Asset* bound = restored.assets[0].asset.Get();
	ASSERT_NE(bound, nullptr);
	ASSERT_EQ(bound->GetAssetTypeId(), SpriteAsset::GetReflectType()->name);
	const TextureSprite* definition = static_cast<const SpriteAsset*>(bound)->GetDefinition();
	ASSERT_NE(definition, nullptr);
	EXPECT_EQ(definition->rect, RectI(8, 0, 8, 8));
	AssetDatabase::UnloadAsset(cooked);
}

TEST_F(CookClosureFixture, CustomWidgetSpriteReachesPackage) {
	ASSERT_TRUE(UI::WidgetRegistry::Register(UI::WidgetDesc{"item-icon", &MakeClIcon, ClIconAttributes}, &CL_WIDGET_OWNER));
	const Path atlasPath = StageGemAtlas();
	StageText("Assets/UI/Hud.slxml", R"(<ui version="1"><panel><item-icon icon="project://Assets/UI/Atlas.tga#Gem"/></panel></ui>)");
	Rescan();

	const Guid documentGuid = GuidOf("project://Assets/UI/Hud.slxml");
	const Guid atlasGuid = GuidOf("project://Assets/UI/Atlas.tga");
	Asset* document = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(document, nullptr);
	const Guid spriteGuid = ProductLookup::Find(atlasPath, "Sprites/gem");
	ASSERT_TRUE(spriteGuid.IsValid());

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({documentGuid}), closure, error)) << error;
	EXPECT_TRUE(Holds(closure, spriteGuid));
	EXPECT_TRUE(Holds(closure, atlasGuid));
	const Vector<Guid>* documentEdges = EdgesOf(closure, documentGuid);
	ASSERT_NE(documentEdges, nullptr);
	EXPECT_NE(std::find(documentEdges->begin(), documentEdges->end(), spriteGuid), documentEdges->end());

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;
	AssetDatabase::UnloadAsset(document);
	for (u32 pass = 0; pass < 4 && AssetDatabase::CollectGarbage() > 0; ++pass) {
	}
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(spriteGuid), nullptr);
	MountCooked(package, "Hud.slpak");

	Asset* cooked = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(cooked, nullptr);
	UI::ContextDesc contextDesc;
	contextDesc.sizePx = {64, 64};
	UI::Context ctx(contextDesc, nullptr, nullptr);
	_clIconReceived = nullptr;
	static_cast<UIDocumentAsset*>(cooked)->Instantiate(ctx, ctx.GetRoot());
	ASSERT_NE(_clIconReceived, nullptr);
	EXPECT_EQ(_clIconReceived->assetId, spriteGuid);
	ASSERT_EQ(_clIconReceived->GetAssetTypeId(), SpriteAsset::GetReflectType()->name);
	const TextureSprite* definition = static_cast<const SpriteAsset*>(_clIconReceived)->GetDefinition();
	ASSERT_NE(definition, nullptr);
	EXPECT_EQ(definition->rect, RectI(8, 0, 8, 8));
	AssetDatabase::UnloadAsset(cooked);
}

TEST_F(CookClosureFixture, CookedBindChecksEveryUse) {
	ASSERT_TRUE(UI::WidgetRegistry::Register(UI::WidgetDesc{"item-icon", &MakeClIcon, ClIconAttributes}, &CL_WIDGET_OWNER));
	StageGemAtlas();
	StageText("Assets/UI/Hud.slxml", R"(<ui version="1"><panel><image src="project://Assets/UI/Atlas.tga#Gem"/>)"
									 R"(<item-icon icon="project://Assets/UI/Atlas.tga#Gem"/></panel></ui>)");
	Rescan();

	const Guid documentGuid = GuidOf("project://Assets/UI/Hud.slxml");
	Asset* document = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(document, nullptr);
	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({documentGuid}), closure, error)) << error;
	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;
	AssetDatabase::UnloadAsset(document);
	for (u32 pass = 0; pass < 4 && AssetDatabase::CollectGarbage() > 0; ++pass) {
	}
	MountCooked(package, "Hud.slpak");

	Asset* cooked = AssetDatabase::LoadAssetByGuid(documentGuid);
	ASSERT_NE(cooked, nullptr);
	EXPECT_EQ(static_cast<UIDocumentAsset*>(cooked)->assets.size(), 1u);
	AssetDatabase::UnloadAsset(cooked);
	for (u32 pass = 0; pass < 4 && AssetDatabase::CollectGarbage() > 0; ++pass) {
	}
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(documentGuid), nullptr);

	ASSERT_TRUE(UI::WidgetRegistry::Register(UI::WidgetDesc{"item-icon", &MakeClIcon, ClTextureOnlyAttributes}, &CL_WIDGET_OWNER));
	EXPECT_EQ(AssetDatabase::LoadAssetByGuid(documentGuid), nullptr);
}

TEST_F(CookClosureFixture, ResidentDataAssetCooksAllReflectedHandles) {
	struct ScopedImporter {
		ClDataBankImporter importer;
		ScopedImporter() { AssetImporterRegistry::RegisterExtension(".clbank", &importer); }
		~ScopedImporter() { AssetImporterRegistry::Unregister(&importer); }
	} scopedImporter;

	StageText("Assets/Primary.txt", "primary-clip");
	StageText("Assets/LayerA.txt", "layer-a");
	StageText("Assets/LayerB.txt", "layer-b");
	StageText("Assets/Nested.txt", "nested-clip");
	Rescan();

	const Guid primaryGuid = GuidOf("project://Assets/Primary.txt");
	const Guid layerAGuid = GuidOf("project://Assets/LayerA.txt");
	const Guid layerBGuid = GuidOf("project://Assets/LayerB.txt");
	const Guid nestedGuid = GuidOf("project://Assets/Nested.txt");
	ASSERT_TRUE(primaryGuid.IsValid());
	ASSERT_TRUE(layerAGuid.IsValid());
	ASSERT_TRUE(layerBGuid.IsValid());
	ASSERT_TRUE(nestedGuid.IsValid());

	const str bankJSON = std::format(R"({{
	"_type": "ClDataBank",
	"primary": {{ "_handle": true, "pointeeType": "TextAsset", "assetId": "{}" }},
	"layers": [
		{{ "_handle": true, "pointeeType": "TextAsset", "assetId": "{}" }},
		{{ "_handle": true, "pointeeType": "TextAsset", "assetId": "{}" }}
	],
	"slot": {{
		"nested": {{ "_handle": true, "pointeeType": "TextAsset", "assetId": "{}" }}
	}}
}})",
									 primaryGuid.ToString(), layerAGuid.ToString(), layerBGuid.ToString(),
									 nestedGuid.ToString());
	StageText("Assets/Bank.clbank", bankJSON);
	Rescan();

	const Guid bankGuid = GuidOf("project://Assets/Bank.clbank");
	ASSERT_TRUE(bankGuid.IsValid());

	// keep resident before ComputeClosure to mirror an open editor scene
	Asset* resident = AssetDatabase::LoadAssetByGuid(bankGuid);
	ASSERT_NE(resident, nullptr);

	CookOps::CookClosure closure;
	str error;
	ASSERT_TRUE(CookOps::ComputeClosure(Roots({bankGuid}), closure, error)) << error;
	EXPECT_TRUE(Holds(closure, primaryGuid));
	EXPECT_TRUE(Holds(closure, layerAGuid));
	EXPECT_TRUE(Holds(closure, layerBGuid));
	EXPECT_TRUE(Holds(closure, nestedGuid));

	const Vector<Guid>* bankEdges = EdgesOf(closure, bankGuid);
	ASSERT_NE(bankEdges, nullptr);
	for (const Guid& expected : {primaryGuid, layerAGuid, layerBGuid, nestedGuid}) {
		EXPECT_NE(std::find(bankEdges->begin(), bankEdges->end(), expected), bankEdges->end());
	}

	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	CookOps::PakWriteDesc desc;
	ASSERT_TRUE(CookOps::CookPackage(closure, desc, package, error)) << error;

	AssetDatabase::UnloadAsset(resident);
	for (u32 pass = 0; pass < 4 && AssetDatabase::CollectGarbage() > 0; ++pass) {
	}
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(bankGuid), nullptr);
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(primaryGuid), nullptr);
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(layerAGuid), nullptr);
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(layerBGuid), nullptr);
	ASSERT_EQ(AssetDatabase::FindLoadedByGuid(nestedGuid), nullptr);

	MountCooked(package, "Bank.slpak");

	Asset* cooked = AssetDatabase::LoadAssetByGuid(bankGuid);
	ASSERT_NE(cooked, nullptr);
	const ClDataBank& restored = *static_cast<const ClDataBank*>(cooked);
	ASSERT_TRUE(restored.primary.IsValid());
	EXPECT_EQ(restored.primary->content, "primary-clip");
	ASSERT_EQ(restored.layers.size(), 2u);
	ASSERT_TRUE(restored.layers[0].IsValid());
	ASSERT_TRUE(restored.layers[1].IsValid());
	EXPECT_EQ(restored.layers[0]->content, "layer-a");
	EXPECT_EQ(restored.layers[1]->content, "layer-b");
	ASSERT_TRUE(restored.slot.nested.IsValid());
	EXPECT_EQ(restored.slot.nested->content, "nested-clip");

	AssetDatabase::UnloadAsset(cooked);
}
