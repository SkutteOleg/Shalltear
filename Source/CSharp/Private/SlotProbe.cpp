#include "SlotProbe.h"

#ifdef SLT_CSHARP_TEST_HOOKS

#include "Shalltear/Core/Memory.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::SlotProbe {

	namespace {

		i32 _liveRecipes = 0;
		Counter _counters[3];
		std::function<void(const Recipe&, Vec3)> _keptAction;

	} // anonymous namespace

	Recipe::Recipe() {
		++_liveRecipes;
	}

	Recipe::Recipe(const Recipe& other)
		: name(other.name), steps(other.steps), heat(other.heat) {
		++_liveRecipes;
	}

	Recipe::~Recipe() {
		--_liveRecipes;
	}

	i32 Counter::Add(i32 amount) {
		_total += amount;
		return _total;
	}

	i32 Counter::GetTotal() const {
		return _total;
	}

	i8 EchoI8(i8 value) {
		return value;
	}

	i16 EchoI16(i16 value) {
		return value;
	}

	u8 EchoU8(u8 value) {
		return value;
	}

	u16 EchoU16(u16 value) {
		return value;
	}

	i64 EchoI64(i64 value) {
		return value;
	}

	f64 Blend(bool flag, i32 count, f32 scale, f64 weight, const Vec2& offset, const Vec3& position, const Vec4& plane, const Quat& turn, const Color& tint,
			  const Vec2I& cell) {
		return (flag ? 1.0 : 0.0) + count + scale + weight + offset.x + offset.y + position.x + position.y + position.z + plane.x + plane.y + plane.z + plane.w +
			   turn.x + turn.y + turn.z + turn.w + tint.r + tint.g + tint.b + tint.a + cell.x + cell.y;
	}

	Vec2I Swap(const Vec2I& cell) {
		return Vec2I(cell.y, cell.x);
	}

	Quat Flip(const Quat& turn) {
		return Quat(turn.w, turn.z, turn.y, turn.x);
	}

	Tone NextTone(Tone tone) {
		return tone == Tone::Low ? Tone::Mid : tone == Tone::Mid ? Tone::High
																 : Tone::Low;
	}

	str Greet(std::string_view name, std::string_view ending) {
		return "hello " + str(name) + str(ending);
	}

	void Throw() {
		throw std::runtime_error("thrown by a slot binding");
	}

	Shape Grow(const Shape& shape, f32 by) {
		Shape grown = shape;
		grown.width += by;
		grown.count += 1;
		grown.at = shape.at + Vec3(by, 0.0f, 0.0f);
		grown.flags = static_cast<u8>(shape.flags | 0x80);
		grown.tone = NextTone(shape.tone);
		return grown;
	}

	Wide Halve(const Wide& wide) {
		Wide half;
		half.corner = wide.corner * 0.5f;
		half.weight = wide.weight * 0.5;
		half.on = !wide.on;
		return half;
	}

	void Describe(const Shape& shape, i32& outCount, Vec3& outAt, str& outText, Tone& outTone, bool& outWide, f64& outArea) {
		outCount = shape.count;
		outAt = shape.at;
		outText = std::format("shape {} x{}", shape.width, shape.count);
		outTone = shape.tone;
		outWide = shape.width > 1.0f;
		outArea = static_cast<f64>(shape.width) * shape.count;
	}

	bool Split(const Wide& wide, Wide& outHalf, str& outFirst, str& outSecond) {
		outHalf = Halve(wide);
		outFirst = "first";
		outSecond = std::format("second {}", wide.weight);
		return wide.on;
	}

	SceneObject* FindParent(SceneObject& child, SceneObject*& outRoot) {
		SceneObject* parent = child.GetParent().Get();
		outRoot = &child;
		while (outRoot->GetParent().IsValid()) {
			outRoot = outRoot->GetParent().Get();
		}
		return parent;
	}

	i32 Sum(std::span<const i32> values) {
		i32 total = 0;
		for (const i32 value : values) {
			total += value;
		}
		return total;
	}

	void Fill(std::span<f32> values, f32 value) {
		for (f32& slot : values) {
			slot = value;
		}
	}

	Vector<i32> Count(i32 count) {
		Vector<i32> out;
		for (i32 index = 0; index < count; ++index) {
			out.push_back(index * index);
		}
		return out;
	}

	Vector<Shape> Shapes(i32 count) {
		Vector<Shape> out;
		for (i32 index = 0; index < count; ++index) {
			Shape shape;
			shape.width = static_cast<f32>(index) + 0.5f;
			shape.count = index;
			shape.at = Vec3(static_cast<f32>(index), 1.0f, 2.0f);
			shape.flags = static_cast<u8>(index);
			shape.tone = index % 2 == 0 ? Tone::Low : Tone::High;
			out.push_back(shape);
		}
		return out;
	}

	f32 Widths(std::span<const Shape> shapes) {
		f32 total = 0.0f;
		for (const Shape& shape : shapes) {
			total += shape.width;
		}
		return total;
	}

	str Join(std::span<const std::string_view> parts, std::string_view separator) {
		str out;
		for (size_t index = 0; index < parts.size(); ++index) {
			out += index ? str(separator) : str();
			out += parts[index];
		}
		return out;
	}

	Vector<str> Words(std::string_view text) {
		Vector<str> out;
		size_t start = 0;
		while (start <= text.size()) {
			const size_t end = text.find(' ', start);
			out.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
			if (end == std::string_view::npos) {
				break;
			}
			start = end + 1;
		}
		return out;
	}

	Vector<Handle<SceneObject>> Children(SceneObject& parent) {
		Vector<Handle<SceneObject>> out;
		for (u32 index = 0; index < parent.GetChildCount(); ++index) {
			out.push_back(parent.GetChild(index));
		}
		return out;
	}

	i32 CountNamed(std::span<SceneObject* const> objects, std::string_view name) {
		i32 count = 0;
		for (SceneObject* object : objects) {
			count += object && object->name == name ? 1 : 0;
		}
		return count;
	}

	Vector<Tone> Tones() {
		return Vector<Tone>{Tone::Low, Tone::Mid, Tone::High};
	}

	Vector<f64> Halves(std::span<const f64> values) {
		Vector<f64> out;
		for (const f64 value : values) {
			out.push_back(value * 0.5);
		}
		return out;
	}

	Counter& GetCounter() {
		return _counters[0];
	}

	Vector<Counter*> Counters() {
		return Vector<Counter*>{&_counters[0], &_counters[1], &_counters[2]};
	}

	i32 TotalOf(std::span<Counter* const> counters) {
		i32 total = 0;
		for (Counter* counter : counters) {
			total += counter ? counter->GetTotal() : 0;
		}
		return total;
	}

	Recipe MakeRecipe(std::string_view name, i32 steps) {
		Recipe recipe;
		recipe.name = name;
		for (i32 index = 0; index < steps; ++index) {
			recipe.steps.push_back(index + 1);
		}
		recipe.heat = 0.5f * static_cast<f32>(steps);
		return recipe;
	}

	str DescribeRecipe(const Recipe& recipe) {
		return std::format("{} in {} step(s) at {}", recipe.name, recipe.steps.size(), recipe.heat);
	}

	i32 CountLiveRecipes() {
		return _liveRecipes;
	}

	void Primes(i32 count, Vector<i32>& outPrimes) {
		outPrimes.clear();
		for (i32 candidate = 2; static_cast<i32>(outPrimes.size()) < count; ++candidate) {
			bool prime = true;
			for (const i32 known : outPrimes) {
				prime = prime && candidate % known != 0;
			}
			if (prime) {
				outPrimes.push_back(candidate);
			}
		}
	}

	bool Menu(i32 count, Vector<Recipe>& outRecipes, Vector<str>& outNames) {
		for (i32 index = 0; index < count; ++index) {
			outRecipes.push_back(MakeRecipe(std::format("dish {}", index), index + 1));
			outNames.push_back(outRecipes.back().name);
		}
		return count > 0;
	}

	Vector<Recipe> Recipes(i32 count) {
		Vector<Recipe> recipes;
		for (i32 index = 0; index < count; ++index) {
			recipes.push_back(MakeRecipe(std::format("recipe {}", index), index));
		}
		return recipes;
	}

	i32 CountSteps(const Vector<Recipe>& recipes) {
		i32 steps = 0;
		for (const Recipe& recipe : recipes) {
			steps += static_cast<i32>(recipe.steps.size());
		}
		return steps;
	}

	void Stock(Kitchen& kitchen, i32 count) {
		kitchen.main.name = "main";
		kitchen.shelf = Recipes(count);
	}

	i32 Apply(i32 value, const std::function<i32(i32)>& step) {
		return step ? step(step(value)) : -1;
	}

	str Label(const std::function<str(std::string_view, f32)>& make) {
		return make ? make("label", 1.5f) + "|" + make("", -2.0f) : "none";
	}

	i32 Visit(SceneObject& root, const std::function<bool(SceneObject*, const Shape&, Tone)>& visitor) {
		i32 visited = 0;
		for (u32 index = 0; index < root.GetChildCount(); ++index) {
			Shape shape;
			shape.count = visited;
			shape.width = 0.5f * static_cast<f32>(visited);
			++visited;
			if (!visitor(root.GetChild(index).Get(), shape, visited % 2 == 0 ? Tone::High : Tone::Low)) {
				break;
			}
		}
		return visited;
	}

	void KeepAction(std::function<void(const Recipe&, Vec3)> action) {
		_keptAction = std::move(action);
	}

	bool RunKept(const Recipe& recipe, const Vec3& at) {
		if (!_keptAction) {
			return false;
		}
		_keptAction(recipe, at);
		return true;
	}

	void DropKept() {
		_keptAction = nullptr;
	}

} // namespace Shalltear::CSharpImpl::SlotProbe

#endif
