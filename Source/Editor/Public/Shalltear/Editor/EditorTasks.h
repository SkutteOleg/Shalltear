#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <functional>
#include <optional>
#include <string_view>

namespace Shalltear::EditorTasks {

	struct TaskDesc {
		str text;
		// task listed until End
		std::function<void()> onCancel;
	};

	struct TaskView {
		u64 id = 0;
		str text;
		std::optional<f32> fraction;
		bool cancellable = false;
	};

	SHALLTEAR_EDITOR_API u64 Begin(TaskDesc desc, const void* owner);

	SHALLTEAR_EDITOR_API void Report(u64 task, std::string_view text, std::optional<f32> fraction = std::nullopt);

	SHALLTEAR_EDITOR_API void End(u64 task);

	SHALLTEAR_EDITOR_API bool Cancel(u64 task);

	SHALLTEAR_EDITOR_API void GetActive(Vector<TaskView>& out);

	SHALLTEAR_EDITOR_API u64 GetGeneration();

	SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

} // namespace Shalltear::EditorTasks
