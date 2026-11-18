#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorAPI.h"

namespace Shalltear::CompileStatus {

	struct SourceState {
		CompileState compile = CompileState::Idle;
		u32 errors = 0;
		u32 warnings = 0;
		bool reloadHeld = false;
		// TypeInfo::ownerModule of component types added by this source
		const void* typeOwner = nullptr;
	};

	using StateFn = SourceState (*)(void* user);
	using ActionFn = void (*)(void* user);
	using HeadingFn = str (*)(void* user);

	struct SourceDesc {
		str name;
		StateFn getState = nullptr;
		ActionFn compile = nullptr;
		ActionFn reload = nullptr;
		HeadingFn getHeading = nullptr;
		void* user = nullptr;
	};

	SHALLTEAR_EDITOR_API void RegisterSource(SourceDesc desc, const void* owner);

	SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

	struct SourceView {
		str name;
		str heading;
		SourceState state;
	};

	SHALLTEAR_EDITOR_API void GetSources(Vector<SourceView>& out);

	SHALLTEAR_EDITOR_API u32 GetSourceCount();

	struct Summary {
		CompileState compile = CompileState::Idle;
		u32 errors = 0;
		u32 warnings = 0;
		bool reloadHeld = false;
	};

	SHALLTEAR_EDITOR_API Summary Combine();

	SHALLTEAR_EDITOR_API void RequestCompile();

	SHALLTEAR_EDITOR_API void RequestReload();

} // namespace Shalltear::CompileStatus
