#include "TestsPCH.h"

#ifndef SHALLTEAR_SHARED
#include "Graphics/ShaderCompiler.h"
#endif

#include "ShadingHarness.h"
#include "TestImages.h"

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

using namespace Shalltear;
using ShadingTest::ArmDiagnosticSink;
using ShadingTest::CenterTexel;
using ShadingTest::ExpectedTexel;
using ShadingTest::Eye;
using ShadingTest::MaterialReadbackFixture;
using ShadingTest::MaterialShadingFixture;
using ShadingTest::RepoDir;
using ShadingTest::ShaderDiagnosticSink;
using ShadingTest::ShadingStage;
using ShadingTest::VIEW_AMBIENT;

namespace {

	constexpr f32 BRDF_PI = 3.14159265359f;
	constexpr f32 BRDF_MIN_PERCEPTUAL_ROUGHNESS = 0.045f;

	f32 DistributionGGX(f32 noH, f32 roughness) {
		f32 oneMinusNoHSquared = 1.0f - noH * noH;
		f32 a = noH * roughness;
		f32 k = roughness / (oneMinusNoHSquared + a * a);
		return k * k * (1.0f / BRDF_PI);
	}

	f32 VSmithGGXCorrelated(f32 noV, f32 noL, f32 roughness) {
		f32 a2 = roughness * roughness;
		f32 lambdaV = noL * std::sqrt(noV * noV * (1.0f - a2) + a2);
		f32 lambdaL = noV * std::sqrt(noL * noL * (1.0f - a2) + a2);
		return 0.5f / Math::Max(lambdaV + lambdaL, 1e-5f);
	}

	Vec3 FSchlick(Vec3 f0, f32 voH) {
		f32 f = std::pow(1.0f - voH, 5.0f);
		return f0 + (Vec3(1.0f, 1.0f, 1.0f) - f0) * f;
	}

	f32 LinearRoughness(f32 perceptual) {
		f32 clamped = Math::Max(perceptual, BRDF_MIN_PERCEPTUAL_ROUGHNESS);
		return clamped * clamped;
	}

	struct CPUSurface {
		Vec3 baseColor{1.0f, 1.0f, 1.0f};
		f32 metallic = 0.0f;
		f32 roughness = 1.0f;
		f32 reflectance = 0.5f;
		f32 occlusion = 1.0f;
	};

	Vec3 DiffuseColor(const CPUSurface& surface) {
		return surface.baseColor * (1.0f - surface.metallic);
	}

	Vec3 F0(const CPUSurface& surface) {
		f32 dielectric = 0.16f * surface.reflectance * surface.reflectance;
		return Vec3::Lerp(Vec3(dielectric, dielectric, dielectric), surface.baseColor, surface.metallic);
	}

	Vec3 StandardLighting(const CPUSurface& surface, Vec3 normal, Vec3 lightDirection, Vec3 lightColor,
						  f32 attenuation, Vec3 viewDirection) {
		Vec3 halfVector = (viewDirection + lightDirection).Normalized();
		f32 noV = std::abs(Vec3::Dot(normal, viewDirection)) + 1e-5f;
		f32 noL = Math::Clamp(Vec3::Dot(normal, lightDirection), 0.0f, 1.0f);
		f32 noH = Math::Clamp(Vec3::Dot(normal, halfVector), 0.0f, 1.0f);
		f32 voH = Math::Clamp(Vec3::Dot(viewDirection, halfVector), 0.0f, 1.0f);

		f32 roughness = LinearRoughness(surface.roughness);
		f32 d = DistributionGGX(noH, roughness);
		f32 v = VSmithGGXCorrelated(noV, noL, roughness);
		Vec3 f = FSchlick(F0(surface), voH);

		Vec3 specular = f * (d * v);
		Vec3 diffuse = DiffuseColor(surface) * (1.0f / BRDF_PI);
		return (diffuse + specular) * lightColor * (attenuation * noL);
	}

	bool DeclaresAnOutputAtALocation(const Vector<u8>& spirv) {
		constexpr u32 OP_DECORATE = 71;
		constexpr u32 OP_VARIABLE = 59;
		constexpr u32 DECORATION_LOCATION = 30;
		constexpr u32 STORAGE_CLASS_OUTPUT = 3;
		constexpr size_t HEADER_WORDS = 5;
		if (spirv.size() < HEADER_WORDS * 4 || spirv.size() % 4 != 0) {
			return false;
		}
		const u32* words = reinterpret_cast<const u32*>(spirv.data());
		const size_t wordCount = spirv.size() / 4;
		std::vector<u32> located;
		std::vector<u32> outputs;
		for (size_t at = HEADER_WORDS; at < wordCount;) {
			const u32 length = words[at] >> 16;
			const u32 opcode = words[at] & 0xFFFF;
			if (length == 0 || at + length > wordCount) {
				break;
			}
			if (opcode == OP_DECORATE && length >= 3 && words[at + 2] == DECORATION_LOCATION) {
				located.push_back(words[at + 1]);
			} else if (opcode == OP_VARIABLE && length >= 4 && words[at + 3] == STORAGE_CLASS_OUTPUT) {
				outputs.push_back(words[at + 2]);
			}
			at += length;
		}
		for (u32 id : outputs) {
			if (std::find(located.begin(), located.end(), id) != located.end()) {
				return true;
			}
		}
		return false;
	}
} // anonymous namespace

TEST(ShaderTemplatePragmaTest, ParsesEveryTemplateValue) {
	struct Row {
		const char* source;
		ShaderTemplate expected;
	};
	const Row rows[] = {
		{"#pragma shalltear_template none\n", ShaderTemplate::None},
		{"#pragma shalltear_template lit\n", ShaderTemplate::Lit},
		{"#pragma shalltear_template unlit\n", ShaderTemplate::Unlit},
	};
	for (const Row& row : rows) {
		ShaderPragmas pragmas;
		ShaderAsset::ParsePragmas(row.source, Path("test.hlsl"), pragmas);
		EXPECT_EQ(pragmas.integrator, row.expected) << row.source;
	}

	ShaderPragmas silent;
	ShaderAsset::ParsePragmas("#pragma shalltear_cull off\n", Path("test.hlsl"), silent);
	EXPECT_EQ(silent.integrator, ShaderTemplate::None);
}

TEST(ShaderTemplatePragmaTest, AnUnknownTemplateKeepsWhatWasThere) {
	ShaderPragmas pragmas;
	pragmas.integrator = ShaderTemplate::Lit;
	ShaderAsset::ParsePragmas("#pragma shalltear_template holographic\n", Path("test.hlsl"), pragmas);
	EXPECT_EQ(pragmas.integrator, ShaderTemplate::Lit);
}

TEST(ShaderTemplatePragmaTest, TextureDefaultParsesBesideNumericOne) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_default baseColor 1 0.5 0 1\n"
		"#pragma shalltear_default normalTexture normal\n"
		"#pragma shalltear_default emissiveTexture black\n"
		"#pragma shalltear_default decalTexture transparent\n"
		"#pragma shalltear_default detailTexture white\n"
		"#pragma shalltear_default rampTexture gray\n"
		"#pragma shalltear_default roughness 0.25\n",
		Path("test.hlsl"), pragmas);

	ASSERT_EQ(pragmas.memberDefaults.size(), 2u);
	EXPECT_EQ(pragmas.memberDefaults[0].name, "baseColor");
	EXPECT_EQ(pragmas.memberDefaults[1].name, "roughness");

	ASSERT_EQ(pragmas.textureDefaults.size(), 5u);
	EXPECT_EQ(pragmas.textureDefaults[0].name, "normalTexture");
	EXPECT_EQ(pragmas.textureDefaults[0].fallback, ShaderTextureFallback::Normal);
	EXPECT_EQ(pragmas.textureDefaults[1].fallback, ShaderTextureFallback::Black);
	EXPECT_EQ(pragmas.textureDefaults[2].fallback, ShaderTextureFallback::Transparent);
	EXPECT_EQ(pragmas.textureDefaults[3].fallback, ShaderTextureFallback::White);
	EXPECT_EQ(pragmas.textureDefaults[4].fallback, ShaderTextureFallback::Gray);
}

TEST(ShaderTemplatePragmaTest, AMalformedTextureDefaultIsDroppedWhole) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_default normalTexture chartreuse\n"
		"#pragma shalltear_default normalTexture normal extra\n"
		"#pragma shalltear_default normalTexture\n"
		"#pragma shalltear_default goodTexture normal\n",
		Path("test.hlsl"), pragmas);
	ASSERT_EQ(pragmas.textureDefaults.size(), 1u);
	EXPECT_EQ(pragmas.textureDefaults[0].name, "goodTexture");
	EXPECT_TRUE(pragmas.memberDefaults.empty());
}

TEST(ShaderAnnotationPragmaTest, RangeHintAndGroupParseIntoLists) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_range roughness 0 1\n"
		"#pragma shalltear_range normalScale 0 2\n"
		"#pragma shalltear_hint baseColor color\n"
		"#pragma shalltear_hint emissive hdr\n"
		"#pragma shalltear_group Surface baseColor roughness\n",
		Path("test.hlsl"), pragmas);

	ASSERT_EQ(pragmas.memberRanges.size(), 2u);
	EXPECT_EQ(pragmas.memberRanges[0].name, "roughness");
	EXPECT_FLOAT_EQ(pragmas.memberRanges[0].minValue, 0.0f);
	EXPECT_FLOAT_EQ(pragmas.memberRanges[0].maxValue, 1.0f);
	EXPECT_FLOAT_EQ(pragmas.memberRanges[1].maxValue, 2.0f);

	ASSERT_EQ(pragmas.memberHints.size(), 2u);
	EXPECT_EQ(pragmas.memberHints[0].hint, ShaderHint::Color);
	EXPECT_EQ(pragmas.memberHints[1].hint, ShaderHint::HDR);

	ASSERT_EQ(pragmas.memberGroups.size(), 1u);
	EXPECT_EQ(pragmas.memberGroups[0].name, "Surface");
	ASSERT_EQ(pragmas.memberGroups[0].members.size(), 2u);
	EXPECT_EQ(pragmas.memberGroups[0].members[0], "baseColor");
	EXPECT_EQ(pragmas.memberGroups[0].members[1], "roughness");
}

TEST(ShaderAnnotationPragmaTest, QuotedGroupNameHoldsSpaces) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_group \"Surface Inputs\" baseColor roughness\n"
		"#pragma shalltear_group \"Detail Inputs\" normalScale\n",
		Path("test.hlsl"), pragmas);
	ASSERT_EQ(pragmas.memberGroups.size(), 2u);
	EXPECT_EQ(pragmas.memberGroups[0].name, "Surface Inputs");
	ASSERT_EQ(pragmas.memberGroups[0].members.size(), 2u);
	EXPECT_EQ(pragmas.memberGroups[0].members[0], "baseColor");
	EXPECT_EQ(pragmas.memberGroups[1].name, "Detail Inputs");
}

TEST(ShaderAnnotationPragmaTest, EveryMalformedAnnotationIsDroppedWhole) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_range roughness 0\n"
		"#pragma shalltear_range roughness 1 0\n"
		"#pragma shalltear_range roughness 0 1 2\n"
		"#pragma shalltear_range roughness low high\n"
		"#pragma shalltear_hint baseColor rainbow\n"
		"#pragma shalltear_hint baseColor\n"
		"#pragma shalltear_group Lonely\n"
		"#pragma shalltear_group \"Unterminated baseColor\n"
		"#pragma shalltear_keyword A B C\n",
		Path("test.hlsl"), pragmas);
	EXPECT_TRUE(pragmas.memberRanges.empty());
	EXPECT_TRUE(pragmas.memberHints.empty());
	EXPECT_TRUE(pragmas.memberGroups.empty());
	EXPECT_TRUE(pragmas.keywords.empty());
}

