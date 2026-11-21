#include "SlotSupport.h"

#include "ScriptRuntime.h"

#include <vector>

namespace Shalltear::CSharpImpl::Slots {

	namespace {

		// must match KEPT_LISTS and KEPT_TEXTS in Scripts/generate_csharp_bindings.py
		constexpr u32 KEPT_LIST_COUNT = 8;
		constexpr u32 KEPT_TEXT_COUNT = 16;

		// damn glibc won't unmap a lib with live thread_local destructors
		[[clang::no_destroy]] thread_local std::vector<u64> _keptBytes[KEPT_LIST_COUNT];
		thread_local u32 _keptBytesAt = 0;
		[[clang::no_destroy]] thread_local str _keptTexts[KEPT_TEXT_COUNT];
		thread_local u32 _keptTextAt = 0;

	} // anonymous namespace

	void* KeepBytes(size_t size) {
		std::vector<u64>& kept = _keptBytes[_keptBytesAt++ % KEPT_LIST_COUNT];
		kept.resize(size / sizeof(u64) + 1);
		return kept.data();
	}

	SltString KeepText(std::string_view text) {
		str& kept = _keptTexts[_keptTextAt++ % KEPT_TEXT_COUNT];
		kept.assign(text);
		return SltString{kept.data(), static_cast<int32_t>(kept.size())};
	}

	CallbackHold::~CallbackHold() {
		if (ScriptRuntime::IsAttached()) {
			ScriptRuntime::GetManaged().releaseCallback(id);
		}
	}

	int32_t InvokeCallback(const CallbackHold& hold, const u64* arguments, u64* results) {
		if (!ScriptRuntime::IsAttached()) {
			return SLT_ERR_REFUSED;
		}
		return ScriptRuntime::GetManaged().invokeCallback(hold.id, arguments, results);
	}

} // namespace Shalltear::CSharpImpl::Slots
