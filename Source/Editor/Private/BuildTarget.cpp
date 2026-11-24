#include "Shalltear/Editor/BuildTarget.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"

#include "Platform/EditorPlatform.h"
#include "Shalltear/Editor/WebPreviewServer.h"
#include "Targets/LinuxTarget.h"
#include "Targets/WebTarget.h"
#include "Targets/WindowsTarget.h"

#include <vector>

namespace Shalltear {

	namespace {

		struct TargetEntry {
			str id;
			const IBuildTarget* target = nullptr;
			const void* owner = nullptr;
			const IBuildTarget* shadow = nullptr;
		};

		struct TargetState {
			std::vector<TargetEntry> targets;
		};

		TargetState* _state = nullptr;

		const WindowsTarget _Windows;
		const WindowsServerTarget _WindowsServer;
		const LinuxTarget _Linux;
		const LinuxServerTarget _LinuxServer;
		const WebTarget _Web;

		TargetState& State() {
			if (!_state) {
				_state = New<TargetState>(MemTag::Editor);
				BuildTargetRegistry::RegisterBuiltIns();
			}
			return *_state;
		}

		TargetEntry* FindEntry(std::string_view id) {
			if (!_state) {
				return nullptr;
			}
			for (TargetEntry& entry : _state->targets) {
				if (entry.id == id) {
					return &entry;
				}
			}
			return nullptr;
		}

	} // anonymous namespace

	void BuildTargetRegistry::RegisterBuiltIns() {
		if (!_state) {
			_state = New<TargetState>(MemTag::Editor);
		}
		Register(&_Windows, nullptr);
		Register(&_WindowsServer, nullptr);
		Register(&_Linux, nullptr);
		Register(&_LinuxServer, nullptr);
		Register(&_Web, nullptr);
	}

	void BuildTargetRegistry::Shutdown() {
		WebPreviewServer::Shutdown();
		if (_state) {
			Delete(_state);
			_state = nullptr;
		}
	}

	bool BuildTargetRegistry::Register(const IBuildTarget* target, const void* owner) {
		if (!target || target->GetId().empty()) {
			SLT_LOG_WARN("Build", "BuildTargetRegistry::Register: ignoring a target with no id");
			return false;
		}
		TargetState& state = _state ? *_state : State();

		for (TargetEntry& entry : state.targets) {
			if (entry.id == target->GetId()) {
				if (owner && !entry.owner && !entry.shadow) {
					entry.shadow = entry.target;
				} else if (!owner) {
					entry.shadow = nullptr;
				}
				entry.target = target;
				entry.owner = owner;
				return true;
			}
		}
		state.targets.push_back(TargetEntry{str(target->GetId()), target, owner, nullptr});
		return true;
	}

	const IBuildTarget* BuildTargetRegistry::Find(std::string_view id) {
		State();
		TargetEntry* entry = FindEntry(id);
		return entry ? entry->target : nullptr;
	}

	u32 BuildTargetRegistry::UnregisterOwnedBy(const void* owner) {
		if (!_state || !owner) {
			return 0;
		}
		u32 removed = 0;
		for (size_t i = _state->targets.size(); i > 0; --i) {
			TargetEntry& entry = _state->targets[i - 1];
			if (entry.owner != owner) {
				continue;
			}
			++removed;
			if (entry.shadow) {
				entry.target = entry.shadow;
				entry.owner = nullptr;
				entry.shadow = nullptr;
				continue;
			}
			_state->targets.erase(_state->targets.begin() + static_cast<ptrdiff_t>(i - 1));
		}
		return removed;
	}

	void BuildTargetRegistry::GetTargetIds(Vector<str>& out) {
		out.clear();
		for (const TargetEntry& entry : State().targets) {
			out.push_back(entry.id);
		}
	}

	str BuildTargetRegistry::GetDefaultTargetId() {
		TargetState& state = State();
		for (const TargetEntry& entry : state.targets) {
			if (entry.id == EditorPlatform::GetHostPlatformName()) {
				return entry.id;
			}
		}
		return state.targets.empty() ? str{} : state.targets.front().id;
	}

	void BuildTargetRegistry::GetSchemaFor(std::string_view id, Vector<BuildSettingDesc>& out) {
		out.clear();
		BuildProfileOps::GetCoreSettings(out);

		const IBuildTarget* target = Find(id);
		if (!target) {
			return;
		}

		Vector<BuildSettingDesc> declared{GetMemoryResource(MemTag::Editor)};
		target->DescribeSettings(declared);
		for (BuildSettingDesc& desc : declared) {
			bool collides = false;
			for (const BuildSettingDesc& core : out) {
				if (core.key == desc.key) {
					collides = true;
					break;
				}
			}
			if (collides) {
				SLT_LOG_WARN("Build", "target '{}' declares '{}' (a core build setting, using the core one)",
							 target->GetId(), desc.key);
				continue;
			}
			out.push_back(std::move(desc));
		}
	}

} // namespace Shalltear