TEST(ShaderAnnotationPragmaTest, OptionalMemberBesideKeyword) {
	ShaderPragmas pragmas;
	ShaderAsset::ParsePragmas(
		"#pragma shalltear_keyword DETAIL_MAP\n"
		"#pragma shalltear_keyword RIM_LIGHT rimStrength\n",
		Path("test.hlsl"), pragmas);

	ASSERT_EQ(pragmas.keywords.size(), 2u);
	EXPECT_EQ(pragmas.keywords[0], "DETAIL_MAP");
	EXPECT_EQ(pragmas.keywords[1], "RIM_LIGHT");
	ASSERT_EQ(pragmas.keywordBindings.size(), 1u);
	EXPECT_EQ(pragmas.keywordBindings[0].keyword, "RIM_LIGHT");
	EXPECT_EQ(pragmas.keywordBindings[0].member, "rimStrength");
}

TEST(ShaderTemplatePragmaTest, AnUndeclaredSlotFallsBackToWhite) {
	ShaderAsset shader;
	shader.textureDefaults.push_back({"normalTexture", ShaderTextureFallback::Normal});
	EXPECT_EQ(shader.GetTextureFallback("normalTexture"), ShaderTextureFallback::Normal);
	EXPECT_EQ(shader.GetTextureFallback("baseColorTexture"), ShaderTextureFallback::White);
}

TEST_F(MaterialShadingFixture, ShippedSurfaceShadersImportThroughTemplates) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
	}
	Asset* asset = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* lit = static_cast<ShaderAsset*>(asset);

	EXPECT_EQ(lit->integrator, ShaderTemplate::Lit);
	EXPECT_EQ(lit->pushConstantSize, 16u);

	Vector<str> slots{GetMemoryResource(MemTag::Asset)};
	lit->GetMaterialTextureSlots(slots);
	ASSERT_EQ(slots.size(), 5u);
	EXPECT_EQ(slots[0], "baseColorTexture");
	EXPECT_EQ(slots[1], "metallicRoughnessTexture");
	EXPECT_EQ(slots[2], "normalTexture");
	EXPECT_EQ(slots[3], "occlusionTexture");
	EXPECT_EQ(slots[4], "emissiveTexture");

	u32 materialResources = 0;
	for (const ShaderResourceInfo& resource : lit->resources) {
		if (resource.set == MATERIAL_DESCRIPTOR_SET) {
			++materialResources;
		}
	}
	EXPECT_EQ(materialResources, 11u);
	EXPECT_LE(materialResources, MAX_MATERIAL_SET_RESOURCES);

	EXPECT_EQ(lit->GetTextureFallback("normalTexture"), ShaderTextureFallback::Normal);
	EXPECT_EQ(lit->GetTextureFallback("baseColorTexture"), ShaderTextureFallback::White);

	const UniformBlockLayout* layout = lit->GetMaterialParamLayout();
	ASSERT_NE(layout, nullptr);
	EXPECT_EQ(layout->members.size(), 7u);

	Asset* unlitAsset = AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"));
	ASSERT_NE(unlitAsset, nullptr);
	ShaderAsset* unlit = static_cast<ShaderAsset*>(unlitAsset);
	EXPECT_EQ(unlit->integrator, ShaderTemplate::Unlit);
	EXPECT_EQ(unlit->pushConstantSize, 16u);
}

TEST_F(MaterialShadingFixture, TemplatedShaderDependsOnTemplateAndLibrary) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* asset = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* lit = static_cast<ShaderAsset*>(asset);

	auto dependsOn = [&](const char* leaf) {
		for (const ShaderAsset::Dependency& dependency : lit->dependencies) {
			if (dependency.physicalPath.find(leaf) != str::npos) {
				return true;
			}
		}
		return false;
	};
	ASSERT_FALSE(lit->dependencies.empty());
	EXPECT_NE(lit->dependencies[0].physicalPath.find("Lit.hlsl"), str::npos);
	EXPECT_TRUE(dependsOn("ForwardLit.hlsli"));
	EXPECT_TRUE(dependsOn("Surface.hlsli"));
	EXPECT_TRUE(dependsOn("Brdf.hlsli"));
	EXPECT_TRUE(dependsOn("ShalltearCommon.hlsli"));
}

#ifndef SHALLTEAR_SHARED
TEST_F(MaterialShadingFixture, EverySurfaceShaderCompilesThroughSecondIntegrator) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	std::filesystem::path engineAssets = RepoDir("EngineAssets");
	ASSERT_FALSE(engineAssets.empty());
	Path templatePath((engineAssets / "Shaders" / "Templates" / "DepthOnly.hlsli").generic_string());
	str templateSource = FileSystem::ReadTextFile(templatePath, MemTag::Asset);
	ASSERT_FALSE(templateSource.empty());

	for (const char* name : {"Lit.hlsl", "Unlit.hlsl"}) {
		Path surface((engineAssets / "Shaders" / name).generic_string());
		Vector<str> defines{GetMemoryResource(MemTag::Asset)};
		defines.push_back("SHALLTEAR_SURFACE_FILE=\"" + surface.ToString() + "\"");
		for (ShaderStageKind stage : {ShaderStageKind::Vertex, ShaderStageKind::Pixel}) {
			ShaderCompileResult result = ShaderCompiler::Compile(templateSource, templatePath, stage,
																 {defines.data(), defines.size()}, ShaderCompileMode::Optimized);
			EXPECT_TRUE(result.success) << name << ' ' << result.errors;
		}
	}
}
#endif

TEST_F(MaterialShadingFixture, DiagnosticNamesFileAndLine) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Broken.hlsl",
					 "#pragma shalltear_template lit\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = thisIdentifierDoesNotExist;\n"
					 "}\n");

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<ShaderDiagnosticSink>();
	LogManager::AddSink(sink);
	ASSERT_TRUE(ArmDiagnosticSink(sink));

	ShaderAsset asset;
	bool imported = asset.Import(FileSystem::Resolve(Path("project://Broken.hlsl")));

	LogManager::Terminate();
	str captured = sink->Joined();
	sink.reset();
	JobSystem::Terminate();

	EXPECT_FALSE(imported);
	ASSERT_FALSE(captured.empty());
	EXPECT_NE(captured.find("Broken.hlsl:4:"), str::npos) << captured;
	EXPECT_NE(captured.find("thisIdentifierDoesNotExist"), str::npos);
	EXPECT_NE(captured.find("compilation failed for"), str::npos);
	EXPECT_EQ(captured.find("compilation failed for '" + FileSystem::Resolve(Path("engine://Shaders/Templates/ForwardLit.hlsli")).ToString()), str::npos);
}

TEST_F(MaterialShadingFixture, ProjectSurfaceShaderCompilesThroughEngineTemplate) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("ProjectToon.hlsl",
					 "#pragma shalltear_template lit\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 tint; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = tint.rgb;\n"
					 "\tsurface.roughness = 0.3;\n"
					 "}\n");
	Asset* asset = AssetDatabase::LoadAsset(Path("project://ProjectToon.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* shader = static_cast<ShaderAsset*>(asset);
	EXPECT_EQ(shader->integrator, ShaderTemplate::Lit);
	EXPECT_EQ(shader->pushConstantSize, 16u);
	EXPECT_FALSE(shader->variants.empty());
}

TEST_F(MaterialReadbackFixture, ALitTexelMatchesTheAnalyticBrdf) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* litShader = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(litShader, nullptr);

	CPUSurface surface;
	surface.baseColor = Vec3(0.8f, 0.4f, 0.2f);
	surface.metallic = 0.0f;
	surface.roughness = 0.35f;
	surface.reflectance = 0.5f;

	MaterialAsset* material = LoadMaterial("Analytic.mat", litShader->assetId,
										   "\t\"colors\": { \"baseColor\": [0.8, 0.4, 0.2, 1] },\n"
										   "\t\"floats\": { \"metallic\": 0, \"roughness\": 0.35, \"reflectance\": 0.5, \"normalScale\": 1, \"occlusionStrength\": 1 },\n"
										   "\t\"vec3s\": { \"emissive\": [0, 0, 0] }");
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));

	const Vec3 normal(0.0f, 0.0f, 1.0f);
	const Vec3 view = Eye.Normalized();

	Vec3 expectedLinear = Vec3(VIEW_AMBIENT, VIEW_AMBIENT, VIEW_AMBIENT) * DiffuseColor(surface) * surface.occlusion;
	expectedLinear += StandardLighting(surface, normal, stage.lightDirection, stage.lightColor, 1.0f, view);
	Vec3 expected = ExpectedTexel(expectedLinear);
	Vec3 measured = CenterTexel(pixels, size);

	if (measured.x + measured.y + measured.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, UnlitTexelIsBaseColor) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* unlitShader = AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"));
	ASSERT_NE(unlitShader, nullptr);

	const Vec3 baseColor(0.25f, 0.5f, 0.75f);
	MaterialAsset* material = LoadMaterial("Flat.mat", unlitShader->assetId,
										   "\t\"colors\": { \"baseColor\": [0.25, 0.5, 0.75, 1] }");
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));

	Vec3 expected = ExpectedTexel(baseColor);
	Vec3 measured = CenterTexel(pixels, size);
	if (measured.x + measured.y + measured.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, CustomLightingReplacesBrdf) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Toon.hlsl",
					 "#define SHALLTEAR_CUSTOM_LIGHTING\n"
					 "#pragma shalltear_template lit\n"
					 "#pragma shalltear_default baseColor 1 1 1 1\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
					 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
					 "\tfloat NoL = saturate(dot(normalWS, light.direction));\n"
					 "\tfloat band = NoL > 0.5 ? 1.0 : 0.25;\n"
					 "\treturn surface.baseColor * light.color * (band * light.attenuation);\n"
					 "}\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = baseColor.rgb;\n"
					 "\tsurface.metallic = 0.0;\n"
					 "}\n");
	Asset* toonShader = AssetDatabase::LoadAsset(Path("project://Toon.hlsl"));
	ASSERT_NE(toonShader, nullptr);

	const Vec3 baseColor(0.6f, 0.3f, 0.9f);
	MaterialAsset* material = LoadMaterial("Toon.mat", toonShader->assetId,
										   "\t\"colors\": { \"baseColor\": [0.6, 0.3, 0.9, 1] }");
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));

	const Vec3 normal(0.0f, 0.0f, 1.0f);
	f32 noL = Math::Clamp(Vec3::Dot(normal, stage.lightDirection), 0.0f, 1.0f);
	f32 band = noL > 0.5f ? 1.0f : 0.25f;
	CPUSurface surface;
	surface.baseColor = baseColor;
	surface.metallic = 0.0f;

	Vec3 expectedLinear = Vec3(VIEW_AMBIENT, VIEW_AMBIENT, VIEW_AMBIENT) * DiffuseColor(surface);
	expectedLinear += baseColor * stage.lightColor * band;
	Vec3 expected = ExpectedTexel(expectedLinear);
	Vec3 measured = CenterTexel(pixels, size);
	if (measured.x + measured.y + measured.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

namespace {

	struct ShadowTally {
		u32 lit = 0;
		u32 shadowed = 0;
		u32 disagreeing = 0;
	};

	void TallyShadow(MaterialAsset* ground, MaterialAsset* occluder, ShadowTally& tally) {
		Asset* cubeAsset = AssetDatabase::LoadAsset(Path("engine://Meshes/Cube.primitive"));
		ASSERT_NE(cubeAsset, nullptr);
		Handle<MeshAsset> cube(static_cast<MeshAsset*>(cubeAsset));
		Handle<Scene> scene = SceneSystem::CreateScene("ShadowApart");
		Handle<SceneObject> slab = SceneSystem::CreateObject("Ground", scene);
		slab->SetLocalPosition(Vec3(0.0f, -1.0f, 0.0f));
		slab->SetLocalScale(Vec3(32.0f, 0.5f, 32.0f));
		Handle<MeshRenderer> slabRenderer = slab->AddComponent<MeshRenderer>();
		slabRenderer->SetMesh(cube);
		slabRenderer->SetMaterial(0, Handle<MaterialAsset>(ground));
		Handle<SceneObject> block = SceneSystem::CreateObject("Occluder", scene);
		block->SetLocalPosition(Vec3(1.0f, 2.0f, -0.5f));
		block->SetLocalScale(Vec3(2.0f, 2.0f, 2.0f));
		Handle<MeshRenderer> blockRenderer = block->AddComponent<MeshRenderer>();
		blockRenderer->SetMesh(cube);
		if (occluder) {
			blockRenderer->SetMaterial(0, Handle<MaterialAsset>(occluder));
		}
		Handle<SceneObject> sun = SceneSystem::CreateObject("Sun", scene);
		sun->SetLocalRotation(Quat::FromEuler(Vec3(52.0f, 37.0f, 11.0f)));
		sun->AddComponent<Light>()->SetIntensity(1.0f);
		SceneSystem::EndFrame();

		ViewTargetId target = RenderSystem::CreateViewTarget({128, 128});
		ASSERT_NE(target, INVALID_VIEW_TARGET);
		ViewDesc desc;
		desc.view = Mat4::LookAt(Vec3(-4.0f, 9.0f, -10.0f), Vec3(1.0f, 0.0f, 0.0f));
		desc.projection = Mat4::PerspectiveReverseZ(60.0f, 1.0f, 0.1f, 100.0f);
		desc.cameraPosition = Vec3(-4.0f, 9.0f, -10.0f);
		desc.clearColor = Color::Black();
		RenderSystem::SubmitView(target, desc);
		RenderSystem::RenderAll();
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		const bool read = RenderSystem::ReadbackViewTarget(target, pixels, size);
		const u32 shadowLights = RenderSystem::GetStats().shadowLights;
		RenderSystem::DestroyViewTarget(target);
		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
		ASSERT_TRUE(read);
		ASSERT_GT(shadowLights, 0u);
		for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
			const u8 shadow = pixels[i];
			const u8 attenuation = pixels[i + 1];
			if (std::abs(static_cast<i32>(pixels[i + 2]) - 137) > 3) {
				continue;
			}
			tally.lit += shadow > 250 ? 1u : 0u;
			tally.shadowed += shadow < 128 ? 1u : 0u;
			tally.disagreeing += std::abs(static_cast<i32>(shadow) - static_cast<i32>(attenuation)) > 2 ? 1u : 0u;
		}
	}

} // anonymous namespace

