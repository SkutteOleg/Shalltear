#include "SlotSupport.h"

#include "ScriptRuntime.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
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

		struct SharedEntry {
			std::shared_ptr<void> held;
			u32 count = 0;
		};

		std::mutex _sharedGate;
		std::unordered_map<const void*, SharedEntry> _shared;

		struct FieldEntry {
			u64 field = 0;
			u64 group = 0;
			std::shared_ptr<const void> held;
		};

		struct KeptTexts {
			str blob;
			std::vector<std::string_view> views;
		};

		std::mutex _fieldGate;
		std::unordered_map<const void*, std::vector<FieldEntry>> _fields;

		// _fieldGate held by caller
		void Replace(const void* object, u64 field, std::shared_ptr<const void> held) {
			std::vector<FieldEntry>& entries = _fields[object];
			std::erase_if(entries, [field](const FieldEntry& entry) { return entry.group == field; });
			entries.push_back(FieldEntry{field, field, std::move(held)});
		}

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

	u64 KeepSharedRaw(std::shared_ptr<void> held) {
		const void* const pointer = held.get();
		if (!pointer) {
			return 0;
		}
		const std::lock_guard lock(_sharedGate);
		SharedEntry& entry = _shared[pointer];
		if (entry.count++ == 0) {
			entry.held = std::move(held);
		}
		return static_cast<u64>(reinterpret_cast<uintptr_t>(pointer));
	}

	std::shared_ptr<void> FindShared(const void* pointer) {
		const std::lock_guard lock(_sharedGate);
		const auto found = _shared.find(pointer);
		return found != _shared.end() ? found->second.held : nullptr;
	}

	void DropShared(const void* pointer) {
		std::shared_ptr<void> freed;
		const std::lock_guard lock(_sharedGate);
		const auto found = _shared.find(pointer);
		if (found != _shared.end() && --found->second.count == 0) {
			freed = std::move(found->second.held);
			_shared.erase(found);
		}
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

	std::string_view KeepFieldText(const void* object, u64 field, std::string_view text) {
		auto kept = std::make_shared<str>(text);
		const std::string_view view(*kept);
		const std::lock_guard lock(_fieldGate);
		Replace(object, field, std::move(kept));
		return view;
	}

	std::span<const std::string_view> KeepFieldTexts(const void* object, u64 field, std::span<const std::string_view> texts) {
		auto kept = std::make_shared<KeptTexts>();
		size_t size = 0;
		for (const std::string_view text : texts) {
			size += text.size() + 1;
		}
		kept->blob.reserve(size);
		std::vector<size_t> starts;
		starts.reserve(texts.size());
		for (const std::string_view text : texts) {
			starts.push_back(kept->blob.size());
			kept->blob.append(text);
			kept->blob.push_back('\0');
		}
		kept->views.reserve(texts.size());
		for (size_t index = 0; index < texts.size(); ++index) {
			kept->views.emplace_back(kept->blob.data() + starts[index], texts[index].size());
		}
		const std::span<const std::string_view> view(kept->views);
		const std::lock_guard lock(_fieldGate);
		Replace(object, field, std::move(kept));
		return view;
	}

	std::span<const std::byte> KeepFieldBytes(const void* object, u64 field, std::span<const std::byte> bytes) {
		std::shared_ptr<std::byte[]> kept(new std::byte[std::max<size_t>(bytes.size(), 1)]);
		if (!bytes.empty()) {
			std::memcpy(kept.get(), bytes.data(), bytes.size());
		}
		const std::span<const std::byte> view(kept.get(), bytes.size());
		const std::lock_guard lock(_fieldGate);
		Replace(object, field, std::shared_ptr<const void>(kept, kept.get()));
		return view;
	}

	void KeepFieldHeld(const void* object, u64 field, std::shared_ptr<const void> held, std::span<const void* const> sources) {
		const std::lock_guard lock(_fieldGate);
		std::vector<FieldEntry> shared;
		for (const void* source : sources) {
			const auto found = _fields.find(source);
			if (found == _fields.end()) {
				continue;
			}
			for (const FieldEntry& entry : found->second) {
				shared.push_back(FieldEntry{entry.field, field, entry.held});
			}
		}
		Replace(object, field, std::move(held));
		std::vector<FieldEntry>& entries = _fields[object];
		entries.insert(entries.end(), shared.begin(), shared.end());
	}

	void ForgetFields(const void* object) {
		std::vector<FieldEntry> freed;
		const std::lock_guard lock(_fieldGate);
		const auto found = _fields.find(object);
		if (found != _fields.end()) {
			freed = std::move(found->second);
			_fields.erase(found);
		}
	}

#ifdef SLT_CSHARP_TEST_HOOKS
	size_t CountKeptFields() {
		const std::lock_guard lock(_fieldGate);
		size_t count = 0;
		for (const auto& [object, entries] : _fields) {
			count += entries.size();
		}
		return count;
	}
#endif

	void ForgetAllFields() {
		std::unordered_map<const void*, std::vector<FieldEntry>> freed;
		const std::lock_guard lock(_fieldGate);
		freed.swap(_fields);
	}

} // namespace Shalltear::CSharpImpl::Slots
