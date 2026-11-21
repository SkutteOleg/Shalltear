#include "PackageBindings.h"

#include "Generated/PostProcessingBindings.generated.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::PostProcessingCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "PostProcessing";

		SltPostProcessingBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillPostProcessingTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_POSTPROCESSING_LAYOUT_HASH);
	}

	void ShutdownPackage() {
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::PostProcessingCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(PostProcessingCSharp, &Shalltear::PostProcessingCSharpImpl::RegisterPackage, &Shalltear::PostProcessingCSharpImpl::ShutdownPackage)
