#include "Shalltear/Editor/CompileStatus.h"

#include <algorithm>
#include <vector>

namespace Shalltear::CompileStatus {

	namespace {

		struct Source {
			SourceDesc desc;
			const void* owner = nullptr;
		};

		std::vector<Source> _sources;

		bool IsBusy(CompileState state) {
			return state == CompileState::Configuring || state == CompileState::Compiling;
		}

		u32 Rank(CompileState state) {
			switch (state) {
			case CompileState::Idle:
				return 0;
			case CompileState::Succeeded:
				return 1;
			case CompileState::Failed:
				return 2;
			case CompileState::Configuring:
				return 3;
			case CompileState::Compiling:
				return 4;
			}
			return 0;
		}

	} // anonymous namespace

	void RegisterSource(SourceDesc desc, const void* owner) {
		if (!desc.getState) {
			return;
		}
		_sources.push_back({std::move(desc), owner});
	}

	u32 UnregisterOwnedBy(const void* owner) {
		return static_cast<u32>(std::erase_if(_sources, [owner](const Source& source) { return source.owner == owner; }));
	}

	void GetSources(Vector<SourceView>& out) {
		out.clear();
		for (const Source& source : _sources) {
			const SourceDesc& desc = source.desc;
			out.push_back({desc.name, desc.getHeading ? desc.getHeading(desc.user) : desc.name, desc.getState(desc.user)});
		}
	}

	u32 GetSourceCount() {
		return static_cast<u32>(_sources.size());
	}

	Summary Combine() {
		Summary summary;
		for (const Source& source : _sources) {
			const SourceState state = source.desc.getState(source.desc.user);
			if (Rank(state.compile) > Rank(summary.compile)) {
				summary.compile = state.compile;
			}
			summary.errors += state.errors;
			summary.warnings += state.warnings;
			summary.reloadHeld = summary.reloadHeld || state.reloadHeld;
		}
		return summary;
	}

	void RequestCompile() {
		for (size_t index = 0; index < _sources.size(); ++index) {
			const SourceDesc desc = _sources[index].desc;
			if (desc.compile && !IsBusy(desc.getState(desc.user).compile)) {
				desc.compile(desc.user);
			}
		}
	}

	void RequestReload() {
		for (size_t index = 0; index < _sources.size(); ++index) {
			const SourceDesc desc = _sources[index].desc;
			if (desc.reload) {
				desc.reload(desc.user);
			}
		}
	}

} // namespace Shalltear::CompileStatus
