#include "Shalltear/Core/Reflection/Reflect.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <cstring>
#include <vector>

namespace Shalltear {

	namespace {
		const StringHash AttrHidden = StringHash("Hidden");
		const StringHash AttrReadonly = StringHash("ReadOnly");
		const StringHash AttrTransient = StringHash("Transient");

		u32 ProcessSpecialAttributes(u32 flags, std::initializer_list<AttributeInfo> attrs) {
			for (const AttributeInfo& a : attrs) {
				if (a.key == AttrHidden) {
					flags &= ~FieldFlagEditorVisible;
				} else if (a.key == AttrReadonly) {
					flags |= FieldFlagReadOnly;
				} else if (a.key == AttrTransient) {
					flags |= FieldFlagTransient;
				}
			}
			return flags;
		}
	} // anonymous namespace

	struct ReflectBuilder::Impl {
		StringHash name;
		size_t size;
		size_t alignment;
		const TypeInfo* parent = nullptr;
		u32 flags = TypeFlagNone;
		void (*constructor)(void*) = nullptr;
		void (*destructor)(void*) = nullptr;
		Object* (*createFn)(MemTag) = nullptr;
		Object* (*createTypedFn)(const TypeInfo*, MemTag) = nullptr;
		const char* displayName = nullptr;

		std::vector<FieldInfo> fields;
		std::vector<MethodInfo> methods;
		std::vector<AttributeInfo> typeAttributes;

		std::vector<std::vector<AttributeInfo>> fieldAttributes;
	};

	ReflectBuilder::ReflectBuilder(StringHash name, size_t size, size_t alignment) {
		_impl = New<Impl>(MemTag::Core);
		_impl->name = name;
		_impl->size = size;
		_impl->alignment = alignment;
	}

	ReflectBuilder::~ReflectBuilder() {
		if (_impl) {
			Delete(_impl);
			_impl = nullptr;
		}
	}

	void ReflectBuilder::SetParent(const TypeInfo* parent) {
		_impl->parent = parent;
	}

	void ReflectBuilder::SetFlags(u32 flags) {
		_impl->flags = flags;
	}

	void ReflectBuilder::SetConstructor(void (*fn)(void*)) {
		_impl->constructor = fn;
	}

	void ReflectBuilder::SetDestructor(void (*fn)(void*)) {
		_impl->destructor = fn;
	}

	void ReflectBuilder::SetCreateFn(Object* (*fn)(MemTag)) {
		_impl->createFn = fn;
	}

	void ReflectBuilder::SetCreateTypedFn(Object* (*fn)(const TypeInfo*, MemTag)) {
		_impl->createTypedFn = fn;
	}

	void ReflectBuilder::SetDisplayName(const char* literal) {
		_impl->displayName = literal;
	}

	void ReflectBuilder::AddField(
		StringHash name,
		const char* displayName,
		const TypeInfo* fieldType,
		size_t size,
		size_t alignment,
		void (*getFn)(const void*, Variant*),
		void (*setFn)(void*, const Variant*),
		void* (*addrFn)(void*),
		u32 flags,
		std::initializer_list<AttributeInfo> attributes) {

		FieldInfo info{};
		info.name = name;
		info.displayName = displayName;
		info.ownerType = nullptr;
		info.fieldType = fieldType;
		info.size = size;
		info.alignment = alignment;
		info.getFn = getFn;
		info.setFn = setFn;
		info.addrFn = addrFn;
		info.attributes = nullptr;
		info.attrCount = 0;
		info.flags = ProcessSpecialAttributes(flags, attributes);

		if (fieldType && (fieldType->flags & TypeFlagHandle)) {
			info.flags |= FieldFlagHandle;
		}

		if (fieldType && (fieldType->flags & TypeFlagEnum)) {
			info.flags |= FieldFlagEnum;
		}

		const u32 bespokeMask = FieldFlagHandle | FieldFlagSubobject | FieldFlagSubobjectArray | FieldFlagLocalIndex;
		if (fieldType && (fieldType->flags & TypeFlagTriviallyCopyable) && !(info.flags & bespokeMask)) {
			info.flags |= FieldFlagTriviallyCopyable;
		}

		std::vector<AttributeInfo> fieldAttrs;
		for (const AttributeInfo& a : attributes) {
			if (a.key == AttrHidden || a.key == AttrReadonly || a.key == AttrTransient)
				continue;
			fieldAttrs.push_back(a);
		}

		_impl->fields.push_back(std::move(info));
		_impl->fieldAttributes.push_back(std::move(fieldAttrs));
	}

