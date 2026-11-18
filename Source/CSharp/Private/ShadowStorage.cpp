#include "ShadowStorage.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Reflect.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace Shalltear::CSharpImpl::ShadowStorage {

	namespace {

		void ConstructElements(const ElementOps& ops, u8* at, size_t count) {
			std::memset(at, 0, count * ops.size);
			for (size_t index = 0; index < count; ++index) {
				u8* element = at + index * ops.size;
				if (ops.text) {
					new (element) str();
				}
				Construct(element, ops.inits);
			}
		}

		void DestroyElements(const ElementOps& ops, u8* at, size_t count) {
			if (!ops.text && ops.inits.empty()) {
				return;
			}
			for (size_t index = 0; index < count; ++index) {
				u8* element = at + index * ops.size;
				if (ops.text) {
					std::destroy_at(reinterpret_cast<str*>(element));
				}
				Destroy(element, ops.inits);
			}
		}

		void MoveText(u8* to, u8* from) {
			str* source = reinterpret_cast<str*>(from);
			new (to) str(std::move(*source));
			std::destroy_at(source);
		}

		// a str may point into itself
		void MoveElements(const ElementOps& ops, u8* to, u8* from, size_t count) {
			std::memcpy(to, from, count * ops.size);
			for (size_t index = 0; index < count; ++index) {
				u8* target = to + index * ops.size;
				u8* source = from + index * ops.size;
				if (ops.text) {
					MoveText(target, source);
				}
				for (const ShadowInit& init : ops.inits) {
					if (!init.list) {
						MoveText(target + init.offset, source + init.offset);
					}
				}
			}
		}

		void Reserve(ShadowList& list, size_t capacity) {
			const ElementOps& ops = *list.ops;
			u8* data = static_cast<u8*>(Allocate(capacity * ops.size, MemTag::Scripting, std::max<u32>(ops.alignment, 16)));
			if (list.data) {
				MoveElements(ops, data, list.data, list.count);
				Free(list.data);
			}
			list.data = data;
			list.capacity = static_cast<u32>(capacity);
		}

		size_t ListSize(const void* container) {
			return static_cast<const ShadowList*>(container)->count;
		}

		void ListResize(void* container, size_t count) {
			Resize(*static_cast<ShadowList*>(container), count);
		}

		void* ListAt(void* container, size_t index) {
			ShadowList& list = *static_cast<ShadowList*>(container);
			return list.data + index * list.ops->size;
		}

	} // anonymous namespace

	void Construct(void* block, const std::vector<ShadowInit>& inits) {
		for (const ShadowInit& init : inits) {
			void* at = static_cast<u8*>(block) + init.offset;
			if (init.list) {
				new (at) ShadowList{nullptr, 0, 0, init.list};
			} else {
				new (at) str();
			}
		}
	}

	void Destroy(void* block, const std::vector<ShadowInit>& inits) {
		for (const ShadowInit& init : inits) {
			void* at = static_cast<u8*>(block) + init.offset;
			if (!init.list) {
				std::destroy_at(static_cast<str*>(at));
				continue;
			}
			ShadowList& list = *static_cast<ShadowList*>(at);
			if (list.data) {
				DestroyElements(*list.ops, list.data, list.count);
				Free(list.data);
			}
			list = ShadowList{nullptr, 0, 0, list.ops};
		}
	}

	void* Resize(ShadowList& list, size_t count) {
		const ElementOps& ops = *list.ops;
		if (count < list.count) {
			DestroyElements(ops, list.data + count * ops.size, list.count - count);
		} else if (count > list.count) {
			if (count > list.capacity) {
				Reserve(list, std::max<size_t>(count, static_cast<size_t>(list.capacity) * 2));
			}
			ConstructElements(ops, list.data + static_cast<size_t>(list.count) * ops.size, count - list.count);
		}
		list.count = static_cast<u32>(count);
		return list.data;
	}

	void FillContainer(ContainerInfo& container, const TypeInfo* elementType, u32 elementSize) {
		container.elementType = elementType;
		container.elementFlags = ContainerElementFlags(elementType);
		container.elementSize = elementSize;
		container.sizeFn = &ListSize;
		container.resizeFn = &ListResize;
		container.atFn = &ListAt;
	}

} // namespace Shalltear::CSharpImpl::ShadowStorage
