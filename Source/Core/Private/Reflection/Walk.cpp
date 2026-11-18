#include "Shalltear/Core/Reflection/Walk.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"

#include <cstring>

namespace Shalltear {

	namespace {

		void WalkTypeChain(const TypeInfo* type, FieldWalkCallback callback, const void* instance, void* userData, u32 walkFlags) {
			if (!type)
				return;

			if ((walkFlags & FieldWalkFlagIncludeInherited) && type->parent) {
				WalkTypeChain(type->parent, callback, instance, userData, walkFlags);
			}

			for (u32 i = 0; i < type->fieldCount; ++i) {
				const FieldInfo* field = &type->fields[i];

				if ((walkFlags & FieldWalkFlagExcludeHidden) && !(field->flags & FieldFlagEditorVisible))
					continue;
				if ((walkFlags & FieldWalkFlagExcludeReadOnly) && (field->flags & FieldFlagReadOnly))
					continue;
				if ((walkFlags & FieldWalkFlagExcludeTransient) && (field->flags & FieldFlagTransient))
					continue;
				if (!(walkFlags & FieldWalkFlagIncludeLocalIndices) && (field->flags & FieldFlagLocalIndex))
					continue;

				Variant value;
				GetFieldValue(field, instance, &value);

				callback(field, &value, userData);

				if ((walkFlags & FieldWalkFlagIncludeSubobjects) && (field->flags & FieldFlagSubobject) && field->fieldType && field->addrFn) {
				}
			}
		}

		void WalkTypeChainRaw(const TypeInfo* type, FieldRawCallback callback, void* instance, void* userData, u32 walkFlags) {
			if (!type)
				return;

			if ((walkFlags & FieldWalkFlagIncludeInherited) && type->parent) {
				WalkTypeChainRaw(type->parent, callback, instance, userData, walkFlags);
			}

			for (u32 i = 0; i < type->fieldCount; ++i) {
				const FieldInfo* field = &type->fields[i];

				if ((walkFlags & FieldWalkFlagExcludeHidden) && !(field->flags & FieldFlagEditorVisible))
					continue;
				if ((walkFlags & FieldWalkFlagExcludeReadOnly) && (field->flags & FieldFlagReadOnly))
					continue;
				if ((walkFlags & FieldWalkFlagExcludeTransient) && (field->flags & FieldFlagTransient))
					continue;
				if (!(walkFlags & FieldWalkFlagIncludeLocalIndices) && (field->flags & FieldFlagLocalIndex))
					continue;

				void* fieldPtr = field->addrFn ? field->addrFn(instance) : nullptr;
				callback(field, fieldPtr, userData);

				if ((walkFlags & FieldWalkFlagIncludeSubobjects) && (field->flags & FieldFlagSubobject) && field->fieldType && fieldPtr) {
					WalkTypeChainRaw(field->fieldType, callback, fieldPtr, userData, walkFlags);
				}
			}
		}

	} // anonymous namespace

	void ForEachField(const void* instance, const TypeInfo* type, FieldWalkCallback callback, void* userData, u32 walkFlags) {
		if (!instance || !type || !callback)
			return;
		SyncFieldsBeforeRead(instance, type);
		WalkTypeChain(type, callback, instance, userData, walkFlags);
	}

	void ForEachFieldRaw(void* instance, const TypeInfo* type, FieldRawCallback callback, void* userData, u32 walkFlags) {
		if (!instance || !type || !callback)
			return;
		SyncFieldsBeforeRead(instance, type);
		WalkTypeChainRaw(type, callback, instance, userData, walkFlags);
	}

	namespace {

		u64 EnumWidthMask(const TypeInfo* type) {
			size_t size = type ? type->size : 0;
			if (size == 0 || size >= sizeof(u64)) {
				return ~0ULL;
			}
			return (1ULL << (size * 8)) - 1ULL;
		}

	} // namespace

	const EnumConstantInfo* FindEnumConstantByValue(const TypeInfo* type, u64 rawBits) {
		if (!type) {
			return nullptr;
		}
		const u64 mask = EnumWidthMask(type);
		for (u32 i = 0; i < type->enumConstantCount; ++i) {
			if ((static_cast<u64>(type->enumConstants[i].value) & mask) == (rawBits & mask)) {
				return &type->enumConstants[i];
			}
		}
		return nullptr;
	}

