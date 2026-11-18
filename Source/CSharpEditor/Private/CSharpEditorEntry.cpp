#include "Shalltear/Editor/EditorExtensions.h"

namespace Shalltear::CSharpEditor {

	void RegisterEditorExtensions(const void* owner) {
		(void)owner;
	}

} // namespace Shalltear::CSharpEditor

SLT_IMPLEMENT_EDITOR_EXTENSIONS(&Shalltear::CSharpEditor::RegisterEditorExtensions)
