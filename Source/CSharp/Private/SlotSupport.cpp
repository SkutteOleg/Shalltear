#include "SlotSupport.h"

#include <vector>

namespace Shalltear::CSharpImpl::Slots {

	namespace {

		constexpr u32 KEPT_TEXT_COUNT = 16;

		// damn glibc won't unmap a lib with live thread_local destructors
		[[clang::no_destroy]] thread_local std::vector<u64> _keptBytes;
		[[clang::no_destroy]] thread_local str _keptTexts[KEPT_TEXT_COUNT];
		thread_local u32 _keptTextAt = 0;

	} // anonymous namespace

	void* KeepBytes(size_t size) {
		_keptBytes.resize(size / sizeof(u64) + 1);
		return _keptBytes.data();
	}

	SltString KeepText(std::string_view text) {
		str& kept = _keptTexts[_keptTextAt++ % KEPT_TEXT_COUNT];
		kept.assign(text);
		return SltString{kept.data(), static_cast<int32_t>(kept.size())};
	}

} // namespace Shalltear::CSharpImpl::Slots
