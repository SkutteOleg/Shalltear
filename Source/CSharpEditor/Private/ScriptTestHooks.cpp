#ifdef SLT_CSHARP_TEST_HOOKS

#include "ScriptTestHooks.h"

#include "BindingProbe.h"
#include "CSharpComponentMenu.h"
#include "CSharpDebugLineGizmo.h"
#include "CoreCLRHost.h"
#include "DotNetPin.generated.h"
#include "DotNetToolchain.h"
#include "InputMaps.h"
#include "ManagedTypes.h"
#include "ScriptLog.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Export.h"

extern "C" SLT_API_EXPORT const Shalltear::CSharpImpl::ScriptTestHooks* ShalltearCSharpTestHooks() {
	using namespace Shalltear;
	static const CSharpImpl::ScriptTestHooks HOOKS{
		&CSharpEditor::DotNetToolchain::FindSDKRoot,
		&CSharpEditor::DotNetToolchain::GetRuntimeAssemblyPath,
		&CSharpEditor::CoreCLRHost::Start,
		&CSharpEditor::CoreCLRHost::GetEntry,
		&CSharpImpl::ScriptRuntime::MakeEngineAPI,
		&CSharpImpl::ScriptRuntime::AttachAs,
		&CSharpImpl::ScriptRuntime::Detach,
		&CSharpImpl::ScriptRuntime::IsAttached,
		&CSharpImpl::ScriptRuntime::GetManaged,
		&CSharpImpl::ScriptRuntime::UnloadUserAssemblies,
		&CSharpImpl::ManagedTypes::GetCounters,
		&CSharpImpl::ManagedTypes::GetOwnerToken,
		&CSharpEditor::InstallComponentMenu,
		&CSharpImpl::BindingProbe::TakeRecords,
		&CSharpImpl::InternLogOrigin,
		&CSharpImpl::BindingProbe::SetDriver,
		&CSharpImpl::BindingProbe::SetAssetFinder,
		&CSharpImpl::InputMaps::GetAliveCount,
		&CSharpEditor::RegisterDebugLineGizmo,
		CSharpEditor::DotNetPinData::RUNTIME_VERSION,
	};
	return &HOOKS;
}

#endif
