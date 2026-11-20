using System;

using Shalltear.Interop;

namespace Shalltear;

public partial struct RaycastHit {
	/// <summary>Collider of the hit.</summary>
	public readonly Collider? Collider => ObjectCache.Find<Collider>(_colliderId);

	/// <summary>Object of that collider.</summary>
	public readonly SceneObject? Object => ObjectCache.SceneObject(_objectId);

	/// <summary>Body of that collider (null for a static one).</summary>
	public readonly Rigidbody? Rigidbody => ObjectCache.Find<Rigidbody>(_rigidbodyId);
}

public partial struct Collision {
	/// <summary>Most contact points per collision.</summary>
	public const int MAX_CONTACTS = 4;

	/// <summary>Other collider.</summary>
	public readonly Collider? Collider => ObjectCache.Find<Collider>(_colliderId);

	/// <summary>Object of the other collider.</summary>
	public readonly SceneObject? Object => ObjectCache.SceneObject(_objectId);

	/// <summary>Body of the other collider (null for a static one).</summary>
	public readonly Rigidbody? Rigidbody => ObjectCache.Find<Rigidbody>(_rigidbodyId);

	/// <summary>Contact point at index (under contactCount).</summary>
	public readonly ContactPoint GetContact(int index) {
		if ((uint)index >= (uint)contactCount) {
			throw new ArgumentOutOfRangeException(nameof(index));
		}
		return index switch {
			0 => _contact0,
			1 => _contact1,
			2 => _contact2,
			_ => _contact3,
		};
	}
}

public partial struct ControllerColliderHit {
	/// <summary>Character controller of the move.</summary>
	public readonly CharacterController? Controller => ObjectCache.Find<CharacterController>(_controllerId);

	/// <summary>Collider of the hit.</summary>
	public readonly Collider? Collider => ObjectCache.Find<Collider>(_colliderId);

	/// <summary>Object of that collider.</summary>
	public readonly SceneObject? Object => ObjectCache.SceneObject(_objectId);

	/// <summary>Body of that collider (null for a static one).</summary>
	public readonly Rigidbody? Rigidbody => ObjectCache.Find<Rigidbody>(_rigidbodyId);
}
