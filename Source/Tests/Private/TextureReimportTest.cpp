#include "TestsPCH.h"

#include "ScopedImportCostThreshold.h"
#include "ShadingHarness.h"
#include "TestImages.h"

#include "Shalltear/Core/Graphics/TextureAsset.h"

using namespace Shalltear;
using ShadingTest::CenterTexel;
using ShadingTest::MaterialReadbackFixture;
using ShadingTest::ShadingStage;

namespace {

	const u8 red[4] = {255, 0, 0, 255};
	const u8 green[4] = {0, 255, 0, 255};

	Guid UnlitShaderGuid() {
		Asset* shader = AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"));
		return shader ? shader->assetId : Guid{};
	}

} // anonymous namespace

class TextureReimportFixture : public MaterialReadbackFixture {};

TEST_F(TextureReimportFixture, HeldTextureTakesRewrittenFileInPlace) {
	TestImages::WriteTGA(_root / "Map.tga", red);
	Asset* held = AssetDatabase::LoadAsset(Path("project://Map.tga"));
	ASSERT_NE(held, nullptr);
	TextureAsset* texture = static_cast<TextureAsset*>(held);
	ASSERT_EQ(texture->pixels.size(), 4u);
	ASSERT_EQ(texture->pixels[0], 255);
	const u64 instance = texture->GetInstanceId();
	const u32 versionBefore = texture->version;

	TestImages::WriteTGA(_root / "Map.tga", green);
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Map.tga")));

	Asset* again = AssetDatabase::LoadAsset(Path("project://Map.tga"));
	EXPECT_EQ(again, held);
	EXPECT_EQ(again->GetInstanceId(), instance);
	EXPECT_EQ(texture->pixels[0], 0);
	EXPECT_EQ(texture->pixels[1], 255);
	EXPECT_EQ(texture->version, versionBefore + 1);
	AssetDatabase::UnloadAsset(again);
	AssetDatabase::UnloadAsset(held);
}

TEST_F(TextureReimportFixture, RewriteToSameBytesRefillsNothing) {
	ScopedImportCostThreshold noCache{ScopedImportCostThreshold::CACHE_NOTHING};
	TestImages::WriteTGA(_root / "Map.tga", red);
	Asset* held = AssetDatabase::LoadAsset(Path("project://Map.tga"));
	ASSERT_NE(held, nullptr);
	TextureAsset* texture = static_cast<TextureAsset*>(held);
	const u32 versionBefore = texture->version;

	TestImages::WriteTGA(_root / "Map.tga", red);
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Map.tga")));
	EXPECT_EQ(texture->version, versionBefore);
	EXPECT_EQ(texture->pixels[0], 255);

	TestImages::WriteTGA(_root / "Map.tga", green);
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Map.tga")));
	EXPECT_EQ(texture->version, versionBefore + 1);
	AssetDatabase::UnloadAsset(held);
}

TEST_F(TextureReimportFixture, UndecodableFileKeepsTexture) {
	TestImages::WriteTGA(_root / "Map.tga", red);
	Asset* held = AssetDatabase::LoadAsset(Path("project://Map.tga"));
	ASSERT_NE(held, nullptr);
	TextureAsset* texture = static_cast<TextureAsset*>(held);
	const u32 versionBefore = texture->version;

	WriteProjectFile("Map.tga", "this is not an image");
	EXPECT_FALSE(AssetDatabase::ReimportAsset(Path("project://Map.tga")));

	ASSERT_EQ(texture->pixels.size(), 4u);
	EXPECT_EQ(texture->pixels[0], 255);
	EXPECT_EQ(texture->version, versionBefore);
	EXPECT_EQ(AssetDatabase::LoadAsset(Path("project://Map.tga")), held);
	AssetDatabase::UnloadAsset(held);
	AssetDatabase::UnloadAsset(held);
}

TEST_F(TextureReimportFixture, DrawOfHeldTextureShowsRewrittenFile) {
	if (!RenderSystem::IsRenderingAvailable() || !CompilerAvailable()) {
		GTEST_SKIP() << "no Vulkan device or no shader compiler";
	}
	TestImages::WriteTGA(_root / "Map.tga", red);
	Asset* texture = AssetDatabase::LoadAsset(Path("project://Map.tga"));
	ASSERT_NE(texture, nullptr);
	const str textures = "\t\"textures\": { \"baseColorTexture\": \"" + texture->assetId.ToString() + "\" }";
	MaterialAsset* material = LoadMaterial("Mapped.mat", UnlitShaderGuid(), textures);
	ASSERT_NE(material, nullptr);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 before = CenterTexel(pixels, size);
	ASSERT_GT(before.x, 200.0f);
	ASSERT_LT(before.y, 40.0f);

	TestImages::WriteTGA(_root / "Map.tga", green);
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Map.tga")));

	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 after = CenterTexel(pixels, size);
	EXPECT_LT(after.x, 40.0f);
	EXPECT_GT(after.y, 200.0f);

	TearDownStage(stage);
	AssetDatabase::UnloadAsset(texture);
}