TEST_F(MaterialReadbackFixture, CustomLightingReadsInputsApart) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	constexpr const char* SURFACE = "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
									"\tsurface.baseColor = float3(0.0, 0.0, 0.0);\n"
									"\tsurface.reflectance = 0.0;\n"
									"}\n";
	WriteProjectFile("ReadsUV.hlsl", str("#define SHALLTEAR_CUSTOM_LIGHTING\n"
										 "#pragma shalltear_template lit\n"
										 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
										 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
										 "\treturn float3(input.uv, 0.0) * light.color * light.attenuation;\n"
										 "}\n") +
										 SURFACE);
	WriteProjectFile("ShadowApart.hlsl", str("#define SHALLTEAR_CUSTOM_LIGHTING\n"
											 "#pragma shalltear_template lit\n"
											 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
											 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
											 "\treturn float3(light.shadow, light.attenuation, 0.25);\n"
											 "}\n") +
											 SURFACE);
	Asset* readsUV = AssetDatabase::LoadAsset(Path("project://ReadsUV.hlsl"));
	Asset* shadowApart = AssetDatabase::LoadAsset(Path("project://ShadowApart.hlsl"));
	ASSERT_NE(readsUV, nullptr);
	ASSERT_NE(shadowApart, nullptr);

	MaterialAsset* uvMaterial = LoadMaterial("ReadsUV.mat", readsUV->assetId, "");
	ASSERT_NE(uvMaterial, nullptr);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(uvMaterial, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 expected = ExpectedTexel(Vec3(0.5f, 0.5f, 0.0f) * stage.lightColor);
	const Vec3 measured = CenterTexel(pixels, size);
	EXPECT_NEAR(measured.x, expected.x, 6.0f);
	EXPECT_NEAR(measured.y, expected.y, 6.0f);
	EXPECT_NEAR(measured.z, 0.0f, 1.5f);
	TearDownStage(stage);

	MaterialAsset* shadowMaterial = LoadMaterial("ShadowApart.mat", shadowApart->assetId, "");
	ASSERT_NE(shadowMaterial, nullptr);
	ShadowTally tally;
	TallyShadow(shadowMaterial, nullptr, tally);
	if (HasFatalFailure()) {
		return;
	}
	EXPECT_GT(tally.shadowed, 50u);
	EXPECT_GT(tally.lit, 50u);
	EXPECT_EQ(tally.disagreeing, 0u);
}

TEST_F(MaterialReadbackFixture, ACustomLightingSurfaceCastsItsShadow) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Caster.hlsl",
					 "#define SHALLTEAR_CUSTOM_LIGHTING\n"
					 "#pragma shalltear_template lit\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
					 "\treturn ShalltearStandardLighting(surface, normalWS, light, input.viewDirectionWS) * 0.5;\n"
					 "}\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {}\n");
	WriteProjectFile("ShadowApart.hlsl", "#define SHALLTEAR_CUSTOM_LIGHTING\n"
										 "#pragma shalltear_template lit\n"
										 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
										 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
										 "\treturn float3(light.shadow, light.attenuation, 0.25);\n"
										 "}\n"
										 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
										 "\tsurface.baseColor = float3(0.0, 0.0, 0.0);\n"
										 "\tsurface.reflectance = 0.0;\n"
										 "}\n");
	Asset* caster = AssetDatabase::LoadAsset(Path("project://Caster.hlsl"));
	Asset* shadowApart = AssetDatabase::LoadAsset(Path("project://ShadowApart.hlsl"));
	ASSERT_NE(caster, nullptr);
	ASSERT_NE(shadowApart, nullptr);
	MaterialAsset* casterMaterial = LoadMaterial("Caster.mat", caster->assetId, "");
	MaterialAsset* shadowMaterial = LoadMaterial("ShadowApart.mat", shadowApart->assetId, "");
	ASSERT_NE(casterMaterial, nullptr);
	ASSERT_NE(shadowMaterial, nullptr);
	ShadowTally tally;
	TallyShadow(shadowMaterial, casterMaterial, tally);
	if (HasFatalFailure()) {
		return;
	}
	EXPECT_GT(tally.shadowed, 50u);
	EXPECT_GT(tally.lit, 50u);
}

namespace {

	str TerminatorShader(const char* surfaceLines) {
		return str("#define SHALLTEAR_CUSTOM_LIGHTING\n"
				   "#pragma shalltear_template lit\n"
				   "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
				   "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
				   "\tfloat3 facet = normalize(cross(ddx(input.positionWS), ddy(input.positionWS)));\n"
				   "\tfloat facing = dot(facet, light.direction) * sign(dot(facet, input.normalWS));\n"
				   "\treturn float3(light.shadow, facing > 0.005 ? 1.0 : (facing < 0.0 ? 0.0 : 0.5), 0.25);\n"
				   "}\n"
				   "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
				   "\tsurface.baseColor = float3(0.0, 0.0, 0.0);\n"
				   "\tsurface.reflectance = 0.0;\n") +
			   surfaceLines + "}\n";
	}

	void RenderTerminator(MaterialAsset* material, Vector<u8>& pixels) {
		Asset* sphere = AssetDatabase::LoadAsset(Path("engine://Meshes/Sphere.primitive"));
		ASSERT_NE(sphere, nullptr);
		Handle<Scene> scene = SceneSystem::CreateScene("Terminator");
		Handle<SceneObject> body = SceneSystem::CreateObject("Sphere", scene);
		body->SetLocalScale(Vec3(2.0f, 2.0f, 2.0f));
		Handle<MeshRenderer> renderer = body->AddComponent<MeshRenderer>();
		renderer->SetMesh(Handle<MeshAsset>(static_cast<MeshAsset*>(sphere)));
		renderer->SetMaterial(0, Handle<MaterialAsset>(material));
		Handle<SceneObject> sun = SceneSystem::CreateObject("Sun", scene);
		sun->SetLocalRotation(Quat::FromEuler(Vec3(30.0f, 90.0f, 0.0f)));
		sun->AddComponent<Light>()->SetIntensity(1.0f);
		SceneSystem::EndFrame();

		ViewTargetId target = RenderSystem::CreateViewTarget({512, 512});
		ASSERT_NE(target, INVALID_VIEW_TARGET);
		ViewDesc desc;
		desc.view = Mat4::LookAt(Vec3(0.0f, 0.5f, 3.0f), Vec3(0.0f, 0.0f, 0.0f));
		desc.projection = Mat4::PerspectiveReverseZ(60.0f, 1.0f, 0.1f, 100.0f);
		desc.cameraPosition = Vec3(0.0f, 0.5f, 3.0f);
		desc.clearColor = Color::Black();
		RenderSystem::SubmitView(target, desc);
		RenderSystem::RenderAll();
		Vec2I size{};
		const bool read = RenderSystem::ReadbackViewTarget(target, pixels, size);
		const u32 shadowLights = RenderSystem::GetStats().shadowLights;
		RenderSystem::DestroyViewTarget(target);
		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
		ASSERT_TRUE(read);
		ASSERT_GT(shadowLights, 0u);
	}

	bool TerminatorDrew(const Vector<u8>& pixels, size_t i) {
		return std::abs(static_cast<i32>(pixels[i + 2]) - 137) <= 3;
	}

} // anonymous namespace

TEST_F(MaterialReadbackFixture, SphereTerminatorHasNoAcne) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Terminator.hlsl", TerminatorShader(""));
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Terminator.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("Terminator.mat", shader->assetId, "");
	ASSERT_NE(material, nullptr);
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	RenderTerminator(material, pixels);
	if (HasFatalFailure()) {
		return;
	}
	u32 facing = 0;
	u32 acne = 0;
	u32 averted = 0;
	u32 shadowed = 0;
	for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
		if (!TerminatorDrew(pixels, i)) {
			continue;
		}
		if (pixels[i + 1] > 200) {
			++facing;
			acne += pixels[i] < 249 ? 1u : 0u;
		} else if (pixels[i + 1] < 50) {
			++averted;
			shadowed += pixels[i] < 128 ? 1u : 0u;
		}
	}
	EXPECT_GT(facing, 10000u);
	EXPECT_GT(averted, 10000u);
	EXPECT_GT(shadowed, averted / 2);
	EXPECT_EQ(acne, 0u) << facing;
}

TEST_F(MaterialReadbackFixture, ShadowIgnoresNormalMap) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Flat.hlsl", TerminatorShader(""));
	WriteProjectFile("Bumped.hlsl", TerminatorShader("\tsurface.normal = normalize(float3(0.35 * sin(input.uv.x * 900.0), 0.35 * cos(input.uv.y * 700.0), 1.0));\n"));
	Asset* flat = AssetDatabase::LoadAsset(Path("project://Flat.hlsl"));
	Asset* bumped = AssetDatabase::LoadAsset(Path("project://Bumped.hlsl"));
	ASSERT_NE(flat, nullptr);
	ASSERT_NE(bumped, nullptr);
	MaterialAsset* flatMaterial = LoadMaterial("Flat.mat", flat->assetId, "");
	MaterialAsset* bumpedMaterial = LoadMaterial("Bumped.mat", bumped->assetId, "");
	ASSERT_NE(flatMaterial, nullptr);
	ASSERT_NE(bumpedMaterial, nullptr);
	Vector<u8> flatPixels{GetMemoryResource(MemTag::Graphics)};
	Vector<u8> bumpedPixels{GetMemoryResource(MemTag::Graphics)};
	RenderTerminator(flatMaterial, flatPixels);
	if (HasFatalFailure()) {
		return;
	}
	RenderTerminator(bumpedMaterial, bumpedPixels);
	if (HasFatalFailure()) {
		return;
	}
	ASSERT_EQ(flatPixels.size(), bumpedPixels.size());
	u32 drawn = 0;
	u32 shadowed = 0;
	u32 lit = 0;
	u32 differing = 0;
	for (size_t i = 0; i + 3 < flatPixels.size(); i += 4) {
		if (!TerminatorDrew(flatPixels, i)) {
			continue;
		}
		++drawn;
		shadowed += flatPixels[i] < 128 ? 1u : 0u;
		lit += flatPixels[i] > 250 ? 1u : 0u;
		differing += std::abs(static_cast<i32>(flatPixels[i]) - static_cast<i32>(bumpedPixels[i])) > 1 ? 1u : 0u;
	}
	EXPECT_GT(drawn, 20000u);
	EXPECT_GT(shadowed, 10000u);
	EXPECT_GT(lit, 10000u);
	EXPECT_EQ(differing, 0u);
}

