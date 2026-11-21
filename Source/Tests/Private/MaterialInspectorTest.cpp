#include "TestsPCH.h"

#include "TestImages.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/ShaderCompileQueue.h"
#include "Shalltear/Core/Graphics/ShaderReflection.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EditorVariantCompiles.h"
#include "Shalltear/Editor/MaterialInspectorOps.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PropertyDrawers.h"

#include "Shalltear/Core/Asset/AssetImporter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace Shalltear;
using namespace UITest;

namespace {

	std::filesystem::path MaterialRepoDir(const char* relative) {
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

	UI::ElementHandle FindByText(UI::Context& context, UI::ElementHandle from, std::string_view text) {
		if (context.GetTextString(from) == text) {
			return from;
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return {};
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			UI::ElementHandle found = FindByText(context, handle, text);
			if (context.IsAlive(found)) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	UI::ElementHandle RowFor(UI::Context& context, UI::ElementHandle from, std::string_view label) {
		UI::ElementHandle cell = FindByText(context, from, label);
		if (!context.IsAlive(cell)) {
			return {};
		}
		const UI::Element* element = context.GetElement(cell);
		return element ? context.MakeHandle(element->parent) : UI::ElementHandle{};
	}

	class MaterialInspectorFixture : public ::testing::Test {
	protected:
		void SetUp() override {
			std::filesystem::path engineAssets = MaterialRepoDir("EngineAssets");
			if (engineAssets.empty()) {
				GTEST_SKIP() << "EngineAssets not found";
			}
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			TypeRegistry::InitializePrimitives();
			AssetDatabase::Initialize();
			UISystem::Initialize();
			PanelManager::Initialize();
			RegisterBuiltInPanels();
			if (!UISystem::GetDefaultFont().IsValid()) {
				GTEST_SKIP() << "default font unavailable";
			}
		}

		void TearDown() override {
			PanelManager::Terminate();
			ShaderCompileQueue::Terminate();
			UISystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("engine");
		}

		static void DeclareScalarInterface(ShaderAsset& shader) {
			UniformBlockLayout& block = shader.uniformBlocks.emplace_back();
			block.set = MATERIAL_DESCRIPTOR_SET;
			block.binding = 0;
			block.sizeBytes = 32;
			block.members.push_back({"baseColor", UniformMemberType::Vec4, 0, 16});
			block.members.push_back({"specular", UniformMemberType::Float, 16, 4});
			block.members.push_back({"shininess", UniformMemberType::Float, 20, 4});
			block.members.push_back({"sltReserved", UniformMemberType::Vec2, 24, 8});

			ShaderResourceInfo& texture = shader.resources.emplace_back();
			texture.set = MATERIAL_DESCRIPTOR_SET;
			texture.binding = 1;
			texture.type = RHI::DescriptorType::SampledTexture;
			texture.name = "baseColorTexture";
		}

		static void BuildInspector(UI::Context& context, UI::ElementHandle parent, MaterialAsset& material) {
			const AssetInspectorFn* inspector = PropertyDrawers::FindAssetInspector(StringHash("MaterialAsset"));
			ASSERT_NE(inspector, nullptr);
			UI::Style bodyStyle;
			bodyStyle.flexDirection = UI::FlexDirection::Column;
			bodyStyle.width = UI::Length::Percent(1.0f);
			context.SetStyle(parent, bodyStyle);
			(*inspector)(context, parent, Guid{}, material);
			context.Update();
		}
	};

} // anonymous namespace

TEST_F(MaterialInspectorFixture, FloatRowsMatchColorRows) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetColor("baseColor", Color::White());
	material.SetFloat("specular", 0.5f);
	material.SetFloat("shininess", 32.0f);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle colorRow = RowFor(context, root, "baseColor");
	UI::ElementHandle floatRow = RowFor(context, root, "specular");
	ASSERT_TRUE(context.IsAlive(colorRow));
	ASSERT_TRUE(context.IsAlive(floatRow));

	UI::ElementHandle colorField = FindByClass(context, colorRow, StringHash("slt-number-input"), 0);
	UI::ElementHandle floatField = FindByClass(context, floatRow, StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(colorField));
	ASSERT_TRUE(context.IsAlive(floatField));

	f32 colorHeight = context.GetLayout(colorField).borderBox.h;
	f32 floatHeight = context.GetLayout(floatField).borderBox.h;
	ASSERT_GT(colorHeight, 12.0f);
	EXPECT_NEAR(floatHeight, colorHeight, 1.0f);
	EXPECT_EQ(context.GetTextString(floatField), "0.5");
}

TEST_F(MaterialInspectorFixture, EditingAFloatParamWritesTheMaterial) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetFloat("shininess", 32.0f);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle field = FindByClass(context, RowFor(context, root, "shininess"),
										  StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(field));

	ClickElement(context, field);
	ASSERT_EQ(context.GetFocused(), field);

	InputEvent selectAll{};
	selectAll.type = InputEventType::KeyDown;
	selectAll.key = Key::A;
	selectAll.mods = KeyMod::Ctrl;
	context.InjectInput(selectAll);
	for (char c : std::string_view("8")) {
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

	EXPECT_FLOAT_EQ(material.GetFloat(StringHash("shininess"), -1.0f), 8.0f);
}

namespace {

	using MaterialInspectorOps::RowKind;

	void DeclareAnnotatedShader(ShaderAsset& shader) {
		UniformBlockLayout& block = shader.uniformBlocks.emplace_back();
		block.set = MATERIAL_DESCRIPTOR_SET;
		block.binding = 0;
		block.sizeBytes = 48;
		block.members.push_back({"baseColor", UniformMemberType::Vec4, 0, 16});
		block.members.push_back({"emissive", UniformMemberType::Vec3, 16, 12});
		block.members.push_back({"roughness", UniformMemberType::Float, 28, 4});
		block.members.push_back({"offset", UniformMemberType::Vec2, 32, 8});
		block.members.push_back({"tiling", UniformMemberType::Int, 40, 4});

		ShaderResourceInfo& baseMap = shader.resources.emplace_back();
		baseMap.set = MATERIAL_DESCRIPTOR_SET;
		baseMap.binding = 1;
		baseMap.type = RHI::DescriptorType::SampledTexture;
		baseMap.name = "baseColorTexture";

		shader.memberRanges.push_back({"roughness", 0.0f, 1.0f});
		shader.memberHints.push_back({"baseColor", ShaderHint::Color});
		shader.memberHints.push_back({"emissive", ShaderHint::HDR});
	}

	const MaterialInspectorOps::Group* GroupNamed(const Vector<MaterialInspectorOps::Group>& groups,
												  std::string_view name) {
		for (const MaterialInspectorOps::Group& group : groups) {
			if (group.name == name) {
				return &group;
			}
		}
		return nullptr;
	}

	const MaterialInspectorOps::Row* RowNamed(const Vector<MaterialInspectorOps::Group>& groups,
											  std::string_view name) {
		for (const MaterialInspectorOps::Group& group : groups) {
			for (const MaterialInspectorOps::Row& row : group.rows) {
				if (row.name == name) {
					return &row;
				}
			}
		}
		return nullptr;
	}

} // anonymous namespace

TEST(MaterialRowSchemaTest, EveryMemberKindAndAnnotationPicksControl) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);

	ASSERT_NE(RowNamed(groups, "baseColor"), nullptr);
	EXPECT_EQ(RowNamed(groups, "baseColor")->kind, RowKind::Color);
	EXPECT_EQ(RowNamed(groups, "emissive")->kind, RowKind::HDRColor);
	EXPECT_EQ(RowNamed(groups, "roughness")->kind, RowKind::Slider);
	EXPECT_FLOAT_EQ(RowNamed(groups, "roughness")->maxValue, 1.0f);
	EXPECT_EQ(RowNamed(groups, "offset")->kind, RowKind::Vector);
	EXPECT_EQ(RowNamed(groups, "tiling")->kind, RowKind::Number);
	EXPECT_EQ(RowNamed(groups, "baseColorTexture")->kind, RowKind::Texture);

