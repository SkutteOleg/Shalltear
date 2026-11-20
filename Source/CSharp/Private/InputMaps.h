#pragma once

#include "Shalltear/Core/Input/InputAction.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::InputMaps {

	u32 Create();
	void Destroy(u32 map);
	void DestroyAll();

	u32 RegisterAction(u32 map, std::string_view name, ActionType type);
	u32 FindAction(u32 map, std::string_view name);
	void Bind(u32 map, u32 action, u8 source, u16 code, KeyMod requiredMods, bool exactMods, f32 scale, u8 component, u8 gamepadIndex);
	void ClearBindings(u32 map, u32 action);
	void SetEnabled(u32 map, bool enabled);
	void Evaluate(u32 map);

	bool IsPressed(u32 map, u32 action) noexcept;
	bool WasPerformed(u32 map, u32 action) noexcept;
	f32 GetAxis1D(u32 map, u32 action) noexcept;
	Vec2 GetAxis2D(u32 map, u32 action) noexcept;
	i32 GetActionCount(u32 map) noexcept;

#ifdef SLT_CSHARP_TEST_HOOKS
	i32 GetAliveCount();
#endif

} // namespace Shalltear::CSharpImpl::InputMaps
