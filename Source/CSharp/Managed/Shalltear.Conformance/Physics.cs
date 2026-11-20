using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class ContactListener : Behavior, IPhysicsListener {
	internal int enters;
	internal int stays;
	internal int exits;
	internal int triggerEnters;
	internal int triggerStays;
	internal int triggerExits;
	internal int breaks;
	internal int hits;
	internal Collider? other;
	internal SceneObject? otherObject;
	internal Rigidbody? otherBody;
	internal CharacterController? mover;
	internal int contacts;
	internal float normalY;
	internal float breakForce;
	internal bool throws;

	public void OnCollisionEnter(in Collision collision) {
		++enters;
		other = collision.Collider;
		otherObject = collision.Object;
		otherBody = collision.Rigidbody;
		contacts = collision.contactCount;
		normalY = collision.contactCount > 0 ? collision.GetContact(0).normal.y : 0f;
		if (throws) {
			throw new InvalidOperationException("thrown by a physics listener");
		}
	}

	public void OnCollisionStay(in Collision collision) {
		++stays;
	}

	public void OnCollisionExit(in Collision collision) {
		++exits;
	}

	public void OnTriggerEnter(Collider other) {
		++triggerEnters;
		this.other = other;
	}

	public void OnTriggerStay(Collider other) {
		++triggerStays;
	}

	public void OnTriggerExit(Collider? other) {
		++triggerExits;
	}

	public void OnJointBreak(float breakForce) {
		++breaks;
		this.breakForce = breakForce;
	}

	public void OnControllerColliderHit(in ControllerColliderHit hit) {
		++hits;
		other = hit.Collider;
		otherObject = hit.Object;
		mover = hit.Controller;
		normalY = hit.normal.y;
	}
}

sealed class EnterOnly : Behavior, IPhysicsListener {
	internal int enters;

	public void OnCollisionEnter(in Collision collision) {
		++enters;
	}
}

sealed class BodyHolder : Behavior {
	public Rigidbody? body;
	public Collider? shape;
	public List<Rigidbody> bodies = new();
}

static class PhysicsProbe {
	[ConformanceTest]
	internal static void ListenerThrows() {
		SceneObject floor = PhysicsTests.Ball("ThrowFloor", new Vec3(0f, 0f, 900f), false);
		SceneObject ball = PhysicsTests.Ball("ThrowBall", new Vec3(0f, 0.6f, 900f), true);
		ball.AddComponent<ContactListener>().throws = true;
		EnterOnly second = ball.AddComponent<EnterOnly>();
		PhysicsTests.Step(1);
		BindingProbe.Record("second heard " + second.enters);
		floor.SetActive(false);
		ball.SetActive(false);
	}
}

static class PhysicsTests {
	const int FIXED_STEPS = 6;
	const float RADIUS = 0.5f;

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	internal static void Step(int count) {
		BindingProbe.Drive(FIXED_STEPS, count, 0);
	}

	static SceneObject Place(string name, Vec3 position) {
		SceneObject made = SceneSystem.CreateObject(name);
		made.WorldPosition = position;
		return made;
	}

	static Rigidbody Pinned(SceneObject target) {
		Rigidbody body = target.AddComponent<Rigidbody>();
		body.UseGravity = false;
		body.FreezePositionX = true;
		body.FreezePositionY = true;
		body.FreezePositionZ = true;
		return body;
	}

	internal static SceneObject Ball(string name, Vec3 position, bool body, bool trigger = false) {
		SceneObject made = Place(name, position);
		if (body) {
			Pinned(made);
		}
		SphereCollider shape = made.AddComponent<SphereCollider>();
		shape.Radius = RADIUS;
		shape.IsTrigger = trigger;
		return made;
	}

	static SceneObject Wall(string name, Vec3 position, Vec3 size) {
		SceneObject made = Place(name, position);
		made.AddComponent<BoxCollider>().Size = size;
		return made;
	}

	static void Retire(params SceneObject[] used) {
		foreach (SceneObject target in used) {
			target.SetActive(false);
			target.Destroy();
		}
	}

	static void Near(float expected, float actual, float tolerance, string what) {
		Check.True(MathF.Abs(expected - actual) <= tolerance, what + ": expected " + expected + ", got " + actual);
	}

