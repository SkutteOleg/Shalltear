#include "TestsPCH.h"

#include "ProductLookup.h"

#include "FBXParser.h"
#include "ModelImport.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/SkeletonAsset.h"
#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Graphics/SkinAsset.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"

#include <algorithm>
#include <filesystem>

using namespace Shalltear;

namespace {

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

	std::filesystem::path CorpusDir() {
		return RepoDir("External/Submodules/ufbx/data");
	}

	Path CorpusFile(const char* name) {
		return Path((CorpusDir() / name).generic_string());
	}

	Mat4 WorldMatrix(const ModelParseResult& parsed, size_t node) {
		Mat4 world = parsed.nodes[node].localMatrix;
		for (i32 parent = parsed.nodes[node].parent; parent >= 0; parent = parsed.nodes[static_cast<size_t>(parent)].parent) {
			world = parsed.nodes[static_cast<size_t>(parent)].localMatrix * world;
		}
		return world;
	}

	Vec3 WorldSize(const ModelParseResult& parsed) {
		AABB bounds = AABB::Empty();
		for (size_t i = 0; i < parsed.nodes.size(); ++i) {
			if (parsed.nodes[i].mesh < 0) {
				continue;
			}
			const ModelMesh& mesh = parsed.meshes[static_cast<size_t>(parsed.nodes[i].mesh)];
			Mat4 world = WorldMatrix(parsed, i);
			for (const MeshVertex& vertex : mesh.vertices) {
				Vec4 transformed = world * Vec4(vertex.position, 1.0f);
				bounds.Encapsulate(Vec3(transformed.x, transformed.y, transformed.z));
			}
		}
		return bounds.Size();
	}

	bool TryNarrow(const std::filesystem::path& path, str& out) {
		try {
			out = path.generic_string();
			return true;
		} catch (const std::exception&) {
			return false;
		}
	}

	size_t CountNodesNamed(const ModelParseResult& parsed, std::string_view name) {
		size_t count = 0;
		for (const ModelNode& node : parsed.nodes) {
			if (node.name == name) {
				++count;
			}
		}
		return count;
	}

	class FBXImportFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_fbx_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));

			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			}
			AssetDatabase::Initialize();
		}

		void TearDown() override {
			AssetDatabase::Terminate();
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		bool StageCorpusFile(const char* name) {
			std::filesystem::path corpus = CorpusDir();
			if (corpus.empty()) {
				return false;
			}
			std::error_code ec;
			std::filesystem::copy_file(corpus / name, _root / name,
									   std::filesystem::copy_options::overwrite_existing, ec);
			return !ec;
		}

		Guid ImportModel(const char* virtualPath) {
			Asset* model = AssetDatabase::LoadAsset(Path(virtualPath));
			if (!model) {
				return {};
			}
			Guid guid = model->assetId;
			AssetDatabase::UnloadAsset(model);
			AssetDatabase::CollectGarbage();
			return guid;
		}
	};

} // namespace

TEST(FBXCorpusTest, CorpusIsCheckedOutBesideParser) {
	std::filesystem::path corpus = CorpusDir();
	ASSERT_FALSE(corpus.empty());

	std::error_code ec;
	size_t files = 0;
	for (const auto& entry : std::filesystem::directory_iterator(corpus, ec)) {
		if (entry.path().extension() == ".fbx") {
			++files;
		}
	}
	EXPECT_GE(files, 500u) << "corpus at " << corpus.generic_string();
}