namespace {

	using TestImages::FLIPBOOK_CELLS;
	using TestImages::WriteFlipbook;
	using TestImages::WriteTGA;

	str SlotsOfEveryDimension(const char* pragmas, const char* channels) {
		return str("#pragma shalltear_template unlit\n") + pragmas +
			   "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
			   "[[vk::binding(1, 1)]] Texture2DArray layers;\n"
			   "[[vk::binding(2, 1)]] SamplerState layersSampler;\n"
			   "[[vk::binding(3, 1)]] Texture3D volume;\n"
			   "[[vk::binding(4, 1)]] SamplerState volumeSampler;\n"
			   "[[vk::binding(5, 1)]] TextureCube sky;\n"
			   "[[vk::binding(6, 1)]] SamplerState skySampler;\n"
			   "[[vk::binding(7, 1)]] Texture2D flat;\n"
			   "[[vk::binding(8, 1)]] SamplerState flatSampler;\n"
			   "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
			   "\tfloat layer = layers.SampleLevel(layersSampler, float3(0.5, 0.5, 0.0), 0).r;\n"
			   "\tfloat slice = volume.SampleLevel(volumeSampler, float3(0.5, 0.5, 0.5), 0).r;\n"
			   "\tfloat face = sky.SampleLevel(skySampler, float3(0.0, 0.0, 1.0), 0).r;\n"
			   "\tfloat map = flat.SampleLevel(flatSampler, float2(0.5, 0.5), 0).r;\n"
			   "\tsurface.baseColor = " +
			   channels +
			   ";\n"
			   "}\n";
	}

} // anonymous namespace

TEST_F(MaterialReadbackFixture, EmptySlotBindsFallbackOfDimension) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Kinds.hlsl", SlotsOfEveryDimension("", "float3(layer, slice, face)"));
	WriteProjectFile("Declared.hlsl", SlotsOfEveryDimension("#pragma shalltear_default volume black\n", "float3(layer, slice + 0.25, face)"));
	Asset* kinds = AssetDatabase::LoadAsset(Path("project://Kinds.hlsl"));
	Asset* declared = AssetDatabase::LoadAsset(Path("project://Declared.hlsl"));
	ASSERT_NE(kinds, nullptr);
	ASSERT_NE(declared, nullptr);
	EXPECT_EQ(static_cast<ShaderAsset*>(kinds)->GetTextureSlotDimension("layers"), RHI::TextureDimension::Texture2DArray);
	EXPECT_EQ(static_cast<ShaderAsset*>(kinds)->GetTextureSlotDimension("volume"), RHI::TextureDimension::Texture3D);
	EXPECT_EQ(static_cast<ShaderAsset*>(kinds)->GetTextureSlotDimension("sky"), RHI::TextureDimension::Cube);
	EXPECT_EQ(static_cast<ShaderAsset*>(kinds)->GetTextureSlotDimension("flat"), RHI::TextureDimension::Texture2D);

	const f32 gray = 128.0f / 255.0f;
	struct Row {
		const char* material;
		const Guid& shader;
		Vec3 linear;
	};
	const Row rows[] = {
		{"Kinds.mat", kinds->assetId, Vec3(gray, gray, gray)},
		{"Declared.mat", declared->assetId, Vec3(gray, 0.25f, gray)},
	};
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<ShaderDiagnosticSink>();
	LogManager::AddSink(sink);
	ASSERT_TRUE(ArmDiagnosticSink(sink));
	for (const Row& row : rows) {
		MaterialAsset* material = LoadMaterial(row.material, row.shader, "");
		ASSERT_NE(material, nullptr);
		ShadingStage stage;
		ASSERT_TRUE(BuildStage(material, stage));
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		ASSERT_TRUE(RenderStage(stage, pixels, size));
		const Vec3 measured = CenterTexel(pixels, size);
		const Vec3 expected = ExpectedTexel(row.linear);
		EXPECT_NEAR(measured.x, expected.x, 1.5f) << row.material;
		EXPECT_NEAR(measured.y, expected.y, 1.5f) << row.material;
		EXPECT_NEAR(measured.z, expected.z, 1.5f) << row.material;
		TearDownStage(stage);
	}
	LogManager::Terminate();
	const str captured = sink->Joined();
	sink.reset();
	JobSystem::Terminate();
	EXPECT_EQ(captured.find("viewType"), str::npos) << captured;
}

TEST_F(MaterialReadbackFixture, WrongDimensionTextureFallsBack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const u8 red[4] = {255, 0, 0, 255};
	WriteTGA(_root / "Red.tga", red);
	WriteProjectFile("Kinds.hlsl", SlotsOfEveryDimension("", "float3(face, map, 0.0)"));
	Asset* texture = AssetDatabase::LoadAsset(Path("project://Red.tga"));
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Kinds.hlsl"));
	ASSERT_NE(texture, nullptr);
	ASSERT_NE(shader, nullptr);
	const str textures = "\t\"textures\": { \"sky\": \"" + texture->assetId.ToString() + "\", \"flat\": \"" + texture->assetId.ToString() + "\" }";
	MaterialAsset* material = LoadMaterial("Mismatched.mat", shader->assetId, textures);
	ASSERT_NE(material, nullptr);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<ShaderDiagnosticSink>();
	LogManager::AddSink(sink);
	ASSERT_TRUE(ArmDiagnosticSink(sink));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	const bool rendered = RenderStage(stage, pixels, size) && RenderStage(stage, pixels, size);
	LogManager::Terminate();
	const str captured = sink->Joined();
	sink.reset();
	JobSystem::Terminate();
	ASSERT_TRUE(rendered);

	const Vec3 measured = CenterTexel(pixels, size);
	const Vec3 expected = ExpectedTexel(Vec3(128.0f / 255.0f, 1.0f, 0.0f));
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	size_t warnings = 0;
	for (size_t at = captured.find("samples a cube texture"); at != str::npos; at = captured.find("samples a cube texture", at + 1)) {
		++warnings;
	}
	EXPECT_EQ(warnings, 1u) << captured;
	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, MaterialHoldsOneRefPerSlot) {
	const u8 red[4] = {255, 0, 0, 255};
	const u8 green[4] = {0, 255, 0, 255};
	WriteTGA(_root / "Red.tga", red);
	WriteTGA(_root / "Green.tga", green);
	Asset* redMap = AssetDatabase::LoadAsset(Path("project://Red.tga"));
	Asset* greenMap = AssetDatabase::LoadAsset(Path("project://Green.tga"));
	ASSERT_NE(redMap, nullptr);
	ASSERT_NE(greenMap, nullptr);
	const int redHeld = redMap->GetRefCount();
	const int greenHeld = greenMap->GetRefCount();
	const StringHash slot("baseColorTexture");

	{
		MaterialAsset draft;
		draft.SetTexture("baseColorTexture", redMap->assetId);
		EXPECT_FALSE(draft.GetTexture(slot).IsValid());
		EXPECT_EQ(redMap->GetRefCount(), redHeld);
	}

	const str textures = "\t\"textures\": { \"baseColorTexture\": \"" + redMap->assetId.ToString() + "\" }";
	MaterialAsset* material = LoadMaterial("Held.mat", Guid{}, textures);
	ASSERT_NE(material, nullptr);
	ASSERT_EQ(material->GetLoadState(), AssetLoadState::Loaded);
	ASSERT_EQ(material->GetTexture(slot).GetInstanceId(), redMap->GetInstanceId());
	ASSERT_EQ(redMap->GetRefCount(), redHeld + 1);

	material->SetTexture("baseColorTexture", redMap->assetId);
	EXPECT_EQ(material->GetTexture(slot).GetInstanceId(), redMap->GetInstanceId());
	EXPECT_EQ(redMap->GetRefCount(), redHeld + 1);

	material->SetTexture("baseColorTexture", greenMap->assetId);
	EXPECT_EQ(material->GetTexture(slot).GetInstanceId(), greenMap->GetInstanceId());
	EXPECT_EQ(redMap->GetRefCount(), redHeld);
	EXPECT_EQ(greenMap->GetRefCount(), greenHeld + 1);

	material->SetTexture("baseColorTexture", Guid{});
	EXPECT_FALSE(material->GetTexture(slot).IsValid());
	EXPECT_EQ(greenMap->GetRefCount(), greenHeld);

	material->SetTexture("baseColorTexture", greenMap->assetId);
	EXPECT_EQ(material->GetTexture(slot).GetInstanceId(), greenMap->GetInstanceId());
	EXPECT_EQ(greenMap->GetRefCount(), greenHeld + 1);
	AssetDatabase::UnloadAsset(material);
	AssetDatabase::CollectGarbage();
	EXPECT_EQ(greenMap->GetRefCount(), greenHeld);

	AssetDatabase::UnloadAsset(greenMap);
	AssetDatabase::UnloadAsset(redMap);
}

TEST_F(MaterialReadbackFixture, AssignedTextureDraws) {
	if (!RenderSystem::IsRenderingAvailable() || !CompilerAvailable()) {
		GTEST_SKIP() << "no Vulkan device or no shader compiler";
	}
	const u8 red[4] = {255, 0, 0, 255};
	const u8 green[4] = {0, 255, 0, 255};
	WriteTGA(_root / "Red.tga", red);
	WriteTGA(_root / "Green.tga", green);
	const Guid redMap = AssetDatabase::EnsureAssetIdentity(Path("project://Red.tga"));
	const Guid greenMap = AssetDatabase::EnsureAssetIdentity(Path("project://Green.tga"));
	ASSERT_TRUE(redMap.IsValid() && greenMap.IsValid());
	Asset* unlit = AssetDatabase::LoadAsset(Path("engine://Shaders/Unlit.hlsl"));
	ASSERT_NE(unlit, nullptr);
	MaterialAsset* material = LoadMaterial("Assigned.mat", unlit->assetId, "");
	ASSERT_NE(material, nullptr);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};

	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 empty = CenterTexel(pixels, size);
	ASSERT_GT(empty.x, 200.0f);
	ASSERT_GT(empty.y, 200.0f);

	material->SetTexture("baseColorTexture", redMap);
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 assigned = CenterTexel(pixels, size);
	EXPECT_GT(assigned.x, 200.0f);
	EXPECT_LT(assigned.y, 40.0f);

	material->SetTexture("baseColorTexture", greenMap);
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 replaced = CenterTexel(pixels, size);
	EXPECT_LT(replaced.x, 40.0f);
	EXPECT_GT(replaced.y, 200.0f);

	material->SetTexture("baseColorTexture", Guid{});
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 cleared = CenterTexel(pixels, size);
	EXPECT_GT(cleared.x, 200.0f);
	EXPECT_GT(cleared.y, 200.0f);

	TearDownStage(stage);
	AssetDatabase::UnloadAsset(unlit);
}

