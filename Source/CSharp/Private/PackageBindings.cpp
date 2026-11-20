#include "PackageBindings.h"

#include "ManagedBehavior.h"
#include "ManagedTypes.h"

#include <format>
#include <stdexcept>
#include <vector>

namespace Shalltear::CSharpImpl::PackageBindings {

	namespace {

		struct Table {
			str name;
			const void* table;
			i32 size;
			u64 layoutHash;
		};

		std::vector<Table> _tables;
		std::vector<str> _slots;

	} // anonymous namespace

	void Add(std::string_view name, const void* table, i32 size, u64 layoutHash) {
		Remove(name);
		_tables.push_back(Table{str(name), table, size, layoutHash});
	}

	void Remove(std::string_view name) {
		std::erase_if(_tables, [&](const Table& held) { return held.name == name; });
	}

	const void* Find(std::string_view name, u64 layoutHash, i32 size) {
		for (const Table& held : _tables) {
			if (held.name != name) {
				continue;
			}
			if (held.layoutHash != layoutHash || held.size != size) {
				throw std::runtime_error(std::format("Shalltear.{} doesn't match this engine (engine layout {:016x} size {}, assembly layout {:016x} size {}). Rebuild the engine",
													 name, held.layoutHash, held.size, layoutHash, size));
			}
			return held.table;
		}
		throw std::runtime_error(std::format("Shalltear.{} has no bindings in this engine. Enable the package", name));
	}

	u32 GetCount() {
		return static_cast<u32>(_tables.size());
	}

	std::string_view GetName(u32 index) {
		return _tables[index].name;
	}

	i32 ReserveListenerSlot(std::string_view name) {
		for (size_t index = 0; index < _slots.size(); ++index) {
			if (_slots[index] == name) {
				return static_cast<i32>(index);
			}
		}
		if (_slots.size() >= 32) {
			throw std::runtime_error(std::format("no listener slot left for {} (32 in use)", name));
		}
		_slots.emplace_back(name);
		return static_cast<i32>(_slots.size() - 1);
	}

	intptr_t FindListener(Component& component, i32 slot) noexcept {
		if (component.GetType()->ownerModule != ManagedTypes::GetOwnerToken()) {
			return 0;
		}
		const ManagedBehavior& proxy = static_cast<const ManagedBehavior&>(component);
		return (proxy.GetManagedType().listenerMask >> slot) & 1u ? proxy.GetInstance() : 0;
	}

} // namespace Shalltear::CSharpImpl::PackageBindings
