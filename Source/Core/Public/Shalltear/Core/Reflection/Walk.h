#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Reflection/Variant.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear {

	using FieldWalkCallback = void (*)(const FieldInfo* field, const Variant* value, void* userData);

	using FieldRawCallback = void (*)(const FieldInfo* field, void* fieldDataPtr, void* userData);

	SHALLTEAR_CORE_API void ForEachField(
		const void* instance,
		const TypeInfo* type,
		FieldWalkCallback callback,
		void* userData = nullptr,
		u32 walkFlags = FieldWalkFlagDefault);

	SHALLTEAR_CORE_API void ForEachFieldRaw(
		void* instance,
		const TypeInfo* type,
		FieldRawCallback callback,
		void* userData = nullptr,
		u32 walkFlags = FieldWalkFlagDefault);

	SHALLTEAR_CORE_API bool GetField(
		const void* instance,
		const TypeInfo* type,
		StringHash fieldName,
		Variant* out);

	SHALLTEAR_CORE_API bool SetField(
		void* instance,
		const TypeInfo* type,
		StringHash fieldName,
		const Variant* in);

	SHALLTEAR_CORE_API const FieldInfo* FindField(
		const TypeInfo* type,
		StringHash fieldName);

	// instance = field's direct holder (a nested value for a nested field)
	SHALLTEAR_CORE_API bool GetFieldValue(const FieldInfo* field, const void* instance, Variant* out);
	SHALLTEAR_CORE_API bool SetFieldValue(const FieldInfo* field, void* instance, const Variant* in);

	inline void SyncFieldsBeforeRead(const void* instance, const TypeInfo* type) {
		if (type && type->fieldSyncFn && instance) {
			type->fieldSyncFn(const_cast<void*>(instance), FieldSync::BeforeRead);
		}
	}

	inline void SyncFieldsAfterWrite(void* instance, const TypeInfo* type) {
		if (type && type->fieldSyncFn && instance) {
			type->fieldSyncFn(instance, FieldSync::AfterWrite);
		}
	}

	SHALLTEAR_CORE_API const MethodInfo* FindMethod(
		const TypeInfo* type,
		StringHash methodName);

	SHALLTEAR_CORE_API bool InvokeMethod(
		void* instance,
		const TypeInfo* type,
		StringHash methodName,
		const Variant* args,
		u32 argCount,
		Variant* outReturn);

	SHALLTEAR_CORE_API const EnumConstantInfo* FindEnumConstantByValue(const TypeInfo* type, u64 rawBits);
	SHALLTEAR_CORE_API const EnumConstantInfo* FindEnumConstantByName(const TypeInfo* type, std::string_view name);

	// only sanctioned way to move enum values through addrFn
	SHALLTEAR_CORE_API u64 EnumValueToBits(const TypeInfo* type, i64 value);

	SHALLTEAR_CORE_API const AttributeInfo* FindFieldAttribute(
		const FieldInfo* field,
		StringHash key);

	SHALLTEAR_CORE_API const AttributeInfo* FindTypeAttribute(
		const TypeInfo* type,
		StringHash key);

	template <typename Callable>
	inline void ForEachField(const void* instance, const TypeInfo* type, Callable&& callback, u32 walkFlags = FieldWalkFlagDefault) {
		using CallableType = std::decay_t<Callable>;
		CallableType* ptr = &const_cast<Callable&>(callback);
		auto trampoline = [](const FieldInfo* field, const Variant* value, void* userData) {
			CallableType* c = static_cast<CallableType*>(userData);
			(*c)(field, value);
		};
		ForEachField(instance, type, trampoline, ptr, walkFlags);
	}

	template <typename Callable>
	inline void ForEachFieldRaw(void* instance, const TypeInfo* type, Callable&& callback, u32 walkFlags = FieldWalkFlagDefault) {
		using CallableType = std::decay_t<Callable>;
		CallableType* ptr = &const_cast<Callable&>(callback);
		auto trampoline = [](const FieldInfo* field, void* fieldDataPtr, void* userData) {
			CallableType* c = static_cast<CallableType*>(userData);
			(*c)(field, fieldDataPtr);
		};
		ForEachFieldRaw(instance, type, trampoline, ptr, walkFlags);
	}

} // namespace Shalltear
