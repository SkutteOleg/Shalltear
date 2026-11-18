#pragma once

#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::BindingProbe {

	i32 GetMathCaseCount() noexcept;
	i32 EvaluateMath(std::string_view name, std::span<const f32> inputs, std::span<f32> outputs);

	bool EchoBool(bool value) noexcept;
	i32 EchoI32(i32 value) noexcept;
	u32 EchoU32(u32 value) noexcept;
	i64 EchoI64(i64 value) noexcept;
	u64 EchoU64(u64 value) noexcept;
	f32 EchoF32(f32 value) noexcept;
	f64 EchoF64(f64 value) noexcept;
	Vec2 EchoVec2(const Vec2& value) noexcept;
	Vec3 EchoVec3(const Vec3& value) noexcept;
	Vec4 EchoVec4(const Vec4& value) noexcept;
	Quat EchoQuat(const Quat& value) noexcept;
	Color EchoColor(const Color& value) noexcept;
	f64 Mix(bool flag, i32 count, f32 scale, const Vec3& position, f64 weight, const Vec4& plane, u64 mask, const Quat& rotation,
			const Color& tint, const Vec2& offset) noexcept;

	Vec3 SumGuarded(const Vec3& first, const Vec4& second);
	bool IsPositive(f32 value);
	void Throw();
	i32 SumBytes(std::string_view text);

} // namespace Shalltear::CSharpImpl::BindingProbe
