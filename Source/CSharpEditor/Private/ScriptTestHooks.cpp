#ifdef SLT_CSHARP_TEST_HOOKS

#include "ScriptTestHooks.h"

#include "CoreCLRHost.h"
#include "DotNetToolchain.h"
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
		SLT_DOTNET_RUNTIME_VERSION,
	};
	return &HOOKS;
}

#endif