	ShaderAsset bare;
	DeclareAnnotatedShader(bare);
	bare.memberRanges.clear();
	bare.memberHints.clear();
	Vector<MaterialInspectorOps::Group> plain{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(bare, plain);
	EXPECT_EQ(RowNamed(plain, "roughness")->kind, RowKind::Number);
	EXPECT_EQ(RowNamed(plain, "baseColor")->kind, RowKind::Vector);
	EXPECT_EQ(RowNamed(plain, "emissive")->kind, RowKind::Vector);
}

TEST(MaterialRowSchemaTest, LabelsTogglesAndHiddenShapeTheRows) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	shader.memberLabels.push_back({"roughness", "Surface Roughness"});
	shader.memberLabels.push_back({"baseColorTexture", "Base Map"});
	shader.memberHints.push_back({"tiling", ShaderHint::Toggle});
	shader.memberHints.push_back({"offset", ShaderHint::Hidden});
	ShaderMemberGroup group;
	group.name = "Surface Inputs";
	group.members.push_back("emissive");
	group.members.push_back("offset");
	shader.memberGroups.push_back(std::move(group));
	ShaderResourceInfo& secret = shader.resources.emplace_back();
	secret.set = MATERIAL_DESCRIPTOR_SET;
	secret.binding = 3;
	secret.type = RHI::DescriptorType::SampledTexture;
	secret.name = "secretTexture";
	shader.memberHints.push_back({"secretTexture", ShaderHint::Hidden});

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);

	const MaterialInspectorOps::Row* roughness = RowNamed(groups, "roughness");
	ASSERT_NE(roughness, nullptr);
	EXPECT_EQ(roughness->DisplayName(), "Surface Roughness");
	EXPECT_EQ(roughness->kind, RowKind::Slider);
	EXPECT_EQ(RowNamed(groups, "baseColorTexture")->DisplayName(), "Base Map");
	EXPECT_EQ(RowNamed(groups, "baseColor")->DisplayName(), "baseColor");

	ASSERT_NE(RowNamed(groups, "tiling"), nullptr);
	EXPECT_EQ(RowNamed(groups, "tiling")->kind, RowKind::Toggle);

	EXPECT_EQ(RowNamed(groups, "offset"), nullptr);
	EXPECT_EQ(RowNamed(groups, "secretTexture"), nullptr);
	ASSERT_NE(GroupNamed(groups, "Surface Inputs"), nullptr);
	EXPECT_EQ(GroupNamed(groups, "Surface Inputs")->rows.size(), 1u);

	shader.memberHints.clear();
	MaterialInspectorOps::BuildLayout(shader, groups);
	EXPECT_NE(RowNamed(groups, "offset"), nullptr);
	EXPECT_NE(RowNamed(groups, "secretTexture"), nullptr);
	EXPECT_EQ(RowNamed(groups, "tiling")->kind, RowKind::Number);
}

TEST(MaterialRowSchemaTest, DeclaredGroupsLeadAndClaimTheirMembers) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	ShaderMemberGroup surface;
	surface.name = "Surface Inputs";
	surface.members.push_back("baseColor");
	surface.members.push_back("baseColorTexture");
	shader.memberGroups.push_back(std::move(surface));
	ShaderMemberGroup emission;
	emission.name = "Emission";
	emission.members.push_back("emissive");
	shader.memberGroups.push_back(std::move(emission));

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);

	ASSERT_GE(groups.size(), 5u);
	EXPECT_EQ(groups[0].name, MaterialInspectorOps::SurfaceOptionsGroup);
	EXPECT_EQ(groups[1].name, "Surface Inputs");
	EXPECT_EQ(groups[2].name, "Emission");
	ASSERT_EQ(groups[1].rows.size(), 2u);
	EXPECT_EQ(groups[1].rows[0].name, "baseColor");
	EXPECT_EQ(groups[1].rows[1].name, "baseColorTexture");
	EXPECT_EQ(groups[1].rows[1].kind, RowKind::Texture);

	const MaterialInspectorOps::Group* parameters = GroupNamed(groups, MaterialInspectorOps::ParametersGroup);
	ASSERT_NE(parameters, nullptr);
	ASSERT_EQ(parameters->rows.size(), 3u);
	EXPECT_EQ(parameters->rows[0].name, "roughness");
	EXPECT_EQ(parameters->rows[1].name, "offset");
	EXPECT_EQ(parameters->rows[2].name, "tiling");
	EXPECT_EQ(GroupNamed(groups, MaterialInspectorOps::TexturesGroup), nullptr);
}

TEST(MaterialRowSchemaTest, GroupNamingEverythingLeavesNoEmptyRuns) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	ShaderMemberGroup all;
	all.name = "All";
	for (const UniformMember& member : shader.uniformBlocks[0].members) {
		all.members.push_back(member.name);
	}
	all.members.push_back("baseColorTexture");
	shader.memberGroups.push_back(std::move(all));

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);
	ASSERT_EQ(groups.size(), 3u);
	EXPECT_EQ(groups[1].name, "All");
	EXPECT_EQ(groups[1].rows.size(), 6u);
}

TEST(MaterialRowSchemaTest, SharedMemberGoesToFirstGroup) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	ShaderMemberGroup first;
	first.name = "First";
	first.members.push_back("roughness");
	shader.memberGroups.push_back(std::move(first));
	ShaderMemberGroup second;
	second.name = "Second";
	second.members.push_back("roughness");
	second.members.push_back("offset");
	shader.memberGroups.push_back(std::move(second));

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);
	const MaterialInspectorOps::Group* firstGroup = GroupNamed(groups, "First");
	const MaterialInspectorOps::Group* secondGroup = GroupNamed(groups, "Second");
	ASSERT_NE(firstGroup, nullptr);
	ASSERT_NE(secondGroup, nullptr);
	ASSERT_EQ(firstGroup->rows.size(), 1u);
	ASSERT_EQ(secondGroup->rows.size(), 1u);
	EXPECT_EQ(secondGroup->rows[0].name, "offset");
}

TEST(MaterialRowSchemaTest, InterfacelessShaderHasEngineRows) {
	ShaderAsset raw;
	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(raw, groups);
	ASSERT_EQ(groups.size(), 2u);
	EXPECT_EQ(groups[0].name, MaterialInspectorOps::SurfaceOptionsGroup);
	EXPECT_EQ(groups[1].name, MaterialInspectorOps::AdvancedOptionsGroup);
}

TEST(MaterialRowSchemaTest, SkyShaderHasNoneOfEnginesRows) {
	ShaderAsset sky;
	sky.integrator = ShaderTemplate::Sky;
	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(sky, groups);
	for (const MaterialInspectorOps::Group& group : groups) {
		EXPECT_NE(group.name, MaterialInspectorOps::SurfaceOptionsGroup);
		EXPECT_NE(group.name, MaterialInspectorOps::AdvancedOptionsGroup);
	}
	EXPECT_TRUE(groups.empty());
}

TEST_F(MaterialInspectorFixture, DraggingARangeSliderWritesTheMaterial) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	shader.memberRanges.push_back({"specular", 0.0f, 1.0f});

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetFloat("specular", 0.25f);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle row = RowFor(context, root, "specular");
	ASSERT_TRUE(context.IsAlive(row));
	UI::ElementHandle rail = FindByClass(context, row, StringHash("slt-slider-rail"), 0);
	ASSERT_TRUE(context.IsAlive(rail));

	DragElementBy(context, rail, static_cast<i32>(context.GetLayout(rail).borderBox.w), 0);

	EXPECT_NEAR(material.GetFloat(StringHash("specular"), -1.0f), 1.0f, 0.05f);
}

TEST_F(MaterialInspectorFixture, EmptyTextureWellNamesDeclaredFallback) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	shader.textureDefaults.push_back({"baseColorTexture", ShaderTextureFallback::Normal});

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, root, "None (normal)")));

	ShaderAsset plain;
	DeclareScalarInterface(plain);
	MaterialAsset second;
	second.shader = Handle<ShaderAsset>(&plain);
	UI::Context plainContext(desc, UISystem::GetFontSystem());
	UI::ElementHandle plainRoot = plainContext.CreateElement();
	plainContext.AppendChild(plainContext.GetRoot(), plainRoot);
	BuildInspector(plainContext, plainRoot, second);
	EXPECT_TRUE(plainContext.IsAlive(UITest::FindByText(plainContext, plainRoot, "None (white)")));
}

TEST_F(MaterialInspectorFixture, EditingOneVectorComponentLeavesSiblingAlone) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetVec2("sltReserved", Vec2{3.0f, 7.0f});

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle row = RowFor(context, root, "sltReserved");
	ASSERT_TRUE(context.IsAlive(row));
	UI::ElementHandle first = FindByClass(context, row, StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(first));

	ClickElement(context, first);
	ASSERT_EQ(context.GetFocused(), first);
	InputEvent selectAll{};
	selectAll.type = InputEventType::KeyDown;
	selectAll.key = Key::A;
	selectAll.mods = KeyMod::Ctrl;
	context.InjectInput(selectAll);
	for (char c : std::string_view("5")) {
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

	const MaterialAsset::Param* param = material.FindParam(StringHash("sltReserved"));
	ASSERT_NE(param, nullptr);
	EXPECT_FLOAT_EQ(param->numbers[0], 5.0f);
	EXPECT_FLOAT_EQ(param->numbers[1], 7.0f);
}

