using System;
using System.Buffers;
using System.Collections.Generic;
using Shalltear.Interop;

namespace Shalltear;

/// <summary>Cameras drawn by the renderer.</summary>
public static partial class RenderSystem {
	/// <summary>Fill results with all enabled cameras (cleared 1st).</summary>
	public static void GetCameras(List<Camera> results) {
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<ulong> ids = stackalloc ulong[ObjectCache.STACK_IDS];
		ulong[]? rented = null;
		int count = CameraListBindings.FillCameras(ids);
		if (count > ids.Length) {
			rented = ArrayPool<ulong>.Shared.Rent(count);
			ids = rented;
			count = Math.Min(CameraListBindings.FillCameras(ids), ids.Length);
		}
		foreach (ulong id in ids.Slice(0, count)) {
			Camera? found = ObjectCache.Find<Camera>(id);
			if (found is not null) {
				results.Add(found);
			}
		}
		if (rented != null) {
			ArrayPool<ulong>.Shared.Return(rented);
		}
	}
}

public sealed partial class Camera {
	/// <summary>Part of the target drawn, normalized, (0, 0) at bottom left.</summary>
	public RectF ViewportRect {
		get {
			Vec4 rect = ViewportVector;
			return new RectF(rect.x, rect.y, rect.z, rect.w);
		}
		set => ViewportVector = new Vec4(value.position.x, value.position.y, value.size.x, value.size.y);
	}
}

public sealed partial class Light {
	/// <summary>Lowest ShadowNearPlane.</summary>
	public const float MIN_SHADOW_NEAR_PLANE = 0.01f;

	/// <summary>ShadowNearPlane of a new light.</summary>
	public const float DEFAULT_SHADOW_NEAR_PLANE = 0.2f;
}

public sealed partial class Material {
	/// <summary>Color parameter name (white when none).</summary>
	public Color GetColor(string name) {
		return GetColor(name, Color.White);
	}
}

public sealed partial class Skeleton {
	/// <summary>Set bone's pose in its parent's space.</summary>
	public void SetBoneLocalPose(int bone, BoneTransform transform) {
		WriteBoneLocalPose(bone, transform.position, transform.rotation, transform.scale);
	}
}

public partial struct BoneTransform {
	/// <summary>Pose at position with rotation + scale.</summary>
	public BoneTransform(Vec3 position, Quat rotation, Vec3 scale) {
		this.position = position;
		this.rotation = rotation;
		this.scale = scale;
	}

	/// <summary>No offset, rotation or scaling.</summary>
	public static BoneTransform Identity => new(Vec3.Zero, Quat.Identity, Vec3.One);
}
