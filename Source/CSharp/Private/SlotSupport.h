#pragma once

#include "BindingSupport.h"
#include "EventBridge.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Shalltear::ScriptFunctionDetail {

	template <>
	struct Slot<Vec2I> {
		static constexpr std::string_view TypeName = "Vec2I";
		static constexpr size_t SLOT_COUNT = 1;
		static Vec2I Read(const u64* slots) { return Vec2I(static_cast<i32>(static_cast<u32>(slots[0])), static_cast<i32>(static_cast<u32>(slots[0] >> 32))); }
		static void Write(u64* slots, const Vec2I& value) { slots[0] = static_cast<u64>(static_cast<u32>(value.x)) | (static_cast<u64>(static_cast<u32>(value.y)) << 32); }
	};

} // namespace Shalltear::ScriptFunctionDetail

namespace Shalltear::CSharpImpl::Slots {

	// valid until 8 more KeepBytes calls on this thread
	SHALLTEAR_CSHARP_API void* KeepBytes(size_t size);

	// valid until 16 more KeepText calls on this thread
	SHALLTEAR_CSHARP_API SltString KeepText(std::string_view text);

	inline f32 ToF32(u64 slot) noexcept {
		return std::bit_cast<f32>(static_cast<u32>(slot));
	}
	inline f64 ToF64(u64 slot) noexcept {
		return std::bit_cast<f64>(slot);
	}
	inline u64 OfF32(f32 value) noexcept {
		return std::bit_cast<u32>(value);
	}
	inline u64 OfF64(f64 value) noexcept {
		return std::bit_cast<u64>(value);
	}

	template <typename T>
	T Read(const u64* slots) {
		return ScriptFunctionDetail::Slot<T>::Read(slots);
	}

	template <typename T>
	void Write(u64* slots, const T& value) {
		ScriptFunctionDetail::Slot<T>::Write(slots, value);
	}

	inline std::string_view View(const u64* slots) {
		return ScriptFunctionDetail::Slot<std::string_view>::Read(slots);
	}

