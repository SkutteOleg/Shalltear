using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Which colliders a query hits (default = all categories).</summary>
public struct PhysicsQueryFilter {
	/// <summary>Categories the query is in, comma separated.</summary>
	public string? categories;

	/// <summary>Whether collidesWith is a skip list or a hit list.</summary>
	public PhysicsCategoryMatch collidesWithMode;

	/// <summary>Category names for collidesWithMode, comma separated.</summary>
	public string? collidesWith;

	/// <summary>Whether trigger colliders are hit.</summary>
	public PhysicsQueryTriggers triggerInteraction;
}

public static unsafe partial class Physics {
	/// <summary>Longest query distance.</summary>
	public const float MAX_QUERY_DISTANCE = 1.0e6f;

	const int STACK_IDS = 64;

	/// <summary>Closest collider along ray (colliders around the ray's start missed).</summary>
	public static bool Raycast(Ray ray, out RaycastHit hit, float maxDistance = MAX_QUERY_DISTANCE, in PhysicsQueryFilter filter = default) {
		hit = default;
		return CastRay(ray.origin, ray.direction, maxDistance, filter.categories ?? "", filter.collidesWithMode, filter.collidesWith ?? "",
					   filter.triggerInteraction, new Span<RaycastHit>(ref hit), "Physics.Raycast");
	}

	/// <summary>Closest collider hit by a sphere swept along ray.</summary>
	public static bool SphereCast(Ray ray, float radius, out RaycastHit hit, float maxDistance = MAX_QUERY_DISTANCE, in PhysicsQueryFilter filter = default) {
		hit = default;
		return CastSphere(ray.origin, ray.direction, radius, maxDistance, filter.categories ?? "", filter.collidesWithMode, filter.collidesWith ?? "",
						  filter.triggerInteraction, new Span<RaycastHit>(ref hit), "Physics.SphereCast");
	}

	/// <summary>Closest collider hit by a capsule (between point1 + point2) swept along direction.</summary>
	public static bool CapsuleCast(Vec3 point1, Vec3 point2, float radius, Vec3 direction, out RaycastHit hit, float maxDistance = MAX_QUERY_DISTANCE,
								   in PhysicsQueryFilter filter = default) {
		hit = default;
		return CastCapsule(point1, point2, radius, direction, maxDistance, filter.categories ?? "", filter.collidesWithMode, filter.collidesWith ?? "",
						   filter.triggerInteraction, new Span<RaycastHit>(ref hit), "Physics.CapsuleCast");
	}

	/// <summary>Fill results with each collider touching the sphere. Returns their count.</summary>
	[SkipLocalsInit]
	public static int OverlapSphere(Vec3 center, float radius, List<Collider> results, in PhysicsQueryFilter filter = default) {
		const string MEMBER = "Physics.OverlapSphere";
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		string categories = filter.categories ?? "";
		string collidesWith = filter.collidesWith ?? "";
		Span<ulong> ids = stackalloc ulong[STACK_IDS];
		int count = FindOverlaps(center, radius, categories, filter.collidesWithMode, collidesWith, filter.triggerInteraction, ids, MEMBER);
		ulong[]? rented = null;
		if (count > ids.Length) {
			rented = ArrayPool<ulong>.Shared.Rent(count);
			ids = rented;
			count = Math.Min(FindOverlaps(center, radius, categories, filter.collidesWithMode, collidesWith, filter.triggerInteraction, ids, MEMBER), ids.Length);
		}
		foreach (ulong id in ids.Slice(0, count)) {
			if (ObjectCache.Find(id) is Collider collider) {
				results.Add(collider);
			}
		}
		if (rented != null) {
			ArrayPool<ulong>.Shared.Return(rented);
		}
		return results.Count;
	}
}
