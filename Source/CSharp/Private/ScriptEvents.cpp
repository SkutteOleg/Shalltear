#include "ScriptEvents.h"

#include "ManagedTypes.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Scene/SceneEvents.h"

#include <stdexcept>

namespace Shalltear::CSharpImpl::ScriptEvents {

	namespace {

		u32 _listening = 0;

		void Dispatch(Event event, u64 subject) {
			if (!(_listening & (1u << static_cast<u32>(event))) || !ScriptRuntime::IsAttached()) {
				return;
			}
			ManagedTypes::FlushDirty();
			ScriptRuntime::GetManaged().dispatchEvent(static_cast<int32_t>(event), subject);
		}

		class Relay final : public Object {
		public:
			void OnSceneLoaded(const SceneLoadedEvent& event) { Dispatch(Event::SceneLoaded, event.scene.GetInstanceId()); }
			void OnSceneUnloading(const SceneUnloadingEvent& event) { Dispatch(Event::SceneUnloading, event.scene.GetInstanceId()); }
		};

		Relay* _relay = nullptr;

	} // anonymous namespace

	void SetListening(i32 event, bool listening) {
		if (event < 0 || event >= static_cast<i32>(Event::Count)) {
			throw std::runtime_error("unknown script event");
		}
		if (listening && !_relay) {
			_relay = New<Relay>(MemTag::Scripting);
			SceneEvents::Loaded.Subscribe(EventBus::GetGlobal(), Handle<Relay>(_relay), &Relay::OnSceneLoaded);
			SceneEvents::Unloading.Subscribe(EventBus::GetGlobal(), Handle<Relay>(_relay), &Relay::OnSceneUnloading);
		}
		const u32 bit = 1u << static_cast<u32>(event);
		_listening = listening ? _listening | bit : _listening & ~bit;
	}

	void Reset() {
		if (_relay) {
			EventBus::GetGlobal().UnsubscribeAll(_relay->GetInstanceId());
			Delete(_relay);
			_relay = nullptr;
		}
		_listening = 0;
	}

} // namespace Shalltear::CSharpImpl::ScriptEvents