	[ConformanceTest]
	internal static void BodyPropertiesRoundTrip() {
		SceneObject crate = Place("Crate", new Vec3(0f, 0f, 100f));
		Rigidbody body = crate.AddComponent<Rigidbody>();
		Check.Same(body, crate.GetComponent<Rigidbody>());
		Check.Same(crate, body.Owner);
		Check.Equal(1f, body.Mass);
		Check.True(body.UseGravity && !body.IsKinematic && body.AutomaticCenterOfMass && body.AutomaticInertiaTensor, "defaults");
		Check.Equal(RigidbodyInterpolation.None, body.Interpolation);
		Check.Equal(RigidbodyCollisionDetection.Continuous, body.CollisionDetectionMode);

		body.Mass = 4f;
		body.LinearDamping = 0.25f;
		body.AngularDamping = 0.5f;
		body.AutomaticCenterOfMass = false;
		body.CenterOfMass = new Vec3(0.1f, 0.2f, 0.3f);
		body.AutomaticInertiaTensor = false;
		body.InertiaTensor = new Vec3(2f, 3f, 4f);
		body.InertiaTensorRotation = new Quat(0f, 0.7071068f, 0f, 0.7071068f);
		body.UseGravity = false;
		body.Interpolation = RigidbodyInterpolation.Extrapolate;
		body.CollisionDetectionMode = RigidbodyCollisionDetection.ContinuousDynamic;
		body.CarryRiders = true;
		body.SleepThreshold = 0.125f;
		body.FreezePositionX = true;
		body.FreezePositionZ = true;
		body.FreezeRotationY = true;
		Check.Equal(4f, body.Mass);
		Check.Equal(0.25f, body.LinearDamping);
		Check.Equal(0.5f, body.AngularDamping);
		Check.True(!body.AutomaticCenterOfMass && !body.AutomaticInertiaTensor && !body.UseGravity && body.CarryRiders, "switches");
		Check.Equal(new Vec3(0.1f, 0.2f, 0.3f), body.CenterOfMass);
		Check.Equal(new Vec3(2f, 3f, 4f), body.InertiaTensor);
		Check.Equal(new Quat(0f, 0.7071068f, 0f, 0.7071068f), body.InertiaTensorRotation);
		Check.Equal(RigidbodyInterpolation.Extrapolate, body.Interpolation);
		Check.Equal(RigidbodyCollisionDetection.ContinuousDynamic, body.CollisionDetectionMode);
		Check.Equal(0.125f, body.SleepThreshold);
		Check.True(body.FreezePositionX && !body.FreezePositionY && body.FreezePositionZ, "frozen position");
		Check.True(!body.FreezeRotationX && body.FreezeRotationY && !body.FreezeRotationZ, "frozen rotation");

		body.FreezePositionX = false;
		body.FreezePositionZ = false;
		body.LinearVelocity = new Vec3(0f, 0f, 2f);
		body.AngularVelocity = new Vec3(0f, 1f, 0f);
		Check.Equal(new Vec3(0f, 0f, 2f), body.LinearVelocity);
		Step(30);
		Near(101f, crate.WorldPosition.z, 0.3f, "moved by its velocity");
		body.Sleep();
		Check.True(body.IsSleeping, "asleep");
		body.WakeUp();
		Check.True(!body.IsSleeping, "awake");
		body.IsKinematic = true;
		Check.True(body.IsKinematic, "kinematic");
		Retire(crate);
	}

