#include "PackageBindings.h"

#include "Generated/VisualEffectsBindings.generated.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::VisualEffectsCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "VisualEffects";

		SltVisualEffectsBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillVisualEffectsTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_VISUALEFFECTS_LAYOUT_HASH);
	}

	void ShutdownPackage() {
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::VisualEffectsCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(VisualEffectsCSharp, &Shalltear::VisualEffectsCSharpImpl::RegisterPackage, &Shalltear::VisualEffectsCSharpImpl::ShutdownPackage)
