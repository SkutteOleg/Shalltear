#pragma once

#include "ManagedTypes.h"

#include "Shalltear/Core/Types.h"

#include <span>
#include <vector>

namespace Shalltear::CSharpScenarios {

	struct FieldHost {
		std::vector<str> (*takeRecords)();
		CSharpImpl::ManagedTypes::Counters& (*getCounters)();
	};

	struct FieldScenario {
		const char* name;
		void (*run)(const FieldHost& host);
	};

	std::span<const FieldScenario> GetFieldScenarios();

	class FieldProject {
	public:
		FieldProject();
		~FieldProject();
	private:
		str _root;
		char _token = 0;
	};

} // namespace Shalltear::CSharpScenarios
