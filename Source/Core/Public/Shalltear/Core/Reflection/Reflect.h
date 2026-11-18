#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Variant.h"

#include <initializer_list>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Shalltear {

	class SHALLTEAR_CORE_API ReflectBuilder {
	public:
		ReflectBuilder(StringHash name, size_t size, size_t alignment);
		~ReflectBuilder();

		void SetParent(const TypeInfo* parent);
		void SetFlags(u32 flags);
		void SetConstructor(void (*fn)(void*));
		void SetDestructor(void (*fn)(void*));
		void SetCreateFn(Object* (*fn)(MemTag));
		void SetCreateTypedFn(Object* (*fn)(const TypeInfo*, MemTag));
		// static storage only
		void SetDisplayName(const char* literal);

		void AddField(
			StringHash name,
			const char* displayName,
			const TypeInfo* fieldType,
			size_t size,
			size_t alignment,
			void (*getFn)(const void*, Variant*),
			void (*setFn)(void*, const Variant*),
			void* (*addrFn)(void*),
			u32 flags,
			std::initializer_list<AttributeInfo> attributes);

		// container must have static storage in the declaring image
		void AddContainerField(
			StringHash name,
			const char* displayName,
			size_t size,
			size_t alignment,
			void* (*addrFn)(void*),
			u32 flags,
			const ContainerInfo* container,
			std::initializer_list<AttributeInfo> attributes);

		void AddMethod(
			StringHash name,
			const char* displayName,
			const TypeInfo* returnType,
			const TypeInfo* const* paramTypes,
			u8 paramCount,
			void (*invokeFn)(void*, const Variant*, u32, Variant*),
			u32 flags);

		void AddAttribute(StringHash key, Variant value);

		const TypeInfo* Finalize();

		// null when name is taken
		const TypeInfo* FinalizeOwned(const void* ownerToken);
	private:
		const TypeInfo* FinalizeWith(const void* owner, bool owned);

		struct Impl;
		Impl* _impl;
	};

	template <typename T, typename = void>
	struct ComponentReflectTraits {
		static void Describe(ReflectBuilder&) {}
	};

	template <typename T>
	inline void FieldValueToVariant(const T& value, Variant* out) {
		if constexpr (std::is_enum_v<T>) {
			*out = static_cast<i64>(value);
		} else {
			*out = value;
		}
	}

	template <typename T>
	inline void VariantToFieldValue(const Variant* in, T& target) {
		if constexpr (std::is_enum_v<T>) {
			if (in && in->Is<i64>()) {
				target = static_cast<T>(in->As<i64>());
			}
		} else {
			if (in && in->Is<T>()) {
				target = in->As<T>();
			}
		}
	}

	inline u32 ContainerElementFlags(const TypeInfo* elementType) {
		if (!elementType) {
			return 0;
		}
		u32 flags = 0;
		if (elementType->flags & TypeFlagHandle) {
			flags |= FieldFlagHandle;
		}
		if (elementType->flags & TypeFlagEnum) {
			flags |= FieldFlagEnum;
		}
		if (!(elementType->flags & (TypeFlagHandle | TypeFlagEnum | TypeFlagPrimitive)) &&
			(elementType->fieldCount > 0 || elementType->parent)) {
			flags |= FieldFlagSubobject;
		}
		if ((elementType->flags & TypeFlagTriviallyCopyable) && !(flags & (FieldFlagHandle | FieldFlagSubobject))) {
			flags |= FieldFlagTriviallyCopyable;
		}
		return flags;
	}

	template <typename M>
	struct MethodTraitsHelper;

	template <typename Ret, typename Self, typename... Args>
	struct MethodTraitsHelper<Ret (Self::*)(Args...)> {
		using ReturnType = Ret;
		using SelfType = Self;
		using ArgsTuple = std::tuple<Args...>;
		static constexpr size_t ARITY = sizeof...(Args);
		static constexpr bool IS_CONST = false;
	};

	template <typename Ret, typename Self, typename... Args>
	struct MethodTraitsHelper<Ret (Self::*)(Args...) const> {
		using ReturnType = Ret;
		using SelfType = Self;
		using ArgsTuple = std::tuple<Args...>;
		static constexpr size_t ARITY = sizeof...(Args);
		static constexpr bool IS_CONST = true;
	};

	template <auto Method>
	struct MethodTrampoline {
		using Traits = MethodTraitsHelper<decltype(Method)>;
		using Self = typename Traits::SelfType;
		using Ret = typename Traits::ReturnType;
		using ArgsTuple = typename Traits::ArgsTuple;

		static void Invoke(void* instance, const Variant* args, u32 argCount, Variant* outReturn) {
			(void)argCount;
			InvokeImpl(instance, args, outReturn, std::make_index_sequence<Traits::ARITY>{});
		}

		template <size_t... Is>
		static void InvokeImpl(void* instance, const Variant* args, Variant* outReturn, std::index_sequence<Is...>) {
			Self* self = static_cast<Self*>(instance);
			if constexpr (std::is_void_v<Ret>) {
				(self->*Method)(args[Is].template As<std::decay_t<std::tuple_element_t<Is, ArgsTuple>>>()...);
				(void)outReturn;
			} else {
				auto result = (self->*Method)(args[Is].template As<std::decay_t<std::tuple_element_t<Is, ArgsTuple>>>()...);
				if (outReturn) {
					*outReturn = std::move(result);
				}
			}
		}
	};

} // namespace Shalltear

