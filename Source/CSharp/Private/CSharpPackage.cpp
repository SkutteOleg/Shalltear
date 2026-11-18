#include "ScriptRuntime.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::CSharpImpl {

	void RegisterPackage(const void* owner) {
		(void)owner;
	}

	void ShutdownPackage() {
		ScriptRuntime::Detach();
	}

} // namespace Shalltear::CSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(CSharp, &Shalltear::CSharpImpl::RegisterPackage, &Shalltear::CSharpImpl::ShutdownPackage)