	inline void Text(u64* slots, SltString text) {
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(text.ptr));
		slots[1] = static_cast<u64>(text.length);
	}
	inline void Text(u64* slots, std::string_view text) {
		Text(slots, KeepText(text));
	}
	inline void Text(u64* slots, const char* text) {
		Text(slots, KeepText(text ? std::string_view(text) : std::string_view()));
	}
	template <std::same_as<Path> T>
	void Text(u64* slots, const T& path) {
		Text(slots, KeepText(path.ToString()));
	}

	template <typename T>
	T Struct(u64 slot) {
		T value;
		std::memcpy(static_cast<void*>(&value), reinterpret_cast<const void*>(static_cast<uintptr_t>(slot)), sizeof(T));
		return value;
	}

	template <typename T>
	void StructOut(u64 slot, const T& value) {
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(slot)), static_cast<const void*>(&value), sizeof(T));
	}

	template <typename T>
	u64 Pointer(const T* value) {
		return static_cast<u64>(reinterpret_cast<uintptr_t>(value));
	}
	template <typename T>
		requires(!std::is_pointer_v<T>)
	u64 Pointer(const T& value) {
		return static_cast<u64>(reinterpret_cast<uintptr_t>(&value));
	}

	template <typename T>
	u64 Pointer(const Handle<T>& value) {
		return Pointer(value.Get());
	}

	template <typename T>
	u64 Box(const T& value) {
		return static_cast<u64>(reinterpret_cast<uintptr_t>(new T(value)));
	}
	template <typename T>
	u64 Box(T&& value) {
		return static_cast<u64>(reinterpret_cast<uintptr_t>(new T(std::move(value))));
	}

	template <typename T, typename Range>
	void KeepList(u64* slots, const Range& range) {
		const size_t count = static_cast<size_t>(std::distance(std::begin(range), std::end(range)));
		char* kept = static_cast<char*>(KeepBytes(count * sizeof(T)));
		size_t at = 0;
		for (const auto& item : range) {
			const T copy = static_cast<T>(item);
			std::memcpy(kept + at * sizeof(T), static_cast<const void*>(&copy), sizeof(T));
			++at;
		}
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(kept));
		slots[1] = count;
	}

	inline u64 IdOfItem(const Object* object) noexcept {
		return object ? object->GetInstanceId() : 0;
	}
	inline u64 IdOfItem(const Object& object) noexcept {
		return object.GetInstanceId();
	}
	template <typename T>
	u64 IdOfItem(const Handle<T>& handle) noexcept {
		return handle.IsValid() ? handle.GetInstanceId() : 0;
	}

	template <typename Range>
	void KeepObjects(u64* slots, const Range& range) {
		const size_t count = static_cast<size_t>(std::distance(std::begin(range), std::end(range)));
		u64* kept = static_cast<u64*>(KeepBytes(count * sizeof(u64)));
		size_t at = 0;
		for (const auto& item : range) {
			kept[at++] = IdOfItem(item);
		}
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(kept));
		slots[1] = count;
	}

	template <typename Range>
	void KeepPointers(u64* slots, const Range& range) {
		const size_t count = static_cast<size_t>(std::distance(std::begin(range), std::end(range)));
		u64* kept = static_cast<u64*>(KeepBytes(count * sizeof(u64)));
		size_t at = 0;
		for (const auto& item : range) {
			kept[at++] = Pointer(item);
		}
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(kept));
		slots[1] = count;
	}

	template <typename T, typename Range>
	void KeepBoxes(u64* slots, const Range& range) {
		const size_t count = static_cast<size_t>(std::distance(std::begin(range), std::end(range)));
		u64* kept = static_cast<u64*>(KeepBytes(count * sizeof(u64)));
		size_t at = 0;
		for (const auto& item : range) {
			kept[at++] = Box<T>(static_cast<const T&>(item));
		}
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(kept));
		slots[1] = count;
	}

	template <typename T>
	decltype(auto) TextOf(const T& item) {
		if constexpr (std::same_as<T, Path>) {
			return item.ToString();
		} else if constexpr (std::is_pointer_v<T>) {
			return item ? std::string_view(item) : std::string_view();
		} else {
			return std::string_view(item);
		}
	}

	template <typename Range>
	void KeepTexts(u64* slots, const Range& range) {
		size_t count = 0;
		size_t bytes = 0;
		for (const auto& item : range) {
			bytes += TextOf(item).size();
			++count;
		}
		u64* kept = static_cast<u64*>(KeepBytes(count * 2 * sizeof(u64) + bytes));
		char* text = reinterpret_cast<char*>(kept + count * 2);
		size_t at = 0;
		for (const auto& item : range) {
			const auto held = TextOf(item);
			const std::string_view view(held);
			if (!view.empty()) {
				std::memcpy(text, view.data(), view.size());
			}
			kept[at * 2] = static_cast<u64>(reinterpret_cast<uintptr_t>(text));
			kept[at * 2 + 1] = view.size();
			text += view.size();
			++at;
		}
		slots[0] = static_cast<u64>(reinterpret_cast<uintptr_t>(kept));
		slots[1] = count;
	}

	template <typename T>
	Vector<T*> Objects(const u64* slots) {
		const u64* ids = reinterpret_cast<const u64*>(static_cast<uintptr_t>(slots[0]));
		Vector<T*> out{GetMemoryResource(MemTag::Scripting)};
		out.reserve(static_cast<size_t>(slots[1]));
		for (u64 index = 0; index < slots[1]; ++index) {
			out.push_back(ResolveObject<T>(ids[index]));
		}
		return out;
	}

	inline Vector<std::string_view> Views(const u64* slots) {
		const u64* pairs = reinterpret_cast<const u64*>(static_cast<uintptr_t>(slots[0]));
		Vector<std::string_view> out{GetMemoryResource(MemTag::Scripting)};
		out.reserve(static_cast<size_t>(slots[1]));
		for (u64 index = 0; index < slots[1]; ++index) {
			out.push_back(View(pairs + index * 2));
		}
		return out;
	}

	template <typename Range>
	Vector<Path> ToPaths(const Range& range) {
		Vector<Path> out{GetMemoryResource(MemTag::Scripting)};
		for (const auto& item : range) {
			out.emplace_back(str(item));
		}
		return out;
	}

	template <typename Array>
	Array ToArray(std::span<const typename Array::value_type> items) {
		Array out{};
		for (size_t index = 0; index < out.size() && index < items.size(); ++index) {
			out[index] = items[index];
		}
		return out;
	}

	template <typename T, typename Range>
	Vector<T> ToVector(const Range& range) {
		Vector<T> out{GetMemoryResource(MemTag::Scripting)};
		for (const auto& item : range) {
			out.emplace_back(item);
		}
		return out;
	}

	struct CallbackHold {
		explicit CallbackHold(u64 id)
			: id(id) {}
		SHALLTEAR_CSHARP_API ~CallbackHold();
		CallbackHold(const CallbackHold&) = delete;
		CallbackHold& operator=(const CallbackHold&) = delete;

		u64 id;
	};

	inline SharedPtr<CallbackHold> HoldCallback(u64 id) {
		return id != 0 ? MakeShared<CallbackHold>(id) : SharedPtr<CallbackHold>();
	}

	SHALLTEAR_CSHARP_API int32_t InvokeCallback(const CallbackHold& hold, const u64* arguments, u64* results);

	template <typename F>
	struct CallbackResult;
	template <typename R, typename... Args>
	struct CallbackResult<std::function<R(Args...)>> {
		using Type = R;
	};
	template <typename F>
	using ResultOf = typename CallbackResult<std::remove_cvref_t<F>>::Type;

	// held until 1 DropShared per KeepSharedRaw of that pointer
	SHALLTEAR_CSHARP_API u64 KeepSharedRaw(std::shared_ptr<void> held);
	SHALLTEAR_CSHARP_API std::shared_ptr<void> FindShared(const void* pointer);
	SHALLTEAR_CSHARP_API void DropShared(const void* pointer);

	template <typename T>
	u64 KeepShared(const std::shared_ptr<T>& held) {
		return KeepSharedRaw(std::static_pointer_cast<void>(std::const_pointer_cast<std::remove_const_t<T>>(held)));
	}

	// low bit set = owned by returned pointer
	template <typename T>
	std::shared_ptr<T> Shared(u64 slot) {
		T* const pointer = reinterpret_cast<T*>(static_cast<uintptr_t>(slot & ~u64{1}));
		if (!pointer) {
			return nullptr;
		}
		if (std::shared_ptr<void> kept = FindShared(pointer)) {
			return std::static_pointer_cast<T>(std::move(kept));
		}
		if (slot & 1) {
			return std::shared_ptr<T>(pointer);
		}
		return std::shared_ptr<T>(pointer, [](T*) {});
	}

	template <typename Optional, typename T>
	bool Take(Optional&& found, T& out) {
		if (!found.has_value()) {
			return false;
		}
		out = T(*std::forward<Optional>(found));
		return true;
	}

	template <typename Optional, typename T>
	bool Take(Optional&& found, T*& out) {
		if (!found.has_value()) {
			return false;
		}
		if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(*found)>>) {
			out = *found;
		} else {
			out = (*found).Get();
		}
		return true;
	}

	template <typename Pair, typename A, typename B>
	void Split(Pair&& pair, A& first, B& second) {
		first = A(std::forward<Pair>(pair).first);
		second = B(std::forward<Pair>(pair).second);
	}

	template <typename T, typename Out>
	bool TakeAt(const T* found, Out& out) {
		if (!found) {
			return false;
		}
		out = Out(*found);
		return true;
	}

	template <typename T>
	u64 Bits(const T& value) noexcept {
		if constexpr (std::is_pointer_v<T>) {
			return static_cast<u64>(reinterpret_cast<uintptr_t>(value));
		} else {
			return static_cast<u64>(value);
		}
	}

	template <typename T>
	T FromBits(u64 bits) noexcept {
		if constexpr (std::is_pointer_v<T>) {
			return reinterpret_cast<T>(static_cast<uintptr_t>(bits));
		} else {
			return static_cast<T>(bits);
		}
	}

	template <typename Container>
	decltype(auto) At(Container& container, int32_t index) {
		if (index < 0 || static_cast<size_t>(index) >= container.size()) {
			throw std::out_of_range("index is outside the list");
		}
		return container[static_cast<size_t>(index)];
	}

	template <typename T>
	T& Deref(T* pointer) {
		if (!pointer) {
			throw std::invalid_argument("C++ pointer is null");
		}
		return *pointer;
	}

	template <typename T>
	std::optional<T> Optional(std::span<const T> items) {
		return items.empty() ? std::optional<T>() : std::optional<T>(items[0]);
	}

	template <typename T>
	const T* OptionalPointer(std::span<const T> items) {
		return items.empty() ? nullptr : items.data();
	}

	template <typename Span>
	Span FixedSpan(std::span<typename Span::element_type> items) {
		if (items.size() != Span::extent) {
			throw std::length_error("list has another size than the C++ span");
		}
		return Span(items.data(), Span::extent);
	}

	template <typename Map>
	Vector<typename Map::key_type> Keys(const Map& map) {
		Vector<typename Map::key_type> keys{GetMemoryResource(MemTag::Scripting)};
		keys.reserve(map.size());
		for (const auto& [key, value] : map) {
			keys.push_back(key);
		}
		return keys;
	}

	template <typename Map>
	Vector<typename Map::mapped_type> Values(const Map& map) {
		Vector<typename Map::mapped_type> values{GetMemoryResource(MemTag::Scripting)};
		values.reserve(map.size());
		for (const auto& [key, value] : map) {
			values.push_back(value);
		}
		return values;
	}

	// items written back at end of full expression
	template <typename T>
	class SpanScope {
	public:
		explicit SpanScope(std::span<T* const> items)
			: _items(items) {
			_values.reserve(items.size());
			for (const T* item : items) {
				_values.push_back(*item);
			}
		}
		~SpanScope() {
			for (size_t index = 0; index < _items.size(); ++index) {
				*_items[index] = std::move(_values[index]);
			}
		}
		SpanScope(const SpanScope&) = delete;
		SpanScope& operator=(const SpanScope&) = delete;

		operator std::span<T>() { return _values; }
	private:
		std::span<T* const> _items;
		std::vector<T> _values;
	};

	// object written back at end of full expression
	template <typename T>
	class HandleOut {
	public:
		explicit HandleOut(T*& target)
			: _target(target) {}
		~HandleOut() { _target = _handle.Get(); }
		HandleOut(const HandleOut&) = delete;
		HandleOut& operator=(const HandleOut&) = delete;

		operator Handle<T>&() { return _handle; }
		Handle<T>* Address() { return &_handle; }
	private:
		T*& _target;
		Handle<T> _handle;
	};

	template <typename T, typename F>
	decltype(auto) WithInitializerList(std::span<const T> items, F&& call) {
		switch (items.size()) {
		case 0:
			return call(std::initializer_list<T>{});
		case 1:
			return call(std::initializer_list<T>{items[0]});
		case 2:
			return call(std::initializer_list<T>{items[0], items[1]});
		case 3:
			return call(std::initializer_list<T>{items[0], items[1], items[2]});
		case 4:
			return call(std::initializer_list<T>{items[0], items[1], items[2], items[3]});
		case 5:
			return call(std::initializer_list<T>{items[0], items[1], items[2], items[3], items[4]});
		case 6:
			return call(std::initializer_list<T>{items[0], items[1], items[2], items[3], items[4], items[5]});
		case 7:
			return call(std::initializer_list<T>{items[0], items[1], items[2], items[3], items[4], items[5], items[6]});
		case 8:
			return call(std::initializer_list<T>{items[0], items[1], items[2], items[3], items[4], items[5], items[6], items[7]});
		default:
			throw std::length_error("C++ function takes at most 8 items here");
		}
	}

	template <typename T, size_t Count>
	void Assign(T (&array)[Count], std::span<const T> items) {
		for (size_t index = 0; index < Count && index < items.size(); ++index) {
			array[index] = items[index];
		}
	}

	template <typename Container, typename T>
	Container Copies(std::span<T* const> items) {
		Container out;
		out.reserve(items.size());
		for (const T* item : items) {
			if (item) {
				out.push_back(*item);
			}
		}
		return out;
	}

	// valid until set again, object freed by C# or scripts unloaded
	SHALLTEAR_CSHARP_API std::string_view KeepFieldText(const void* object, u64 field, std::string_view text);
	SHALLTEAR_CSHARP_API std::span<const std::string_view> KeepFieldTexts(const void* object, u64 field, std::span<const std::string_view> texts);
	SHALLTEAR_CSHARP_API std::span<const std::byte> KeepFieldBytes(const void* object, u64 field, std::span<const std::byte> bytes);
	// data kept for sources also held for object
	SHALLTEAR_CSHARP_API void KeepFieldHeld(const void* object, u64 field, std::shared_ptr<const void> held, std::span<const void* const> sources);
	SHALLTEAR_CSHARP_API void ForgetFields(const void* object);
	SHALLTEAR_CSHARP_API void ForgetAllFields();
