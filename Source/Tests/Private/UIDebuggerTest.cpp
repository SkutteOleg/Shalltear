#include "TestsPCH.h"

#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/Docking.h"
#include "Shalltear/Core/UI/StyleSheetAsset.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/UI/WidgetRegistry.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/UIBuilderOps.h"
#include "Shalltear/Editor/UIDebugOps.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;
using namespace UITest;

namespace {

	class UIDebuggerTest : public ::testing::Test {
	protected:
		void SetUp() override {
			TypeRegistry::InitializePrimitives();
		}

		static void DressLikeTheEditor(UI::Context& context) {
			EditorTheme::ApplyToContext(context);
		}

		static void LoadProbeSheet(UI::Context& context, UIDebugOps::NameDictionary& names, std::string_view name,
								   std::string_view source) {
			context.LoadStyleSheetOnce(StringHash(name), source);
			names.AddCandidateSheet(name, source);
		}

		static UI::ElementHandle Child(UI::Context& context, UI::ElementHandle parent) {
			UI::ElementHandle handle = context.CreateElement();
			context.AppendChild(parent, handle);
			return handle;
		}

		static size_t RowOf(const UIDebugOps::Snapshot& snapshot, UI::ElementHandle element) {
			for (size_t i = 0; i < snapshot.rows.size(); ++i) {
				if (snapshot.rows[i].element == element) {
					return i;
				}
			}
			return static_cast<size_t>(-1);
		}
	};

	TEST_F(UIDebuggerTest, EveryPropertyRoundTripsThroughParser) {
		UI::Style style;
		style.display = UI::Display::None;
		style.position = UI::Position::Absolute;
		style.flexDirection = UI::FlexDirection::Column;
		style.justifyContent = UI::Align::SpaceBetween;
		style.alignItems = UI::Align::Center;
		style.alignSelf = UI::Align::End;
		style.overflow = UI::Overflow::Scroll;
		style.width = UI::Length::Pixel(120.5f);
		style.height = UI::Length::Percent(0.5f);
		style.minWidth = UI::Length::Pixel(3.0f);
		style.minHeight = UI::Length::Percent(0.25f);
		style.maxWidth = UI::Length::Pixel(900.0f);
		style.maxHeight = UI::Length::Auto();
		style.margin = {1.0f, 2.0f, 3.0f, 4.0f};
		style.padding = UI::Edges::All(6.0f);
		style.flexGrow = 2.5f;
		style.flexShrink = 0.0f;
		style.left = UI::Length::Pixel(-8.0f);
		style.top = UI::Length::Percent(1.0f);
		style.right = UI::Length::Pixel(0.0f);
		style.bottom = std::nullopt;
		style.background = UI::Brush::Gradient(Color::FromHex("#112233ff"), Color::FromHex("#445566aa"), {0.0f, 1.0f});
		style.borderColor = Color::FromHex("#ff0000cc");
		style.borderWidth = {1.0f, 2.0f, 1.0f, 2.0f};
		style.borderRadius = {4.0f, 5.0f, 6.0f, 7.0f};
		style.AddBoxShadow(UI::BoxShadow{1.0f, 2.0f, 3.0f, 4.0f, Color::FromHex("#00000080"), false});
		style.AddBoxShadow(UI::BoxShadow{-1.0f, -2.0f, 0.0f, 0.0f, Color::FromHex("#ffffff40"), true});
		style.opacity = 0.75f;
		style.zIndex = -3;
		style.textAlign = UI::Align::Center;
		style.textAlignVertical = UI::Align::End;
		style.textOverflow = UI::TextOverflow::Ellipsis;
		style.textOutlineWidth = 1.5f;
		style.textOutlineColor = Color::FromHex("#0000ff80");
		style.letterSpacing = -0.5f;
		style.pointerEvents = false;
		style.cursor = SystemCursor::ResizeEW;

		char buffer[UI::MAX_STYLE_VALUE_TEXT];
		for (std::string_view name : UI::GetStylePropertyNames()) {
			std::string_view text = UI::ReadStyleProperty(style, StringHash(name), buffer, sizeof(buffer));
			ASSERT_FALSE(text.empty()) << name << "'";

			UI::StyleRule rule;
			rule.properties.push_back(UI::StyleProperty{StringHash(name), str(text), false});
			UI::Style applied;
			UI::StyleSheet::ApplyRule(applied, rule);

			char echo[UI::MAX_STYLE_VALUE_TEXT];
			std::string_view second = UI::ReadStyleProperty(applied, StringHash(name), echo, sizeof(echo));
			EXPECT_EQ(text, second) << "'" << name << ' ' << text << "' -> '" << second << "'";
		}
	}

	TEST_F(UIDebuggerTest, AnUnknownPropertyNameReadsEmpty) {
		UI::Style style;
		char buffer[UI::MAX_STYLE_VALUE_TEXT];
		EXPECT_TRUE(UI::ReadStyleProperty(style, StringHash("border-collapse"), buffer, sizeof(buffer)).empty());
		EXPECT_TRUE(UI::ReadStyleProperty(style, StringHash(""), buffer, sizeof(buffer)).empty());
	}

