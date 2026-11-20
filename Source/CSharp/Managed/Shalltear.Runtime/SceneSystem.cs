using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear;

public static partial class SceneSystem {
	/// <summary>Instance of prefab (under parent, in scene or in PrimaryScene).</summary>
	public static SceneObject Instantiate(Prefab prefab, SceneObject? parent = null, Scene? scene = null) {
		return InstantiatePrefab(prefab, Vec3.Zero, Quat.Identity, parent, scene);
	}

	/// <summary>Instance of prefab at a local position + rotation.</summary>
	public static SceneObject Instantiate(Prefab prefab, Vec3 position, Quat rotation, SceneObject? parent = null, Scene? scene = null) {
		return InstantiatePrefab(prefab, position, rotation, parent, scene);
	}

	/// <summary>Fill results with loaded scenes.</summary>
	[SkipLocalsInit]
	public static void GetLoadedScenes(List<Scene> results) {
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<ulong> ids = stackalloc ulong[ObjectCache.STACK_IDS];
		ulong[]? rented = null;
		int count = FillLoadedScenes(ids);
		if (count > ids.Length) {
			rented = ArrayPool<ulong>.Shared.Rent(count);
			ids = rented;
			count = Math.Min(FillLoadedScenes(ids), ids.Length);
		}
		foreach (ulong id in ids.Slice(0, count)) {
			Scene? found = ObjectCache.Find<Scene>(id);
			if (found is not null) {
				results.Add(found);
			}
		}
		if (rented != null) {
			ArrayPool<ulong>.Shared.Return(rented);
		}
	}
}