TEST(FBXCorpusTest, CorpusParsesOrFailsWithReason) {
	std::filesystem::path corpus = CorpusDir();
	ASSERT_FALSE(corpus.empty());

	std::error_code ec;
	size_t parsed = 0;
	size_t refused = 0;
	size_t refusedAsEmpty = 0;
	size_t refusedForBadFile = 0;
	size_t unnameable = 0;
	Vector<str> badFiles{GetMemoryResource(MemTag::Asset)};
	str firstContradiction;
	for (const auto& entry : std::filesystem::directory_iterator(corpus, ec)) {
		if (entry.path().extension() != ".fbx") {
			continue;
		}
		str physical;
		str name;
		if (!TryNarrow(entry.path(), physical) || !TryNarrow(entry.path().filename(), name)) {
			++unnameable;
			continue;
		}
		ModelParseResult result = ParseFBX(Path(physical));
		if (!result.success) {
			++refused;
			EXPECT_FALSE(result.error.empty()) << name;
			if (result.error == "the file has no nodes/geometry") {
				++refusedAsEmpty;
			} else {
				++refusedForBadFile;
				if (badFiles.size() < 12) {
					badFiles.push_back(std::format("{}: {}", name, result.error));
				}
			}
			continue;
		}
		++parsed;

		for (size_t n = 0; n < result.nodes.size() && firstContradiction.empty(); ++n) {
			const ModelNode& node = result.nodes[n];
			if (node.parent >= static_cast<i32>(n)) {
				firstContradiction = std::format("{}: node {} precedes its parent {}", name, n, node.parent);
			} else if (node.mesh >= static_cast<i32>(result.meshes.size())) {
				firstContradiction = std::format("{}: node {} names mesh {} of {}", name, n, node.mesh, result.meshes.size());
			}
		}
		for (const ModelMesh& mesh : result.meshes) {
			if (!firstContradiction.empty()) {
				break;
			}
			if (mesh.indices.size() % 3 != 0) {
				firstContradiction = std::format("{}: mesh '{}' has {} indices, not a whole number of triangles", name, mesh.name, mesh.indices.size());
				break;
			}
			for (const SubMesh& submesh : mesh.submeshes) {
				if (static_cast<size_t>(submesh.firstIndex) + submesh.indexCount > mesh.indices.size()) {
					firstContradiction = std::format("{}: mesh '{}' submesh overruns its index buffer", name, mesh.name);
					break;
				}
				if (submesh.materialSlot >= mesh.materials.size()) {
					firstContradiction = std::format("{}: mesh '{}' submesh names slot {} of {}", name, mesh.name, submesh.materialSlot, mesh.materials.size());
					break;
				}
			}
			for (u32 index : mesh.indices) {
				if (index >= mesh.vertices.size()) {
					firstContradiction = std::format("{}: mesh '{}' index {} of {} vertices", name, mesh.name, index, mesh.vertices.size());
					break;
				}
			}
		}
		for (const ModelMaterial& material : result.materials) {
			if (material.baseColorImage >= static_cast<i32>(result.images.size())) {
				firstContradiction = std::format("{}: material '{}' names image {} of {}", name, material.name, material.baseColorImage, result.images.size());
				break;
			}
		}
	}

	EXPECT_TRUE(firstContradiction.empty()) << firstContradiction;

	str badSummary;
	for (const str& bad : badFiles) {
		badSummary += "\n  " + bad;
	}
	EXPECT_GE(parsed, 668u) << parsed << " parsed / " << refusedAsEmpty << " empty / "
							<< refusedForBadFile << " unreadable / " << unnameable << " unnameable";
	EXPECT_LE(refusedForBadFile, 9u) << badSummary;
	EXPECT_LE(refusedAsEmpty, 2u) << refusedAsEmpty;
	EXPECT_LE(refused, 11u) << refused << " refused of " << (parsed + refused);
	EXPECT_EQ(unnameable, 0u);
}

TEST(FBXUnitTest, CentimetreAndMetreMatch) {
	ModelParseResult centimetres = ParseFBX(CorpusFile("blender_293_textures_7400_binary.fbx"));
	ASSERT_TRUE(centimetres.success) << centimetres.error;
	ModelParseResult metres = ParseFBX(CorpusFile("blender_293_embedded_textures_7400_binary.fbx"));
	ASSERT_TRUE(metres.success) << metres.error;

	Vec3 fromCentimetres = WorldSize(centimetres);
	Vec3 fromMetres = WorldSize(metres);
	EXPECT_NEAR(fromCentimetres.x, 2.0f, 1e-3f);
	EXPECT_NEAR(fromCentimetres.y, 2.0f, 1e-3f);
	EXPECT_NEAR(fromCentimetres.z, 2.0f, 1e-3f);
	EXPECT_NEAR(fromCentimetres.x, fromMetres.x, 1e-3f);
	EXPECT_NEAR(fromCentimetres.y, fromMetres.y, 1e-3f);
	EXPECT_NEAR(fromCentimetres.z, fromMetres.z, 1e-3f);
}

TEST(FBXUnitTest, InchConvertsByRatio) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max_geometry_transform_types_6100_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;

	Vec3 size = WorldSize(parsed);
	EXPECT_NEAR(size.x, 0.508f, 1e-4f);
	EXPECT_NEAR(size.y, 0.254f, 1e-4f);
	EXPECT_NEAR(size.z, 0.508f, 1e-4f);
}

