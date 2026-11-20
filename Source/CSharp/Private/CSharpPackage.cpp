#include "DebugLines.h"
#include "NativeAOTHost.h"
#include "ScriptRuntime.h"
#include "UIBindings.h"

#ifdef SLT_WITH_EDITOR
#include "CSharpBuildSteps.h"
#include "CSharpScriptAsset.h"
#include "CSharpSetup.h"
#endif

#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Object.h"

namespace Shalltear::CSharpImpl {

	namespace {

#ifndef SLT_WITH_EDITOR
		class PlayerStart final : public Object {
		public:
			void OnExtensionsRegistered(const EmptyEventPayload&) { NativeAOTHost::StartPlayer(); }
		};

		PlayerStart* _playerStart = nullptr;

		void ReleasePlayerStart() {
			if (_playerStart) {
				EventBus::GetGlobal().UnsubscribeAll(_playerStart->GetInstanceId());
				Delete(_playerStart);
				_playerStart = nullptr;
			}
		}
#endif

		void TickStage(const FrameStageArgs&, void*) {
#ifndef SLT_WITH_EDITOR
			ReleasePlayerStart();
#endif
			ScriptRuntime::Tick();
			DebugLines::EndFrame();
			UIBindings::FlushPending();
		}

	} // anonymous namespace

	void RegisterPackage(const void* owner) {
		FrameStages::Register(FrameStage::EndFrame, FrameStagePosition::Late, &TickStage, nullptr, owner);
#ifdef SLT_WITH_EDITOR
		CSharpEditor::CSharpScripts::RegisterImporter(owner);
		CSharpEditor::CSharpBuildSteps::Register(owner);
#else
		ReleasePlayerStart();
		_playerStart = New<PlayerStart>(MemTag::Scripting);
		ModuleEvents::ExtensionsRegistered.Subscribe(EventBus::GetGlobal(), Handle<PlayerStart>(_playerStart), &PlayerStart::OnExtensionsRegistered);
#endif
	}

	void ShutdownPackage() {
#ifdef SLT_WITH_EDITOR
		CSharpEditor::CSharpSetup::Shutdown();
		CSharpEditor::CSharpBuildSteps::Shutdown();
#else
		ReleasePlayerStart();
#endif
		ScriptRuntime::Detach();
		DebugLines::Clear();
		UIBindings::Release();
	}

} // namespace Shalltear::CSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(CSharp, &Shalltear::CSharpImpl::RegisterPackage, &Shalltear::CSharpImpl::ShutdownPackage)
