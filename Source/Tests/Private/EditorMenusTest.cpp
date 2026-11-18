#include "TestsPCH.h"

#include "UITestUtil.h"

#include "Shalltear/Core/UI/Context.h"

#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorMenus.h"

using namespace Shalltear;

namespace {

	class EditorMenusTest : public ::testing::Test {
	protected:
		char _first = 0;
		char _second = 0;
		UITest::MockFontSystem _fonts;
		UI::Context _context{UI::ContextDesc{}, &_fonts};
		u32 _selected = 0;

		void TearDown() override { EditorMenus::Terminate(); }

		EditorMenus::MenuItemDesc Item(const char* label) {
			EditorMenus::MenuItemDesc item;
			item.label = label;
			item.onSelect = [this](UI::Context& context) { _selected += &context == &_context ? 1u : 100u; };
			return item;
		}

		Vector<UI::Widgets::MenuItem> Shown(std::string_view menu) {
			Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
			EditorMenus::AppendContributed(menu, _context, items);
			return items;
		}
	};

} // anonymous namespace

TEST_F(EditorMenusTest, ContributionSweptWithOwner) {
	EXPECT_TRUE(EditorMenus::Register("Scripts", Item("Compile Scripts"), &_first));
	EXPECT_TRUE(EditorMenus::Register("Scripts", Item("Open Script Project"), &_second));
	EXPECT_TRUE(EditorMenus::Register("Window", Item("Script Log"), &_first));
	ASSERT_EQ(Shown("Scripts").size(), 2u);
	EXPECT_EQ(Shown("Scripts")[0].label, "Compile Scripts");
	EXPECT_EQ(Shown("Scripts")[1].label, "Open Script Project");
	ASSERT_EQ(Shown("Window").size(), 1u);
	EXPECT_TRUE(Shown("File").empty());

	EXPECT_EQ(EditorExtensions::Unload(&_first), 2u);
	ASSERT_EQ(Shown("Scripts").size(), 1u);
	EXPECT_EQ(Shown("Scripts")[0].label, "Open Script Project");
	EXPECT_TRUE(Shown("Window").empty());
	EXPECT_EQ(EditorMenus::UnregisterOwnedBy(&_first), 0u);
	EXPECT_EQ(EditorMenus::UnregisterOwnedBy(nullptr), 0u);
	EXPECT_EQ(EditorMenus::UnregisterOwnedBy(&_second), 1u);
	EXPECT_TRUE(Shown("Scripts").empty());
}

TEST_F(EditorMenusTest, ItemCarriesStateAndSelectsWithContext) {
	bool enabled = false;
	EditorMenus::MenuItemDesc item = Item("New Script...");
	item.shortcut = "F9";
	item.separatorBefore = true;
	item.isEnabled = [&enabled]() { return enabled; };
	ASSERT_TRUE(EditorMenus::Register("Scripts", std::move(item), &_first));
	ASSERT_TRUE(EditorMenus::Register("Scripts", Item("Plain"), &_first));

	Vector<UI::Widgets::MenuItem> shown = Shown("Scripts");
	ASSERT_EQ(shown.size(), 2u);
	EXPECT_EQ(shown[0].shortcut, "F9");
	EXPECT_TRUE(shown[0].separatorBefore);
	EXPECT_FALSE(shown[0].enabled);
	EXPECT_TRUE(shown[1].enabled);
	EXPECT_FALSE(shown[1].separatorBefore);
	enabled = true;
	shown = Shown("Scripts");
	EXPECT_TRUE(shown[0].enabled);

	shown[0].onSelect();
	EXPECT_EQ(_selected, 1u);
}

TEST_F(EditorMenusTest, IncompleteItemsAreRefused) {
	EditorMenus::MenuItemDesc silent;
	silent.label = "Does Nothing";
	EXPECT_FALSE(EditorMenus::Register("Scripts", std::move(silent), &_first));
	EXPECT_FALSE(EditorMenus::Register("Scripts", Item(""), &_first));
	EXPECT_FALSE(EditorMenus::Register("", Item("Nowhere"), &_first));
	EXPECT_FALSE(EditorMenus::Register("Scripts", Item("Unowned"), nullptr));
	EXPECT_TRUE(Shown("Scripts").empty());
	EXPECT_TRUE(Shown("").empty());
}
