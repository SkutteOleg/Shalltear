#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Reflection/Variant.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	struct TypeInfo;
	struct FieldInfo;
	struct MethodInfo;
	struct AttributeInfo;

	class Object;
	enum class MemTag : u8;

	enum FieldFlag : u32 {
		FieldFlagNone = 0,
		FieldFlagPublicWalkable = 1u << 0,
		FieldFlagEditorVisible = 1u << 1,
		FieldFlagReadOnly = 1u << 2,
		FieldFlagHandle = 1u << 3,
		FieldFlagSubobject = 1u << 4,
		FieldFlagSubobjectArray = 1u << 5,
		FieldFlagLocalIndex = 1u << 6,
		FieldFlagTransient = 1u << 7,
		FieldFlagEnum = 1u << 8,
		FieldFlagScriptBindable = 1u << 9,
		FieldFlagTriviallyCopyable = 1u << 10,
		FieldFlagContainer = 1u << 11,

		FieldFlagDefault = FieldFlagPublicWalkable | FieldFlagEditorVisible,
	};

	enum TypeFlag : u32 {
		TypeFlagNone = 0,
		TypeFlagComponent = 1u << 0,
		TypeFlagAsset = 1u << 1,
		TypeFlagAbstract = 1u << 2,
		TypeFlagTriviallyCopyable = 1u << 3,
		TypeFlagHandle = 1u << 4,
		TypeFlagSubobject = 1u << 5,
		TypeFlagPrimitive = 1u << 6,

		TypeFlagHeapMetadata = 1u << 7,

		TypeFlagEnum = 1u << 8,

		TypeFlagTypedCreate = 1u << 9,
	};

	enum MethodFlag : u32 {
		MethodFlagNone = 0,
		MethodFlagConst = 1u << 0,
		MethodFlagStatic = 1u << 1,
		MethodFlagScriptBindable = 1u << 2,
	};

	enum class FieldSync : u8 {
		BeforeRead,
		AfterWrite,
	};

	enum FieldWalkFlag : u32 {
		FieldWalkFlagNone = 0,
		FieldWalkFlagIncludeInherited = 1u << 0,
		FieldWalkFlagExcludeHidden = 1u << 1,
		FieldWalkFlagExcludeReadOnly = 1u << 2,
		FieldWalkFlagExcludeTransient = 1u << 3,
		FieldWalkFlagIncludeSubobjects = 1u << 4,
		FieldWalkFlagIncludeLocalIndices = 1u << 5,

		FieldWalkFlagDefault = FieldWalkFlagIncludeInherited | FieldWalkFlagIncludeLocalIndices,
	};

	struct AttributeInfo {
		StringHash key;
		Variant value;
	};

	// widened to i64 (u64 past INT64_MAX can't be represented)
	struct EnumConstantInfo {
		StringHash name;
		const char* displayName;
		i64 value;
	};

	constexpr const char* UnqualifiedTypeName(const char* qualified) {
		const char* tail = qualified;
		for (const char* c = qualified; *c != '\0'; ++c) {
			if (*c == ':') {
				tail = c + 1;
			}
		}
		return tail;
	}

	// only called through thunks instantiated in the declaring image
	struct ContainerInfo {
		const TypeInfo* elementType;
		u32 elementFlags;
		u32 elementSize;

		size_t (*sizeFn)(const void* container);
		void (*resizeFn)(void* container, size_t count);
		void* (*atFn)(void* container, size_t index);
	};

	struct FieldInfo {
		StringHash name;
		const TypeInfo* ownerType;
		const TypeInfo* fieldType;
		size_t size;
		size_t alignment;

		void (*getFn)(const void* instance, Variant* out);
		void (*setFn)(void* instance, const Variant* in);
		void* (*addrFn)(void* instance);

		const AttributeInfo* attributes;
		u8 attrCount;

		u32 flags;

		// lives with the declaring image, never copy/cache
		const ContainerInfo* container;

		// lives in the defining image, never cache
		const char* displayName;
	};

	struct MethodInfo {
		StringHash name;
		const TypeInfo* ownerType;
		const TypeInfo* returnType;
		const TypeInfo* const* paramTypes;
		u8 paramCount;
		void (*invokeFn)(void* instance, const Variant* args, u32 argCount, Variant* outReturn);
		u32 flags;

		// append last (modules compile their copy of this struct)
		const char* displayName;
	};

	struct TypeInfo {
		StringHash name;
		size_t size;
		size_t alignment;

		const TypeInfo* parent;

		u16 classId;

		u32 moduleEpoch;

		void (*constructor)(void* storage);
		void (*destructor)(void* storage);

		const FieldInfo* fields;
		u32 fieldCount;

		const MethodInfo* methods;
		u32 methodCount;

		const AttributeInfo* attributes;
		u32 attributeCount;

		u64 layoutHash;

		u32 flags;

		// called with (type, tag) when TypeFlagTypedCreate is set
		Object* (*createFn)(MemTag tag);

		const void* ownerModule;

		const char* displayName;

		const EnumConstantInfo* enumConstants;
		u32 enumConstantCount;

		void (*fieldSyncFn)(void* instance, FieldSync phase);
	};

} // namespace Shalltear
