#include "PackageBindings.h"
#include "ScriptRuntime.h"

#include "Generated/ImportPipelineBindings.generated.h"

#include "Shalltear/ImportPipeline/ModelContributors.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::ImportPipelineCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "ImportPipeline";

		SltImportPipelineBindings _table{};

		void SweepScripts(const void* owner) {
			ModelContributors::UnregisterOwnedBy(owner);
		}

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillImportPipelineTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_IMPORTPIPELINE_LAYOUT_HASH);
		CSharpImpl::ScriptRuntime::AddOwnerSweep(&SweepScripts);
	}

	void ShutdownPackage() {
		CSharpImpl::ScriptRuntime::RemoveOwnerSweep(&SweepScripts);
		SweepScripts(CSharpImpl::ScriptRuntime::GetScriptsOwner());
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::ImportPipelineCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(ImportPipelineCSharp, &Shalltear::ImportPipelineCSharpImpl::RegisterPackage, &Shalltear::ImportPipelineCSharpImpl::ShutdownPackage)
