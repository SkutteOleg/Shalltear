using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Lamp : Behavior {
	public Camera? eye;
	public Light? light;
	public MeshRenderer? body;
	public Material? paint;
}

static class RenderingProbe {
	[ConformanceTest]
	internal static void ListsCameras() {
		SceneObject eye = SceneSystem.CreateObject("Lister");
		Camera camera = eye.AddComponent<Camera>();
		List<Camera> cameras = new();
		RenderSystem.GetCameras(cameras);
		int listed = cameras.Contains(camera) ? cameras.Count : -1;
		camera.SetEnabled(false);
		RenderSystem.GetCameras(cameras);
		BindingProbe.Record("cameras " + listed + " then " + cameras.Count);
		eye.Destroy();
	}

	[ConformanceTest]
	internal static void DrawsLines() {
		DebugLines.Draw(Vec3.Zero, new Vec3(1f, 2f, 3f), Color.Red);
		DebugLines.DrawRay(Vec3.Zero, Vec3.Up, Color.Green, 0.5f, false);
	}

	[ConformanceTest]
	internal static void CountsDrawnLines() {
		int before = DebugLines.GetCount();
		DebugLines.Draw(Vec3.Zero, Vec3.One, Color.Red);
		DebugLines.DrawRay(Vec3.Zero, Vec3.Up, Color.Green, 2f, false);
		Check.Fail("lines " + (DebugLines.GetCount() - before));
	}
}

static class RenderingTests {
	const string GLOW = "project://Assets/Glow.mat";
	const string DOT = "project://Assets/Dot.png";

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	static void Retire(SceneObject used) {
		used.SetActive(false);
		used.Destroy();
	}

	[ConformanceTest]
	internal static void CameraPropertiesRoundTrip() {
		SceneObject eye = SceneSystem.CreateObject("Eye");
		Camera camera = eye.AddComponent<Camera>();
		Check.Same(camera, eye.GetComponent<Camera>());
		Check.Equal(60f, camera.FieldOfViewDeg);
		Check.Equal(BackgroundType.Sky, camera.BackgroundType);
		Check.Equal(new RectF(0f, 0f, 1f, 1f), camera.ViewportRect);
		camera.FieldOfViewDeg = 75f;
		camera.NearPlane = 0.5f;
		camera.FarPlane = 200f;
		camera.Priority = 3;
		camera.BackgroundType = BackgroundType.SolidColor;
		camera.ClearColor = new Color(0.1f, 0.2f, 0.3f, 1f);
		camera.CullingMask = 5u;
		camera.ViewportRect = new RectF(0.25f, 0f, 0.5f, 1f);
		camera.Iso = 400;
		camera.ShutterSpeed = 0.01f;
		camera.Aperture = 8f;
		Check.Equal(75f, camera.FieldOfViewDeg);
		Check.Equal(0.5f, camera.NearPlane);
		Check.Equal(200f, camera.FarPlane);
		Check.Equal(3, camera.Priority);
		Check.Equal(BackgroundType.SolidColor, camera.BackgroundType);
		Check.Equal(new Color(0.1f, 0.2f, 0.3f, 1f), camera.ClearColor);
		Check.Equal(5u, camera.CullingMask);
		Check.Equal(new RectF(new Vec2(0.25f, 0f), new Vec2(0.5f, 1f)), camera.ViewportRect);
		Check.Equal(400, camera.Iso);
		Check.Equal(0.01f, camera.ShutterSpeed);
		Check.Equal(8f, camera.Aperture);

		Check.Equal(0u, camera.PassCount);
		Check.True(Gone(camera.GetPass(0)) && Gone(camera.GetPass(-1)), "no pass");
		camera.SetPass(0, null);
		Check.Equal(1u, camera.PassCount);
		Check.Equal("Camera has 1 pass slot(s). Pass a slot from 0 to 1", Refusal(() => camera.SetPass(5, null)));
		Check.Equal("Camera has 1 pass slot(s). Pass a slot from 0 to 1", Refusal(() => camera.SetPass(-1, null)));
		List<Camera> cameras = new();
		RenderSystem.GetCameras(cameras);
		Retire(eye);
	}

