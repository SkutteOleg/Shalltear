#include "Shalltear/Editor/UndoSystem.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"

#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSceneOps.h"

namespace Shalltear {

	namespace {

		constexpr size_t STACK_CAP = 256;

		struct CommandEntry {
			str label;
			Vector<UniquePtr<IEditorCommand>> commands{GetMemoryResource(MemTag::Editor)};
		};

		class UndoState : public Object {
		public:
			Vector<CommandEntry> undoStack{GetMemoryResource(MemTag::Editor)};
			Vector<CommandEntry> redoStack{GetMemoryResource(MemTag::Editor)};

			CommandEntry openGroup;
			i32 groupDepth = 0;

			PlayModeState lastPlayState = PlayModeState::Editing;

			void OnPlayModeChanged(const EditorPlayModeChangedEvent& event) {
				bool worldSwapped = (lastPlayState == PlayModeState::Editing) != (event.state == PlayModeState::Editing);
				lastPlayState = event.state;
				if (worldSwapped) {
					UndoSystem::Clear();
				}
			}

			void OnModuleReloadBegan(const EditorModuleReloadBeganEvent& event) {
				// module typed Variants must not outlive their image
				if (!TypeRegistry::IsOwnerToken(event.moduleBase)) {
					UndoSystem::Clear();
				}
			}
		};

		UndoState* _state = nullptr;

		void TrimToCap(Vector<CommandEntry>& stack) {
			while (stack.size() > STACK_CAP) {
				stack.erase(stack.begin());
			}
		}

		void MarkCommandSceneDirty(const IEditorCommand& command) {
			Handle<Scene> scene = command.GetAffectedScene();
			if (scene.IsValid()) {
				EditorSceneOps::MarkDirty(scene);
			}
		}

	} // anonymous namespace

	PropertySetCommand::PropertySetCommand(EditTarget target, StringHash field, Variant before, Variant after, std::string_view label)
		: _target(target), _field(field), _before(std::move(before)), _after(std::move(after)), _label(label) {}

	void PropertySetCommand::Write(const Variant& value) {
		Object* object = _target.Resolve();
		if (!object) {
			SLT_LOG_WARN("Editor", "undo command target no longer exists. Skipping");
			return;
		}
		const TypeInfo* type = object->GetType();

		if (TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
			SceneObject* sceneObject = static_cast<SceneObject*>(object);
			if (_field == "_localPosition"_hash && value.Is<Vec3>()) {
				sceneObject->SetLocalPosition(value.As<Vec3>());
				_target.NotifyWritten();
				return;
			}
			if (_field == "_localRotation"_hash && value.Is<Quat>()) {
				sceneObject->SetLocalRotation(value.As<Quat>());
				_target.NotifyWritten();
				return;
			}
			if (_field == "_localScale"_hash && value.Is<Vec3>()) {
				sceneObject->SetLocalScale(value.As<Vec3>());
				_target.NotifyWritten();
				return;
			}
		}

		if (!SetField(object, type, _field, &value)) {
			SLT_LOG_WARN("Editor", "undo command couldn't set field {:#x}", _field.GetValue());
			return;
		}
		_target.NotifyWritten();
	}

	void PropertySetCommand::Apply() {
		Write(_after);
	}

	void PropertySetCommand::Revert() {
		Write(_before);
	}

	str PropertySetCommand::GetLabel() const {
		return _label.empty() ? str("Edit Property") : _label;
	}

	Handle<Scene> PropertySetCommand::GetAffectedScene() const {
		Object* object = _target.Resolve();
		if (!object) {
			return {};
		}
		const TypeInfo* type = object->GetType();
		if (TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
			return static_cast<SceneObject*>(object)->GetScene();
		}
		if (TypeRegistry::IsDerivedFrom(type, Component::GetReflectType())) {
			return static_cast<Component*>(object)->GetScene();
		}
		return {};
	}

	bool PropertySetCommand::TryCoalesce(const IEditorCommand& next) {
		const PropertySetCommand* incoming = next.AsPropertySet();
		if (!incoming || !(incoming->_target == _target) || incoming->_field != _field) {
			return false;
		}
		_after = incoming->_after;
		return true;
	}

	void UndoSystem::Initialize() {
		if (_state) {
			return;
		}
		_state = New<UndoState>(MemTag::Editor);

		Handle<UndoState> self(_state);
		EventBus& bus = EventBus::GetGlobal();
		EditorEvents::PlayModeChanged.Subscribe(bus, self, &UndoState::OnPlayModeChanged);
		EditorEvents::ModuleReloadBegan.Subscribe(bus, self, &UndoState::OnModuleReloadBegan);
	}