	[ConformanceTest]
	internal static void ColliderClassesResolve() {
		SceneObject shapes = Place("Shapes", new Vec3(0f, 0f, 200f));
		BoxCollider box = shapes.AddComponent<BoxCollider>();
		SphereCollider sphere = shapes.AddComponent<SphereCollider>();
		CapsuleCollider capsule = shapes.AddComponent<CapsuleCollider>();
		MeshCollider mesh = shapes.AddComponent<MeshCollider>();

		Check.Same<Collider>(box, shapes.GetComponent<Collider>());
		Check.Same(sphere, shapes.GetComponent<SphereCollider>());
		Check.True(Gone(shapes.GetComponent<Rigidbody>()) && Gone(shapes.GetComponent<Joint>()), "no body, no joint");
		List<Collider> found = new();
		shapes.GetComponents(found);
		Check.Equal(4, found.Count);
		Check.True(found[0] is BoxCollider && found[1] is SphereCollider && found[2] is CapsuleCollider && found[3] is MeshCollider, "most derived class");
		List<CapsuleCollider> capsules = new();
		shapes.GetComponents(capsules);
		Check.Equal(1, capsules.Count);
		Check.Same(capsule, capsules[0]);
		Check.Equal("SceneObject.AddComponent can't add Collider (abstract)", ObjectTests.Refusal(() => shapes.AddComponent<Collider>()));

		Collider general = sphere;
		Check.True(!general.IsTrigger && Gone(general.Material) && general.IsInSimulation, "collider defaults");
		Check.Equal("Default", general.Categories);
		Check.Equal(PhysicsCategoryMatch.AllExcept, general.CollidesWithMode);
		Check.Equal("", general.CollidesWith);
		general.IsTrigger = true;
		general.Categories = "Default";
		general.CollidesWithMode = PhysicsCategoryMatch.Only;
		general.CollidesWith = "Default";
		Check.True(sphere.IsTrigger, "trigger");
		Check.Equal(PhysicsCategoryMatch.Only, sphere.CollidesWithMode);
		Check.Equal("Default", sphere.CollidesWith);

		box.Center = new Vec3(0f, 1f, 0f);
		box.Size = new Vec3(2f, 3f, 4f);
		sphere.Center = new Vec3(1f, 0f, 0f);
		sphere.Radius = 0.75f;
		capsule.Center = new Vec3(0f, 0f, 1f);
		capsule.Radius = 0.25f;
		capsule.Height = 3f;
		capsule.Direction = CapsuleDirection.ZAxis;
		mesh.Convex = true;
		Check.Equal(new Vec3(0f, 1f, 0f), box.Center);
		Check.Equal(new Vec3(2f, 3f, 4f), box.Size);
		Check.Equal(new Vec3(1f, 0f, 0f), sphere.Center);
		Check.Equal(0.75f, sphere.Radius);
		Check.Equal(new Vec3(0f, 0f, 1f), capsule.Center);
		Check.Equal(0.25f, capsule.Radius);
		Check.Equal(3f, capsule.Height);
		Check.Equal(CapsuleDirection.ZAxis, capsule.Direction);
		Check.True(mesh.Convex && Gone(mesh.Mesh) && !mesh.IsInSimulation, "mesh collider without a mesh");
		mesh.Mesh = null;
		Retire(shapes);
	}

	[ConformanceTest]
	internal static void MaterialAppliesToCollider() {
		PhysicsMaterial? surface = ObjectCache.Find<PhysicsMaterial>(BindingProbe.FindAsset("project://Assets/Surface.physmat"));
		if (Gone(surface)) {
			throw new ConformanceFailure("no PhysicsMaterial at Assets/Surface.physmat");
		}
		Near(0.6f, surface!.Friction, 0.0001f, "default friction");
		Check.Equal(PhysicsMaterialCombine.Average, surface.FrictionCombine);
		surface.Friction = 0.25f;
		surface.Bounciness = 0.5f;
		surface.RollingResistance = 0.125f;
		surface.FrictionCombine = PhysicsMaterialCombine.Minimum;
		surface.BounceCombine = PhysicsMaterialCombine.Maximum;
		Check.Equal(0.25f, surface.Friction);
		Check.Equal(0.5f, surface.Bounciness);
		Check.Equal(0.125f, surface.RollingResistance);
		Check.Equal(PhysicsMaterialCombine.Minimum, surface.FrictionCombine);
		Check.Equal(PhysicsMaterialCombine.Maximum, surface.BounceCombine);

		SceneObject slab = Wall("Slab", new Vec3(0f, 0f, 300f), new Vec3(1f, 1f, 1f));
		Collider shape = slab.GetComponent<Collider>()!;
		shape.Material = surface;
		Check.Same(surface, shape.Material);
		shape.Material = null;
		Check.True(Gone(shape.Material), "material cleared");
		surface.Friction = 0.6f;
		surface.FrictionCombine = PhysicsMaterialCombine.Average;
		Retire(slab);
	}