	[ConformanceTest]
	internal static void LightPropertiesRoundTrip() {
		SceneObject lamp = SceneSystem.CreateObject("Lamp");
		Light light = lamp.AddComponent<Light>();
		Check.Equal(LightType.Directional, light.LightType);
		Check.Equal(Light.DEFAULT_SHADOW_NEAR_PLANE, light.ShadowNearPlane);
		Check.True(Light.MIN_SHADOW_NEAR_PLANE < Light.DEFAULT_SHADOW_NEAR_PLANE, "near plane bounds");
		ulong cookieId = BindingProbe.MakeAsset("TextureAsset");
		Texture cookie = ObjectCache.Find<Texture>(cookieId)!;
		light.LightType = LightType.Spot;
		light.Color = new Color(1f, 0.5f, 0.25f, 1f);
		light.Intensity = 3f;
		light.Range = 12f;
		light.SpotAngleDeg = 60f;
		light.InnerSpotAngleDeg = 20f;
		light.CastShadows = false;
		light.ShadowStrength = 0.5f;
		light.ShadowBias = 2f;
		light.ShadowNormalBias = 0.5f;
		light.ShadowNearPlane = 0.3f;
		light.ShadowCasterBias = 1.5f;
		light.ShadowCasterSlopeBias = 2.5f;
		light.Cookie = cookie;
		light.CookieSize = new Vec2(2f, 3f);
		light.CookieOffset = new Vec2(0.5f, 0.25f);
		light.RenderingLayerMask = 6u;
		light.ShadowLayerMask = 2u;
		Check.Equal(LightType.Spot, light.LightType);
		Check.Equal(new Color(1f, 0.5f, 0.25f, 1f), light.Color);
		Check.Equal(3f, light.Intensity);
		Check.Equal(12f, light.Range);
		Check.Equal(60f, light.SpotAngleDeg);
		Check.Equal(20f, light.InnerSpotAngleDeg);
		Check.True(!light.CastShadows, "shadows off");
		Check.Equal(0.5f, light.ShadowStrength);
		Check.Equal(2f, light.ShadowBias);
		Check.Equal(0.5f, light.ShadowNormalBias);
		Check.Equal(0.3f, light.ShadowNearPlane);
		Check.Equal(1.5f, light.ShadowCasterBias);
		Check.Equal(2.5f, light.ShadowCasterSlopeBias);
		Check.Same(cookie, light.Cookie);
		Check.Equal(new Vec2(2f, 3f), light.CookieSize);
		Check.Equal(new Vec2(0.5f, 0.25f), light.CookieOffset);
		Check.Equal(6u, light.RenderingLayerMask);
		Check.Equal(2u, light.ShadowLayerMask);
		light.Cookie = null;
		Check.True(Gone(light.Cookie), "cookie cleared");
		Retire(lamp);
		BindingProbe.FreeAsset(cookieId);
	}

	[ConformanceTest]
	internal static void MeshRendererBindsMaterials() {
		ulong meshId = BindingProbe.MakeMesh(0);
		Mesh cube = ObjectCache.Find<Mesh>(meshId)!;
		Material glow = AnimationTests.Stored<Material>(GLOW);
		SceneObject box = SceneSystem.CreateObject("Box");
		MeshRenderer renderer = box.AddComponent<MeshRenderer>();
		Check.True(Gone(renderer.Mesh), "no mesh");
		Check.Equal(0u, renderer.MaterialCount);
		Check.Equal("MeshRenderer has 0 material slot(s). Pass a slot from 0 to 0", Refusal(() => renderer.SetMaterial(1, glow)));
		renderer.Mesh = cube;
		Check.Same(cube, renderer.Mesh);
		Check.Equal(1, cube.SubmeshCount);
		renderer.SetMaterial(0, glow);
		Check.Equal(1u, renderer.MaterialCount);
		Check.Same(glow, renderer.GetMaterial(0));
		Check.Same(glow, renderer.GetRenderedMaterial(0));
		Check.True(Gone(renderer.GetMaterial(5)) && Gone(renderer.GetMaterial(-1)), "no 6th slot");
		Material copy = renderer.GetMaterialInstance(0)!;
		Check.True(!ReferenceEquals(copy, glow), "renderer's copy");
		Check.Same(copy, renderer.GetMaterialInstance(0));
		Check.Same(copy, renderer.GetRenderedMaterial(0));
		Check.Same(glow, renderer.GetMaterial(0));
		Check.Equal("MeshRenderer has no material slot 3 (1 slot(s))", Refusal(() => renderer.GetMaterialInstance(3)));
		renderer.SetMaterial(0, glow);
		Check.Same(glow, renderer.GetRenderedMaterial(0));
		renderer.SetMaterial(0, null);
		Check.True(Gone(renderer.GetMaterial(0)), "slot cleared");

		Check.Equal(ShadowCasting.On, renderer.CastShadows);
		renderer.CastShadows = ShadowCasting.ShadowsOnly;
		renderer.ReceiveShadows = false;
		renderer.RenderingLayerMask = 9u;
		Check.Equal(ShadowCasting.ShadowsOnly, renderer.CastShadows);
		Check.True(!renderer.ReceiveShadows, "receives none");
		Check.Equal(9u, renderer.RenderingLayerMask);

		SceneObject crate = SceneSystem.CreateObject("Crate");
		MeshRenderer other = crate.AddComponent<MeshRenderer>();
		other.Mesh = cube;
		other.SetMaterial(1, glow);
		Check.Equal(2u, other.MaterialCount);
		Check.Same(glow, other.GetMaterial(1));
		Retire(crate);
		Retire(box);
		BindingProbe.FreeAsset(meshId);
	}

