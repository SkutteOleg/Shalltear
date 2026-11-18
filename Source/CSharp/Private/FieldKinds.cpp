#include "FieldKinds.h"

#include "ScriptBridge.h"

#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/SpriteAsset.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/MathReflect.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/SceneObject.h"

namespace Shalltear::CSharpImpl::FieldKinds {

	namespace {

		template <typename T>
		FieldKindInfo Describe() {
			EnsureVariantVTable<T>();
			return {GetStaticType<T>(), static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T))};
		}

		struct EngineHandle {
			u64 pointee;
			const TypeInfo* handle;
		};

		template <typename T>
		EngineHandle Wrapped() {
			EnsureVariantVTable<Handle<T>>();
			return {T::GetReflectType()->name.GetValue(), GetStaticType<Handle<T>>()};
		}

	} // anonymous namespace

	const FieldKindInfo* Find(i32 kind) {
		// order must match SltFieldKind
		static const FieldKindInfo kinds[SLT_KIND_STRING] = {
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
		return kind >= 0 && kind < SLT_KIND_STRING ? &kinds[kind] : nullptr;
	}

	const FieldKindInfo& GetText() {
		static const FieldKindInfo text = Describe<str>();
		return text;
	}

	const TypeInfo* FindEngineHandle(u64 pointeeHash) {
		// must match EngineType attributes in Shalltear.Runtime
		static const EngineHandle wrapped[] = {
			Wrapped<SceneObject>(),
			Wrapped<MaterialAsset>(),
			Wrapped<MeshAsset>(),
			Wrapped<TextureAsset>(),
			Wrapped<PrefabAsset>(),
			Wrapped<SpriteAsset>(),
			Wrapped<TextAsset>(),
		};
		for (const EngineHandle& entry : wrapped) {
			if (entry.pointee == pointeeHash) {
				return entry.handle;
			}
		}
		return nullptr;
	}

} // namespace Shalltear::CSharpImpl::FieldKinds
