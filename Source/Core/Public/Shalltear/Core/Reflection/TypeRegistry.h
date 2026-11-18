#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	struct SHALLTEAR_CORE_API TypeRegistrarNode {
		const TypeInfo* (*registerFn)();
		TypeRegistrarNode* next;

		explicit TypeRegistrarNode(const TypeInfo* (*fn)());
	};

	class SHALLTEAR_CORE_API TypeRegistry {
	public:
		static const TypeInfo* Register(TypeInfo* typeInfo, const void* ownerHint = nullptr);

		// null when name is taken
		static const TypeInfo* RegisterOwned(TypeInfo* typeInfo, const void* ownerToken);

		// still true once its types are unregistered
		static bool IsOwnerToken(const void* owner);

		// never frees the metadata (statics may cache the pointer)
		static void Unregister(StringHash name);

		static u32 UnregisterOwnedBy(const void* imageBase);

		static const TypeInfo* FindType(StringHash name);

		static bool IsDerivedFrom(const TypeInfo* child, const TypeInfo* parent);

		static u32 GetTypeCount();

		static void ForEachType(void (*visitor)(const TypeInfo* type, void* user), void* user);

		// caller owns the result
		static Object* CreateObjectInstance(const TypeInfo* type, MemTag tag);

		static void DestroyObjectInstance(Object* object);

		static void AddPendingRegistration(TypeRegistrarNode* node);

		static void RegisterPendingTypes();

		// nodes must leave the chain before FreeLibrary
		static u32 UnlinkPendingRegistrationsIn(const void* imageBase);

		static void InitializePrimitives();
	private:
		static const TypeInfo* RegisterAs(TypeInfo* typeInfo, const void* owner, bool ownerIsToken);
	};

	struct FieldRef {
		StringHash ownerType;
		StringHash field;

		const FieldInfo* cached = nullptr;
		u32 epoch = 0;

		SHALLTEAR_CORE_API const FieldInfo* Resolve();
	};

	namespace ReflectionDetail {

		template <typename T>
		concept ReflectableSequence = requires(T& container, const T& readOnly, size_t index) {
			typename T::value_type;
			readOnly.size();
			container.resize(index);
			container[index];
		};

	} // namespace ReflectionDetail

	template <typename T>
	struct GetTypeInfoFor {
		static const TypeInfo* Get() {
			if constexpr (requires { T::GetReflectType(); }) {
				return T::GetReflectType();
			} else if constexpr (ReflectionDetail::ReflectableSequence<T>) {
				static_assert(sizeof(T) == 0,
							  "Container field declared with a scalar reflection macro. "
							  "Use SLT_REFLECT_ARRAY(field) for a resizable sequence: only its "
							  "ELEMENT type needs reflection, specializing GetTypeInfoFor for "
							  "the container should not be the fix.");
				return nullptr;
			} else {
				static_assert(sizeof(T) == 0,
							  "Type has no reflection metadata. Use SLT_REFLECT_CLASS on user types "
							  "(or provide a GetTypeInfoFor<T> specialization for primitives).");
				return nullptr;
			}
		}
	};

	template <typename T>
	inline const TypeInfo* GetStaticType() {
		return GetTypeInfoFor<std::remove_cv_t<T>>::Get();
	}

} // namespace Shalltear
