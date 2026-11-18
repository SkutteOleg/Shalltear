#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::CSharpImpl {

	void RegisterPackage(const void* owner) {
		(void)owner;
	}

} // namespace Shalltear::CSharpImpl

SLT_IMPLEMENT_PACKAGE(CSharp, &Shalltear::CSharpImpl::RegisterPackage)
