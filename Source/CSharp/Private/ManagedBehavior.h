#pragma once

#include "ManagedTypes.h"

#include "Shalltear/Core/Scene/Behavior.h"

#include <cstdint>

namespace Shalltear::CSharpImpl {

	class ManagedBehavior final : public Behavior {
	public:
		explicit ManagedBehavior(ManagedType& type);
		~ManagedBehavior() override;

		const TypeInfo* GetType() const override { return _type.info; }

		void ReleaseInstance();

		ManagedBehavior* previous = nullptr;
		ManagedBehavior* next = nullptr;
	protected:
		void OnAttach() override { Invoke(SLT_HOOK_ATTACH); }
		void OnDetach() override { Invoke(SLT_HOOK_DETACH); }
		void OnEnable() override { Invoke(SLT_HOOK_ENABLE); }
		void OnDisable() override { Invoke(SLT_HOOK_DISABLE); }
		void OnDestroy() override { Invoke(SLT_HOOK_DESTROY); }
		void OnUpdate() override { Invoke(SLT_HOOK_UPDATE); }
		void OnFixedUpdate() override { Invoke(SLT_HOOK_FIXED_UPDATE); }
		void OnLateUpdate() override { Invoke(SLT_HOOK_LATE_UPDATE); }
	private:
		void Invoke(i32 hook);

		ManagedType& _type;
		intptr_t _instance = 0;
	};

} // namespace Shalltear::CSharpImpl