	void ReflectBuilder::AddContainerField(
		StringHash name,
		const char* displayName,
		size_t size,
		size_t alignment,
		void* (*addrFn)(void*),
		u32 flags,
		const ContainerInfo* container,
		std::initializer_list<AttributeInfo> attributes) {

		FieldInfo info{};
		info.name = name;
		info.displayName = displayName;
		info.ownerType = nullptr;
		info.fieldType = nullptr;
		info.size = size;
		info.alignment = alignment;
		info.getFn = nullptr;
		info.setFn = nullptr;
		info.addrFn = addrFn;
		info.attributes = nullptr;
		info.attrCount = 0;
		info.flags = ProcessSpecialAttributes(flags, attributes) | FieldFlagContainer;
		info.container = container;

		std::vector<AttributeInfo> fieldAttrs;
		for (const AttributeInfo& a : attributes) {
			if (a.key == AttrHidden || a.key == AttrReadonly || a.key == AttrTransient)
				continue;
			fieldAttrs.push_back(a);
		}

		_impl->fields.push_back(std::move(info));
		_impl->fieldAttributes.push_back(std::move(fieldAttrs));
	}

	void ReflectBuilder::AddMethod(
		StringHash name,
		const char* displayName,
		const TypeInfo* returnType,
		const TypeInfo* const* paramTypes,
		u8 paramCount,
		void (*invokeFn)(void*, const Variant*, u32, Variant*),
		u32 flags) {

		MethodInfo info{};
		info.name = name;
		info.displayName = displayName;
		info.ownerType = nullptr;
		info.returnType = returnType;
		info.paramTypes = paramTypes;
		info.paramCount = paramCount;
		info.invokeFn = invokeFn;
		info.flags = flags;

		_impl->methods.push_back(info);
	}

	void ReflectBuilder::AddAttribute(StringHash key, Variant value) {
		_impl->typeAttributes.push_back({key, std::move(value)});
	}

	const TypeInfo* ReflectBuilder::Finalize() {
		// return address in the defining image = ownership fingerprint
		return FinalizeWith(__builtin_return_address(0), false);
	}

	const TypeInfo* ReflectBuilder::FinalizeOwned(const void* ownerToken) {
		return FinalizeWith(ownerToken, true);
	}

