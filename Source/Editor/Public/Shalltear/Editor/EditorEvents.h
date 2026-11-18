#pragma once

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <span>

namespace Shalltear {

	class Scene;
	class SceneObject;

	enum class PlayModeState : u8 {
		Editing = 0,
		Playing,
		Paused
	};

	struct EditorSelectionChangedEvent {};

	struct EditorPlayModeChangedEvent {
		PlayModeState state = PlayModeState::Editing;
	};

	struct EditorPlayStartingEvent {};

	struct EditorSceneDirtiedEvent {
		Handle<Scene> scene;
	};

	struct EditorModuleReloadBeganEvent {
		const void* moduleBase = nullptr;
	};
	struct EditorModuleReloadEndedEvent {
		bool succeeded = true;
		const void* moduleBase = nullptr;
	};

	struct EditorIdRemap {
		u64 oldId = 0;
		u64 newId = 0;
	};
	struct EditorWorldRemappedEvent {
		std::span<const EditorIdRemap> ids;
	};

	struct EditorTransformGestureEvent {
		std::span<const Handle<SceneObject>> objects;
	};

	struct EditorPingTarget {
		Guid asset;
		u64 object = 0;

		bool IsValid() const { return asset.IsValid() || object != 0; }
	};
	struct EditorPingRequestedEvent {
		EditorPingTarget target;
	};

	SHALLTEAR_EDITOR_API EditorPingTarget PingTargetForInstance(u64 instanceId);

	struct EditorFollowRequestedEvent {
		u64 object = 0;
	};

	struct EditorCompileStateChangedEvent {
		u8 state = 0;
		u32 errorCount = 0;
		u32 warningCount = 0;
	};

	namespace EditorEvents {
		inline constexpr Event<EditorSelectionChangedEvent> SelectionChanged{"Editor.SelectionChanged"_hash};
		inline constexpr Event<EditorPlayModeChangedEvent> PlayModeChanged{"Editor.PlayModeChanged"_hash};
		// sent before edited world is snapshotted for Play
		inline constexpr Event<EditorPlayStartingEvent> PlayStarting{"Editor.PlayStarting"_hash};
		inline constexpr Event<EditorSceneDirtiedEvent> SceneDirtied{"Editor.SceneDirtied"_hash};
		inline constexpr Event<EditorModuleReloadBeganEvent> ModuleReloadBegan{"Editor.ModuleReloadBegan"_hash};
		inline constexpr Event<EditorModuleReloadEndedEvent> ModuleReloadEnded{"Editor.ModuleReloadEnded"_hash};
		inline constexpr Event<EditorCompileStateChangedEvent> CompileStateChanged{"Editor.CompileStateChanged"_hash};
		inline constexpr Event<EditorPingRequestedEvent> PingRequested{"Editor.PingRequested"_hash};
		inline constexpr Event<EditorFollowRequestedEvent> FollowRequested{"Editor.FollowRequested"_hash};
		inline constexpr Event<EditorWorldRemappedEvent> WorldRemapped{"Editor.WorldRemapped"_hash};
		inline constexpr Event<EditorTransformGestureEvent> TransformGesture{"Editor.TransformGesture"_hash};
	} // namespace EditorEvents

} // namespace Shalltear