	[ConformanceTest]
	internal static void JointPropertiesRoundTrip() {
		SceneObject anchor = Place("Anchor", new Vec3(0f, 2f, 400f));
		Rigidbody anchorBody = Pinned(anchor);
		SceneObject arm = Place("Arm", new Vec3(0f, 1f, 400f));
		Pinned(arm);

		HingeJoint hinge = arm.AddComponent<HingeJoint>();
		Joint joint = hinge;
		Check.Same<Joint>(hinge, arm.GetComponent<Joint>());
		Check.True(Gone(joint.ConnectedBody), "connected to the world");
		Check.Equal(Joint.NEVER_BREAKS, joint.BreakForce);
		Check.Equal(Joint.NEVER_BREAKS, joint.BreakTorque);
		joint.ConnectedBody = anchorBody;
		joint.Anchor = new Vec3(0f, 0.5f, 0f);
		joint.AutoConfigureConnectedAnchor = false;
		joint.ConnectedAnchor = new Vec3(0f, -0.5f, 0f);
		joint.BreakForce = 500f;
		joint.BreakTorque = 250f;
		joint.EnableCollision = true;
		Check.Same(anchorBody, joint.ConnectedBody);
		Check.Equal(new Vec3(0f, 0.5f, 0f), joint.Anchor);
		Check.True(!joint.AutoConfigureConnectedAnchor && joint.EnableCollision, "joint switches");
		Check.Equal(new Vec3(0f, -0.5f, 0f), joint.ConnectedAnchor);
		Check.Equal(500f, joint.BreakForce);
		Check.Equal(250f, joint.BreakTorque);

		hinge.Axis = new Vec3(0f, 0f, 1f);
		hinge.UseSpring = true;
		hinge.Spring = 10f;
		hinge.Damper = 1f;
		hinge.SpringTargetPosition = 15f;
		hinge.UseMotor = true;
		hinge.MotorTargetVelocity = 90f;
		hinge.MotorForce = 20f;
		hinge.UseLimits = true;
		hinge.LimitMin = -30f;
		hinge.LimitMax = 45f;
		Check.Equal(new Vec3(0f, 0f, 1f), hinge.Axis);
		Check.True(hinge.UseSpring && hinge.UseMotor && hinge.UseLimits, "hinge switches");
		Check.Equal(10f, hinge.Spring);
		Check.Equal(1f, hinge.Damper);
		Check.Equal(15f, hinge.SpringTargetPosition);
		Check.Equal(90f, hinge.MotorTargetVelocity);
		Check.Equal(20f, hinge.MotorForce);
		Check.Equal(-30f, hinge.LimitMin);
		Check.Equal(45f, hinge.LimitMax);
		Step(1);
		Check.True(hinge.IsInSimulation, "hinge in the simulation");

		SceneObject rest = Place("Rest", new Vec3(5f, 1f, 400f));
		Pinned(rest);
		SpringJoint spring = rest.AddComponent<SpringJoint>();
		spring.Spring = 30f;
		spring.Damper = 3f;
		spring.MinDistance = 0.5f;
		spring.MaxDistance = 2f;
		Check.Equal(30f, spring.Spring);
		Check.Equal(3f, spring.Damper);
		Check.Equal(0.5f, spring.MinDistance);
		Check.Equal(2f, spring.MaxDistance);

		CharacterJoint ball = rest.AddComponent<CharacterJoint>();
		ball.Axis = new Vec3(1f, 0f, 0f);
		ball.SwingAxis = new Vec3(0f, 1f, 0f);
		ball.UseSwingLimit = true;
		ball.Swing1Limit = 20f;
		ball.Swing2Limit = 25f;
		ball.UseTwistLimit = true;
		ball.LowTwistLimit = -10f;
		ball.HighTwistLimit = 35f;
		ball.UseDrive = true;
		ball.DriveSpring = 7f;
		ball.DriveDamper = 0.5f;
		ball.TargetRotation = new Quat(0f, 0f, 0.7071068f, 0.7071068f);
		Check.Equal(new Vec3(1f, 0f, 0f), ball.Axis);
		Check.Equal(new Vec3(0f, 1f, 0f), ball.SwingAxis);
		Check.True(ball.UseSwingLimit && ball.UseTwistLimit && ball.UseDrive, "character joint switches");
		Check.Equal(20f, ball.Swing1Limit);
		Check.Equal(25f, ball.Swing2Limit);
		Check.Equal(-10f, ball.LowTwistLimit);
		Check.Equal(35f, ball.HighTwistLimit);
		Check.Equal(7f, ball.DriveSpring);
		Check.Equal(0.5f, ball.DriveDamper);
		Check.Equal(new Quat(0f, 0f, 0.7071068f, 0.7071068f), ball.TargetRotation);

		PrismaticJoint slider = rest.AddComponent<PrismaticJoint>();
		slider.Axis = new Vec3(0f, 1f, 0f);
		slider.UseSpring = true;
		slider.Spring = 40f;
		slider.Damper = 4f;
		slider.SpringTargetPosition = 0.25f;
		slider.UseMotor = true;
		slider.MotorTargetVelocity = 1.5f;
		slider.MotorForce = 60f;
		slider.UseLimits = true;
		slider.LimitMin = -1f;
		slider.LimitMax = 2f;
		Check.Equal(new Vec3(0f, 1f, 0f), slider.Axis);
		Check.True(slider.UseSpring && slider.UseMotor && slider.UseLimits, "slider switches");
		Check.Equal(40f, slider.Spring);
		Check.Equal(4f, slider.Damper);
		Check.Equal(0.25f, slider.SpringTargetPosition);
		Check.Equal(1.5f, slider.MotorTargetVelocity);
		Check.Equal(60f, slider.MotorForce);
		Check.Equal(-1f, slider.LimitMin);
		Check.Equal(2f, slider.LimitMax);

		List<Joint> joints = new();
		rest.GetComponents(joints);
		Check.Equal(3, joints.Count);
		Check.True(joints[0] is SpringJoint && joints[1] is CharacterJoint && joints[2] is PrismaticJoint, "most derived joint class");
		Check.True(rest.AddComponent<FixedJoint>() is Joint, "fixed joint");
		Retire(anchor, arm, rest);
	}

