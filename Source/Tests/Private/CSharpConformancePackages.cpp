#include "TestsPCH.h"

#include "CSharpConformance.h"

#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"

#ifndef SHALLTEAR_SHARED
SLT_DECLARE_LINKED_PACKAGE(Physics3D)
SLT_DECLARE_LINKED_PACKAGE(Physics3DCSharp)
#endif

namespace Shalltear::CSharpConformance {

#ifdef SHALLTEAR_SHARED
	namespace {
		ModuleHandle _bridge;
	} // anonymous namespace
#endif

	bool StandUpPackages(const void* owner) {
#ifdef SHALLTEAR_SHARED
		(void)owner;
		_bridge = {};
		if (!ModuleSystem::IsLoaded(ModuleSystem::Load(Path(str("Physics3D") + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace))) {
			return false;
		}
		_bridge = ModuleSystem::Load(Path(str("Physics3DCSharp") + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace);
		return ModuleSystem::IsLoaded(_bridge);
#else
		SLT_CALL_LINKED_PACKAGE(Physics3D, owner);
		SLT_CALL_LINKED_PACKAGE(Physics3DCSharp, owner);
		return true;
#endif
	}

	bool UnloadBridges() {
#ifdef SHALLTEAR_SHARED
		return ModuleSystem::Unload(_bridge);
#else
		return false;
#endif
	}

} // namespace Shalltear::CSharpConformance
