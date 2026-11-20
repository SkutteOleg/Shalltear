using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear;

public sealed partial class Scene {
	/// <summary>Fill results with this scene's objects, children included.</summary>
	[SkipLocalsInit]
	public void GetObjects(List<SceneObject> results) {
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<ulong> ids = stackalloc ulong[ObjectCache.STACK_IDS];
		ulong[]? rented = null;
		int count = FillObjects(ids);
		if (count > ids.Length) {
			rented = ArrayPool<ulong>.Shared.Rent(count);
			ids = rented;
			count = Math.Min(FillObjects(ids), ids.Length);
		}
		foreach (ulong id in ids.Slice(0, count)) {
			SceneObject? found = ObjectCache.SceneObject(id);
			if (found is not null) {
				results.Add(found);
			}
		}
		if (rented != null) {
			ArrayPool<ulong>.Shared.Return(rented);
		}
	}
}