	[ConformanceTest]
	internal static void RaycastFindsClosest() {
		Vec3 origin = new(0f, 0f, 500f);
		SceneObject near = Wall("NearWall", origin + new Vec3(0f, 0f, 5f), new Vec3(4f, 4f, 1f));
		SceneObject far = Wall("FarWall", origin + new Vec3(0f, 0f, 9f), new Vec3(4f, 4f, 1f));
		Pinned(far);
		Step(1);
		Ray ray = new(origin, new Vec3(0f, 0f, 3f));
		Check.Equal(new Vec3(0f, 0f, 1f), ray.direction);

		Check.True(Physics.Raycast(ray, out RaycastHit hit), "wall in the way");
		Check.Same<Collider>(near.GetComponent<BoxCollider>(), hit.Collider);
		Check.Same(near, hit.Object);
		Check.True(Gone(hit.Rigidbody), "static wall has no body");
		Near(4.5f, hit.distance, 0.01f, "distance");
		Near(504.5f, hit.point.z, 0.01f, "point");
		Near(-1f, hit.normal.z, 0.01f, "normal");
		Near(ray.GetPoint(hit.distance).z, hit.point.z, 0.001f, "point along the ray");

		Check.True(!Physics.Raycast(ray, out RaycastHit missed, 4f), "ray too short");
		Check.True(Gone(missed.Collider) && missed.distance == 0f, "a miss leaves hit empty");
		Check.True(!Physics.Raycast(new Ray(origin, new Vec3(0f, 0f, -1f)), out _), "nothing behind");

		near.SetActive(false);
		Check.True(Physics.Raycast(ray, out hit), "far wall once the near one is off");
		Check.Same(far, hit.Object);
		Check.Same(far.GetComponent<Rigidbody>(), hit.Rigidbody);
		near.SetActive(true);

		PhysicsQueryFilter none = new() { collidesWithMode = PhysicsCategoryMatch.Only };
		Check.True(!Physics.Raycast(ray, out _, Physics.MAX_QUERY_DISTANCE, none), "a query that collides with nothing");
		PhysicsQueryFilter skipped = new() { collidesWith = "Default" };
		Check.True(!Physics.Raycast(ray, out _, Physics.MAX_QUERY_DISTANCE, skipped), "walls' category skipped");
		PhysicsQueryFilter named = new() { categories = "Default", collidesWithMode = PhysicsCategoryMatch.Only, collidesWith = "Default" };
		Check.True(Physics.Raycast(ray, out _, Physics.MAX_QUERY_DISTANCE, named), "only walls' category");

		near.GetComponent<Collider>()!.IsTrigger = true;
		Step(1);
		PhysicsQueryFilter solid = new() { triggerInteraction = PhysicsQueryTriggers.Ignore };
		Check.True(Physics.Raycast(ray, out hit, Physics.MAX_QUERY_DISTANCE, solid), "trigger skipped");
		Check.Same(far, hit.Object);
		PhysicsQueryFilter every = new() { triggerInteraction = PhysicsQueryTriggers.Collide };
		Check.True(Physics.Raycast(ray, out hit, Physics.MAX_QUERY_DISTANCE, every), "trigger hit");
		Check.Same(near, hit.Object);
		Retire(near, far);
	}

