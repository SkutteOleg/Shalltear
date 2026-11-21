#include "TestsPCH.h"

#include "DeviceLossHarness.h"

#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Scene.h"

#include <stdexcept>

using namespace Shalltear;

namespace {

	f32 SftScale(f32 value, i32 factor) {
		return value * static_cast<f32>(factor);
	}
	SLT_SCRIPT_EXPORT(SftScale)

	str SftGreet(std::string_view name, const str& ending) {
		return "hello " + str(name) + ending;
	}
	SLT_SCRIPT_EXPORT(SftGreet)

	f64 SftMix(Vec2 two, Vec3 three, Vec4 four, Quat turn, Color tint, bool flag, u8 small, i16 negative, i64 big, f64 wide) noexcept {
		return static_cast<f64>(two.x + two.y + three.x + three.y + three.z + four.x + four.y + four.z + four.w + turn.x + turn.y + turn.z + turn.w + tint.r +
								tint.g + tint.b + tint.a) +
			   (flag ? 1000.0 : 0.0) + static_cast<f64>(small) + static_cast<f64>(negative) + static_cast<f64>(big) + wide;
	}
	SLT_SCRIPT_EXPORT(SftMix)

	Quat SftTurn(Quat turn) {
		return Quat(turn.w, turn.z, turn.y, turn.x);
	}
	SLT_SCRIPT_EXPORT(SftTurn)

	Color SftTint(Vec4 value) {
		return Color(value.x, value.y, value.z, value.w);
	}
	SLT_SCRIPT_EXPORT(SftTint)

	u64 SftNameLength(Handle<SceneObject> object) {
		return object.IsValid() ? object->name.size() : 999;
	}
	SLT_SCRIPT_EXPORT(SftNameLength)

	void SftThrow() {
		throw std::runtime_error("thrown by an export");
	}
	SLT_SCRIPT_EXPORT(SftThrow)

	i32 _calls = 0;

	void SftCount() {
		++_calls;
	}
	SLT_SCRIPT_EXPORT(SftCount)

	void Doubler(u64 context, const u64* arguments, u64* result) {
		result[0] = arguments[0] * 2 + context;
	}

	void Tripler(u64 context, const u64* arguments, u64* result) {
		result[0] = arguments[0] * 3 + context;
	}

} // anonymous namespace

