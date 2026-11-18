#include "CSharpComponentMenu.h"

#include "ManagedTypes.h"

#include "Shalltear/Editor/ComponentMenu.h"

namespace Shalltear::CSharpEditor {

	namespace {

		void OnTypeRegistered(const TypeInfo* type, std::string_view fullName, const void* ownerToken) {
			if (type->flags & TypeFlagAbstract) {
				return;
			}
			const size_t dot = fullName.rfind('.');
			const str category = dot == std::string_view::npos ? str("Scripts") : "Scripts/" + str(fullName.substr(0, dot));
			ComponentMenu::Register(type->name, category, ownerToken);
		}

		void OnTypesReleased(const void* ownerToken) {
			ComponentMenu::UnregisterOwnedBy(ownerToken);
		}

	} // anonymous namespace

	void InstallComponentMenu() {
		CSharpImpl::ManagedTypes::SetTypeObserver({&OnTypeRegistered, &OnTypesReleased});
	}

} // namespace Shalltear::CSharpEditor