	[ConformanceTest]
	internal static void ShapeCastsSweep() {
		Vec3 origin = new(0f, 0f, 600f);
		SceneObject wall = Wall("SweepWall", origin + new Vec3(0f, 0f, 5f), new Vec3(4f, 4f, 1f));
		Step(1);
		Ray ray = new(origin, new Vec3(0f, 0f, 1f));

		Check.True(Physics.SphereCast(ray, 0.5f, out RaycastHit hit), "sphere reaches the wall");
		Check.Same(wall, hit.Object);
		Near(4f, hit.distance, 0.02f, "sphere's sweep distance");
		Check.True(!Physics.SphereCast(ray, 0.5f, out _, 3f), "sphere's sweep too short");
		Check.True(!Physics.SphereCast(new Ray(origin + new Vec3(4f, 0f, 0f), ray.direction), 0.5f, out _), "sphere passes the wall's side");
		Check.True(Physics.SphereCast(new Ray(origin + new Vec3(2.4f, 0f, 0f), ray.direction), 0.5f, out _), "sphere clips the wall's edge");

		Vec3 low = origin + new Vec3(0f, -1f, 0f);
		Vec3 high = origin + new Vec3(0f, 1f, 0f);
		Check.True(Physics.CapsuleCast(low, high, 0.25f, new Vec3(0f, 0f, 1f), out hit), "capsule reaches the wall");
		Check.Same<Collider>(wall.GetComponent<Collider>(), hit.Collider);
		Near(4.25f, hit.distance, 0.02f, "capsule's sweep distance");
		Check.True(!Physics.CapsuleCast(low, high, 0.25f, new Vec3(0f, 0f, -1f), out _), "capsule swept away");
		PhysicsQueryFilter skipped = new() { collidesWith = "Default" };
		Check.True(!Physics.CapsuleCast(low, high, 0.25f, new Vec3(0f, 0f, 1f), out _, Physics.MAX_QUERY_DISTANCE, skipped), "capsule's filter");
		Retire(wall);
	}

	[ConformanceTest]
	internal static void OverlapSphereFillsList() {
		Vec3 origin = new(0f, 0f, 700f);
		List<SceneObject> made = new();
		for (int index = 0; index < 70; ++index) {
			made.Add(Ball("Dot" + index, origin + new Vec3(0.01f * index, 0f, 0f), false));
		}
		SceneObject away = Ball("Away", origin + new Vec3(0f, 30f, 0f), false);
		SceneObject sensor = Ball("Sensor", origin, false, true);
		Step(1);

		List<Collider> found = new() { away.GetComponent<Collider>()! };
		PhysicsQueryFilter solid = new() { triggerInteraction = PhysicsQueryTriggers.Ignore };
		Check.Equal(70, Physics.OverlapSphere(origin, 1f, found, solid));
		Check.Equal(70, found.Count);
		Check.True(!found.Contains(away.GetComponent<Collider>()!) && !found.Contains(sensor.GetComponent<Collider>()!), "far + trigger left out");
		Check.True(found.Contains(made[0].GetComponent<Collider>()!) && found.Contains(made[69].GetComponent<Collider>()!), "1st + last found");
		PhysicsQueryFilter every = new() { triggerInteraction = PhysicsQueryTriggers.Collide };
		Check.Equal(71, Physics.OverlapSphere(origin, 1f, found, every));
		Check.Equal(1, Physics.OverlapSphere(origin + new Vec3(0f, 30f, 0f), 1f, found));
		Check.Same<Collider>(away.GetComponent<SphereCollider>(), found[0]);
		Check.Equal(0, Physics.OverlapSphere(origin + new Vec3(0f, -30f, 0f), 1f, found));
		Check.Equal(0, found.Count);
		made.Add(away);
		made.Add(sensor);
		Retire(made.ToArray());
	}

