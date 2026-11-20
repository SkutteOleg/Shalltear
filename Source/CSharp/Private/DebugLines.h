#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl::DebugLines {

	void Draw(const Vec3& from, const Vec3& to, const Color& color, f32 seconds, bool depthTested) noexcept;

	void Collect(Vector<DebugLine>& lines, Vector<DebugLine>& overlayLines);

	void EndFrame() noexcept;

	u32 GetCount() noexcept;

	void Clear() noexcept;

} // namespace Shalltear::CSharpImpl::DebugLines