TEST(MaterialRowSchemaTest, DeclaredKeywordsAreRows) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	shader.keywords.push_back("DETAIL_MAP");
	shader.keywords.push_back(str(ShaderAlphaClipKeyword));
	shader.keywords.push_back("RIM_LIGHT");
	shader.keywordBindings.push_back({"RIM_LIGHT", "roughness"});

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);

	const MaterialInspectorOps::Group* keywords = GroupNamed(groups, MaterialInspectorOps::KeywordsGroup);
	ASSERT_NE(keywords, nullptr);
	ASSERT_EQ(keywords->rows.size(), 2u);
	EXPECT_EQ(keywords->rows[0].name, "DETAIL_MAP");
	EXPECT_EQ(keywords->rows[0].kind, RowKind::Keyword);
	EXPECT_TRUE(keywords->rows[0].keywordMember.empty());
	EXPECT_EQ(keywords->rows[1].name, "RIM_LIGHT");
	EXPECT_EQ(keywords->rows[1].keywordMember, "roughness");

	EXPECT_EQ(groups[groups.size() - 2].name, MaterialInspectorOps::KeywordsGroup);
	EXPECT_EQ(groups[groups.size() - 1].name, MaterialInspectorOps::AdvancedOptionsGroup);
}

TEST(MaterialRowSchemaTest, BoundKeywordReadsAsTogglesLabel) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	shader.keywords.push_back("_MOSS");
	shader.keywords.push_back("RIM_LIGHT");
	shader.keywordBindings.push_back({"_MOSS", "roughness"});
	shader.keywordBindings.push_back({"RIM_LIGHT", "tiling"});
	shader.memberLabels.push_back({"roughness", "Moss"});

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);
	const MaterialInspectorOps::Group* keywords = GroupNamed(groups, MaterialInspectorOps::KeywordsGroup);
	ASSERT_NE(keywords, nullptr);
	ASSERT_EQ(keywords->rows.size(), 2u);
	EXPECT_EQ(keywords->rows[0].DisplayName(), "Moss");
	EXPECT_EQ(keywords->rows[0].name, "_MOSS");
	EXPECT_EQ(keywords->rows[1].DisplayName(), "RIM_LIGHT");
}

TEST(MaterialRowSchemaTest, SurfaceOptionsLeadAndAdvancedOptionsClose) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);

	ASSERT_GE(groups.size(), 2u);
	const MaterialInspectorOps::Group& first = groups[0];
	const MaterialInspectorOps::Group& last = groups[groups.size() - 1];
	EXPECT_EQ(first.name, MaterialInspectorOps::SurfaceOptionsGroup);
	EXPECT_TRUE(first.builtIn);
	ASSERT_EQ(first.rows.size(), 3u);
	EXPECT_EQ(first.rows[0].kind, RowKind::Blend);
	EXPECT_EQ(first.rows[1].kind, RowKind::Cull);
	EXPECT_EQ(first.rows[2].kind, RowKind::AlphaClip);

	EXPECT_EQ(last.name, MaterialInspectorOps::AdvancedOptionsGroup);
	EXPECT_TRUE(last.builtIn);
	ASSERT_EQ(last.rows.size(), 1u);
	EXPECT_EQ(last.rows[0].kind, RowKind::SortingPriority);

	const MaterialInspectorOps::Group* parameters = GroupNamed(groups, MaterialInspectorOps::ParametersGroup);
	ASSERT_NE(parameters, nullptr);
	EXPECT_GT(parameters, &first);
	EXPECT_LT(parameters, &last);
}

TEST(MaterialRowSchemaTest, KeywordGroupIsDropDown) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	for (const char* keyword : {"_MODE_B", "RIM_LIGHT", "_MODE_A", "_MODE_C"}) {
		shader.keywords.push_back(keyword);
	}
	shader.keywordBindings.push_back({"_MODE_B", "tiling", 1, "Beta"});
	shader.keywordBindings.push_back({"_MODE_A", "tiling", 0, "Alpha"});
	shader.keywordBindings.push_back({"_MODE_C", "tiling", 2, ""});
	shader.memberLabels.push_back({"tiling", "Mode"});

	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);
	const MaterialInspectorOps::Group* keywords = GroupNamed(groups, MaterialInspectorOps::KeywordsGroup);
	ASSERT_NE(keywords, nullptr);
	ASSERT_EQ(keywords->rows.size(), 2u);
	const MaterialInspectorOps::Row& group = keywords->rows[0];
	EXPECT_EQ(group.kind, RowKind::KeywordEnum);
	EXPECT_EQ(group.keywordMember, "tiling");
	EXPECT_EQ(group.DisplayName(), "Mode");
	ASSERT_EQ(group.entries.size(), 3u);
	EXPECT_EQ(group.entries[0].keyword, "_MODE_A");
	EXPECT_EQ(group.entries[0].name, "Alpha");
	EXPECT_EQ(group.entries[1].name, "Beta");
	EXPECT_EQ(group.entries[2].name, "_MODE_C");
	EXPECT_EQ(keywords->rows[1].kind, RowKind::Keyword);
	EXPECT_EQ(keywords->rows[1].name, "RIM_LIGHT");
}

TEST(MaterialRowSchemaTest, KeywordlessShaderHasNoGroup) {
	ShaderAsset shader;
	DeclareAnnotatedShader(shader);
	shader.keywords.push_back(str(ShaderAlphaClipKeyword));
	Vector<MaterialInspectorOps::Group> groups{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::BuildLayout(shader, groups);
	EXPECT_EQ(GroupNamed(groups, MaterialInspectorOps::KeywordsGroup), nullptr);
}

TEST_F(MaterialInspectorFixture, KeywordTickWritesMember) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	shader.keywords.push_back("RIM_LIGHT");
	shader.keywordBindings.push_back({"RIM_LIGHT", "specular"});

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetFloat("specular", 0.0f);
	u64 unkeyed = material.keywordHash;

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle row = RowFor(context, root, "RIM_LIGHT");
	ASSERT_TRUE(context.IsAlive(row));
	UI::ElementHandle box = FindByClass(context, row, StringHash("slt-checkbox"), 0);
	ASSERT_TRUE(context.IsAlive(box));

	ClickElement(context, box);
	context.Update();

	ASSERT_EQ(material.keywords.size(), 1u);
	EXPECT_EQ(material.keywords[0], "RIM_LIGHT");
	EXPECT_NE(material.keywordHash, unkeyed);
	EXPECT_FLOAT_EQ(material.GetFloat(StringHash("specular"), -1.0f), 1.0f);

	UI::Context second(desc, UISystem::GetFontSystem());
	UI::ElementHandle secondRoot = second.CreateElement();
	second.AppendChild(second.GetRoot(), secondRoot);
	BuildInspector(second, secondRoot, material);
	UI::ElementHandle ticked = FindByClass(second, RowFor(second, secondRoot, "RIM_LIGHT"),
										   StringHash("slt-checkbox"), 0);
	ASSERT_TRUE(second.IsAlive(ticked));
	ClickElement(second, ticked);
	second.Update();

	EXPECT_TRUE(material.keywords.empty());
	EXPECT_EQ(material.keywordHash, unkeyed);
	EXPECT_FLOAT_EQ(material.GetFloat(StringHash("specular"), -1.0f), 0.0f);
}

TEST_F(MaterialInspectorFixture, GroupEntryPicksOneKeyword) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	for (const char* keyword : {"_MODE_A", "_MODE_B", "_MODE_C", "RIM_LIGHT"}) {
		shader.keywords.push_back(keyword);
	}
	shader.keywordBindings.push_back({"_MODE_A", "specular", 0, "Alpha"});
	shader.keywordBindings.push_back({"_MODE_B", "specular", 1, "Beta"});
	shader.keywordBindings.push_back({"_MODE_C", "specular", 2, "Gamma"});
	shader.memberLabels.push_back({"specular", "Mode"});
	shader.memberHints.push_back({"specular", ShaderHint::Hidden});

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetFloat("specular", 0.0f);
	const str held[] = {"_MODE_A", "RIM_LIGHT"};
	material.SetKeywords(held);

	auto pick = [&](std::string_view entry) {
		UI::ContextDesc desc;
		desc.sizePx = {420, 900};
		UI::Context context(desc, UISystem::GetFontSystem());
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		BuildInspector(context, root, material);
		UI::ElementHandle control = FindByClass(context, RowFor(context, root, "Mode"), StringHash("slt-dropdown"), 0);
		ASSERT_TRUE(context.IsAlive(control));
		ClickElement(context, control);
		context.Update();
		UI::ElementHandle item = UITest::FindByText(context, context.GetRoot(), entry);
		ASSERT_TRUE(context.IsAlive(item)) << entry;
		ClickElement(context, item);
		context.Update();
	};
	auto enabled = [&](std::string_view keyword) {
		return std::find(material.keywords.begin(), material.keywords.end(), keyword) != material.keywords.end();
	};

	ASSERT_NO_FATAL_FAILURE(pick("Gamma"));
	EXPECT_TRUE(enabled("_MODE_C"));
	EXPECT_FALSE(enabled("_MODE_A"));
	EXPECT_TRUE(enabled("RIM_LIGHT"));
	EXPECT_FLOAT_EQ(material.GetFloat(StringHash("specular"), -1.0f), 2.0f);

	ASSERT_NO_FATAL_FAILURE(pick("Beta"));
	EXPECT_TRUE(enabled("_MODE_B"));
	EXPECT_FALSE(enabled("_MODE_C"));
	EXPECT_EQ(material.keywords.size(), 2u);
	EXPECT_FLOAT_EQ(material.GetFloat(StringHash("specular"), -1.0f), 1.0f);
}

