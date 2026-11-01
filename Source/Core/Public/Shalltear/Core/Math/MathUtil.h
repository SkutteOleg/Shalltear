#pragma once

#include "Shalltear/Core/Types.h"

#include <bit>
#include <cmath>
#include <type_traits>

namespace Shalltear::Math {

	inline constexpr f32 PI = 3.14159265358979323846f;
	inline constexpr f32 TAU = PI * 2.0f;
	inline constexpr f32 HALF_PI = PI * 0.5f;
	inline constexpr f32 DEG_TO_RAD = PI / 180.0f;
	inline constexpr f32 RAD_TO_DEG = 180.0f / PI;

	inline constexpr f32 EPSILON = 1e-5f;

	template <typename T>
	[[nodiscard]] constexpr T Abs(T v) noexcept {
		static_assert(std::is_arithmetic_v<T>);
		return v < T(0) ? -v : v;
	}

	template <typename T>
	[[nodiscard]] constexpr T Sign(T v) noexcept {
		static_assert(std::is_arithmetic_v<T>);
		return v > T(0) ? T(1) : (v < T(0) ? T(-1) : T(0));
	}

	template <typename T>
	[[nodiscard]] constexpr T Min(T a, T b) noexcept {
		return a < b ? a : b;
	}

	template <typename T>
	[[nodiscard]] constexpr T Max(T a, T b) noexcept {
		return a > b ? a : b;
	}

	template <typename T>
	[[nodiscard]] constexpr T Clamp(T v, T min, T max) noexcept {
		return v < min ? min : (v > max ? max : v);
	}

	[[nodiscard]] constexpr f32 Clamp01(f32 v) noexcept {
		return Clamp(v, 0.0f, 1.0f);
	}

	[[nodiscard]] constexpr f32 DegToRad(f32 degrees) noexcept {
		return degrees * DEG_TO_RAD;
	}

	[[nodiscard]] constexpr f32 RadToDeg(f32 radians) noexcept {
		return radians * RAD_TO_DEG;
	}

	[[nodiscard]] constexpr f32 Lerp(f32 a, f32 b, f32 t) noexcept {
		return a + (b - a) * Clamp01(t);
	}

	[[nodiscard]] constexpr f32 LerpUnclamped(f32 a, f32 b, f32 t) noexcept {
		return a + (b - a) * t;
	}

	[[nodiscard]] constexpr f32 InverseLerp(f32 a, f32 b, f32 v) noexcept {
		return a != b ? Clamp01((v - a) / (b - a)) : 0.0f;
	}

	[[nodiscard]] constexpr f32 SmoothStep(f32 from, f32 to, f32 t) noexcept {
		f32 x = Clamp01(t);
		x = x * x * (3.0f - 2.0f * x);
		return from + (to - from) * x;
	}

	[[nodiscard]] constexpr f32 MoveTowards(f32 current, f32 target, f32 maxDelta) noexcept {
		if (Abs(target - current) <= maxDelta) {
			return target;
		}
		return current + Sign(target - current) * maxDelta;
	}

	[[nodiscard]] inline f32 Repeat(f32 v, f32 length) noexcept {
		return Clamp(v - std::floor(v / length) * length, 0.0f, length);
	}

	[[nodiscard]] constexpr bool Approximately(f32 a, f32 b, f32 epsilon = EPSILON) noexcept {
		return Abs(a - b) <= epsilon;
	}

	[[nodiscard]] constexpr bool IsFinite(f32 v) noexcept {
		return (std::bit_cast<u32>(v) & 0x7f800000u) != 0x7f800000u;
	}

	[[nodiscard]] inline f32 Sqrt(f32 v) noexcept {
		return std::sqrt(v);
	}

	[[nodiscard]] inline f32 Sin(f32 radians) noexcept {
		return std::sin(radians);
	}

	[[nodiscard]] inline f32 Cos(f32 radians) noexcept {
		return std::cos(radians);
	}

	[[nodiscard]] inline f32 Tan(f32 radians) noexcept {
		return std::tan(radians);
	}

