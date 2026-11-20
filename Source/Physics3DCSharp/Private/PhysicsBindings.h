#pragma once

#include "Generated/Physics3DBindings.generated.h"

#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Physics3D/Physics.h"

#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::PhysicsBindings {

	bool CastRay(const Vec3& origin, const Vec3& direction, f32 maxDistance, std::string_view categories, PhysicsCategoryMatch collidesWithMode,
				 std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit);
	bool CastSphere(const Vec3& origin, const Vec3& direction, f32 radius, f32 maxDistance, std::string_view categories,
					PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit);
	bool CastCapsule(const Vec3& point1, const Vec3& point2, f32 radius, const Vec3& direction, f32 maxDistance, std::string_view categories,
					 PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit);

	// returned count may exceed ids.size()
	i32 FindOverlaps(const Vec3& center, f32 radius, std::string_view categories, PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith,
					 PhysicsQueryTriggers triggers, std::span<u64> ids);

	// managed function pointers, must match PhysicsEvents.cs
	i32 AttachEvents(i64 collision, i64 trigger, i64 jointBreak, i64 controllerHit);

	void Register();
	void Unregister();

} // namespace Shalltear::CSharpImpl::PhysicsBindings