TEST_F(MaterialInspectorFixture, ToggleRowTicksInt) {
	ShaderAsset shader;
	UniformBlockLayout& block = shader.uniformBlocks.emplace_back();
	block.set = MATERIAL_DESCRIPTOR_SET;
	block.binding = 0;
	block.sizeBytes = 16;
	block.members.push_back({"useRim", UniformMemberType::Int, 0, 4});
	shader.memberHints.push_back({"useRim", ShaderHint::Toggle});
	shader.memberLabels.push_back({"useRim", "Use Rim"});

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetInt("useRim", 0);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	EXPECT_FALSE(context.IsAlive(UITest::FindByText(context, root, "useRim")));
	UI::ElementHandle row = RowFor(context, root, "Use Rim");
	ASSERT_TRUE(context.IsAlive(row));
	UI::ElementHandle box = FindByClass(context, row, StringHash("slt-checkbox"), 0);
	ASSERT_TRUE(context.IsAlive(box));

	ClickElement(context, box);
	context.Update();
	const MaterialAsset::Param* param = material.FindParam(StringHash("useRim"));
	ASSERT_NE(param, nullptr);
	EXPECT_EQ(param->integer, 1);

	UI::Context second(desc, UISystem::GetFontSystem());
	UI::ElementHandle secondRoot = second.CreateElement();
	second.AppendChild(second.GetRoot(), secondRoot);
	BuildInspector(second, secondRoot, material);
	UI::ElementHandle ticked = FindByClass(second, RowFor(second, secondRoot, "Use Rim"), StringHash("slt-checkbox"), 0);
	ASSERT_TRUE(second.IsAlive(ticked));
	ClickElement(second, ticked);
	second.Update();
	EXPECT_EQ(material.FindParam(StringHash("useRim"))->integer, 0);
}

TEST_F(MaterialInspectorFixture, WideControlDoesNotCollapseRowsLabel) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	material.SetColor("baseColor", Color::White());

	UI::ContextDesc desc;
	desc.sizePx = {320, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle colorLabel = UITest::FindByText(context, root, "baseColor");
	UI::ElementHandle floatLabel = UITest::FindByText(context, root, "shininess");
	ASSERT_TRUE(context.IsAlive(colorLabel));
	ASSERT_TRUE(context.IsAlive(floatLabel));

	f32 colorWidth = context.GetLayout(colorLabel).borderBox.w;
	f32 floatWidth = context.GetLayout(floatLabel).borderBox.w;
	EXPECT_GT(colorWidth, 0.0f);
	EXPECT_FLOAT_EQ(colorWidth, floatWidth);
}

TEST_F(MaterialInspectorFixture, BlendRowOverridesShader) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	shader.renderState.blend = RHI::BlendMode::Additive;

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	ASSERT_FALSE(material.blend.has_value());

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	EXPECT_TRUE(context.IsAlive(UITest::FindByText(context, root, "Shader default (additive)")));

	UI::ElementHandle control = FindByClass(context, RowFor(context, root, "Blend"), StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(control));
	ClickElement(context, control);
	context.Update();
	UI::ElementHandle item = UITest::FindByText(context, context.GetRoot(), "multiply");
	ASSERT_TRUE(context.IsAlive(item));
	ClickElement(context, item);
	context.Update();

	ASSERT_TRUE(material.blend.has_value());
	EXPECT_EQ(*material.blend, RHI::BlendMode::Multiply);
	EXPECT_EQ(material.ResolveRenderState(shader.renderState).blend, RHI::BlendMode::Multiply);

	UI::Context second(desc, UISystem::GetFontSystem());
	UI::ElementHandle secondRoot = second.CreateElement();
	second.AppendChild(second.GetRoot(), secondRoot);
	BuildInspector(second, secondRoot, material);
	ClickElement(second, FindByClass(second, RowFor(second, secondRoot, "Blend"), StringHash("slt-dropdown"), 0));
	second.Update();
	UI::ElementHandle back = UITest::FindByText(second, second.GetRoot(), "Shader default (additive)");
	ASSERT_TRUE(second.IsAlive(back));
	ClickElement(second, back);
	second.Update();

	EXPECT_FALSE(material.blend.has_value());
	EXPECT_EQ(material.ResolveRenderState(shader.renderState).blend, RHI::BlendMode::Additive);
}

TEST_F(MaterialInspectorFixture, AlphaClipDragMovesVariant) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	shader.keywords.push_back(str(ShaderAlphaClipKeyword));

	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);
	u64 unclipped = material.keywordHash;

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle row = RowFor(context, root, "Alpha Clip");
	ASSERT_TRUE(context.IsAlive(row));
	EXPECT_FALSE(context.IsAlive(FindByClass(context, row, StringHash("slt-checkbox"), 0)));
	UI::ElementHandle rail = FindByClass(context, row, StringHash("slt-slider-rail"), 0);
	ASSERT_TRUE(context.IsAlive(rail));

	DragElementBy(context, rail, static_cast<i32>(context.GetLayout(rail).borderBox.w), 0);
	EXPECT_GT(material.alphaClipThreshold, 0.5f);
	EXPECT_NE(material.keywordHash, unclipped);

	DragElementBy(context, rail, -static_cast<i32>(context.GetLayout(rail).borderBox.w), 0);
	EXPECT_FLOAT_EQ(material.alphaClipThreshold, 0.0f);
	EXPECT_EQ(material.keywordHash, unclipped);
}

TEST_F(MaterialInspectorFixture, TheSortingPriorityRowWritesAnInteger) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);
	MaterialAsset material;
	material.shader = Handle<ShaderAsset>(&shader);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle field = FindByClass(context, RowFor(context, root, "Sorting Priority"),
										  StringHash("slt-number-input"), 0);
	ASSERT_TRUE(context.IsAlive(field));
	ClickElement(context, field);
	ASSERT_EQ(context.GetFocused(), field);
	InputEvent selectAll{};
	selectAll.type = InputEventType::KeyDown;
	selectAll.key = Key::A;
	selectAll.mods = KeyMod::Ctrl;
	context.InjectInput(selectAll);
	for (char c : std::string_view("-3")) {
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

	EXPECT_EQ(material.sortingPriority, -3);
}

namespace {

	class ShaderDropdownFixture : public MaterialInspectorFixture {
	protected:
		std::filesystem::path _project;

		void SetUp() override {
			MaterialInspectorFixture::SetUp();
			if (::testing::Test::IsSkipped()) {
				return;
			}
			static std::atomic<u64> counter{0};
			_project = std::filesystem::temp_directory_path() /
					   ("shalltear_shaderdrop_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_project / "Assets");
			FileSystem::Mount("project", Path(_project.generic_string()));
		}

		void TearDown() override {
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_project, ec);
			MaterialInspectorFixture::TearDown();
		}

		Guid WriteShader(const char* name, const char* source) {
			Path virtualPath = Path(str("project://Assets/") + name);
			FileSystem::WriteTextFile(FileSystem::Resolve(virtualPath), str(source));
			return AssetDatabase::EnsureAssetIdentity(virtualPath);
		}

		static bool Offers(const Vector<MaterialInspectorOps::ShaderChoice>& choices, const Guid& guid) {
			for (const MaterialInspectorOps::ShaderChoice& choice : choices) {
				if (choice.guid == guid) {
					return true;
				}
			}
			return false;
		}
	};

} // anonymous namespace