	[ConformanceTest]
	internal static void ListenerHearsContacts() {
		Vec3 origin = new(0f, 0f, 800f);
		SceneObject floor = Ball("Floor", origin, false);
		SceneObject ball = Ball("Ball", origin + new Vec3(0f, 0.6f, 0f), true);
		ContactListener heard = ball.AddComponent<ContactListener>();
		EnterOnly counted = ball.AddComponent<EnterOnly>();
		ContactListener below = floor.AddComponent<ContactListener>();

		Step(1);
		Check.Equal(1, heard.enters);
		Check.Equal(0, heard.stays);
		Check.Equal(1, counted.enters);
		Check.Same<Collider>(floor.GetComponent<SphereCollider>(), heard.other);
		Check.Same(floor, heard.otherObject);
		Check.True(Gone(heard.otherBody), "static floor has no body");
		Check.True(heard.contacts > 0 && heard.contacts <= Collision.MAX_CONTACTS, "contact count");
		Check.True(heard.normalY < 0f, "normal points from the listener to the other");
		Check.Equal(1, below.enters);
		Check.Same(ball, below.otherObject);
		Check.Same(ball.GetComponent<Rigidbody>(), below.otherBody);
		Check.True(below.normalY > 0f, "other side's normal");

		Step(3);
		Check.Equal(1, heard.enters);
		Check.Equal(3, heard.stays);
		Check.Equal(0, heard.exits);

		ball.WorldPosition = origin + new Vec3(0f, 20f, 0f);
		Step(2);
		Check.Equal(1, heard.exits);
		Check.Equal(1, below.exits);
		Check.Equal(1, counted.enters);
		Check.Equal(0, heard.triggerEnters + heard.triggerStays + heard.triggerExits + heard.breaks + heard.hits);
		Retire(floor, ball);
	}

	[ConformanceTest]
	internal static void ListenerHearsTriggers() {
		Vec3 origin = new(0f, 0f, 1000f);
		SceneObject volume = Ball("Volume", origin, false, true);
		SceneObject visitor = Ball("Visitor", origin + new Vec3(0f, 0.4f, 0f), true);
		ContactListener inside = volume.AddComponent<ContactListener>();
		ContactListener outside = visitor.AddComponent<ContactListener>();

		Step(1);
		Check.Equal(1, inside.triggerEnters);
		Check.Equal(1, outside.triggerEnters);
		Check.Same<Collider>(visitor.GetComponent<SphereCollider>(), inside.other);
		Check.Same<Collider>(volume.GetComponent<SphereCollider>(), outside.other);
		Check.Equal(0, inside.enters + outside.enters);
		Step(3);
		Check.Equal(3, inside.triggerStays);
		visitor.WorldPosition = origin + new Vec3(0f, 20f, 0f);
		Step(2);
		Check.Equal(1, inside.triggerExits);
		Check.Equal(1, outside.triggerExits);
		Check.Equal(1, inside.triggerEnters);
		Retire(volume, visitor);
	}

	[ConformanceTest]
	internal static void ListenerHearsJointBreak() {
		SceneObject hanging = Place("Hanging", new Vec3(0f, -1f, 1100f));
		Rigidbody body = hanging.AddComponent<Rigidbody>();
		FixedJoint joint = hanging.AddComponent<FixedJoint>();
		joint.BreakForce = 4f;
		ContactListener heard = hanging.AddComponent<ContactListener>();
		Check.Equal(1f, body.Mass);

		Step(4);
		Check.Equal(1, heard.breaks);
		Check.True(heard.breakForce > 4f, "force that broke it: " + heard.breakForce);
		Check.True(Gone(hanging.GetComponent<FixedJoint>()), "broken joint is gone");
		Step(4);
		Check.Equal(1, heard.breaks);
		Retire(hanging);
	}

