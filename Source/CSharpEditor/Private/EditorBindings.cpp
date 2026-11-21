#include "EditorBindings.h"

#include "PackageBindings.h"

#include "Generated/EditorBindings.generated.h"

namespace Shalltear::CSharpEditor::EditorBindings {

	namespace {

		SltEditorBindings _table{};

	} // anonymous namespace

	void Register() {
		CSharpImpl::Generated::FillEditorTable(_table);
		CSharpImpl::PackageBindings::Add(Name, &_table, static_cast<i32>(sizeof(_table)), SLT_EDITOR_LAYOUT_HASH);
	}

} // namespace Shalltear::CSharpEditor::EditorBindings
