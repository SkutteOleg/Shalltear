#include "ScriptRuntime.h"

#include "ScriptLog.h"

#include "FunctionBindings.h"

#include "ManagedBehavior.h"
#include "ManagedDataAsset.h"
#include "InputMaps.h"
#include "ManagedTypes.h"
#include "NativeFieldBindings.h"
#include "PackageBindings.h"
#include "ScriptEvents.h"
#include "ScriptProfile.h"
#include "ShadowStorage.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module/TypeReplacement.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

namespace Shalltear::CSharpImpl::ScriptRuntime {

	namespace {

		SltManagedAPI _managed{};
		bool _attached = false;
		// glibc won't unmap a library holding live thread_local destructors
		[[clang::no_destroy]] thread_local str _lastError;
		[[clang::no_destroy]] thread_local str _keptString;

		std::string_view View(SltString text) {
			return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
		}

		void ReportException(SltString type, SltString message, SltString stackTrace, SltString file, int32_t line) noexcept {
			try {
				if (LogLevel::Error >= LogManager::GetGlobalLevel()) {
					LogManager::Dispatch(LogLevel::Error, "C#", InternLogOrigin(View(file)), static_cast<u32>(std::max<int32_t>(line, 0)), "", "{}: {}\n{}",
										 View(type), View(message), View(stackTrace));
				}
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

		int32_t RegisterManagedType(const SltTypeDesc* type) noexcept {
			int32_t result = SLT_ERR_REFUSED;
			const int32_t guarded = Guard([&] { result = ManagedTypes::Register(*type); });
			return guarded == SLT_OK ? result : guarded;
		}

		int32_t RegisterManagedEnum(const SltEnumDesc* type) noexcept {
			int32_t result = SLT_ERR_REFUSED;
			const int32_t guarded = Guard([&] { result = ManagedTypes::RegisterEnum(*type); });
			return guarded == SLT_OK ? result : guarded;
		}

		int32_t RegisterManagedValue(SltValueDesc* type) noexcept {
			int32_t result = SLT_ERR_REFUSED;
			const int32_t guarded = Guard([&] { result = ManagedTypes::RegisterValue(*type); });
			return guarded == SLT_OK ? result : guarded;
		}

		void StringAssign(void* field, SltString text) noexcept {
#ifdef SLT_CSHARP_TEST_HOOKS
			++ManagedTypes::GetCounters().textAssigns;
#endif
			Guard([&] { static_cast<str*>(field)->assign(View(text)); });
		}

		void StringRead(const void* field, SltString* outText) noexcept {
			const str& text = *static_cast<const str*>(field);
			*outText = SltString{text.data(), static_cast<int32_t>(text.size())};
		}

		void* ListResize(void* field, int32_t count) noexcept {
			void* data = nullptr;
			Guard([&] { data = ShadowStorage::Resize(*static_cast<ShadowList*>(field), static_cast<size_t>(std::max(count, 0))); });
			return data;
		}

		void* ListData(const void* field, int32_t* outCount) noexcept {
			const ShadowList& list = *static_cast<const ShadowList*>(field);
			*outCount = static_cast<int32_t>(list.count);
			return list.data;
		}

		intptr_t ScriptOf(uint64_t id) noexcept {
			Object* object = ObjectRegistry::Resolve(id);
			if (!object || object->GetType()->ownerModule != ManagedTypes::GetOwnerToken()) {
				return 0;
			}
			return TypeRegistry::IsDerivedFrom(object->GetType(), Asset::GetReflectType()) ? static_cast<ManagedDataAsset*>(object)->GetInstance()
																						   : static_cast<ManagedBehavior*>(object)->GetInstance();
		}

		int32_t FindBindings(SltString name, uint64_t layoutHash, int32_t size, const void** outTable) noexcept {
			return Guard([&] { *outTable = PackageBindings::Find(View(name), layoutHash, size); });
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

	SltString KeepString(std::string_view text) {
		_keptString.assign(text);
		return SltString{_keptString.data(), static_cast<int32_t>(_keptString.size())};
	}

	void Tick() {
		if (_attached) {
			FunctionBindings::Poll();
			_managed.tick();
		}
	}

	void Pump(i32 phase) {
		if (_attached) {
			FunctionBindings::Poll();
			_managed.pump(phase);
		}
	}

	SltEngineAPI MakeEngineAPI() {
		SltEngineAPI engine{};
		engine.abiVersion = SLT_BRIDGE_ABI_VERSION;
		engine.layoutHash = SLT_BRIDGE_LAYOUT_HASH;
		Generated::FillBindings(engine.bindings);
		engine.reportException = &ReportException;
		engine.log = &Log;
		engine.getLastError = &GetLastError;
		engine.textSize = static_cast<int32_t>(sizeof(str));
		engine.registerManagedType = &RegisterManagedType;
		engine.registerManagedEnum = &RegisterManagedEnum;
		engine.registerManagedValue = &RegisterManagedValue;
		engine.stringAssign = &StringAssign;
		engine.stringRead = &StringRead;
		engine.listResize = &ListResize;
		engine.listData = &ListData;
		engine.scriptOf = &ScriptOf;
		engine.findBindings = &FindBindings;
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
		bool unloaded = false;
		if (ManagedTypes::HasLiveAssets()) {
			const auto unload = [](void* user) {
				*static_cast<bool*>(user) = true;
				return UnloadUserAssemblies() == SLT_OK;
			};
			TypeReplacement::Replace(ManagedTypes::GetOwnerToken(), unload, &unloaded);
		}
		if (!unloaded) {
			UnloadUserAssemblies();
		}
		_managed.shutdown();
		ScriptEvents::Reset();
		ScriptProfile::Reset();
		FunctionBindings::Reset();
		NativeFieldBindings::Reset();
		_managed = {};
		_attached = false;
	}

	i32 LoadUserAssemblies(std::span<const str> paths) {
		if (!_attached || !_managed.loadUserAssemblies) {
			return SLT_ERR_REFUSED;
		}
		std::vector<SltString> views;
		views.reserve(paths.size());
		for (const str& path : paths) {
			views.push_back(SltString{path.data(), static_cast<int32_t>(path.size())});
		}
		return _managed.loadUserAssemblies(views.data(), static_cast<int32_t>(views.size()));
	}

	i32 LoadKeptUserAssemblies() {
		return _attached && _managed.loadKeptUserAssemblies ? _managed.loadKeptUserAssemblies() : SLT_ERR_REFUSED;
	}

	i32 UnloadUserAssemblies() {
		if (!_attached) {
			return SLT_OK;
		}
		ManagedTypes::ReleaseAll();
		InputMaps::DestroyAll();
		return _managed.unloadUserAssemblies ? _managed.unloadUserAssemblies() : SLT_OK;
	}

	bool IsAttached() {
		return _attached;
	}

	const SltManagedAPI& GetManaged() {
		return _managed;
	}

} // namespace Shalltear::CSharpImpl::ScriptRuntime
