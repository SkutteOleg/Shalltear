#include "EventBridge.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"

namespace Shalltear::CSharpImpl::EventBridge {

	namespace {

		class Relay final : public Object {
		public:
			void On(const u8& payload) {
				if (handler) {
					handler(&payload);
				}
			}

			std::function<void(const void*)> handler;
		};

		Vector<Relay*> _relays;
		Vector<Relay*> _retired;

	} // anonymous namespace

	u64 SubscribeRaw(StringHash eventId, std::function<void(const void*)> handler) {
		if (!handler) {
			return 0;
		}
		Relay* relay = New<Relay>(MemTag::Scripting);
		relay->handler = std::move(handler);
		_relays.push_back(relay);
		EventBus::GetGlobal().Subscribe<u8, Relay, const u8&>(eventId, Handle<Relay>(relay), &Relay::On);
		return relay->GetInstanceId();
	}

	void Unsubscribe(u64 subscription) {
		for (size_t index = 0; index < _relays.size(); ++index) {
			if (_relays[index]->GetInstanceId() == subscription) {
				EventBus::GetGlobal().UnsubscribeAll(subscription);
				_retired.push_back(_relays[index]);
				_relays.erase(_relays.begin() + static_cast<ptrdiff_t>(index));
				return;
			}
		}
	}

	void Collect() {
		for (Relay* relay : _retired) {
			Delete(relay);
		}
		_retired.clear();
	}

	void UnsubscribeAll() {
		for (Relay* relay : _relays) {
			EventBus::GetGlobal().UnsubscribeAll(relay->GetInstanceId());
			Delete(relay);
		}
		_relays.clear();
		Collect();
	}

} // namespace Shalltear::CSharpImpl::EventBridge
