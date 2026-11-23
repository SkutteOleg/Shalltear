#include "SerializerInternal.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Serialization/BinarySerializer.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"

#include <charconv>
#include <format>

namespace Shalltear {
	namespace SerializerInternal {

		PrimKind ClassifyPrim(const TypeInfo* type) {
			if (!type) {
				return PrimKind::None;
			}
			static const StringHash hBool("bool");
			static const StringHash hI8("i8");
			static const StringHash hI16("i16");
			static const StringHash hI32("i32");
			static const StringHash hI64("i64");
			static const StringHash hU8("u8");
			static const StringHash hU16("u16");
			static const StringHash hU32("u32");
			static const StringHash hU64("u64");
			static const StringHash hf32("f32");
			static const StringHash hf64("f64");

			const StringHash n = type->name;
			if (n == hBool)
				return PrimKind::Bool;
			if (n == hI8)
				return PrimKind::I8;
			if (n == hI16)
				return PrimKind::I16;
			if (n == hI32)
				return PrimKind::I32;
			if (n == hI64)
				return PrimKind::I64;
			if (n == hU8)
				return PrimKind::U8;
			if (n == hU16)
				return PrimKind::U16;
			if (n == hU32)
				return PrimKind::U32;
			if (n == hU64)
				return PrimKind::U64;
			if (n == hf32)
				return PrimKind::F32;
			if (n == hf64)
				return PrimKind::F64;
			return PrimKind::None;
		}

		bool IsStrType(const TypeInfo* type) {
			static const StringHash strHash("str");
			return type && type->name == strHash;
		}

		u64 ReadEnumBits(const void* ptr, size_t size) {
			u64 raw = 0;
			std::memcpy(&raw, ptr, size > sizeof(u64) ? sizeof(u64) : size);
			return raw;
		}

		void WriteEnumBits(void* ptr, size_t size, u64 raw) {
			std::memcpy(ptr, &raw, size > sizeof(u64) ? sizeof(u64) : size);
		}

		namespace {

			bool IsGuidType(const TypeInfo* type) {
				static const StringHash guidHash("Guid");
				return type && type->name == guidHash;
			}

			struct MathPrimDesc {
				enum class Comp : u8 {
					F32,
					I32,
					U8,
				};
				Comp comp;
				u32 count;
			};

			bool ClassifyMathPrim(const TypeInfo* type, MathPrimDesc& out) {
				if (!type) {
					return false;
				}
				struct Entry {
					StringHash name;
					MathPrimDesc desc;
				};
				static const Entry entries[] = {
					{StringHash("Vec2"), {MathPrimDesc::Comp::F32, 2}},
					{StringHash("Vec3"), {MathPrimDesc::Comp::F32, 3}},
					{StringHash("Vec4"), {MathPrimDesc::Comp::F32, 4}},
					{StringHash("Quat"), {MathPrimDesc::Comp::F32, 4}},
					{StringHash("Color"), {MathPrimDesc::Comp::F32, 4}},
					{StringHash("Vec2I"), {MathPrimDesc::Comp::I32, 2}},
					{StringHash("Vec3I"), {MathPrimDesc::Comp::I32, 3}},
					{StringHash("Vec4I"), {MathPrimDesc::Comp::I32, 4}},
					{StringHash("Color32"), {MathPrimDesc::Comp::U8, 4}},
					{StringHash("RectI"), {MathPrimDesc::Comp::I32, 4}},
					{StringHash("RectF"), {MathPrimDesc::Comp::F32, 4}},
				};
				for (const Entry& entry : entries) {
					if (type->name == entry.name) {
						out = entry.desc;
						return true;
					}
				}
				return false;
			}

