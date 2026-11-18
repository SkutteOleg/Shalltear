#include "ScriptRuntime.h"

#ifdef SLT_WITH_EDITOR
#include "CSharpSetup.h"
#endif

#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::CSharpImpl {

	namespace {

		void TickStage(const FrameStageArgs&, void*) {
			ScriptRuntime::Tick();
		}

	} // anonymous namespace

	void RegisterPackage(const void* owner) {
		FrameStages::Register(FrameStage::EndFrame, FrameStagePosition::Late, &TickStage, nullptr, owner);
	}

	void ShutdownPackage() {
#ifdef SLT_WITH_EDITOR
		CSharpEditor::CSharpSetup::Shutdown();
#endif
		ScriptRuntime::Detach();
	}

} // namespace Shalltear::CSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(CSharp, &Shalltear::CSharpImpl::RegisterPackage, &Shalltear::CSharpImpl::ShutdownPackage)