	[ConformanceTest]
	internal static void SkinnedRendererHoldsBlendShapes() {
		ulong meshId = BindingProbe.MakeMesh(2);
		Mesh shaped = ObjectCache.Find<Mesh>(meshId)!;
		Check.Equal(2, shaped.BlendShapeCount);
		Check.Equal(1, shaped.FindBlendShape("Shape1"));
		Check.Equal(-1, shaped.FindBlendShape("Missing"));
		Check.Equal("Shape0", shaped.GetBlendShapeName(0));
		Check.Equal("Mesh has no blend shape 2 (2 blend shape(s))", Refusal(() => shaped.GetBlendShapeName(2)));

		SceneObject face = SceneSystem.CreateObject("Face");
		SkinnedMeshRenderer skin = face.AddComponent<SkinnedMeshRenderer>();
		Check.Equal("SkinnedMeshRenderer's mesh has no blend shape 0 (0 blend shape(s))", Refusal(() => skin.SetBlendShapeWeight(0, 50f)));
		skin.Mesh = shaped;
		skin.SetBlendShapeWeightCount(2);
		Check.Equal(2u, skin.BlendShapeWeightCount);
		skin.SetBlendShapeWeight(0, 25f);
		skin.SetBlendShapeWeight(1, 50f);
		Check.Equal(50f, skin.GetBlendShapeWeight(1));
		Check.Equal(0f, skin.GetBlendShapeWeight(7));
		Span<float> weights = stackalloc float[1];
		Check.Equal(2, skin.GetBlendShapeWeights(weights));
		Check.Equal(25f, weights[0]);
		Check.Equal("SkinnedMeshRenderer's mesh has 2 blend shape(s). Pass a count from 0 to 2", Refusal(() => skin.SetBlendShapeWeightCount(3)));
		Check.Equal("SkinnedMeshRenderer's mesh has no blend shape 2 (2 blend shape(s))", Refusal(() => skin.SetBlendShapeWeight(2, 1f)));

		ulong skinId = BindingProbe.MakeAsset("SkinAsset");
		SkinAsset joints = ObjectCache.Find<SkinAsset>(skinId)!;
		Skeleton bones = face.AddComponent<Skeleton>();
		skin.Skin = joints;
		skin.Skeleton = bones;
		Check.Same(joints, skin.Skin);
		Check.Same(bones, skin.Skeleton);
		Material glow = AnimationTests.Stored<Material>(GLOW);
		skin.SetMaterial(0, glow);
		Check.Same(glow, skin.GetMaterial(0));
		Check.True(!ReferenceEquals(skin.GetMaterialInstance(0), glow), "renderer's copy");
		Check.Equal("SkinnedMeshRenderer has no material slot 1 (1 slot(s))", Refusal(() => skin.GetMaterialInstance(1)));
		skin.CastShadows = ShadowCasting.TwoSided;
		Check.Equal(ShadowCasting.TwoSided, skin.CastShadows);
		skin.Skin = null;
		skin.Skeleton = null;
		Retire(face);
		BindingProbe.FreeAsset(skinId);
		BindingProbe.FreeAsset(meshId);
	}