			JSONValue MathPrimToJSON(const MathPrimDesc& desc, const void* ptr) {
				JSONValue arr = JSONValue::MakeArray();
				for (u32 i = 0; i < desc.count; ++i) {
					switch (desc.comp) {
					case MathPrimDesc::Comp::F32:
						arr.Push(JSONValue::MakeFloat(reinterpret_cast<const f32*>(ptr)[i]));
						break;
					case MathPrimDesc::Comp::I32:
						arr.Push(JSONValue::MakeNumber(static_cast<f64>(reinterpret_cast<const i32*>(ptr)[i])));
						break;
					case MathPrimDesc::Comp::U8:
						arr.Push(JSONValue::MakeNumber(static_cast<f64>(reinterpret_cast<const u8*>(ptr)[i])));
						break;
					}
				}
				return arr;
			}

			void JSONToMathPrim(const MathPrimDesc& desc, const JSONValue& value, void* ptr) {
				if (!value.IsArray() || value.Size() != desc.count) {
					return;
				}
				for (u32 i = 0; i < desc.count; ++i) {
					const JSONValue& component = value[i];
					switch (desc.comp) {
					case MathPrimDesc::Comp::F32:
						reinterpret_cast<f32*>(ptr)[i] = static_cast<f32>(component.AsNumber());
						break;
					case MathPrimDesc::Comp::I32:
						reinterpret_cast<i32*>(ptr)[i] = static_cast<i32>(component.AsInt());
						break;
					case MathPrimDesc::Comp::U8:
						reinterpret_cast<u8*>(ptr)[i] = static_cast<u8>(component.AsInt());
						break;
					}
				}
			}