	const EnumConstantInfo* FindEnumConstantByName(const TypeInfo* type, std::string_view name) {
		if (!type) {
			return nullptr;
		}
		for (u32 i = 0; i < type->enumConstantCount; ++i) {
			const char* candidate = type->enumConstants[i].displayName;
			if (candidate && name == candidate) {
				return &type->enumConstants[i];
			}
		}
		return nullptr;
	}

	u64 EnumValueToBits(const TypeInfo* type, i64 value) {
		return static_cast<u64>(value) & EnumWidthMask(type);
	}

	const FieldInfo* FindField(const TypeInfo* type, StringHash fieldName) {
		while (type) {
			for (u32 i = 0; i < type->fieldCount; ++i) {
				if (type->fields[i].name == fieldName) {
					return &type->fields[i];
				}
			}
			type = type->parent;
		}
		return nullptr;
	}

	const MethodInfo* FindMethod(const TypeInfo* type, StringHash methodName) {
		while (type) {
			for (u32 i = 0; i < type->methodCount; ++i) {
				if (type->methods[i].name == methodName) {
					return &type->methods[i];
				}
			}
			type = type->parent;
		}
		return nullptr;
	}

	bool GetField(const void* instance, const TypeInfo* type, StringHash fieldName, Variant* out) {
		if (!instance || !type || !out)
			return false;
		const FieldInfo* field = FindField(type, fieldName);
		if (!field)
			return false;
		SyncFieldsBeforeRead(instance, type);
		return GetFieldValue(field, instance, out);
	}

	bool GetFieldValue(const FieldInfo* field, const void* instance, Variant* out) {
		if (!field || !instance || !out)
			return false;
		if (field->getFn) {
			field->getFn(instance, out);
			return true;
		}
		if (!field->addrFn || !field->fieldType)
			return false;
		const void* at = field->addrFn(const_cast<void*>(instance));
		return at && Variant::CopyFromRaw(*out, field->fieldType->name, at);
	}

	bool SetFieldValue(const FieldInfo* field, void* instance, const Variant* in) {
		if (!field || !instance || !in)
			return false;
		if (field->setFn) {
			field->setFn(instance, in);
			return true;
		}
		if (!field->addrFn || !field->fieldType || in->GetTypeId() != field->fieldType->name)
			return false;
		void* at = field->addrFn(instance);
		const VariantVTable* vtable = VariantDispatch::Lookup(field->fieldType->name);
		if (!at || !vtable)
			return false;
		vtable->destroy(at);
		vtable->copy(at, in->GetData());
		return true;
	}

	bool SetField(void* instance, const TypeInfo* type, StringHash fieldName, const Variant* in) {
		if (!instance || !type || !in)
			return false;
		const FieldInfo* field = FindField(type, fieldName);
		if (!field || !field->fieldType || (!field->setFn && !field->addrFn))
			return false;

		if (in->GetTypeId() != field->fieldType->name) {
			static const StringHash i64Name("i64");
			if (!(field->flags & FieldFlagEnum) || in->GetTypeId() != i64Name) {
				return false;
			}
		}

		SyncFieldsBeforeRead(instance, type);
		const bool written = SetFieldValue(field, instance, in);
		SyncFieldsAfterWrite(instance, type);
		return written;
	}

	bool InvokeMethod(void* instance, const TypeInfo* type, StringHash methodName, const Variant* args, u32 argCount, Variant* outReturn) {
		if (!type)
			return false;
		const MethodInfo* method = FindMethod(type, methodName);
		if (!method || !method->invokeFn)
			return false;

		if (!(method->flags & MethodFlagStatic) && !instance)
			return false;

		if (method->paramCount != argCount)
			return false;

		method->invokeFn(instance, args, argCount, outReturn);
		return true;
	}

	const AttributeInfo* FindFieldAttribute(const FieldInfo* field, StringHash key) {
		if (!field)
			return nullptr;
		for (u8 i = 0; i < field->attrCount; ++i) {
			if (field->attributes[i].key == key) {
				return &field->attributes[i];
			}
		}
		return nullptr;
	}

	const AttributeInfo* FindTypeAttribute(const TypeInfo* type, StringHash key) {
		if (!type)
			return nullptr;
		for (u32 i = 0; i < type->attributeCount; ++i) {
			if (type->attributes[i].key == key) {
				return &type->attributes[i];
			}
		}
		return nullptr;
	}

} // namespace Shalltear
