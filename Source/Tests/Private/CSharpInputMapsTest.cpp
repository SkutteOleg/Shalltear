#include "TestsPCH.h"

#ifndef SHALLTEAR_SHARED

#include "InputMaps.h"

#include <stdexcept>
#include <string>

using namespace Shalltear;
using namespace Shalltear::CSharpImpl;

namespace {

	std::string Refusal(void (*call)()) {
		try {
			call();
		} catch (const std::runtime_error& error) {
			return error.what();
		}
		return "allowed";
	}

	u32 _stale = 0;

	class CSharpInputMapsTest : public ::testing::Test {
	protected:
		void TearDown() override { InputMaps::DestroyAll(); }
	};

} // anonymous namespace

TEST_F(CSharpInputMapsTest, DestroyedMapIdStaysDead) {
	const u32 first = InputMaps::Create();
	const u32 second = InputMaps::Create();
	ASSERT_NE(first, 0u);
	ASSERT_NE(first, second);
	const u32 fire = InputMaps::RegisterAction(first, "Fire", ActionType::Button);
	EXPECT_EQ(InputMaps::GetActionCount(first), 1);
	EXPECT_EQ(InputMaps::FindAction(first, "Fire"), fire);
	EXPECT_EQ(InputMaps::GetActionCount(second), 0);

	InputMaps::Destroy(first);
	InputMaps::Destroy(first);
	EXPECT_EQ(InputMaps::GetActionCount(first), 0);
	EXPECT_FALSE(InputMaps::IsPressed(first, fire));
	_stale = first;
	EXPECT_EQ(Refusal(+[] { InputMaps::Evaluate(_stale); }), "InputActionMap.Evaluate used after Dispose");

	const u32 reused = InputMaps::Create();
	EXPECT_NE(reused, first);
	EXPECT_EQ(reused & 0xFFFFu, first & 0xFFFFu);
	EXPECT_EQ(InputMaps::RegisterAction(reused, "Jump", ActionType::Button), 0u);
	EXPECT_EQ(InputMaps::GetActionCount(first), 0);
	EXPECT_EQ(Refusal(+[] { InputMaps::RegisterAction(_stale, "Fire", ActionType::Button); }), "InputActionMap.RegisterAction used after Dispose");
}

TEST_F(CSharpInputMapsTest, DestroyAllEndsEveryMap) {
	const u32 first = InputMaps::Create();
	const u32 second = InputMaps::Create();
	InputMaps::RegisterAction(first, "Fire", ActionType::Button);
	InputMaps::RegisterAction(second, "Move", ActionType::Axis2D);

	InputMaps::DestroyAll();
	EXPECT_EQ(InputMaps::GetActionCount(first), 0);
	EXPECT_EQ(InputMaps::GetActionCount(second), 0);
	InputMaps::Destroy(second);
	const u32 fresh = InputMaps::Create();
	EXPECT_NE(fresh, 0u);
	EXPECT_EQ(InputMaps::GetActionCount(fresh), 0);
}

TEST_F(CSharpInputMapsTest, MistakesThrowBeforeCoreAsserts) {
	static u32 map = 0;
	map = InputMaps::Create();
	InputMaps::RegisterAction(map, "Fire", ActionType::Button);
	EXPECT_EQ(Refusal(+[] { InputMaps::RegisterAction(map, "Fire", ActionType::Axis1D); }), "InputActionMap.RegisterAction got 'Fire' again with another type");
	EXPECT_EQ(Refusal(+[] { InputMaps::RegisterAction(map, "Odd", static_cast<ActionType>(9)); }), "InputActionMap.RegisterAction got an unknown ActionType");
	EXPECT_EQ(Refusal(+[] { InputMaps::Bind(map, 7, 0, 0, KeyMod::None, false, 1.0f, 0, 0); }),
			  "InputActionMap.Bind got an action of another map. Use RegisterAction's result");
	EXPECT_EQ(Refusal(+[] { InputMaps::Bind(map, 0, 9, 0, KeyMod::None, false, 1.0f, 0, 0); }), "InputActionMap.Bind got an unknown InputBinding.Source");
	EXPECT_EQ(Refusal(+[] { InputMaps::ClearBindings(map, 7); }), "InputActionMap.ClearBindings got an action of another map. Use RegisterAction's result");
	EXPECT_EQ(Refusal(+[] { InputMaps::Bind(map, 0, 0, static_cast<u16>(Key::F), KeyMod::None, false, 1.0f, 0, 0); }), "allowed");
}

#endif
