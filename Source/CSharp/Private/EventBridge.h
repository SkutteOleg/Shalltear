#pragma once

#include "ScriptAPI.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include <functional>
#include <utility>

namespace Shalltear::CSharpImpl::EventBridge {

	SHALLTEAR_CSHARP_API u64 SubscribeRaw(StringHash eventId, std::function<void(const void*)> handler);

	// handler freed at next Collect (caller may be the handler)
	SHALLTEAR_CSHARP_API void Unsubscribe(u64 subscription);

	void Collect();

	void UnsubscribeAll();

	template <typename TPayload>
	u64 Subscribe(const Event<TPayload>& event, std::function<void(const TPayload&)> handler) {
		if (!handler) {
			return 0;
		}
		return SubscribeRaw(event.GetId(), [handler = std::move(handler)](const void* payload) { handler(*static_cast<const TPayload*>(payload)); });
	}

	template <typename TPayload>
	u64 Subscribe(const Event<TPayload>& event, std::function<void()> handler) {
		if (!handler) {
			return 0;
		}
		return SubscribeRaw(event.GetId(), [handler = std::move(handler)](const void*) { handler(); });
	}

} // namespace Shalltear::CSharpImpl::EventBridge