TEST(TextureFlipbookTest, FlipbookSlicesInReadingOrder) {
	const std::filesystem::path dir =
		std::filesystem::temp_directory_path() / std::format("slt_flipbook_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	std::filesystem::create_directories(dir);
	WriteFlipbook(dir / "Flip.tga");
	const Path path((dir / "Flip.tga").generic_string());

	for (TextureShape shape : {TextureShape::Texture2DArray, TextureShape::Texture3D}) {
		TextureAsset asset;
		asset.settings.shape = shape;
		asset.settings.columns = 2;
		asset.settings.rows = 2;
		ASSERT_TRUE(asset.Import(path));
		EXPECT_EQ(asset.GetDimension(), shape == TextureShape::Texture2DArray ? RHI::TextureDimension::Texture2DArray : RHI::TextureDimension::Texture3D);
		ASSERT_EQ(asset.slices, 4u);
		EXPECT_EQ(asset.size.x, 2);
		EXPECT_EQ(asset.size.y, 2);
		ASSERT_EQ(asset.pixels.size(), 4u * 2 * 2 * 4);
		for (u32 slice = 0; slice < 4; ++slice) {
			for (u32 texel = 0; texel < 4; ++texel) {
				EXPECT_EQ(std::memcmp(&asset.pixels[(slice * 4 + texel) * 4], FLIPBOOK_CELLS[slice], 4), 0) << "slice " << slice << " texel " << texel;
			}
		}
	}

	TextureAsset single;
	single.settings.shape = TextureShape::Texture2DArray;
	ASSERT_TRUE(single.Import(path));
	EXPECT_EQ(single.slices, 1u);
	EXPECT_EQ(single.GetDimension(), RHI::TextureDimension::Texture2DArray);

	TextureAsset uneven;
	uneven.settings.shape = TextureShape::Texture2DArray;
	uneven.settings.columns = 3;
	EXPECT_FALSE(uneven.Import(path));
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

TEST(TextureFlipbookTest, AFlipbookCooksToTheSameSlices) {
	const std::filesystem::path dir =
		std::filesystem::temp_directory_path() / std::format("slt_flipcook_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	std::filesystem::create_directories(dir);
	WriteFlipbook(dir / "Flip.tga");
	TextureAsset imported;
	imported.settings.shape = TextureShape::Texture3D;
	imported.settings.columns = 2;
	imported.settings.rows = 2;
	ASSERT_TRUE(imported.Import(Path((dir / "Flip.tga").generic_string())));
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);

	Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
	Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
	PakStringPool pool;
	CookWriter descriptor(descriptorBytes, pool);
	CookWriter payload(payloadBytes, pool);
	ASSERT_TRUE(imported.Cook(descriptor, payload));
	Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
	PakWriteStringBlob(stringBytes, pool.Texts());
	PakStringTable table;
	ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));
	TextureAsset restored;
	CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
	CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
	ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader));
	EXPECT_EQ(restored.GetDimension(), RHI::TextureDimension::Texture3D);
	EXPECT_EQ(restored.slices, 4u);
	EXPECT_EQ(restored.size.x, 2);
	EXPECT_TRUE(restored.pixels == imported.pixels);
}

TEST_F(MaterialReadbackFixture, FlipbookSamplesNamedCell) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	auto importAs = [](const char* file, const char* shape) {
		const Path path(str("project://") + file);
		Asset* minted = AssetDatabase::LoadAsset(path);
		if (!minted) {
			return Guid{};
		}
		const Guid guid = minted->assetId;
		AssetDatabase::UnloadAsset(minted);
		JSONValue settings = JSONValue::MakeObject();
		settings.AddMember("shape", JSONValue::MakeString(shape));
		settings.AddMember("columns", JSONValue::MakeNumber(2.0));
		settings.AddMember("rows", JSONValue::MakeNumber(2.0));
		return AssetDatabase::WriteImporterSettings(path, settings) && AssetDatabase::ReimportAsset(path) ? guid : Guid{};
	};
	WriteFlipbook(_root / "Layers.tga");
	WriteFlipbook(_root / "Volume.tga");
	const Guid layers = importAs("Layers.tga", "2darray");
	const Guid volume = importAs("Volume.tga", "3d");
	ASSERT_TRUE(layers.IsValid());
	ASSERT_TRUE(volume.IsValid());

	WriteProjectFile("Cells.hlsl", "#pragma shalltear_template unlit\n"
								   "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
								   "[[vk::binding(1, 1)]] Texture2DArray layers;\n"
								   "[[vk::binding(2, 1)]] SamplerState layersSampler;\n"
								   "[[vk::binding(3, 1)]] Texture3D volume;\n"
								   "[[vk::binding(4, 1)]] SamplerState volumeSampler;\n"
								   "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
								   "\tfloat3 layer = layers.SampleLevel(layersSampler, float3(0.5, 0.5, 2.0), 0).rgb;\n"
								   "\tfloat3 slice = volume.SampleLevel(volumeSampler, float3(0.5, 0.5, 0.625), 0).rgb;\n"
								   "\tsurface.baseColor = float3(layer.r, layer.g, slice.b);\n"
								   "}\n");
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Cells.hlsl"));
	ASSERT_NE(shader, nullptr);
	const str textures = "\t\"textures\": { \"layers\": \"" + layers.ToString() + "\", \"volume\": \"" + volume.ToString() + "\" }";
	MaterialAsset* material = LoadMaterial("Cells.mat", shader->assetId, textures);
	ASSERT_NE(material, nullptr);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 measured = CenterTexel(pixels, size);
	EXPECT_NEAR(measured.x, FLIPBOOK_CELLS[2][0], 2.0f);
	EXPECT_NEAR(measured.y, FLIPBOOK_CELLS[2][1], 2.0f);
	EXPECT_NEAR(measured.z, FLIPBOOK_CELLS[2][2], 2.0f);
	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, MirroredDrawTiltsNormalAlongSameBitangent) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Tilt.hlsl",
					 "#define SHALLTEAR_CUSTOM_LIGHTING\n"
					 "#pragma shalltear_template lit\n"
					 "#pragma shalltear_cull off\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "float3 ShalltearLightingFunction(ShalltearSurfaceInput input, ShalltearSurface surface, float3 normalWS, ShalltearLightSample light) {\n"
					 "\treturn normalWS * 0.5 + 0.5;\n"
					 "}\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = float3(0.0, 0.0, 0.0);\n"
					 "\tsurface.reflectance = 0.0;\n"
					 "\tsurface.normal = float3(0.0, 0.6, 0.8);\n"
					 "}\n");
	Asset* tiltShader = AssetDatabase::LoadAsset(Path("project://Tilt.hlsl"));
	ASSERT_NE(tiltShader, nullptr);
	MaterialAsset* material = LoadMaterial("Tilt.mat", tiltShader->assetId, {});
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 plain = CenterTexel(pixels, size);
	if (plain.x + plain.y + plain.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}
	ASSERT_GT(std::fabs(plain.y - ExpectedTexel(Vec3(0.5f, 0.5f, 0.5f)).y), 30.0f);

	stage.quad->SetLocalScale(Vec3(-1.0f, 1.0f, 1.0f));
	SceneSystem::EndFrame();
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 mirrored = CenterTexel(pixels, size);
	EXPECT_NEAR(mirrored.x, plain.x, 1.5f);
	EXPECT_NEAR(mirrored.y, plain.y, 1.5f);
	EXPECT_NEAR(mirrored.z, plain.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, SurfaceInputCarriesDrawsMatrixAndInverse) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Placed.hlsl",
					 "#pragma shalltear_template unlit\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tfloat4x4 product = mul(input.worldToObject, input.objectToWorld);\n"
					 "\tfloat error = 0.0;\n"
					 "\t[unroll] for (uint row = 0; row < 4; ++row) {\n"
					 "\t\terror += dot(abs(product[row] - float4(row == 0, row == 1, row == 2, row == 3)), 1.0);\n"
					 "\t}\n"
					 "\tsurface.baseColor = float3(input.objectToWorld._m03 + 0.5, length(input.objectToWorld._m00_m10_m20) / 3.0,\n"
					 "\t\t\t\t\t\t\t\tsaturate(1.0 - error * 50.0));\n"
					 "\tsurface.alpha = 1.0;\n"
					 "}\n");
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Placed.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("Placed.mat", shader->assetId, {});
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	stage.quad->SetLocalPosition(Vec3(0.1f, 0.2f, 0.0f));
	stage.quad->SetLocalRotation(Quat::AngleAxis(30.0f, Vec3(0.0f, 0.0f, 1.0f)));
	stage.quad->SetLocalScale(Vec3(1.5f, 1.2f, 1.0f));
	SceneSystem::EndFrame();

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 measured = CenterTexel(pixels, size);
	const Vec3 expected = ExpectedTexel(Vec3(0.6f, 0.5f, 1.0f));
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, ASurfaceKnowsWhichFaceItShades) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Faces.hlsl",
					 "#pragma shalltear_template unlit\n"
					 "#pragma shalltear_cull off\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = input.isFrontFace ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);\n"
					 "\tsurface.alpha = 1.0;\n"
					 "}\n");
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Faces.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("Faces.mat", shader->assetId, {});
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 front = CenterTexel(pixels, size);
	const Vec3 red = ExpectedTexel(Vec3(1.0f, 0.0f, 0.0f));
	EXPECT_NEAR(front.x, red.x, 1.5f);
	EXPECT_NEAR(front.y, red.y, 1.5f);

	stage.quad->SetLocalRotation(Quat::AngleAxis(180.0f, Vec3(0.0f, 1.0f, 0.0f)));
	SceneSystem::EndFrame();
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 back = CenterTexel(pixels, size);
	const Vec3 green = ExpectedTexel(Vec3(0.0f, 1.0f, 0.0f));
	EXPECT_NEAR(back.x, green.x, 1.5f);
	EXPECT_NEAR(back.y, green.y, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, MirroredQuadShadesFrontFace) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("CulledFaces.hlsl",
					 "#pragma shalltear_template unlit\n"
					 "#pragma shalltear_cull back\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = input.isFrontFace ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);\n"
					 "\tsurface.alpha = 1.0;\n"
					 "}\n");
	Asset* shader = AssetDatabase::LoadAsset(Path("project://CulledFaces.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("CulledFaces.mat", shader->assetId, {});
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	const Vec3 red = ExpectedTexel(Vec3(1.0f, 0.0f, 0.0f));

	struct Row {
		Vec3 scale;
		f32 yawDeg;
		bool drawn;
	};
	const Row rows[] = {
		{Vec3(1.0f, 1.0f, 1.0f), 0.0f, true},
		{Vec3(-1.0f, 1.0f, 1.0f), 0.0f, true},
		{Vec3(-1.0f, 1.0f, 1.0f), 180.0f, false},
		{Vec3(1.0f, 1.0f, -1.0f), 0.0f, false},
		{Vec3(1.0f, 1.0f, -1.0f), 180.0f, true},
		{Vec3(-1.0f, -1.0f, 1.0f), 0.0f, true},
		{Vec3(-1.0f, -1.0f, 1.0f), 180.0f, false},
	};
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	for (u32 i = 0; i < std::size(rows); ++i) {
		stage.quad->SetLocalScale(rows[i].scale);
		stage.quad->SetLocalRotation(Quat::AngleAxis(rows[i].yawDeg, Vec3(0.0f, 1.0f, 0.0f)));
		SceneSystem::EndFrame();
		ASSERT_TRUE(RenderStage(stage, pixels, size));
		const Vec3 center = CenterTexel(pixels, size);
		const Vec3 expected = rows[i].drawn ? red : Vec3::Zero();
		EXPECT_NEAR(center.x, expected.x, 1.5f) << i;
		EXPECT_NEAR(center.y, expected.y, 1.5f) << i;
	}

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, ViewRecordCarriesCameraData) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	FrameTime frame;
	frame.time = 1.25;
	frame.deltaTime = 0.02f;
	Time::FrameScope clock(frame);

	const f32 halfExtent = std::tan(30.0f * Math::DEG_TO_RAD);
	const str source = std::format(
		"#pragma shalltear_template unlit\n"
		"[[vk::binding(0, 1)]] cbuffer ShalltearMaterial {{ float4 unused; }};\n"
		"float Relative(float a, float b) {{ return abs(a - b) / max(abs(b), 1.0); }}\n"
		"void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {{\n"
		"\tfloat4 projection = ShalltearProjectionParameters();\n"
		"\tfloat4 screen = ShalltearScreenParameters();\n"
		"\tfloat facts = Relative(projection.x, 0.1) + Relative(projection.y, 100.0) + projection.z\n"
		"\t\t+ Relative(screen.x, 64.0) + Relative(screen.y, 64.0) + Relative(screen.z, {0}) + Relative(screen.w, {0})\n"
		"\t\t+ Relative(input.positionSS.x, 32.5) + Relative(input.positionSS.y, 32.5);\n"
		"\tfloat3 eye = mul(ShalltearInverseViewMatrix(), float4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
		"\tfloat3 eyeInView = mul(ShalltearViewMatrix(), float4(eye, 1.0)).xyz;\n"
		"\tfloat4 clip = mul(sltViewProjection, float4(input.positionWS, 1.0));\n"
		"\tfloat4 back = mul(ShalltearInverseViewProjectionMatrix(), clip);\n"
		"\tfloat4x4 identity = mul(ShalltearProjectionMatrix(), ShalltearInverseProjectionMatrix());\n"
		"\tfloat matrices = length(eye - float3(0.0, 0.0, 3.0)) + length(eyeInView) + length(back.xyz / back.w - input.positionWS)\n"
		"\t\t+ abs(identity._m00 - 1.0) + abs(identity._m11 - 1.0) + abs(identity._m22 - 1.0) + abs(identity._m33 - 1.0)\n"
		"\t\t+ abs(identity._m23) + abs(identity._m32);\n"
		"\tfloat4 time = ShalltearTimeParameters();\n"
		"\tfloat clock = abs(time.x - 1.25) + abs(time.y - {1}) + abs(time.z - {2}) + abs(time.w - 0.02);\n"
		"\tsurface.baseColor = saturate(1.0 - float3(facts, matrices, clock) * 50.0);\n"
		"\tsurface.alpha = 1.0;\n"
		"}}\n",
		halfExtent, std::sin(1.25), std::cos(1.25));
	WriteProjectFile("Viewer.hlsl", source);
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Viewer.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("Viewer.mat", shader->assetId, {});
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));

	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	ASSERT_EQ(size.x, 64);
	const Vec3 measured = CenterTexel(pixels, size);
	EXPECT_GT(measured.x, 250.0f);
	EXPECT_GT(measured.y, 250.0f);
	EXPECT_GT(measured.z, 250.0f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, OrthographicViewDirectionIsCamerasBackwardAxis) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Looking.hlsl",
					 "#pragma shalltear_template unlit\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "\tsurface.baseColor = input.viewDirectionWS * 0.5 + 0.5;\n"
					 "\tsurface.alpha = 1.0;\n"
					 "}\n");
	Asset* shader = AssetDatabase::LoadAsset(Path("project://Looking.hlsl"));
	ASSERT_NE(shader, nullptr);
	MaterialAsset* material = LoadMaterial("Looking.mat", shader->assetId, {});
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));

	ViewDesc desc = ShadingTest::QuadView();
	desc.projection = Mat4::OrthographicReverseZ(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
	RenderSystem::SubmitView(stage.target, desc);
	RenderSystem::RenderAll();
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderSystem::ReadbackViewTarget(stage.target, pixels, size));
	const size_t index = (static_cast<size_t>(size.y / 2) * size.x + 17) * 4;
	const Vec3 measured(pixels[index], pixels[index + 1], pixels[index + 2]);
	ASSERT_GT(measured.x + measured.y + measured.z, 0.0f);
	const Vec3 toward = -Vec3(desc.view(2, 0), desc.view(2, 1), desc.view(2, 2)).Normalized();
	const Vec3 expected = ExpectedTexel(toward * 0.5f + Vec3(0.5f, 0.5f, 0.5f));
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, EmptyNormalSlotIsFlat) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* litShader = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(litShader, nullptr);

	CPUSurface surface;
	surface.baseColor = Vec3(0.8f, 0.8f, 0.8f);
	surface.roughness = 0.25f;
	MaterialAsset* material = LoadMaterial("Normals.mat", litShader->assetId,
										   "\t\"colors\": { \"baseColor\": [0.8, 0.8, 0.8, 1] },\n"
										   "\t\"floats\": { \"metallic\": 0, \"roughness\": 0.25, \"reflectance\": 0.5, \"normalScale\": 1, \"occlusionStrength\": 1 },\n"
										   "\t\"vec3s\": { \"emissive\": [0, 0, 0] }");
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	Vec3 measured = CenterTexel(pixels, size);
	if (measured.x + measured.y + measured.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}

	const Vec3 view = Eye.Normalized();
	auto shadeWith = [&](Vec3 normal) {
		Vec3 linear = Vec3(VIEW_AMBIENT, VIEW_AMBIENT, VIEW_AMBIENT) * DiffuseColor(surface);
		linear += StandardLighting(surface, normal, stage.lightDirection, stage.lightColor, 1.0f, view);
		return ExpectedTexel(linear);
	};

	const Vec3 flatNormal(0.0f, 0.0f, 1.0f);
	const Vec3 whiteWouldGive = (Vec3(-1.0f, 0.0f, 0.0f) * 1.0f + Vec3(0.0f, -1.0f, 0.0f) * 1.0f).Normalized();

	Vec3 expected = shadeWith(flatNormal);
	Vec3 wrong = shadeWith(whiteWouldGive);
	ASSERT_GT((expected - wrong).Length(), 8.0f);

	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, KeywordToggleSwapsVariant) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("KeywordProbe.hlsl",
					 "#pragma shalltear_template unlit\n"
					 "#pragma shalltear_keyword PROBE_GREEN tint\n"
					 "#pragma shalltear_default tint 0\n"
					 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float tint; };\n"
					 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
					 "#ifdef PROBE_GREEN\n"
					 "\tsurface.baseColor = float3(0.0, 0.75, 0.0);\n"
					 "#else\n"
					 "\tsurface.baseColor = float3(0.75, 0.0, 0.0);\n"
					 "#endif\n"
					 "}\n");
	Asset* probeShader = AssetDatabase::LoadAsset(Path("project://KeywordProbe.hlsl"));
	ASSERT_NE(probeShader, nullptr);
	ShaderAsset* shader = static_cast<ShaderAsset*>(probeShader);

	MaterialAsset* material = LoadMaterial("Keyword.mat", probeShader->assetId, str());
	ASSERT_NE(material, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	Vec3 off = CenterTexel(pixels, size);
	if (off.x + off.y + off.z == 0.0f) {
		TearDownStage(stage);
		GTEST_SKIP() << "quad didn't draw";
	}
	EXPECT_GT(off.x, off.y);

	const str enabled[] = {str("PROBE_GREEN")};
	material->SetKeywords({enabled, 1});
	shader->GetOrCompileVariant({enabled, 1});
	ASSERT_NE(shader->FindVariant(material->keywordHash), SHADER_BASE_VARIANT);

	ASSERT_TRUE(RenderStage(stage, pixels, size));
	Vec3 on = CenterTexel(pixels, size);
	EXPECT_GT(on.y, on.x);
	EXPECT_LT(off.z, 32.0f);
	EXPECT_LT(on.z, 32.0f);

	TearDownStage(stage);
}

