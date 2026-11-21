using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Object in a scene.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class SceneObject : EngineObject {
	internal const string ENGINE_TYPE = "SceneObject";
	const int STACK_HANDLES = 32;
	const int EVERY_SCRIPT = 0;

	internal SceneObject(ulong id) {
		_id = id;
	}

	/// <summary>Move under parent (null = scene root). World transform unchanged.</summary>
	public void SetParent(SceneObject? parent) {
		SetParent(parent, true);
	}

	/// <summary>Destroy with children + components at end of frame.</summary>
	public void Destroy() {
		ObjectBindings.Destroy(_id, "SceneObject.Destroy");
	}

	/// <summary>1st component of type T on this object (null when none).</summary>
	[SkipLocalsInit]
	public T? GetComponent<T>()
		where T : class {
		const string MEMBER = "SceneObject.GetComponent";
		int script = ScriptIndex<T>.value;
		if (script != 0) {
			long found = ObjectBindings.FindScript(this, script, MEMBER);
			return found == 0 ? null : GCHandle.FromIntPtr((nint)found).Target as T;
		}
		ulong engineType = EngineTypeOf<T>.hash;
		if (engineType != 0) {
			return ObjectCache.Find(ObjectBindings.FindComponent(this, engineType, MEMBER)) as T;
		}
		Span<long> handles = stackalloc long[STACK_HANDLES];
		long[]? rented = FindAll(EVERY_SCRIPT, ref handles, MEMBER);
		T? match = null;
		foreach (long handle in handles) {
			match = GCHandle.FromIntPtr((nint)handle).Target as T;
			if (match != null) {
				break;
			}
		}
		if (rented != null) {
			ArrayPool<long>.Shared.Return(rented);
		}
		return match;
	}

	/// <summary>Fill results with each component of type T on this object.</summary>
	[SkipLocalsInit]
	public void GetComponents<T>(List<T> results)
		where T : class {
		const string MEMBER = "SceneObject.GetComponents";
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		ulong engineType = EngineTypeOf<T>.hash;
		if (engineType != 0) {
			Span<ulong> ids = stackalloc ulong[STACK_HANDLES];
			int count = ObjectBindings.FindComponents(this, engineType, ids, MEMBER);
			ulong[]? rentedIds = null;
			if (count > ids.Length) {
				rentedIds = ArrayPool<ulong>.Shared.Rent(count);
				ids = rentedIds;
				count = Math.Min(ObjectBindings.FindComponents(this, engineType, ids, MEMBER), ids.Length);
			}
			foreach (ulong id in ids.Slice(0, count)) {
				if (ObjectCache.Find(id) is T found) {
					results.Add(found);
				}
			}
			if (rentedIds != null) {
				ArrayPool<ulong>.Shared.Return(rentedIds);
			}
			return;
		}
		Span<long> handles = stackalloc long[STACK_HANDLES];
		long[]? rented = FindAll(ScriptIndex<T>.value, ref handles, MEMBER);
		foreach (long handle in handles) {
			if (GCHandle.FromIntPtr((nint)handle).Target is T script) {
				results.Add(script);
			}
		}
		if (rented != null) {
			ArrayPool<long>.Shared.Return(rented);
		}
	}

	/// <summary>1st component of type T on this object or a descendant, parents first (null when none).</summary>
	public T? GetComponentInChildren<T>(bool includeInactive = false)
		where T : class {
		if (!includeInactive && !IsActiveInHierarchy) {
			return null;
		}
		T? found = GetComponent<T>();
		for (int index = 0, count = found == null ? ChildCount : 0; index < count && found == null; ++index) {
			found = GetChild(index)!.GetComponentInChildren<T>(includeInactive);
		}
		return found;
	}

	/// <summary>1st component of type T on this object or an ancestor (null when none).</summary>
	public T? GetComponentInParent<T>()
		where T : class {
		T? found = GetComponent<T>();
		for (SceneObject? node = Parent; found == null && node is not null; node = node.Parent) {
			found = node.GetComponent<T>();
		}
		return found;
	}

	/// <summary>Add a component of type T.</summary>
	public T AddComponent<T>()
		where T : Component {
		const string MEMBER = "SceneObject.AddComponent";
		int script = ScriptIndex<T>.value;
		ulong engineType = EngineTypeOf<T>.hash;
		if (script == 0 && engineType != 0) {
			return (T)ObjectCache.Find(ObjectBindings.AddComponent(this, engineType, MEMBER))!;
		}
		if (script == 0) {
			throw new EngineException(MEMBER + " can't add " + typeof(T).Name + " (not a registered script class)");
		}
		return (T)GCHandle.FromIntPtr((nint)ObjectBindings.AddScript(this, script, MEMBER)).Target!;
	}

	long[]? FindAll(int script, ref Span<long> handles, string member) {
		int count = ObjectBindings.FindScripts(this, script, handles, member);
		long[]? rented = null;
		if (count > handles.Length) {
			rented = ArrayPool<long>.Shared.Rent(count);
			handles = rented;
			count = Math.Min(ObjectBindings.FindScripts(this, script, handles, member), handles.Length);
		}
		handles = handles.Slice(0, count);
		return rented;
	}
}
