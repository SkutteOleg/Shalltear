#include "CoreCLRHost.h"
#include "DotNetToolchain.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Log.h"

#include "Shalltear/Editor/EditorExtensions.h"

#include <chrono>
#include <format>

namespace Shalltear::CSharpEditor {

	namespace {

		bool StartRuntime(str& outError) {
			const str sdkRoot = DotNetToolchain::FindSDKRoot();
			if (sdkRoot.empty()) {
				outError = std::format(".NET SDK {} was not found. Run python Scripts/fetch_prebuilt.py --dotnet", SLT_DOTNET_SDK_VERSION);
				return false;
			}
			const SltInitializeFn entry =
				CoreCLRHost::Start(sdkRoot, SLT_DOTNET_RUNTIME_VERSION, DotNetToolchain::GetRuntimeAssemblyPath(), outError);
			return entry && CSharpImpl::ScriptRuntime::Attach(entry, outError);
		}

	} // anonymous namespace

	void RegisterEditorExtensions(const void* owner) {
		(void)owner;
		const auto begin = std::chrono::steady_clock::now();
		str error;
		if (!StartRuntime(error)) {
			SLT_LOG_ERROR("C#", "{}", error);
			return;
		}
		SLT_LOG_INFO("C#", "C# runtime ready in {:.0f} ms",
					 std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - begin).count());
	}

} // namespace Shalltear::CSharpEditor

SLT_IMPLEMENT_EDITOR_EXTENSIONS(&Shalltear::CSharpEditor::RegisterEditorExtensions)