TEST_F(MaterialReadbackFixture, EveryBlendModeCompositesAgainstTheDestination) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}

	const Vec3 destination(0.4f, 0.6f, 0.2f);
	const Vec3 source(0.5f, 0.25f, 0.75f);
	constexpr f32 SOURCE_ALPHA = 0.5f;

	struct Row {
		const char* pragma;
		const char* fileStem;
		Vec3 expected;
	};
	const Row rows[] = {
		{"opaque", "BlendOpaque", source},
		{"alpha", "BlendAlpha", source * SOURCE_ALPHA + destination * (1.0f - SOURCE_ALPHA)},
		{"additive", "BlendAdditive", source * SOURCE_ALPHA + destination},
		{"premultiplied", "BlendPremultiplied", source + destination * (1.0f - SOURCE_ALPHA)},
		{"multiply", "BlendMultiply", destination * source},
	};
	static_assert(std::size(rows) == static_cast<size_t>(RHI::BlendMode::Count));

	for (const Row& row : rows) {
		str shaderName = str(row.fileStem) + ".hlsl";
		WriteProjectFile(shaderName.c_str(),
						 str("#pragma shalltear_template unlit\n"
							 "#pragma shalltear_blend ") +
							 row.pragma + "\n" +
							 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
							 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							 "\tsurface.baseColor = baseColor.rgb;\n"
							 "\tsurface.alpha = baseColor.a;\n"
							 "}\n");
		Asset* shaderAsset = AssetDatabase::LoadAsset(Path(str("project://") + shaderName));
		ASSERT_NE(shaderAsset, nullptr) << "blend '" << row.pragma;

		str materialName = str(row.fileStem) + ".mat";
		MaterialAsset* material = LoadMaterial(materialName.c_str(), shaderAsset->assetId,
											   "\t\"colors\": { \"baseColor\": [0.5, 0.25, 0.75, 0.5] }");
		ASSERT_NE(material, nullptr) << "blend '" << row.pragma << "'";

		ShadingStage stage;
		ASSERT_TRUE(BuildStage(material, stage)) << "blend '" << row.pragma << "'";
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		ASSERT_TRUE(RenderStage(stage, pixels, size, Color(destination.x, destination.y, destination.z, 1.0f)))
			<< "blend '" << row.pragma << "'";

		Vec3 measured = CenterTexel(pixels, size);
		Vec3 expected = ExpectedTexel(row.expected);
		Vec3 cleared = ExpectedTexel(destination);
		if ((measured - cleared).Length() < 0.5f) {
			TearDownStage(stage);
			GTEST_SKIP() << "quad didn't draw";
		}
		EXPECT_NEAR(measured.x, expected.x, 1.5f) << "blend '" << row.pragma << "' red";
		EXPECT_NEAR(measured.y, expected.y, 1.5f) << "blend '" << row.pragma << "' green";
		EXPECT_NEAR(measured.z, expected.z, 1.5f) << "blend '" << row.pragma << "' blue";

		TearDownStage(stage);
	}
}

namespace {

	str UnlitProbeSource(const char* blendPragma) {
		return str("#pragma shalltear_template unlit\n"
				   "#pragma shalltear_blend ") +
			   blendPragma + "\n" +
			   "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
			   "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
			   "\tsurface.baseColor = baseColor.rgb;\n"
			   "\tsurface.alpha = baseColor.a;\n"
			   "}\n";
	}

} // anonymous namespace

TEST_F(MaterialReadbackFixture, AlphaClipDropsTexel) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("ClipProbe.hlsl", UnlitProbeSource("opaque"));
	Asset* shaderAsset = AssetDatabase::LoadAsset(Path("project://ClipProbe.hlsl"));
	ASSERT_NE(shaderAsset, nullptr);
	ShaderAsset* shader = static_cast<ShaderAsset*>(shaderAsset);
	ASSERT_NE(std::find(shader->keywords.begin(), shader->keywords.end(), str(ShaderAlphaClipKeyword)),
			  shader->keywords.end());

	const Vec3 baseColor(0.5f, 0.25f, 0.75f);
	const char* colorRow = "\t\"colors\": { \"baseColor\": [0.5, 0.25, 0.75, 0.25] }";

	{
		MaterialAsset* material = LoadMaterial("Unclipped.mat", shaderAsset->assetId, colorRow);
		ASSERT_NE(material, nullptr);
		EXPECT_EQ(shader->FindVariant(material->keywordHash), SHADER_BASE_VARIANT);

		ShadingStage stage;
		ASSERT_TRUE(BuildStage(material, stage));
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		ASSERT_TRUE(RenderStage(stage, pixels, size));
		Vec3 measured = CenterTexel(pixels, size);
		Vec3 expected = ExpectedTexel(baseColor);
		EXPECT_NEAR(measured.x, expected.x, 1.5f);
		EXPECT_NEAR(measured.y, expected.y, 1.5f);
		EXPECT_NEAR(measured.z, expected.z, 1.5f);
		TearDownStage(stage);
	}

	{
		MaterialAsset* material = LoadMaterial("Clipped.mat", shaderAsset->assetId,
											   str(colorRow) + ",\n\t\"alphaClip\": 0.5");
		ASSERT_NE(material, nullptr);
		EXPECT_FLOAT_EQ(material->alphaClipThreshold, 0.5f);
		ASSERT_NE(shader->FindVariant(material->keywordHash), SHADER_BASE_VARIANT);

		ShadingStage stage;
		ASSERT_TRUE(BuildStage(material, stage));
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		ASSERT_TRUE(RenderStage(stage, pixels, size));
		Vec3 measured = CenterTexel(pixels, size);
		EXPECT_NEAR(measured.x, 0.0f, 1.5f);
		EXPECT_NEAR(measured.y, 0.0f, 1.5f);
		EXPECT_NEAR(measured.z, 0.0f, 1.5f);
		TearDownStage(stage);
	}
}