TEST_F(ShaderDropdownFixture, OnlyMaterialShadersOffered) {
	Guid surface = WriteShader("MyToon.hlsl", "#pragma shalltear_template lit\n");
	Guid annotated = WriteShader("Raw.hlsl",
								 "#pragma shalltear_blend alpha\n"
								 "#pragma shalltear_default tint 1 1 1 1\n");
	Guid passOnly = WriteShader("Overlay.hlsl",
								"#pragma shalltear_blend alpha\n"
								"#pragma shalltear_depth off\n"
								"#pragma shalltear_cull none\n");
	Guid kernel = WriteShader("Simulate.hlsl",
							  "#pragma shalltear_kernel\n"
							  "#pragma shalltear_default tint 1 1 1 1\n");
	ASSERT_TRUE(surface.IsValid());
	ASSERT_TRUE(annotated.IsValid());
	ASSERT_TRUE(passOnly.IsValid());
	ASSERT_TRUE(kernel.IsValid());

	Vector<MaterialInspectorOps::ShaderChoice> choices{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::CollectShaderChoices(choices);

	EXPECT_TRUE(Offers(choices, surface));
	EXPECT_TRUE(Offers(choices, annotated));
	EXPECT_FALSE(Offers(choices, passOnly));
	EXPECT_FALSE(Offers(choices, kernel));
}

TEST_F(ShaderDropdownFixture, NodeShaderOfferedSweptNot) {
	constexpr const char* LIT_GRAPH = R"({"_type":"NodeShader","version":1,"settings":{"target":"lit"},"contexts":[]})";
	constexpr const char* EFFECT_GRAPH = R"({"_type":"EffectGraph","version":1,"contexts":[]})";
	Guid graph = WriteShader("Rock.slshader", LIT_GRAPH);
	Guid effect = WriteShader("Sparks.slshader", EFFECT_GRAPH);
	ASSERT_TRUE(graph.IsValid());
	ASSERT_TRUE(effect.IsValid());

	Vector<MaterialInspectorOps::ShaderChoice> choices{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::CollectShaderChoices(choices);
	EXPECT_TRUE(Offers(choices, graph));
	EXPECT_FALSE(Offers(choices, effect));

	struct ProbeImporter : IAssetImporter {
		StringHash GetAssetType() const override { return StringHash("ShaderAsset"); }
		Asset* Import(const Path&) override { return nullptr; }
	};
	static ProbeImporter importer;
	static const int token = 0;
	AssetImporterRegistry::RegisterExtension(".probeshader", &importer);
	Guid probe = WriteShader("Probe.probeshader", "anything");
	ASSERT_TRUE(probe.IsValid());
	MaterialInspectorOps::CollectShaderChoices(choices);
	EXPECT_FALSE(Offers(choices, probe));
	ASSERT_TRUE(MaterialInspectorOps::RegisterShaderSource(".probeshader", [](std::string_view) { return true; }, &token));
	MaterialInspectorOps::CollectShaderChoices(choices);
	EXPECT_TRUE(Offers(choices, probe));
	EXPECT_FALSE(MaterialInspectorOps::RegisterShaderSource(".probeshader", [](std::string_view) { return false; }, &graph));
	EXPECT_EQ(MaterialInspectorOps::UnregisterShaderSourcesOwnedBy(&token), 1u);
	MaterialInspectorOps::CollectShaderChoices(choices);
	EXPECT_FALSE(Offers(choices, probe));
	AssetImporterRegistry::Unregister(&importer);
}

TEST_F(ShaderDropdownFixture, EngineShadersFirstRawAbsent) {
	Guid project = WriteShader("ZzProject.hlsl", "#pragma shalltear_template unlit\n");
	ASSERT_TRUE(project.IsValid());

	Vector<MaterialInspectorOps::ShaderChoice> choices{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::CollectShaderChoices(choices);
	ASSERT_FALSE(choices.empty());

	bool sawProject = false;
	for (const MaterialInspectorOps::ShaderChoice& choice : choices) {
		if (choice.engine) {
			EXPECT_FALSE(sawProject) << choice.displayName;
		} else {
			sawProject = true;
		}
		EXPECT_NE(choice.displayName, "PresentPass");
		EXPECT_NE(choice.displayName, "UI");
		EXPECT_NE(choice.displayName, "Wireframe");
		EXPECT_NE(choice.displayName, "DebugLines");
		EXPECT_NE(choice.displayName, "EditorGrid");
	}
	EXPECT_TRUE(sawProject);

	bool sawLit = false;
	bool sawUnlit = false;
	for (const MaterialInspectorOps::ShaderChoice& choice : choices) {
		sawLit = sawLit || choice.displayName == "Lit";
		sawUnlit = sawUnlit || choice.displayName == "Unlit";
	}
	EXPECT_TRUE(sawLit);
	EXPECT_TRUE(sawUnlit);
}

TEST_F(ShaderDropdownFixture, ASharedDisplayNameBecomesThePath) {
	Guid shadow = WriteShader("Lit.hlsl", "#pragma shalltear_template lit\n");
	ASSERT_TRUE(shadow.IsValid());

	Vector<MaterialInspectorOps::ShaderChoice> choices{GetMemoryResource(MemTag::Editor)};
	MaterialInspectorOps::CollectShaderChoices(choices);

	for (const MaterialInspectorOps::ShaderChoice& choice : choices) {
		EXPECT_NE(choice.displayName, "Lit");
	}
	bool named = false;
	for (const MaterialInspectorOps::ShaderChoice& choice : choices) {
		named = named || choice.displayName == "project://Assets/Lit.hlsl";
	}
	EXPECT_TRUE(named);
}

TEST_F(ShaderDropdownFixture, PickingShaderFromDropdownAssignsIt) {
	Guid toon = WriteShader("MyToon.hlsl", "#pragma shalltear_template lit\n");
	ASSERT_TRUE(toon.IsValid());

	MaterialAsset material;
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle control = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(control));

	ClickElement(context, control);
	context.Update();
	UI::ElementHandle item = UITest::FindByText(context, context.GetRoot(), "MyToon");
	ASSERT_TRUE(context.IsAlive(item));
	ClickElement(context, item);
	context.Update();

	EXPECT_EQ(material.shaderGuid, toon);
}

TEST_F(ShaderDropdownFixture, KernelNotAShader) {
	Guid toon = WriteShader("MyToon.hlsl", "#pragma shalltear_template lit\n");
	Guid kernel = WriteShader("Simulate.hlsl",
							  "#pragma shalltear_kernel\n"
							  "[[vk::binding(0, 0)]] RWByteAddressBuffer values;\n"
							  "[numthreads(64, 1, 1)]\n"
							  "void CSMain(uint3 id : SV_DispatchThreadID) { values.Store(id.x * 4, id.x); }\n");
	ASSERT_TRUE(toon.IsValid() && kernel.IsValid());
	Asset* probe = AssetDatabase::LoadAssetByGuid(kernel);
	if (!probe) {
		GTEST_SKIP() << "kernel didn't compile (no dxcompiler.dll?)";
	}
	ASSERT_TRUE(static_cast<ShaderAsset*>(probe)->IsKernel());
	AssetDatabase::UnloadAsset(probe);

	MaterialAsset material;
	material.shaderGuid = toon;
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);
	UI::ElementHandle control = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(control));
	auto& events = context.Events(control);
	ASSERT_TRUE(events.onDrop);
	UI::DragPayload payload;
	payload.type = "Asset"_hash;
	payload.data = Variant(AssetDragData{kernel, StringHash("ShaderAsset").GetValue()});
	events.onDrop(payload, {});
	EXPECT_EQ(material.shaderGuid, toon);

	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/OnKernel.mat")),
							  std::format(R"({{"shader": "{}", "floats": {{}}, "colors": {{}}, "textures": {{}}}})", kernel));
	Guid materialGuid = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/OnKernel.mat"));
	Asset* onKernel = AssetDatabase::LoadAssetByGuid(materialGuid);
	ASSERT_NE(onKernel, nullptr);
	ASSERT_EQ(static_cast<MaterialAsset*>(onKernel)->shaderGuid, kernel);
	EXPECT_FALSE(static_cast<MaterialAsset*>(onKernel)->shader.IsValid());
	AssetDatabase::UnloadAsset(onKernel);
}

TEST_F(ShaderDropdownFixture, PickingShaderCompilesVariantMaterialDrawsWith) {
	Guid probe = WriteShader("ClipProbe.hlsl",
							 "#pragma shalltear_template unlit\n"
							 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; };\n"
							 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							 "\tsurface.baseColor = baseColor.rgb;\n"
							 "\tsurface.alpha = baseColor.a;\n"
							 "}\n");
	ASSERT_TRUE(probe.IsValid());
	Asset* compiled = AssetDatabase::LoadAssetByGuid(probe);
	if (!compiled) {
		GTEST_SKIP() << "probe shader didn't compile (no dxcompiler.dll?)";
	}

	MaterialAsset material;
	material.SetAlphaClipThreshold(0.5f);
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle control = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(control));
	ClickElement(context, control);
	context.Update();
	UI::ElementHandle item = UITest::FindByText(context, context.GetRoot(), "ClipProbe");
	ASSERT_TRUE(context.IsAlive(item));
	const ShaderCompileStats before = ShaderCompileQueue::GetStats();
	ClickElement(context, item);
	context.Update();
	EXPECT_EQ(ShaderCompileQueue::GetStats().compilesElsewhere, before.compilesElsewhere);

	ShaderAsset* shader = material.shader.Get();
	ASSERT_NE(shader, nullptr);
	for (int frame = 0; frame < 600 && shader->FindVariant(material.keywordHash) == SHADER_BASE_VARIANT; ++frame) {
		EditorVariantCompiles::Drain();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_NE(shader->FindVariant(material.keywordHash), SHADER_BASE_VARIANT);
	EditorVariantCompiles::Terminate();
	AssetDatabase::UnloadAsset(compiled);
}