TEST(ScriptFunctionsTest, ExportsAreCalledThroughSlots) {
	TypeRegistry::RegisterPendingTypes();
	ScriptFunction<f32(f32, i32)> scale = ScriptFunctions::Find<f32(f32, i32)>("SftScale");
	ASSERT_TRUE(scale.IsBound());
	EXPECT_EQ(scale(1.5f, -4), -6.0f);

	ScriptFunction<str(std::string_view, const str&)> greet("SftGreet");
	ASSERT_TRUE(greet.IsBound());
	EXPECT_EQ(greet("caf\xC3\xA9", "!"), "hello caf\xC3\xA9!");
	EXPECT_EQ(greet("", ""), "hello ");

	ScriptFunction<f64(Vec2, Vec3, Vec4, Quat, Color, bool, u8, i16, i64, f64)> mix("SftMix");
	ASSERT_TRUE(mix.IsBound());
	EXPECT_DOUBLE_EQ(mix(Vec2(1, 2), Vec3(3, 4, 5), Vec4(6, 7, 8, 9), Quat(10, 11, 12, 13), Color(14, 15, 16, 17), true, 200, -300, -5'000'000'000ll, 0.25),
					 153.0 + 1000.0 + 200.0 - 300.0 - 5'000'000'000.0 + 0.25);

	ScriptFunction<Quat(Quat)> turn("SftTurn");
	const Quat turned = turn(Quat(1, 2, 3, 4));
	EXPECT_EQ(turned.x, 4.0f);
	EXPECT_EQ(turned.y, 3.0f);
	EXPECT_EQ(turned.z, 2.0f);
	EXPECT_EQ(turned.w, 1.0f);
	ScriptFunction<Color(Vec4)> tint("SftTint");
	const Color tinted = tint(Vec4(0.1f, 0.2f, 0.3f, 0.4f));
	EXPECT_EQ(tinted.b, 0.3f);
	EXPECT_EQ(tinted.a, 0.4f);

	ScriptFunction<void()> count("SftCount");
	_calls = 0;
	count();
	count();
	EXPECT_EQ(_calls, 2);
}

TEST(ScriptFunctionsTest, SignatureMismatchLeavesUnbound) {
	TypeRegistry::RegisterPendingTypes();
	ScriptFunction<f32(f32, f32)> wrong("SftScale");
	EXPECT_FALSE(wrong.IsBound());
	EXPECT_EQ(wrong(2.0f, 3.0f), 0.0f);
	str registered;
	EXPECT_NE(ScriptFunctions::FindRaw("SftScale", &registered).thunk, nullptr);
	EXPECT_EQ(registered, "f32(f32,i32)");
	EXPECT_EQ(ScriptFunctions::FindRaw("SftGreet", &registered).context, 0u);
	EXPECT_EQ(registered, "string(string,string)");
	ScriptFunctions::FindRaw("SftMix", &registered);
	EXPECT_EQ(registered, "f64(Vec2,Vec3,Vec4,Quat,Color,bool,u8,i16,i64,f64)");
	ScriptFunctions::FindRaw("SftNameLength", &registered);
	EXPECT_EQ(registered, "u64(object)");

	registered = "kept";
	EXPECT_EQ(ScriptFunctions::FindRaw("SftNowhere", &registered).thunk, nullptr);
	EXPECT_TRUE(registered.empty());
	ScriptFunction<void()> missing("SftNowhere");
	EXPECT_FALSE(missing.IsBound());
	missing();
}

TEST(ScriptFunctionsTest, OwnerSweepUnbindsAndMovesEpoch) {
	char first = 0;
	char second = 0;
	ASSERT_TRUE(ScriptFunctions::Register("SftTokened", "u64(u64)", &Doubler, 5, &first));
	ASSERT_TRUE(ScriptFunctions::Register("SftOther", "u64(u64)", &Tripler, 1, &second));
	EXPECT_TRUE(ScriptFunctions::Register("SftTokened", "u64(u64)", &Doubler, 5, &first));
	EXPECT_FALSE(ScriptFunctions::Register("SftTokened", "u64(u64)", &Tripler, 5, &first));
	EXPECT_FALSE(ScriptFunctions::Register("SftTokened", "u64(u64)", &Doubler, 6, &first));
	EXPECT_FALSE(ScriptFunctions::Register("SftTokened", "u32(u64)", &Doubler, 5, &first));
	EXPECT_FALSE(ScriptFunctions::Register("", "void()", &Doubler));
	EXPECT_FALSE(ScriptFunctions::Register("SftNull", "void()", nullptr));

	ScriptFunction<u64(u64)> doubled("SftTokened");
	ScriptFunction<u64(u64)> tripled("SftOther");
	EXPECT_EQ(doubled(10), 25u);
	EXPECT_EQ(tripled(10), 31u);

	const u32 epoch = ScriptFunctions::GetEpoch();
	TypeRegistry::UnregisterOwnedBy(&first);
	EXPECT_NE(ScriptFunctions::GetEpoch(), epoch);
	EXPECT_FALSE(doubled.IsBound());
	EXPECT_EQ(doubled(10), 0u);
	EXPECT_EQ(tripled(10), 31u);

	ASSERT_TRUE(ScriptFunctions::Register("SftTokened", "u64(u64)", &Tripler, 0, &first));
	EXPECT_EQ(doubled(10), 30u);
	TypeRegistry::UnregisterOwnedBy(&first);
	TypeRegistry::UnregisterOwnedBy(&second);
	EXPECT_FALSE(tripled.IsBound());

	TypeRegistry::RegisterPendingTypes();
	EXPECT_TRUE(ScriptFunction<void()>("SftCount").IsBound());
}

TEST(ScriptFunctionsTest, ThrowIsLoggedAndReturnsDefault) {
	TypeRegistry::RegisterPendingTypes();
	DeviceLossTest::ScopedLog log;
	ScriptFunction<void()> thrower("SftThrow");
	ASSERT_TRUE(thrower.IsBound());
	thrower();
	EXPECT_EQ(log.Delivered().Count("exported function threw: thrown by an export"), 1u) << log.Delivered().Joined();
}

TEST(ScriptFunctionsTest, HandleOfAnotherTypeReadsNull) {
	TypeRegistry::RegisterPendingTypes();
	SceneSystem::Initialize();
	Handle<Scene> scene = SceneSystem::CreateScene("World");
	Handle<SceneObject> object = SceneSystem::CreateObject("Seven77", scene);
	ScriptFunction<u64(Handle<SceneObject>)> length("SftNameLength");
	ASSERT_TRUE(length.IsBound());
	EXPECT_EQ(length(object), 7u);
	EXPECT_EQ(length(Handle<SceneObject>()), 999u);

	const ScriptFunctions::Found raw = ScriptFunctions::FindRaw("SftNameLength");
	u64 argument = scene.GetInstanceId();
	u64 result[2] = {};
	raw.thunk(raw.context, &argument, result);
	EXPECT_EQ(result[0], 999u);
	SceneSystem::Terminate();
}