TEST_F(MaterialReadbackFixture, ASortingPriorityReordersTwoBlendedDraws) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("OrderProbe.hlsl", UnlitProbeSource("opaque"));
	Asset* shaderAsset = AssetDatabase::LoadAsset(Path("project://OrderProbe.hlsl"));
	ASSERT_NE(shaderAsset, nullptr);

	const Vec3 nearColor(1.0f, 0.0f, 0.0f);
	const Vec3 farColor(0.0f, 0.0f, 1.0f);
	constexpr f32 SRC_ALPHA = 0.5f;
	MaterialAsset* nearMaterial = LoadMaterial("Near.mat", shaderAsset->assetId,
											   "\t\"colors\": { \"baseColor\": [1, 0, 0, 0.5] },\n\t\"blend\": \"alpha\"");
	MaterialAsset* farMaterial = LoadMaterial("Far.mat", shaderAsset->assetId,
											  "\t\"colors\": { \"baseColor\": [0, 0, 1, 0.5] },\n\t\"blend\": \"alpha\"");
	ASSERT_NE(nearMaterial, nullptr);
	ASSERT_NE(farMaterial, nullptr);
	ASSERT_TRUE(nearMaterial->blend.has_value());
	ASSERT_EQ(*nearMaterial->blend, RHI::BlendMode::Alpha);

	auto over = [](Vec3 source, Vec3 destination) {
		return source * SRC_ALPHA + destination * (1.0f - SRC_ALPHA);
	};
	const Vec3 farLast = ExpectedTexel(over(farColor, over(nearColor, Vec3())));
	const Vec3 nearLast = ExpectedTexel(over(nearColor, over(farColor, Vec3())));
	ASSERT_GT((farLast - nearLast).Length(), 8.0f);

	auto renderBoth = [&]() {
		ShadingStage stage;
		EXPECT_TRUE(BuildStage(nearMaterial, stage));
		EXPECT_TRUE(AddQuad(stage, farMaterial, -1.0f));
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		EXPECT_TRUE(RenderStage(stage, pixels, size));
		Vec3 measured = CenterTexel(pixels, size);
		TearDownStage(stage);
		return measured;
	};

	Vec3 byDepth = renderBoth();
	EXPECT_NEAR(byDepth.x, nearLast.x, 1.5f);
	EXPECT_NEAR(byDepth.y, nearLast.y, 1.5f);
	EXPECT_NEAR(byDepth.z, nearLast.z, 1.5f);

	farMaterial->sortingPriority = 1;
	Vec3 byPriority = renderBoth();
	EXPECT_NEAR(byPriority.x, farLast.x, 1.5f);
	EXPECT_NEAR(byPriority.y, farLast.y, 1.5f);
	EXPECT_NEAR(byPriority.z, farLast.z, 1.5f);
}

TEST_F(MaterialReadbackFixture, TwoBlendsTwoPipelines) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("PipelineProbe.hlsl", UnlitProbeSource("opaque"));
	Asset* shaderAsset = AssetDatabase::LoadAsset(Path("project://PipelineProbe.hlsl"));
	ASSERT_NE(shaderAsset, nullptr);

	MaterialAsset* first = LoadMaterial("PipeA.mat", shaderAsset->assetId,
										"\t\"colors\": { \"baseColor\": [1, 0, 0, 0.5] }");
	MaterialAsset* second = LoadMaterial("PipeB.mat", shaderAsset->assetId,
										 "\t\"colors\": { \"baseColor\": [0, 0, 1, 0.5] }");
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);

	ShadingStage stage;
	ASSERT_TRUE(BuildStage(first, stage));
	ASSERT_TRUE(AddQuad(stage, second, -1.0f));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};

	auto render = [&]() {
		EXPECT_TRUE(RenderStage(stage, pixels, size));
		return RenderSystem::GetStats().livePipelines;
	};

	render();
	const u32 baseline = render();
	EXPECT_EQ(render(), baseline);

	second->blend = RHI::BlendMode::Alpha;
	const u32 twoBlends = render();
	EXPECT_EQ(twoBlends, baseline + 1);
	EXPECT_EQ(render(), twoBlends);

	first->blend = RHI::BlendMode::Alpha;
	EXPECT_EQ(render(), twoBlends);

	TearDownStage(stage);
}

TEST_F(MaterialShadingFixture, TemplatedShadersCarryDepthProgram) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable (run Scripts/fetch_prebuilt.py)";
	}
	for (const char* name : {"engine://Shaders/Lit.hlsl", "engine://Shaders/Unlit.hlsl"}) {
		Asset* asset = AssetDatabase::LoadAsset(Path(name));
		ASSERT_NE(asset, nullptr) << name;
		ShaderAsset* shader = static_cast<ShaderAsset*>(asset);
		ASSERT_FALSE(shader->variants.empty()) << name;
		const ShaderVariant& base = shader->variants[SHADER_BASE_VARIANT];
		EXPECT_TRUE(base.HasProgram(ShaderProgramKind::Forward)) << name;
		EXPECT_TRUE(base.HasProgram(ShaderProgramKind::Depth));
		EXPECT_FALSE(base.depthPixelSpirv.empty()) << name;
		EXPECT_FALSE(base.depthClipPixelSpirv.empty()) << name;
		EXPECT_FALSE(DeclaresAnOutputAtALocation(base.depthClipPixelSpirv)) << name;
		EXPECT_NE(base.pixelSpirv, base.depthPixelSpirv) << name;
	}

	Asset* rawAsset = AssetDatabase::LoadAsset(Path("engine://Shaders/Wireframe.hlsl"));
	ASSERT_NE(rawAsset, nullptr);
	ShaderAsset* raw = static_cast<ShaderAsset*>(rawAsset);
	ASSERT_FALSE(raw->variants.empty());
	EXPECT_EQ(raw->integrator, ShaderTemplate::None);
	EXPECT_FALSE(raw->variants[SHADER_BASE_VARIANT].HasProgram(ShaderProgramKind::Depth));
}

TEST_F(MaterialShadingFixture, SkipOptimizationsCoversBothPrograms) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const str surface = "#pragma shalltear_template lit\n"
						"[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; float roughness; };\n"
						"void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
						"\tfloat3 tint = baseColor.rgb;\n"
						"\tfor (int i = 0; i < 3; ++i) { tint = tint * 0.5 + float3(0.1, 0.2, 0.3); }\n"
						"\tsurface.baseColor = tint;\n"
						"\tsurface.roughness = saturate(roughness * 2.0);\n"
						"}\n";
	WriteProjectFile("Optimized.hlsl", surface);
	WriteProjectFile("Skipping.hlsl", "#pragma shalltear_skip_optimizations\n" + surface);
	ShaderAsset optimized;
	ShaderAsset skipping;
	ASSERT_TRUE(optimized.Import(FileSystem::Resolve(Path("project://Optimized.hlsl"))));
	ASSERT_TRUE(skipping.Import(FileSystem::Resolve(Path("project://Skipping.hlsl"))));
	EXPECT_TRUE(skipping.skipOptimizations);
	const ShaderVariant& plain = optimized.variants[SHADER_BASE_VARIANT];
	const ShaderVariant& skipped = skipping.variants[SHADER_BASE_VARIANT];
	ASSERT_TRUE(skipped.HasProgram(ShaderProgramKind::Depth));
#if defined(SHALLTEAR_CONFIG_RELEASE)
	EXPECT_GT(skipped.pixelSpirv.size(), plain.pixelSpirv.size());
	EXPECT_GT(skipped.depthPixelSpirv.size(), plain.depthPixelSpirv.size());
#endif
	(void)plain;
}

TEST_F(MaterialShadingFixture, TemplatedShaderDependsOnDepthTemplate) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* asset = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* lit = static_cast<ShaderAsset*>(asset);
	bool recorded = false;
	for (const ShaderAsset::Dependency& dependency : lit->dependencies) {
		if (dependency.physicalPath.find("DepthOnly.hlsli") != str::npos) {
			recorded = true;
			EXPECT_NE(dependency.contentHash, 0u);
		}
	}
	EXPECT_TRUE(recorded);
}

TEST_F(MaterialShadingFixture, DepthProgramsInterfaceFitsInsideForwardOnes) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const RHI::ShaderBytecodeFormat savedFormat = ShaderAsset::GetDefaultImportBytecodeFormat();
	ShaderAsset::SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat::Spirv);
	struct RestoreFormat {
		RHI::ShaderBytecodeFormat format;
		~RestoreFormat() { ShaderAsset::SetDefaultImportBytecodeFormat(format); }
	} restore{savedFormat};
	ShaderAsset litShader;
	ASSERT_TRUE(litShader.Import(FileSystem::Resolve(Path("engine://Shaders/Lit.hlsl"))));
	ShaderAsset* lit = &litShader;
	const ShaderVariant& base = lit->variants[SHADER_BASE_VARIANT];
	ASSERT_TRUE(base.HasProgram(ShaderProgramKind::Depth));

	SpirvReflection depthVertex = ReflectSpirv({base.depthVertexSpirv.data(), base.depthVertexSpirv.size()});
	SpirvReflection depthPixel = ReflectSpirv({base.depthPixelSpirv.data(), base.depthPixelSpirv.size()});
	Vector<RHI::DescriptorBindingDesc> depthBindings{GetMemoryResource(MemTag::Asset)};
	u32 depthPushConstantSize = 0;
	str error;
	ASSERT_TRUE(MergeReflections(depthVertex, depthPixel, depthBindings, depthPushConstantSize, error)) << error;
	EXPECT_LE(depthPushConstantSize, lit->pushConstantSize);
	for (const RHI::DescriptorBindingDesc& declared : depthBindings) {
		bool found = false;
		for (const RHI::DescriptorBindingDesc& known : lit->bindings) {
			if (known.set == declared.set && known.binding == declared.binding && known.type == declared.type) {
				found = true;
				break;
			}
		}
		EXPECT_TRUE(found);
	}

	auto declares = [](const ShaderAsset& shader, u32 set, u32 binding) {
		for (const RHI::DescriptorBindingDesc& entry : shader.bindings) {
			if (entry.set == set && entry.binding == binding) {
				return true;
			}
		}
		return false;
	};
	EXPECT_TRUE(declares(*lit, 0, 1));
	EXPECT_TRUE(declares(*lit, 0, 2));

	Asset* wireAsset = AssetDatabase::LoadAsset(Path("engine://Shaders/Wireframe.hlsl"));
	ASSERT_NE(wireAsset, nullptr);
	ShaderAsset* wire = static_cast<ShaderAsset*>(wireAsset);
	EXPECT_TRUE(declares(*wire, 0, 0));
	EXPECT_FALSE(declares(*wire, 0, 1));
	EXPECT_FALSE(declares(*wire, 0, 2));
}