TEST_F(ShaderDropdownFixture, PickedShaderAppliesToggles) {
	Guid toggles = WriteShader("Toggles.hlsl",
							   "#pragma shalltear_template unlit\n"
							   "#pragma shalltear_keyword _BANDS bandsOn\n"
							   "#pragma shalltear_default bandsOn 1\n"
							   "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 baseColor; int bandsOn; };\n"
							   "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							   "#ifdef _BANDS\n"
							   "\tsurface.baseColor = baseColor.rgb * 0.5;\n"
							   "#else\n"
							   "\tsurface.baseColor = baseColor.rgb;\n"
							   "#endif\n"
							   "}\n");
	ASSERT_TRUE(toggles.IsValid());
	Asset* compiled = AssetDatabase::LoadAssetByGuid(toggles);
	if (!compiled) {
		GTEST_SKIP() << "toggle shader didn't compile (no dxcompiler.dll?)";
	}
	auto banded = [](const MaterialAsset& material) {
		return std::find(material.keywords.begin(), material.keywords.end(), "_BANDS") != material.keywords.end();
	};
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};

	MaterialAsset opened;
	opened.shaderGuid = toggles;
	opened.shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(compiled));
	{
		UI::Context context(desc, UISystem::GetFontSystem());
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		BuildInspector(context, root, opened);
	}
	const MaterialAsset::Param* member = opened.FindParam(StringHash("bandsOn"));
	ASSERT_NE(member, nullptr);
	EXPECT_EQ(member->integer, 1);
	EXPECT_FALSE(banded(opened));

	MaterialAsset picked;
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, picked);
	UI::ElementHandle control = FindByClass(context, root, StringHash("slt-dropdown"), 0);
	ASSERT_TRUE(context.IsAlive(control));
	ClickElement(context, control);
	context.Update();
	UI::ElementHandle item = UITest::FindByText(context, context.GetRoot(), "Toggles");
	ASSERT_TRUE(context.IsAlive(item));
	ClickElement(context, item);
	context.Update();
	EXPECT_EQ(picked.shaderGuid, toggles);
	EXPECT_TRUE(banded(picked));

	EditorVariantCompiles::Terminate();
	AssetDatabase::UnloadAsset(compiled);
}

TEST_F(ShaderDropdownFixture, TextureSlotTakesOnlyTextureOfDimension) {
	Guid kinds = WriteShader("Kinds.hlsl",
							 "#pragma shalltear_template unlit\n"
							 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
							 "[[vk::binding(1, 1)]] Texture2DArray layers;\n"
							 "[[vk::binding(2, 1)]] SamplerState layersSampler;\n"
							 "[[vk::binding(3, 1)]] TextureCube sky;\n"
							 "[[vk::binding(4, 1)]] SamplerState skySampler;\n"
							 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							 "\tsurface.baseColor = layers.SampleLevel(layersSampler, float3(0.5, 0.5, 0.0), 0).rgb\n"
							 "\t\t+ sky.SampleLevel(skySampler, float3(0.0, 0.0, 1.0), 0).rgb;\n"
							 "}\n");
	ASSERT_TRUE(kinds.IsValid());
	Asset* shader = AssetDatabase::LoadAssetByGuid(kinds);
	if (!shader) {
		GTEST_SKIP() << "shader didn't compile (no dxcompiler.dll?)";
	}
	const u8 red[4] = {255, 0, 0, 255};
	TestImages::WriteTGA(_project / "Assets" / "Flat.tga", red);
	TestImages::WriteFlipbook(_project / "Assets" / "Flip.tga");
	const Guid flat = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/Flat.tga"));
	const Guid flip = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/Flip.tga"));
	JSONValue settings = JSONValue::MakeObject();
	settings.AddMember("shape", JSONValue::MakeString("2darray"));
	settings.AddMember("columns", JSONValue::MakeNumber(2.0));
	settings.AddMember("rows", JSONValue::MakeNumber(2.0));
	ASSERT_TRUE(AssetDatabase::WriteImporterSettings(Path("project://Assets/Flip.tga"), settings));

	MaterialAsset material;
	material.shaderGuid = kinds;
	material.shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(shader));
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, material);

	UI::ElementHandle arrayField;
	UI::ElementHandle cubeField;
	for (u32 skip = 0;; ++skip) {
		UI::ElementHandle field = FindByClass(context, root, EditorTheme::Class::ObjectField, skip);
		if (!context.IsAlive(field)) {
			break;
		}
		if (context.IsAlive(UITest::FindByText(context, field, "None (gray 2D array)"))) {
			arrayField = field;
		} else if (context.IsAlive(UITest::FindByText(context, field, "None (gray cube)"))) {
			cubeField = field;
		}
	}
	ASSERT_TRUE(context.IsAlive(arrayField));
	ASSERT_TRUE(context.IsAlive(cubeField));

	auto payloadFor = [](const Guid& texture) {
		UI::DragPayload payload;
		payload.type = "Asset"_hash;
		payload.data = Variant(AssetDragData{texture, StringHash("TextureAsset").GetValue()});
		return payload;
	};
	auto& arrayEvents = context.Events(arrayField);
	auto& cubeEvents = context.Events(cubeField);
	ASSERT_TRUE(arrayEvents.onDragOver && arrayEvents.onDrop && cubeEvents.onDragOver);
	EXPECT_TRUE(arrayEvents.onDragOver(payloadFor(flip)));
	EXPECT_FALSE(arrayEvents.onDragOver(payloadFor(flat)));
	EXPECT_FALSE(cubeEvents.onDragOver(payloadFor(flat)));
	EXPECT_FALSE(cubeEvents.onDragOver(payloadFor(flip)));

	auto holds = [&material](std::string_view slot, const Guid& texture) {
		for (const MaterialAsset::TextureParam& param : material.textures) {
			if (param.name == slot && param.texture == texture) {
				return true;
			}
		}
		return false;
	};
	arrayEvents.onDrop(payloadFor(flat), {});
	EXPECT_FALSE(holds("layers", flat));
	arrayEvents.onDrop(payloadFor(flip), {});
	EXPECT_TRUE(holds("layers", flip));
	AssetDatabase::UnloadAsset(shader);
}

TEST_F(ShaderDropdownFixture, DroppedTextureIsHeld) {
	Guid flatShader = WriteShader("Flat.hlsl",
								  "#pragma shalltear_template unlit\n"
								  "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 unused; };\n"
								  "[[vk::binding(1, 1)]] Texture2D flat;\n"
								  "[[vk::binding(2, 1)]] SamplerState flatSampler;\n"
								  "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
								  "\tsurface.baseColor = flat.SampleLevel(flatSampler, float2(0.5, 0.5), 0).rgb;\n"
								  "}\n");
	ASSERT_TRUE(flatShader.IsValid());
	Asset* shader = AssetDatabase::LoadAssetByGuid(flatShader);
	if (!shader) {
		GTEST_SKIP() << "shader didn't compile (no dxcompiler.dll?)";
	}
	const u8 red[4] = {255, 0, 0, 255};
	TestImages::WriteTGA(_project / "Assets" / "Red.tga", red);
	const Guid texture = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/Red.tga"));
	ASSERT_TRUE(texture.IsValid());
	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/Flat.mat")),
							  std::format(R"({{"shader": "{}", "floats": {{}}, "colors": {{}}, "textures": {{}}}})", flatShader));
	Asset* loaded = AssetDatabase::LoadAssetByGuid(AssetDatabase::EnsureAssetIdentity(Path("project://Assets/Flat.mat")));
	ASSERT_NE(loaded, nullptr);
	MaterialAsset& material = *static_cast<MaterialAsset*>(loaded);

	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context first(desc, UISystem::GetFontSystem());
	UI::Context second(desc, UISystem::GetFontSystem());
	UI::ElementHandle firstRoot = first.CreateElement();
	UI::ElementHandle secondRoot = second.CreateElement();
	first.AppendChild(first.GetRoot(), firstRoot);
	second.AppendChild(second.GetRoot(), secondRoot);
	BuildInspector(first, firstRoot, material);
	BuildInspector(second, secondRoot, material);

	auto fieldIn = [](UI::Context& context, UI::ElementHandle root) {
		for (u32 skip = 0;; ++skip) {
			UI::ElementHandle field = FindByClass(context, root, EditorTheme::Class::ObjectField, skip);
			if (!context.IsAlive(field) || context.IsAlive(UITest::FindByText(context, field, "None (white)"))) {
				return field;
			}
		}
	};
	UI::ElementHandle firstField = fieldIn(first, firstRoot);
	UI::ElementHandle secondField = fieldIn(second, secondRoot);
	ASSERT_TRUE(first.IsAlive(firstField));
	ASSERT_TRUE(second.IsAlive(secondField));

	UI::DragPayload payload;
	payload.type = "Asset"_hash;
	payload.data = Variant(AssetDragData{texture, StringHash("TextureAsset").GetValue()});
	auto& events = first.Events(firstField);
	ASSERT_TRUE(events.onDragOver && events.onDrop);
	ASSERT_TRUE(events.onDragOver(payload));
	events.onDrop(payload, {});

	Asset* held = AssetDatabase::FindLoadedByGuid(texture);
	ASSERT_NE(held, nullptr);
	EXPECT_EQ(material.GetTexture(StringHash("flat")).GetInstanceId(), held->GetInstanceId());
	PropertyDrawers::RefreshRows(first);
	PropertyDrawers::RefreshRows(second);
	EXPECT_TRUE(first.IsAlive(UITest::FindByText(first, firstField, "Red.tga")));
	EXPECT_TRUE(second.IsAlive(UITest::FindByText(second, secondField, "Red.tga")));

	UI::ElementHandle clear = UITest::FindByText(second, secondField, "\xC3\x97");
	ASSERT_TRUE(second.IsAlive(clear));
	ClickElement(second, clear);
	second.Update();
	EXPECT_EQ(material.GetTexture(StringHash("flat")).GetInstanceId(), 0u);
	PropertyDrawers::RefreshRows(first);
	PropertyDrawers::RefreshRows(second);
	EXPECT_TRUE(first.IsAlive(UITest::FindByText(first, firstField, "None (white)")));
	EXPECT_TRUE(second.IsAlive(UITest::FindByText(second, secondField, "None (white)")));

	AssetDatabase::UnloadAsset(loaded);
	AssetDatabase::UnloadAsset(shader);
}