			JSONValue PrimToJSON(PrimKind kind, const void* ptr) {
				switch (kind) {
				case PrimKind::Bool:
					return JSONValue::MakeBool(*reinterpret_cast<const bool*>(ptr));
				case PrimKind::I8:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const i8*>(ptr)));
				case PrimKind::I16:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const i16*>(ptr)));
				case PrimKind::I32:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const i32*>(ptr)));
				case PrimKind::I64:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const i64*>(ptr)));
				case PrimKind::U8:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const u8*>(ptr)));
				case PrimKind::U16:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const u16*>(ptr)));
				case PrimKind::U32:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const u32*>(ptr)));
				case PrimKind::U64:
					return JSONValue::MakeNumber(static_cast<f64>(*reinterpret_cast<const u64*>(ptr)));
				case PrimKind::F32:
					return JSONValue::MakeFloat(*reinterpret_cast<const f32*>(ptr));
				case PrimKind::F64:
					return JSONValue::MakeNumber(*reinterpret_cast<const f64*>(ptr));
				default:
					return JSONValue::MakeNull();
				}
			}

			void JSONToPrim(PrimKind kind, const JSONValue& value, void* ptr) {
				switch (kind) {
				case PrimKind::Bool:
					*reinterpret_cast<bool*>(ptr) = value.AsBool();
					break;
				case PrimKind::I8:
					*reinterpret_cast<i8*>(ptr) = static_cast<i8>(value.AsInt());
					break;
				case PrimKind::I16:
					*reinterpret_cast<i16*>(ptr) = static_cast<i16>(value.AsInt());
					break;
				case PrimKind::I32:
					*reinterpret_cast<i32*>(ptr) = static_cast<i32>(value.AsInt());
					break;
				case PrimKind::I64:
					*reinterpret_cast<i64*>(ptr) = static_cast<i64>(value.AsNumber());
					break;
				case PrimKind::U8:
					*reinterpret_cast<u8*>(ptr) = static_cast<u8>(value.AsInt());
					break;
				case PrimKind::U16:
					*reinterpret_cast<u16*>(ptr) = static_cast<u16>(value.AsInt());
					break;
				case PrimKind::U32:
					*reinterpret_cast<u32*>(ptr) = static_cast<u32>(value.AsInt());
					break;
				case PrimKind::U64:
					*reinterpret_cast<u64*>(ptr) = static_cast<u64>(value.AsNumber());
					break;
				case PrimKind::F32:
					*reinterpret_cast<f32*>(ptr) = static_cast<f32>(value.AsNumber());
					break;
				case PrimKind::F64:
					*reinterpret_cast<f64*>(ptr) = value.AsNumber();
					break;
				default:
					break;
				}
			}

		} // anonymous namespace

		FieldCategory CategorizeType(const TypeInfo* type, u32 flags) {
			if (flags & FieldFlagHandle) {
				return FieldCategory::Handle;
			}
			if (flags & FieldFlagSubobject) {
				return FieldCategory::Subobject;
			}
			if (IsStrType(type)) {
				return FieldCategory::Str;
			}
			if (IsGuidType(type)) {
				return FieldCategory::Guid;
			}
			if (MathPrimDesc mathDesc{}; ClassifyMathPrim(type, mathDesc)) {
				return FieldCategory::MathPrim;
			}
			// before the trivial fallback (an enum is trivially copyable)
			if (flags & FieldFlagEnum) {
				return FieldCategory::Enum;
			}
			if (ClassifyPrim(type) != PrimKind::None) {
				return FieldCategory::Prim;
			}
			if (flags & (FieldFlagTriviallyCopyable | FieldFlagLocalIndex)) {
				return FieldCategory::Trivial;
			}
			return FieldCategory::Unsupported;
		}

		FieldCategory CategorizeField(const FieldInfo* field) {
			if (field->flags & FieldFlagContainer) {
				return FieldCategory::Container;
			}
			return CategorizeType(field->fieldType, field->flags);
		}

		ValueDesc DescribeField(const FieldInfo* field) {
			ValueDesc desc;
			desc.category = CategorizeField(field);
			desc.type = field->fieldType;
			desc.size = field->size;
			desc.container = field->container;
			desc.debugName = field->name;
			return desc;
		}

		ValueDesc DescribeElement(const ContainerInfo& container, StringHash debugName) {
			ValueDesc desc;
			desc.category = CategorizeType(container.elementType, container.elementFlags);
			desc.type = container.elementType;
			desc.size = container.elementSize;
			desc.container = nullptr;
			desc.debugName = debugName;
			return desc;
		}

		void CollectSerializedFields(const TypeInfo* type, std::vector<const FieldInfo*>& out) {
			if (!type) {
				return;
			}
			CollectSerializedFields(type->parent, out);
			for (u32 i = 0; i < type->fieldCount; ++i) {
				const FieldInfo* field = &type->fields[i];
				if (field->flags & FieldFlagTransient) {
					continue;
				}
				out.push_back(field);
			}
		}

		str HashToHex(StringHash hash) {
			return std::format("{:016x}", hash.GetValue());
		}

		bool HexToHash(std::string_view text, StringHash& out) {
			if (text.size() != 16) {
				return false;
			}
			u64 value = 0;
			auto result = std::from_chars(text.data(), text.data() + 16, value, 16);
			if (result.ec != std::errc{} || result.ptr != text.data() + 16) {
				return false;
			}
			out = StringHash(value);
			return true;
		}

		namespace {

			bool IsReservedDocumentKey(std::string_view spelling) {
				return spelling == "_type" || spelling == "_opaque" || spelling == "_opaqueBinary";
			}

			std::string_view TextKey(const char* displayName, StringHash hash, StringPool& pool) {
				std::string_view spelling = displayName ? std::string_view(displayName) : std::string_view();
				if (spelling.empty() || IsReservedDocumentKey(spelling)) {
					pool.push_back(HashToHex(hash));
				} else {
					pool.push_back(str(spelling));
				}
				return pool.back();
			}

			// spelling first, hex text is a legal identifier
			const FieldInfo* FindFieldByKey(const TypeInfo* type, std::string_view key) {
				if (const FieldInfo* field = FindField(type, StringHash(key))) {
					return field;
				}
				StringHash hashed;
				if (HexToHash(key, hashed)) {
					return FindField(type, hashed);
				}
				return nullptr;
			}

			StringHash ReadTypeKey(std::string_view key) {
				if (key.empty()) {
					return StringHash();
				}
				StringHash spelled(key);
				if (const TypeInfo* found = TypeRegistry::FindType(spelled)) {
					return found->name;
				}
				StringHash hashed;
				if (HexToHash(key, hashed)) {
					if (const TypeInfo* found = TypeRegistry::FindType(hashed)) {
						return found->name;
					}
					return hashed;
				}
				return spelled;
			}

		} // anonymous namespace

		namespace {

			JSONValue TextBuildValue(const ValueDesc& desc, const void* fieldPtr, StringPool& pool, u32 depth) {
				if (!fieldPtr) {
					return JSONValue::MakeNull();
				}
				switch (desc.category) {
				case FieldCategory::Handle: {
					u64 instanceId = *reinterpret_cast<const u64*>(fieldPtr);
					Guid guid{};
					StringHash pointee{};
					HandleResolver::ToGuid(instanceId, &guid, &pointee);

					JSONValue handle = JSONValue::MakeObject();
					handle.AddMember("_handle", JSONValue::MakeBool(true));
					const TypeInfo* pointeeInfo = pointee.GetValue() != 0 ? TypeRegistry::FindType(pointee) : nullptr;
					handle.AddMember("pointeeType",
									 JSONValue::MakeString(TextKey(pointeeInfo ? pointeeInfo->displayName : nullptr, pointee, pool)));
					pool.push_back(guid.ToString());
					handle.AddMember("assetId", JSONValue::MakeString(pool.back()));
					return handle;
				}
				case FieldCategory::Subobject:
					return TextBuildObject(fieldPtr, desc.type, pool, depth + 1);
				case FieldCategory::Str: {
					pool.push_back(*reinterpret_cast<const str*>(fieldPtr));
					return JSONValue::MakeString(pool.back());
				}
				case FieldCategory::Guid: {
					pool.push_back(reinterpret_cast<const Guid*>(fieldPtr)->ToString());
					return JSONValue::MakeString(pool.back());
				}
				case FieldCategory::MathPrim: {
					MathPrimDesc mathDesc{};
					ClassifyMathPrim(desc.type, mathDesc);
					return MathPrimToJSON(mathDesc, fieldPtr);
				}
				case FieldCategory::Prim:
					return PrimToJSON(ClassifyPrim(desc.type), fieldPtr);
				case FieldCategory::Enum: {
					u64 raw = ReadEnumBits(fieldPtr, desc.size);
					const EnumConstantInfo* constant = FindEnumConstantByValue(desc.type, raw);
					if (!constant || !constant->displayName) {
						return JSONValue::MakeNumber(static_cast<f64>(raw));
					}
					pool.push_back(str(constant->displayName));
					return JSONValue::MakeString(pool.back());
				}
				case FieldCategory::Container: {
					JSONValue array = JSONValue::MakeArray();
					const ContainerInfo* container = desc.container;
					if (!container || !container->sizeFn || !container->atFn) {
						return array;
					}
					ValueDesc element = DescribeElement(*container, desc.debugName);
					void* mutablePtr = const_cast<void*>(fieldPtr);
					size_t count = container->sizeFn(fieldPtr);
					for (size_t i = 0; i < count; ++i) {
						array.Push(TextBuildValue(element, container->atFn(mutablePtr, i), pool, depth));
					}
					return array;
				}
				default:
					return JSONValue::MakeNull();
				}
			}

		} // anonymous namespace

		JSONValue TextBuildObject(const void* instance, const TypeInfo* type, StringPool& pool, u32 depth) {
			JSONValue obj = JSONValue::MakeObject();
			if (!type || depth > SLT_MAX_SERIALIZATION_DEPTH) {
				return obj;
			}

			obj.AddMember("_type", JSONValue::MakeString(TextKey(type->displayName, type->name, pool)));

			SyncFieldsBeforeRead(instance, type);
			std::vector<const FieldInfo*> fields;
			CollectSerializedFields(type, fields);
			for (const FieldInfo* field : fields) {
				void* fieldPtr = field->addrFn ? field->addrFn(const_cast<void*>(instance)) : nullptr;
				std::string_view key = TextKey(field->displayName, field->name, pool);
				obj.AddMember(key, TextBuildValue(DescribeField(field), fieldPtr, pool, depth));
			}
			return obj;
		}

		namespace {

			void TextReadValue(const ValueDesc& desc, void* fieldPtr, const JSONValue& value, u32 depth) {
				switch (desc.category) {
				case FieldCategory::Handle:
					if (value.IsObject() && value["_handle"].AsBool()) {
						Guid guid = Guid::FromString(value["assetId"].AsString());
						StringHash pointee = ReadTypeKey(value["pointeeType"].AsString());
						// loaders must have their scope's id map complete first
						*reinterpret_cast<u64*>(fieldPtr) = guid.IsValid() ? HandleResolver::ToHandle(guid, pointee) : 0;
					}
					break;
				case FieldCategory::Subobject:
					if (value.IsObject()) {
						TextReadObject(fieldPtr, desc.type, value, depth + 1);
					}
					break;
				case FieldCategory::Str:
					if (value.IsString()) {
						*reinterpret_cast<str*>(fieldPtr) = str(value.AsString());
					}
					break;
				case FieldCategory::Guid:
					if (value.IsString()) {
						*reinterpret_cast<Guid*>(fieldPtr) = Guid::FromString(value.AsString());
					}
					break;
				case FieldCategory::MathPrim: {
					MathPrimDesc mathDesc{};
					ClassifyMathPrim(desc.type, mathDesc);
					JSONToMathPrim(mathDesc, value, fieldPtr);
					break;
				}
				case FieldCategory::Prim:
					if (value.IsNumber() || value.IsBool()) {
						JSONToPrim(ClassifyPrim(desc.type), value, fieldPtr);
					}
					break;
				case FieldCategory::Enum:
					if (value.IsString()) {
						if (const EnumConstantInfo* constant =
								FindEnumConstantByName(desc.type, value.AsString())) {
							WriteEnumBits(fieldPtr, desc.size,
										  EnumValueToBits(desc.type, constant->value));
						}
					} else if (value.IsNumber()) {
						WriteEnumBits(fieldPtr, desc.size,
									  EnumValueToBits(desc.type, static_cast<i64>(value.AsInt())));
					}
					break;
				case FieldCategory::Container: {
					const ContainerInfo* container = desc.container;
					if (!value.IsArray() || !container || !container->resizeFn || !container->atFn) {
						break;
					}
					ValueDesc element = DescribeElement(*container, desc.debugName);
					container->resizeFn(fieldPtr, value.Size());
					for (size_t i = 0; i < value.Size(); ++i) {
						TextReadValue(element, container->atFn(fieldPtr, i), value[i], depth);
					}
					break;
				}
				default:
					break;
				}
			}

		} // anonymous namespace

		bool TextReadObject(void* instance, const TypeInfo* type, const JSONValue& obj, u32 depth) {
			if (!type || depth > SLT_MAX_SERIALIZATION_DEPTH) {
				return false;
			}

			SyncFieldsBeforeRead(instance, type);
			for (size_t i = 0; i < obj.MemberCount(); ++i) {
				std::string_view key = obj.KeyAt(i);
				if (key == "_type") {
					continue;
				}

				const FieldInfo* field = FindFieldByKey(type, key);
				if (!field || !field->addrFn) {
					continue;
				}
				if (field->flags & FieldFlagTransient) {
					continue;
				}
				TextReadValue(DescribeField(field), field->addrFn(instance), obj.ChildAt(i), depth);
			}
			SyncFieldsAfterWrite(instance, type);
			return true;
		}

		namespace {

			bool BinaryWriteValue(BinaryWriter& writer, const ValueDesc& desc, const void* fieldPtr, u32 depth) {
				switch (desc.category) {
				case FieldCategory::Handle: {
					u64 instanceId = *reinterpret_cast<const u64*>(fieldPtr);
					Guid guid{};
					StringHash pointee{};
					HandleResolver::ToGuid(instanceId, &guid, &pointee);
					if (IsAssetGuid(guid)) {
						writer.NoteAssetRef(guid);
					}
					writer.WriteU64(pointee.GetValue());
					writer.WriteU64(guid.low);
					writer.WriteU64(guid.high);
					return true;
				}
				case FieldCategory::Subobject:
					return BinaryWriteObject(writer, fieldPtr, desc.type, depth + 1);
				case FieldCategory::Str: {
					const str& value = *reinterpret_cast<const str*>(fieldPtr);
					writer.WriteU32(static_cast<u32>(value.size()));
					writer.WriteBytes(value.data(), value.size());
					return true;
				}
				case FieldCategory::Guid:
				case FieldCategory::MathPrim:
				case FieldCategory::Prim:
				case FieldCategory::Enum:
				case FieldCategory::Trivial:
					writer.WriteBytes(fieldPtr, desc.size);
					return true;
				case FieldCategory::Container: {
					const ContainerInfo* container = desc.container;
					if (!container || !container->sizeFn || !container->atFn) {
						writer.WriteU32(0);
						return true;
					}
					ValueDesc element = DescribeElement(*container, desc.debugName);
					void* mutablePtr = const_cast<void*>(fieldPtr);
					size_t count = container->sizeFn(fieldPtr);
					writer.WriteU32(static_cast<u32>(count));
					for (size_t i = 0; i < count; ++i) {
						if (!BinaryWriteValue(writer, element, container->atFn(mutablePtr, i), depth)) {
							return false;
						}
					}
					return true;
				}
				case FieldCategory::Unsupported:
				default:
					SLT_LOG_WARN("Serialization",
								 "BinarySerializer: field hash {} has an unsupported kind. Wrote empty value",
								 desc.debugName.GetValue());
					return true;
				}
			}

		} // anonymous namespace

		bool BinaryWriteObject(BinaryWriter& writer, const void* instance, const TypeInfo* type, u32 depth) {
			if (!type) {
				return false;
			}
			if (depth > SLT_MAX_SERIALIZATION_DEPTH) {
				SLT_LOG_FATAL("Serialization", "BinarySerializer: exceeded max depth {} while writing", SLT_MAX_SERIALIZATION_DEPTH);
				return false;
			}

			SyncFieldsBeforeRead(instance, type);
			std::vector<const FieldInfo*> fields;
			CollectSerializedFields(type, fields);

			writer.WriteU64(type->name.GetValue());
			writer.WriteU32(static_cast<u32>(fields.size()));

			for (const FieldInfo* field : fields) {
				writer.WriteU64(field->name.GetValue());
				writer.WriteU32(field->flags);

				size_t patch = writer.BeginPatch();
				// only read through (const_cast is safe)
				void* fieldPtr = field->addrFn ? field->addrFn(const_cast<void*>(instance)) : nullptr;
				if (fieldPtr) {
					if (!BinaryWriteValue(writer, DescribeField(field), fieldPtr, depth)) {
						return false;
					}
				}
				writer.EndPatch(patch);
			}
			return true;
		}

		namespace {

			bool BinaryReadValueInto(BinaryReader& reader, const ValueDesc& desc, void* fieldPtr, u32 depth, std::vector<HandleFixupJob>& fixups, u32 valueLen) {
				switch (desc.category) {
				case FieldCategory::Handle: {
					u64 pointee = 0;
					u64 low = 0;
					u64 high = 0;
					if (!reader.ReadU64(pointee) || !reader.ReadU64(low) || !reader.ReadU64(high)) {
						return false;
					}
					fixups.push_back(HandleFixupJob{fieldPtr, StringHash(pointee), Guid{low, high}});
					*reinterpret_cast<u64*>(fieldPtr) = 0;
					return true;
				}
				case FieldCategory::Subobject:
					return BinaryReadObject(reader, fieldPtr, desc.type, depth + 1, fixups);
				case FieldCategory::Str: {
					u32 length = 0;
					if (!reader.ReadU32(length)) {
						return false;
					}
					if (length > reader.Remaining()) {
						return false;
					}
					*reinterpret_cast<str*>(fieldPtr) = str(reinterpret_cast<const char*>(reader.CurrentPtr()), length);
					return reader.Skip(length);
				}
				case FieldCategory::Guid:
				case FieldCategory::MathPrim:
				case FieldCategory::Prim:
				case FieldCategory::Enum:
				case FieldCategory::Trivial:
					if (valueLen == desc.size) {
						return reader.ReadBytes(fieldPtr, desc.size);
					}
					return true;
				case FieldCategory::Container: {
					const ContainerInfo* container = desc.container;
					if (!container || !container->resizeFn || !container->atFn) {
						return true;
					}
					u32 count = 0;
					if (!reader.ReadU32(count)) {
						return false;
					}
					if (count > reader.Remaining()) {
						return false;
					}
					ValueDesc element = DescribeElement(*container, desc.debugName);
					container->resizeFn(fieldPtr, count);
					for (u32 i = 0; i < count; ++i) {
						if (!BinaryReadValueInto(reader, element, container->atFn(fieldPtr, i), depth, fixups,
												 static_cast<u32>(element.size))) {
							return false;
						}
					}
					return true;
				}
				case FieldCategory::Unsupported:
				default:
					return true;
				}
			}

			bool BinaryReadFields(BinaryReader& reader, void* instance, const TypeInfo* type, u32 depth, std::vector<HandleFixupJob>& fixups) {
				u64 typeNameHash = 0;
				u32 fieldCount = 0;
				if (!reader.ReadU64(typeNameHash) || !reader.ReadU32(fieldCount)) {
					return false;
				}

				for (u32 i = 0; i < fieldCount; ++i) {
					u64 nameHash = 0;
					u32 fieldFlags = 0;
					u32 valueLen = 0;
					if (!reader.ReadU64(nameHash) || !reader.ReadU32(fieldFlags) || !reader.ReadU32(valueLen)) {
						return false;
					}

					size_t valueStart = reader.Tell();
					if (valueLen > reader.Remaining()) {
						return false;
					}

					const FieldInfo* field = FindField(type, StringHash(nameHash));
					if (field && field->addrFn && !(field->flags & FieldFlagTransient)) {
						BinaryReadValueInto(reader, DescribeField(field), field->addrFn(instance), depth, fixups, valueLen);
					}

					if (!reader.Seek(valueStart + valueLen)) {
						return false;
					}
				}
				return true;
			}

		} // anonymous namespace

		bool BinaryReadObject(BinaryReader& reader, void* instance, const TypeInfo* type, u32 depth, std::vector<HandleFixupJob>& fixups) {
			if (!type) {
				return false;
			}
			if (depth > SLT_MAX_SERIALIZATION_DEPTH) {
				SLT_LOG_FATAL("Serialization", "BinarySerializer: exceeded max depth {} while reading (corrupt blob?)", SLT_MAX_SERIALIZATION_DEPTH);
				return false;
			}
			if (!type->fieldSyncFn) {
				return BinaryReadFields(reader, instance, type, depth, fixups);
			}

			const size_t pending = fixups.size();
			SyncFieldsBeforeRead(instance, type);
			const bool read = BinaryReadFields(reader, instance, type, depth, fixups);
			SyncFieldsAfterWrite(instance, type);
			if (fixups.size() > pending) {
				fixups.push_back(HandleFixupJob{instance, StringHash(), Guid{}, type});
			}
			return read;
		}

		void RunHandleFixups(const std::vector<HandleFixupJob>& fixups) {
			for (const HandleFixupJob& job : fixups) {
				if (job.writtenType) {
					SyncFieldsAfterWrite(job.fieldAddr, job.writtenType);
					continue;
				}
				if (!job.assetId.IsValid()) {
					*reinterpret_cast<u64*>(job.fieldAddr) = 0;
					continue;
				}
				*reinterpret_cast<u64*>(job.fieldAddr) = HandleResolver::ToHandle(job.assetId, job.pointeeType);
			}
		}

	} // namespace SerializerInternal
} // namespace Shalltear
