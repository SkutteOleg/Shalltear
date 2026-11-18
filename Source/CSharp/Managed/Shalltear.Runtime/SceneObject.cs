using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Object in a scene.</summary>
public sealed partial class SceneObject : EngineObject {
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

	/// <summary>1st script of type T on this object (null when none).</summary>
	[SkipLocalsInit]
	public T? GetComponent<T>()
		where T : class {
		const string MEMBER = "SceneObject.GetComponent";
		int script = ScriptIndex<T>.value;
		if (script != 0) {
			long found = ObjectBindings.FindScript(this, script, MEMBER);
			return found == 0 ? null : GCHandle.FromIntPtr((nint)found).Target as T;
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

	/// <summary>Fill results with each script of type T on this object.</summary>
	[SkipLocalsInit]
	public void GetComponents<T>(List<T> results)
		where T : class {
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<long> handles = stackalloc long[STACK_HANDLES];
		long[]? rented = FindAll(ScriptIndex<T>.value, ref handles, "SceneObject.GetComponents");
		foreach (long handle in handles) {
			if (GCHandle.FromIntPtr((nint)handle).Target is T script) {
				results.Add(script);
			}
		}
		if (rented != null) {
			ArrayPool<long>.Shared.Return(rented);
		}
	}

	/// <summary>Add a script of type T.</summary>
	public T AddComponent<T>()
		where T : Component {
		const string MEMBER = "SceneObject.AddComponent";
		int script = ScriptIndex<T>.value;
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
