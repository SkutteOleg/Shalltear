#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Types.h"

#include <functional>
#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::SlotProbe {

	enum class Tone : i8 {
		Low = -2,
		Mid = 0,
		High = 3,
	};

	struct Shape {
		f32 width = 0.0f;
		i32 count = 0;
		Vec3 at;
		u8 flags = 0;
		Tone tone = Tone::Mid;
	};

	struct Wide {
		Vec4 corner;
		f64 weight = 0.0;
		bool on = false;
	};

	struct Recipe {
		Recipe();
		Recipe(const Recipe& other);
		~Recipe();

		str name;
		Vector<i32> steps;
		f32 heat = 0.0f;
	};

	class Counter {
	public:
		i32 Add(i32 amount);
		i32 GetTotal() const;
	private:
		i32 _total = 0;
	};

	i8 EchoI8(i8 value);
	i16 EchoI16(i16 value);
	u8 EchoU8(u8 value);
	u16 EchoU16(u16 value);
	i64 EchoI64(i64 value);
	f64 Blend(bool flag, i32 count, f32 scale, f64 weight, const Vec2& offset, const Vec3& position, const Vec4& plane, const Quat& turn, const Color& tint,
			  const Vec2I& cell);
	Vec2I Swap(const Vec2I& cell);
	Quat Flip(const Quat& turn);
	Tone NextTone(Tone tone);
	str Greet(std::string_view name, std::string_view ending);
	void Throw();

	Shape Grow(const Shape& shape, f32 by);
	Wide Halve(const Wide& wide);
	void Describe(const Shape& shape, i32& outCount, Vec3& outAt, str& outText, Tone& outTone, bool& outWide, f64& outArea);
	bool Split(const Wide& wide, Wide& outHalf, str& outFirst, str& outSecond);
	SceneObject* FindParent(SceneObject& child, SceneObject*& outRoot);

	i32 Sum(std::span<const i32> values);
	void Fill(std::span<f32> values, f32 value);
	Vector<i32> Count(i32 count);
	Vector<Shape> Shapes(i32 count);
	f32 Widths(std::span<const Shape> shapes);
	str Join(std::span<const std::string_view> parts, std::string_view separator);
	Vector<str> Words(std::string_view text);
	Vector<Handle<SceneObject>> Children(SceneObject& parent);
	i32 CountNamed(std::span<SceneObject* const> objects, std::string_view name);
	Vector<Tone> Tones();
	Vector<f64> Halves(std::span<const f64> values);

	Counter& GetCounter();
	Vector<Counter*> Counters();
	i32 TotalOf(std::span<Counter* const> counters);
	Recipe MakeRecipe(std::string_view name, i32 steps);
	str DescribeRecipe(const Recipe& recipe);
	i32 CountLiveRecipes();

	inline constexpr i32 PROBE_LIMIT = 42;
	inline constexpr u64 PROBE_MASK = 0xFFFFFFFFFFFFFFFFull;
	inline constexpr i64 PROBE_FLOOR = -9223372036854775807ll - 1;
	inline constexpr f32 PROBE_SCALE = 0.1f;
	inline constexpr f64 PROBE_RATIO = 1.0 / 3.0;
	inline constexpr bool PROBE_ON = true;
	inline constexpr std::string_view ProbeName = "probe";

	struct Kitchen {
		Recipe main;
		Vector<Recipe> shelf;
	};

	void Primes(i32 count, Vector<i32>& outPrimes);
	bool Menu(i32 count, Vector<Recipe>& outRecipes, Vector<str>& outNames);
	Vector<Recipe> Recipes(i32 count);
	i32 CountSteps(const Vector<Recipe>& recipes);
	void Stock(Kitchen& kitchen, i32 count);

	i32 Apply(i32 value, const std::function<i32(i32)>& step);
	str Label(const std::function<str(std::string_view, f32)>& make);
	i32 Visit(SceneObject& root, const std::function<bool(SceneObject*, const Shape&, Tone)>& visitor);
	void KeepAction(std::function<void(const Recipe&, Vec3)> action);
	bool RunKept(const Recipe& recipe, const Vec3& at);
	void DropKept();

} // namespace Shalltear::CSharpImpl::SlotProbe