#ifdef SLT_CSHARP_TEST_HOOKS
	SHALLTEAR_CSHARP_API size_t CountKeptFields();
#endif

	template <typename T>
	std::span<const T> KeepFieldSpan(const void* object, u64 field, std::span<const T> items) {
		static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		const std::span<const std::byte> kept = KeepFieldBytes(object, field, std::as_bytes(items));
		return {reinterpret_cast<const T*>(kept.data()), items.size()};
	}

	template <typename T>
	std::span<const T> KeepFieldCopies(const void* object, u64 field, std::span<T* const> items) {
		auto copies = std::make_shared<std::vector<T>>();
		std::vector<const void*> sources;
		copies->reserve(items.size());
		for (T* const item : items) {
			copies->push_back(item ? *item : T{});
			sources.push_back(item);
		}
		const std::span<const T> kept(copies->data(), copies->size());
		KeepFieldHeld(object, field, std::move(copies), sources);
		return kept;
	}

	template <typename T>
	std::span<const Handle<T>> KeepFieldHandles(const void* object, u64 field, const Vector<T*>& items) {
		auto handles = std::make_shared<std::vector<Handle<T>>>();
		handles->reserve(items.size());
		for (T* const item : items) {
			handles->push_back(Handle<T>(item));
		}
		const std::span<const Handle<T>> kept(handles->data(), handles->size());
		KeepFieldHeld(object, field, std::move(handles), {});
		return kept;
	}

	template <size_t Count>
	void AssignFieldTexts(const void* object, u64 field, const char* (&array)[Count], std::span<const std::string_view> items) {
		if (items.size() > Count) {
			throw std::length_error(std::format("field takes at most {} item(s)", Count));
		}
		const std::span<const std::string_view> kept = KeepFieldTexts(object, field, items);
		for (size_t index = 0; index < Count; ++index) {
			array[index] = index < kept.size() ? kept[index].data() : nullptr;
		}
	}

	template <typename T, size_t Count, typename Range>
	void AssignAll(T (&array)[Count], const Range& items) {
		if (std::size(items) > Count) {
			throw std::length_error(std::format("field takes at most {} item(s)", Count));
		}
		size_t index = 0;
		for (const auto& item : items) {
			if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(item)>>) {
				array[index++] = item ? T(*item) : T{};
			} else {
				array[index++] = T(item);
			}
		}
		for (; index < Count; ++index) {
			array[index] = T{};
		}
	}

} // namespace Shalltear::CSharpImpl::Slots
