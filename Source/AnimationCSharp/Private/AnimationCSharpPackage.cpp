#include "AnimationBindings.h"
#include "PackageBindings.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::AnimationCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "Animation";

		SltAnimationBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillAnimationTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_ANIMATION_LAYOUT_HASH);
	}

	void ShutdownPackage() {
		CSharpImpl::AnimationBindings::Release();
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::AnimationCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(AnimationCSharp, &Shalltear::AnimationCSharpImpl::RegisterPackage, &Shalltear::AnimationCSharpImpl::ShutdownPackage)
