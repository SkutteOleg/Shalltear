#include "Shalltear/CSharp/StaticHost.h"

namespace Shalltear::CSharpStaticHost {

	namespace {

		StaticEntryFn _staticEntry = nullptr;

	} // anonymous namespace

	void SetEntry(StaticEntryFn entry) {
		_staticEntry = entry;
	}

	StaticEntryFn GetEntry() {
		return _staticEntry;
	}

} // namespace Shalltear::CSharpStaticHost

#ifndef SLT_WITH_EDITOR

#include "NativeAOTHost.h"

#include "HostLibrary.h"
#include "ManagedTypes.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Time.h"

#include <filesystem>
#include <format>

namespace Shalltear::CSharpImpl::NativeAOTHost {

	bool Start(const str& libraryPath, str& outError) {
		str reason;
		void* library = OpenHostLibrary(libraryPath, reason);
		if (!library) {
			outError = std::format("{} didn't load ({})", libraryPath, reason);
			return false;
		}
		const auto initialize = reinterpret_cast<SltInitializeFn>(FindHostSymbol(library, ENTRY_SYMBOL));
		if (!initialize) {
			outError = std::format("{} doesn't export {}", libraryPath, ENTRY_SYMBOL);
			return false;
		}
		return ScriptRuntime::Attach(initialize, outError);
	}

	void StartPlayer() {
		if (const CSharpStaticHost::StaticEntryFn staticEntry = CSharpStaticHost::GetEntry()) {
			const u64 begin = Time::GetTicks();
			str error;
			if (!ScriptRuntime::Attach(reinterpret_cast<SltInitializeFn>(staticEntry), error)) {
				SLT_LOG_ERROR("C#", "statically linked scripts failed to start ({})", error);
				return;
			}
			u32 types = 0;
			ManagedTypes::ForEachType([](const ManagedType&, void* user) { ++*static_cast<u32*>(user); }, &types);
			SLT_LOG_INFO("C#", "statically linked scripts started in {:.1f} ms ({} script type(s))",
						 static_cast<f64>(Time::GetTicks() - begin) * 1000.0 / static_cast<f64>(Time::GetTicksPerSecond()), types);
			return;
		}
		const str name = GetPlayerLibraryName();
		const str library = FileSystem::GetExecutablePath().GetDirectory() + "/" + name;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(std::filesystem::path(library), ec)) {
			return;
		}
		const u64 begin = Time::GetTicks();
		str error;
		if (!Start(library, error)) {
			SLT_LOG_ERROR("C#", "{}", error);
			return;
		}
		u32 types = 0;
		ManagedTypes::ForEachType([](const ManagedType&, void* user) { ++*static_cast<u32*>(user); }, &types);
		SLT_LOG_INFO("C#", "{} started in {:.1f} ms ({} script type(s))", name,
					 static_cast<f64>(Time::GetTicks() - begin) * 1000.0 / static_cast<f64>(Time::GetTicksPerSecond()), types);
	}

} // namespace Shalltear::CSharpImpl::NativeAOTHost

#endif