	void UndoSystem::Terminate() {
		if (!_state) {
			return;
		}
		EventBus::GetGlobal().UnsubscribeAll(_state->GetInstanceId());
		Delete(_state);
		_state = nullptr;
	}

	void UndoSystem::Push(UniquePtr<IEditorCommand> command) {
		if (!_state || !command) {
			return;
		}
		UndoState& state = *_state;

		command->Apply();
		MarkCommandSceneDirty(*command);
		state.redoStack.clear();

		if (state.groupDepth > 0) {
			for (auto it = state.openGroup.commands.rbegin(); it != state.openGroup.commands.rend(); ++it) {
				if ((*it)->TryCoalesce(*command)) {
					return;
				}
				if (!(*it)->AsPropertySet() && !(*it)->AsFieldState()) {
					break;
				}
			}
			state.openGroup.commands.push_back(std::move(command));
			return;
		}

		CommandEntry entry;
		entry.label = command->GetLabel();
		entry.commands.push_back(std::move(command));
		state.undoStack.push_back(std::move(entry));
		TrimToCap(state.undoStack);
	}

	void UndoSystem::BeginGroup(std::string_view label) {
		if (!_state) {
			return;
		}
		UndoState& state = *_state;
		if (state.groupDepth == 0) {
			state.openGroup.label = str(label);
			state.openGroup.commands.clear();
		}
		++state.groupDepth;
	}

	void UndoSystem::EndGroup() {
		if (!_state) {
			return;
		}
		UndoState& state = *_state;
		if (state.groupDepth == 0) {
			SLT_LOG_WARN("Editor", "UndoSystem::EndGroup without a matching BeginGroup");
			return;
		}
		if (--state.groupDepth > 0) {
			return;
		}
		if (state.openGroup.commands.empty()) {
			return;
		}
		CommandEntry entry;
		entry.label = std::move(state.openGroup.label);
		entry.commands = std::move(state.openGroup.commands);
		state.openGroup = {};
		state.undoStack.push_back(std::move(entry));
		TrimToCap(state.undoStack);
	}

	bool UndoSystem::IsGroupOpen() {
		return _state && _state->groupDepth > 0;
	}

	bool UndoSystem::Undo() {
		if (!_state || _state->undoStack.empty()) {
			return false;
		}
		UndoState& state = *_state;
		if (state.groupDepth > 0) {
			SLT_LOG_WARN("Editor", "can't undo while an undo group is open");
			return false;
		}
		CommandEntry entry = std::move(state.undoStack.back());
		state.undoStack.pop_back();
		for (size_t i = entry.commands.size(); i > 0; --i) {
			entry.commands[i - 1]->Revert();
			MarkCommandSceneDirty(*entry.commands[i - 1]);
		}
		state.redoStack.push_back(std::move(entry));
		return true;
	}

	bool UndoSystem::Redo() {
		if (!_state || _state->redoStack.empty()) {
			return false;
		}
		UndoState& state = *_state;
		if (state.groupDepth > 0) {
			SLT_LOG_WARN("Editor", "can't redo while an undo group is open");
			return false;
		}
		CommandEntry entry = std::move(state.redoStack.back());
		state.redoStack.pop_back();
		for (UniquePtr<IEditorCommand>& command : entry.commands) {
			command->Apply();
			MarkCommandSceneDirty(*command);
		}
		state.undoStack.push_back(std::move(entry));
		return true;
	}

	bool UndoSystem::CanUndo() {
		return _state && !_state->undoStack.empty() && _state->groupDepth == 0;
	}

	bool UndoSystem::CanRedo() {
		return _state && !_state->redoStack.empty() && _state->groupDepth == 0;
	}

	str UndoSystem::GetUndoLabel() {
		return CanUndo() ? _state->undoStack.back().label : str();
	}

	str UndoSystem::GetRedoLabel() {
		return CanRedo() ? _state->redoStack.back().label : str();
	}

	void UndoSystem::Clear() {
		if (!_state) {
			return;
		}
		_state->undoStack.clear();
		_state->redoStack.clear();
		_state->openGroup = {};
		_state->groupDepth = 0;
	}

	u32 UndoSystem::GetUndoDepth() {
		return _state ? static_cast<u32>(_state->undoStack.size()) : 0;
	}

	u32 UndoSystem::GetRedoDepth() {
		return _state ? static_cast<u32>(_state->redoStack.size()) : 0;
	}

} // namespace Shalltear