TEST(FBXUnitTest, ConversionGoesToTransforms) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_cube_7500_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.meshes.size(), 1u);
	ASSERT_EQ(parsed.nodes.size(), 1u);

	AABB local = parsed.meshes[0].bounds;
	EXPECT_NEAR(local.Size().x, 1.0f, 1e-4f);
	EXPECT_NEAR(local.Size().y, 1.0f, 1e-4f);
	EXPECT_NEAR(local.Size().z, 1.0f, 1e-4f);

	EXPECT_NEAR(parsed.nodes[0].scale.x, 0.01f, 1e-6f);
	EXPECT_NEAR(parsed.nodes[0].scale.y, 0.01f, 1e-6f);
	EXPECT_NEAR(parsed.nodes[0].scale.z, 0.01f, 1e-6f);

	Vec3 size = WorldSize(parsed);
	EXPECT_NEAR(size.x, 0.01f, 1e-6f);
}

TEST(FBXParseTest, NothingIsLostAboveFilesRootNodes) {
	ModelParseResult parsed = ParseFBX(CorpusFile("blender_279_default_7400_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;

	Vec3 size = WorldSize(parsed);
	EXPECT_NEAR(size.x, 2.0f, 1e-3f);
	EXPECT_NEAR(size.y, 2.0f, 1e-3f);
	EXPECT_NEAR(size.z, 2.0f, 1e-3f);
}

TEST(FBXParseTest, ParentsAlwaysPrecedeTheirChildren) {
	const char* files[] = {
		"maya_kenney_character_7700_binary.fbx",
		"max2009_blob_5800_ascii.fbx",
		"blender_293_barbarian_7400_binary.fbx",
		"motionbuilder_cube_7700_binary.fbx",
		"zbrush_d20_7500_binary.fbx",
	};
	for (const char* file : files) {
		ModelParseResult parsed = ParseFBX(CorpusFile(file));
		ASSERT_TRUE(parsed.success) << file << ": " << parsed.error;
		ASSERT_FALSE(parsed.nodes.empty()) << file;

		size_t roots = 0;
		for (size_t i = 0; i < parsed.nodes.size(); ++i) {
			EXPECT_LT(parsed.nodes[i].parent, static_cast<i32>(i)) << file << " node " << i;
			if (parsed.nodes[i].parent < 0) {
				++roots;
			}
		}
		EXPECT_GT(roots, 0u) << file;
	}
}

TEST(FBXParseTest, RiggedCharacterKeepsNodes) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_kenney_character_7700_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;

	EXPECT_EQ(parsed.nodes.size(), 60u);
	ASSERT_EQ(parsed.meshes.size(), 1u);
	EXPECT_EQ(parsed.materials.size(), 1u);

	size_t meshNodes = 0;
	for (const ModelNode& node : parsed.nodes) {
		if (node.mesh >= 0) {
			++meshNodes;
		}
	}
	EXPECT_EQ(meshNodes, 1u);

	EXPECT_EQ(parsed.meshes[0].indices.size(), 1604u * 3);
	EXPECT_GE(parsed.meshes[0].vertices.size(), 804u);
	EXPECT_LT(parsed.meshes[0].vertices.size(), 1604u * 3);

	Vec3 size = WorldSize(parsed);
	EXPECT_NEAR(size.y, 3.7647f, 1e-3f);
}

TEST(FBXParseTest, ParserResolvesEulerAndPrePost) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_rotation_order_7500_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.nodes.size(), 6u);

	size_t distinct = 0;
	for (size_t i = 0; i < parsed.nodes.size(); ++i) {
		bool duplicate = false;
		for (size_t j = 0; j < i; ++j) {
			if (Quat::Approximately(parsed.nodes[i].rotation, parsed.nodes[j].rotation, 1e-3f)) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			++distinct;
		}
		EXPECT_FALSE(parsed.nodes[i].shearedMatrix) << "node " << i;
	}
	EXPECT_EQ(distinct, 6u);
}

TEST(FBXParseTest, UninheritedScaleGetsHelper) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_dual_quaternion_scale_7500_ascii.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.nodes.size(), 5u);

	ASSERT_EQ(CountNodesNamed(parsed, "(scale compensation)"), 1u);
	for (const ModelNode& node : parsed.nodes) {
		if (node.name != "(scale compensation)") {
			continue;
		}
		EXPECT_NEAR(node.scale.x, 1.0f, 1e-5f);
		EXPECT_NEAR(node.scale.y, 0.5f, 1e-5f);
		EXPECT_NEAR(node.scale.z, 2.0f, 1e-5f);
		EXPECT_GE(node.parent, 0);
	}
}

TEST(FBXParseTest, GeometryTransformBaked) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max_geometry_transform_types_6100_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;

	EXPECT_EQ(parsed.nodes.size(), 5u);
	EXPECT_EQ(CountNodesNamed(parsed, "(geometry transform)"), 1u);

	ASSERT_EQ(parsed.meshes.size(), 1u);
	AABB local = parsed.meshes[0].bounds;
	EXPECT_GT(local.Size().x, 0.0f);
	EXPECT_FALSE(Vec3::Approximately(local.Center(), Vec3(), 1e-4f));
}

