#ifdef SLT_CSHARP_TEST_HOOKS

#include "BindingProbe.h"

#include "ManagedTypes.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Math/MathUtil.h"

#include <iterator>
#include <mutex>
#include <stdexcept>

namespace Shalltear::CSharpImpl::BindingProbe {

	namespace {

		struct MathCase {
			std::string_view name;
			i32 inputs;
			i32 outputs;
			void (*run)(const f32* in, f32* out);
		};

		Vec2 ReadVec2(const f32* in) {
			return Vec2(in[0], in[1]);
		}
		Vec3 ReadVec3(const f32* in) {
			return Vec3(in[0], in[1], in[2]);
		}
		Vec4 ReadVec4(const f32* in) {
			return Vec4(in[0], in[1], in[2], in[3]);
		}
		Quat ReadQuat(const f32* in) {
			return Quat(in[0], in[1], in[2], in[3]);
		}
		Color ReadColor(const f32* in) {
			return Color(in[0], in[1], in[2], in[3]);
		}

		void Put(f32* out, f32 value) {
			out[0] = value;
		}
		void Put(f32* out, bool value) {
			out[0] = value ? 1.0f : 0.0f;
		}
		void Put(f32* out, const Vec2& value) {
			out[0] = value.x;
			out[1] = value.y;
		}
		void Put(f32* out, const Vec3& value) {
			out[0] = value.x;
			out[1] = value.y;
			out[2] = value.z;
		}
		void Put(f32* out, const Vec4& value) {
			out[0] = value.x;
			out[1] = value.y;
			out[2] = value.z;
			out[3] = value.w;
		}
		void Put(f32* out, const Quat& value) {
			out[0] = value.x;
			out[1] = value.y;
			out[2] = value.z;
			out[3] = value.w;
		}
		void Put(f32* out, const Color& value) {
			out[0] = value.r;
			out[1] = value.g;
			out[2] = value.b;
			out[3] = value.a;
		}

