#include "NativeFieldBindings.h"

#include "FieldKinds.h"
#include "ScriptBridge.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Variant.h"
#include "Shalltear/Core/Reflection/Walk.h"

#include <cstring>
#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::NativeFieldBindings {

	namespace {

		using ScriptFunctionDetail::Slot;

		struct Key {
			u64 type;
			u64 member;

			bool operator==(const Key&) const = default;
		};

		struct KeyHash {
			size_t operator()(const Key& key) const noexcept { return static_cast<size_t>(key.type ^ (key.member * 0x9E3779B97F4A7C15ull)); }
		};

		struct Resolved {
			FieldRef ref;
			const TypeInfo* type = nullptr;
			u32 epoch = ~0u;
		};

		HashMap<Key, Resolved, KeyHash>* _fields = nullptr;

		const TypeInfo& CheckType(Component& target, u64 type) {
			const TypeInfo* declared = TypeRegistry::FindType(StringHash(type));
			if (!declared || (target.GetType() != declared && !TypeRegistry::IsDerivedFrom(target.GetType(), declared))) {
				throw std::runtime_error("component isn't of the wrapper's C++ class. Compile the C# scripts again");
			}
			return *declared;
		}

		const FieldInfo& Resolve(Component& target, u64 type, u64 field) {
			if (!_fields) {
				_fields = New<HashMap<Key, Resolved, KeyHash>>(MemTag::Scripting, GetMemoryResource(MemTag::Scripting));
			}
			Resolved& resolved = (*_fields)[Key{type, field}];
			if (!resolved.type) {
				resolved.ref = FieldRef{StringHash(type), StringHash(field)};
			}
			const FieldInfo* info = resolved.ref.Resolve();
			if (resolved.epoch != resolved.ref.epoch || !resolved.type) {
				resolved.epoch = resolved.ref.epoch;
				resolved.type = TypeRegistry::FindType(StringHash(type));
			}
			if (!info || !resolved.type) {
				throw std::runtime_error("C++ field is gone. Compile the C# scripts again");
			}
			if (target.GetType() != resolved.type && !TypeRegistry::IsDerivedFrom(target.GetType(), resolved.type)) {
				throw std::runtime_error("component isn't of the wrapper's C++ class. Compile the C# scripts again");
			}
			return *info;
		}

		void CheckKind(const FieldInfo& info, i32 kind) {
			bool matches = false;
			if (kind == SLT_KIND_OBJECT) {
				matches = info.fieldType && (info.fieldType->flags & TypeFlagHandle) != 0;
			} else if (kind == SLT_KIND_ENUM) {
				matches = info.fieldType && (info.fieldType->flags & TypeFlagEnum) != 0 && info.size <= sizeof(i64);
			} else if (kind == SLT_KIND_STRING) {
				matches = info.fieldType == FieldKinds::GetText().type;
			} else if (const FieldKindInfo* expected = FieldKinds::Find(kind)) {
				matches = info.fieldType == expected->type;
			}
			if (!matches || info.container) {
				throw std::runtime_error("C++ field changed its type. Compile the C# scripts again");
			}
		}

		bool IsSigned(const TypeInfo& type) {
			for (u32 index = 0; index < type.enumConstantCount; ++index) {
				if (type.enumConstants[index].value < 0) {
					return true;
				}
			}
			return false;
		}

		template <typename T>
		void ReadAs(const void* at, std::span<u64> slots) {
			Slot<T>::Write(slots.data(), *static_cast<const T*>(at));
		}

		template <typename T>
		void WriteAs(void* at, std::span<const u64> slots) {
			*static_cast<T*>(at) = Slot<T>::Read(slots.data());
		}

	} // anonymous namespace

	void Read(Component& target, u64 type, u64 field, i32 kind, std::span<u64> slots) {
		const FieldInfo& info = Resolve(target, type, field);
		CheckKind(info, kind);
		if (slots.size() < 2) {
			throw std::runtime_error("field read needs 2 slots");
		}
		const void* at = info.addrFn(static_cast<Object*>(&target));
		switch (kind) {
		case SLT_KIND_BOOL:
			return ReadAs<bool>(at, slots);
		case SLT_KIND_I8:
			return ReadAs<i8>(at, slots);
		case SLT_KIND_U8:
			return ReadAs<u8>(at, slots);
		case SLT_KIND_I16:
			return ReadAs<i16>(at, slots);
		case SLT_KIND_U16:
			return ReadAs<u16>(at, slots);
		case SLT_KIND_I32:
			return ReadAs<i32>(at, slots);
		case SLT_KIND_U32:
			return ReadAs<u32>(at, slots);
		case SLT_KIND_I64:
			return ReadAs<i64>(at, slots);
		case SLT_KIND_U64:
			return ReadAs<u64>(at, slots);
		case SLT_KIND_F32:
			return ReadAs<f32>(at, slots);
		case SLT_KIND_F64:
			return ReadAs<f64>(at, slots);
		case SLT_KIND_VEC2:
			return ReadAs<Vec2>(at, slots);
		case SLT_KIND_VEC3:
			return ReadAs<Vec3>(at, slots);
		case SLT_KIND_VEC4:
			return ReadAs<Vec4>(at, slots);
		case SLT_KIND_QUAT:
			return ReadAs<Quat>(at, slots);
		case SLT_KIND_COLOR:
			return ReadAs<Color>(at, slots);
		case SLT_KIND_ENUM: {
			u64 value = 0;
			std::memcpy(&value, at, info.size);
			const u32 bits = static_cast<u32>(info.size) * 8;
			if (bits < 64 && (value >> (bits - 1)) != 0 && IsSigned(*info.fieldType)) {
				value |= ~0ull << bits;
			}
			slots[0] = value;
			return;
		}
		default:
			slots[0] = static_cast<const Handle<Object>*>(at)->GetInstanceId();
			return;
		}
	}

	void Write(Component& target, u64 type, u64 field, i32 kind, std::span<const u64> slots) {
		const FieldInfo& info = Resolve(target, type, field);
		CheckKind(info, kind);
		if (slots.size() < 2) {
			throw std::runtime_error("field write needs 2 slots");
		}
		if (info.flags & FieldFlagReadOnly) {
			throw std::runtime_error("C++ field is read-only");
		}
		void* at = info.addrFn(static_cast<Object*>(&target));
		switch (kind) {
		case SLT_KIND_BOOL:
			return WriteAs<bool>(at, slots);
		case SLT_KIND_I8:
			return WriteAs<i8>(at, slots);
		case SLT_KIND_U8:
			return WriteAs<u8>(at, slots);
		case SLT_KIND_I16:
			return WriteAs<i16>(at, slots);
		case SLT_KIND_U16:
			return WriteAs<u16>(at, slots);
		case SLT_KIND_I32:
			return WriteAs<i32>(at, slots);
		case SLT_KIND_U32:
			return WriteAs<u32>(at, slots);
		case SLT_KIND_I64:
			return WriteAs<i64>(at, slots);
		case SLT_KIND_U64:
			return WriteAs<u64>(at, slots);
		case SLT_KIND_F32:
			return WriteAs<f32>(at, slots);
		case SLT_KIND_F64:
			return WriteAs<f64>(at, slots);
		case SLT_KIND_VEC2:
			return WriteAs<Vec2>(at, slots);
		case SLT_KIND_VEC3:
			return WriteAs<Vec3>(at, slots);
		case SLT_KIND_VEC4:
			return WriteAs<Vec4>(at, slots);
		case SLT_KIND_QUAT:
			return WriteAs<Quat>(at, slots);
		case SLT_KIND_COLOR:
			return WriteAs<Color>(at, slots);
		case SLT_KIND_ENUM: {
			const i64 value = static_cast<i64>(slots[0]);
			std::memcpy(at, &value, info.size);
			return;
		}
		default: {
			Object* pointee = ObjectRegistry::Resolve(slots[0]);
			const TypeInfo* wanted = TypeRegistry::FindType(HandlePointeeName(*info.fieldType));
			if (pointee && wanted && !TypeRegistry::IsDerivedFrom(pointee->GetType(), wanted) && pointee->GetType() != wanted) {
				throw std::runtime_error("object isn't of the C++ field's class");
			}
			*static_cast<Handle<Object>*>(at) = Handle<Object>(pointee);
			return;
		}
		}
	}

	std::string_view ReadText(Component& target, u64 type, u64 field) {
		const FieldInfo& info = Resolve(target, type, field);
		CheckKind(info, SLT_KIND_STRING);
		return *static_cast<const str*>(info.addrFn(static_cast<Object*>(&target)));
	}

	void WriteText(Component& target, u64 type, u64 field, std::string_view value) {
		const FieldInfo& info = Resolve(target, type, field);
		CheckKind(info, SLT_KIND_STRING);
		if (info.flags & FieldFlagReadOnly) {
			throw std::runtime_error("C++ field is read-only");
		}
		static_cast<str*>(info.addrFn(static_cast<Object*>(&target)))->assign(value);
	}

	void Invoke(Component& target, u64 type, u64 method) {
		const TypeInfo& declared = CheckType(target, type);
		const MethodInfo* found = FindMethod(&declared, StringHash(method));
		if (!found || found->paramCount != 0) {
			throw std::runtime_error("C++ method is gone or takes arguments. Compile the C# scripts again");
		}
		Variant ignored;
		found->invokeFn(static_cast<Object*>(&target), nullptr, 0, &ignored);
	}

	void Reset() {
		if (_fields) {
			Delete(_fields);
			_fields = nullptr;
		}
	}

} // namespace Shalltear::CSharpImpl::NativeFieldBindings
