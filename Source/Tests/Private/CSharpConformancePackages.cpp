#include "TestsPCH.h"

#include "CSharpConformance.h"

#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"

#include <array>

#ifndef SHALLTEAR_SHARED
SLT_DECLARE_LINKED_PACKAGE(Physics3D)
SLT_DECLARE_LINKED_PACKAGE(Physics3DCSharp)
SLT_DECLARE_LINKED_PACKAGE(Animation)
SLT_DECLARE_LINKED_PACKAGE(AnimationCSharp)
#endif

namespace Shalltear::CSharpConformance {

#ifdef SHALLTEAR_SHARED
	namespace {
		constexpr std::array<const char*, 4> Packages{"Physics3D", "Physics3DCSharp", "Animation", "AnimationCSharp"};

		std::array<ModuleHandle, Packages.size()> _loaded;
	} // anonymous namespace
#endif

	bool StandUpPackages(const void* owner) {
#ifdef SHALLTEAR_SHARED
		(void)owner;
		_loaded = {};
		for (size_t index = 0; index < Packages.size(); ++index) {
			_loaded[index] = ModuleSystem::Load(Path(str(Packages[index]) + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace);
			if (!ModuleSystem::IsLoaded(_loaded[index])) {
				return false;
			}
		}
		return true;
#else
		SLT_CALL_LINKED_PACKAGE(Physics3D, owner);
		SLT_CALL_LINKED_PACKAGE(Physics3DCSharp, owner);
		SLT_CALL_LINKED_PACKAGE(Animation, owner);
		SLT_CALL_LINKED_PACKAGE(AnimationCSharp, owner);
		return true;
#endif
	}

	bool UnloadBridge(std::string_view name) {
#ifdef SHALLTEAR_SHARED
		for (size_t index = 0; index < Packages.size(); ++index) {
			if (Packages[index] == name) {
				return ModuleSystem::Unload(_loaded[index]);
			}
		}
#else
		(void)name;
#endif
		return false;
	}

} // namespace Shalltear::CSharpConformance