		constexpr MathCase MathCases[] = {
			{"Vec2.Add", 4, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) + ReadVec2(in + 2)); }},
			{"Vec2.Subtract", 4, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) - ReadVec2(in + 2)); }},
			{"Vec2.Multiply", 4, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) * ReadVec2(in + 2)); }},
			{"Vec2.Divide", 4, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) / ReadVec2(in + 2)); }},
			{"Vec2.Scale", 3, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) * in[2]); }},
			{"Vec2.ScaleLeft", 3, 2, [](const f32* in, f32* out) { Put(out, in[2] * ReadVec2(in)); }},
			{"Vec2.DivideScalar", 3, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in) / in[2]); }},
			{"Vec2.Negate", 2, 2, [](const f32* in, f32* out) { Put(out, -ReadVec2(in)); }},
			{"Vec2.LengthSquared", 2, 1, [](const f32* in, f32* out) { Put(out, ReadVec2(in).LengthSquared()); }},
			{"Vec2.Length", 2, 1, [](const f32* in, f32* out) { Put(out, ReadVec2(in).Length()); }},
			{"Vec2.Normalized", 2, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in).Normalized()); }},
			{"Vec2.Dot", 4, 1, [](const f32* in, f32* out) { Put(out, Vec2::Dot(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Cross", 4, 1, [](const f32* in, f32* out) { Put(out, Vec2::Cross(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.DistanceSquared", 4, 1, [](const f32* in, f32* out) { Put(out, Vec2::DistanceSquared(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Distance", 4, 1, [](const f32* in, f32* out) { Put(out, Vec2::Distance(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Lerp", 5, 2, [](const f32* in, f32* out) { Put(out, Vec2::Lerp(ReadVec2(in), ReadVec2(in + 2), in[4])); }},
			{"Vec2.LerpUnclamped", 5, 2, [](const f32* in, f32* out) { Put(out, Vec2::LerpUnclamped(ReadVec2(in), ReadVec2(in + 2), in[4])); }},
			{"Vec2.Min", 4, 2, [](const f32* in, f32* out) { Put(out, Vec2::Min(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Max", 4, 2, [](const f32* in, f32* out) { Put(out, Vec2::Max(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Abs", 2, 2, [](const f32* in, f32* out) { Put(out, Vec2::Abs(ReadVec2(in))); }},
			{"Vec2.Perpendicular", 2, 2, [](const f32* in, f32* out) { Put(out, ReadVec2(in).Perpendicular()); }},
			{"Vec2.Reflect", 4, 2, [](const f32* in, f32* out) { Put(out, Vec2::Reflect(ReadVec2(in), ReadVec2(in + 2))); }},
			{"Vec2.Approximately", 5, 1, [](const f32* in, f32* out) { Put(out, Vec2::Approximately(ReadVec2(in), ReadVec2(in + 2), Math::Abs(in[4]))); }},
			{"Vec2.Equals", 4, 1, [](const f32* in, f32* out) { Put(out, ReadVec2(in) == ReadVec2(in + 2)); }},

			{"Vec3.Add", 6, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) + ReadVec3(in + 3)); }},
			{"Vec3.Subtract", 6, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) - ReadVec3(in + 3)); }},
			{"Vec3.Multiply", 6, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) * ReadVec3(in + 3)); }},
			{"Vec3.Divide", 6, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) / ReadVec3(in + 3)); }},
			{"Vec3.Scale", 4, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) * in[3]); }},
			{"Vec3.ScaleLeft", 4, 3, [](const f32* in, f32* out) { Put(out, in[3] * ReadVec3(in)); }},
			{"Vec3.DivideScalar", 4, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in) / in[3]); }},
			{"Vec3.Negate", 3, 3, [](const f32* in, f32* out) { Put(out, -ReadVec3(in)); }},
			{"Vec3.LengthSquared", 3, 1, [](const f32* in, f32* out) { Put(out, ReadVec3(in).LengthSquared()); }},
			{"Vec3.Length", 3, 1, [](const f32* in, f32* out) { Put(out, ReadVec3(in).Length()); }},
			{"Vec3.Normalized", 3, 3, [](const f32* in, f32* out) { Put(out, ReadVec3(in).Normalized()); }},
			{"Vec3.Dot", 6, 1, [](const f32* in, f32* out) { Put(out, Vec3::Dot(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Cross", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::Cross(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.DistanceSquared", 6, 1, [](const f32* in, f32* out) { Put(out, Vec3::DistanceSquared(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Distance", 6, 1, [](const f32* in, f32* out) { Put(out, Vec3::Distance(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Lerp", 7, 3, [](const f32* in, f32* out) { Put(out, Vec3::Lerp(ReadVec3(in), ReadVec3(in + 3), in[6])); }},
			{"Vec3.LerpUnclamped", 7, 3, [](const f32* in, f32* out) { Put(out, Vec3::LerpUnclamped(ReadVec3(in), ReadVec3(in + 3), in[6])); }},
			{"Vec3.Min", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::Min(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Max", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::Max(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Abs", 3, 3, [](const f32* in, f32* out) { Put(out, Vec3::Abs(ReadVec3(in))); }},
			{"Vec3.Reflect", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::Reflect(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Project", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::Project(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.ProjectOnPlane", 6, 3, [](const f32* in, f32* out) { Put(out, Vec3::ProjectOnPlane(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Angle", 6, 1, [](const f32* in, f32* out) { Put(out, Vec3::Angle(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Vec3.Approximately", 7, 1, [](const f32* in, f32* out) { Put(out, Vec3::Approximately(ReadVec3(in), ReadVec3(in + 3), Math::Abs(in[6]))); }},
			{"Vec3.Equals", 6, 1, [](const f32* in, f32* out) { Put(out, ReadVec3(in) == ReadVec3(in + 3)); }},

			{"Vec4.Add", 8, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) + ReadVec4(in + 4)); }},
			{"Vec4.Subtract", 8, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) - ReadVec4(in + 4)); }},
			{"Vec4.Multiply", 8, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) * ReadVec4(in + 4)); }},
			{"Vec4.Divide", 8, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) / ReadVec4(in + 4)); }},
			{"Vec4.Scale", 5, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) * in[4]); }},
			{"Vec4.ScaleLeft", 5, 4, [](const f32* in, f32* out) { Put(out, in[4] * ReadVec4(in)); }},
			{"Vec4.DivideScalar", 5, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in) / in[4]); }},
			{"Vec4.Negate", 4, 4, [](const f32* in, f32* out) { Put(out, -ReadVec4(in)); }},
			{"Vec4.LengthSquared", 4, 1, [](const f32* in, f32* out) { Put(out, ReadVec4(in).LengthSquared()); }},
			{"Vec4.Length", 4, 1, [](const f32* in, f32* out) { Put(out, ReadVec4(in).Length()); }},
			{"Vec4.Normalized", 4, 4, [](const f32* in, f32* out) { Put(out, ReadVec4(in).Normalized()); }},
			{"Vec4.Dot", 8, 1, [](const f32* in, f32* out) { Put(out, Vec4::Dot(ReadVec4(in), ReadVec4(in + 4))); }},
			{"Vec4.Lerp", 9, 4, [](const f32* in, f32* out) { Put(out, Vec4::Lerp(ReadVec4(in), ReadVec4(in + 4), in[8])); }},
			{"Vec4.LerpUnclamped", 9, 4, [](const f32* in, f32* out) { Put(out, Vec4::LerpUnclamped(ReadVec4(in), ReadVec4(in + 4), in[8])); }},
			{"Vec4.Min", 8, 4, [](const f32* in, f32* out) { Put(out, Vec4::Min(ReadVec4(in), ReadVec4(in + 4))); }},
			{"Vec4.Max", 8, 4, [](const f32* in, f32* out) { Put(out, Vec4::Max(ReadVec4(in), ReadVec4(in + 4))); }},
			{"Vec4.Approximately", 9, 1, [](const f32* in, f32* out) { Put(out, Vec4::Approximately(ReadVec4(in), ReadVec4(in + 4), Math::Abs(in[8]))); }},
			{"Vec4.Equals", 8, 1, [](const f32* in, f32* out) { Put(out, ReadVec4(in) == ReadVec4(in + 4)); }},
			{"Vec4.XYZ", 4, 3, [](const f32* in, f32* out) { Put(out, ReadVec4(in).XYZ()); }},

			{"Quat.Multiply", 8, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in) * ReadQuat(in + 4)); }},
			{"Quat.Rotate", 7, 3, [](const f32* in, f32* out) { Put(out, ReadQuat(in) * ReadVec3(in + 4)); }},
			{"Quat.Negate", 4, 4, [](const f32* in, f32* out) { Put(out, -ReadQuat(in)); }},
			{"Quat.Scale", 5, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in) * in[4]); }},
			{"Quat.Add", 8, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in) + ReadQuat(in + 4)); }},
			{"Quat.Dot", 8, 1, [](const f32* in, f32* out) { Put(out, Quat::Dot(ReadQuat(in), ReadQuat(in + 4))); }},
			{"Quat.LengthSquared", 4, 1, [](const f32* in, f32* out) { Put(out, ReadQuat(in).LengthSquared()); }},
			{"Quat.Length", 4, 1, [](const f32* in, f32* out) { Put(out, ReadQuat(in).Length()); }},
			{"Quat.Normalized", 4, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in).Normalized()); }},
			{"Quat.Conjugated", 4, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in).Conjugated()); }},
			{"Quat.Inverse", 4, 4, [](const f32* in, f32* out) { Put(out, ReadQuat(in).Inverse()); }},
			{"Quat.AngleAxis", 4, 4, [](const f32* in, f32* out) { Put(out, Quat::AngleAxis(in[0] * 60.0f, ReadVec3(in + 1))); }},
			{"Quat.FromEuler", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f)); }},
			{"Quat.FromEulerXYZ", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::XYZ)); }},
			{"Quat.FromEulerXZY", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::XZY)); }},
			{"Quat.FromEulerYXZ", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::YXZ)); }},
			{"Quat.FromEulerYZX", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::YZX)); }},
			{"Quat.FromEulerZXY", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::ZXY)); }},
			{"Quat.FromEulerZYX", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::FromEuler(ReadVec3(in) * 60.0f, RotationOrder::ZYX)); }},
			{"Quat.ToEuler", 4, 3, [](const f32* in, f32* out) { Put(out, ReadQuat(in).ToEuler()); }},
			{"Quat.FromBasis", 9, 4, [](const f32* in, f32* out) { Put(out, Quat::FromBasis(ReadVec3(in), ReadVec3(in + 3), ReadVec3(in + 6))); }},
			{"Quat.LookRotation", 6, 4, [](const f32* in, f32* out) { Put(out, Quat::LookRotation(ReadVec3(in), ReadVec3(in + 3))); }},
			{"Quat.LookRotationUp", 3, 4, [](const f32* in, f32* out) { Put(out, Quat::LookRotation(ReadVec3(in))); }},
			{"Quat.Angle", 8, 1, [](const f32* in, f32* out) { Put(out, Quat::Angle(ReadQuat(in), ReadQuat(in + 4))); }},
			{"Quat.Lerp", 9, 4, [](const f32* in, f32* out) { Put(out, Quat::Lerp(ReadQuat(in), ReadQuat(in + 4), in[8])); }},
			{"Quat.LerpUnclamped", 9, 4, [](const f32* in, f32* out) { Put(out, Quat::LerpUnclamped(ReadQuat(in), ReadQuat(in + 4), in[8])); }},
			{"Quat.Slerp", 9, 4, [](const f32* in, f32* out) { Put(out, Quat::Slerp(ReadQuat(in), ReadQuat(in + 4), in[8])); }},
			{"Quat.SlerpUnclamped", 9, 4, [](const f32* in, f32* out) { Put(out, Quat::SlerpUnclamped(ReadQuat(in), ReadQuat(in + 4), in[8])); }},
			{"Quat.Approximately", 9, 1, [](const f32* in, f32* out) { Put(out, Quat::Approximately(ReadQuat(in), ReadQuat(in + 4), Math::Abs(in[8]))); }},
			{"Quat.Equals", 8, 1, [](const f32* in, f32* out) { Put(out, ReadQuat(in) == ReadQuat(in + 4)); }},

			{"Color.Add", 8, 4, [](const f32* in, f32* out) { Put(out, ReadColor(in) + ReadColor(in + 4)); }},
			{"Color.Subtract", 8, 4, [](const f32* in, f32* out) { Put(out, ReadColor(in) - ReadColor(in + 4)); }},
			{"Color.Multiply", 8, 4, [](const f32* in, f32* out) { Put(out, ReadColor(in) * ReadColor(in + 4)); }},
			{"Color.Scale", 5, 4, [](const f32* in, f32* out) { Put(out, ReadColor(in) * in[4]); }},
			{"Color.ScaleLeft", 5, 4, [](const f32* in, f32* out) { Put(out, in[4] * ReadColor(in)); }},
			{"Color.Lerp", 9, 4, [](const f32* in, f32* out) { Put(out, Color::Lerp(ReadColor(in), ReadColor(in + 4), in[8])); }},
			{"Color.LerpUnclamped", 9, 4, [](const f32* in, f32* out) { Put(out, Color::LerpUnclamped(ReadColor(in), ReadColor(in + 4), in[8])); }},
			{"Color.Approximately", 9, 1, [](const f32* in, f32* out) { Put(out, Color::Approximately(ReadColor(in), ReadColor(in + 4), Math::Abs(in[8]))); }},
			{"Color.Saturated", 4, 4, [](const f32* in, f32* out) { Put(out, ReadColor(in).Saturated()); }},
			{"Color.Equals", 8, 1, [](const f32* in, f32* out) { Put(out, ReadColor(in) == ReadColor(in + 4)); }},
		};

		std::mutex _recordsMutex;
		std::vector<str> _records;
		i32 _touches = 0;
		DriverFn _driver = nullptr;

	} // anonymous namespace

	i32 GetMathCaseCount() noexcept {
		return static_cast<i32>(std::size(MathCases));
	}

	i32 EvaluateMath(std::string_view name, std::span<const f32> inputs, std::span<f32> outputs) {
		for (const MathCase& entry : MathCases) {
			if (entry.name != name) {
				continue;
			}
			if (static_cast<i32>(inputs.size()) != entry.inputs || static_cast<i32>(outputs.size()) < entry.outputs) {
				return -1;
			}
			entry.run(inputs.data(), outputs.data());
			return entry.outputs;
		}
		return -1;
	}

	bool EchoBool(bool value) noexcept {
		return !value;
	}
	i32 EchoI32(i32 value) noexcept {
		return value + 1;
	}
	u32 EchoU32(u32 value) noexcept {
		return ~value;
	}
	i64 EchoI64(i64 value) noexcept {
		return -value;
	}
	u64 EchoU64(u64 value) noexcept {
		return value ^ 0xFFFF0000FFFF0000ull;
	}
	f32 EchoF32(f32 value) noexcept {
		return value * 0.5f;
	}
	f64 EchoF64(f64 value) noexcept {
		return value * 0.25;
	}
	Vec2 EchoVec2(const Vec2& value) noexcept {
		return Vec2(value.y, value.x) * 2.0f;
	}
	Vec3 EchoVec3(const Vec3& value) noexcept {
		return Vec3(value.z, value.x, value.y) * 2.0f;
	}
	Vec4 EchoVec4(const Vec4& value) noexcept {
		return Vec4(value.w, value.z, value.y, value.x) * 2.0f;
	}
	Quat EchoQuat(const Quat& value) noexcept {
		return Quat(value.w, value.z, value.y, value.x);
	}
	Color EchoColor(const Color& value) noexcept {
		return Color(value.a, value.b, value.g, value.r);
	}

	f64 Mix(bool flag, i32 count, f32 scale, const Vec3& position, f64 weight, const Vec4& plane, u64 mask, const Quat& rotation,
			const Color& tint, const Vec2& offset) noexcept {
		return (flag ? 1.0 : 0.0) + 2.0 * count + 3.0 * scale + 5.0 * position.x + 7.0 * position.y + 11.0 * position.z + 13.0 * weight +
			   17.0 * plane.x + 19.0 * plane.y + 23.0 * plane.z + 29.0 * plane.w + 31.0 * static_cast<f64>(mask) + 37.0 * rotation.x +
			   41.0 * rotation.y + 43.0 * rotation.z + 47.0 * rotation.w + 53.0 * tint.r + 59.0 * tint.g + 61.0 * tint.b + 67.0 * tint.a +
			   71.0 * offset.x + 73.0 * offset.y;
	}

	Vec3 SumGuarded(const Vec3& first, const Vec4& second) {
		return Vec3(first.x + second.x, first.y + second.y, first.z + second.z + second.w);
	}

	bool IsPositive(f32 value) {
		return value > 0.0f;
	}

	void Throw() {
		throw std::runtime_error("thrown by a binding");
	}

	i32 SumBytes(std::string_view text) {
		i32 sum = static_cast<i32>(text.size()) * 1000;
		for (const char character : text) {
			sum += static_cast<u8>(character);
		}
		return sum;
	}

	void Record(std::string_view text) {
		const std::lock_guard lock(_recordsMutex);
		_records.emplace_back(text);
	}

	std::vector<str> TakeRecords() {
		const std::lock_guard lock(_recordsMutex);
		std::vector<str> taken;
		taken.swap(_records);
		return taken;
	}

	SceneObject* CreateObject(std::string_view name) {
		return SceneSystem::CreateObject(name).Get();
	}

	i32 Touch(SceneObject& target) {
		(void)target;
		return ++_touches;
	}

	i32 GetTouches() {
		return _touches;
	}

	i32 GetAliveChecks() {
		return static_cast<i32>(ManagedTypes::GetCounters().aliveChecks);
	}

	u64 FindAsset(std::string_view path) {
		const Asset* asset = AssetDatabase::LoadAsset(Path(str(path)));
		return asset ? asset->GetInstanceId() : 0;
	}

	void SetDriver(DriverFn driver) {
		_driver = driver;
	}

	i32 Drive(i32 op, i32 first, i32 second) {
		if (!_driver) {
			throw std::runtime_error("no test driver installed");
		}
		return _driver(op, first, second);
	}

	void RunFrames(i32 count) {
		for (i32 frame = 0; frame < count; ++frame) {
			SceneSystem::FixedUpdateAll();
			SceneSystem::UpdateAll();
			SceneSystem::LateUpdateAll();
			SceneSystem::EndFrame();
		}
	}

} // namespace Shalltear::CSharpImpl::BindingProbe

#endif
