#include "Shalltear/Editor/EditorTasks.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace Shalltear::EditorTasks {

	namespace {

		struct Task {
			u64 id = 0;
			const void* owner = nullptr;
			str text;
			std::optional<f32> fraction;
			std::function<void()> onCancel;
		};

		std::mutex _mutex;
		std::vector<Task> _tasks;
		u64 _nextId = 1;
		u64 _generation = 0;

		Task* Find(u64 id) {
			const auto found = std::find_if(_tasks.begin(), _tasks.end(), [id](const Task& task) { return task.id == id; });
			return found != _tasks.end() ? &*found : nullptr;
		}

	} // anonymous namespace

	u64 Begin(TaskDesc desc, const void* owner) {
		std::lock_guard lock(_mutex);
		Task task;
		task.id = _nextId++;
		task.owner = owner;
		task.text = std::move(desc.text);
		task.onCancel = std::move(desc.onCancel);
		_tasks.push_back(std::move(task));
		++_generation;
		return _tasks.back().id;
	}

	void Report(u64 task, std::string_view text, std::optional<f32> fraction) {
		std::lock_guard lock(_mutex);
		Task* found = Find(task);
		if (!found) {
			return;
		}
		if (fraction) {
			fraction = std::clamp(*fraction, 0.0f, 1.0f);
		}
		if (found->text == text && found->fraction == fraction) {
			return;
		}
		found->text = text;
		found->fraction = fraction;
		++_generation;
	}

	void End(u64 task) {
		std::lock_guard lock(_mutex);
		if (std::erase_if(_tasks, [task](const Task& candidate) { return candidate.id == task; }) > 0) {
			++_generation;
		}
	}

	bool Cancel(u64 task) {
		std::function<void()> cancel;
		{
			std::lock_guard lock(_mutex);
			const Task* found = Find(task);
			if (!found || !found->onCancel) {
				return false;
			}
			cancel = found->onCancel;
		}
		cancel();
		return true;
	}

	void GetActive(Vector<TaskView>& out) {
		out.clear();
		std::lock_guard lock(_mutex);
		for (const Task& task : _tasks) {
			out.push_back({task.id, task.text, task.fraction, static_cast<bool>(task.onCancel)});
		}
	}

	u64 GetGeneration() {
		std::lock_guard lock(_mutex);
		return _generation;
	}

	u32 UnregisterOwnedBy(const void* owner) {
		std::lock_guard lock(_mutex);
		const u32 removed = static_cast<u32>(std::erase_if(_tasks, [owner](const Task& task) { return task.owner == owner; }));
		_generation += removed > 0 ? 1 : 0;
		return removed;
	}

} // namespace Shalltear::EditorTasks
