#include "Shalltear/Core/Reflection/Variant.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"

namespace Shalltear {

	Variant::Variant() noexcept
		: _typeId(), _heapPtr(nullptr) {
	}

	Variant::~Variant() {
		DestroyPayload();
	}

	Variant::Variant(const Variant& other)
		: _typeId(), _heapPtr(nullptr) {
		if (other.IsEmpty())
			return;

		const VariantVTable* vtable = VariantDispatch::Lookup(other._typeId);
		SLT_CORE_ASSERT_MSG(vtable != nullptr,
							"Variant copy failed: type '{}' isn't registered in VariantDispatch. "
							"Owning DLL may have unloaded without UnregisterModule",
							other._typeId.GetValue());
		if (!vtable)
			return;

		void* storage = AllocateStorage(vtable->size, vtable->alignment);
		vtable->copy(storage, other.GetStoragePtr());
		_typeId = other._typeId;
	}

	Variant& Variant::operator=(const Variant& other) {
		if (this == &other)
			return *this;
		Clear();

		if (other.IsEmpty())
			return *this;

		const VariantVTable* vtable = VariantDispatch::Lookup(other._typeId);
		SLT_CORE_ASSERT_MSG(vtable != nullptr, "Variant copy-assignment: unregistered type");
		if (!vtable)
			return *this;

		void* storage = AllocateStorage(vtable->size, vtable->alignment);
		vtable->copy(storage, other.GetStoragePtr());
		_typeId = other._typeId;
		return *this;
	}

	Variant::Variant(Variant&& other) noexcept
		: _typeId(), _heapPtr(nullptr) {
		if (other.IsEmpty())
			return;

		if (other.IsOnHeap()) {
			_typeId = other._typeId;
			_heapPtr = other._heapPtr;
			other._typeId = StringHash();
			other._heapPtr = nullptr;
			return;
		}

		const VariantVTable* vtable = VariantDispatch::Lookup(other._typeId);
		SLT_CORE_ASSERT_MSG(vtable != nullptr, "Variant move: unregistered type");
		if (!vtable)
			return;

		vtable->move(_buffer, other._buffer);
		_typeId = other._typeId;

		vtable->destroy(other._buffer);
		other._typeId = StringHash();
	}

	Variant& Variant::operator=(Variant&& other) noexcept {
		if (this == &other)
			return *this;
		Clear();

		if (other.IsEmpty())
			return *this;

		if (other.IsOnHeap()) {
			_typeId = other._typeId;
			_heapPtr = other._heapPtr;
			other._typeId = StringHash();
			other._heapPtr = nullptr;
			return *this;
		}

		const VariantVTable* vtable = VariantDispatch::Lookup(other._typeId);
		SLT_CORE_ASSERT_MSG(vtable != nullptr, "Variant move-assignment: unregistered type");
		if (!vtable)
			return *this;

		vtable->move(_buffer, other._buffer);
		_typeId = other._typeId;

		vtable->destroy(other._buffer);
		other._typeId = StringHash();
		return *this;
	}

	void Variant::Clear() noexcept {
		DestroyPayload();
	}

	bool Variant::CopyFromRaw(Variant& out, StringHash typeId, const void* source) {
		const VariantVTable* vtable = VariantDispatch::Lookup(typeId);
		if (!vtable || !source) {
			return false;
		}
		out.Clear();
		void* storage = out.AllocateStorage(vtable->size, vtable->alignment);
		vtable->copy(storage, source);
		out._typeId = typeId;
		return true;
	}

	void* Variant::GetData() noexcept {
		return IsEmpty() ? nullptr : GetStoragePtr();
	}

	const void* Variant::GetData() const noexcept {
		return IsEmpty() ? nullptr : GetStoragePtr();
	}

	void* Variant::GetStoragePtr() noexcept {
		return IsOnHeap() ? _heapPtr : static_cast<void*>(_buffer);
	}

	const void* Variant::GetStoragePtr() const noexcept {
		return IsOnHeap() ? _heapPtr : static_cast<const void*>(_buffer);
	}

	void* Variant::AllocateStorage(size_t size, size_t alignment) {
		if (size <= BUFFER_SIZE && alignment <= BUFFER_ALIGN) {
			_heapPtr = nullptr;
			return _buffer;
		}

		_heapPtr = Shalltear::Allocate(size, MemTag::Core, alignment);
		return _heapPtr;
	}

	void Variant::ReleaseUnconstructedStorage() noexcept {
		if (_heapPtr) {
			Shalltear::Free(_heapPtr);
			_heapPtr = nullptr;
		}
		_typeId = StringHash();
	}

	void Variant::DestroyPayload() noexcept {
		if (IsEmpty())
			return;

		const VariantVTable* vtable = VariantDispatch::Lookup(_typeId);
		SLT_CORE_ASSERT_MSG(vtable != nullptr,
							"Variant::Clear: type '{}' is no longer registered. "
							"The owning DLL was likely unloaded without proper cleanup.",
							_typeId.GetValue());

		if (vtable) {
			vtable->destroy(GetStoragePtr());
		}

		if (IsOnHeap()) {
			Shalltear::Free(_heapPtr);
			_heapPtr = nullptr;
		}

		_typeId = StringHash();
	}

} // namespace Shalltear