namespace {

	constexpr std::string_view NormalWarning = "Texture not imported as a normal map";

	UI::ElementHandle ParentOf(UI::Context& context, UI::ElementHandle element) {
		const UI::Element* found = context.GetElement(element);
		return found ? context.MakeHandle(found->parent) : UI::ElementHandle{};
	}

	bool NormalWarningShown(UI::Context& context, UI::ElementHandle root) {
		const UI::ElementHandle row = ParentOf(context, UITest::FindByText(context, root, NormalWarning));
		return context.IsAlive(row) && context.GetStyle(row).display != UI::Display::None;
	}

	class NormalMapFixFixture : public ShaderDropdownFixture {
	protected:
		Path _texturePath{"project://Assets/Bumps.tga"};
		Guid _texture;
		Asset* _material = nullptr;
		ShaderAsset _shader;

		void SetUp() override {
			ShaderDropdownFixture::SetUp();
			if (::testing::Test::IsSkipped()) {
				return;
			}
			const u8 red[4] = {255, 0, 0, 255};
			TestImages::WriteTGA(_project / "Assets" / "Bumps.tga", red);
			_texture = AssetDatabase::EnsureAssetIdentity(_texturePath);
			ASSERT_TRUE(_texture.IsValid());
			JSONValue settings = JSONValue::MakeObject();
			settings.AddMember("filter", JSONValue::MakeString("point"));
			settings.AddMember("compression", JSONValue::MakeString("none"));
			settings.AddMember("future", JSONValue::MakeString("kept"));
			ASSERT_TRUE(AssetDatabase::WriteImporterSettings(_texturePath, settings));

			FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/Bumpy.mat")), str(R"({"floats": {}, "colors": {}, "textures": {}})"));
			_material = AssetDatabase::LoadAssetByGuid(AssetDatabase::EnsureAssetIdentity(Path("project://Assets/Bumpy.mat")));
			ASSERT_NE(_material, nullptr);
			DeclareScalarInterface(_shader);
			_shader.textureDefaults.push_back({"baseColorTexture", ShaderTextureFallback::Normal});
			Material().shader = Handle<ShaderAsset>(&_shader);
		}

		void TearDown() override {
			if (_material) {
				Material().shader = {};
				AssetDatabase::UnloadAsset(_material);
			}
			ShaderDropdownFixture::TearDown();
		}

		MaterialAsset& Material() const { return *static_cast<MaterialAsset*>(_material); }

		TextureAsset* Texture() const { return static_cast<TextureAsset*>(AssetDatabase::FindLoadedByGuid(_texture)); }

		str Saved(std::string_view key) const {
			const str text = FileSystem::ReadTextFile(FileSystem::Resolve(AssetDatabase::GetMetaPath(_texturePath)), MemTag::Editor);
			LinearAllocator arena(text.size() + 64, MemTag::Editor);
			const JSONValue root = JSONParse(text, arena);
			return str(root["importer"]["settings"][key].AsString());
		}

		UI::DragPayload Payload() const {
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(AssetDragData{_texture, StringHash("TextureAsset").GetValue()});
			return payload;
		}
	};

} // anonymous namespace

TEST_F(NormalMapFixFixture, FixNowMarksNormalMap) {
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	UI::Context first(desc, UISystem::GetFontSystem());
	UI::Context second(desc, UISystem::GetFontSystem());
	UI::ElementHandle firstRoot = first.CreateElement();
	UI::ElementHandle secondRoot = second.CreateElement();
	first.AppendChild(first.GetRoot(), firstRoot);
	second.AppendChild(second.GetRoot(), secondRoot);
	BuildInspector(first, firstRoot, Material());
	BuildInspector(second, secondRoot, Material());
	EXPECT_FALSE(NormalWarningShown(first, firstRoot));
	EXPECT_FALSE(NormalWarningShown(second, secondRoot));

	UI::ElementHandle field = ParentOf(first, UITest::FindByText(first, firstRoot, "None (normal)"));
	while (first.IsAlive(field) && !first.HasClass(field, EditorTheme::Class::ObjectField)) {
		field = ParentOf(first, field);
	}
	ASSERT_TRUE(first.IsAlive(field));
	auto& events = first.Events(field);
	ASSERT_TRUE(events.onDragOver && events.onDrop);
	ASSERT_TRUE(events.onDragOver(Payload()));
	events.onDrop(Payload(), {});
	ASSERT_NE(Texture(), nullptr);
	ASSERT_EQ(Texture()->settings.type, TextureType::Default);

	PropertyDrawers::RefreshRows(first);
	PropertyDrawers::RefreshRows(second);
	first.Update();
	second.Update();
	EXPECT_TRUE(NormalWarningShown(first, firstRoot));
	ASSERT_TRUE(NormalWarningShown(second, secondRoot));

	const TextureAsset* before = Texture();
	ClickElement(second, ParentOf(second, UITest::FindByText(second, secondRoot, "Fix Now")));
	EXPECT_EQ(Saved("textureType"), "normalMap");
	EXPECT_EQ(Saved("filter"), "point");
	EXPECT_EQ(Saved("compression"), "none");
	EXPECT_EQ(Saved("future"), "kept");
	ASSERT_NE(Texture(), nullptr);
	EXPECT_EQ(Texture(), before);
	EXPECT_EQ(Texture()->settings.type, TextureType::NormalMap);
	EXPECT_EQ(Material().GetTexture(StringHash("baseColorTexture")).Get(), before);

	PropertyDrawers::RefreshRows(first);
	PropertyDrawers::RefreshRows(second);
	EXPECT_FALSE(NormalWarningShown(first, firstRoot));
	EXPECT_FALSE(NormalWarningShown(second, secondRoot));
}

TEST_F(NormalMapFixFixture, NormalWarningOnlyUnderNormalSlot) {
	UI::ContextDesc desc;
	desc.sizePx = {420, 900};
	Material().SetTexture("baseColorTexture", _texture);
	ASSERT_NE(Texture(), nullptr);

	ShaderAsset plain;
	DeclareScalarInterface(plain);
	Material().shader = Handle<ShaderAsset>(&plain);
	UI::Context plainContext(desc, UISystem::GetFontSystem());
	UI::ElementHandle plainRoot = plainContext.CreateElement();
	plainContext.AppendChild(plainContext.GetRoot(), plainRoot);
	BuildInspector(plainContext, plainRoot, Material());
	PropertyDrawers::RefreshRows(plainContext);
	EXPECT_TRUE(plainContext.IsAlive(UITest::FindByText(plainContext, plainRoot, "Bumps.tga")));
	EXPECT_FALSE(plainContext.IsAlive(UITest::FindByText(plainContext, plainRoot, NormalWarning)));

	Material().shader = Handle<ShaderAsset>(&_shader);
	UI::Context context(desc, UISystem::GetFontSystem());
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	BuildInspector(context, root, Material());
	ASSERT_TRUE(NormalWarningShown(context, root));

	ClickElement(context, UITest::FindByText(context, root, "\xC3\x97"));
	context.Update();
	ASSERT_EQ(Material().GetTexture(StringHash("baseColorTexture")).Get(), nullptr);
	PropertyDrawers::RefreshRows(context);
	EXPECT_FALSE(NormalWarningShown(context, root));
}

