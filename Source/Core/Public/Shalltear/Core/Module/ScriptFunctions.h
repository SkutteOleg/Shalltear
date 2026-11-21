#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Types.h"

#include <bit>
#include <concepts>
#include <exception>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace Shalltear {

	namespace ScriptFunctions {

		// arguments + result cross as 8 byte slots (ScriptFunctionDetail::Slot per type)
		using Thunk = void (*)(u64 context, const u64* arguments, u64* result);

		struct Found {
			Thunk thunk = nullptr;
			u64 context = 0;
		};

		// name + signature copied. Owned by thunk's image when ownerToken is null
		SHALLTEAR_CORE_API bool Register(std::string_view name, std::string_view signature, Thunk thunk, u64 context = 0, const void* ownerToken = nullptr);

		SHALLTEAR_CORE_API Found FindRaw(std::string_view name, str* outSignature = nullptr);

		SHALLTEAR_CORE_API u32 GetEpoch() noexcept;

	} // namespace ScriptFunctions

	namespace ScriptFunctionDetail {

		SHALLTEAR_CORE_API void ReportThrow(const char* what) noexcept;
		SHALLTEAR_CORE_API void ReportTaken(std::string_view name) noexcept;

		// valid until this thread's next KeepText
		SHALLTEAR_CORE_API std::string_view KeepText(str&& text);

		template <typename T>
		struct Slot;

		template <>
		struct Slot<void> {
			static constexpr std::string_view TypeName = "void";
			static constexpr size_t SLOT_COUNT = 0;
		};

		template <>
		struct Slot<bool> {
			static constexpr std::string_view TypeName = "bool";
			static constexpr size_t SLOT_COUNT = 1;
			static bool Read(const u64* slots) { return slots[0] != 0; }
			static void Write(u64* slots, bool value) { slots[0] = value ? 1 : 0; }
		};

		constexpr std::string_view IntegerName(size_t size, bool isSigned) {
			switch (size) {
			case 1:
				return isSigned ? "i8" : "u8";
			case 2:
				return isSigned ? "i16" : "u16";
			case 4:
				return isSigned ? "i32" : "u32";
			default:
				return isSigned ? "i64" : "u64";
			}
		}

		template <typename T>
			requires(std::integral<T> && !std::same_as<T, bool>)
		struct Slot<T> {
			static constexpr std::string_view TypeName = IntegerName(sizeof(T), std::is_signed_v<T>);
			static constexpr size_t SLOT_COUNT = 1;
			static T Read(const u64* slots) { return static_cast<T>(slots[0]); }
			static void Write(u64* slots, T value) { slots[0] = static_cast<u64>(static_cast<i64>(value)); }
		};

		template <typename T>
			requires std::is_enum_v<T>
		struct Slot<T> {
			static constexpr std::string_view TypeName = Slot<std::underlying_type_t<T>>::TypeName;
			static constexpr size_t SLOT_COUNT = 1;
			static T Read(const u64* slots) { return static_cast<T>(Slot<std::underlying_type_t<T>>::Read(slots)); }
			static void Write(u64* slots, T value) { Slot<std::underlying_type_t<T>>::Write(slots, static_cast<std::underlying_type_t<T>>(value)); }
		};

		template <>
		struct Slot<f32> {
			static constexpr std::string_view TypeName = "f32";
			static constexpr size_t SLOT_COUNT = 1;
			static f32 Read(const u64* slots) { return std::bit_cast<f32>(static_cast<u32>(slots[0])); }
			static void Write(u64* slots, f32 value) { slots[0] = std::bit_cast<u32>(value); }
		};

		template <>
		struct Slot<f64> {
			static constexpr std::string_view TypeName = "f64";
			static constexpr size_t SLOT_COUNT = 1;
			static f64 Read(const u64* slots) { return std::bit_cast<f64>(slots[0]); }
			static void Write(u64* slots, f64 value) { slots[0] = std::bit_cast<u64>(value); }
		};

		inline u64 PackFloats(f32 low, f32 high) {
			return static_cast<u64>(std::bit_cast<u32>(low)) | (static_cast<u64>(std::bit_cast<u32>(high)) << 32);
		}

		inline f32 LowFloat(u64 slot) {
			return std::bit_cast<f32>(static_cast<u32>(slot));
		}

		inline f32 HighFloat(u64 slot) {
			return std::bit_cast<f32>(static_cast<u32>(slot >> 32));
		}

		template <>
		struct Slot<Vec2> {
			static constexpr std::string_view TypeName = "Vec2";
			static constexpr size_t SLOT_COUNT = 1;
			static Vec2 Read(const u64* slots) { return Vec2(LowFloat(slots[0]), HighFloat(slots[0])); }
			static void Write(u64* slots, const Vec2& value) { slots[0] = PackFloats(value.x, value.y); }
		};

		template <>
		struct Slot<Vec3> {
			static constexpr std::string_view TypeName = "Vec3";
			static constexpr size_t SLOT_COUNT = 2;
			static Vec3 Read(const u64* slots) { return Vec3(LowFloat(slots[0]), HighFloat(slots[0]), LowFloat(slots[1])); }
			static void Write(u64* slots, const Vec3& value) {
				slots[0] = PackFloats(value.x, value.y);
				slots[1] = PackFloats(value.z, 0.0f);
			}
		};

		template <>
		struct Slot<Vec4> {
			static constexpr std::string_view TypeName = "Vec4";
			static constexpr size_t SLOT_COUNT = 2;
			static Vec4 Read(const u64* slots) { return Vec4(LowFloat(slots[0]), HighFloat(slots[0]), LowFloat(slots[1]), HighFloat(slots[1])); }
			static void Write(u64* slots, const Vec4& value) {
				slots[0] = PackFloats(value.x, value.y);
				slots[1] = PackFloats(value.z, value.w);
			}
		};

		template <>
		struct Slot<Quat> {
			static constexpr std::string_view TypeName = "Quat";
			static constexpr size_t SLOT_COUNT = 2;
			static Quat Read(const u64* slots) { return Quat(LowFloat(slots[0]), HighFloat(slots[0]), LowFloat(slots[1]), HighFloat(slots[1])); }
			static void Write(u64* slots, const Quat& value) {
				slots[0] = PackFloats(value.x, value.y);
				slots[1] = PackFloats(value.z, value.w);
			}
		};

		template <>
		struct Slot<Color> {
			static constexpr std::string_view TypeName = "Color";
			static constexpr size_t SLOT_COUNT = 2;
			static Color Read(const u64* slots) { return Color(LowFloat(slots[0]), HighFloat(slots[0]), LowFloat(slots[1]), HighFloat(slots[1])); }
			static void Write(u64* slots, const Color& value) {
				slots[0] = PackFloats(value.r, value.g);
				slots[1] = PackFloats(value.b, value.a);
			}
		};

		template <typename T>
		struct Slot<Handle<T>> {
			static constexpr std::string_view TypeName = "object";
			static constexpr size_t SLOT_COUNT = 1;
			static Handle<T> Read(const u64* slots) {
				Object* object = ObjectRegistry::Resolve(slots[0]);
				return object && TypeRegistry::IsDerivedFrom(object->GetType(), T::GetReflectType()) ? Handle<T>(static_cast<T*>(object)) : Handle<T>();
			}
			static void Write(u64* slots, const Handle<T>& value) { slots[0] = value.GetInstanceId(); }
		};

		// UTF-8, valid during the call as an argument
		template <>
		struct Slot<std::string_view> {
			static constexpr std::string_view TypeName = "string";
			static constexpr size_t SLOT_COUNT = 2;
			static std::string_view Read(const u64* slots) {
				return slots[1] ? std::string_view(reinterpret_cast<const char*>(static_cast<uintptr_t>(slots[0])), static_cast<size_t>(slots[1])) : std::string_view();
			}
			static void Write(u64* slots, std::string_view value) {
				slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(value.data()));
				slots[1] = value.size();
			}
		};

		template <>
		struct Slot<str> {
			static constexpr std::string_view TypeName = "string";
			static constexpr size_t SLOT_COUNT = 2;
			static str Read(const u64* slots) { return str(Slot<std::string_view>::Read(slots)); }
			static void Write(u64* slots, str value) { Slot<std::string_view>::Write(slots, KeepText(std::move(value))); }
		};

		template <typename T>
		using Bare = std::remove_cvref_t<T>;

		template <typename R, typename... Args>
		str Describe() {
			str text(Slot<Bare<R>>::TypeName);
			text += '(';
			bool first = true;
			((text += first ? "" : ",", text += Slot<Bare<Args>>::TypeName, first = false), ...);
			text += ')';
			return text;
		}

		template <typename T>
		void WriteArgument(u64* slots, const T& value) {
			if constexpr (std::same_as<T, str>) {
				Slot<std::string_view>::Write(slots, value);
			} else {
				Slot<T>::Write(slots, value);
			}
		}

		template <typename T>
		Bare<T> ReadNext(const u64* slots, size_t& at) {
			const size_t from = at;
			at += Slot<Bare<T>>::SLOT_COUNT;
			return Slot<Bare<T>>::Read(slots + from);
		}

		template <typename Function>
		struct Shape;

		template <typename R, typename... Args>
		struct Shape<R (*)(Args...)> {
			using Result = R;

			static str Describe() { return ScriptFunctionDetail::Describe<R, Args...>(); }

			static std::tuple<Bare<Args>...> Read(const u64* slots) {
				size_t at = 0;
				return std::tuple<Bare<Args>...>{ReadNext<Args>(slots, at)...};
			}
		};

		template <typename R, typename... Args>
		struct Shape<R (*)(Args...) noexcept> : Shape<R (*)(Args...)> {};

		template <auto Function>
		struct Exported {
			using Form = Shape<decltype(Function)>;

			static void Call(u64, const u64* arguments, u64* result) noexcept {
				try {
					auto values = Form::Read(arguments);
					if constexpr (std::is_void_v<typename Form::Result>) {
						std::apply(Function, std::move(values));
					} else {
						Slot<Bare<typename Form::Result>>::Write(result, std::apply(Function, std::move(values)));
					}
				} catch (const std::exception& error) {
					ReportThrow(error.what());
				} catch (...) {
					ReportThrow("unknown exception");
				}
			}

			static void Register(std::string_view name) {
				if (!ScriptFunctions::Register(name, Form::Describe(), &Call)) {
					ReportTaken(name);
				}
			}
		};

	} // namespace ScriptFunctionDetail

	template <typename Signature>
	class ScriptFunction;

	template <typename R, typename... Args>
	class ScriptFunction<R(Args...)> {
	public:
		ScriptFunction() = default;
		explicit ScriptFunction(std::string_view name) : _name(name) {}

		bool IsBound() {
			Refresh();
			return _found.thunk != nullptr;
		}

		R operator()(Args... arguments) {
			Refresh();
			if (!_found.thunk) {
				return R();
			}
			u64 slots[(ScriptFunctionDetail::Slot<ScriptFunctionDetail::Bare<Args>>::SLOT_COUNT + ... + 1)] = {};
			u64 result[2] = {};
			size_t at = 0;
			((ScriptFunctionDetail::WriteArgument<ScriptFunctionDetail::Bare<Args>>(slots + at, arguments), at += ScriptFunctionDetail::Slot<ScriptFunctionDetail::Bare<Args>>::SLOT_COUNT), ...);
			_found.thunk(_found.context, slots, result);
			if constexpr (!std::is_void_v<R>) {
				return ScriptFunctionDetail::Slot<ScriptFunctionDetail::Bare<R>>::Read(result);
			}
		}
	private:
		void Refresh() {
			const u32 epoch = ScriptFunctions::GetEpoch();
			if (epoch == _epoch) {
				return;
			}
			_epoch = epoch;
			str registered;
			const ScriptFunctions::Found found = ScriptFunctions::FindRaw(_name, &registered);
			_found = registered == ScriptFunctionDetail::Describe<R, Args...>() ? found : ScriptFunctions::Found{};
		}

		str _name;
		ScriptFunctions::Found _found;
		u32 _epoch = ~0u;
	};

	namespace ScriptFunctions {

		template <typename Signature>
		ScriptFunction<Signature> Find(std::string_view name) {
			return ScriptFunction<Signature>(name);
		}

	} // namespace ScriptFunctions

} // namespace Shalltear

#define SLT_DETAIL_SCRIPT_EXPORT_NAME_INNER(a, b) a##b
#define SLT_DETAIL_SCRIPT_EXPORT_NAME(a, b) SLT_DETAIL_SCRIPT_EXPORT_NAME_INNER(a, b)

#define SLT_SCRIPT_EXPORT(Function)                                                                       \
	static ::Shalltear::TypeRegistrarNode SLT_DETAIL_SCRIPT_EXPORT_NAME(g_sltTypeRegistrar, __COUNTER__){ \
		+[]() -> const ::Shalltear::TypeInfo* {                                                           \
			::Shalltear::ScriptFunctionDetail::Exported<&Function>::Register(#Function);                  \
			return nullptr;                                                                               \
		}};