	[ConformanceTest]
	internal static void ControllerMovesAndReportsHits() {
		Vec3 origin = new(0f, 0f, 1200f);
		SceneObject ground = Wall("Ground", origin, new Vec3(20f, 1f, 20f));
		SceneObject hero = Place("Hero", origin + new Vec3(0f, 1.6f, 0f));
		CharacterController controller = hero.AddComponent<CharacterController>();
		ContactListener heard = hero.AddComponent<ContactListener>();
		Check.Same<Collider>(controller, hero.GetComponent<Collider>());
		controller.SlopeLimit = 50f;
		controller.StepOffset = 0.25f;
		controller.SkinWidth = 0.05f;
		controller.MinMoveDistance = 0f;
		controller.Center = new Vec3(0f, 0f, 0f);
		controller.Radius = 0.4f;
		controller.Height = 2f;
		Check.Equal(50f, controller.SlopeLimit);
		Check.Equal(0.25f, controller.StepOffset);
		Check.Equal(0.05f, controller.SkinWidth);
		Check.Equal(0f, controller.MinMoveDistance);
		Check.Equal(new Vec3(0f, 0f, 0f), controller.Center);
		Check.Equal(0.4f, controller.Radius);
		Check.Equal(2f, controller.Height);
		Check.True(controller.DetectCollisions && !controller.IsGrounded, "controller defaults");
		Step(1);

		CollisionFlags flags = controller.Move(new Vec3(0f, -0.5f, 0f));
		Check.Equal(CollisionFlags.Below, flags);
		Check.Equal(CollisionFlags.Below, controller.CollisionFlags);
		Check.True(controller.IsGrounded, "grounded");
		Check.Equal(1, heard.hits);
		Check.Same(ground, heard.otherObject);
		Check.Same<Collider>(ground.GetComponent<BoxCollider>(), heard.other);
		Check.Same(controller, heard.mover);
		Check.True(heard.normalY > 0.9f, "ground's normal");
		Near(1.55f, hero.WorldPosition.y, 0.02f, "resting height");

		Check.Equal(CollisionFlags.None, controller.Move(new Vec3(0f, 1f, 0f)));
		Check.True(!controller.IsGrounded, "in the air");
		Check.True(controller.SimpleMove(new Vec3(1f, 0f, 0f)) == controller.IsGrounded, "SimpleMove reports grounding");
		Check.True(controller.Velocity.x > 0f, "velocity of the last move");
		controller.DetectCollisions = false;
		Check.True(!controller.DetectCollisions && !controller.IsInSimulation, "out of the simulation");
		Retire(ground, hero);
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldPackageTypes() {
		SceneObject source = Place("Holder", new Vec3(0f, 0f, 1300f));
		Rigidbody body = Pinned(source);
		SphereCollider shape = source.AddComponent<SphereCollider>();
		BodyHolder holder = source.AddComponent<BodyHolder>();
		holder.body = body;
		holder.shape = shape;
		holder.bodies.Add(body);

		SceneObject copy = SceneSystem.Instantiate(source);
		BodyHolder copied = copy.GetComponent<BodyHolder>()!;
		Check.Same(copy.GetComponent<Rigidbody>(), copied.body);
		Check.Same<Collider>(copy.GetComponent<SphereCollider>(), copied.shape);
		Check.Equal(1, copied.bodies.Count);
		Check.Same(copy.GetComponent<Rigidbody>(), copied.bodies[0]);
		Retire(source, copy);
	}

	[ConformanceTest]
	internal static void RepeatedQueriesAllocateNothing() {
		Vec3 origin = new(0f, 0f, 1400f);
		SceneObject wall = Wall("QueryWall", origin + new Vec3(0f, 0f, 5f), new Vec3(4f, 4f, 1f));
		SceneObject floor = Ball("QueryFloor", origin + new Vec3(50f, 0f, 0f), false);
		SceneObject ball = Ball("QueryBall", origin + new Vec3(50f, 0.6f, 0f), true);
		ContactListener heard = ball.AddComponent<ContactListener>();
		Rigidbody body = ball.GetComponent<Rigidbody>()!;
		SphereCollider shape = ball.GetComponent<SphereCollider>()!;
		Ray ray = new(origin, new Vec3(0f, 0f, 1f));
		PhysicsQueryFilter filter = new() { categories = "Default", triggerInteraction = PhysicsQueryTriggers.Ignore };
		List<Collider> found = new(8);
		Step(2);
		Physics.Raycast(ray, out RaycastHit warm);
		Check.True(!Gone(warm.Collider) && !Gone(warm.Object), "wrappers made before counting");
		Physics.OverlapSphere(origin + new Vec3(0f, 0f, 5f), 1f, found);
		int staysBefore = heard.stays;

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			hits += Physics.Raycast(ray, out RaycastHit hit, 20f, filter) && hit.Collider is BoxCollider && ReferenceEquals(hit.Object, wall) ? 1 : 0;
			hits += Physics.SphereCast(ray, 0.25f, out hit) && hit.distance > 4f ? 1 : 0;
			hits += Physics.CapsuleCast(origin, origin + new Vec3(0f, 1f, 0f), 0.25f, ray.direction, out hit) ? 1 : 0;
			hits += Physics.OverlapSphere(origin + new Vec3(0f, 0f, 5f), 1f, found, filter);
			body.LinearVelocity = new Vec3(0f, 0f, 0f);
			hits += body.LinearVelocity == Vec3.Zero && body.AngularVelocity == Vec3.Zero && !body.IsKinematic ? 1 : 0;
			hits += shape.Radius == RADIUS && !shape.IsTrigger && ReferenceEquals(ball.GetComponent<Rigidbody>(), body) ? 1 : 0;
		}
		body.WakeUp();
		Step(20);
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(6000, hits);
		Check.Equal(20, heard.stays - staysBefore);
		Check.Equal(0L, allocated);
		Retire(wall, floor, ball);
	}
}
