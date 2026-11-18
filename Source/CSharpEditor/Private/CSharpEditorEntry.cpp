#include "CSharpAuthoring.h"
#include "CSharpComponentMenu.h"
#include "CSharpProjectFiles.h"
#include "CSharpScriptEditor.h"
#include "CSharpSettings.h"
#include "CSharpSetup.h"

#include "Shalltear/Core/Frame/FrameStages.h"

#include "Shalltear/Editor/EditorExtensions.h"

namespace Shalltear::CSharpEditor {

	namespace {

		void PumpStage(const FrameStageArgs&, void*) {
			CSharpSetup::Pump();
			CSharpAuthoring::Pump();
		}

	} // anonymous namespace

	void RegisterEditorExtensions(const void* owner) {
		CSharpSettings::Declare();
		CSharpSettings::RegisterPage(owner);
		InstallComponentMenu();
		RegisterScriptEditor(owner);
		CSharpProjectFiles::RegisterAttachConfiguration(owner);
		FrameStages::Register(FrameStage::EndFrame, FrameStagePosition::Normal, &PumpStage, nullptr, owner);
		CSharpSetup::Begin(CSharpSetup::GetDefaultOptions(), owner);
	}

} // namespace Shalltear::CSharpEditor

SLT_IMPLEMENT_EDITOR_EXTENSIONS(&Shalltear::CSharpEditor::RegisterEditorExtensions)
