#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include <new>
#include <type_traits>
#include <utility>

namespace Shalltear {

	struct TypeInfo;

	struct VariantVTable {
		void (*copy)(void* dst, const void* src);
		void (*move)(void* dst, void* src);
		void (*destroy)(void* p);
		size_t size;
		size_t alignment;
	};

	template <typename T>
	struct VariantPrimitiveTraits;

	template <typename T>
	inline StringHash GetVariantTypeId() {
		if constexpr (requires { T::GetReflectType(); }) {
			return T::GetReflectType()->name;
		} else {
			return VariantPrimitiveTraits<std::remove_cv_t<T>>::TypeId();
		}
	}

	template <typename T>
	const VariantVTable* EnsureVariantVTable();

	class SHALLTEAR_CORE_API Variant {
	public:
		static constexpr size_t BUFFER_SIZE = 32;
		static constexpr size_t BUFFER_ALIGN = 16;

		Variant() noexcept;
		~Variant();

		Variant(const Variant& other);
		Variant& operator=(const Variant& other);

		Variant(Variant&& other) noexcept;
		Variant& operator=(Variant&& other) noexcept;

		template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Variant>>>
		Variant(T&& value) {
			_typeId = StringHash();
			_heapPtr = nullptr;
			ConstructFrom<std::decay_t<T>>(std::forward<T>(value));
		}

		template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Variant>>>
		Variant& operator=(T&& value) {
			Clear();
			ConstructFrom<std::decay_t<T>>(std::forward<T>(value));
			return *this;
		}

		bool IsEmpty() const noexcept { return _typeId.GetValue() == 0; }
		StringHash GetTypeId() const noexcept { return _typeId; }

		void Clear() noexcept;

		template <typename T>
		bool Is() const noexcept {
			return _typeId == GetVariantTypeId<std::decay_t<T>>();
		}

		template <typename T>
		T& As();

		template <typename T>
		const T& As() const;

		template <typename T>
		T* TryGet() noexcept {
			if (!Is<T>()) {
				return nullptr;
			}
			return static_cast<T*>(GetStoragePtr());
		}

		template <typename T>
		const T* TryGet() const noexcept {
			if (!Is<T>()) {
				return nullptr;
			}
			return static_cast<const T*>(GetStoragePtr());
		}

		void* GetData() noexcept;
		const void* GetData() const noexcept;

		bool IsOnHeap() const noexcept { return _heapPtr != nullptr; }

		// source must point to a typeId value
		static bool CopyFromRaw(Variant& out, StringHash typeId, const void* source);
	private:
		void* GetStoragePtr() noexcept;
		const void* GetStoragePtr() const noexcept;

		template <typename T, typename Src>
		void ConstructFrom(Src&& src) {
			const VariantVTable* vtable = EnsureVariantVTable<T>();

			void* storage = AllocateStorage(vtable->size, vtable->alignment);

			try {
				new (storage) T(std::forward<Src>(src));
			} catch (...) {
				ReleaseUnconstructedStorage();
				throw;
			}

			_typeId = GetVariantTypeId<T>();
		}

		void* AllocateStorage(size_t size, size_t alignment);

		void ReleaseUnconstructedStorage() noexcept;

		void DestroyPayload() noexcept;

		alignas(BUFFER_ALIGN) u8 _buffer[BUFFER_SIZE];
		StringHash _typeId;
		void* _heapPtr;
	};

	static_assert(sizeof(Variant) == 48, "Variant must be 48 bytes");

} // namespace Shalltear

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"

namespace Shalltear {

	template <typename T>
	const VariantVTable* EnsureVariantVTable() {
		static const VariantVTable* registered = []() -> const VariantVTable* {
			static const VariantVTable vtable = {
				[](void* dst, const void* src) {
					new (dst) T(*static_cast<const T*>(src));
				},
				[](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); },
				[](void* p) { static_cast<T*>(p)->~T(); },
				sizeof(T),
				alignof(T)};
			StringHash typeId = GetVariantTypeId<T>();
			VariantDispatch::Register(typeId, &vtable);
			return &vtable;
		}();
		return registered;
	}

	template <typename T>
	T& Variant::As() {
		SLT_CORE_ASSERT_MSG(Is<T>(), "Variant::As<T> called with type mismatch");
		return *static_cast<T*>(GetStoragePtr());
	}

	template <typename T>
	const T& Variant::As() const {
		SLT_CORE_ASSERT_MSG(Is<T>(), "Variant::As<T> called with type mismatch");
		return *static_cast<const T*>(GetStoragePtr());
	}

} // namespace Shalltear
