#pragma once

#include "ScriptAPI.h"
#include "ScriptBridge.h"

#include "Shalltear/Core/Types.h"

#include <exception>
#include <span>

namespace Shalltear::CSharpImpl {

	namespace ScriptRuntime {

		bool Attach(SltInitializeFn initialize, str& outError);
		bool AttachAs(SltEngineAPI engine, SltInitializeFn initialize, str& outError);

		void Detach();

		i32 LoadUserAssemblies(std::span<const str> paths);
		i32 LoadKeptUserAssemblies();
		i32 UnloadUserAssemblies();

		bool IsAttached();

		// valid while attached
		const SltManagedAPI& GetManaged();

		SltEngineAPI MakeEngineAPI();

		SHALLTEAR_CSHARP_API void SetLastError(std::string_view text);

		// valid until this thread's next KeepString
		SHALLTEAR_CSHARP_API SltString KeepString(std::string_view text);

		void Tick();

		// no C++ exception may unwind into managed frames
		template <typename Body>
		int32_t Guard(Body&& body) noexcept {
			try {
				body();
				return SLT_OK;
			} catch (const std::exception& error) {
				SetLastError(error.what());
			} catch (...) {
				SetLastError("unknown native exception");
			}
			return SLT_ERR_EXCEPTION;
		}

	} // namespace ScriptRuntime

} // namespace Shalltear::CSharpImpl