	[ConformanceTest]
	internal static void MaterialParametersRoundTrip() {
		Material glow = AnimationTests.Stored<Material>(GLOW);
		Material made = glow.CreateInstance()!;
		Check.True(!ReferenceEquals(made, glow), "a copy");
		Check.True(!made.HasFloat("_Glow") && !made.HasColor("_Tint") && !made.HasTexture("_Base"), "empty material");
		Check.Equal(0.5f, made.GetFloat("_Glow", 0.5f));
		Check.Equal(0f, made.GetFloat("_Glow"));
		Check.Equal(Color.White, made.GetColor("_Tint"));
		Check.Equal(Color.Black, made.GetColor("_Tint", Color.Black));
		made.SetFloat("_Glow", 2f);
		made.SetColor("_Tint", new Color(1f, 0.5f, 0.25f, 1f));
		made.SetVec2("_Scroll", new Vec2(1f, 2f));
		made.SetVec3("_Wind", new Vec3(1f, 2f, 3f));
		made.SetInt("_Steps", 4);
		Check.True(made.HasFloat("_Glow") && made.HasColor("_Tint"), "parameters set");
		Check.True(!made.HasFloat("_Scroll") && !made.HasColor("_Wind"), "other kinds");
		Check.Equal(2f, made.GetFloat("_Glow"));
		Check.Equal(new Color(1f, 0.5f, 0.25f, 1f), made.GetColor("_Tint"));
		Check.True(!glow.HasFloat("_Glow"), "source untouched");

		Texture? dot = ObjectCache.Find<Texture>(BindingProbe.FindAsset(DOT));
		if (!Gone(dot)) {
			made.SetTexture("_Base", dot);
			Check.True(made.HasTexture("_Base"), "texture set");
			Check.Same(dot, made.GetTexture("_Base"));
		}
		ulong looseId = BindingProbe.MakeAsset("TextureAsset");
		Texture loose = ObjectCache.Find<Texture>(looseId)!;
		Check.Equal("Material.SetTexture got a texture with no asset id for '_Base'. Pass a texture of the project",
					Refusal(() => made.SetTexture("_Base", loose)));
		made.SetTexture("_Base", null);
		Check.True(Gone(made.GetTexture("_Base")), "texture cleared");
		BindingProbe.FreeAsset(looseId);

		made.SetKeyword("GLOW_ON", true);
		made.SetKeyword("GLOW_ON", true);
		made.SetKeyword("FOG", true);
		Check.Equal(2, made.KeywordCount);
		Check.Equal("FOG", made.GetKeyword(0));
		Check.True(made.HasKeyword("GLOW_ON") && !made.HasKeyword("RIM"), "keywords on");
		made.SetKeyword("GLOW_ON", false);
		made.SetKeyword("FOG", false);
		Check.Equal(0, made.KeywordCount);
		Check.Equal("Material has no keyword 0 (0 keyword(s))", Refusal(() => made.GetKeyword(0)));
		Check.Equal("Material.SetKeyword got an empty keyword. Pass a keyword's name", Refusal(() => made.SetKeyword("", true)));

		made.AlphaClipThreshold = 0.5f;
		made.SortingPriority = 2;
		Check.Equal(0.5f, made.AlphaClipThreshold);
		Check.Equal(2, made.SortingPriority);
		made.DestroyInstance();
	}

