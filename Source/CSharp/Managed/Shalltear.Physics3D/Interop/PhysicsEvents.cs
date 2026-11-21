using System;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

public static unsafe partial class Physics3DBindings {
	static partial void Attached() {
		PhysicsEvents.Attach();
	}
}

// must match PhysicsBindings.cpp
internal static unsafe class PhysicsEvents {
	const int PHASE_ENTER = 0;
	const int PHASE_STAY = 1;

	internal static void Attach() {
		int slot = Physics.AttachEvents((long)(delegate* unmanaged<nint, int, Collision*, void>)&OnCollision,
										(long)(delegate* unmanaged<nint, int, ulong, void>)&OnTrigger, (long)(delegate* unmanaged<nint, float, void>)&OnJointBreak,
										(long)(delegate* unmanaged<nint, ControllerColliderHit*, void>)&OnControllerColliderHit);
		ScriptTypes.AddListener(slot, typeof(IPhysicsListener));
	}

	[UnmanagedCallersOnly]
	static void OnCollision(nint instance, int phase, Collision* collision) {
		nint outer = HookState._owner;
		try {
			if (GCHandle.FromIntPtr(instance).Target is not IPhysicsListener listener) {
				return;
			}
			HookState._owner = instance;
			if (phase == PHASE_ENTER) {
				listener.OnCollisionEnter(in *collision);
			} else if (phase == PHASE_STAY) {
				listener.OnCollisionStay(in *collision);
			} else {
				listener.OnCollisionExit(in *collision);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
		HookState._owner = outer;
	}

	[UnmanagedCallersOnly]
	static void OnTrigger(nint instance, int phase, ulong otherId) {
		nint outer = HookState._owner;
		try {
			if (GCHandle.FromIntPtr(instance).Target is not IPhysicsListener listener) {
				return;
			}
			HookState._owner = instance;
			Collider? other = ObjectCache.Find<Collider>(otherId);
			if (phase > PHASE_STAY) {
				listener.OnTriggerExit(other);
			} else if (other is null) {
			} else if (phase == PHASE_ENTER) {
				listener.OnTriggerEnter(other);
			} else {
				listener.OnTriggerStay(other);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
		HookState._owner = outer;
	}

	[UnmanagedCallersOnly]
	static void OnJointBreak(nint instance, float breakForce) {
		nint outer = HookState._owner;
		try {
			if (GCHandle.FromIntPtr(instance).Target is IPhysicsListener listener) {
				HookState._owner = instance;
				listener.OnJointBreak(breakForce);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
		HookState._owner = outer;
	}

	[UnmanagedCallersOnly]
	static void OnControllerColliderHit(nint instance, ControllerColliderHit* hit) {
		nint outer = HookState._owner;
		try {
			if (GCHandle.FromIntPtr(instance).Target is IPhysicsListener listener) {
				HookState._owner = instance;
				listener.OnControllerColliderHit(in *hit);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
		HookState._owner = outer;
	}
}
