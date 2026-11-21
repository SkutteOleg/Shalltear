#include "FunctionBindings.h"

#include "ManagedTypes.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Module/ScriptFunctions.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::FunctionBindings {

	namespace {

		u32 _seenEpoch = 0;

		void CallExport(u64 context, const u64* arguments, u64* result) {
			if (ScriptRuntime::IsAttached()) {
				ScriptRuntime::GetManaged().callExport(context, arguments, result);
			}
		}

	} // anonymous namespace

	void Find(std::string_view name, std::string_view signature, std::span<int64_t> out) {
		str registered;
		const ScriptFunctions::Found found = ScriptFunctions::FindRaw(name, &registered);
		if (!found.thunk) {
			throw std::runtime_error(std::format("native function '{}' isn't exported. Add SLT_SCRIPT_EXPORT({}) to the project's C++ (or load its module)", name, name));
		}
		if (registered != signature) {
			throw std::runtime_error(std::format("native function '{}' is exported as {} (declared as {} in C#). Make both signatures match", name, registered, signature));
		}
		if (out.size() < 2) {
			throw std::runtime_error("native function lookup needs 2 result slots");
		}
		out[0] = static_cast<int64_t>(reinterpret_cast<intptr_t>(found.thunk));
		out[1] = static_cast<int64_t>(found.context);
	}

	void Export(std::string_view name, std::string_view signature, u64 id) {
		if (!ScriptFunctions::Register(name, signature, &CallExport, id, ManagedTypes::GetOwnerToken())) {
			throw std::runtime_error(std::format("'{}' is already exported. Rename one of the 2 functions", name));
		}
	}

	void Poll() {
		const u32 epoch = ScriptFunctions::GetEpoch();
		if (epoch != _seenEpoch && ScriptRuntime::IsAttached()) {
			_seenEpoch = epoch;
			ScriptRuntime::GetManaged().functionsChanged();
		}
	}

	void Reset() {
		_seenEpoch = 0;
	}

} // namespace Shalltear::CSharpImpl::FunctionBindings