	[ConformanceTest]
	internal static void SkeletonPoseRoundTrips() {
		ulong rigId = BindingProbe.MakeSkeletonAsset();
		SkeletonAsset rig = ObjectCache.Find<SkeletonAsset>(rigId)!;
		Span<BoneTransform> rest = stackalloc BoneTransform[4];
		Check.Equal(2, rig.GetRestPose(rest));
		Check.Equal(BoneTransform.Identity.scale, rest[1].scale);
		Check.Equal(Quat.Identity, rest[1].rotation);

		SceneObject body = SceneSystem.CreateObject("Posed");
		Skeleton skeleton = body.AddComponent<Skeleton>();
		Span<BoneTransform> pose = stackalloc BoneTransform[1];
		Check.Equal(0, skeleton.GetLocalPose(pose));
		skeleton.SkeletonAsset = rig;
		Check.Equal(2, skeleton.GetLocalPose(pose));
		skeleton.SetBoneLocalPose(1, new BoneTransform(new Vec3(0f, 1f, 0f), Quat.Identity, new Vec3(2f, 2f, 2f)));
		Span<BoneTransform> both = stackalloc BoneTransform[2];
		Check.Equal(2, skeleton.GetLocalPose(both));
		Check.Equal(new Vec3(0f, 1f, 0f), both[1].position);
		Check.Equal(new Vec3(2f, 2f, 2f), both[1].scale);
		Check.Equal(Vec3.Zero, both[0].position);
		Check.Equal("Skeleton has no bone 2 (2 bone(s))", Refusal(() => skeleton.SetBoneLocalPose(2, BoneTransform.Identity)));
		skeleton.SkeletonAsset = null;
		Check.Equal("Skeleton has no bone 0 (0 bone(s))", Refusal(() => skeleton.SetBoneLocalPose(0, BoneTransform.Identity)));
		Retire(body);
		BindingProbe.FreeAsset(rigId);
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldRenderingTypes() {
		SceneObject source = SceneSystem.CreateObject("Lit");
		Lamp lamp = source.AddComponent<Lamp>();
		lamp.eye = source.AddComponent<Camera>();
		lamp.light = source.AddComponent<Light>();
		lamp.body = source.AddComponent<MeshRenderer>();
		lamp.paint = AnimationTests.Stored<Material>(GLOW);

		SceneObject copy = SceneSystem.Instantiate(source);
		Lamp copied = copy.GetComponent<Lamp>()!;
		Check.Same(copy.GetComponent<Camera>(), copied.eye);
		Check.Same(copy.GetComponent<Light>(), copied.light);
		Check.Same(copy.GetComponent<MeshRenderer>(), copied.body);
		Check.Same(lamp.paint, copied.paint);
		Retire(source);
		Retire(copy);
	}

	[ConformanceTest]
	internal static void RepeatedRenderingCallsAllocateNothing() {
		ulong meshId = BindingProbe.MakeMesh(0);
		SceneObject stage = SceneSystem.CreateObject("Stage");
		Camera camera = stage.AddComponent<Camera>();
		Light light = stage.AddComponent<Light>();
		MeshRenderer renderer = stage.AddComponent<MeshRenderer>();
		Skeleton skeleton = stage.AddComponent<Skeleton>();
		ulong rigId = BindingProbe.MakeSkeletonAsset();
		skeleton.SkeletonAsset = ObjectCache.Find<SkeletonAsset>(rigId);
		renderer.Mesh = ObjectCache.Find<Mesh>(meshId);
		renderer.SetMaterial(0, AnimationTests.Stored<Material>(GLOW));
		Material paint = renderer.GetMaterialInstance(0)!;
		paint.SetFloat("_Glow", 1f);
		paint.SetColor("_Tint", Color.White);
		List<Camera> cameras = new(4);
		RenderSystem.GetCameras(cameras);
		Span<BoneTransform> pose = stackalloc BoneTransform[2];
		BoneTransform raised = new(new Vec3(0f, 1f, 0f), Quat.Identity, Vec3.One);

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			camera.FieldOfViewDeg = 70f;
			hits += camera.FieldOfViewDeg == 70f && camera.ViewportRect.size.x == 1f ? 1 : 0;
			light.Intensity = 2f;
			light.Color = Color.Red;
			hits += light.Intensity == 2f && light.Color == Color.Red ? 1 : 0;
			paint.SetFloat("_Glow", round);
			paint.SetColor("_Tint", Color.Blue);
			hits += paint.GetFloat("_Glow") == round && paint.GetColor("_Tint") == Color.Blue && paint.HasFloat("_Glow") ? 1 : 0;
			hits += ReferenceEquals(renderer.GetRenderedMaterial(0), paint) && renderer.MaterialCount == 1u ? 1 : 0;
			skeleton.SetBoneLocalPose(1, raised);
			hits += skeleton.GetLocalPose(pose) == 2 && pose[1].position.y == 1f ? 1 : 0;
			DebugLines.Draw(Vec3.Zero, Vec3.One, Color.Yellow);
			RenderSystem.GetCameras(cameras);
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(5000, hits);
		Check.Equal(0L, allocated);
		Retire(stage);
		BindingProbe.FreeAsset(rigId);
		BindingProbe.FreeAsset(meshId);
	}
}
