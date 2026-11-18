#include "FieldKinds.h"

#include "ScriptBridge.h"

#include "Shalltear/Core/Math/MathReflect.h"
#include "Shalltear/Core/Reflection/Primitives.h"

namespace Shalltear::CSharpImpl::FieldKinds {

	namespace {

		template <typename T>
		FieldKindInfo Describe() {
			EnsureVariantVTable<T>();
			return {GetStaticType<T>(), static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T))};
		}

	} // anonymous namespace

	const FieldKindInfo* Find(i32 kind) {
		// order must match SltFieldKind
		static const FieldKindInfo kinds[SLT_KIND_COUNT] = {
			Describe<bool>(),
			Describe<i8>(),
			Describe<u8>(),
			Describe<i16>(),
			Describe<u16>(),
			Describe<i32>(),
			Describe<u32>(),
			Describe<i64>(),
			Describe<u64>(),
			Describe<f32>(),
			Describe<f64>(),
			Describe<Vec2>(),
			Describe<Vec3>(),
			Describe<Vec4>(),
			Describe<Quat>(),
			Describe<Color>(),
		};
		return kind >= 0 && kind < SLT_KIND_COUNT ? &kinds[kind] : nullptr;
	}

} // namespace Shalltear::CSharpImpl::FieldKinds