TEST(FBXParseTest, PolygonsTriangulatedAndWelded) {
	ModelParseResult parsed = ParseFBX(CorpusFile("blender_300_ngon_big_7400_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_FALSE(parsed.meshes.empty());

	const ModelMesh& mesh = parsed.meshes[0];
	EXPECT_GT(mesh.indices.size(), 0u);
	EXPECT_EQ(mesh.indices.size() % 3, 0u);

	EXPECT_LT(mesh.vertices.size(), mesh.indices.size());

	for (u32 index : mesh.indices) {
		ASSERT_LT(index, mesh.vertices.size());
	}
}

TEST(FBXParseTest, MaterialSlotsDensePerMesh) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max2009_blob_5800_ascii.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_FALSE(parsed.meshes.empty());

	const ModelMesh& blob = parsed.meshes[0];
	EXPECT_EQ(blob.name, "Box01");
	EXPECT_EQ(blob.submeshes.size(), 4u);
	EXPECT_EQ(blob.materials.size(), 4u);

	Vector<u32> seen{GetMemoryResource(MemTag::Asset)};
	size_t triangles = 0;
	for (const SubMesh& submesh : blob.submeshes) {
		ASSERT_LT(submesh.materialSlot, blob.materials.size());
		EXPECT_EQ(std::find(seen.begin(), seen.end(), submesh.materialSlot), seen.end());
		seen.push_back(submesh.materialSlot);
		triangles += submesh.indexCount / 3;
	}
	EXPECT_EQ(triangles, 704u);
}

TEST(FBXParseTest, NodeNamedMeshTakesNodeName) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_kenney_character_7700_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.meshes.size(), 1u);

	EXPECT_EQ(parsed.meshes[0].name, "characterMedium");
}

TEST(FBXMaterialTest, APhongMaterialArrivesAsMetallicRoughness) {
	ModelParseResult parsed = ParseFBX(CorpusFile("maya_textured_cube_7500_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.materials.size(), 1u);

	const ModelMaterial& material = parsed.materials[0];
	EXPECT_EQ(material.name, "phong1");
	EXPECT_NEAR(material.roughnessFactor, 0.553f, 1e-2f);
	EXPECT_NEAR(material.metallicFactor, 0.0f, 1e-6f);

	EXPECT_NEAR(material.baseColorFactor.r, 0.8f, 1e-3f);
	EXPECT_NEAR(material.baseColorFactor.a, 1.0f, 1e-6f);

	ASSERT_GE(material.baseColorImage, 0);
	EXPECT_LT(static_cast<size_t>(material.baseColorImage), parsed.images.size());
}

TEST(FBXMaterialTest, AFileThatStatesMetalnessKeepsIt) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max_pbr_metal_rough_material_7700_ascii.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_FALSE(parsed.materials.empty());

	EXPECT_NEAR(parsed.materials[0].metallicFactor, 0.05f, 1e-3f);
	EXPECT_NEAR(parsed.materials[0].roughnessFactor, 0.06f, 1e-3f);
}

TEST(FBXMaterialTest, SlottedMapsBind) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max_physical_material_textures_7500_binary.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.materials.size(), 1u);
	ASSERT_EQ(parsed.images.size(), 10u);

	const ModelMaterial& material = parsed.materials[0];
	EXPECT_EQ(material.name, "PhysicalMaterial");

	EXPECT_EQ(material.baseColorImage, 1);
	EXPECT_EQ(material.normalImage, 8);
	EXPECT_EQ(material.emissiveImage, 6);
	EXPECT_EQ(material.metallicRoughnessImage, -1);

	EXPECT_EQ(material.occlusionImage, -1);

	EXPECT_NEAR(material.normalScale, 1.0f, 1e-6f);
	EXPECT_NEAR(material.occlusionStrength, 1.0f, 1e-6f);
}

