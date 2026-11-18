#ifdef SLT_CSHARP_TEST_HOOKS

#include "EditorTestHooks.h"

#include "CSharpSettings.h"
#include "HostFxr.h"
#include "SHA512.h"
#include "SystemTools.h"

#include "Shalltear/Core/Export.h"

#include <chrono>
#include <thread>

namespace {

	using namespace Shalltear;
	using namespace Shalltear::CSharpEditor;

	bool RunAcquire(const AcquireOptions& options, AcquireStep cancelAt, AcquireStatus& outStatus) {
		DotNetAcquire acquire(options);
		acquire.Start();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
		while (!acquire.IsFinished()) {
			if ((cancelAt != AcquireStep::Idle && acquire.GetStatus().step == cancelAt) || std::chrono::steady_clock::now() > deadline) {
				acquire.Cancel();
				break;
			}
			acquire.Pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		outStatus = acquire.GetStatus();
		return outStatus.step == AcquireStep::Done;
	}

	void DeclareSettings(const void* owner) {
		CSharpSettings::Declare();
		CSharpSettings::RegisterPage(owner);
	}

} // anonymous namespace

extern "C" SLT_API_EXPORT const Shalltear::CSharpEditor::EditorTestHooks* ShalltearCSharpEditorTestHooks() {
	static const EditorTestHooks HOOKS{
		&HashFile,
		&DotNetToolchain::GetPin,
		&DotNetToolchain::Discover,
		&DotNetToolchain::Describe,
		&RunAcquire,
		&DescribeAcquireStep,
		&FindSystemTool,
		&MakeLongPath,
		&CSharpProjectFiles::GetDefaultOptions,
		&CSharpProjectFiles::Write,
		&CSharpSetup::Begin,
		&CSharpSetup::Pump,
		&CSharpSetup::Shutdown,
		&CSharpSetup::GetState,
		&CSharpSetup::GetSDKRoot,
		&DeclareSettings,
		HostFxr::GetMuxerName().data(),
		HostFxr::GetLibraryName().data(),
	};
	return &HOOKS;
}

#endif
