#include "ScriptLog.h"

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl {

	void WriteLog(LogLevel level, std::string_view category, std::string_view message) {
		if (level < LogManager::GetGlobalLevel()) {
			return;
		}
		const str name(category);
		LogManager::Dispatch(level, name.c_str(), "", 0, "", "{}", message);
	}

} // namespace Shalltear::CSharpImpl
