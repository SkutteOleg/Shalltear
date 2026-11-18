#include "ScriptRuntime.h"

#include "Shalltear/Core/Log.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::ScriptRuntime {

	namespace {

		SltManagedAPI _managed{};
		bool _attached = false;
		thread_local str _lastError;

		std::string_view View(SltString text) {
			return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
		}

		void ReportException(SltString type, SltString message, SltString stackTrace) noexcept {
			try {
				SLT_LOG_ERROR("C#", "{}: {}\n{}", View(type), View(message), View(stackTrace));
			} catch (...) {
			}
		}

		void Log(int32_t level, SltString message) noexcept {
			try {
				const LogLevel clamped = static_cast<LogLevel>(std::clamp<int32_t>(level, 0, static_cast<int32_t>(LogLevel::Fatal)));
				if (clamped >= LogManager::GetGlobalLevel()) {
					LogManager::Dispatch(clamped, "C#", "", 0, "", "{}", View(message));
				}
			} catch (...) {
			}
		}

		void GetLastError(SltString* outText) noexcept {
			*outText = SltString{_lastError.data(), static_cast<int32_t>(_lastError.size())};
		}

#ifdef SLT_CSHARP_TEST_HOOKS
		int32_t ThrowForTest() noexcept {
			return Guard([] { throw std::runtime_error("thrown for a test"); });
		}
#endif

	} // anonymous namespace

	void SetLastError(std::string_view text) {
		_lastError.assign(text);
	}

	SltEngineAPI MakeEngineAPI() {
		SltEngineAPI engine{};
		engine.abiVersion = SLT_BRIDGE_ABI_VERSION;
		engine.layoutHash = SLT_BRIDGE_LAYOUT_HASH;
		engine.reportException = &ReportException;
		engine.log = &Log;
		engine.getLastError = &GetLastError;
#ifdef SLT_CSHARP_TEST_HOOKS
		engine.throwForTest = &ThrowForTest;
#endif
		return engine;
	}

	bool Attach(SltInitializeFn initialize, str& outError) {
		return AttachAs(MakeEngineAPI(), initialize, outError);
	}

	bool AttachAs(SltEngineAPI engine, SltInitializeFn initialize, str& outError) {
		if (_attached) {
			outError = "C# runtime is already attached";
			return false;
		}
		SltManagedAPI managed{};
		if (initialize(&engine, &managed) != SLT_OK) {
			outError = std::format("C# runtime doesn't match this engine (bridge ABI {} layout {:016x}, runtime ABI {} layout {:016x}). Rebuild the engine",
								   engine.abiVersion, engine.layoutHash, managed.abiVersion, managed.layoutHash);
			return false;
		}
		_managed = managed;
		_attached = true;
		return true;
	}

	void Detach() {
		if (!_attached) {
			return;
		}
		if (_managed.unloadUserAssemblies) {
			_managed.unloadUserAssemblies();
		}
		_managed.shutdown();
		_managed = {};
		_attached = false;
	}

	bool IsAttached() {
		return _attached;
	}

	const SltManagedAPI& GetManaged() {
		return _managed;
	}

} // namespace Shalltear::CSharpImpl::ScriptRuntime
