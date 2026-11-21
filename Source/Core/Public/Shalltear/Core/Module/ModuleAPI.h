#pragma once

#include "Shalltear/Core/BuildIdentity.generated.h"
#include "Shalltear/Core/Export.h"
#include "Shalltear/Core/Module/EngineContext.h"
#include "Shalltear/Core/Module/ModuleThunkRegistry.h"
#include "Shalltear/Core/Platform.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	inline constexpr u32 SLT_MODULE_FLAG_RELEASE = 1u << 0;

	struct ShalltearModuleAPI {
		u32 contextVersion;
		u32 flags;
		u64 abiHash;
		const char* name;

		bool (*bind)(const EngineContext*);
		void (*registerTypes)();
		void (*shutdown)();
	};

	using ShalltearModuleGetAPIFn = ShalltearModuleAPI* (*)(u32 hostContextVersion);

	using CoreExtensionRegisterFn = void (*)(const void* owner);

} // namespace Shalltear

#define SLT_MODULE_GET_API_SYMBOL "ShalltearModuleGetApi"
#define SLT_CORE_EXTENSION_REGISTER_SYMBOL "ShalltearRegisterCoreExtensions"

#ifdef NDEBUG
#define SLT_DETAIL_MODULE_FLAGS ::Shalltear::SLT_MODULE_FLAG_RELEASE
#else
#define SLT_DETAIL_MODULE_FLAGS 0u
#endif

// plants a trivial DllMain to avoid the xinput.lib DllMain symbol
#if defined(SLT_PLATFORM_WINDOWS) && !defined(SHALLTEAR_SHARED)
#define SLT_DETAIL_MODULE_DLLMAIN()                                 \
	extern "C" int __stdcall DllMain(void*, unsigned long, void*) { \
		return 1;                                                   \
	}
#else
#define SLT_DETAIL_MODULE_DLLMAIN()
#endif

#define SLT_DETAIL_IMPLEMENT_MODULE(ModuleName, ShutdownFn)                           \
	SLT_DETAIL_MODULE_DLLMAIN()                                                       \
	extern "C" SLT_API_EXPORT ::Shalltear::ShalltearModuleAPI* ShalltearModuleGetApi( \
		::Shalltear::u32) {                                                           \
		static ::Shalltear::ShalltearModuleAPI api{                                   \
			::Shalltear::SLT_ENGINE_CONTEXT_VERSION,                                  \
			SLT_DETAIL_MODULE_FLAGS,                                                  \
			SLT_MODULE_ABI_HASH,                                                      \
			#ModuleName,                                                              \
			+[](const ::Shalltear::EngineContext* context) -> bool {                  \
				return ::Shalltear::BindEngineContext(context);                       \
			},                                                                        \
			+[]() {                                                                   \
				::Shalltear::TypeRegistry::RegisterPendingTypes();                    \
				::Shalltear::ModuleThunkRegistry::RegisterPendingThunkTables();       \
			},                                                                        \
			ShutdownFn};                                                              \
		return &api;                                                                  \
	}

#define SLT_IMPLEMENT_MODULE(ModuleName) \
	SLT_DETAIL_IMPLEMENT_MODULE(ModuleName, +[]() {})

#define SLT_IMPLEMENT_MODULE_WITH_SHUTDOWN(ModuleName, ShutdownFn) \
	SLT_DETAIL_IMPLEMENT_MODULE(ModuleName, ShutdownFn)

#define SLT_IMPLEMENT_CORE_EXTENSIONS(RegisterFunction)                                 \
	extern "C" SLT_API_EXPORT void ShalltearRegisterCoreExtensions(const void* owner) { \
		::Shalltear::CoreExtensionRegisterFn registerFn = (RegisterFunction);           \
		registerFn(owner);                                                              \
	}

#define SLT_DETAIL_PACKAGE_SYMBOL(PackageName) ShalltearPackageRegister_##PackageName

// for teardown that must not run under the loader lock

#define SLT_DETAIL_PACKAGE_SHUTDOWN_CHECK(PackageName) ShalltearPackageShutdownCheck_##PackageName

#ifdef SLT_PACKAGE_STATIC

#define SLT_IMPLEMENT_PACKAGE(PackageName, RegisterFunction)                    \
	extern "C" void SLT_DETAIL_PACKAGE_SYMBOL(PackageName)(const void* owner) { \
		::Shalltear::CoreExtensionRegisterFn registerFn = (RegisterFunction);   \
		registerFn(owner);                                                      \
	}

#define SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(PackageName, RegisterFunction, ShutdownFunction)                      \
	SLT_IMPLEMENT_PACKAGE(PackageName, RegisterFunction)                                                          \
	namespace {                                                                                                   \
		[[maybe_unused]] constexpr void (*SLT_DETAIL_PACKAGE_SHUTDOWN_CHECK(PackageName))() = (ShutdownFunction); \
	}

#else

#define SLT_IMPLEMENT_PACKAGE(PackageName, RegisterFunction) \
	SLT_IMPLEMENT_MODULE(PackageName)                        \
	SLT_IMPLEMENT_CORE_EXTENSIONS(RegisterFunction)

#define SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(PackageName, RegisterFunction, ShutdownFunction) \
	SLT_IMPLEMENT_MODULE_WITH_SHUTDOWN(PackageName, ShutdownFunction)                        \
	SLT_IMPLEMENT_CORE_EXTENSIONS(RegisterFunction)

#endif

#define SLT_DECLARE_LINKED_PACKAGE(PackageName) \
	extern "C" void SLT_DETAIL_PACKAGE_SYMBOL(PackageName)(const void* owner);

#define SLT_CALL_LINKED_PACKAGE(PackageName, Owner) SLT_DETAIL_PACKAGE_SYMBOL(PackageName)(Owner)
