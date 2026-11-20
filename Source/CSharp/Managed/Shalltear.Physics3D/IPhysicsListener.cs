namespace Shalltear;

/// <summary>Physics events of a script's object (implement the ones to hear).</summary>
public interface IPhysicsListener {
	/// <summary>Collider of this object started touching another.</summary>
	void OnCollisionEnter(in Collision collision) {}

	/// <summary>Still touching (once per fixed step, never for a sleeping body).</summary>
	void OnCollisionStay(in Collision collision) {}

	/// <summary>Stopped touching (collision.Collider is null when the other was destroyed).</summary>
	void OnCollisionExit(in Collision collision) {}

	/// <summary>Collider started overlapping a trigger (heard on both objects).</summary>
	void OnTriggerEnter(Collider other) {}

	/// <summary>Still overlapping (once per fixed step).</summary>
	void OnTriggerStay(Collider other) {}

	/// <summary>Stopped overlapping (other is null when it was destroyed).</summary>
	void OnTriggerExit(Collider? other) {}

	/// <summary>Joint of this object broke under breakForce.</summary>
	void OnJointBreak(float breakForce) {}

	/// <summary>This object's character controller hit a collider (once per collider per move).</summary>
	void OnControllerColliderHit(in ControllerColliderHit hit) {}
}
