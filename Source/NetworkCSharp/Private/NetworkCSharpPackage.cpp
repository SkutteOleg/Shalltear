#include "PackageBindings.h"

#include "Generated/NetworkBindings.generated.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::NetworkCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "Network";

		SltNetworkBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillNetworkTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_NETWORK_LAYOUT_HASH);
	}

	void ShutdownPackage() {
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::NetworkCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(NetworkCSharp, &Shalltear::NetworkCSharpImpl::RegisterPackage, &Shalltear::NetworkCSharpImpl::ShutdownPackage)