	const TypeInfo* ReflectBuilder::FinalizeWith(const void* owner, bool owned) {
		SLT_CORE_ASSERT_MSG(_impl != nullptr, "ReflectBuilder::Finalize called twice");

		TypeInfo* info = New<TypeInfo>(MemTag::Core);
		info->name = _impl->name;
		info->size = _impl->size;
		info->alignment = _impl->alignment;
		info->parent = _impl->parent;
		info->classId = 0;
		info->moduleEpoch = 0;
		info->constructor = _impl->constructor;
		info->destructor = _impl->destructor;
		info->createFn = _impl->createTypedFn ? reinterpret_cast<Object* (*)(MemTag)>(_impl->createTypedFn) : _impl->createFn;
		info->ownerModule = nullptr;
		info->displayName = _impl->displayName;

		info->flags = _impl->flags | TypeFlagHeapMetadata | (_impl->createTypedFn ? TypeFlagTypedCreate : 0u);
		info->fields = nullptr;
		info->fieldCount = 0;
		info->methods = nullptr;
		info->methodCount = 0;
		info->attributes = nullptr;
		info->attributeCount = 0;
		info->layoutHash = 0;

		size_t totalAttrs = 0;
		for (const auto& fa : _impl->fieldAttributes)
			totalAttrs += fa.size();

		AttributeInfo* attrPool = nullptr;
		if (totalAttrs > 0) {
			attrPool = static_cast<AttributeInfo*>(
				Allocate(sizeof(AttributeInfo) * totalAttrs, MemTag::Core, alignof(AttributeInfo)));
			size_t cursor = 0;
			for (size_t f = 0; f < _impl->fields.size(); ++f) {
				for (const AttributeInfo& a : _impl->fieldAttributes[f]) {
					new (&attrPool[cursor]) AttributeInfo{a.key, a.value};
					cursor++;
				}
			}
		}

		if (!_impl->fields.empty()) {
			FieldInfo* fieldArr = static_cast<FieldInfo*>(
				Allocate(sizeof(FieldInfo) * _impl->fields.size(), MemTag::Core, alignof(FieldInfo)));
			size_t attrCursor = 0;
			for (size_t f = 0; f < _impl->fields.size(); ++f) {
				new (&fieldArr[f]) FieldInfo(_impl->fields[f]);
				fieldArr[f].ownerType = info;
				if (!_impl->fieldAttributes[f].empty()) {
					fieldArr[f].attributes = &attrPool[attrCursor];
					fieldArr[f].attrCount = static_cast<u8>(_impl->fieldAttributes[f].size());
					attrCursor += _impl->fieldAttributes[f].size();
				} else {
					fieldArr[f].attributes = nullptr;
					fieldArr[f].attrCount = 0;
				}
			}
			info->fields = fieldArr;
			info->fieldCount = static_cast<u32>(_impl->fields.size());
		}

		if (!_impl->methods.empty()) {
			MethodInfo* methodArr = static_cast<MethodInfo*>(
				Allocate(sizeof(MethodInfo) * _impl->methods.size(), MemTag::Core, alignof(MethodInfo)));
			for (size_t m = 0; m < _impl->methods.size(); ++m) {
				new (&methodArr[m]) MethodInfo(_impl->methods[m]);
				methodArr[m].ownerType = info;
			}
			info->methods = methodArr;
			info->methodCount = static_cast<u32>(_impl->methods.size());
		}

		if (!_impl->typeAttributes.empty()) {
			AttributeInfo* typeAttrArr = static_cast<AttributeInfo*>(
				Allocate(sizeof(AttributeInfo) * _impl->typeAttributes.size(), MemTag::Core, alignof(AttributeInfo)));
			for (size_t a = 0; a < _impl->typeAttributes.size(); ++a) {
				new (&typeAttrArr[a]) AttributeInfo{_impl->typeAttributes[a].key, _impl->typeAttributes[a].value};
			}
			info->attributes = typeAttrArr;
			info->attributeCount = static_cast<u32>(_impl->typeAttributes.size());
		}

		u64 hash = info->size * 0x9E3779B97F4A7C15ULL + info->alignment * 0x1ULL;
		for (u32 i = 0; i < info->fieldCount; ++i) {
			hash ^= info->fields[i].name.GetValue();
			hash *= 0x100000001B3ULL;
			if (info->fields[i].fieldType) {
				hash ^= info->fields[i].fieldType->name.GetValue();
				hash *= 0x100000001B3ULL;
			}
		}
		info->layoutHash = hash;

		const TypeInfo* registered = owned ? TypeRegistry::RegisterOwned(info, owner) : TypeRegistry::Register(info, owner);

		// AttributeInfo holds a Variant (destructors before the bulk free)
		if (registered != info) {
			if (attrPool) {
				for (size_t i = 0; i < totalAttrs; ++i) {
					attrPool[i].~AttributeInfo();
				}
				Free(attrPool);
			}
			if (info->fields)
				Free(const_cast<FieldInfo*>(info->fields));
			if (info->methods)
				Free(const_cast<MethodInfo*>(info->methods));
			if (info->attributes) {
				for (u32 i = 0; i < info->attributeCount; ++i) {
					const_cast<AttributeInfo*>(&info->attributes[i])->~AttributeInfo();
				}
				Free(const_cast<AttributeInfo*>(info->attributes));
			}
			Delete(info);
		}

		Delete(_impl);
		_impl = nullptr;

		return registered;
	}

} // namespace Shalltear
