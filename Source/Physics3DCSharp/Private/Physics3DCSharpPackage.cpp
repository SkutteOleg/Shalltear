#include "PackageBindings.h"
#include "PhysicsBindings.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::Physics3DCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "Physics3D";

		SltPhysics3DBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillPhysics3DTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_PHYSICS3D_LAYOUT_HASH);
		CSharpImpl::PhysicsBindings::Register();
	}

	void ShutdownPackage() {
		CSharpImpl::PhysicsBindings::Unregister();
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::Physics3DCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(Physics3DCSharp, &Shalltear::Physics3DCSharpImpl::RegisterPackage, &Shalltear::Physics3DCSharpImpl::ShutdownPackage)
