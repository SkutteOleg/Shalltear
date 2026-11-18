#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Types.h"

#include <exception>

namespace Shalltear::CSharpImpl {

	namespace ScriptRuntime {

		bool Attach(SltInitializeFn initialize, str& outError);
		bool AttachAs(SltEngineAPI engine, SltInitializeFn initialize, str& outError);

		void Detach();

		bool IsAttached();

		// valid while attached
		const SltManagedAPI& GetManaged();

		SltEngineAPI MakeEngineAPI();

		void SetLastError(std::string_view text);

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
