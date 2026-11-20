#include "InputMaps.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringHash.h"

#include <format>
#include <stdexcept>
#include <vector>

namespace Shalltear::CSharpImpl::InputMaps {

	namespace {

		constexpr u32 SLOT_BITS = 16;
		constexpr u32 SLOT_MASK = (1u << SLOT_BITS) - 1;

		struct Slot {
			InputActionMap* map = nullptr;
			u32 generation = 0;
			std::vector<ActionType> types;
		};

		struct State {
			std::vector<Slot> slots;
			std::vector<u32> free;
		};

		State* _state = nullptr;

		Slot* Find(u32 id) noexcept {
			const u32 index = id & SLOT_MASK;
			if (!_state || id == 0 || index >= _state->slots.size()) {
				return nullptr;
			}
			Slot& slot = _state->slots[index];
			return slot.map && slot.generation == id >> SLOT_BITS ? &slot : nullptr;
		}

		Slot& Require(u32 id, std::string_view member) {
			if (Slot* slot = Find(id)) {
				return *slot;
			}
			throw std::runtime_error(std::format("InputActionMap.{} used after Dispose", member));
		}

		void RequireAction(const Slot& slot, u32 action, std::string_view member) {
			if (action >= slot.types.size()) {
				throw std::runtime_error(std::format("InputActionMap.{} got an action of another map. Use RegisterAction's result", member));
			}
		}

		void Release(Slot& slot) {
			Delete(slot.map);
			slot.map = nullptr;
			slot.types.clear();
		}

	} // anonymous namespace

	u32 Create() {
		if (!_state) {
			_state = New<State>(MemTag::Scripting);
		}
		u32 index = 0;
		if (!_state->free.empty()) {
			index = _state->free.back();
			_state->free.pop_back();
		} else {
			if (_state->slots.size() >= SLOT_MASK) {
				throw std::runtime_error("InputActionMap limit reached (65534 alive). Dispose unused maps");
			}
			_state->slots.emplace_back();
			index = static_cast<u32>(_state->slots.size() - 1);
		}
		Slot& slot = _state->slots[index];
		slot.map = New<InputActionMap>(MemTag::Scripting);
		slot.generation = slot.generation % SLOT_MASK + 1;
		return slot.generation << SLOT_BITS | index;
	}

	void Destroy(u32 map) {
		if (Slot* slot = Find(map)) {
			Release(*slot);
			_state->free.push_back(map & SLOT_MASK);
		}
	}

	void DestroyAll() {
		if (!_state) {
			return;
		}
		for (Slot& slot : _state->slots) {
			if (slot.map) {
				Release(slot);
			}
		}
		Delete(_state);
		_state = nullptr;
	}

	u32 RegisterAction(u32 map, std::string_view name, ActionType type) {
		Slot& slot = Require(map, "RegisterAction");
		if (type > ActionType::Axis2D) {
			throw std::runtime_error("InputActionMap.RegisterAction got an unknown ActionType");
		}
		const StringHash hash(name);
		const ActionId existing = slot.map->FindAction(hash);
		if (existing != INVALID_ACTION_ID && slot.types[existing] != type) {
			throw std::runtime_error(std::format("InputActionMap.RegisterAction got '{}' again with another type", name));
		}
		const ActionId action = slot.map->RegisterAction(hash, type);
		if (action >= slot.types.size()) {
			slot.types.resize(action + 1, type);
		}
		return action;
	}

	u32 FindAction(u32 map, std::string_view name) {
		return Require(map, "FindAction").map->FindAction(StringHash(name));
	}

	void Bind(u32 map, u32 action, u8 source, u16 code, KeyMod requiredMods, bool exactMods, f32 scale, u8 component, u8 gamepadIndex) {
		Slot& slot = Require(map, "Bind");
		RequireAction(slot, action, "Bind");
		if (source > static_cast<u8>(InputBinding::Source::GamepadAxis)) {
			throw std::runtime_error("InputActionMap.Bind got an unknown InputBinding.Source");
		}
		InputBinding binding;
		binding.source = static_cast<InputBinding::Source>(source);
		binding.code = code;
		binding.requiredMods = requiredMods;
		binding.exactMods = exactMods;
		binding.scale = scale;
		binding.component = component;
		binding.gamepadIndex = gamepadIndex;
		slot.map->Bind(action, binding);
	}

	void ClearBindings(u32 map, u32 action) {
		Slot& slot = Require(map, "ClearBindings");
		RequireAction(slot, action, "ClearBindings");
		slot.map->ClearBindings(action);
	}

	void SetEnabled(u32 map, bool enabled) {
		Slot& slot = Require(map, enabled ? "Enable" : "Disable");
		if (enabled) {
			slot.map->Enable();
		} else {
			slot.map->Disable();
		}
	}

	void Evaluate(u32 map) {
		Require(map, "Evaluate").map->Evaluate();
	}

	bool IsPressed(u32 map, u32 action) noexcept {
		const Slot* slot = Find(map);
		return slot && slot->map->IsPressed(action);
	}

	bool WasPerformed(u32 map, u32 action) noexcept {
		const Slot* slot = Find(map);
		return slot && slot->map->WasPerformed(action);
	}

	f32 GetAxis1D(u32 map, u32 action) noexcept {
		const Slot* slot = Find(map);
		return slot ? slot->map->GetAxis1D(action) : 0.0f;
	}

	Vec2 GetAxis2D(u32 map, u32 action) noexcept {
		const Slot* slot = Find(map);
		return slot ? slot->map->GetAxis2D(action) : Vec2();
	}

	i32 GetActionCount(u32 map) noexcept {
		const Slot* slot = Find(map);
		return slot ? static_cast<i32>(slot->map->GetActionCount()) : 0;
	}

#ifdef SLT_CSHARP_TEST_HOOKS
	i32 GetAliveCount() {
		i32 alive = 0;
		for (size_t index = 0; _state && index < _state->slots.size(); ++index) {
			alive += _state->slots[index].map ? 1 : 0;
		}
		return alive;
	}
#endif

} // namespace Shalltear::CSharpImpl::InputMaps