#define SLT_DETAIL_REFLECT_OPEN(TypeName)                               \
public:                                                                 \
	using _ReflectSelf = TypeName;                                      \
	static const ::Shalltear::TypeInfo* GetReflectType() {              \
		static const ::Shalltear::TypeInfo* info = _BuildReflectType(); \
		return info;                                                    \
	}                                                                   \
private:                                                                \
	static const ::Shalltear::TypeInfo* _BuildReflectType() {           \
		using Self = TypeName;                                          \
		::Shalltear::ReflectBuilder builder(                            \
			::Shalltear::StringHash(#TypeName),                         \
			sizeof(TypeName),                                           \
			alignof(TypeName));                                         \
		builder.SetDisplayName(#TypeName);

#define SLT_REFLECT_CLASS(TypeName)                                                                                  \
public:                                                                                                              \
	_Pragma("clang diagnostic push")                                                                                 \
		_Pragma("clang diagnostic ignored \"-Winconsistent-missing-override\"") virtual const ::Shalltear::TypeInfo* \
		GetType() const {                                                                                            \
		return GetReflectType();                                                                                     \
	}                                                                                                                \
	_Pragma("clang diagnostic pop") template <typename, typename>                                                    \
	friend struct ::Shalltear::ComponentReflectTraits;                                                               \
	SLT_DETAIL_REFLECT_OPEN(TypeName)                                                                                \
	static_assert(std::is_base_of_v<::Shalltear::Object, TypeName>,                                                  \
				  "SLT_REFLECT_CLASS requires an Object-derived type. Use SLT_REFLECT_VALUE for plain reflected value structs");

#define SLT_REFLECT_VALUE(TypeName)                                  \
	SLT_DETAIL_REFLECT_OPEN(TypeName)                                \
	static_assert(!std::is_base_of_v<::Shalltear::Object, TypeName>, \
				  "Object-derived types must use SLT_REFLECT_CLASS so GetType() reports the concrete type");

#define SLT_REFLECT_PARENT(ParentType) \
	builder.SetParent(::Shalltear::GetStaticType<ParentType>());

#define SLT_REFLECT_FIELD(FieldName, ...)                                                                \
	builder.AddField(                                                                                    \
		::Shalltear::StringHash(#FieldName),                                                             \
		#FieldName,                                                                                      \
		::Shalltear::GetStaticType<std::remove_reference_t<decltype(std::declval<Self>().FieldName)>>(), \
		sizeof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                       \
		alignof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                      \
		+[](const void* inst, ::Shalltear::Variant* out) {                                               \
			::Shalltear::FieldValueToVariant(static_cast<const Self*>(inst)->FieldName, out);            \
		},                                                                                               \
		+[](void* inst, const ::Shalltear::Variant* in) {                                                \
			::Shalltear::VariantToFieldValue(in, static_cast<Self*>(inst)->FieldName);                   \
		},                                                                                               \
		+[](void* inst) -> void* {                                                                       \
			return &static_cast<Self*>(inst)->FieldName;                                                 \
		},                                                                                               \
		::Shalltear::FieldFlagDefault,                                                                   \
		std::initializer_list<::Shalltear::AttributeInfo>{__VA_ARGS__});

#define SLT_REFLECT_SUBOBJECT(FieldName, ...)                                                            \
	builder.AddField(                                                                                    \
		::Shalltear::StringHash(#FieldName),                                                             \
		#FieldName,                                                                                      \
		::Shalltear::GetStaticType<std::remove_reference_t<decltype(std::declval<Self>().FieldName)>>(), \
		sizeof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                       \
		alignof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                      \
		+[](const void* inst, ::Shalltear::Variant* out) {                                               \
			::Shalltear::FieldValueToVariant(static_cast<const Self*>(inst)->FieldName, out);            \
		},                                                                                               \
		+[](void* inst, const ::Shalltear::Variant* in) {                                                \
			::Shalltear::VariantToFieldValue(in, static_cast<Self*>(inst)->FieldName);                   \
		},                                                                                               \
		+[](void* inst) -> void* {                                                                       \
			return &static_cast<Self*>(inst)->FieldName;                                                 \
		},                                                                                               \
		::Shalltear::FieldFlagDefault | ::Shalltear::FieldFlagSubobject,                                 \
		std::initializer_list<::Shalltear::AttributeInfo>{__VA_ARGS__});

#define SLT_REFLECT_SUBOBJECT_ARRAY(FieldName, ...)                                                      \
	builder.AddField(                                                                                    \
		::Shalltear::StringHash(#FieldName),                                                             \
		#FieldName,                                                                                      \
		::Shalltear::GetStaticType<std::remove_reference_t<decltype(std::declval<Self>().FieldName)>>(), \
		sizeof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                       \
		alignof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                      \
		+[](const void* inst, ::Shalltear::Variant* out) {                                               \
			::Shalltear::FieldValueToVariant(static_cast<const Self*>(inst)->FieldName, out);            \
		},                                                                                               \
		+[](void* inst, const ::Shalltear::Variant* in) {                                                \
			::Shalltear::VariantToFieldValue(in, static_cast<Self*>(inst)->FieldName);                   \
		},                                                                                               \
		+[](void* inst) -> void* {                                                                       \
			return &static_cast<Self*>(inst)->FieldName;                                                 \
		},                                                                                               \
		::Shalltear::FieldFlagDefault | ::Shalltear::FieldFlagSubobjectArray,                            \
		std::initializer_list<::Shalltear::AttributeInfo>{__VA_ARGS__});

#define SLT_REFLECT_ARRAY(FieldName, ...)                                                    \
	{                                                                                        \
		using SltArrayT = std::remove_reference_t<decltype(std::declval<Self>().FieldName)>; \
		using SltElemT = typename SltArrayT::value_type;                                     \
		static const ::Shalltear::ContainerInfo sltContainerInfo = []() {                    \
			::Shalltear::ContainerInfo ci{};                                                 \
			ci.elementType = ::Shalltear::GetStaticType<SltElemT>();                         \
			ci.elementFlags = ::Shalltear::ContainerElementFlags(ci.elementType);            \
			ci.elementSize = static_cast<::Shalltear::u32>(sizeof(SltElemT));                \
			ci.sizeFn = +[](const void* c) -> size_t {                                       \
				return static_cast<const SltArrayT*>(c)->size();                             \
			};                                                                               \
			ci.resizeFn = +[](void* c, size_t n) {                                           \
				static_cast<SltArrayT*>(c)->resize(n);                                       \
			};                                                                               \
			ci.atFn = +[](void* c, size_t i) -> void* {                                      \
				return &(*static_cast<SltArrayT*>(c))[i];                                    \
			};                                                                               \
			return ci;                                                                       \
		}();                                                                                 \
		builder.AddContainerField(                                                           \
			::Shalltear::StringHash(#FieldName),                                             \
			#FieldName,                                                                      \
			sizeof(SltArrayT),                                                               \
			alignof(SltArrayT),                                                              \
			+[](void* inst) -> void* {                                                       \
				return &static_cast<Self*>(inst)->FieldName;                                 \
			},                                                                               \
			::Shalltear::FieldFlagDefault,                                                   \
			&sltContainerInfo,                                                               \
			std::initializer_list<::Shalltear::AttributeInfo>{__VA_ARGS__});                 \
	}

#define SLT_REFLECT_LOCAL_INDEX(FieldName, ...)                                                          \
	builder.AddField(                                                                                    \
		::Shalltear::StringHash(#FieldName),                                                             \
		#FieldName,                                                                                      \
		::Shalltear::GetStaticType<std::remove_reference_t<decltype(std::declval<Self>().FieldName)>>(), \
		sizeof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                       \
		alignof(std::remove_reference_t<decltype(std::declval<Self>().FieldName)>),                      \
		+[](const void* inst, ::Shalltear::Variant* out) {                                               \
			::Shalltear::FieldValueToVariant(static_cast<const Self*>(inst)->FieldName, out);            \
		},                                                                                               \
		+[](void* inst, const ::Shalltear::Variant* in) {                                                \
			::Shalltear::VariantToFieldValue(in, static_cast<Self*>(inst)->FieldName);                   \
		},                                                                                               \
		+[](void* inst) -> void* {                                                                       \
			return &static_cast<Self*>(inst)->FieldName;                                                 \
		},                                                                                               \
		::Shalltear::FieldFlagDefault | ::Shalltear::FieldFlagLocalIndex,                                \
		std::initializer_list<::Shalltear::AttributeInfo>{__VA_ARGS__});

#define SLT_REFLECT_METHOD(MethodName)                                        \
	builder.AddMethod(                                                        \
		::Shalltear::StringHash(#MethodName),                                 \
		#MethodName,                                                          \
		nullptr,                                                              \
		nullptr,                                                              \
		static_cast<::Shalltear::u8>(                                         \
			::Shalltear::MethodTrampoline<&Self::MethodName>::Traits::ARITY), \
		&::Shalltear::MethodTrampoline<&Self::MethodName>::Invoke,            \
		::Shalltear::MethodTrampoline<&Self::MethodName>::Traits::IS_CONST ? ::Shalltear::MethodFlagConst : ::Shalltear::MethodFlagNone);

#define SLT_END_REFLECT()      \
	return builder.Finalize(); \
	}                          \
public:

#define SLT_REFLECT_ENUM(QualifiedEnum)                                                                       \
	template <>                                                                                               \
	struct ::Shalltear::GetTypeInfoFor<QualifiedEnum> {                                                       \
		static const ::Shalltear::TypeInfo* Get() {                                                           \
			using Self = QualifiedEnum;                                                                       \
			static_assert(std::is_enum_v<Self>,                                                               \
						  "SLT_REFLECT_ENUM requires an enum type. Use SLT_REFLECT_VALUE for value structs"); \
			static constexpr const char* SLT_ENUM_NAME = ::Shalltear::UnqualifiedTypeName(#QualifiedEnum);    \
			static const ::Shalltear::EnumConstantInfo SLT_ENUM_CONSTANTS[] = {

#define SLT_ENUM_VALUE(ConstantName)                        \
	{::Shalltear::StringHash(#ConstantName), #ConstantName, \
	 static_cast<::Shalltear::i64>(Self::ConstantName)},

#define SLT_END_ENUM()                                                          \
	}                                                                           \
	;                                                                           \
	static ::Shalltear::TypeInfo info = []() {                                  \
		::Shalltear::TypeInfo ti{};                                             \
		ti.name = ::Shalltear::StringHash(SLT_ENUM_NAME);                       \
		ti.size = sizeof(Self);                                                 \
		ti.alignment = alignof(Self);                                           \
		ti.flags = ::Shalltear::TypeFlagPrimitive | ::Shalltear::TypeFlagEnum | \
				   ::Shalltear::TypeFlagTriviallyCopyable;                      \
		ti.displayName = SLT_ENUM_NAME;                                         \
		ti.enumConstants = SLT_ENUM_CONSTANTS;                                  \
		ti.enumConstantCount =                                                  \
			static_cast<::Shalltear::u32>(sizeof(SLT_ENUM_CONSTANTS) /          \
										  sizeof(SLT_ENUM_CONSTANTS[0]));       \
		return ti;                                                              \
	}();                                                                        \
	static bool registered = []() {                                             \
		::Shalltear::TypeRegistry::Register(&info);                             \
		return true;                                                            \
	}();                                                                        \
	(void)registered;                                                           \
	return &info;                                                               \
	}                                                                           \
	}                                                                           \
	;

#define SLT_REFLECT_CONSTRUCTIBLE()             \
	builder.SetConstructor(+[](void* storage) { \
		new (storage) Self();                   \
	});                                         \
	builder.SetDestructor(+[](void* storage) {  \
		static_cast<Self*>(storage)->~Self();   \
	});

#define SLT_REFLECT_CREATABLE()                                                 \
	builder.SetConstructor(+[](void* storage) {                                 \
		new (storage) Self();                                                   \
	});                                                                         \
	builder.SetDestructor(+[](void* storage) {                                  \
		static_cast<Self*>(storage)->~Self();                                   \
	});                                                                         \
	builder.SetCreateFn(+[](::Shalltear::MemTag tag) -> ::Shalltear::Object* {  \
		void* memory = ::Shalltear::Allocate(sizeof(Self), tag, alignof(Self)); \
		return static_cast<::Shalltear::Object*>(new (memory) Self());          \
	});

#define SLT_REFLECT_COMPONENT(TypeName, ParentType)   \
	SLT_REFLECT_CLASS(TypeName)                       \
	SLT_REFLECT_PARENT(ParentType)                    \
	builder.SetFlags(::Shalltear::TypeFlagComponent); \
	SLT_REFLECT_CREATABLE()                           \
	::Shalltear::ComponentReflectTraits<Self>::Describe(builder);

#define SLT_REFLECT_COMPONENT_ABSTRACT(TypeName, ParentType) \
	SLT_REFLECT_CLASS(TypeName)                              \
	SLT_REFLECT_PARENT(ParentType)                           \
	builder.SetFlags(::Shalltear::TypeFlagComponent | ::Shalltear::TypeFlagAbstract);

#define SLT_DETAIL_CONCAT_INNER(a, b) a##b
#define SLT_DETAIL_CONCAT(a, b) SLT_DETAIL_CONCAT_INNER(a, b)
#define SLT_REGISTER_TYPE(TypeName)                                                           \
	static ::Shalltear::TypeRegistrarNode SLT_DETAIL_CONCAT(g_sltTypeRegistrar, __COUNTER__){ \
		+[]() -> const ::Shalltear::TypeInfo* { return TypeName::GetReflectType(); }};

#define SLT_RANGE(MinVal, MaxVal)                           \
	::Shalltear::AttributeInfo {                            \
		::Shalltear::StringHash("Range"),                   \
			::Shalltear::Variant(                           \
				::Shalltear::AttrRangeData{                 \
					static_cast<::Shalltear::f32>(MinVal),  \
					static_cast<::Shalltear::f32>(MaxVal)}) \
	}

#define SLT_TOOLTIP(TextLiteral)                                \
	::Shalltear::AttributeInfo {                                \
		::Shalltear::StringHash("Tooltip"),                     \
			::Shalltear::Variant(::Shalltear::str(TextLiteral)) \
	}

#define SLT_HEADER(TextLiteral)                                 \
	::Shalltear::AttributeInfo {                                \
		::Shalltear::StringHash("Header"),                      \
			::Shalltear::Variant(::Shalltear::str(TextLiteral)) \
	}

#define SLT_MUTABILITY(MutabilityClassValue)                                          \
	::Shalltear::AttributeInfo {                                                      \
		::Shalltear::StringHash("Mutability"),                                        \
			::Shalltear::Variant(static_cast<::Shalltear::i32>(MutabilityClassValue)) \
	}

#define SLT_HIDDEN                                                    \
	::Shalltear::AttributeInfo {                                      \
		::Shalltear::StringHash("Hidden"), ::Shalltear::Variant(true) \
	}

#define SLT_READONLY                                                    \
	::Shalltear::AttributeInfo {                                        \
		::Shalltear::StringHash("ReadOnly"), ::Shalltear::Variant(true) \
	}

#define SLT_TRANSIENT                                                    \
	::Shalltear::AttributeInfo {                                         \
		::Shalltear::StringHash("Transient"), ::Shalltear::Variant(true) \
	}

// semicolon separated guids
#define SLT_ASSET_REFS                                                   \
	::Shalltear::AttributeInfo {                                         \
		::Shalltear::StringHash("AssetRefs"), ::Shalltear::Variant(true) \
	}

#define SLT_SETTINGS_CATEGORY(PathLiteral) \
	builder.AddAttribute(::Shalltear::StringHash("SettingsCategory"), ::Shalltear::Variant(::Shalltear::str(PathLiteral)));