	[[nodiscard]] inline f32 Asin(f32 v) noexcept {
		return std::asin(Clamp(v, -1.0f, 1.0f));
	}

	[[nodiscard]] inline f32 Acos(f32 v) noexcept {
		return std::acos(Clamp(v, -1.0f, 1.0f));
	}

	[[nodiscard]] inline f32 Atan(f32 v) noexcept {
		return std::atan(v);
	}

	[[nodiscard]] inline f32 Atan2(f32 y, f32 x) noexcept {
		return std::atan2(y, x);
	}

	[[nodiscard]] inline f32 Pow(f32 x, f32 y) noexcept {
		return std::pow(x, y);
	}

	[[nodiscard]] inline f32 Exp(f32 power) noexcept {
		return std::exp(power);
	}

	[[nodiscard]] inline f32 Log(f32 v) noexcept {
		return std::log(v);
	}

	[[nodiscard]] inline f32 Log10(f32 v) noexcept {
		return std::log10(v);
	}

	[[nodiscard]] inline f32 Floor(f32 v) noexcept {
		return std::floor(v);
	}

	[[nodiscard]] inline f32 Ceil(f32 v) noexcept {
		return std::ceil(v);
	}

	[[nodiscard]] inline f32 Round(f32 v) noexcept {
		return std::round(v);
	}

	[[nodiscard]] inline i32 FloorToInt(f32 v) noexcept {
		return static_cast<i32>(std::floor(v));
	}

	[[nodiscard]] inline i32 CeilToInt(f32 v) noexcept {
		return static_cast<i32>(std::ceil(v));
	}

	[[nodiscard]] inline i32 RoundToInt(f32 v) noexcept {
		return static_cast<i32>(std::round(v));
	}

	[[nodiscard]] inline f32 PingPong(f32 v, f32 length) noexcept {
		f32 t = Repeat(v, length * 2.0f);
		return length - Abs(t - length);
	}

	[[nodiscard]] inline f32 DeltaAngle(f32 current, f32 target) noexcept {
		f32 delta = Repeat(target - current, 360.0f);
		if (delta > 180.0f) {
			delta -= 360.0f;
		}
		return delta;
	}

	[[nodiscard]] inline f32 LerpAngle(f32 a, f32 b, f32 t) noexcept {
		return a + DeltaAngle(a, b) * Clamp01(t);
	}

	[[nodiscard]] inline f32 MoveTowardsAngle(f32 current, f32 target, f32 maxDelta) noexcept {
		f32 deltaAngle = DeltaAngle(current, target);
		if (-maxDelta < deltaAngle && deltaAngle < maxDelta) {
			return target;
		}
		return MoveTowards(current, current + deltaAngle, maxDelta);
	}

	[[nodiscard]] constexpr f32 InverseSafe(f32 v, f32 epsilon = EPSILON) noexcept {
		return Abs(v) > epsilon ? 1.0f / v : 0.0f;
	}

	struct HermiteWeights {
		f32 h00 = 0.0f;
		f32 h10 = 0.0f;
		f32 h01 = 0.0f;
		f32 h11 = 0.0f;
	};

	[[nodiscard]] constexpr HermiteWeights HermiteBasis(f32 s) noexcept {
		const f32 s2 = s * s;
		const f32 s3 = s2 * s;
		return {2.0f * s3 - 3.0f * s2 + 1.0f, s3 - 2.0f * s2 + s, -2.0f * s3 + 3.0f * s2, s3 - s2};
	}

	[[nodiscard]] constexpr f32 AutoSlope(f32 timeBefore, f32 valueBefore, f32 value, f32 timeAfter, f32 valueAfter, bool interior,
										  bool clamped) noexcept {
		const f32 span = timeAfter - timeBefore;
		if (!(span > 0.0f)) {
			return 0.0f;
		}
		if (clamped && interior && (value - valueBefore) * (valueAfter - value) <= 0.0f) {
			return 0.0f;
		}
		return (valueAfter - valueBefore) * (1.0f / span);
	}

} // namespace Shalltear::Math