	TEST_F(UIDebuggerTest, EdgeOffsetAutoOr0) {
		UI::Style style;
		char buffer[UI::MAX_STYLE_VALUE_TEXT];
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("left"), buffer, sizeof(buffer)), "auto");
		style.left = UI::Length::Pixel(0.0f);
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("left"), buffer, sizeof(buffer)), "0px");
	}

	TEST_F(UIDebuggerTest, ShorthandAndLonghandsBothShowValue) {
		UI::Style style;
		style.margin = {4.0f, 1.0f, 2.0f, 3.0f};
		char buffer[UI::MAX_STYLE_VALUE_TEXT];
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("margin"), buffer, sizeof(buffer)), "1px 2px 3px 4px");
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("margin-left"), buffer, sizeof(buffer)), "4px");
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("margin-top"), buffer, sizeof(buffer)), "1px");

		style.padding = UI::Edges::All(5.0f);
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("padding"), buffer, sizeof(buffer)), "5px");

		style.background = UI::Brush::Gradient(Color::FromHex("#010203ff"), Color::FromHex("#040506ff"));
		style.background.solid = Color::FromHex("#0a0b0cff");
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("background"), buffer, sizeof(buffer)),
				  "linear-gradient(#010203ff, #040506ff, to right)");
		EXPECT_EQ(UI::ReadStyleProperty(style, StringHash("background-color"), buffer, sizeof(buffer)), "#0a0b0cff");
	}

	TEST_F(UIDebuggerTest, LongValueTruncated) {
		UI::Style style;
		for (u32 i = 0; i < UI::Style::MAX_BOX_SHADOWS; ++i) {
			style.AddBoxShadow(UI::BoxShadow{100.0f, 200.0f, 300.0f, 400.0f, Color::FromHex("#12345678"), true});
		}
		char guarded[24];
		guarded[20] = '\xAB';
		std::string_view text = UI::ReadStyleProperty(style, StringHash("box-shadow"), guarded, 20);
		EXPECT_EQ(text.size(), 20u);
		EXPECT_EQ(guarded[20], '\xAB');
	}

	TEST_F(UIDebuggerTest, TreeRevisionMovesOnStructureAndIdentity) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		u32 before = context.GetTreeRevision();
		EXPECT_NE(before, 0u);

		UI::ElementHandle a = context.CreateElement();
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.AppendChild(context.GetRoot(), a);
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.AddClass(a, StringHash("row"));
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.RemoveClass(a, StringHash("row"));
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.SetId(a, StringHash("thing"));
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.SetTypeTag(a, StringHash("button"));
		EXPECT_NE(context.GetTreeRevision(), before);

		before = context.GetTreeRevision();
		context.DestroyElement(a);
		EXPECT_NE(context.GetTreeRevision(), before);
	}

	TEST_F(UIDebuggerTest, RevisionIgnoresStyleTextHover) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle box = Child(context, context.GetRoot());
		context.SetStyle(box, FixedBox(100.0f, 50.0f));
		context.SetText(box, {}, 12.0f, Color::White(), "hello");
		context.Update();

		u32 before = context.GetTreeRevision();
		UI::Style moved = context.GetStyle(box);
		moved.padding = UI::Edges::All(9.0f);
		context.SetStyle(box, moved);
		EXPECT_EQ(context.GetTreeRevision(), before);

		context.SetTextString(box, "goodbye");
		EXPECT_EQ(context.GetTreeRevision(), before);

		context.LoadStyleSheet(".x { opacity: 0.5; }");
		EXPECT_EQ(context.GetTreeRevision(), before);

		context.SetStyleVar(StringHash("--slt-accent"), "#123456");
		EXPECT_EQ(context.GetTreeRevision(), before);

		InputEvent move{};
		move.type = InputEventType::MouseMove;
		move.mousePosPx = {10, 10};
		context.InjectInput(move);
		context.Update();
		ASSERT_TRUE(context.GetHovered().IsValid());
		EXPECT_EQ(context.GetTreeRevision(), before);
	}

	TEST_F(UIDebuggerTest, SheetTagsInLoadOrder) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		EXPECT_TRUE(context.GetLoadedSheetTags().empty());

		context.LoadStyleSheetOnce(StringHash("first"), ".a { opacity: 1; }");
		context.LoadStyleSheetOnce(StringHash("second"), ".b { opacity: 1; }");
		context.LoadStyleSheetOnce(StringHash("first"), ".c { opacity: 1; }");

		std::span<const StringHash> tags = context.GetLoadedSheetTags();
		ASSERT_EQ(tags.size(), 2u);
		EXPECT_EQ(tags[0], StringHash("first"));
		EXPECT_EQ(tags[1], StringHash("second"));

		context.ClearStyleSheets();
		EXPECT_TRUE(context.GetLoadedSheetTags().empty());
	}

	TEST_F(UIDebuggerTest, SnapshotIsPreOrder) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.GetRoot();
		UI::ElementHandle a = Child(context, root);
		UI::ElementHandle a0 = Child(context, a);
		UI::ElementHandle a1 = Child(context, a);
		UI::ElementHandle a00 = Child(context, a0);
		UI::ElementHandle b = Child(context, root);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);

		ASSERT_EQ(snapshot.rows.size(), 6u);
		EXPECT_EQ(snapshot.rows[0].element, root);
		EXPECT_EQ(snapshot.rows[1].element, a);
		EXPECT_EQ(snapshot.rows[2].element, a0);
		EXPECT_EQ(snapshot.rows[3].element, a00);
		EXPECT_EQ(snapshot.rows[4].element, a1);
		EXPECT_EQ(snapshot.rows[5].element, b);

		EXPECT_EQ(snapshot.rows[0].depth, 0u);
		EXPECT_EQ(snapshot.rows[1].depth, 1u);
		EXPECT_EQ(snapshot.rows[2].depth, 2u);
		EXPECT_EQ(snapshot.rows[3].depth, 3u);
		EXPECT_EQ(snapshot.rows[4].depth, 2u);
		EXPECT_EQ(snapshot.rows[5].depth, 1u);

		EXPECT_EQ(snapshot.rows[0].childCount, 2u);
		EXPECT_EQ(snapshot.rows[1].childCount, 2u);
		EXPECT_EQ(snapshot.rows[3].childCount, 0u);
		EXPECT_EQ(snapshot.revision, context.GetTreeRevision());
	}

	TEST_F(UIDebuggerTest, SnapshotOmitsMarkedOverlayElementsAndSubtrees) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.GetRoot();
		Child(context, root);

		UIDebugOps::Snapshot before;
		UIDebugOps::BuildSnapshot(context, before);

		UI::ElementHandle overlay = Child(context, root);
		context.AddClass(overlay, StringHash(UIDebugOps::MarkerClass));
		Child(context, overlay);

		UIDebugOps::Snapshot after;
		UIDebugOps::BuildSnapshot(context, after);
		EXPECT_EQ(after.rows.size(), before.rows.size());
		EXPECT_EQ(RowOf(after, overlay), static_cast<size_t>(-1));
		EXPECT_EQ(context.GetLiveCount(), 3u);
	}

	TEST_F(UIDebuggerTest, PathSurvivesRebuild) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.GetRoot();
		UI::ElementHandle parent = Child(context, root);
		Child(context, parent);
		UI::ElementHandle target = Child(context, parent);
		context.SetId(target, StringHash("second-child"));

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, target, path);
		ASSERT_EQ(path.size(), 2u);
		EXPECT_EQ(path[0], 0u);
		EXPECT_EQ(path[1], 1u);
		EXPECT_EQ(UIDebugOps::Resolve(context, path), target);

		context.ClearChildren(parent);
		EXPECT_FALSE(context.IsAlive(target));
		Child(context, parent);
		UI::ElementHandle rebuilt = Child(context, parent);
		context.SetId(rebuilt, StringHash("second-child"));

		UI::ElementHandle resolved = UIDebugOps::Resolve(context, path);
		EXPECT_EQ(resolved, rebuilt);
		EXPECT_NE(resolved, target);
	}

	TEST_F(UIDebuggerTest, StalePathResolvesNothing) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle parent = Child(context, context.GetRoot());
		UI::ElementHandle target = Child(context, parent);

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, target, path);
		ASSERT_FALSE(path.empty());
		context.DestroyElement(target);
		EXPECT_FALSE(UIDebugOps::Resolve(context, path).IsValid());
	}

	TEST_F(UIDebuggerTest, StrangerNotSelected) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle parent = Child(context, context.GetRoot());
		UI::ElementHandle target = Child(context, parent);
		context.SetId(target, StringHash("runtime-view"));
		context.AddClass(target, StringHash("ed-viewport-placeholder"));

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, target, path);
		UIDebugOps::ElementIdentity identity = UIDebugOps::IdentityOf(context, target);
		ASSERT_TRUE(identity.valid);
		EXPECT_EQ(UIDebugOps::ResolveSame(context, path, identity), target);

		context.ClearChildren(parent);
		UI::ElementHandle rebuilt = Child(context, parent);
		context.SetId(rebuilt, StringHash("runtime-view"));
		context.AddClass(rebuilt, StringHash("ed-viewport-placeholder"));
		EXPECT_NE(rebuilt, target);
		EXPECT_EQ(UIDebugOps::ResolveSame(context, path, identity), rebuilt);

		context.ClearChildren(parent);
		UI::ElementHandle stranger = Child(context, parent);
		context.AddClass(stranger, StringHash("slt-scroll-root"));
		EXPECT_TRUE(UIDebugOps::Resolve(context, path).IsValid());
		EXPECT_FALSE(UIDebugOps::ResolveSame(context, path, identity).IsValid());
	}

	TEST_F(UIDebuggerTest, IdentityMatchesClassSet) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle parent = Child(context, context.GetRoot());
		UI::ElementHandle row = Child(context, parent);
		context.AddClass(row, StringHash("slt-row"));
		context.AddClass(row, StringHash("slt-selected"));
		context.AddClass(row, StringHash("slt-cursor"));
		context.RemoveClass(row, StringHash("slt-row"));

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, row, path);
		UIDebugOps::ElementIdentity identity = UIDebugOps::IdentityOf(context, row);
		const UI::Element* live = context.GetElement(row);
		ASSERT_NE(live, nullptr);
		ASSERT_EQ(live->classes[0], StringHash("slt-cursor"));

		context.ClearChildren(parent);
		UI::ElementHandle rebuilt = Child(context, parent);
		context.AddClass(rebuilt, StringHash("slt-selected"));
		context.AddClass(rebuilt, StringHash("slt-cursor"));
		ASSERT_EQ(context.GetElement(rebuilt)->classes[0], StringHash("slt-selected"));
		EXPECT_TRUE(UIDebugOps::ResolveSame(context, path, identity).IsValid());

		context.ClearChildren(parent);
		UI::ElementHandle other = Child(context, parent);
		context.AddClass(other, StringHash("slt-row"));
		context.AddClass(other, StringHash("slt-header"));
		EXPECT_FALSE(UIDebugOps::ResolveSame(context, path, identity).IsValid());
	}

	TEST_F(UIDebuggerTest, ADetachedElementHasNoPath) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		Child(context, context.GetRoot());
		UI::ElementHandle orphan = context.CreateElement();
		ASSERT_TRUE(context.IsAlive(orphan));

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, orphan, path);
		EXPECT_TRUE(path.empty());
		EXPECT_EQ(UIDebugOps::Resolve(context, path), context.GetRoot());
	}

	TEST_F(UIDebuggerTest, DictionaryNamesEveryClassCoreSkinsSpell) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		context.LoadStyleSheetOnce(StringHash("slt-widgets"), UI::Widgets::GetDefaultSkinSource());
		context.LoadStyleSheetOnce(StringHash("slt-dock"), UI::DockManager::GetDefaultSkinSource());

		UIDebugOps::NameDictionary names;
		names.Rebuild(context);

		u32 checked = 0;
		Vector<UI::StyleRuleSource> rules{GetMemoryResource(MemTag::Editor)};
		for (std::string_view source : {UI::Widgets::GetDefaultSkinSource(), UI::DockManager::GetDefaultSkinSource()}) {
			UI::StyleSheet::SplitSource(source, rules);
			for (const UI::StyleRuleSource& rule : rules) {
				std::string_view selector = rule.selector;
				if (size_t colon = selector.find(':'); colon != std::string_view::npos) {
					selector = selector.substr(0, colon);
				}
				if (selector.empty() || selector == "*") {
					continue;
				}
				std::string_view name = (selector[0] == '.' || selector[0] == '#') ? selector.substr(1) : selector;
				if (name.find_first_of(".#") != std::string_view::npos) {
					continue;
				}
				EXPECT_EQ(names.Lookup(StringHash(name)), name) << "unnamed: " << selector;
				++checked;
			}
		}
		EXPECT_GT(checked, 20u);
		EXPECT_TRUE(names.Lookup(StringHash("no-skin-ever-spelled-this")).empty());
	}

	TEST_F(UIDebuggerTest, HashOrNameRendering) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ".known-class { opacity: 1; }");
		names.Rebuild(context);
		EXPECT_EQ(names.Render(StringHash("known-class")), "known-class");

		str rendered = names.Render(StringHash("set-from-cpp-and-named-nowhere"));
		EXPECT_TRUE(rendered.starts_with("#0x")) << rendered;
		EXPECT_NE(rendered.find("\xE2\x80\xA6"), str::npos) << rendered;
		EXPECT_TRUE(names.Render(StringHash{}).empty());
	}

	TEST_F(UIDebuggerTest, UnrecoverableSheetListed) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		context.LoadStyleSheetOnce(StringHash("slt-widgets"), UI::Widgets::GetDefaultSkinSource());
		context.LoadStyleSheetOnce(StringHash("project://UI/Theme.slss"), ".author-class { opacity: 1; }");

		UIDebugOps::NameDictionary names;
		names.Rebuild(context);
		ASSERT_EQ(names.GetSheets().size(), 2u);
		EXPECT_EQ(names.GetSheets()[0].name, "slt-widgets");
		EXPECT_FALSE(names.GetSheets()[0].source.empty());
		EXPECT_TRUE(names.GetSheets()[1].name.empty());
		EXPECT_TRUE(names.GetSheets()[1].source.empty());
		EXPECT_TRUE(names.Lookup(StringHash("author-class")).empty());

		names.AddCandidateSheet("project://UI/Theme.slss", ".author-class { opacity: 1; }");
		names.Rebuild(context);
		ASSERT_EQ(names.GetSheets().size(), 2u);
		EXPECT_EQ(names.GetSheets()[1].name, "project://UI/Theme.slss");
		EXPECT_EQ(names.Lookup(StringHash("author-class")), "author-class");
	}

	TEST_F(UIDebuggerTest, RowReadsIdTagOrType) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", "#header { opacity: 1; } button { opacity: 1; } .tall { opacity: 1; }");
		names.Rebuild(context);

		UI::ElementHandle bare = Child(context, context.GetRoot());
		EXPECT_EQ(UIDebugOps::RowLabel(context, bare, names), "Element");

		context.SetTypeTag(bare, StringHash("button"));
		EXPECT_EQ(UIDebugOps::RowLabel(context, bare, names), "button");

		context.AddClass(bare, StringHash("tall"));
		EXPECT_EQ(UIDebugOps::RowLabel(context, bare, names), "button .tall");

		context.SetId(bare, StringHash("header"));
		EXPECT_EQ(UIDebugOps::RowLabel(context, bare, names), "#header .tall");
	}

	TEST_F(UIDebuggerTest, MatchesComeBackAscendingWithWinnerLast) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe",
					   "#id-rule { opacity: 0.1; }"
					   "button { opacity: 0.2; }"
					   ".klass { opacity: 0.3; }"
					   ".unrelated { opacity: 0.4; }");
		UI::ElementHandle element = Child(context, context.GetRoot());
		context.SetTypeTag(element, StringHash("button"));
		context.AddClass(element, StringHash("klass"));
		context.SetId(element, StringHash("id-rule"));
		names.Rebuild(context);
		Vector<UIDebugOps::Match> matches{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildMatches(context, element, names, matches);

		ASSERT_EQ(matches.size(), 3u);
		EXPECT_EQ(matches[0].selector, "button");
		EXPECT_EQ(matches[1].selector, ".klass");
		EXPECT_EQ(matches[2].selector, "#id-rule");
		EXPECT_LT(matches[0].specificity, matches[1].specificity);
		EXPECT_LT(matches[1].specificity, matches[2].specificity);
		EXPECT_EQ(matches[2].sheet, "probe");
		char computed[UI::MAX_STYLE_VALUE_TEXT];
		EXPECT_EQ(UI::ReadStyleProperty(context.GetComputedStyle(element), StringHash("opacity"), computed, sizeof(computed)), "0.1");
	}

	TEST_F(UIDebuggerTest, PseudoRuleListedAndMarked) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ".btn { opacity: 1; } .btn:hover { opacity: 0.5; }");
		UI::ElementHandle element = Child(context, context.GetRoot());
		context.AddClass(element, StringHash("btn"));
		context.SetStyle(element, FixedBox(100.0f, 40.0f));
		context.Update();
		names.Rebuild(context);
		Vector<UIDebugOps::Match> matches{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildMatches(context, element, names, matches);
		ASSERT_EQ(matches.size(), 2u);
		EXPECT_EQ(matches[1].pseudo, UI::PSEUDO_HOVER);
		EXPECT_TRUE(matches[0].applies);
		EXPECT_FALSE(matches[1].applies);

		InputEvent move{};
		move.type = InputEventType::MouseMove;
		move.mousePosPx = {20, 20};
		context.InjectInput(move);
		context.Update();
		ASSERT_EQ(context.GetHovered(), element);

		UIDebugOps::BuildMatches(context, element, names, matches);
		ASSERT_EQ(matches.size(), 2u);
		EXPECT_TRUE(matches[1].applies);
	}

	TEST_F(UIDebuggerTest, SourceColumnShowsOrigin) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ".card { opacity: 0.4; } #won { opacity: 0.9; }");
		UI::ElementHandle element = Child(context, context.GetRoot());
		context.AddClass(element, StringHash("card"));
		context.SetId(element, StringHash("won"));
		UI::Style inlineStyle;
		inlineStyle.opacity = 0.2f;
		inlineStyle.zIndex = 7;
		context.SetStyle(element, inlineStyle);
		names.Rebuild(context);
		Vector<UIDebugOps::PropertyRow> rows{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildProperties(context, element, names, rows);
		ASSERT_EQ(rows.size(), UI::GetStylePropertyNames().size());

		auto find = [&rows](std::string_view name) -> const UIDebugOps::PropertyRow* {
			for (const UIDebugOps::PropertyRow& row : rows) {
				if (row.name == name) {
					return &row;
				}
			}
			return nullptr;
		};

		const UIDebugOps::PropertyRow* opacity = find("opacity");
		ASSERT_NE(opacity, nullptr);
		EXPECT_EQ(opacity->source, UIDebugOps::ValueSource::Sheet);
		EXPECT_EQ(opacity->value, "0.9");
		EXPECT_EQ(opacity->sheet, "probe");

		const UIDebugOps::PropertyRow* zIndex = find("z-index");
		ASSERT_NE(zIndex, nullptr);
		EXPECT_EQ(zIndex->source, UIDebugOps::ValueSource::Inline);
		EXPECT_EQ(zIndex->value, "7");
		EXPECT_TRUE(zIndex->sheet.empty());

		const UIDebugOps::PropertyRow* spacing = find("letter-spacing");
		ASSERT_NE(spacing, nullptr);
		EXPECT_EQ(spacing->source, UIDebugOps::ValueSource::Default);
		EXPECT_TRUE(spacing->sheet.empty());
	}

	TEST_F(UIDebuggerTest, SourceColumnNamesTextOrigins) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ".loud { color: #ff0000; font-size: 20px; } .leaf { font-weight: bold; }");
		UI::ElementHandle parent = Child(context, context.GetRoot());
		context.AddClass(parent, StringHash("loud"));
		UI::ElementHandle label = Child(context, parent);
		context.AddClass(label, StringHash("leaf"));
		context.SetText(label, {}, 12.0f, Color(0.0f, 0.0f, 1.0f, 1.0f), "x");
		names.Rebuild(context);
		Vector<UIDebugOps::PropertyRow> rows{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildProperties(context, label, names, rows);

		auto source = [&rows](std::string_view name) {
			for (const UIDebugOps::PropertyRow& row : rows) {
				if (row.name == name) {
					return row.source;
				}
			}
			return UIDebugOps::ValueSource::Default;
		};
		EXPECT_EQ(source("color"), UIDebugOps::ValueSource::Text);
		EXPECT_EQ(source("font-size"), UIDebugOps::ValueSource::Text);
		EXPECT_EQ(source("font-weight"), UIDebugOps::ValueSource::Sheet);

		UI::ElementHandle child = Child(context, parent);
		UIDebugOps::BuildProperties(context, child, names, rows);
		EXPECT_EQ(source("color"), UIDebugOps::ValueSource::Inherited);
		EXPECT_EQ(source("font-size"), UIDebugOps::ValueSource::Inherited);
		EXPECT_EQ(source("font-weight"), UIDebugOps::ValueSource::Default);
	}

	TEST_F(UIDebuggerTest, LayoutBandsAreGaps) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		UI::Context context(desc, &fonts);

		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style style;
		style.width = UI::Length::Pixel(200.0f);
		style.height = UI::Length::Pixel(100.0f);
		style.margin = {3.0f, 5.0f, 7.0f, 11.0f};
		style.borderWidth = {2.0f, 4.0f, 6.0f, 8.0f};
		style.padding = {1.0f, 9.0f, 13.0f, 17.0f};
		context.SetStyle(element, style);
		context.Update();

		UIDebugOps::Geometry geometry;
		UIDebugOps::BuildGeometry(context, element, geometry);

		const UI::LayoutData& box = context.GetLayout(element);
		EXPECT_EQ(geometry.worldBound, box.borderBox);
		EXPECT_FLOAT_EQ(geometry.margin.left, box.borderBox.x - box.marginBox.x);
		EXPECT_FLOAT_EQ(geometry.margin.bottom,
						(box.marginBox.y + box.marginBox.h) - (box.borderBox.y + box.borderBox.h));
		EXPECT_FLOAT_EQ(geometry.border.top, box.paddingBox.y - box.borderBox.y);
		EXPECT_FLOAT_EQ(geometry.border.right,
						(box.borderBox.x + box.borderBox.w) - (box.paddingBox.x + box.paddingBox.w));
		EXPECT_FLOAT_EQ(geometry.padding.left, box.content.x - box.paddingBox.x);
		EXPECT_FLOAT_EQ(geometry.padding.bottom,
						(box.paddingBox.y + box.paddingBox.h) - (box.content.y + box.content.h));

		EXPECT_NE(geometry.margin.left, geometry.margin.top);
		EXPECT_NE(geometry.border.left, geometry.padding.left);
	}

	TEST_F(UIDebuggerTest, RectsAreLocal) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		UI::Context context(desc, &fonts);

		UI::ElementHandle parent = Child(context, context.GetRoot());
		UI::Style parentStyle;
		parentStyle.width = UI::Length::Pixel(400.0f);
		parentStyle.height = UI::Length::Pixel(300.0f);
		parentStyle.margin = UI::Edges::All(20.0f);
		parentStyle.padding = UI::Edges::All(30.0f);
		context.SetStyle(parent, parentStyle);

		UI::ElementHandle element = Child(context, parent);
		UI::Style style;
		style.width = UI::Length::Pixel(100.0f);
		style.height = UI::Length::Pixel(50.0f);
		style.borderWidth = UI::Edges::All(2.0f);
		style.padding = UI::Edges::All(5.0f);
		context.SetStyle(element, style);
		context.Update();

		UIDebugOps::Geometry geometry;
		UIDebugOps::BuildGeometry(context, element, geometry);
		const UI::LayoutData& box = context.GetLayout(element);
		const UI::Rect& parentContent = context.GetLayout(parent).content;

		EXPECT_FLOAT_EQ(geometry.contentRect.x, box.content.x - box.borderBox.x);
		EXPECT_FLOAT_EQ(geometry.contentRect.y, box.content.y - box.borderBox.y);
		EXPECT_FLOAT_EQ(geometry.contentRect.w, box.content.w);
		EXPECT_GT(geometry.contentRect.x, 0.0f);

		EXPECT_FLOAT_EQ(geometry.layout.x, box.borderBox.x - parentContent.x);
		EXPECT_FLOAT_EQ(geometry.layout.y, box.borderBox.y - parentContent.y);
		EXPECT_NE(geometry.layout.x, geometry.worldBound.x);

		UIDebugOps::Geometry rootGeometry;
		UIDebugOps::BuildGeometry(context, context.GetRoot(), rootGeometry);
		EXPECT_EQ(rootGeometry.layout, rootGeometry.worldBound);
	}

	TEST_F(UIDebuggerTest, WorldClipIntersectsAllAncestors) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		UI::Context context(desc, &fonts);

		UI::ElementHandle outer = Child(context, context.GetRoot());
		UI::Style outerStyle;
		outerStyle.width = UI::Length::Pixel(100.0f);
		outerStyle.height = UI::Length::Pixel(400.0f);
		outerStyle.overflow = UI::Overflow::Hidden;
		context.SetStyle(outer, outerStyle);

		UI::ElementHandle inner = Child(context, outer);
		UI::Style innerStyle;
		innerStyle.width = UI::Length::Pixel(300.0f);
		innerStyle.height = UI::Length::Pixel(400.0f);
		innerStyle.flexShrink = 0.0f;
		innerStyle.overflow = UI::Overflow::Hidden;
		context.SetStyle(inner, innerStyle);

		UI::ElementHandle element = Child(context, inner);
		UI::Style style;
		style.width = UI::Length::Pixel(300.0f);
		style.height = UI::Length::Pixel(20.0f);
		style.flexShrink = 0.0f;
		context.SetStyle(element, style);
		context.Update();

		UIDebugOps::Geometry geometry;
		UIDebugOps::BuildGeometry(context, element, geometry);
		const UI::Rect& elementBox = context.GetLayout(element).borderBox;
		const UI::Rect& outerBox = context.GetLayout(outer).borderBox;
		const UI::Rect& innerBox = context.GetLayout(inner).borderBox;

		ASSERT_LT(outerBox.w, innerBox.w);
		EXPECT_TRUE(geometry.clipped);
		EXPECT_FLOAT_EQ(geometry.worldClip.w, outerBox.w);
		EXPECT_LT(geometry.worldClip.w, elementBox.w);

		UIDebugOps::Geometry loose;
		UIDebugOps::BuildGeometry(context, outer, loose);
		EXPECT_FALSE(loose.clipped);
		EXPECT_EQ(loose.worldClip, loose.worldBound);
	}

	TEST_F(UIDebuggerTest, PickHonoursBorderRadius) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle rounded = Child(context, context.GetRoot());
		UI::Style style;
		style.position = UI::Position::Absolute;
		style.left = UI::Length::Pixel(0.0f);
		style.top = UI::Length::Pixel(0.0f);
		style.width = UI::Length::Pixel(100.0f);
		style.height = UI::Length::Pixel(100.0f);
		style.borderRadius = {40.0f, 40.0f, 40.0f, 40.0f};
		context.SetStyle(rounded, style);
		context.Update();

		const Vec2 corner{4.0f, 4.0f};
		EXPECT_EQ(context.HitTest(corner, UI::HitTestMode::Box), rounded);
		EXPECT_EQ(UIDebugOps::PickAt(context, corner), context.GetRoot());
		EXPECT_EQ(UIDebugOps::PickAt(context, {50.0f, 50.0f}), rounded);
	}

	TEST_F(UIDebuggerTest, PickSkipsDebuggerSubtrees) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle panel = Child(context, context.GetRoot());
		UI::Style fill;
		fill.width = UI::Length::Percent(1.0f);
		fill.height = UI::Length::Percent(1.0f);
		context.SetStyle(panel, fill);

		UI::ElementHandle list = Child(context, panel);
		context.AddClass(list, StringHash(UIDebugOps::MarkerClass));
		context.SetStyle(list, fill);
		UI::ElementHandle inner = Child(context, list);
		context.AddClass(inner, StringHash(UIDebugOps::MarkerClass));
		context.SetStyle(inner, fill);
		UI::ElementHandle row = Child(context, inner);
		context.SetStyle(row, fill);
		context.Update();

		const Vec2 point{200.0f, 150.0f};
		ASSERT_EQ(context.HitTest(point, UI::HitTestMode::Precise), row);
		EXPECT_EQ(UIDebugOps::PickAt(context, point), panel);

		context.RemoveClass(list, StringHash(UIDebugOps::MarkerClass));
		context.RemoveClass(inner, StringHash(UIDebugOps::MarkerClass));
		context.Update();
		EXPECT_EQ(UIDebugOps::PickAt(context, point), row);
	}

	TEST_F(UIDebuggerTest, APickOverNothingAnswersNothing) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		Child(context, context.GetRoot());
		context.Update();

		EXPECT_FALSE(UIDebugOps::PickAt(context, {900.0f, 900.0f}).IsValid());
	}

	TEST_F(UIDebuggerTest, HighlightBandsMatchBoxes) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style style;
		style.width = UI::Length::Pixel(120.0f);
		style.height = UI::Length::Pixel(60.0f);
		style.margin = {7.0f, 3.0f, 11.0f, 5.0f};
		style.borderWidth = {2.0f, 4.0f, 2.0f, 4.0f};
		style.padding = {9.0f, 6.0f, 9.0f, 6.0f};
		context.SetStyle(element, style);
		context.Update();

		UIDebugOps::Geometry geometry;
		UIDebugOps::BuildGeometry(context, element, geometry);
		UIDebugOps::Highlight highlight;
		UIDebugOps::BuildHighlight(context, element, highlight);
		const UI::LayoutData& box = context.GetLayout(element);

		using Ring = UIDebugOps::HighlightRing;
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Margin)].box, box.marginBox);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Margin)].ring, geometry.margin);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Border)].box, box.borderBox);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Border)].ring, geometry.border);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Padding)].box, box.paddingBox);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Padding)].ring, geometry.padding);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Content)].box, box.content);
		EXPECT_EQ(highlight.bands[static_cast<size_t>(Ring::Content)].ring, UI::Edges{});
		ASSERT_NE(box.content.x, geometry.contentRect.x);

		UIDebugOps::BuildHighlight(context, UI::ElementHandle{}, highlight);
		for (const UIDebugOps::HighlightBand& band : highlight.bands) {
			EXPECT_EQ(band.box.w, 0.0f);
			EXPECT_EQ(band.box.h, 0.0f);
		}
	}

	TEST_F(UIDebuggerTest, HighlightBandsInLogicalPx) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		desc.contentScale = 1.25f;
		UI::Context context(desc, &fonts);

		UI::ElementHandle spacer = Child(context, context.GetRoot());
		UI::Style spacerStyle;
		spacerStyle.width = UI::Length::Pixel(40.0f);
		spacerStyle.height = UI::Length::Pixel(40.0f);
		spacerStyle.flexShrink = 0.0f;
		context.SetStyle(spacer, spacerStyle);

		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style style;
		style.width = UI::Length::Pixel(120.0f);
		style.height = UI::Length::Pixel(60.0f);
		style.borderWidth = {4.0f, 4.0f, 4.0f, 4.0f};
		style.flexShrink = 0.0f;
		context.SetStyle(element, style);
		context.Update();

		const UI::Rect& physical = context.GetLayout(element).borderBox;
		ASSERT_FLOAT_EQ(physical.w, 150.0f);
		ASSERT_GT(physical.x, 0.0f);

		UIDebugOps::Highlight highlight;
		UIDebugOps::BuildHighlight(context, element, highlight);
		const UIDebugOps::HighlightBand& border = highlight.bands[static_cast<size_t>(UIDebugOps::HighlightRing::Border)];
		EXPECT_FLOAT_EQ(border.box.w, 120.0f);
		EXPECT_FLOAT_EQ(border.box.h, 60.0f);
		EXPECT_FLOAT_EQ(border.box.x, physical.x / 1.25f);
		EXPECT_FLOAT_EQ(border.ring.left, 4.0f);
	}

	TEST_F(UIDebuggerTest, AttributeRowsReadBackWhatElementCarries) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		UI::Context context(desc, &fonts);
		DressLikeTheEditor(context);
		UIDebugOps::NameDictionary names;
		names.Rebuild(context);

		UI::ElementHandle element = Child(context, context.GetRoot());
		context.AddClass(element, StringHash("slt-button"));
		context.SetVisible(element, false);
		context.SetHoverable(element, false);
		context.SetTint(element, Color(1.0f, 0.0f, 0.0f, 1.0f));
		context.SetUserTag(element, 0xABCDu);
		UI::Style style;
		style.zIndex = 7;
		style.cursor = SystemCursor::Hand;
		context.SetStyle(element, style);
		context.Update();

		Vector<UIDebugOps::AttributeRow> rows{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildAttributes(context, element, names, rows);
		ASSERT_FALSE(rows.empty());

		auto valueOf = [&rows](std::string_view name) -> str {
			for (const UIDebugOps::AttributeRow& row : rows) {
				if (row.name == name) {
					return row.value;
				}
			}
			return "<<missing>>";
		};

		EXPECT_EQ(valueOf("classes"), ".slt-button");
		EXPECT_EQ(valueOf("visible"), "no");
		EXPECT_EQ(valueOf("hoverable"), "no");
		EXPECT_EQ(valueOf("focusable"), "no");
		EXPECT_EQ(valueOf("tint"), "#ff0000ff");
		EXPECT_EQ(valueOf("z index"), "7");
		EXPECT_EQ(valueOf("cursor"), "hand");
		EXPECT_EQ(valueOf("user tag"), "0xabcd");
		EXPECT_EQ(valueOf("id"), "");
		EXPECT_EQ(valueOf("state"), "\xE2\x80\x94");
		EXPECT_EQ(valueOf("content"), "\xE2\x80\x94");
		EXPECT_NE(valueOf("scroll offset"), "<<missing>>");

		UI::ElementHandle live = Child(context, context.GetRoot());
		context.SetFocusable(live, true);
		context.SetText(live, {}, 12.0f, Color::White(), "x");
		context.SetFocus(live);
		context.Update();
		UIDebugOps::BuildAttributes(context, live, names, rows);
		EXPECT_EQ(valueOf("state"), "focused");
		EXPECT_EQ(valueOf("content"), "text");
	}

	TEST_F(UIDebuggerTest, SheetsListedWithRuleCounts) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "uidebug-sheets-probe",
					   ".a { color: #ff0000ff; } .b { color: #00ff00ff; } .c:hover { color: #0000ffff; }");
		context.LoadStyleSheetOnce(StringHash("uidebug-sheets-unnamed"), ".d { color: #ffffffff; }");
		names.Rebuild(context);

		Vector<UIDebugOps::SheetRow> sheets{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildSheets(names, sheets);
		ASSERT_EQ(sheets.size(), 2u);

		EXPECT_EQ(sheets[0].name, "uidebug-sheets-probe");
		EXPECT_TRUE(sheets[0].readable);
		EXPECT_EQ(sheets[0].ruleCount, 3u);
		EXPECT_EQ(sheets[0].tag, StringHash("uidebug-sheets-probe"));

		EXPECT_TRUE(sheets[1].name.empty());
		EXPECT_FALSE(sheets[1].readable);
		EXPECT_EQ(sheets[1].ruleCount, 0u);
		EXPECT_EQ(sheets[1].tag, StringHash("uidebug-sheets-unnamed"));
	}

	TEST_F(UIDebuggerTest, EditorNameResidueReported) {
		EditorSelection::Initialize();
		PanelManager::Initialize();
		RegisterBuiltInPanels();

		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {1600, 900};
		UI::Context context(desc, &fonts);
		DressLikeTheEditor(context);

		const char* panels[] = {"AssetBrowserPanel", "BuildProfilesPanel", "BuildReportPanel", "ConsolePanel",
								"HierarchyPanel", "InspectorPanel", "PackageManagerPanel", "ProfilerPanel",
								"RuntimeViewPanel", "SceneViewPanel", "SettingsPanel", "UIBuilderPanel"};
		u32 built = 0;
		for (const char* name : panels) {
			const TypeInfo* type = TypeRegistry::FindType(StringHash(name));
			if (!type) {
				continue;
			}
			Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
			if (!panel.IsValid()) {
				continue;
			}
			UI::ElementHandle host = Child(context, context.GetRoot());
			panel.Get()->BuildContent(context, host);
			++built;
		}
		context.Update();
		ASSERT_GE(built, 8u);

		UIDebugOps::NameDictionary names;
		names.Rebuild(context);
		UIDebugOps::NameDictionary::Residue residue = names.MeasureResidue(context);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		std::printf("[ RESIDUE  ] %u panels, %zu elements, %u dictionary names, %u sheets\n", built,
					snapshot.rows.size(), names.GetCount(), static_cast<u32>(names.GetSheets().size()));
		std::printf("[ RESIDUE  ] identities seen %u, unresolved %u => %.2f%%\n", residue.total, residue.unresolved,
					static_cast<f64>(residue.Fraction()) * 100.0);
		std::printf("[ RESIDUE  ] ids %u/%u unresolved, tags %u/%u, classes %u/%u\n", residue.idsUnresolved,
					residue.ids, residue.tagsUnresolved, residue.tags, residue.classesUnresolved, residue.classes);

		ASSERT_GT(residue.total, 150u);
		EXPECT_LT(residue.Fraction(), 0.35f) << residue.unresolved << " of " << residue.total;

		UI::ElementHandle probe = Child(context, context.GetRoot());
		context.AddClass(probe, StringHash("no-sheet-ever-spelled-this-class"));
		UIDebugOps::NameDictionary::Residue withProbe = names.MeasureResidue(context);
		EXPECT_EQ(withProbe.total, residue.total + 1);
		EXPECT_EQ(withProbe.unresolved, residue.unresolved + 1);

		PanelManager::Terminate();
		EditorSelection::Terminate();
	}

	TEST_F(UIDebuggerTest, RowKeyIsPathKey) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle a = Child(context, context.GetRoot());
		UI::ElementHandle b = Child(context, context.GetRoot());
		Child(context, a);
		Child(context, a);
		Child(context, b);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		ASSERT_EQ(snapshot.rows.size(), 6u);

		HashSet<u64> seen{GetMemoryResource(MemTag::Editor)};
		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		for (const UIDebugOps::Row& row : snapshot.rows) {
			UIDebugOps::PathTo(context, row.element, path);
			EXPECT_EQ(row.key, UIDebugOps::PathKey(path));
			EXPECT_TRUE(seen.insert(row.key).second);
		}
	}

	TEST_F(UIDebuggerTest, MarkerSubtreeDoesNotMoveSiblingsAddresses) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		Child(context, context.GetRoot());
		UI::ElementHandle overlay = Child(context, context.GetRoot());
		UI::ElementHandle after = Child(context, context.GetRoot());

		UIDebugOps::Snapshot before;
		UIDebugOps::BuildSnapshot(context, before);
		ASSERT_EQ(before.rows.size(), 4u);
		const u64 addressOfAfter = before.rows.back().key;

		context.AddClass(overlay, StringHash(UIDebugOps::MarkerClass));
		UIDebugOps::Snapshot withOverlay;
		UIDebugOps::BuildSnapshot(context, withOverlay);
		ASSERT_EQ(withOverlay.rows.size(), 3u);
		EXPECT_EQ(withOverlay.rows.back().element, after);
		EXPECT_EQ(withOverlay.rows.back().key, addressOfAfter);
	}

	TEST_F(UIDebuggerTest, KeySurvivesRebuildThatRecyclesHandles) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle host = Child(context, context.GetRoot());
		UI::ElementHandle first = Child(context, host);
		Child(context, first);

		UIDebugOps::Snapshot before;
		UIDebugOps::BuildSnapshot(context, before);
		ASSERT_EQ(before.rows.size(), 4u);
		const u64 addressOfLeaf = before.rows.back().key;
		const UI::ElementHandle leafBefore = before.rows.back().element;

		context.ClearChildren(host);
		UI::ElementHandle rebuilt = Child(context, host);
		UI::ElementHandle leaf = Child(context, rebuilt);
		ASSERT_NE(leaf, leafBefore);

		UIDebugOps::Snapshot after;
		UIDebugOps::BuildSnapshot(context, after);
		ASSERT_EQ(after.rows.size(), 4u);
		EXPECT_EQ(after.rows.back().key, addressOfLeaf);
	}

	struct ProbeTree {
		UI::ElementHandle alpha, alphaLeaf, beta, betaLeaf;
	};

	ProbeTree BuildProbeTree(UI::Context& context) {
		ProbeTree tree;
		tree.alpha = context.CreateElement();
		context.AppendChild(context.GetRoot(), tree.alpha);
		context.AddClass(tree.alpha, StringHash("alpha"));
		tree.alphaLeaf = context.CreateElement();
		context.AppendChild(tree.alpha, tree.alphaLeaf);
		context.AddClass(tree.alphaLeaf, StringHash("needle"));
		tree.beta = context.CreateElement();
		context.AppendChild(context.GetRoot(), tree.beta);
		context.AddClass(tree.beta, StringHash("beta"));
		tree.betaLeaf = context.CreateElement();
		context.AppendChild(tree.beta, tree.betaLeaf);
		context.AddClass(tree.betaLeaf, StringHash("haystack"));
		return tree;
	}

	const UIDebugOps::TreeRow* RowFor(const Vector<UIDebugOps::TreeRow>& rows, u64 key) {
		for (const UIDebugOps::TreeRow& row : rows) {
			if (row.key == key) {
				return &row;
			}
		}
		return nullptr;
	}

	constexpr std::string_view ProbeSheet = ".alpha{} .beta{} .needle{} .haystack{}";

	TEST_F(UIDebuggerTest, FoldingRowHidesSubtreeAndNothingElse) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		ProbeTree tree = BuildProbeTree(context);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ProbeSheet);
		names.Rebuild(context);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		Vector<UIDebugOps::TreeRow> view{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::TreeViewDesc viewDesc;
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		ASSERT_EQ(view.size(), 5u);
		const UIDebugOps::TreeRow* alphaRow = RowFor(view, snapshot.rows[1].key);
		ASSERT_NE(alphaRow, nullptr);
		EXPECT_TRUE(alphaRow->hasChildren);
		EXPECT_TRUE(alphaRow->expanded);

		u64 folded[1] = {snapshot.rows[1].key};
		viewDesc.collapsed = folded;
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		ASSERT_EQ(view.size(), 4u);
		ASSERT_NE(RowFor(view, snapshot.rows[1].key), nullptr);
		EXPECT_FALSE(RowFor(view, snapshot.rows[1].key)->expanded);
		for (const UIDebugOps::TreeRow& row : view) {
			EXPECT_NE(row.element, tree.alphaLeaf);
		}
		EXPECT_NE(RowFor(view, snapshot.rows[3].key), nullptr);
		EXPECT_NE(RowFor(view, snapshot.rows[4].key), nullptr);
	}

	TEST_F(UIDebuggerTest, UnfoldingPathsAncestorsGivesElementRowAgain) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		ProbeTree tree = BuildProbeTree(context);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ProbeSheet);
		names.Rebuild(context);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, tree.alphaLeaf, path);
		const u64 pickedKey = UIDebugOps::PathKey(path);

		Vector<u64> collapsed{GetMemoryResource(MemTag::Editor)};
		collapsed.push_back(snapshot.rows[1].key);

		Vector<UIDebugOps::TreeRow> view{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::TreeViewDesc viewDesc;
		viewDesc.collapsed = collapsed;
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		ASSERT_EQ(RowFor(view, pickedKey), nullptr);

		EXPECT_TRUE(UIDebugOps::ExpandAncestors(path, collapsed));
		viewDesc.collapsed = collapsed;
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		EXPECT_NE(RowFor(view, pickedKey), nullptr);

		EXPECT_FALSE(UIDebugOps::ExpandAncestors(path, collapsed));
	}

	TEST_F(UIDebuggerTest, ExpandKeepsUnrelatedFolds) {
		const u32 pathIndices[] = {2u, 0u, 5u};
		const std::span<const u32> path(pathIndices);
		const u32 strangerIndices[] = {7u};

		const u64 rootKey = UIDebugOps::PathKey({});
		const u64 parentKey = UIDebugOps::PathKey(path.first(2));
		const u64 ownKey = UIDebugOps::PathKey(path);
		const u64 strangerKey = UIDebugOps::PathKey(std::span<const u32>(strangerIndices));

		Vector<u64> collapsed{GetMemoryResource(MemTag::Editor)};
		collapsed.push_back(rootKey);
		collapsed.push_back(parentKey);
		collapsed.push_back(ownKey);
		collapsed.push_back(strangerKey);

		EXPECT_TRUE(UIDebugOps::ExpandAncestors(path, collapsed));
		auto folded = [&collapsed](u64 key) {
			return std::find(collapsed.begin(), collapsed.end(), key) != collapsed.end();
		};
		EXPECT_FALSE(folded(rootKey));
		EXPECT_FALSE(folded(parentKey));
		EXPECT_TRUE(folded(ownKey));
		EXPECT_TRUE(folded(strangerKey));
	}

	TEST_F(UIDebuggerTest, SearchReachesFoldedBranches) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		ProbeTree tree = BuildProbeTree(context);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ProbeSheet);
		names.Rebuild(context);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		u64 folded[1] = {snapshot.rows[1].key};
		Vector<UIDebugOps::TreeRow> view{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::TreeViewDesc viewDesc;
		viewDesc.query = "needle";
		viewDesc.collapsed = folded;
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);

		ASSERT_EQ(view.size(), 3u);
		EXPECT_EQ(view.back().element, tree.alphaLeaf);
		EXPECT_TRUE(view.back().matched);
		EXPECT_EQ(view[1].element, tree.alpha);
		EXPECT_FALSE(view[1].matched);
		EXPECT_TRUE(view[1].expanded);
		for (const UIDebugOps::TreeRow& row : view) {
			EXPECT_NE(row.element, tree.beta);
			EXPECT_NE(row.element, tree.betaLeaf);
		}
	}

	TEST_F(UIDebuggerTest, SearchIgnoresCase) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		BuildProbeTree(context);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe", ProbeSheet);
		names.Rebuild(context);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		Vector<UIDebugOps::TreeRow> view{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::TreeViewDesc viewDesc;
		viewDesc.query = "NeEdLe";
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		EXPECT_EQ(view.size(), 3u);

		viewDesc.query = "nothing-is-called-this";
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		EXPECT_TRUE(view.empty());

		viewDesc.query = {};
		UIDebugOps::BuildTreeView(context, snapshot, names, viewDesc, view);
		EXPECT_EQ(view.size(), snapshot.rows.size());
	}

	UI::ElementHandle FindRowWithTag(UI::Context& context, UI::ElementHandle root, u64 tag,
									 StringHash requiredClass = StringHash("slt-tree-row")) {
		const UI::Element* pod = context.GetElement(root);
		if (!pod) {
			return {};
		}
		if (pod->userTag == tag && context.HasClass(root, requiredClass)) {
			return root;
		}
		for (u32 child = pod->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			UI::ElementHandle found = FindRowWithTag(context, handle, tag, requiredClass);
			if (found.IsValid()) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	class UIDebuggerPanelFixture : public ::testing::Test {
	protected:
		Handle<Window> _window;

		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			EditorSelection::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			PanelManager::Initialize();

			WindowDesc desc;
			desc.title = "UIDebuggerTestWindow";
			desc.clientSize = {1200, 800};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			UI::Context* context = UISystem::CreateContext(_window);
			ASSERT_NE(context, nullptr);
			EditorTheme::ApplyToContext(*context);
		}

		void TearDown() override {
			PanelManager::Terminate();
			UISystem::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			EditorSelection::Terminate();
		}

		UI::Context& WindowContext() { return *UISystem::GetContext(_window); }

		void Frame(EditorPanel& panel) {
			panel.OnTick(0.0f);
			UISystem::Update();
		}

		Handle<EditorPanel> OpenDebugger(UI::ElementHandle host) {
			const TypeInfo* type = TypeRegistry::FindType(StringHash("UIDebuggerPanel"));
			if (!type) {
				return {};
			}
			Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
			if (panel.IsValid()) {
				panel.Get()->BuildContent(WindowContext(), host);
			}
			return panel;
		}

		UI::ElementHandle Host() {
			UI::Context& context = WindowContext();
			UI::ElementHandle host = context.CreateElement();
			context.AppendChild(context.GetRoot(), host);
			return host;
		}

		static u32 DrawnRows(UI::Context& context, UI::ElementHandle host) {
			return CountByClass(context, host, StringHash("slt-tree-row"));
		}

		static UI::ElementHandle PickButton(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle strip = FindByClass(context, host, EditorTheme::Class::PanelHeader, 0);
			return context.IsAlive(strip) ? FindByClass(context, strip, EditorTheme::Class::IconButton, 0)
										  : UI::ElementHandle{};
		}

		void ArmPick(EditorPanel& panel, UI::ElementHandle host) {
			UI::Context& context = WindowContext();
			UI::ElementHandle button = PickButton(context, host);
			ASSERT_TRUE(button.IsValid());
			ClickElement(context, button);
			for (int i = 0; i < 2; ++i) {
				Frame(panel);
			}
		}

		static bool IsPickArmed(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle button = PickButton(context, host);
			return context.IsAlive(button) && context.HasClass(button, EditorTheme::Class::ToolButtonOn);
		}

		static UI::ElementHandle TextOverlayButton(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle strip = FindByClass(context, host, EditorTheme::Class::PanelHeader, 0);
			return context.IsAlive(strip) ? FindByClass(context, strip, EditorTheme::Class::IconButton, 1)
										  : UI::ElementHandle{};
		}

		void ToggleOverlayKind(EditorPanel& panel, UI::ElementHandle host, std::string_view kind) {
			UI::Context& context = WindowContext();
			UI::ElementHandle button = TextOverlayButton(context, host);
			ASSERT_TRUE(button.IsValid());
			ClickElement(context, button);
			UI::ElementHandle entry = FindByText(context, context.GetRoot(), kind);
			ASSERT_TRUE(entry.IsValid());
			ClickElement(context, entry);
			for (int i = 0; i < 2; ++i) {
				Frame(panel);
			}
		}

		static UI::ElementHandle AtlasButton(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle strip = FindByClass(context, host, EditorTheme::Class::PanelHeader, 0);
			return context.IsAlive(strip) ? FindByClass(context, strip, EditorTheme::Class::IconButton, 2)
										  : UI::ElementHandle{};
		}

		static str StatusText(UI::Context& context, UI::ElementHandle root) {
			str text = context.GetTextString(root);
			if (text.find(" elements") != str::npos) {
				return text;
			}
			const UI::Element* pod = context.GetElement(root);
			if (!pod) {
				return {};
			}
			for (u32 child = pod->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				str found = StatusText(context, handle);
				if (!found.empty()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static u32 OverlayCanvases(UI::Context& context) {
			const UI::Element* root = context.GetElement(context.GetRoot());
			u32 count = 0;
			for (u32 child = root->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (context.HasClass(handle, StringHash(UIDebugOps::MarkerClass))) {
					++count;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return count;
		}

		static bool PointerIsFree(UI::Context& context) {
			UI::ElementHandle probe = context.CreateElement();
			context.AppendChild(context.GetRoot(), probe);
			const u32 claim = context.ClaimPointer(probe, CursorMode::Normal);
			context.ReleasePointer(claim);
			context.DestroyElement(probe);
			return claim != 0;
		}

		static bool CarriesAControl(UI::Context& context, UI::ElementHandle row) {
			for (StringHash klass : {StringHash("slt-textfield"), StringHash("slt-checkbox"),
									 StringHash("slt-numberfield")}) {
				if (FindByClass(context, row, klass, 0).IsValid()) {
					return true;
				}
			}
			return false;
		}

		static UI::ElementHandle RowNamed(UI::Context& context, UI::ElementHandle root, std::string_view name,
										  bool editableOnly = false) {
			if (context.GetTextString(root) == name) {
				const UI::Element* label = context.GetElement(root);
				if (label && label->parent != UI::INVALID_UI_INDEX) {
					UI::ElementHandle row = context.MakeHandle(label->parent);
					if (!editableOnly || CarriesAControl(context, row)) {
						return row;
					}
				}
			}
			const UI::Element* pod = context.GetElement(root);
			if (!pod) {
				return {};
			}
			for (u32 child = pod->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = RowNamed(context, handle, name, editableOnly);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static UI::ElementHandle ValueFieldOnRow(UI::Context& context, UI::ElementHandle host, std::string_view name) {
			UI::ElementHandle row = RowNamed(context, host, name, true);
			return context.IsAlive(row) ? FindByClass(context, row, StringHash("slt-textfield"), 0) : UI::ElementHandle{};
		}

		static UI::ElementHandle RevertOnRow(UI::Context& context, UI::ElementHandle host, std::string_view name) {
			UI::ElementHandle row = RowNamed(context, host, name, true);
			return context.IsAlive(row) ? FindByClass(context, row, EditorTheme::Class::IconButton, 0)
										: UI::ElementHandle{};
		}

		static bool RowSaysInline(UI::Context& context, UI::ElementHandle host, std::string_view name) {
			UI::ElementHandle row = RowNamed(context, host, name, true);
			return context.IsAlive(row) && FindByText(context, row, "inline").IsValid();
		}

		static void TypeInto(UI::Context& context, UI::ElementHandle field, std::string_view text) {
			ClickElement(context, field);
			InputEvent selectAll{};
			selectAll.type = InputEventType::KeyDown;
			selectAll.key = Key::A;
			selectAll.mods = KeyMod::Ctrl;
			context.InjectInput(selectAll);
			for (char c : text) {
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
		}

		void SelectSubject(EditorPanel& panel, UI::ElementHandle host, UI::ElementHandle subject) {
			UI::Context& context = WindowContext();
			Vector<u32> path{GetMemoryResource(MemTag::Editor)};
			UIDebugOps::PathTo(context, subject, path);
			UI::ElementHandle row = FindRowWithTag(context, host, UIDebugOps::PathKey(path));
			ASSERT_TRUE(row.IsValid());
			ClickElement(context, row);
			for (int i = 0; i < 3; ++i) {
				Frame(panel);
			}
		}

		void ShowAllStyles(EditorPanel& panel, UI::ElementHandle host) {
			UI::Context& context = WindowContext();
			UI::ElementHandle row = RowNamed(context, host, "Show all");
			ASSERT_TRUE(row.IsValid());
			UI::ElementHandle box = FindByClass(context, row, StringHash("slt-checkbox"), 0);
			ASSERT_TRUE(box.IsValid());
			ClickElement(context, box);
			for (int i = 0; i < 3; ++i) {
				Frame(panel);
			}
		}

		static UIDebugOps::Geometry GeometryOf(UI::Context& context, UI::ElementHandle element) {
			UIDebugOps::Geometry geometry;
			UIDebugOps::BuildGeometry(context, element, geometry);
			return geometry;
		}
	};

	TEST_F(UIDebuggerPanelFixture, WindowsAndSurfacesAreTargets) {
		Vector<UIDebugOps::Target> targets{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::EnumerateTargets(targets);
		ASSERT_EQ(targets.size(), 1u);
		EXPECT_EQ(targets[0].kind, UIDebugOps::Target::Kind::Window);
		EXPECT_EQ(targets[0].label, "UIDebuggerTestWindow");
		EXPECT_EQ(UIDebugOps::ResolveTarget(targets[0]), &WindowContext());

		UISurfaceId surface = UISystem::CreateSurface({64, 48});
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		UIDebugOps::EnumerateTargets(targets);
		ASSERT_EQ(targets.size(), 2u);
		EXPECT_EQ(targets[1].kind, UIDebugOps::Target::Kind::Surface);
		EXPECT_EQ(UIDebugOps::ResolveTarget(targets[1]), UISystem::GetSurfaceContext(surface));
		EXPECT_NE(targets[1].label.find("64x48"), str::npos);

		UIDebugOps::Target dead = targets[1];
		UISystem::DestroySurface(surface);
		EXPECT_EQ(UIDebugOps::ResolveTarget(dead), nullptr);
		UIDebugOps::EnumerateTargets(targets);
		EXPECT_EQ(targets.size(), 1u);
	}

	TEST_F(UIDebuggerPanelFixture, PanelArmsOnOwnWindow) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());

		EXPECT_GT(DrawnRows(WindowContext(), host), 0u);
		EXPECT_TRUE(FindByText(WindowContext(), host, "UIDebuggerTestWindow").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, TargetingSettles) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		for (int i = 0; i < 6; ++i) {
			Frame(*panel.Get());
		}
		const u32 settled = WindowContext().GetLiveCount();
		ASSERT_GT(settled, 0u);
		for (int i = 0; i < 6; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(WindowContext().GetLiveCount(), settled);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, StructuralChangeToTargetReachesTreePane) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		for (int i = 0; i < 4; ++i) {
			Frame(*panel.Get());
		}
		const u32 before = DrawnRows(WindowContext(), host);
		ASSERT_GT(before, 0u);

		UI::Context& context = WindowContext();
		UI::ElementHandle other = context.CreateElement();
		context.AppendChild(context.GetRoot(), other);
		for (int i = 0; i < 12; ++i) {
			UI::ElementHandle leaf = context.CreateElement();
			context.AppendChild(other, leaf);
		}
		for (int i = 0; i < 4; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_GT(DrawnRows(WindowContext(), host), before);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, DeadTargetLeavesPanelLegible) {
		UISurfaceId surface = UISystem::CreateSurface({200, 200});
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		UI::Context* surfaceContext = UISystem::GetSurfaceContext(surface);
		ASSERT_NE(surfaceContext, nullptr);
		for (int i = 0; i < 5; ++i) {
			UI::ElementHandle leaf = surfaceContext->CreateElement();
			surfaceContext->AppendChild(surfaceContext->GetRoot(), leaf);
		}

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());
		Vector<UIDebugOps::Target> targets{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::EnumerateTargets(targets);
		ASSERT_EQ(targets.size(), 2u);
		UI::ElementHandle button = FindByText(WindowContext(), host, "UIDebuggerTestWindow");
		ASSERT_TRUE(button.IsValid());
		ClickElement(WindowContext(), button);
		UI::ElementHandle entry = FindByText(WindowContext(), WindowContext().GetRoot(), targets[1].label);
		ASSERT_TRUE(entry.IsValid());
		ClickElement(WindowContext(), entry);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		const u32 onSurface = DrawnRows(WindowContext(), host);
		ASSERT_GT(onSurface, 0u);

		UISystem::DestroySurface(surface);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(DrawnRows(WindowContext(), host), onSurface);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, DebuggersKeepSeparateState) {
		UI::ElementHandle hostA = Host();
		UI::ElementHandle hostB = Host();
		Handle<EditorPanel> a = OpenDebugger(hostA);
		Handle<EditorPanel> b = OpenDebugger(hostB);
		ASSERT_TRUE(a.IsValid());
		ASSERT_TRUE(b.IsValid());
		for (int i = 0; i < 5; ++i) {
			Frame(*a.Get());
			Frame(*b.Get());
		}
		const u32 rowsA = DrawnRows(WindowContext(), hostA);
		const u32 rowsB = DrawnRows(WindowContext(), hostB);
		ASSERT_GT(rowsA, 0u);
		ASSERT_GT(rowsB, 0u);

		const u32 settled = WindowContext().GetLiveCount();
		UI::ElementHandle rowOfA = FindByClass(WindowContext(), hostA, StringHash("slt-tree-row"), 1);
		ASSERT_TRUE(rowOfA.IsValid());
		for (int i = 0; i < 8; ++i) {
			Frame(*a.Get());
			Frame(*b.Get());
		}
		ASSERT_EQ(WindowContext().GetLiveCount(), settled);
		EXPECT_TRUE(WindowContext().IsAlive(rowOfA));

		UI::ElementHandle search = ControlWearing(WindowContext(), hostA, EditorIcons::GetGlyph(EditorIcons::Glyph::Search),
												  StringHash("slt-button"));
		ASSERT_TRUE(WindowContext().IsAlive(search));
		ClickElement(WindowContext(), search);
		for (int i = 0; i < 3; ++i) {
			Frame(*a.Get());
			Frame(*b.Get());
		}
		EXPECT_EQ(DrawnRows(WindowContext(), hostB), rowsB);
		PanelManager::ClosePanel(a);
		PanelManager::ClosePanel(b);
	}

	TEST_F(UIDebuggerPanelFixture, SelectionSurvivesTargetRebuild) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();
		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		context.AddClass(subject, StringHash("uidebug-probe-subject"));
		UI::ElementHandle inner = context.CreateElement();
		context.AppendChild(subject, inner);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		const u64 address = UIDebugOps::PathKey(path);
		UI::ElementHandle row = FindRowWithTag(context, host, address);
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		Frame(*panel.Get());
		ASSERT_EQ(CountByClass(context, host, StringHash("slt-selected")), 1u);

		context.ClearChildren(subject);
		UI::ElementHandle rebuilt = context.CreateElement();
		context.AppendChild(subject, rebuilt);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		UI::ElementHandle selected = FindByClass(context, host, StringHash("slt-selected"), 0);
		ASSERT_TRUE(selected.IsValid());
		EXPECT_EQ(CountByClass(context, host, StringHash("slt-selected")), 1u);
		const UI::Element* pod = context.GetElement(selected);
		ASSERT_NE(pod, nullptr);
		EXPECT_EQ(pod->userTag, address);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, SelectingRowFillsInspectorWithEverySection) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		context.AddClass(subject, EditorTheme::Class::SectionHeader);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		UI::ElementHandle row = FindRowWithTag(context, host, UIDebugOps::PathKey(path));
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}

		for (std::string_view title : {"Layout", "Stylesheets", "Matching Selectors", "Attributes", "Styles"}) {
			EXPECT_TRUE(FindByText(context, host, title).IsValid()) << title << " section";
		}
		EXPECT_TRUE(FindByText(context, host, "World Bound").IsValid());
		EXPECT_TRUE(FindByText(context, host, "World Clip").IsValid());
		EXPECT_TRUE(FindByText(context, host, "Content Rect").IsValid());
		EXPECT_TRUE(FindByText(context, host, ".ed-section-header").IsValid());
		EXPECT_TRUE(FindByText(context, host, "hoverable").IsValid());
		EXPECT_TRUE(FindByText(context, host, "background-color").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, ShowAllRevealsDefaults) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		context.AddClass(subject, EditorTheme::Class::SectionHeader);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		ClickElement(context, FindRowWithTag(context, host, UIDebugOps::PathKey(path)));
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}

		auto drawnProperties = [&context, host]() {
			u32 count = 0;
			for (std::string_view name : UI::GetStylePropertyNames()) {
				if (FindByText(context, host, name).IsValid()) {
					++count;
				}
			}
			return count;
		};
		const u32 total = static_cast<u32>(UI::GetStylePropertyNames().size());
		const u32 filtered = drawnProperties();
		EXPECT_GT(filtered, 0u);
		EXPECT_LT(filtered, total);

		ShowAllStyles(*panel.Get(), host);
		EXPECT_EQ(drawnProperties(), total);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, FoldingSectionTakesBodyAndLeavesOthers) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();
		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		context.AddClass(subject, EditorTheme::Class::SectionHeader);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		ClickElement(context, FindRowWithTag(context, host, UIDebugOps::PathKey(path)));
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		ASSERT_TRUE(FindByText(context, host, "World Bound").IsValid());
		ASSERT_TRUE(FindByText(context, host, "hoverable").IsValid());

		UI::ElementHandle header = FindByText(context, host, "Layout");
		ASSERT_TRUE(header.IsValid());
		const UI::Element* labelPod = context.GetElement(header);
		ASSERT_NE(labelPod, nullptr);
		ClickElement(context, context.MakeHandle(labelPod->parent));
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_FALSE(FindByText(context, host, "World Bound").IsValid());
		EXPECT_TRUE(FindByText(context, host, "Layout").IsValid());
		EXPECT_TRUE(FindByText(context, host, "hoverable").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, InspectorIsMarkedOutOfEverySnapshot) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();
		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		context.AddClass(subject, EditorTheme::Class::SectionHeader);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}

		UIDebugOps::Snapshot before;
		UIDebugOps::BuildSnapshot(context, before);
		const u32 liveBefore = context.GetLiveCount();

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		ClickElement(context, FindRowWithTag(context, host, UIDebugOps::PathKey(path)));
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}

		UIDebugOps::Snapshot after;
		UIDebugOps::BuildSnapshot(context, after);
		EXPECT_GT(context.GetLiveCount(), liveBefore + 20u);
		EXPECT_EQ(after.rows.size(), before.rows.size());

		const size_t settled = after.rows.size();
		const u32 liveSettled = context.GetLiveCount();
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		UIDebugOps::Snapshot last;
		UIDebugOps::BuildSnapshot(context, last);
		EXPECT_EQ(last.rows.size(), settled);
		EXPECT_EQ(context.GetLiveCount(), liveSettled);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerTest, TargetNameTruncatesNotWrappingHeaderBand) {
		EditorSelection::Initialize();
		PanelManager::Initialize();
		RegisterBuiltInPanels();

		f32 bandHeight = 0.0f;
		auto measure = [&](f32 widthPx) {
			MockFontSystem fonts;
			UI::ContextDesc desc;
			desc.sizePx = {static_cast<i32>(widthPx), 400};
			UI::Context context(desc, &fonts);
			DressLikeTheEditor(context);
			const TypeInfo* type = TypeRegistry::FindType(StringHash("UIDebuggerPanel"));
			ASSERT_NE(type, nullptr);
			Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
			ASSERT_TRUE(panel.IsValid());
			UI::ElementHandle root = Child(context, context.GetRoot());
			panel.Get()->BuildContent(context, root);
			context.Update();
			UI::ElementHandle strip = FindByClass(context, root, EditorTheme::Class::PanelHeader, 0);
			ASSERT_TRUE(strip.IsValid());
			bandHeight = context.GetLayout(strip).borderBox.h;
			PanelManager::ClosePanel(panel);
		};

		measure(4000.0f);
		const f32 roomy = bandHeight;
		ASSERT_GT(roomy, 0.0f);
		measure(120.0f);
		EXPECT_FLOAT_EQ(bandHeight, roomy);

		PanelManager::Terminate();
		EditorSelection::Terminate();
	}

	class UIDebuggerProjectFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		Handle<Window> _window;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 unique = counter.fetch_add(1);
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() / ("shalltear_uidebug_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root / "Assets");
			FileSystem::Mount("project", Path(_root.generic_string()));

			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			EditorSelection::Initialize();
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			PanelManager::Initialize();

			WindowDesc desc;
			desc.title = "UIDebuggerProjectWindow";
			desc.clientSize = {1200, 800};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			UI::Context* context = UISystem::CreateContext(_window);
			ASSERT_NE(context, nullptr);
			EditorTheme::ApplyToContext(*context);
		}

		void TearDown() override {
			PanelManager::Terminate();
			UISystem::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			AssetDatabase::Terminate();
			EditorSelection::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WriteSource(const str& virtualPath, const str& content) {
			Path resolved = FileSystem::Resolve(Path(virtualPath));
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(resolved.ToString()).parent_path(), ec);
			FileSystem::WriteTextFile(resolved, content);
		}

		UI::Context& WindowContext() { return *UISystem::GetContext(_window); }
	};

	TEST_F(UIDebuggerProjectFixture, ProjectSheetIsReadableInStylesheetsSection) {
		WriteSource("project://Assets/Probe.slss", ".uidebug-probe-rule { flex-direction: row; }\n");
		WriteSource("project://Assets/Probe.slxml", R"(<ui version="1">
	<sheet src="project://Assets/Probe.slss"/>
	<panel id="uidebug-probe-root"/>
</ui>)");
		AssetDatabase::ScanProjectDirectory();

		Asset* loaded = AssetDatabase::LoadAsset(Path("project://Assets/Probe.slxml"));
		ASSERT_NE(loaded, nullptr);
		UIDocumentAsset* document = static_cast<UIDocumentAsset*>(loaded);
		ASSERT_EQ(document->sheets.size(), 1u);
		ASSERT_TRUE(document->sheets[0].sheet.IsValid());

		UI::Context& context = WindowContext();
		UI::ElementHandle documentRoot = context.CreateElement();
		context.AppendChild(context.GetRoot(), documentRoot);
		document->Instantiate(context, documentRoot);

		UI::ElementHandle host = context.CreateElement();
		context.AppendChild(context.GetRoot(), host);
		const TypeInfo* type = TypeRegistry::FindType(StringHash("UIDebuggerPanel"));
		ASSERT_NE(type, nullptr);
		Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
		ASSERT_TRUE(panel.IsValid());
		panel.Get()->BuildContent(context, host);
		for (int i = 0; i < 5; ++i) {
			panel.Get()->OnTick(0.0f);
			UISystem::Update();
		}

		ClickElement(context, FindByClass(context, host, StringHash("slt-tree-row"), 0));
		for (int i = 0; i < 3; ++i) {
			panel.Get()->OnTick(0.0f);
			UISystem::Update();
		}

		EXPECT_TRUE(FindByText(context, host, "project://Assets/Probe.slss").IsValid());
		EXPECT_TRUE(FindByText(context, host, "#uidebug-probe-root").IsValid());
		EXPECT_TRUE(FindByText(context, host, "1 rules").IsValid());
		PanelManager::ClosePanel(panel);
		AssetDatabase::UnloadAsset(loaded);
	}

	TEST_F(UIDebuggerTest, MainWindowCostReported) {
		EditorSelection::Initialize();
		PanelManager::Initialize();
		RegisterBuiltInPanels();

		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {1600, 900};
		UI::Context context(desc, &fonts);
		DressLikeTheEditor(context);

		const char* defaultLayout[] = {"HierarchyPanel", "SceneViewPanel", "InspectorPanel",
									   "AssetBrowserPanel", "ConsolePanel", "UIDebuggerPanel"};
		u32 built = 0;
		for (const char* name : defaultLayout) {
			const TypeInfo* type = TypeRegistry::FindType(StringHash(name));
			if (!type) {
				continue;
			}
			Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
			if (!panel.IsValid()) {
				continue;
			}
			UI::ElementHandle host = Child(context, context.GetRoot());
			panel.Get()->BuildContent(context, host);
			++built;
		}
		context.Update();
		ASSERT_GE(built, 5u);

		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(context, snapshot);
		const size_t rowBytes = sizeof(UIDebugOps::Row);
		const size_t bytes = snapshot.rows.capacity() * rowBytes;
		std::printf("[ M2 COST  ] %u panels, %u live elements, %zu snapshot rows\n", built,
					context.GetLiveCount(), snapshot.rows.size());
		std::printf("[ M2 COST  ] Row is %zu B -> snapshot is %zu B (%zu B/element)\n", rowBytes, bytes,
					rowBytes);

		ASSERT_GT(snapshot.rows.size(), 50u);
		EXPECT_LE(rowBytes, 32u);

		PanelManager::Terminate();
		EditorSelection::Terminate();
	}

	TEST_F(UIDebuggerTest, ReadablePropertiesWritable) {
		UI::Style source;
		source.width = UI::Length::Percent(0.5f);
		source.padding = {1.0f, 2.0f, 3.0f, 4.0f};
		source.background = UI::Brush::SolidColor(Color(0.25f, 0.5f, 0.75f, 1.0f));
		source.opacity = 0.5f;

		char buffer[UI::MAX_STYLE_VALUE_TEXT];
		for (std::string_view name : UI::GetStylePropertyNames()) {
			const StringHash hash{name};
			std::string_view text = UI::ReadStyleProperty(source, hash, buffer, sizeof(buffer));
			UI::Style target;
			EXPECT_TRUE(UI::ApplyStyleDeclaration(target, hash, text, nullptr)) << name;
			char back[UI::MAX_STYLE_VALUE_TEXT];
			EXPECT_EQ(UI::ReadStyleProperty(target, hash, back, sizeof(back)), text) << "'" << name;
		}
	}

	TEST_F(UIDebuggerTest, UnknownPropertyNameIsNotWrite) {
		UI::Style style;
		style.opacity = 0.25f;
		const UI::Style before = style;
		EXPECT_FALSE(UI::ApplyStyleDeclaration(style, StringHash("not-a-property"), "3", nullptr));
		EXPECT_EQ(style, before);
	}

	TEST_F(UIDebuggerTest, SeamAndRuleApplyDeclarationIdentically) {
		UI::StyleVars vars;
		vars.Set(StringHash("--probe"), "#112233ff");

		UI::StyleSheet sheet;
		sheet.Parse(".probe { background-color: var(--probe); margin-left: 7; }");
		ASSERT_EQ(sheet.GetRules().size(), 1u);
		UI::Style byRule;
		UI::StyleSheet::ApplyRule(byRule, sheet.GetRules()[0], &vars);

		UI::Style bySeam;
		EXPECT_TRUE(UI::ApplyStyleDeclaration(bySeam, StringHash("background-color"), "var(--probe)", &vars));
		EXPECT_TRUE(UI::ApplyStyleDeclaration(bySeam, StringHash("margin-left"), "7", &vars));
		EXPECT_EQ(bySeam, byRule);

		UI::Style unresolved;
		EXPECT_TRUE(UI::ApplyStyleDeclaration(unresolved, StringHash("background-color"), "var(--probe)", nullptr));
		EXPECT_EQ(unresolved.background.solid, Color::Magenta());
	}

	TEST_F(UIDebuggerTest, EditAndRevertStyle) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style built;
		built.padding = UI::Edges::All(4.0f);
		context.SetStyle(element, built);

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("padding"), "20"));
		EXPECT_EQ(context.GetComputedStyle(element).padding.left, 20.0f);
		EXPECT_TRUE(edits.IsEdited(StringHash("padding")));

		ASSERT_TRUE(UIDebugOps::RevertProperty(context, element, edits, StringHash("padding")));
		EXPECT_EQ(context.GetComputedStyle(element).padding.left, 4.0f);
		EXPECT_FALSE(edits.IsEdited(StringHash("padding")));
		EXPECT_FALSE(UIDebugOps::RevertProperty(context, element, edits, StringHash("padding")));
	}

	TEST_F(UIDebuggerTest, RevertRestoresFieldNotReparseOfText) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style built;
		built.background = UI::Brush::ImageBrush(RHI::TextureHandle{9, 2});
		context.SetStyle(element, built);

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("background-color"), "#ff0000ff"));
		EXPECT_EQ(context.GetStyle(element).background.type, UI::BrushType::Solid);

		ASSERT_TRUE(UIDebugOps::RevertProperty(context, element, edits, StringHash("background-color")));
		const UI::Style& back = context.GetStyle(element);
		EXPECT_EQ(back.background.type, UI::BrushType::Image);
		EXPECT_EQ(back.background.image.index, 9u);
		EXPECT_EQ(back, built);
	}

	TEST_F(UIDebuggerTest, EditingOnePropertyTwiceRevertsAllWay) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style built;
		built.opacity = 1.0f;
		context.SetStyle(element, built);

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.5"));
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.25"));
		EXPECT_EQ(edits.declarations.size(), 1u);
		EXPECT_FLOAT_EQ(context.GetStyle(element).opacity, 0.25f);
		ASSERT_TRUE(UIDebugOps::RevertProperty(context, element, edits, StringHash("opacity")));
		EXPECT_FLOAT_EQ(context.GetStyle(element).opacity, 1.0f);
	}

	TEST_F(UIDebuggerTest, RevertingOnePropertyKeepsTheOtherEdits) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle element = Child(context, context.GetRoot());

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.5"));
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("width"), "120"));
		ASSERT_TRUE(UIDebugOps::RevertProperty(context, element, edits, StringHash("opacity")));
		EXPECT_FLOAT_EQ(context.GetStyle(element).opacity, 1.0f);
		EXPECT_EQ(context.GetStyle(element).width.value, 120.0f);
	}

	TEST_F(UIDebuggerTest, ForeignWriteRebasesSession) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle element = Child(context, context.GetRoot());
		UI::Style built;
		built.opacity = 1.0f;
		context.SetStyle(element, built);

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.5"));

		UI::Style theirs;
		theirs.opacity = 0.75f;
		theirs.width = UI::Length::Pixel(64.0f);
		context.SetStyle(element, theirs);

		EXPECT_FALSE(UIDebugOps::RevertProperty(context, element, edits, StringHash("opacity")));
		EXPECT_EQ(context.GetStyle(element), theirs);

		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.25"));
		EXPECT_EQ(context.GetStyle(element).width.value, 64.0f);
		ASSERT_TRUE(UIDebugOps::RevertProperty(context, element, edits, StringHash("opacity")));
		EXPECT_FLOAT_EQ(context.GetStyle(element).opacity, 0.75f);
	}

	TEST_F(UIDebuggerTest, RuleOwnedEditTakenBack) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		LoadProbeSheet(context, names, "probe-sheet", ".boxed { opacity: 0.8; }");
		names.Rebuild(context);

		UI::ElementHandle element = Child(context, context.GetRoot());
		context.AddClass(element, StringHash("boxed"));

		UIDebugOps::StyleEdits edits;
		ASSERT_TRUE(UIDebugOps::SetProperty(context, element, edits, StringHash("opacity"), "0.1"));
		EXPECT_FLOAT_EQ(context.GetStyle(element).opacity, 0.1f);
		EXPECT_FLOAT_EQ(context.GetComputedStyle(element).opacity, 0.8f);

		Vector<UIDebugOps::PropertyRow> rows{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildProperties(context, element, names, rows);
		bool checked = false;
		for (const UIDebugOps::PropertyRow& row : rows) {
			if (row.name != "opacity") {
				continue;
			}
			checked = true;
			EXPECT_EQ(row.source, UIDebugOps::ValueSource::Sheet);
			EXPECT_EQ(row.sheet, "probe-sheet");
		}
		EXPECT_TRUE(checked);
	}

	TEST_F(UIDebuggerTest, EventsRowCreatesNothing) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		names.Rebuild(context);

		UI::ElementHandle wired = Child(context, context.GetRoot());
		UI::ElementHandle bare = Child(context, context.GetRoot());
		context.Events(wired).onClick = []() {};
		ASSERT_TRUE(context.HasEvents(wired));
		ASSERT_FALSE(context.HasEvents(bare));

		auto contentOf = [&](UI::ElementHandle element) {
			Vector<UIDebugOps::AttributeRow> rows{GetMemoryResource(MemTag::Editor)};
			UIDebugOps::BuildAttributes(context, element, names, rows);
			for (const UIDebugOps::AttributeRow& row : rows) {
				if (row.name == "content") {
					return row.value;
				}
			}
			return str{};
		};

		EXPECT_NE(contentOf(wired).find("events"), str::npos);
		EXPECT_EQ(contentOf(bare).find("events"), str::npos);
		EXPECT_FALSE(context.HasEvents(bare));
	}

	TEST_F(UIDebuggerTest, ClassTextDiffsAndNamesSplit) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		names.Rebuild(context);

		UI::ElementHandle element = Child(context, context.GetRoot());
		UIDebugOps::SetClasses(context, element, ".alpha .beta", names);
		const UI::Element* pod = context.GetElement(element);
		ASSERT_NE(pod, nullptr);
		ASSERT_EQ(pod->classCount, 2u);
		EXPECT_TRUE(context.HasClass(element, StringHash("alpha")));
		EXPECT_TRUE(context.HasClass(element, StringHash("beta")));
		EXPECT_EQ(names.Lookup(StringHash("alpha")), "alpha");

		const u32 settled = context.GetTreeRevision();
		UIDebugOps::SetClasses(context, element, "beta alpha", names);
		EXPECT_EQ(context.GetTreeRevision(), settled);

		UIDebugOps::SetClasses(context, element, ".beta .gamma", names);
		EXPECT_FALSE(context.HasClass(element, StringHash("alpha")));
		EXPECT_TRUE(context.HasClass(element, StringHash("gamma")));

		UIDebugOps::SetClasses(context, element, ".a .b .c .d .e .f", names);
		EXPECT_EQ(context.GetElement(element)->classCount, UI::MAX_ELEMENT_CLASSES);
	}

	TEST_F(UIDebuggerTest, AttributeRowsCarryWhatEditingThemMeans) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		names.Rebuild(context);
		UI::ElementHandle element = Child(context, context.GetRoot());

		Vector<UIDebugOps::AttributeRow> rows{GetMemoryResource(MemTag::Editor)};
		using Edit = UIDebugOps::AttributeRow::Edit;
		auto editOf = [&rows](std::string_view name) {
			for (const UIDebugOps::AttributeRow& row : rows) {
				if (row.name == name) {
					return row.edit;
				}
			}
			return Edit::None;
		};
		UIDebugOps::BuildAttributes(context, element, names, rows);
		EXPECT_EQ(editOf("id"), Edit::Id);
		EXPECT_EQ(editOf("classes"), Edit::Classes);
		EXPECT_EQ(editOf("visible"), Edit::Visible);
		EXPECT_EQ(editOf("tint"), Edit::Tint);
		EXPECT_EQ(editOf("scroll offset"), Edit::ScrollOffset);
		EXPECT_EQ(editOf("state"), Edit::None);
		EXPECT_EQ(editOf("max scroll"), Edit::None);
		EXPECT_EQ(editOf("cursor"), Edit::None);
		EXPECT_EQ(editOf("z index"), Edit::None);

		EXPECT_EQ(editOf("text"), Edit::None);
		context.SetText(element, {}, 12.0f, Color::White(), "hello");
		UIDebugOps::BuildAttributes(context, element, names, rows);
		EXPECT_EQ(editOf("text"), Edit::Text);
		for (const UIDebugOps::AttributeRow& row : rows) {
			if (row.name == "text") {
				EXPECT_EQ(row.value, "hello");
			}
		}
	}

	TEST_F(UIDebuggerPanelFixture, TypedPaddingMovesElement) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		UI::Style built;
		built.padding = UI::Edges::All(3.0f);
		context.SetStyle(subject, built);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		SelectSubject(*panel.Get(), host, subject);

		UI::ElementHandle field = ValueFieldOnRow(context, host, "padding");
		ASSERT_TRUE(field.IsValid());
		TypeInto(context, field, "17");
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(context.GetComputedStyle(subject).padding.left, 17.0f);

		UI::ElementHandle revert = RevertOnRow(context, host, "padding");
		ASSERT_TRUE(revert.IsValid());
		ClickElement(context, revert);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(context.GetComputedStyle(subject).padding.left, 3.0f);
		EXPECT_FALSE(RevertOnRow(context, host, "padding").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, ColorEditRefreshesTable) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		UI::Style built;
		built.width = UI::Length::Pixel(40.0f);
		built.height = UI::Length::Pixel(20.0f);
		context.SetStyle(subject, built);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		SelectSubject(*panel.Get(), host, subject);
		ShowAllStyles(*panel.Get(), host);

		const UIDebugOps::Geometry before = GeometryOf(context, subject);
		UI::ElementHandle field = ValueFieldOnRow(context, host, "background-color");
		ASSERT_TRUE(field.IsValid());
		TypeInto(context, field, "#123456ff");
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		ASSERT_EQ(context.GetStyle(subject).background.solid, Color::FromHex("#123456ff"));
		ASSERT_EQ(GeometryOf(context, subject), before);

		EXPECT_TRUE(RowSaysInline(context, host, "background-color"));
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, ClickingVisibleCheckboxWritesToLiveElement) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		SelectSubject(*panel.Get(), host, subject);
		ASSERT_TRUE(HasFlag(context.GetElement(subject)->flags, UI::ElementFlags::Visible));

		UI::ElementHandle row = RowNamed(context, host, "visible");
		ASSERT_TRUE(row.IsValid());
		UI::ElementHandle box = FindByClass(context, row, StringHash("slt-checkbox"), 0);
		ASSERT_TRUE(box.IsValid());
		ClickElement(context, box);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_FALSE(HasFlag(context.GetElement(subject)->flags, UI::ElementFlags::Visible));
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, TypedIdShowsInTree) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		SelectSubject(*panel.Get(), host, subject);

		UI::ElementHandle field = ValueFieldOnRow(context, host, "id");
		ASSERT_TRUE(field.IsValid());
		TypeInto(context, field, "hud-root");
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(context.GetElement(subject)->id, StringHash("hud-root"));
		EXPECT_TRUE(FindByText(context, host, "#hud-root").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, EditDiesWithOwnerRebuild) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle owner = context.CreateElement();
		context.AppendChild(context.GetRoot(), owner);
		auto build = [&context, owner]() {
			context.ClearChildren(owner);
			UI::ElementHandle child = context.CreateElement();
			context.AppendChild(owner, child);
			UI::Style style;
			style.padding = UI::Edges::All(3.0f);
			context.SetStyle(child, style);
			return child;
		};
		UI::ElementHandle subject = build();
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		SelectSubject(*panel.Get(), host, subject);

		UI::ElementHandle field = ValueFieldOnRow(context, host, "padding");
		ASSERT_TRUE(field.IsValid());
		TypeInto(context, field, "17");
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		ASSERT_EQ(context.GetComputedStyle(subject).padding.left, 17.0f);

		UI::ElementHandle rebuilt = build();
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_NE(rebuilt, subject);
		EXPECT_EQ(context.GetComputedStyle(rebuilt).padding.left, 3.0f);
		EXPECT_FALSE(RevertOnRow(context, host, "padding").IsValid());
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, ArmedPickClaimsAllWindows) {
		WindowDesc second;
		second.title = "UIDebuggerSecondWindow";
		second.clientSize = {600, 400};
		second.flags = WindowFlags::Hidden;
		Handle<Window> other = WindowSystem::Create(second);
		UI::Context* elsewhere = UISystem::CreateContext(other);
		ASSERT_NE(elsewhere, nullptr);

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());
		UI::Context& context = WindowContext();

		ASSERT_TRUE(PointerIsFree(context));
		ASSERT_TRUE(PointerIsFree(*elsewhere));

		ArmPick(*panel.Get(), host);
		EXPECT_FALSE(PointerIsFree(context));
		EXPECT_FALSE(PointerIsFree(*elsewhere));

		PanelManager::ClosePanel(panel);
		EXPECT_TRUE(PointerIsFree(context));
		EXPECT_TRUE(PointerIsFree(*elsewhere));
	}

	TEST_F(UIDebuggerPanelFixture, PickLeavesLiveCount) {
		WindowDesc second;
		second.title = "UIDebuggerSecondWindow";
		second.clientSize = {600, 400};
		second.flags = WindowFlags::Hidden;
		Handle<Window> other = WindowSystem::Create(second);
		UI::Context* elsewhere = UISystem::CreateContext(other);
		ASSERT_NE(elsewhere, nullptr);
		UI::ElementHandle stranger = elsewhere->CreateElement();
		elsewhere->AppendChild(elsewhere->GetRoot(), stranger);

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());

		const u32 before = elsewhere->GetLiveCount();
		ArmPick(*panel.Get(), host);
		EXPECT_EQ(elsewhere->GetLiveCount(), before + UIDebugOps::HIGHLIGHT_BAND_COUNT);

		PanelManager::ClosePanel(panel);
		EXPECT_EQ(elsewhere->GetLiveCount(), before);
		EXPECT_TRUE(PointerIsFree(*elsewhere));
	}

	TEST_F(UIDebuggerPanelFixture, PickHighlightHiddenFromSnapshots) {
		WindowDesc second;
		second.title = "UIDebuggerSecondWindow";
		second.clientSize = {600, 400};
		second.flags = WindowFlags::Hidden;
		Handle<Window> other = WindowSystem::Create(second);
		UI::Context* elsewhere = UISystem::CreateContext(other);
		ASSERT_NE(elsewhere, nullptr);
		elsewhere->AppendChild(elsewhere->GetRoot(), elsewhere->CreateElement());

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());

		UIDebugOps::Snapshot before;
		UIDebugOps::BuildSnapshot(*elsewhere, before);
		ArmPick(*panel.Get(), host);

		UIDebugOps::Snapshot after;
		UIDebugOps::BuildSnapshot(*elsewhere, after);
		EXPECT_EQ(after.rows.size(), before.rows.size());
		EXPECT_EQ(elsewhere->GetLiveCount(), before.rows.size() - 1 + UIDebugOps::HIGHLIGHT_BAND_COUNT);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerPanelFixture, SecondPickCannotArm) {
		WindowDesc second;
		second.title = "UIDebuggerSecondWindow";
		second.clientSize = {600, 400};
		second.flags = WindowFlags::Hidden;
		Handle<Window> other = WindowSystem::Create(second);
		UI::Context* elsewhere = UISystem::CreateContext(other);
		ASSERT_NE(elsewhere, nullptr);

		UI::ElementHandle hostA = Host();
		UI::ElementHandle hostB = Host();
		Handle<EditorPanel> a = OpenDebugger(hostA);
		Handle<EditorPanel> b = OpenDebugger(hostB);
		ASSERT_TRUE(a.IsValid() && b.IsValid());
		Frame(*a.Get());
		Frame(*b.Get());

		const u32 before = elsewhere->GetLiveCount();
		ArmPick(*a.Get(), hostA);
		ASSERT_EQ(elsewhere->GetLiveCount(), before + UIDebugOps::HIGHLIGHT_BAND_COUNT);
		ASSERT_TRUE(IsPickArmed(WindowContext(), hostA));

		ArmPick(*b.Get(), hostB);
		EXPECT_FALSE(IsPickArmed(WindowContext(), hostB));
		EXPECT_TRUE(IsPickArmed(WindowContext(), hostA));
		EXPECT_EQ(elsewhere->GetLiveCount(), before + UIDebugOps::HIGHLIGHT_BAND_COUNT);
		EXPECT_FALSE(PointerIsFree(*elsewhere));

		PanelManager::ClosePanel(b);
		EXPECT_EQ(elsewhere->GetLiveCount(), before + UIDebugOps::HIGHLIGHT_BAND_COUNT);
		EXPECT_FALSE(PointerIsFree(*elsewhere));

		PanelManager::ClosePanel(a);
		EXPECT_EQ(elsewhere->GetLiveCount(), before);
		EXPECT_TRUE(PointerIsFree(*elsewhere));
	}

	TEST_F(UIDebuggerPanelFixture, ArmedPickClaimsAllContexts) {
		UISurfaceId surface = UISystem::CreateSurface({320, 240});
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		UI::Context* offscreen = UISystem::GetSurfaceContext(surface);
		ASSERT_NE(offscreen, nullptr);
		const u32 before = offscreen->GetLiveCount();

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());
		ArmPick(*panel.Get(), host);

		EXPECT_FALSE(PointerIsFree(*offscreen));
		EXPECT_EQ(offscreen->GetLiveCount(), before + UIDebugOps::HIGHLIGHT_BAND_COUNT);

		PanelManager::ClosePanel(panel);
		EXPECT_EQ(offscreen->GetLiveCount(), before);
		UISystem::DestroySurface(surface);
	}

	namespace {

		u32 RootBands(UI::Context& context, bool visibleOnly) {
			u32 count = 0;
			const UI::Element* root = context.GetElement(context.GetRoot());
			for (u32 child = root->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (context.HasClass(handle, StringHash(UIDebugOps::MarkerClass)) &&
					(!visibleOnly || HasFlag(context.GetElement(handle)->flags, UI::ElementFlags::Visible))) {
					++count;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return count;
		}

	} // namespace

	TEST_F(UIDebuggerPanelFixture, SelectedElementLitInTree) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		UI::Style style;
		style.position = UI::Position::Absolute;
		style.left = UI::Length::Pixel(120.0f);
		style.top = UI::Length::Pixel(90.0f);
		style.width = UI::Length::Pixel(200.0f);
		style.height = UI::Length::Pixel(60.0f);
		style.pointerEvents = false;
		context.SetStyle(subject, style);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_EQ(RootBands(context, true), 0u);

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		UI::ElementHandle row = FindRowWithTag(context, host, UIDebugOps::PathKey(path));
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}

		ASSERT_EQ(RootBands(context, false), UIDebugOps::HIGHLIGHT_BAND_COUNT);
		const UI::Rect& want = context.GetLayout(subject).content;
		bool landed = false;
		const UI::Element* root = context.GetElement(context.GetRoot());
		for (u32 child = root->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle band = context.MakeHandle(child);
			if (context.HasClass(band, StringHash(UIDebugOps::MarkerClass)) &&
				HasFlag(context.GetElement(band)->flags, UI::ElementFlags::Visible)) {
				const UI::Rect& got = context.GetLayout(band).borderBox;
				landed = landed || (std::abs(got.x - want.x) < 0.51f && std::abs(got.y - want.y) < 0.51f &&
									std::abs(got.w - want.w) < 0.51f && std::abs(got.h - want.h) < 0.51f);
			}
			child = context.GetElement(band)->nextSibling;
		}
		EXPECT_TRUE(landed);

		PanelManager::ClosePanel(panel);
		EXPECT_EQ(RootBands(context, false), 0u);
	}

	TEST_F(UIDebuggerPanelFixture, ArmingPickTakesSelectionsHighlightOffTree) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		UI::Context& context = WindowContext();

		UI::ElementHandle subject = context.CreateElement();
		context.AppendChild(context.GetRoot(), subject);
		UI::Style style;
		style.position = UI::Position::Absolute;
		style.left = UI::Length::Pixel(40.0f);
		style.top = UI::Length::Pixel(40.0f);
		style.width = UI::Length::Pixel(150.0f);
		style.height = UI::Length::Pixel(50.0f);
		style.pointerEvents = false;
		context.SetStyle(subject, style);
		for (int i = 0; i < 5; ++i) {
			Frame(*panel.Get());
		}
		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, subject, path);
		UI::ElementHandle row = FindRowWithTag(context, host, UIDebugOps::PathKey(path));
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		for (int i = 0; i < 3; ++i) {
			Frame(*panel.Get());
		}
		ASSERT_GT(RootBands(context, true), 0u);

		ArmPick(*panel.Get(), host);
		EXPECT_EQ(RootBands(context, true), 0u);
		PanelManager::ClosePanel(panel);
	}

	TEST_F(UIDebuggerTest, NameEditorTypedSurvivesRebuildOfDictionary) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UIDebugOps::NameDictionary names;
		names.Rebuild(context);

		EXPECT_TRUE(names.Lookup(StringHash("hud-root")).empty());
		names.AddName("hud-root");
		EXPECT_EQ(names.Lookup(StringHash("hud-root")), "hud-root");
		names.Rebuild(context);
		EXPECT_EQ(names.Lookup(StringHash("hud-root")), "hud-root");
		EXPECT_EQ(names.Render(StringHash("hud-root")), "hud-root");
	}

	class UIDebuggerTargetFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		Handle<Window> _window;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 unique = counter.fetch_add(1);
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() / ("shalltear_uidebug_m6_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root / "Assets");
			FileSystem::Mount("project", Path(_root.generic_string()));

			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			EditorSelection::Initialize();
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			PanelManager::Initialize();

			WindowDesc desc;
			desc.title = "UIDebuggerTargetWindow";
			desc.clientSize = {1400, 900};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			UI::Context* context = UISystem::CreateContext(_window);
			ASSERT_NE(context, nullptr);
			EditorTheme::ApplyToContext(*context);
		}

		void TearDown() override {
			PanelManager::Terminate();
			UISystem::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			AssetDatabase::Terminate();
			EditorSelection::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		UI::Context& WindowContext() { return *UISystem::GetContext(_window); }

		void WriteSource(const str& virtualPath, const str& content) {
			Path resolved = FileSystem::Resolve(Path(virtualPath));
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(resolved.ToString()).parent_path(), ec);
			FileSystem::WriteTextFile(resolved, content);
		}

		void WriteProbeDocument() {
			WriteSource("project://Assets/M6.slss",
						".m6-row { flex-direction: row; }\n"
						".m6-row { align-items: center; }\n"
						".m6-row:hover { flex-direction: column; }\n");
			WriteSource("project://Assets/M6.slxml",
						"<ui version=\"1\">\n\t<sheet src=\"project://Assets/M6.slss\"/>\n"
						"\t<panel id=\"m6-root\" class=\"m6-row\"/>\n</ui>");
			AssetDatabase::ScanProjectDirectory();
		}

		UI::ElementHandle Host() {
			UI::Context& context = WindowContext();
			UI::ElementHandle host = context.CreateElement();
			context.AppendChild(context.GetRoot(), host);
			return host;
		}

		Handle<EditorPanel> OpenPanelOfType(const char* typeName, UI::ElementHandle host) {
			const TypeInfo* type = TypeRegistry::FindType(StringHash(typeName));
			if (!type) {
				return {};
			}
			Handle<EditorPanel> panel = PanelManager::OpenPanel(type);
			if (panel.IsValid()) {
				panel.Get()->BuildContent(WindowContext(), host);
			}
			return panel;
		}

		void Frame(EditorPanel& panel) {
			panel.OnTick(0.0f);
			UISystem::Update();
		}

		void Settle(EditorPanel& panel, int frames = 4) {
			for (int i = 0; i < frames; ++i) {
				Frame(panel);
			}
		}

		static UIDebugOps::Target TargetOf(EditorPanel* panel) {
			UIDebugOps::Target target;
			Variant kind;
			Variant id;
			EXPECT_TRUE(GetField(panel, panel->GetType(), StringHash("_targetKind"), &kind));
			EXPECT_TRUE(GetField(panel, panel->GetType(), StringHash("_targetId"), &id));
			if (const u8* value = kind.TryGet<u8>()) {
				target.kind = static_cast<UIDebugOps::Target::Kind>(*value);
			}
			if (const u64* value = id.TryGet<u64>()) {
				target.id = *value;
			}
			return target;
		}

		static EditorPanel* OpenDebuggerPanel() {
			Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
			PanelManager::GetOpenPanels(panels);
			for (const Handle<EditorPanel>& panel : panels) {
				EditorPanel* resolved = panel.Get();
				if (resolved && resolved->GetType() && resolved->GetType()->name == StringHash("UIDebuggerPanel")) {
					return resolved;
				}
			}
			return nullptr;
		}

		static UI::ElementHandle OverflowButton(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle found;
			for (u32 i = 0;; ++i) {
				UI::ElementHandle candidate = FindByClass(context, host, EditorTheme::Class::IconButton, i);
				if (!candidate.IsValid()) {
					break;
				}
				found = candidate;
			}
			return found;
		}

		static void ClickMenuItem(UI::Context& context, std::string_view label) {
			UI::ElementHandle text = FindByText(context, context.GetRoot(), label);
			ASSERT_TRUE(text.IsValid()) << label << "'";
			const UI::Element* pod = context.GetElement(text);
			ASSERT_NE(pod, nullptr);
			ASSERT_NE(pod->parent, UI::INVALID_UI_INDEX);
			ClickElement(context, context.MakeHandle(pod->parent));
			context.Update();
		}
	};

	TEST_F(UIDebuggerTargetFixture, SurfaceNamedByPresenter) {
		UISurfaceId orphan = UISystem::CreateSurface({64, 48});
		ASSERT_NE(orphan, INVALID_UI_SURFACE);

		WriteProbeDocument();
		Handle<EditorPanel> builder = OpenPanelOfType("UIBuilderPanel", Host());
		ASSERT_TRUE(builder.IsValid());
		ASSERT_TRUE(UIBuilderOps::OpenDocumentInBuilder("project://Assets/M6.slxml"));
		Settle(*builder.Get());

		const UISurfaceId presented = builder.Get()->GetPresentedSurface();
		ASSERT_NE(presented, INVALID_UI_SURFACE);
		ASSERT_NE(presented, orphan);

		Vector<UIDebugOps::Target> targets{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::EnumerateTargets(targets);
		str named;
		str unnamed;
		for (const UIDebugOps::Target& target : targets) {
			if (target.kind != UIDebugOps::Target::Kind::Surface) {
				continue;
			}
			(target.id == presented ? named : unnamed) = target.label;
		}
		EXPECT_NE(named.find("UI Builder"), str::npos) << named << "'";
		EXPECT_EQ(unnamed.find("UI Builder"), str::npos);
		EXPECT_NE(unnamed.find("64x48"), str::npos);
		EXPECT_NE(named.find("x"), str::npos);

		PanelManager::ClosePanel(builder);
		UISystem::DestroySurface(orphan);
	}

	TEST_F(UIDebuggerTargetFixture, DebuggerEditsBuilderPreview) {
		WriteProbeDocument();
		Handle<EditorPanel> builder = OpenPanelOfType("UIBuilderPanel", Host());
		ASSERT_TRUE(builder.IsValid());
		ASSERT_TRUE(UIBuilderOps::OpenDocumentInBuilder("project://Assets/M6.slxml"));
		Settle(*builder.Get());
		const UISurfaceId surface = builder.Get()->GetPresentedSurface();
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		UI::Context* preview = UISystem::GetSurfaceContext(surface);
		ASSERT_NE(preview, nullptr);

		UI::ElementHandle host = Host();
		Handle<EditorPanel> debugger = OpenPanelOfType("UIDebuggerPanel", host);
		ASSERT_TRUE(debugger.IsValid());
		UIDebugOps::Target target;
		target.kind = UIDebugOps::Target::Kind::Surface;
		target.id = surface;
		ASSERT_TRUE(UIDebugOps::OpenDebuggerOn(target));
		Settle(*debugger.Get(), 6);

		EXPECT_EQ(TargetOf(debugger.Get()).id, surface);
		EXPECT_TRUE(FindByText(WindowContext(), host, "#m6-root .m6-row").IsValid());

		UI::ElementHandle documentRoot;
		UIDebugOps::Snapshot snapshot;
		UIDebugOps::BuildSnapshot(*preview, snapshot);
		for (const UIDebugOps::Row& row : snapshot.rows) {
			if (preview->GetElement(row.element)->id == StringHash("m6-root")) {
				documentRoot = row.element;
			}
		}
		ASSERT_TRUE(documentRoot.IsValid());
		UIDebugOps::StyleEdits edits;
		EXPECT_TRUE(UIDebugOps::SetProperty(*preview, documentRoot, edits, StringHash("width"), "77px"));
		preview->Update();
		EXPECT_FLOAT_EQ(preview->GetLayout(documentRoot).borderBox.w, 77.0f * preview->GetContentScale());

		PanelManager::ClosePanel(debugger);
		PanelManager::ClosePanel(builder);
	}

	TEST_F(UIDebuggerTargetFixture, BuildersOverflowMenuOpensDebuggerOnPreview) {
		WriteProbeDocument();
		UI::ElementHandle builderHost = Host();
		Handle<EditorPanel> builder = OpenPanelOfType("UIBuilderPanel", builderHost);
		ASSERT_TRUE(builder.IsValid());
		ASSERT_TRUE(UIBuilderOps::OpenDocumentInBuilder("project://Assets/M6.slxml"));
		Settle(*builder.Get());
		const UISurfaceId surface = builder.Get()->GetPresentedSurface();
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		ASSERT_EQ(OpenDebuggerPanel(), nullptr);

		UI::Context& context = WindowContext();
		UI::ElementHandle overflow = OverflowButton(context, builderHost);
		ASSERT_TRUE(overflow.IsValid());
		ClickElement(context, overflow);
		context.Update();
		ClickMenuItem(context, "UI Debugger");

		EditorPanel* debugger = OpenDebuggerPanel();
		ASSERT_NE(debugger, nullptr);
		const UIDebugOps::Target target = TargetOf(debugger);
		EXPECT_EQ(target.kind, UIDebugOps::Target::Kind::Surface);
		EXPECT_EQ(target.id, surface);
	}

	TEST_F(UIDebuggerTargetFixture, EmptyScreenDisablesDebugger) {
		UI::ElementHandle viewHost = Host();
		Handle<EditorPanel> view = OpenPanelOfType("RuntimeViewPanel", viewHost);
		ASSERT_TRUE(view.IsValid());
		Settle(*view.Get(), 2);
		ASSERT_EQ(OpenDebuggerPanel(), nullptr);

		UI::Context& context = WindowContext();
		UI::ElementHandle overflow = OverflowButton(context, viewHost);
		ASSERT_TRUE(overflow.IsValid());
		ClickElement(context, overflow);
		context.Update();

		ClickMenuItem(context, "UI Debugger");
		EXPECT_EQ(OpenDebuggerPanel(), nullptr);
	}

	TEST_F(UIDebuggerTargetFixture, OverflowOpensDebuggerOnScreen) {
		UI::ElementHandle viewHost = Host();
		Handle<EditorPanel> view = OpenPanelOfType("RuntimeViewPanel", viewHost);
		ASSERT_TRUE(view.IsValid());
		Settle(*view.Get(), 2);
		ASSERT_EQ(OpenDebuggerPanel(), nullptr);

		UIScreenSurfaceDesc desc;
		desc.name = "project://Assets/UI/Hud.slxml";
		const UISurfaceId surface = UISystem::CreateScreenSurface(desc);
		ASSERT_NE(surface, INVALID_UI_SURFACE);
		Settle(*view.Get(), 2);

		UI::Context& context = WindowContext();
		UI::ElementHandle overflow = OverflowButton(context, viewHost);
		ASSERT_TRUE(overflow.IsValid());
		ClickElement(context, overflow);
		context.Update();
		ClickMenuItem(context, "UI Debugger: Hud.slxml");

		EditorPanel* debugger = OpenDebuggerPanel();
		ASSERT_NE(debugger, nullptr);
		const UIDebugOps::Target target = TargetOf(debugger);
		EXPECT_EQ(target.kind, UIDebugOps::Target::Kind::Surface);
		EXPECT_EQ(target.id, surface);
		UISystem::DestroySurface(surface);
	}

	TEST_F(UIDebuggerTargetFixture, SubtreeChangesReachPane) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> debugger = OpenPanelOfType("UIDebuggerPanel", host);
		ASSERT_TRUE(debugger.IsValid());
		Settle(*debugger.Get(), 3);
		UI::Context& context = WindowContext();
		const u32 before = CountByClass(context, host, StringHash("slt-tree-row"));

		UI::ElementHandle hud = context.CreateElement();
		context.AppendChild(context.GetRoot(), hud);
		context.SetId(hud, StringHash("m6-hud"));
		UI::ElementHandle bar = context.CreateElement();
		context.AppendChild(hud, bar);
		Settle(*debugger.Get(), 3);
		EXPECT_GT(CountByClass(context, host, StringHash("slt-tree-row")), before);

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, bar, path);
		UI::ElementHandle row = FindRowWithTag(context, host, UIDebugOps::PathKey(path));
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		Settle(*debugger.Get(), 3);
		EXPECT_TRUE(FindByText(context, host, "World Bound").IsValid());

		context.DestroyElement(hud);
		Settle(*debugger.Get(), 3);
		EXPECT_EQ(CountByClass(context, host, StringHash("slt-tree-row")), before);
		EXPECT_TRUE(FindByText(context, host, "The selected element is gone").IsValid());
		PanelManager::ClosePanel(debugger);
	}

	TEST_F(UIDebuggerTargetFixture, SelectorRowLinksToBuilder) {
		WriteProbeDocument();
		UI::ElementHandle builderHost = Host();
		Handle<EditorPanel> builder = OpenPanelOfType("UIBuilderPanel", builderHost);
		ASSERT_TRUE(builder.IsValid());
		ASSERT_TRUE(UIBuilderOps::OpenDocumentInBuilder("project://Assets/M6.slxml"));
		Settle(*builder.Get());
		const UISurfaceId surface = builder.Get()->GetPresentedSurface();
		ASSERT_NE(UISystem::GetSurfaceContext(surface), nullptr);

		UI::ElementHandle host = Host();
		Handle<EditorPanel> debugger = OpenPanelOfType("UIDebuggerPanel", host);
		ASSERT_TRUE(debugger.IsValid());
		UIDebugOps::Target target;
		target.kind = UIDebugOps::Target::Kind::Surface;
		target.id = surface;
		ASSERT_TRUE(UIDebugOps::OpenDebuggerOn(target));
		Settle(*debugger.Get(), 6);

		UI::Context& context = WindowContext();
		UI::ElementHandle row = FindByText(context, host, "#m6-root .m6-row");
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		Settle(*debugger.Get(), 3);

		EXPECT_EQ(CountByClass(context, host, EditorTheme::Class::UIDebugLink), 3u);
		UI::ElementHandle linkRow = FindByClass(context, host, EditorTheme::Class::UIDebugLink, 1);
		ASSERT_TRUE(linkRow.IsValid());
		UI::ElementHandle cell = FindByText(context, linkRow, ".m6-row");
		ASSERT_TRUE(cell.IsValid());
		ClickElement(context, cell);
		Settle(*builder.Get(), 2);

		const u64 wanted = (1ull << 32) | 2ull;
		UI::ElementHandle lit = FindRowWithTag(context, builderHost, wanted, StringHash("slt-selected"));
		EXPECT_TRUE(lit.IsValid());
		EXPECT_FALSE(FindRowWithTag(context, builderHost, (1ull << 32) | 1ull, StringHash("slt-selected")).IsValid());

		PanelManager::ClosePanel(debugger);
		PanelManager::ClosePanel(builder);
	}

	TEST_F(UIDebuggerTargetFixture, FilelessRuleNotLinked) {
		UI::ElementHandle host = Host();
		Handle<EditorPanel> debugger = OpenPanelOfType("UIDebuggerPanel", host);
		ASSERT_TRUE(debugger.IsValid());
		Settle(*debugger.Get(), 4);

		UI::Context& context = WindowContext();
		UI::ElementHandle row = FindByText(context, host, "Element .ed-panel-header");
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		Settle(*debugger.Get(), 3);

		ASSERT_TRUE(FindByText(context, host, ".ed-panel-header").IsValid());
		EXPECT_EQ(CountByClass(context, host, EditorTheme::Class::UIDebugLink), 0u);
		PanelManager::ClosePanel(debugger);
	}

	TEST_F(UIDebuggerTargetFixture, UndeclaredRuleRefused) {
		WriteProbeDocument();
		EXPECT_FALSE(UIBuilderOps::ShowSheetRuleInBuilder("project://Assets/M6.slss", 0, ".m6-row"));

		Handle<EditorPanel> builder = OpenPanelOfType("UIBuilderPanel", Host());
		ASSERT_TRUE(builder.IsValid());
		ASSERT_TRUE(UIBuilderOps::OpenDocumentInBuilder("project://Assets/M6.slxml"));
		Settle(*builder.Get());
		EXPECT_TRUE(UIBuilderOps::ShowSheetRuleInBuilder("project://Assets/M6.slss", 0, ".m6-row"));
		EXPECT_FALSE(UIBuilderOps::ShowSheetRuleInBuilder("project://Assets/M6.slss", 9, ".m6-nope"));
		PanelManager::ClosePanel(builder);
	}

	class UIDebuggerReloadFixture : public ::testing::Test {
	protected:
		Handle<Window> _window;
		ModuleHandle _module;
		bool _haveModulePanel = false;

		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			SceneSystem::Initialize();
			ModuleSystem::Initialize();
			EditorSelection::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			PanelManager::Initialize();

			WindowDesc desc;
			desc.title = "UIDebuggerReloadWindow";
			desc.clientSize = {1200, 800};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			UI::Context* context = UISystem::CreateContext(_window);
			ASSERT_NE(context, nullptr);
			EditorTheme::ApplyToContext(*context);

			_module = ModuleSystem::Load(Path("ShalltearHotReloadModuleV1" SLT_TEST_DLL_SUFFIX));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_module));
			_haveModulePanel = TypeRegistry::FindType(StringHash("HotReloadEditorPanel")) != nullptr;
		}

		void TearDown() override {
			if (ModuleSystem::IsLoaded(_module)) {
				EditorExtensions::Unload(ModuleSystem::GetNativeHandle(_module));
			}
			PanelManager::Terminate();
			UISystem::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			EditorSelection::Terminate();
			ModuleSystem::Terminate();
			SceneSystem::Terminate();
		}

		UI::Context& WindowContext() { return *UISystem::GetContext(_window); }

		bool ReloadThrough(const char* binary) {
			EventBus& bus = EventBus::GetGlobal();
			const void* base = ModuleSystem::GetNativeHandle(_module);
			EditorEvents::ModuleReloadBegan.Publish(bus, {base});
			Path path{binary};
			const bool succeeded = ModuleSystem::Reload(_module, &path);
			EditorEvents::ModuleReloadEnded.Publish(bus, {succeeded, ModuleSystem::GetNativeHandle(_module)});
			return succeeded;
		}

		EditorPanel* FindPanelOfType(StringHash typeName) const {
			Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
			PanelManager::GetOpenPanels(panels);
			for (const Handle<EditorPanel>& panel : panels) {
				EditorPanel* resolved = panel.Get();
				if (resolved && resolved->GetType() && resolved->GetType()->name == typeName) {
					return resolved;
				}
			}
			return nullptr;
		}
	};

	TEST_F(UIDebuggerReloadFixture, SelectionSurvivesReload) {
		if (!_haveModulePanel) {
			GTEST_SKIP() << "module editor panels are shared link only";
		}
		UI::Context& context = WindowContext();

		UI::ElementHandle leaf = context.CreateElement();
		context.AppendChild(context.GetRoot(), leaf);
		EditorPanel* probe = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("HotReloadEditorPanel"))).Get();
		ASSERT_NE(probe, nullptr);
		probe->BuildContent(context, leaf);

		UI::ElementHandle debuggerHost = context.CreateElement();
		context.AppendChild(context.GetRoot(), debuggerHost);
		Handle<EditorPanel> debugger = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("UIDebuggerPanel")));
		ASSERT_TRUE(debugger.IsValid());
		debugger.Get()->BuildContent(context, debuggerHost);
		for (int i = 0; i < 4; ++i) {
			debugger.Get()->OnTick(0.0f);
			UISystem::Update();
		}

		const UI::Element* leafPod = context.GetElement(leaf);
		ASSERT_NE(leafPod, nullptr);
		ASSERT_NE(leafPod->firstChild, UI::INVALID_UI_INDEX);
		UI::ElementHandle line = context.MakeHandle(leafPod->firstChild);
		ASSERT_EQ(context.GetElement(line)->userTag, 1u);

		Vector<u32> path{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::PathTo(context, line, path);
		UI::ElementHandle row = FindRowWithTag(context, debuggerHost, UIDebugOps::PathKey(path));
		ASSERT_TRUE(row.IsValid());
		ClickElement(context, row);
		for (int i = 0; i < 3; ++i) {
			debugger.Get()->OnTick(0.0f);
			UISystem::Update();
		}
		ASSERT_TRUE(FindByText(context, debuggerHost, "World Bound").IsValid());

		ASSERT_TRUE(ReloadThrough("ShalltearHotReloadModuleV2" SLT_TEST_DLL_SUFFIX));
		context.ClearChildren(leaf);
		EditorPanel* rebuilt = FindPanelOfType(StringHash("HotReloadEditorPanel"));
		ASSERT_NE(rebuilt, nullptr);
		rebuilt->BuildContent(context, leaf);
		for (int i = 0; i < 4; ++i) {
			debugger.Get()->OnTick(0.0f);
			UISystem::Update();
		}

		const UI::Element* rebuiltLeaf = context.GetElement(leaf);
		ASSERT_NE(rebuiltLeaf, nullptr);
		ASSERT_NE(rebuiltLeaf->firstChild, UI::INVALID_UI_INDEX);
		UI::ElementHandle after = context.MakeHandle(rebuiltLeaf->firstChild);
		EXPECT_NE(after, line);
		EXPECT_EQ(context.GetElement(after)->userTag, 2u);
		EXPECT_EQ(UIDebugOps::Resolve(context, path), after);

		EXPECT_TRUE(FindRowWithTag(context, debuggerHost, UIDebugOps::PathKey(path)).IsValid());
		EXPECT_TRUE(FindByText(context, debuggerHost, "World Bound").IsValid());
		EXPECT_FALSE(FindByText(context, debuggerHost, "The selected element is gone").IsValid());

		PanelManager::ClosePanel(debugger);
	}

	void Frame(UI::Context& context) {
		context.Update();
		UI::RenderList list;
		context.BuildRenderList(list);
	}

	TEST_F(UIDebuggerTest, ShapedRunAtPainterOrigin) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle label = Child(context, context.GetRoot());
		UI::Style style;
		style.width = UI::Length::Pixel(200.0f);
		style.height = UI::Length::Pixel(100.0f);
		context.SetStyle(label, style);
		context.SetText(label, {}, 16.0f, Color::White(), "abc");
		Frame(context);

		const UI::Rect content = context.GetLayout(label).content;
		{
			UI::Context::ShapedTextView view = context.GetShapedText(label);
			ASSERT_NE(view.run, nullptr);
			EXPECT_EQ(view.run->glyphs.size(), 3u);
			EXPECT_FLOAT_EQ(view.originPx.x, content.x);
			EXPECT_FLOAT_EQ(view.originPx.y, content.y);
		}

		style.textAlign = UI::Align::Center;
		style.textAlignVertical = UI::Align::Center;
		context.SetStyle(label, style);
		Frame(context);
		{
			UI::Context::ShapedTextView view = context.GetShapedText(label);
			ASSERT_NE(view.run, nullptr);
			const f32 freeX = content.w - view.run->extent.x;
			const f32 freeY = content.h - view.run->extent.y;
			EXPECT_GT(freeX, 0.0f);
			EXPECT_FLOAT_EQ(view.originPx.x, std::round(content.x + freeX * 0.5f));
			EXPECT_FLOAT_EQ(view.originPx.y, std::round(content.y + freeY * 0.5f));
		}

		style.textAlign = UI::Align::End;
		context.SetStyle(label, style);
		Frame(context);
		{
			UI::Context::ShapedTextView view = context.GetShapedText(label);
			ASSERT_NE(view.run, nullptr);
			EXPECT_FLOAT_EQ(view.originPx.x, std::round(content.x + content.w - view.run->extent.x));
		}
	}

	TEST_F(UIDebuggerTest, ShapedTextSeamAnswersNothingNotShaping) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		EXPECT_EQ(context.GetShapedText(UI::ElementHandle{}).run, nullptr);

		UI::ElementHandle plain = Child(context, context.GetRoot());
		context.Update();
		EXPECT_EQ(context.GetShapedText(plain).run, nullptr);

		UI::ElementHandle label = Child(context, context.GetRoot());
		context.SetText(label, {}, 16.0f, Color::White(), "abc");
		EXPECT_EQ(context.GetShapedText(label).run, nullptr);
	}

	TEST_F(UIDebuggerTest, CharacterBoxesLandOnGlyphsRunDraws) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle label = Child(context, context.GetRoot());
		context.SetText(label, {}, 16.0f, Color::White(), "hi there");
		Frame(context);

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		const UIDebugOps::TextOverlayStats stats =
			UIDebugOps::BuildTextBoxes(context, label, UIDebugOps::TextBoxKinds::Characters, boxes);
		UI::Context::ShapedTextView view = context.GetShapedText(label);
		ASSERT_NE(view.run, nullptr);
		ASSERT_FALSE(view.run->glyphs.empty());
		EXPECT_EQ(stats.runs, 1u);
		ASSERT_EQ(boxes.size(), view.run->glyphs.size());
		for (size_t i = 0; i < boxes.size(); ++i) {
			EXPECT_EQ(boxes[i].kind, UIDebugOps::TextBoxKinds::Characters);
			EXPECT_FLOAT_EQ(boxes[i].box.x, view.run->glyphs[i].rect.x + view.originPx.x);
			EXPECT_FLOAT_EQ(boxes[i].box.y, view.run->glyphs[i].rect.y + view.originPx.y);
			EXPECT_FLOAT_EQ(boxes[i].box.w, view.run->glyphs[i].rect.w);
			EXPECT_FLOAT_EQ(boxes[i].box.h, view.run->glyphs[i].rect.h);
		}
	}

	TEST_F(UIDebuggerTest, WordSpansFollowCaret) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle label = Child(context, context.GetRoot());
		UI::Style style;
		style.width = UI::Length::Pixel(300.0f);
		context.SetStyle(label, style);
		context.SetText(label, {}, 16.0f, Color::White(), "ab cde");
		Frame(context);

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::TextOverlayStats stats =
			UIDebugOps::BuildTextBoxes(context, label, UIDebugOps::TextBoxKinds::Words, boxes);
		UI::Context::ShapedTextView view = context.GetShapedText(label);
		ASSERT_NE(view.run, nullptr);
		EXPECT_EQ(stats.wordlessRuns, 0u);
		ASSERT_EQ(boxes.size(), 2u);
		EXPECT_FLOAT_EQ(boxes[0].box.x, view.run->CaretXForByte(0) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[0].box.x + boxes[0].box.w, view.run->CaretXForByte(2) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[1].box.x, view.run->CaretXForByte(3) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[1].box.x + boxes[1].box.w, view.run->CaretXForByte(6) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[0].box.h, view.run->lineHeight);

		style.width = UI::Length::Pixel(30.0f);
		context.SetStyle(label, style);
		Frame(context);
		boxes.clear();
		stats = UIDebugOps::BuildTextBoxes(context, label, UIDebugOps::TextBoxKinds::Words, boxes);
		view = context.GetShapedText(label);
		ASSERT_NE(view.run, nullptr);
		ASSERT_TRUE(view.run->multiline);
		EXPECT_EQ(stats.wordlessRuns, 0u);
		ASSERT_EQ(boxes.size(), 2u);
		EXPECT_FLOAT_EQ(boxes[0].box.y, view.originPx.y);
		EXPECT_FLOAT_EQ(boxes[1].box.y, view.originPx.y + view.run->lineHeight);
		EXPECT_FLOAT_EQ(boxes[1].box.x, view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[1].box.w, 30.0f);
	}

	TEST_F(UIDebuggerTest, RichWordSpansFollowShownText) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle label = Child(context, context.GetRoot());
		context.SetText(label, {}, 16.0f, Color::White(), "<b>ab</b> cde");
		context.SetRichText(label, true);
		Frame(context);

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildTextBoxes(context, label, UIDebugOps::TextBoxKinds::Words, boxes);
		const UI::Context::ShapedTextView view = context.GetShapedText(label);
		ASSERT_NE(view.run, nullptr);
		EXPECT_EQ(view.text, "ab cde");
		ASSERT_EQ(boxes.size(), 2u);
		EXPECT_FLOAT_EQ(boxes[0].box.x + boxes[0].box.w, view.run->CaretXForByte(2) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[1].box.x, view.run->CaretXForByte(3) + view.originPx.x);
		EXPECT_FLOAT_EQ(boxes[1].box.x + boxes[1].box.w, view.run->CaretXForByte(6) + view.originPx.x);

		context.SetRichText(label, false);
		Frame(context);
		EXPECT_EQ(context.GetShapedText(label).text, "<b>ab</b> cde");
	}

	TEST_F(UIDebuggerTest, LineBandsStackByHeight) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle label = Child(context, context.GetRoot());
		context.SetText(label, {}, 16.0f, Color::White(), "abcd\ne");
		Frame(context);

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildTextBoxes(context, label, UIDebugOps::TextBoxKinds::Lines, boxes);
		UI::Context::ShapedTextView view = context.GetShapedText(label);
		ASSERT_NE(view.run, nullptr);
		ASSERT_EQ(boxes.size(), 2u);
		EXPECT_FLOAT_EQ(boxes[1].box.y - boxes[0].box.y, view.run->lineHeight);
		EXPECT_FLOAT_EQ(boxes[0].box.h, view.run->lineHeight);
		EXPECT_GT(boxes[0].box.w, boxes[1].box.w);
	}

	TEST_F(UIDebuggerTest, OverlayWalkSkipsMarked) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);

		UI::ElementHandle plain = Child(context, context.GetRoot());
		context.SetText(plain, {}, 16.0f, Color::White(), "ab");
		UI::ElementHandle marked = Child(context, context.GetRoot());
		context.AddClass(marked, StringHash(UIDebugOps::MarkerClass));
		UI::ElementHandle inside = Child(context, marked);
		context.SetText(inside, {}, 16.0f, Color::White(), "cdef");
		Frame(context);

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		const UIDebugOps::TextOverlayStats stats =
			UIDebugOps::CollectTextBoxes(context, UIDebugOps::TextBoxKinds::Characters, boxes);
		EXPECT_EQ(stats.runs, 1u);
		EXPECT_EQ(boxes.size(), 2u);
	}

	TEST_F(UIDebuggerTest, NoKindsSelectedCollectsNothingAtAll) {
		MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {400, 300};
		UI::Context context(desc, &fonts);
		UI::ElementHandle label = Child(context, context.GetRoot());
		context.SetText(label, {}, 16.0f, Color::White(), "abc");
		context.Update();

		Vector<UIDebugOps::TextBox> boxes{GetMemoryResource(MemTag::Editor)};
		const UIDebugOps::TextOverlayStats stats =
			UIDebugOps::CollectTextBoxes(context, UIDebugOps::TextBoxKinds::None, boxes);
		EXPECT_EQ(stats.runs, 0u);
		EXPECT_TRUE(boxes.empty());
	}

	TEST_F(UIDebuggerPanelFixture, TextOverlayToggles) {
		UI::Context& context = WindowContext();
		UI::ElementHandle label = context.CreateElement();
		context.AppendChild(context.GetRoot(), label);
		context.SetText(label, UISystem::GetDefaultFont(), 16.0f, Color::White(), "overlay me");

		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());
		ASSERT_EQ(OverlayCanvases(context), 0u);
		UI::RenderList before;
		context.BuildRenderList(before);

		ToggleOverlayKind(*panel.Get(), host, "Characters");
		EXPECT_TRUE(context.HasClass(TextOverlayButton(context, host), EditorTheme::Class::ToolButtonOn));
		ASSERT_EQ(OverlayCanvases(context), 1u);

		UI::RenderList after;
		context.BuildRenderList(after);
		Frame(*panel.Get());
		const str status = StatusText(context, host);
		ASSERT_FALSE(status.empty());
		EXPECT_NE(status.find(" runs, "), str::npos) << status;
		EXPECT_EQ(status.find("0 runs"), str::npos) << status;
		EXPECT_GE(after.quads.size(), before.quads.size());

		ToggleOverlayKind(*panel.Get(), host, "Characters");
		EXPECT_EQ(OverlayCanvases(context), 0u);

		ToggleOverlayKind(*panel.Get(), host, "Lines");
		ASSERT_EQ(OverlayCanvases(context), 1u);
		PanelManager::ClosePanel(panel);
		EXPECT_EQ(OverlayCanvases(context), 0u);
	}

	TEST_F(UIDebuggerTest, AtlasListNamesOnlyPagesThatExist) {
		Vector<UIDebugOps::AtlasPageRow> pages{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildAtlasPages(pages);
		EXPECT_TRUE(pages.empty());
	}

	TEST_F(UIDebuggerPanelFixture, AtlasButtonSwapsInspectorColumnAndBack) {
		if (!RenderSystem::IsRenderingAvailable()) {
			GTEST_SKIP() << "no Vulkan device available";
		}
		UI::Context& context = WindowContext();
		UI::ElementHandle host = Host();
		Handle<EditorPanel> panel = OpenDebugger(host);
		ASSERT_TRUE(panel.IsValid());
		Frame(*panel.Get());
		ASSERT_TRUE(FindByText(context, host, "Select an element").IsValid());

		UI::ElementHandle button = AtlasButton(context, host);
		ASSERT_TRUE(button.IsValid());
		ClickElement(context, button);
		for (int i = 0; i < 2; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_TRUE(context.HasClass(AtlasButton(context, host), EditorTheme::Class::ToolButtonOn));
		EXPECT_FALSE(FindByText(context, host, "Select an element").IsValid());
		Vector<UIDebugOps::AtlasPageRow> pages{GetMemoryResource(MemTag::Editor)};
		UIDebugOps::BuildAtlasPages(pages);
		ASSERT_FALSE(pages.empty());
		EXPECT_TRUE(FindByText(context, host, pages.front().name).IsValid());
		EXPECT_TRUE(FindByText(context, host, "Size").IsValid());
		EXPECT_TRUE(FindByText(context, host, "R8 coverage").IsValid());
		EXPECT_TRUE(FindByText(context, host, "Resident").IsValid());
		EXPECT_TRUE(FindByText(context, host, "Full").IsValid());

		ClickElement(context, AtlasButton(context, host));
		for (int i = 0; i < 2; ++i) {
			Frame(*panel.Get());
		}
		EXPECT_TRUE(FindByText(context, host, "Select an element").IsValid());
		PanelManager::ClosePanel(panel);
	}

} // anonymous namespace