TEST_F(MaterialShadingFixture, ClipDepthStageHasNoOutput) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const RHI::ShaderBytecodeFormat savedFormat = ShaderAsset::GetDefaultImportBytecodeFormat();
	ShaderAsset::SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat::Spirv);
	struct RestoreFormat {
		RHI::ShaderBytecodeFormat format;
		~RestoreFormat() { ShaderAsset::SetDefaultImportBytecodeFormat(format); }
	} restore{savedFormat};
	ShaderAsset litShader;
	ASSERT_TRUE(litShader.Import(FileSystem::Resolve(Path("engine://Shaders/Lit.hlsl"))));
	ShaderAsset* lit = &litShader;
	const str clip[] = {str(ShaderAlphaClipKeyword)};
	const u32 index = lit->GetOrCompileVariant({clip, 1});
	ASSERT_NE(index, SHADER_BASE_VARIANT);
	const ShaderVariant& clipped = lit->variants[index];
	ASSERT_TRUE(clipped.HasProgram(ShaderProgramKind::Depth));
	ASSERT_FALSE(clipped.depthClipPixelSpirv.empty());
	EXPECT_NE(clipped.depthClipPixelSpirv, clipped.depthPixelSpirv);

	EXPECT_TRUE(DeclaresAnOutputAtALocation(clipped.depthPixelSpirv));
	EXPECT_FALSE(DeclaresAnOutputAtALocation(clipped.depthClipPixelSpirv));

	SpirvReflection depthVertex = ReflectSpirv({clipped.depthVertexSpirv.data(), clipped.depthVertexSpirv.size()});
	SpirvReflection clipPixel = ReflectSpirv({clipped.depthClipPixelSpirv.data(), clipped.depthClipPixelSpirv.size()});
	Vector<RHI::DescriptorBindingDesc> bindings{GetMemoryResource(MemTag::Asset)};
	u32 pushConstantSize = 0;
	str error;
	ASSERT_TRUE(MergeReflections(depthVertex, clipPixel, bindings, pushConstantSize, error)) << error;
	EXPECT_LE(pushConstantSize, lit->pushConstantSize);
	for (const RHI::DescriptorBindingDesc& declared : bindings) {
		bool found = false;
		for (const RHI::DescriptorBindingDesc& known : lit->bindings) {
			if (known.set == declared.set && known.binding == declared.binding && known.type == declared.type) {
				found = true;
				break;
			}
		}
		EXPECT_TRUE(found);
	}
}

TEST_F(MaterialShadingFixture, CookCarriesDepthProgramToPlayer) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Asset* asset = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* lit = static_cast<ShaderAsset*>(asset);
	ASSERT_TRUE(lit->variants[SHADER_BASE_VARIANT].HasProgram(ShaderProgramKind::Depth));
	const str clip[] = {str(ShaderAlphaClipKeyword)};
	const u32 clipped = lit->GetOrCompileVariant({clip, 1});
	ASSERT_NE(clipped, SHADER_BASE_VARIANT);
	ASSERT_FALSE(lit->variants[clipped].depthClipPixelSpirv.empty());

	Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
	Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
	PakStringPool pool;
	CookWriter descriptor(descriptorBytes, pool);
	CookWriter payload(payloadBytes, pool);
	ASSERT_TRUE(lit->Cook(descriptor, payload));

	Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
	PakWriteStringBlob(stringBytes, pool.Texts());
	PakStringTable table;
	ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));

	ShaderAsset restored;
	CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
	CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
	ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader));
	ASSERT_FALSE(restored.variants.empty());
	const ShaderVariant& before = lit->variants[SHADER_BASE_VARIANT];
	const ShaderVariant& after = restored.variants[SHADER_BASE_VARIANT];
	EXPECT_EQ(after.vertexSpirv, before.vertexSpirv);
	EXPECT_EQ(after.pixelSpirv, before.pixelSpirv);
	EXPECT_EQ(after.depthVertexSpirv, before.depthVertexSpirv);
	EXPECT_EQ(after.depthPixelSpirv, before.depthPixelSpirv);
	EXPECT_EQ(after.depthClipPixelSpirv, before.depthClipPixelSpirv);
	EXPECT_TRUE(after.HasProgram(ShaderProgramKind::Depth));
	ASSERT_GT(restored.variants.size(), clipped);
	EXPECT_EQ(restored.variants[clipped].depthClipPixelSpirv, lit->variants[clipped].depthClipPixelSpirv);
}

TEST_F(MaterialShadingFixture, CookCarriesEveryTextureFallbackToPlayer) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	u32 declaredFallbacks = 0;
	for (const char* path : {"engine://Shaders/Lit.hlsl", "engine://Shaders/Unlit.hlsl"}) {
		Asset* asset = AssetDatabase::LoadAsset(Path(path));
		ASSERT_NE(asset, nullptr) << path;
		ShaderAsset* imported = static_cast<ShaderAsset*>(asset);

		Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
		Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
		PakStringPool pool;
		CookWriter descriptor(descriptorBytes, pool);
		CookWriter payload(payloadBytes, pool);
		ASSERT_TRUE(imported->Cook(descriptor, payload)) << path;

		Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
		PakWriteStringBlob(stringBytes, pool.Texts());
		PakStringTable table;
		ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));

		ShaderAsset restored;
		CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
		CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
		ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader)) << path;

		Vector<str> slots{GetMemoryResource(MemTag::Asset)};
		imported->GetMaterialTextureSlots(slots);
		for (const str& slot : slots) {
			const ShaderTextureFallback expected = imported->GetTextureFallback(slot);
			EXPECT_EQ(restored.GetTextureFallback(slot), expected) << path << " slot " << slot;
			if (expected != ShaderTextureFallback::White) {
				++declaredFallbacks;
			}
		}
	}
	EXPECT_GT(declaredFallbacks, 0u);
}

TEST_F(MaterialShadingFixture, CookCarriesEverySlotsDimensionToPlayer) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Kinds.hlsl", SlotsOfEveryDimension("#pragma shalltear_default flat gray\n", "float3(layer, slice, face + map)"));
	Asset* asset = AssetDatabase::LoadAsset(Path("project://Kinds.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* imported = static_cast<ShaderAsset*>(asset);
	ASSERT_EQ(imported->GetTextureSlotDimension("sky"), RHI::TextureDimension::Cube);
	ASSERT_EQ(imported->GetTextureFallback("flat"), ShaderTextureFallback::Gray);

	Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
	Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
	PakStringPool pool;
	CookWriter descriptor(descriptorBytes, pool);
	CookWriter payload(payloadBytes, pool);
	ASSERT_TRUE(imported->Cook(descriptor, payload));
	Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
	PakWriteStringBlob(stringBytes, pool.Texts());
	PakStringTable table;
	ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));
	ShaderAsset restored;
	CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
	CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
	ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader));

	for (const char* slot : {"layers", "volume", "sky", "flat"}) {
		EXPECT_EQ(restored.GetTextureSlotDimension(slot), imported->GetTextureSlotDimension(slot)) << slot;
		EXPECT_EQ(restored.GetTextureFallback(slot), imported->GetTextureFallback(slot)) << slot;
	}
}

TEST_F(MaterialShadingFixture, CookCarriesEveryShadersTemplateToPlayer) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	const std::pair<const char*, ShaderTemplate> shaders[] = {
		{"engine://Shaders/Lit.hlsl", ShaderTemplate::Lit},
		{"engine://Shaders/Unlit.hlsl", ShaderTemplate::Unlit},
		{"engine://Shaders/GradientSky.hlsl", ShaderTemplate::Sky},
		{"engine://Shaders/Wireframe.hlsl", ShaderTemplate::None},
	};
	for (const auto& [path, expected] : shaders) {
		Asset* asset = AssetDatabase::LoadAsset(Path(path));
		ASSERT_NE(asset, nullptr) << path;
		ShaderAsset* imported = static_cast<ShaderAsset*>(asset);
		ASSERT_EQ(imported->integrator, expected) << path;

		Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
		Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
		PakStringPool pool;
		CookWriter descriptor(descriptorBytes, pool);
		CookWriter payload(payloadBytes, pool);
		ASSERT_TRUE(imported->Cook(descriptor, payload)) << path;
		Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
		PakWriteStringBlob(stringBytes, pool.Texts());
		PakStringTable table;
		ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));

		ShaderAsset restored;
		CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
		CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
		ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader)) << path;
		EXPECT_EQ(restored.integrator, expected) << path;
	}
}

TEST_F(MaterialShadingFixture, CookCarriesEveryDeclaredDefaultToPlayer) {
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	size_t declared = 0;
	for (const char* path : {"engine://Shaders/Lit.hlsl", "engine://Shaders/Unlit.hlsl", "engine://Shaders/GradientSky.hlsl"}) {
		Asset* asset = AssetDatabase::LoadAsset(Path(path));
		ASSERT_NE(asset, nullptr) << path;
		ShaderAsset* imported = static_cast<ShaderAsset*>(asset);

		Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
		Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
		PakStringPool pool;
		CookWriter descriptor(descriptorBytes, pool);
		CookWriter payload(payloadBytes, pool);
		ASSERT_TRUE(imported->Cook(descriptor, payload)) << path;
		Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
		PakWriteStringBlob(stringBytes, pool.Texts());
		PakStringTable table;
		ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));

		ShaderAsset restored;
		CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
		CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
		ASSERT_TRUE(restored.LoadCooked(descriptorReader, payloadReader)) << path;
		ASSERT_EQ(restored.memberDefaults.size(), imported->memberDefaults.size()) << path;
		for (size_t i = 0; i < imported->memberDefaults.size(); ++i) {
			const ShaderMemberDefault& before = imported->memberDefaults[i];
			const ShaderMemberDefault& after = restored.memberDefaults[i];
			EXPECT_EQ(after.name, before.name) << path;
			ASSERT_EQ(after.count, before.count) << path << " " << before.name;
			for (u32 c = 0; c < before.count; ++c) {
				EXPECT_FLOAT_EQ(after.numbers[c], before.numbers[c]) << path << " " << before.name << "[" << c << "]";
			}
		}
		declared += imported->memberDefaults.size();
	}
	EXPECT_GT(declared, 0u);
}

TEST_F(MaterialReadbackFixture, UnauthoredMemberUsesDefault) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	if (!CompilerAvailable()) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	WriteProjectFile("Declared.hlsl", "#pragma shalltear_template unlit\n"
									  "#pragma shalltear_default tint 0.2 0.6 0.9 1\n"
									  "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 tint; };\n"
									  "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
									  "\tsurface.baseColor = tint.rgb;\n"
									  "}\n");
	Asset* asset = AssetDatabase::LoadAsset(Path("project://Declared.hlsl"));
	ASSERT_NE(asset, nullptr);
	ShaderAsset* imported = static_cast<ShaderAsset*>(asset);
	ASSERT_EQ(imported->memberDefaults.size(), 1u);
	MaterialAsset* material = LoadMaterial("Unauthored.mat", imported->assetId, "");
	ASSERT_NE(material, nullptr);
	ASSERT_EQ(material->FindParam(StringHash("tint")), nullptr);

	Vector<u8> descriptorBytes{GetMemoryResource(MemTag::Asset)};
	Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
	PakStringPool pool;
	CookWriter descriptor(descriptorBytes, pool);
	CookWriter payload(payloadBytes, pool);
	ASSERT_TRUE(imported->Cook(descriptor, payload));
	Vector<u8> stringBytes{GetMemoryResource(MemTag::Asset)};
	PakWriteStringBlob(stringBytes, pool.Texts());
	PakStringTable table;
	ASSERT_TRUE(PakReadStringBlob(stringBytes.data(), stringBytes.size(), table));
	CookReader descriptorReader(descriptorBytes.data(), descriptorBytes.size(), table);
	CookReader payloadReader(payloadBytes.data(), payloadBytes.size(), table);
	ShaderAsset* shipped = New<ShaderAsset>(MemTag::Asset);
	ASSERT_TRUE(shipped->LoadCooked(descriptorReader, payloadReader));

	const Handle<ShaderAsset> live = material->shader;
	material->shader = Handle<ShaderAsset>(shipped);
	ShadingStage stage;
	ASSERT_TRUE(BuildStage(material, stage));
	Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
	Vec2I size{};
	ASSERT_TRUE(RenderStage(stage, pixels, size));
	const Vec3 measured = CenterTexel(pixels, size);
	const Vec3 expected = ExpectedTexel(Vec3(0.2f, 0.6f, 0.9f));
	EXPECT_NEAR(measured.x, expected.x, 1.5f);
	EXPECT_NEAR(measured.y, expected.y, 1.5f);
	EXPECT_NEAR(measured.z, expected.z, 1.5f);

	TearDownStage(stage);
	material->shader = live;
	Delete(shipped);
}