TEST(FBXMaterialTest, EmissiveSurvivesNoneStaysBlack) {
	ModelParseResult parsed = ParseFBX(CorpusFile("max2009_blob_5800_ascii.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	ASSERT_EQ(parsed.materials.size(), 7u);
	EXPECT_EQ(parsed.images.size(), 0u);

	EXPECT_EQ(parsed.materials[0].name, "Right");
	EXPECT_NEAR(parsed.materials[0].emissiveFactor.x, 0.0f, 1e-3f);
	EXPECT_NEAR(parsed.materials[0].emissiveFactor.y, 0.804f, 1e-3f);
	EXPECT_NEAR(parsed.materials[0].emissiveFactor.z, 0.0f, 1e-3f);

	EXPECT_EQ(parsed.materials[1].name, "Left");
	EXPECT_NEAR(parsed.materials[1].emissiveFactor.x, 0.824f, 1e-3f);
	EXPECT_NEAR(parsed.materials[1].emissiveFactor.y, 0.0f, 1e-3f);

	for (size_t i = 2; i < parsed.materials.size(); ++i) {
		const Vec3& emissive = parsed.materials[i].emissiveFactor;
		EXPECT_NEAR(emissive.x + emissive.y + emissive.z, 0.0f, 1e-4f) << parsed.materials[i].name;
	}
}

TEST(FBXImageTest, EmbeddedAsBytesExternalAsPath) {
	ModelParseResult embedded = ParseFBX(CorpusFile("blender_293_embedded_textures_7400_binary.fbx"));
	ASSERT_TRUE(embedded.success) << embedded.error;
	ASSERT_EQ(embedded.images.size(), 5u);
	for (const ModelImage& image : embedded.images) {
		EXPECT_FALSE(image.IsExternal()) << image.name;
		EXPECT_GT(image.embeddedBytes.size(), 0u) << image.name;
		EXPECT_EQ(image.extension, ".png") << image.name;
	}
	EXPECT_EQ(embedded.images[0].name, "checkerboard_diffuse");

	ModelParseResult external = ParseFBX(CorpusFile("blender_293_textures_7400_binary.fbx"));
	ASSERT_TRUE(external.success) << external.error;
	ASSERT_EQ(external.images.size(), 5u);
	str corpus = CorpusDir().generic_string();
	for (const ModelImage& image : external.images) {
		ASSERT_TRUE(image.IsExternal()) << image.name;
		EXPECT_TRUE(image.embeddedBytes.empty()) << image.name;
		EXPECT_TRUE(image.externalPath.starts_with(corpus)) << image.externalPath;
		EXPECT_EQ(image.externalPath.find('\\'), str::npos);
	}
}

TEST(FBXHostileInputTest, DamagedFilesFailWithReasonNotAsserting) {
	std::filesystem::path source = CorpusDir() / "maya_cube_7500_binary.fbx";
	ASSERT_TRUE(std::filesystem::exists(source));
	Buffer whole = FileSystem::ReadFile(Path(source.generic_string()), MemTag::Asset);
	ASSERT_TRUE(whole);
	ASSERT_GT(whole.size, 1024u);

	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
									("shalltear_fbx_hostile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code ec;
	std::filesystem::create_directories(scratch, ec);

	auto writeAndParse = [&](const char* name, const Vector<u8>& bytes) {
		Path at((scratch / name).generic_string());
		Buffer payload(bytes.size(), MemTag::Asset);
		if (!bytes.empty()) {
			std::memcpy(payload.data, bytes.data(), bytes.size());
		}
		ASSERT_TRUE(FileSystem::WriteFile(at, payload));
		ModelParseResult parsed = ParseFBX(at);
		if (!parsed.success) {
			EXPECT_FALSE(parsed.error.empty()) << name;
			return;
		}
		for (size_t n = 0; n < parsed.nodes.size(); ++n) {
			ASSERT_LT(parsed.nodes[n].parent, static_cast<i32>(n)) << name;
			ASSERT_LT(parsed.nodes[n].mesh, static_cast<i32>(parsed.meshes.size())) << name;
		}
		for (const ModelMesh& mesh : parsed.meshes) {
			ASSERT_EQ(mesh.indices.size() % 3, 0u) << name;
			for (u32 index : mesh.indices) {
				ASSERT_LT(index, mesh.vertices.size()) << name;
			}
		}
	};

	Vector<u8> bytes{GetMemoryResource(MemTag::Asset)};

	bytes.assign(whole.data, whole.data + whole.size / 2);
	writeAndParse("truncated.fbx", bytes);

	bytes.assign(whole.data, whole.data + whole.size);
	for (size_t i = 64; i < bytes.size(); i += 7) {
		bytes[i] = static_cast<u8>(bytes[i] ^ 0xFF);
	}
	writeAndParse("shredded.fbx", bytes);

	bytes.assign(whole.data, whole.data + whole.size);
	u32 impossible = 999999;
	std::memcpy(bytes.data() + 23, &impossible, sizeof(impossible));
	writeAndParse("version_impossible.fbx", bytes);

	bytes.clear();
	writeAndParse("empty.fbx", bytes);

	std::filesystem::remove_all(scratch, ec);
}

TEST(FBXHostileInputTest, CyclicHierarchyIsRefusedNotFollowed) {
	ModelParseResult parsed = ParseFBX(CorpusFile("synthetic_node_cycle_fail_7700_ascii.fbx"));
	EXPECT_FALSE(parsed.success);
	EXPECT_FALSE(parsed.error.empty());
}

TEST(FBXHostileInputTest, DeepHierarchyWalkedIteratively) {
	ModelParseResult parsed = ParseFBX(CorpusFile("synthetic_id_collision_7500_ascii.fbx"));
	ASSERT_TRUE(parsed.success) << parsed.error;
	EXPECT_EQ(parsed.nodes.size(), 10001u);

	Vector<u32> depth{GetMemoryResource(MemTag::Asset)};
	depth.resize(parsed.nodes.size(), 0);
	u32 deepest = 0;
	for (size_t i = 0; i < parsed.nodes.size(); ++i) {
		ASSERT_LT(parsed.nodes[i].parent, static_cast<i32>(i)) << "node " << i;
		if (parsed.nodes[i].parent >= 0) {
			depth[i] = depth[static_cast<size_t>(parsed.nodes[i].parent)] + 1;
		}
		deepest = std::max(deepest, depth[i]);
	}
	EXPECT_EQ(deepest, 9999u);
}

TEST(FBXHostileInputTest, NonFbxRefusedByName) {
	std::filesystem::path scratch = std::filesystem::temp_directory_path() /
									("shalltear_fbx_notfbx_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code ec;
	std::filesystem::create_directories(scratch, ec);

	Path at((scratch / "actually_text.fbx").generic_string());
	const char* text = "this is not an FBX, it is a note the user forgot to rename\n";
	Buffer payload(std::strlen(text), MemTag::Asset);
	std::memcpy(payload.data, text, payload.size);
	ASSERT_TRUE(FileSystem::WriteFile(at, payload));

	ModelParseResult parsed = ParseModel(at);
	EXPECT_FALSE(parsed.success);
	EXPECT_FALSE(parsed.error.empty());

	std::filesystem::remove_all(scratch, ec);
}

TEST_F(FBXImportFixture, DroppedFbxMakesObjectPerNode) {
	ASSERT_TRUE(StageCorpusFile("maya_kenney_character_7700_binary.fbx"));

	SceneSystem::Initialize();
	{
		Guid model = ImportModel("project://maya_kenney_character_7700_binary.fbx");
		ASSERT_TRUE(model.IsValid());

		Handle<Scene> scene = SceneSystem::CreateScene("FbxDrop");
		Handle<SceneObject> root = SceneSystem::Instantiate(model, scene);
		ASSERT_TRUE(root.IsValid());

		EXPECT_EQ(root->name, "maya_kenney_character_7700_binary");

		size_t objects = 0;
		size_t renderers = 0;
		Handle<SceneObject> withMesh;
		auto walk = [&](auto&& self, const Handle<SceneObject>& object) -> void {
			++objects;
			if (object->GetComponent<SkinnedMeshRenderer>().IsValid()) {
				++renderers;
				withMesh = object;
			}
			EXPECT_FALSE(object->GetComponent<MeshRenderer>().IsValid());
			for (size_t i = 0; i < object->GetChildCount(); ++i) {
				self(self, object->GetChild(i));
			}
		};
		walk(walk, root);

		EXPECT_EQ(objects, 61u);
		ASSERT_EQ(renderers, 1u);
		EXPECT_EQ(withMesh->name, "characterMedium");

		Handle<SkinnedMeshRenderer> renderer = withMesh->GetComponent<SkinnedMeshRenderer>();
		ASSERT_TRUE(renderer.IsValid());
		ASSERT_TRUE(renderer->GetMesh().IsValid());
		EXPECT_EQ(renderer->GetMesh().Get()->indices.size(), 1604u * 3);
		ASSERT_EQ(renderer->GetMaterialCount(), 1u);
		EXPECT_TRUE(renderer->GetMaterial(static_cast<u32>(0)).IsValid());

		ASSERT_TRUE(renderer->GetSkin().IsValid());
		EXPECT_EQ(renderer->GetMesh().Get()->skinInfluences.size(), renderer->GetMesh().Get()->vertices.size());
		ASSERT_TRUE(renderer->GetSkeleton().IsValid());
		Handle<SkeletonAsset> skeleton = renderer->GetSkin().Get()->skeleton;
		ASSERT_TRUE(skeleton.IsValid());
		EXPECT_EQ(renderer->GetSkeleton().Get()->GetSkeleton().GetInstanceId(), skeleton.GetInstanceId());

		const Mat4& toWorld = withMesh->GetWorldMatrix();
		AABB world = AABB::Empty();
		for (const MeshVertex& vertex : renderer->GetMesh().Get()->vertices) {
			Vec4 transformed = toWorld * Vec4(vertex.position, 1.0f);
			world.Encapsulate(Vec3(transformed.x, transformed.y, transformed.z));
		}
		Vec3 size = world.Size();
		f32 tallest = Math::Max(size.x, Math::Max(size.y, size.z));
		EXPECT_NEAR(tallest, 3.7647f, 1e-2f);

		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
	}
	SceneSystem::Terminate();
}

TEST_F(FBXImportFixture, EmbeddedTextureExtractedAndBound) {
	ASSERT_TRUE(StageCorpusFile("blender_293_embedded_textures_7400_binary.fbx"));

	Guid model = ImportModel("project://blender_293_embedded_textures_7400_binary.fbx");
	ASSERT_TRUE(model.IsValid());

	std::filesystem::path images = _root / "blender_293_embedded_textures_7400_binary.fbx.images";
	ASSERT_TRUE(std::filesystem::exists(images)) << images.generic_string();
	EXPECT_FALSE(std::filesystem::exists(_root / "blender_293_embedded_textures_7400_binary.fbx.materials"));

	std::error_code ec;
	size_t extracted = 0;
	bool sawDiffuse = false;
	for (const auto& entry : std::filesystem::directory_iterator(images, ec)) {
		if (entry.path().extension() != ".png") {
			continue;
		}
		++extracted;
		EXPECT_GT(std::filesystem::file_size(entry.path(), ec), 0u);
		if (entry.path().filename().generic_string() == "0_checkerboard_diffuse.png") {
			sawDiffuse = true;
		}
	}
	EXPECT_EQ(extracted, 5u);
	EXPECT_TRUE(sawDiffuse);

	Path source("project://blender_293_embedded_textures_7400_binary.fbx");
	std::vector<str> materialKeys;
	for (const str& key : ProductLookup::Keys(source)) {
		if (key.starts_with("Materials/")) {
			materialKeys.push_back(key);
		}
	}
	ASSERT_EQ(materialKeys.size(), 1u);
	Asset* material = ProductLookup::Load(source, materialKeys[0]);
	ASSERT_NE(material, nullptr);
	EXPECT_TRUE(static_cast<MaterialAsset*>(material)->GetTexture(StringHash("baseColorTexture")).IsValid());
	AssetDatabase::UnloadAsset(material);
}

TEST(FBXAllocationTest, EveryParsePathReturnsMemoryToTag) {
	const char* files[] = {
		"maya_kenney_character_7700_binary.fbx",
		"max2009_blob_5800_ascii.fbx",
		"maya_scale_no_inherit_7700_ascii.fbx",
		"motionbuilder_actor_7700_binary.fbx",
		"synthetic_node_cycle_fail_7700_ascii.fbx",
	};

	{
		ModelParseResult parsed = ParseFBX(CorpusFile("maya_kenney_character_7700_binary.fbx"));
		ASSERT_TRUE(parsed.success) << parsed.error;
	}

	const size_t before = MemoryManager::GetBytesForTag(MemTag::Asset);
	const size_t allocsBefore = MemoryManager::GetAllocationCountForTag(MemTag::Asset);
	for (int round = 0; round < 3; ++round) {
		for (const char* file : files) {
			ModelParseResult parsed = ParseFBX(CorpusFile(file));
			(void)parsed;
		}
	}
	const size_t after = MemoryManager::GetBytesForTag(MemTag::Asset);
	const size_t allocsAfter = MemoryManager::GetAllocationCountForTag(MemTag::Asset);

	EXPECT_LE(after, before + 64u * 1024u) << before << " -> " << after;
	EXPECT_EQ(allocsAfter, allocsBefore) << allocsBefore << " -> " << allocsAfter;
}

namespace {

	class FBXCapturingSink : public ILogSink {
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

		void Clear() {
			std::lock_guard<std::mutex> lock(_mutex);
			_messages.clear();
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
	};

	void DrainLog(FBXCapturingSink& sink, const char* tag) {
		SLT_LOG_INFO("FbxTest", "fbx-log-barrier-{}", tag);
		for (int spin = 0; spin < 2000; ++spin) {
			if (sink.CountMatching(std::string_view(tag)) > 0) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ADD_FAILURE() << "log worker never delivered barrier '" << tag << "'";
	}

} // namespace

TEST(FBXMaterialTest, LossyMappingReportedOnce) {
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<FBXCapturingSink>();
	LogManager::AddSink(sink);

	constexpr std::string_view Report = "the metallic-roughness mapping drops";

	DrainLog(*sink, "before-shared");
	sink->Clear();
	ModelParseResult shared = ParseFBX(CorpusFile("maya_shared_textures_7500_binary.fbx"));
	ASSERT_TRUE(shared.success) << shared.error;
	EXPECT_EQ(shared.materials.size(), 2u);
	DrainLog(*sink, "after-shared");
	EXPECT_EQ(sink->CountMatching(Report), 1u);
	EXPECT_EQ(sink->CountMatching("maya_shared_textures_7500_binary.fbx"), 1u);
	EXPECT_EQ(sink->CountMatching("1 of 2 material(s)"), 1u);

	DrainLog(*sink, "before-blob");
	sink->Clear();
	ModelParseResult blob = ParseFBX(CorpusFile("max2009_blob_5800_ascii.fbx"));
	ASSERT_TRUE(blob.success) << blob.error;
	EXPECT_EQ(blob.materials.size(), 7u);
	DrainLog(*sink, "after-blob");
	EXPECT_EQ(sink->CountMatching(Report), 0u);

	DrainLog(*sink, "before-quiet");
	sink->Clear();
	ModelParseResult quiet = ParseFBX(CorpusFile("blender_279_default_7400_binary.fbx"));
	ASSERT_TRUE(quiet.success) << quiet.error;
	DrainLog(*sink, "after-quiet");
	EXPECT_EQ(sink->CountMatching(Report), 0u);

	LogManager::Terminate();
	LogManager::RemoveAllSinks();
	JobSystem::Terminate();
}

TEST(FBXGeometryPreconditionTest, MeshlessFileImportsAnimation) {
	ModelParseOptions options;
	options.tracks = true;
	ModelParseResult parsed = ParseFBX(CorpusFile("blender_279_bone_radius_6100_ascii.fbx"), options);

	ASSERT_TRUE(parsed.success) << parsed.error;
	EXPECT_TRUE(parsed.meshes.empty());
	EXPECT_EQ(parsed.nodes.size(), 13u);
	EXPECT_EQ(parsed.takes.size(), 1u);
	EXPECT_EQ(parsed.tracks.size(), 15u);
	EXPECT_FALSE(parsed.takes[0].tracks.empty());
}

TEST(FBXGeometryPreconditionTest, EmptyFileRefusedByName) {
	ModelParseResult parsed = ParseFBX(CorpusFile("motionbuilder_actor_7700_binary.fbx"));

	EXPECT_FALSE(parsed.success);
	EXPECT_EQ(parsed.error, "the file has no nodes/geometry");
	EXPECT_TRUE(parsed.nodes.empty());
	EXPECT_TRUE(parsed.meshes.empty());
}

TEST(FBXGeometryPreconditionTest, RefusalIgnoresTrackRequest) {
	ModelParseOptions withTracks;
	withTracks.tracks = true;

	for (const char* name : {"blender_279_bone_radius_6100_ascii.fbx", "motionbuilder_actor_7700_binary.fbx"}) {
		ModelParseResult quiet = ParseFBX(CorpusFile(name));
		ModelParseResult asked = ParseFBX(CorpusFile(name), withTracks);
		EXPECT_EQ(quiet.success, asked.success) << name;
		EXPECT_EQ(quiet.error, asked.error) << name;
	}
}

TEST(FBXGeometryPreconditionTest, TrianglelessMeshWarns) {
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<FBXCapturingSink>();
	LogManager::AddSink(sink);

	constexpr std::string_view Report = "no triangles reach this import";

	DrainLog(*sink, "before-edges");
	sink->Clear();
	ModelParseResult edges = ParseFBX(CorpusFile("blender_279_edge_vertex_7400_binary.fbx"));
	ASSERT_TRUE(edges.success) << edges.error;
	ASSERT_FALSE(edges.meshes.empty());
	DrainLog(*sink, "after-edges");
	EXPECT_EQ(sink->CountMatching(Report), 1u);
	EXPECT_EQ(sink->CountMatching("blender_279_edge_vertex_7400_binary.fbx"), 1u);

	DrainLog(*sink, "before-bones");
	sink->Clear();
	ModelParseResult bones = ParseFBX(CorpusFile("blender_279_bone_radius_6100_ascii.fbx"));
	ASSERT_TRUE(bones.success) << bones.error;
	ASSERT_TRUE(bones.meshes.empty());
	DrainLog(*sink, "after-bones");
	EXPECT_EQ(sink->CountMatching(Report), 0u);

	LogManager::Terminate();
	LogManager::RemoveAllSinks();
	JobSystem::Terminate();
}