TEST_F(ShaderDropdownFixture, LateVariantUsesNewImport) {
	Guid keyed = WriteShader("Restale.hlsl",
							 "#pragma shalltear_template unlit\n"
							 "#pragma shalltear_keyword MOSS\n"
							 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 tint; };\n"
							 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							 "#ifdef MOSS\n"
							 "\tsurface.baseColor = tint.gbr;\n"
							 "#else\n"
							 "\tsurface.baseColor = tint.rgb;\n"
							 "#endif\n"
							 "}\n");
	ASSERT_TRUE(keyed.IsValid());
	Asset* loaded = AssetDatabase::LoadAssetByGuid(keyed);
	if (!loaded) {
		GTEST_SKIP() << "keyed shader didn't compile (no dxcompiler.dll?)";
	}
	ShaderAsset* shader = static_cast<ShaderAsset*>(loaded);
	const str moss[] = {str("MOSS")};
	const u64 hash = ShaderAsset::KeywordSetHash(moss);
	const ShaderCompileStats before = ShaderCompileQueue::GetStats();
	EditorVariantCompiles::Request(*shader, moss);
	++shader->version;
	for (int frame = 0; frame < 600 && shader->FindVariant(hash) == SHADER_BASE_VARIANT; ++frame) {
		EditorVariantCompiles::Drain();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_NE(shader->FindVariant(hash), SHADER_BASE_VARIANT);
	EXPECT_EQ(ShaderCompileQueue::GetStats().submitted - before.submitted, 2u);
	EditorVariantCompiles::Terminate();
	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(ShaderDropdownFixture, AdoptedVariantMakesPicturesOfShaderStale) {
	Guid keyed = WriteShader("Lands.hlsl",
							 "#pragma shalltear_template unlit\n"
							 "#pragma shalltear_keyword MOSS\n"
							 "[[vk::binding(0, 1)]] cbuffer ShalltearMaterial { float4 tint; };\n"
							 "void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {\n"
							 "#ifdef MOSS\n"
							 "\tsurface.baseColor = tint.gbr;\n"
							 "#else\n"
							 "\tsurface.baseColor = tint.rgb;\n"
							 "#endif\n"
							 "}\n");
	ASSERT_TRUE(keyed.IsValid());
	Asset* loaded = AssetDatabase::LoadAssetByGuid(keyed);
	if (!loaded) {
		GTEST_SKIP() << "keyed shader didn't compile (no dxcompiler.dll?)";
	}
	ShaderAsset* shader = static_cast<ShaderAsset*>(loaded);

	struct Up {
		Up() {
			EditorPreviews::Initialize();
			EditorPreviewStore::Initialize();
		}
		~Up() {
			EditorVariantCompiles::Terminate();
			EditorPreviewStore::Terminate();
			EditorPreviews::Terminate();
		}
	} up;
	FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/drawn.txt")), "d");
	Guid drawn = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/drawn.txt"));
	ASSERT_TRUE(drawn.IsValid());
	auto produced = std::make_shared<int>(0);
	EditorPreviews::RegisterProducer(StringHash("TextAsset"), [produced, keyed](const AssetPreviewRequest&, AssetPreviewImage& out) {
		static constexpr u8 PIXELS[4] = {255, 255, 255, 255};
		out.size = {1, 1};
		out.pixelsFormat = RHI::Format::RGBA8Unorm;
		out.pixels = PIXELS;
		out.detail = std::format("produce {}", ++*produced);
		out.dependencies.push_back(keyed);
		return true;
	});
	AssetPreviewEntry entry;
	for (int frame = 0; frame < 4 && !EditorPreviewStore::Request(drawn, entry); ++frame) {
		EditorPreviewStore::Drain();
	}
	ASSERT_TRUE(EditorPreviewStore::Request(drawn, entry));
	ASSERT_EQ(entry.detail, "produce 1");

	const str moss[] = {str("MOSS")};
	const u64 hash = ShaderAsset::KeywordSetHash(moss);
	EditorVariantCompiles::Request(*shader, moss);
	EditorPreviewStore::Request(drawn, entry);
	EXPECT_EQ(EditorPreviewStore::GetPendingCount(), 0u);
	for (int frame = 0; frame < 600 && shader->FindVariant(hash) == SHADER_BASE_VARIANT; ++frame) {
		EditorVariantCompiles::Drain();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_NE(shader->FindVariant(hash), SHADER_BASE_VARIANT);

	EXPECT_TRUE(EditorPreviewStore::Request(drawn, entry));
	EXPECT_EQ(EditorPreviewStore::GetPendingCount(), 1u);
	EditorPreviewStore::Drain();
	ASSERT_TRUE(EditorPreviewStore::Request(drawn, entry));
	EXPECT_EQ(entry.detail, "produce 2");
	AssetDatabase::UnloadAsset(loaded);
}

namespace {

	class MaterialProductImporter : public IAssetImporter {
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
			MaterialAsset* material = New<MaterialAsset>(MemTag::Asset);
			material->SetColor("baseColor", Color::White());
			material->SetFloat("specular", 0.5f);
			context.AddProduct("Materials/Skin", material);
			return Import(sourcePath);
		}
	};

	class MaterialProductFixture : public MaterialInspectorFixture {
	protected:
		std::filesystem::path _project;
		MaterialProductImporter _importer;

		void SetUp() override {
			MaterialInspectorFixture::SetUp();
			if (::testing::Test::IsSkipped()) {
				return;
			}
			static std::atomic<u64> counter{0};
			_project = std::filesystem::temp_directory_path() /
					   ("shalltear_matprod_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_project / "Assets");
			FileSystem::Mount("project", Path(_project.generic_string()));
			MaterialAsset::GetReflectType();
			AssetImporterRegistry::Register(StringHash("MatProbeAsset"), &_importer, {".submat"});
		}

		void TearDown() override {
			AssetImporterRegistry::Unregister(&_importer);
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_project, ec);
			MaterialInspectorFixture::TearDown();
		}

		Guid ImportAndTakeTheProduct() {
			FileSystem::WriteTextFile(FileSystem::Resolve(Path("project://Assets/rig.submat")), "x");
			Asset* source = AssetDatabase::LoadAsset(Path("project://Assets/rig.submat"));
			EXPECT_NE(source, nullptr);
			if (source) {
				AssetDatabase::UnloadAsset(source);
			}
			AssetDatabase::CollectGarbage();
			Guid sourceGuid = AssetDatabase::LookupGuid(Path("project://Assets/rig.submat"));
			Vector<AssetProduct> products{GetMemoryResource(MemTag::Asset)};
			AssetDatabase::GetProducts(sourceGuid, products);
			return products.empty() ? Guid{} : products[0].guid;
		}

		bool SaveButtonShownFor(const Guid& guid, ShaderAsset& shader) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			EXPECT_NE(asset, nullptr);
			if (!asset) {
				return false;
			}
			MaterialAsset* material = static_cast<MaterialAsset*>(asset);
			material->shader = Handle<ShaderAsset>(&shader);

			UI::ContextDesc desc;
			desc.sizePx = {420, 900};
			UI::Context context(desc, UISystem::GetFontSystem());
			UI::ElementHandle root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			const AssetInspectorFn* inspector = PropertyDrawers::FindAssetInspector(StringHash("MaterialAsset"));
			EXPECT_NE(inspector, nullptr);
			bool shown = false;
			if (inspector) {
				(*inspector)(context, root, guid, *material);
				context.Update();
				shown = context.IsAlive(FindByText(context, root, "Save"));
			}
			material->shader = {};
			AssetDatabase::UnloadAsset(asset);
			return shown;
		}
	};

} // anonymous namespace

TEST_F(MaterialProductFixture, OnlyExtractedProductSaves) {
	ShaderAsset shader;
	DeclareScalarInterface(shader);

	Guid product = ImportAndTakeTheProduct();
	ASSERT_TRUE(product.IsValid());
	ASSERT_TRUE(AssetDatabase::LookupPath(product).ToString().empty());

	EXPECT_FALSE(SaveButtonShownFor(product, shader));

	ASSERT_TRUE(AssetDatabase::ExtractProduct(product, Path("project://Assets/Skin.mat")));
	EXPECT_TRUE(SaveButtonShownFor(product, shader));
}
