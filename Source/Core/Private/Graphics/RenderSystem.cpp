#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Graphics/ShaderGlobals.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Decal.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/FramePass.h"
#include "Shalltear/Core/Graphics/GraphicsEvents.h"
#include "Shalltear/Core/Graphics/GraphicsProfile.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/LODGroup.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/ObjectShadow.h"
#include "Shalltear/Core/Graphics/ProceduralRenderer.h"
#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHISwapChain.h"
#include "Shalltear/Core/Graphics/ReflectionProbe.h"
#include "Shalltear/Core/Graphics/RenderPass.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Graphics/SkinAsset.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Math/HalfFloat.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"

#include "Asset/EngineBuiltins.h"
#include "StaticLinkAnchors.h"
#include "Graphics/EnvironmentBlend.h"
#include "Graphics/EnvironmentIBL.h"
#include "Graphics/Exposure.h"
#include "Shalltear/Core/Graphics/CubeFace.h"
#include "Graphics/CameraViewport.h"
#include "Graphics/DeviceContent.h"
#include "Graphics/FrameCapture.h"
#include "Graphics/FramePassInternal.h"
#include "Graphics/GraphicsSettings.h"
#include "Graphics/LightCulling.h"
#include "Graphics/PresentPolicy.h"
#include "Graphics/ProceduralRendererInternal.h"
#include "Graphics/RenderGraph.h"
#include "Graphics/SceneRequests.h"
#include "Graphics/ShadowAtlas.h"
#include "Graphics/ShadowCascades.h"
#include "Graphics/ShadowCasterCulling.h"
#include "Graphics/Skinning.h"
#include "Graphics/TextureSampling.h"
#include "Graphics/UniformPacking.h"
#include "Module/EngineStateTable.h"
#include "ScreenInternal.h"
#include "Shalltear/Core/Window.h"
#include "Window/FramePacing.h"
#include "Window/WindowInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Shalltear {

	using GraphicsInternal::GraphExecution;
	using GraphicsInternal::GraphPass;
	using GraphicsInternal::GraphResourceId;
	using GraphicsInternal::PassWork;

	namespace GraphicsInternal {

		struct PassContextInit {
			GraphExecution* exec = nullptr;
			RHI::TextureHandle color;
			RHI::TextureHandle depth;
			RHI::Format colorFormat = RHI::Format::Unknown;
			RHI::Format depthFormat = RHI::Format::Unknown;
			u32 samples = 1;
			Vec2I extent{1, 1};
			Mat4 view = Mat4::Identity();
			Mat4 projection = Mat4::Identity();
			Vec3 cameraPosition{};
			u32 scopeIndex = 0;
			PassRequests requests = PassRequests::None;
			RHI::TextureHandle sceneColor;
			RHI::TextureHandle sceneDepth;
			RHI::TextureHandle sceneNormals;
			u32 step = 0;
			u32 stepCount = 1;
			RHI::TextureHandle passInput0;
			RHI::TextureHandle passInput1;
			RHI::TextureHandle passVolume;
			bool writesSceneTarget = true;
		};

		struct ProceduralFrame {
			struct Renderer {
				Handle<ProceduralRenderer> renderer;
				AABB bounds;
				bool bounded = false;
				bool isolated = false;
				u64 sceneId = 0;
				const TypeInfo* type = nullptr;
				u32 firstItem = 0;
				u32 itemCount = 0;
				u32 firstStep = 0;
				u32 stepCount = 0;
				u32 renderingLayerMask = 1;
				bool tested = false;
				bool drawn = false;
			};
			struct Item {
				Handle<MaterialAsset> material;
				Vec3 sortPoint{};
				u32 firstRead = 0;
				u32 readCount = 0;
				u32 index = 0;
				u32 renderer = 0;
				ShadowCasting castShadows = ShadowCasting::Off;
			};
			struct Read {
				RHI::BufferHandle buffer;
				RHI::ResourceState state = RHI::ResourceState::ShaderResource;
			};
			struct Step {
				u32 firstAccess = 0;
				u32 accessCount = 0;
				u32 index = 0;
				bool writes = false;
			};
			struct StepAccess {
				RHI::BufferHandle buffer;
				RHI::ResourceState state = RHI::ResourceState::UnorderedAccess;
				bool write = false;
			};
			Vector<Renderer> renderers{GetMemoryResource(MemTag::Graphics)};
			Vector<Item> items{GetMemoryResource(MemTag::Graphics)};
			Vector<Read> reads{GetMemoryResource(MemTag::Graphics)};
			Vector<Step> steps{GetMemoryResource(MemTag::Graphics)};
			Vector<StepAccess> stepAccesses{GetMemoryResource(MemTag::Graphics)};
		};

		struct ProceduralPlanInit {
			ProceduralFrame* frame = nullptr;
			u32 renderer = 0;
		};

		struct ProceduralContextInit {
			RHI::CommandList* cmd = nullptr;
			RHI::Device* device = nullptr;
			const ProceduralFrame* frame = nullptr;
			u32 item = 0;
			ProceduralPass pass = ProceduralPass::Scene;
			RHI::Format colorFormat = RHI::Format::Unknown;
			RHI::Format depthFormat = RHI::Format::Unknown;
			u32 samples = 1;
			Vec2I extent{1, 1};
			Mat4 view = Mat4::Identity();
			Mat4 projection = Mat4::Identity();
			Vec3 cameraPosition{};
			RHI::TextureHandle sceneColor;
			RHI::TextureHandle sceneDepth;
			RHI::TextureHandle sceneNormals;
			RHI::TransientAllocation viewBlock{};
			RHI::TextureHandle shadowAtlas;
			RHI::TextureHandle punctualAtlas;
			RHI::TextureHandle environmentCube;
			RHI::TextureHandle environmentCube2;
			RHI::TextureHandle ambientOcclusion;
			u32* drawCalls = nullptr;
			u32 value = 0;
			Vec2 casterBias{};
		};

		struct ProceduralCulling {
			static void Publish(const ProceduralFrame& frame) {
				for (const ProceduralFrame::Renderer& entry : frame.renderers) {
					if (ProceduralRenderer* renderer = entry.renderer.Get()) {
						renderer->_tested = entry.tested;
						renderer->_drawn = entry.drawn;
					}
				}
			}
		};

	} // namespace GraphicsInternal

	namespace {

		const u8 _FRAME_STAGE_OWNER = 0;

		struct GPULight {
			Vec3 position;
			f32 invRangeSquared = 0.0f;
			Vec3 color;
			f32 spotScale = 0.0f;
			Vec3 direction;
			f32 spotOffset = 1.0f;
			u32 shadowIndex = 0;
			u32 shadowCount = 0;
			f32 shadowStrength = 1.0f;
			// bits 0-7 layers, 8-15 flags, 16-31 extension slot
			u32 info = 0;
		};
		static_assert(sizeof(GPULight) == 64);

		struct GPULightCookie {
			Vec4 rows[3];
			u32 entry = 0;
			u32 projection = 0;
			u32 pad0 = 0;
			u32 pad1 = 0;
		};
		static_assert(sizeof(GPULightCookie) == sizeof(GPULight), "1 extension slot, must match ShalltearLightCookie");

		struct GPUDecal {
			Vec4 rows[3];
			Vec4 uvScaleBias;
			Vec4 baseColor;
			Vec3 emissive;
			f32 normalScale = 1.0f;
			u32 baseColorEntry = RHI::TABLE_NONE;
			u32 normalEntry = RHI::TABLE_NONE;
			u32 maskEntry = RHI::TABLE_NONE;
			u32 emissiveEntry = RHI::TABLE_NONE;
			f32 angleFadeScale = 0.0f;
			f32 angleFadeBias = 1.0f;
			u32 layers = 0;
			u32 pad0 = 0;
		};
		static_assert(sizeof(GPUDecal) == 128, "must match SHALLTEAR_DECAL_STRIDE");
		constexpr u32 DECAL_EVERY_LAYER = 0x100u;

		constexpr u32 LIGHT_LAYERS = 0xFFu;
		constexpr u32 LIGHT_FLAG_COOKIE = 1u << 8;
		constexpr u32 LIGHT_FLAG_EVERY_LAYER = 2u << 8;
		constexpr u32 LIGHT_EXTENSION_SHIFT = 16;
		constexpr u32 COOKIE_ORTHOGRAPHIC = 0;
		constexpr u32 COOKIE_PERSPECTIVE = 1;
		constexpr u32 COOKIE_CUBE = 2;
		// must match SHALLTEAR_TABLE_SAMPLER_SHIFT
		constexpr u32 TABLE_SAMPLER_SHIFT = 20;

		struct GPUViewUniforms {
			Mat4 viewProjection;
			Vec3 cameraPosition;
			f32 directionalLightCount = 0.0f;
			Vec3 ambientColor;
			f32 punctualLightCount = 0.0f;
			u64 lightAddress = 0;
			u64 clusterAddress = 0;
			Vec4 viewForwardRow;
			u32 clusterDimX = 0;
			u32 clusterDimY = 0;
			u32 clusterDimZ = 0;
			u32 clusterPad = 0;
			f32 clusterTileScaleX = 0.0f;
			f32 clusterTileScaleY = 0.0f;
			f32 clusterSliceScale = 0.0f;
			f32 clusterSliceBias = 0.0f;
			u64 shadowMapAddress = 0;
			f32 shadowCascadeCount = 0.0f;
			f32 shadowAtlasTexel = 0.0f;
			f32 shadowPcfTaps = 1.0f;
			f32 shadowFadeStart = 0.0f;
			f32 shadowFadeEnd = 0.0f;
			f32 transparentReceivesShadows = 1.0f;
			u64 environmentAddress = 0;
			f32 renderExtentInverseX = 0.0f;
			f32 renderExtentInverseY = 0.0f;
			u64 probeAddress = 0;
			f32 probeCount = 0.0f;
			f32 fogLayerCount = 0.0f;
			u64 viewDataAddress = 0;
			u64 decalAddress = 0;
			Vec4 shadowMoments;
		};
		static_assert(sizeof(GPUViewUniforms) == 256);

		struct GPUViewData {
			Mat4 view;
			Mat4 inverseView;
			Mat4 projection;
			Mat4 inverseProjection;
			Mat4 inverseViewProjection;
			Vec4 time;
			Vec4 projectionParams;
			Vec4 screenParams;
			u64 exposureAddress = 0;
			f32 exposurePad0 = 0.0f;
			f32 exposurePad1 = 0.0f;
		};
		static_assert(sizeof(GPUViewData) == 384);
		static_assert(offsetof(GPUViewData, time) == 320, "must match SHALLTEAR_VIEW_DATA_TIME");
		static_assert(offsetof(GPUViewData, projectionParams) == 336, "must match SHALLTEAR_VIEW_DATA_PROJECTION_PARAMS");
		static_assert(offsetof(GPUViewData, screenParams) == 352, "must match SHALLTEAR_VIEW_DATA_SCREEN_PARAMS");
		static_assert(offsetof(GPUViewData, exposureAddress) == 368, "must match SHALLTEAR_VIEW_DATA_EXPOSURE");

		struct GPUEnvironment {
			Vec4 sh[9];
			f32 mipCount0 = 1.0f;
			f32 weight0 = 1.0f;
			f32 mipCount1 = 0.0f;
			f32 weight1 = 0.0f;
			Vec4 flat;
		};
		static_assert(sizeof(GPUEnvironment) == 176, "must match SHALLTEAR_ENVIRONMENT_FLAT_OFFSET + 16 and SHALLTEAR_FOG_OFFSET");
		static_assert(offsetof(GPUEnvironment, mipCount0) == 144, "must match SHALLTEAR_ENVIRONMENT_PARAMS_OFFSET");
		static_assert(offsetof(GPUEnvironment, flat) == 160, "must match SHALLTEAR_ENVIRONMENT_FLAT_OFFSET");

		struct GPUFogLayer {
			Vec4 colorShare;
			Vec4 params;
		};
		static_assert(sizeof(GPUFogLayer) == 32, "must match SHALLTEAR_FOG_STRIDE");
		static_assert(static_cast<u8>(FogMode::Linear) == 0 && static_cast<u8>(FogMode::Exponential) == 1,
					  "must match SHALLTEAR_FOG_LINEAR and SHALLTEAR_FOG_EXPONENTIAL");

		struct GPUReflectionProbe {
			Vec4 boxMin;
			Vec4 boxMax;
			Vec4 captureWS;
		};
		static_assert(sizeof(GPUReflectionProbe) == 48, "must match SHALLTEAR_PROBE_STRIDE");

		struct GPUShadowMap {
			Mat4 worldToAtlas;
			Vec4 uvBounds;
			f32 farDepth = 0.0f;
			f32 worldTexelSize = 0.0f;
			f32 depthBias = 0.0f;
			f32 normalBias = 0.0f;
		};
		static_assert(sizeof(GPUShadowMap) == 96, "must match SHALLTEAR_SHADOW_MAP_STRIDE");
		static_assert(offsetof(GPUShadowMap, farDepth) == 80, "must match SHALLTEAR_SHADOW_MAP_FAR_OFFSET");

		constexpr u32 MAX_MATERIAL_BLOCK_BYTES = 1024;

		constexpr Vec3 DefaultAmbient{0.03f, 0.03f, 0.03f};

		constexpr u32 PROCEDURAL_SKY_FACE_SIZE = 64;

		struct GPUPerDraw {
			Mat4 world;
			Vec4 normalMatrix0;
			Vec4 normalMatrix1;
			Vec4 normalMatrix2;
			u64 vertexAddress = 0;
			f32 alphaClipThreshold = 0.0f;
			u32 flags = 0;
		};
		static_assert(sizeof(GPUPerDraw) == 128, "must match SHALLTEAR_PER_DRAW_STRIDE");
		static_assert(offsetof(GPUPerDraw, alphaClipThreshold) == 120, "must match SHALLTEAR_ALPHA_CLIP_OFFSET");
		static_assert(offsetof(GPUPerDraw, flags) == 124, "must match SHALLTEAR_DRAW_FLAGS_OFFSET");

		// scene shader push blocks start with this
		struct GPUDrawHeader {
			u64 drawAddress = 0;
			u32 value = 0;
			u32 reserved = 0;
		};
		static_assert(sizeof(GPUDrawHeader) == 16);
		constexpr u32 DRAW_HEADER_BYTES = 16;

		struct GPUWirePerDraw {
			GPUDrawHeader draw;
			u32 color = 0; // SRGB RGBA8 from Color32::ToU32
		};
		constexpr u32 WIRE_PER_DRAW_BYTES = 20;

		struct GPUSkyPush {
			Mat4 invViewProjection;
			Vec3 sunDirection;
			f32 sunCosSize = 2.0f;
			Vec3 sunColor;
			f32 skyIntensity = 1.0f;
			f32 sunAngularDiameter = 0.0f;
			u32 capturing = 0;
			f32 weight = 1.0f;
		};
		static_assert(offsetof(GPUSkyPush, weight) == 104);
		constexpr u32 SKY_PUSH_BYTES = 108;

		struct GPUDebugLinePush {
			Mat4 viewProjection;
			u64 vertexAddress = 0;
		};
		constexpr u32 DEBUG_LINE_PUSH_BYTES = 72;

		struct GPUPresentPush {
			u32 tonemap = 0;
		};
		static_assert(sizeof(GPUPresentPush) == 4);

		struct GPUDebugVertex {
			Vec3 position;
			u32 color = 0;
		};
		static_assert(sizeof(GPUDebugVertex) == 16);

		constexpr size_t EXTRACTION_ARENA_BYTES = 2 * 1024 * 1024;
		constexpr size_t LANE_ARENA_BYTES = 256 * 1024;
		constexpr u32 RENDERERS_PER_LANE = 256;

		constexpr RHI::Format SCENE_COLOR_FORMAT = RHI::Format::RGBA16Float;
		constexpr RHI::Format SCENE_DEPTH_FORMAT = RHI::Format::D32Float;

		constexpr RHI::Format SCENE_NORMALS_FORMAT = RHI::Format::RGBA8Unorm;

		constexpr RHI::Format VALUE_FORMAT = RHI::Format::R32Uint;

		constexpr RHI::Format AMBIENT_OCCLUSION_FORMAT = RHI::Format::R8Unorm;

		constexpr u32 SHADOW_ATLAS_SIZE = 2048;
		constexpr i32 MIN_SHADOW_ATLAS_SIZE = 256;
		constexpr i32 MAX_SHADOW_ATLAS_SIZE = 8192;
		constexpr u32 DRAW_RECEIVES_SHADOWS = 1u;
		constexpr u32 DRAW_BLEND_SHIFT = 1u;
		constexpr u32 DRAW_LAYERS_SHIFT = 8u;
		// must match ShalltearCommon.hlsli's SHALLTEAR_DRAW_FADE_ values
		constexpr u32 DRAW_FADE_SHIFT = 16u;
		constexpr u32 DRAW_FADE_ABOVE = 1u << 24;
		constexpr RHI::SpecializationConstant LODFadeConstant{SHADER_LOD_FADE_CONSTANT_ID, 1};
		constexpr RHI::SpecializationConstant ShadowMomentsConstant{SHADER_SHADOW_MOMENTS_CONSTANT_ID, 1};
		constexpr RHI::SpecializationConstant GammaColorSpaceConstant{SHADER_GAMMA_COLOR_SPACE_CONSTANT_ID, 1};
		static_assert(static_cast<u32>(RHI::BlendMode::Count) <= 8u, "the blend's 3 bits of the per-draw flags");
		static_assert(static_cast<u32>(RHI::BlendMode::Opaque) == 0u && static_cast<u32>(RHI::BlendMode::Alpha) == 1u &&
						  static_cast<u32>(RHI::BlendMode::Additive) == 2u && static_cast<u32>(RHI::BlendMode::Premultiplied) == 3u &&
						  static_cast<u32>(RHI::BlendMode::Multiply) == 4u,
					  "must match ShalltearCommon.hlsli's SHALLTEAR_BLEND_ values");
		constexpr RHI::Format SHADOW_ATLAS_FORMAT = RHI::Format::D32Float;

		constexpr f32 SHADOW_CASTER_DISTANCE = 200.0f;

		struct ExtractionLane;

		class RenderState : public Object {
		public:
			RHI::Device* device = nullptr;
			bool renderingAvailable = false;

			struct WindowContext {
				RHI::SwapChain* swapChain = nullptr;
				Handle<Window> window;
				RHI::TextureHandle sceneColor;
				Vec2I targetSize{};
				RHI::TextureHandle frameColor;
				Vec2I frameColorSize{};
				Vec2I frameSize{};
				bool presentedThisFrame = false;
			};
			HashMap<u64, WindowContext> windowContexts{GetMemoryResource(MemTag::Graphics)};

			Vector<Handle<Camera>> cameras{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Light>> lights{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<MeshRenderer>> meshRenderers{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Light>> isolatedLights{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Decal>> decals{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Decal>> isolatedDecals{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<MeshRenderer>> isolatedMeshRenderers{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Environment>> environments{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Environment>> isolatedEnvironments{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Environment>> warnedDuplicateEnvironments{GetMemoryResource(MemTag::Graphics)};
			struct WarnedSkySource {
				Handle<Environment> environment;
				u64 source = 0;
			};
			Vector<WarnedSkySource> warnedSkySources{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<ReflectionProbe>> reflectionProbes{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<ReflectionProbe>> isolatedReflectionProbes{GetMemoryResource(MemTag::Graphics)};

			struct LODRecord {
				Handle<LODGroup> group;
				u64 signature = 0;
				bool dirty = true;
				Vec3 localCenter{};
				f32 localSize = 0.0f;
				Vector<u64> renderers{GetMemoryResource(MemTag::Graphics)};
			};
			Vector<LODRecord> lodRecords{GetMemoryResource(MemTag::Graphics)};
			struct LODFrameGroup {
				const LODGroup* group = nullptr;
				u64 id = 0;
				Vec3 center{};
				f32 size = 1.0f;
			};
			Vector<LODFrameGroup> lodFrame{GetMemoryResource(MemTag::Graphics)};
			struct LODMember {
				u32 group = 0;
				u64 levels = 0;
				bool procedural = false;
			};
			HashMap<u64, LODMember> lodMembers{GetMemoryResource(MemTag::Graphics)};
			u32 lodProceduralMembers = 0;
			f32 lodBias = 1.0f;
			u32 lodFinestLevel = 0;
			bool lodCrossFade = true;
			struct LODFadeSlot {
				LODFadeState fade;
				LODChoice choice;
				u64 seen = 0;
			};
			HashMap<u64, LODFadeSlot> lodFades{GetMemoryResource(MemTag::Graphics)};
			u64 lodFrameIndex = 0;
			u64 lodView = 0;

			struct SkinnedOutput {
				u64 vertexAddress = 0;
				AABB bounds = AABB::Empty();
			};
			Vector<Handle<SkinnedMeshRenderer>> skinnedMeshRenderers{GetMemoryResource(MemTag::Graphics)};
			Vector<SkinnedOutput> skinnedOutputs{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<SkinnedMeshRenderer>> isolatedSkinnedMeshRenderers{GetMemoryResource(MemTag::Graphics)};
			Vector<SkinnedOutput> isolatedSkinnedOutputs{GetMemoryResource(MemTag::Graphics)};
			GraphicsInternal::SkinDeformer skinDeformer;
			GraphicsInternal::SkinDeformer isolatedSkinDeformer;
			Vector<Mat4> skinMatrices{GetMemoryResource(MemTag::Graphics)};

			struct SceneReader {
				AABB bounds;
				bool bounded = false;
				u64 sceneId = 0;
				u32 renderingLayerMask = 1;
				PassRequests reads = PassRequests::None;
				bool isolated = false;
			};
			Vector<SceneReader> sceneReaders{GetMemoryResource(MemTag::Graphics)};

			Vector<Handle<ProceduralRenderer>> proceduralRenderers{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<ProceduralRenderer>> isolatedProceduralRenderers{GetMemoryResource(MemTag::Graphics)};
			GraphicsInternal::ProceduralFrame procedural;
			Vector<u32> proceduralVisible{GetMemoryResource(MemTag::Graphics)};
			struct ProceduralViewStep {
				u32 renderer = 0;
				u32 step = 0;
				ProceduralView view;
			};
			Vector<ProceduralViewStep> proceduralViewSteps{GetMemoryResource(MemTag::Graphics)};

			struct ViewTarget {
				RHI::TextureHandle color;
				Vec2I size{};
				bool colorSampleable = false;
				bool drawn = false;
				bool colorLost = false;
				bool radianceLost = false;
				Vector<GraphicsInternal::CascadeView> cascadeFit{GetMemoryResource(MemTag::Graphics)};
				Vector<PickAnswer> pickAnswers{GetMemoryResource(MemTag::Graphics)};
				RHI::TextureHandle radiance;
			};
			HashMap<u32, ViewTarget> viewTargets{GetMemoryResource(MemTag::Graphics)};
			u32 nextViewTargetId = 1;

			struct ShadowFitCamera {
				Mat4 view;
				Mat4 projection;
			};

			struct PendingView {
				u32 targetId = 0;
				Mat4 view;
				Mat4 projection;
				Vec3 cameraPosition{};
				u32 cullingMask = ~0u;
				Color clearColor = Color::Black();
				ViewFill fill = ViewFill::Solid;
				Color32 wireColor = Color32::White();
				DebugView debugView = DebugView::None;
				DebugOverlay debugOverlay = DebugOverlay::None;
				bool hasTime = false;
				f64 time = 0.0;
				bool tonemap = true;
				Handle<Camera> camera;
				RHI::TransientAllocation viewAllocation{};
				bool retainCascadeFit = false;
				bool radianceCapture = false;
				bool hasShadowFit = false;
				ShadowFitCamera shadowFit;
				u32 cascadeCount = 0;
				u32 firstLine = 0;
				u32 lineCount = 0;
				u32 firstOverlayLine = 0;
				u32 overlayLineCount = 0;
				u32 firstRow = 0;
				u32 rowCount = 0;
				u32 firstScene = 0;
				u32 sceneCount = 0;
				u32 firstSkipped = 0;
				u32 skippedCount = 0;
				SceneKind kind = SceneKind::World;
				u32 scopeIndex = 0;
				u32 firstProcedural = 0;
				u32 proceduralCount = 0;
				PassRequests sceneReads = PassRequests::None;
				u32 firstCaster = 0;
				u32 casterCount = 0;
				u32 firstOutline = 0;
				u32 outlineCount = 0;
				Color32 outlineColors[MAX_OUTLINE_COLORS];
				u32 outlineColorCount = 0;
				f32 outlineWidth = 2.0f;
				f32 outlineHiddenOpacity = 0.3f;
				bool hasPick = false;
				ViewPick pick;
				u32 pickSlot = ~0u;
			};
			Vector<PendingView> pendingViews{GetMemoryResource(MemTag::Graphics)};
			bool hasViewTime = false;
			f64 viewTime = 0.0;
			u64 viewExposureAddress = 0;

			struct PassScope {
				u32 firstScene = 0;
				u32 sceneCount = 0;
				SceneKind kind = SceneKind::World;
				f32 exposure = 1.0f;
				u64 exposureAddress = 0;
			};
			Vector<PassScope> passScopes{GetMemoryResource(MemTag::Graphics)};
			Vector<DebugLine> viewLines{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<RenderPass>> viewPassRows{GetMemoryResource(MemTag::Graphics)};
			Vector<u64> viewScenes{GetMemoryResource(MemTag::Graphics)};
			Vector<u64> viewSkippedTypes{GetMemoryResource(MemTag::Graphics)};
			struct OutlineEntry {
				u64 object = 0;
				u32 value = 0;
			};
			Vector<OutlineEntry> viewOutlines{GetMemoryResource(MemTag::Graphics)};

			static constexpr u32 PICK_SLOTS = 8;
			struct PickSlot {
				bool inUse = false;
				u64 frame = 0;
				u32 targetId = 0;
				u64 tag = 0;
				Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Graphics)};
			};
			PickSlot pickSlots[PICK_SLOTS];

			struct FrameCapture {
				Handle<Window> window;
				ViewTargetId target = INVALID_VIEW_TARGET;
				RHI::BufferHandle readback;
				Vec2I size{};
				RHI::Format format = RHI::Format::Unknown;
				u64 requestedFrame = 0;
				u64 copiedFrame = 0;
				bool copied = false;
				bool failed = false;
				GraphicsInternal::CaptureDelivery deliver;
				GraphicsInternal::CaptureFollowUp followUp;
			};
			Vector<FrameCapture> frameCaptures{GetMemoryResource(MemTag::Graphics)};

			Vector<MaterialAsset::Param> globalValues{GetMemoryResource(MemTag::Graphics)};
			struct GlobalTexture {
				StringHash hash;
				Handle<TextureAsset> texture;
			};
			Vector<GlobalTexture> globalTextures{GetMemoryResource(MemTag::Graphics)};
			RHI::BufferHandle pickRing;
			u64 renderedFrames = 0;

			ShadowCascadeFit shadowCascadeFit = ShadowCascadeFit::Fixed;
			static constexpr u32 DEPTH_RANGE_SLOTS = 32;
			struct DepthRangeSlot {
				bool inUse = false;
				u64 frame = 0;
				u64 view = 0;
			};
			DepthRangeSlot depthRangeSlots[DEPTH_RANGE_SLOTS];
			RHI::BufferHandle depthRangeRing;
			RHI::BufferHandle depthRangeScratch;
			RHI::BufferHandle depthRangeSeed;
			HashMap<u64, GraphicsInternal::CascadeDepthFit> depthFits{GetMemoryResource(MemTag::Graphics)};
			Handle<ShaderAsset> depthRangeShader;
			Handle<ShaderAsset> depthRangeShaderMultisampled;
			bool depthRangeShadersTried = false;

			struct RowPlan {
				u32 firstStep = 0;
				u32 firstTarget = 0;
				u16 stepCount = 0;
				u16 targetCount = 0;
				RHI::TextureHandle volume;
			};
			struct RowTarget {
				GraphicsInternal::GraphResourceId resource = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				RHI::Format format = RHI::Format::Unknown;
				Vec2I size{1, 1};
			};
			Vector<RowPlan> rowPlans{GetMemoryResource(MemTag::Graphics)};
			Vector<PassStep> rowPlanSteps{GetMemoryResource(MemTag::Graphics)};
			Vector<RowTarget> rowPlanTargets{GetMemoryResource(MemTag::Graphics)};

			struct RowVolume {
				RHI::TextureHandle texture;
				Vec3I size{};
				RHI::Format format = RHI::Format::Unknown;
				u64 revision = 0;
				bool uploaded = false;
			};
			HashMap<Handle<RenderPass>, RowVolume> rowVolumes{GetMemoryResource(MemTag::Graphics)};

			bool sceneCamerasRenderToWindows = true;

			struct CameraDepthRecord {
				Handle<Camera> camera;
				RHI::TextureHandle image;
				Vec2I size{};
				Mat4 view = Mat4::Identity();
				Mat4 projection = Mat4::Identity();
				bool drawn = false;
				bool asked = false;
				bool kept = false;
				u32 idleFrames = 0;
			};
			Vector<CameraDepthRecord> cameraDepths{GetMemoryResource(MemTag::Graphics)};
			Vector<RHI::TextureHandle> retiredCameraDepths{GetMemoryResource(MemTag::Graphics)};

			IWindowOverlay* overlay = nullptr;

			LinearAllocator* extractionArena = nullptr;
			Vector<ExtractionLane*> extractionLanes{GetMemoryResource(MemTag::Graphics)};
			MaterialAsset* errorMaterial = nullptr;

			struct CameraWork {
				Camera* camera = nullptr;
				u32 firstRow = 0;
				u32 rowCount = 0;
				u32 scopeIndex = 0;
				u32 firstProcedural = 0;
				u32 proceduralCount = 0;
				PassRequests sceneReads = PassRequests::None;
				u32 firstCaster = 0;
				u32 casterCount = 0;
				RectI pixels;
				bool composited = false;
				bool inlineResolve = false;
				bool discardColor = false;
				bool discardDepth = false;
				bool resolvedPost = false;
			};
			struct WindowWork {
				Handle<Window> window;
				ViewTargetId target = INVALID_VIEW_TARGET;
				Vector<CameraWork> cameras{GetMemoryResource(MemTag::Graphics)};
			};
			Vector<WindowWork> frameWork{GetMemoryResource(MemTag::Graphics)};
			struct CameraSubmission {
				ViewTargetId target = INVALID_VIEW_TARGET;
				u32 firstCamera = 0;
				u32 cameraCount = 0;
			};
			Vector<CameraSubmission> cameraSubmissions{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Camera>> submittedCameras{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<Window>> frameOverlayWindows{GetMemoryResource(MemTag::Graphics)};

			GraphicsInternal::RenderGraph graph;

			struct PipelineEntry {
				RHI::PipelineHandle pipeline;
				u64 shaderId = 0;
				u32 shaderVersion = 0;
				bool instanced = false;
			};
			HashMap<u64, PipelineEntry> pipelineCache{GetMemoryResource(MemTag::Graphics)};
			u32 pipelineCacheSwept = 0;

			HashMap<u64, Handle<ShaderAsset>> shaderRegistry{GetMemoryResource(MemTag::Graphics)};

			struct NamedRefusal {
				Handle<ShaderAsset> shader;
				u32 version = 0;
			};
			Vector<NamedRefusal> namedMeshRefusals{GetMemoryResource(MemTag::Graphics)};
			Vector<NamedRefusal> namedProceduralRefusals{GetMemoryResource(MemTag::Graphics)};

			Handle<ShaderAsset> presentShader;
			Handle<ShaderAsset> standardShader;
			Handle<ShaderAsset> debugLineShader;
			Handle<ShaderAsset> wireframeShader;
			bool wireframeShaderTried = false;
			Handle<ShaderAsset> blitShader;
			bool blitShaderTried = false;
			Handle<ShaderAsset> depthCopyShader;
			bool depthCopyShaderTried = false;
			Handle<ShaderAsset> debugViewShader;
			bool debugViewShaderTried = false;
			Handle<ShaderAsset> debugOverlayShader;
			bool debugOverlayShaderTried = false;
			Handle<ShaderAsset> outlineShader;
			bool outlineShaderTried = false;
			static constexpr u32 FALLBACK_DIMENSIONS = static_cast<u32>(RHI::TextureDimension::Count);
			static constexpr u32 FALLBACK_KINDS = static_cast<u32>(ShaderTextureFallback::Gray) + 1;
			RHI::TextureHandle fallbackTextures[FALLBACK_DIMENSIONS][FALLBACK_KINDS];
			Vector<u64> dimensionMismatchesWarned{GetMemoryResource(MemTag::Graphics)};
			bool fallbackRefused[FALLBACK_DIMENSIONS][FALLBACK_KINDS] = {};
			RHI::SamplerHandle repeatSampler;
			RHI::SamplerHandle clampSampler;
			struct CachedSampler {
				u32 key = 0;
				RHI::SamplerHandle handle;
			};
			Vector<CachedSampler> textureSamplers{GetMemoryResource(MemTag::Graphics)};

			std::mutex hotReloadMutex;
			Vector<str> hotReloadPaths{GetMemoryResource(MemTag::Graphics)};

			i32 vsyncOverride = -1;

			bool physicalLightUnits = false;
			ColorSpace colorSpace = ColorSpace::Linear;

			GraphicsProfile* applied = nullptr;
			GraphicsProfile* builtIn = nullptr;

			const GraphicsSettings* graphicsSettings = nullptr;

			Handle<GraphicsProfile> activeProfile;
			Guid activeProfileId;
			bool profileSelectionDirty = true;
			Guid warnedProfileId;

			u32 msaaSamples = 1;

			f32 renderScale = 1.0f;

			TonemapCurve tonemap = TonemapCurve::None;

			u32 lightsPerView = 256;
			u32 lightsPerCluster = 32;
			u32 decalsPerCluster = 32;

			f32 shadowDistance = 60.0f;
			u32 shadowCascades = 4;
			f32 shadowFirstCascadeDistance = 4.0f;
			u32 shadowPcfTaps = 5;
			u32 shadowAtlasSize = SHADOW_ATLAS_SIZE;
			i32 reflectionProbeResolution = 128;
			i32 shadowAtlasCreatedAt = -1;
			bool transparentReceiveShadows = true;

			TextureAnisotropy anisotropicFiltering = TextureAnisotropy::PerTexture;
			i32 maxAnisotropy = 16;

			RHI::TextureHandle shadowAtlas;
			RHI::TextureHandle punctualShadowAtlas;
			RHI::SamplerHandle shadowSampler;
			ShadowFilter shadowFilter = ShadowFilter::PCF;
			u32 shadowMomentBlur = 0;
			// positive exponent, negative exponent, bleed reduction
			Vec3 shadowMomentParams{40.0f, 5.0f, 0.25f};
			RHI::TextureHandle momentAtlas;
			RHI::TextureHandle momentDepth;
			u32 momentSamples = 1;
			Handle<ShaderAsset> momentResolveShader;
			bool momentResolveShaderTried = false;

			bool MomentsActive() const {
				return shadowFilter == ShadowFilter::Moments && momentAtlas.IsValid() && momentDepth.IsValid() && momentResolveShader.Get();
			}
			RHI::TextureHandle DirectionalShadowTexture() const { return MomentsActive() ? momentAtlas : shadowAtlas; }

			struct EnvironmentSetup {
				RHI::TextureHandle cube;
				u32 mipCount = 1;
				Vec3 sh[9]{};
				f32 skyIntensity = 1.0f;
				f32 reflectionIntensity = 1.0f;
				f32 radianceScale = 1.0f;
				Vec3 flatRadiance{};
				bool drawSky = false;
				f32 sunCosSize = 2.0f;
				// rads
				f32 sunAngularDiameter = 0.0f;
				Handle<MaterialAsset> skyMaterial;
				Vec3 sunTint{1.0f, 1.0f, 1.0f};
			};

			static constexpr u32 MAX_SKY_LAYERS = 4;
			struct BlendedEnvironment {
				RHI::TextureHandle cube[2];
				u32 mipCount[2]{};
				f32 cubeWeight[2]{};
				Vec3 flatRadiance{};
				f32 reflectionIntensity = 1.0f;
				Vec3 sh[9]{};
				EnvironmentSetup skies[MAX_SKY_LAYERS];
				f32 skyAlpha[MAX_SKY_LAYERS]{};
				u32 skyCount = 0;
				Vec3 sunTint{1.0f, 1.0f, 1.0f};
				struct FogLayer {
					Vec3 color{};
					f32 share = 0.0f;
					Vec3 params{};
				};
				FogLayer fog[GraphicsInternal::MAX_ENVIRONMENT_LAYERS];
				u32 fogCount = 0;
			};

			RHI::TextureHandle defaultEnvironmentCube;
			RHI::TextureHandle defaultReflectionProbeArray;
			struct ProbeSliceKey {
				u64 instance = 0;
				u32 version = 0;
				bool operator==(const ProbeSliceKey&) const = default;
			};
			RHI::TextureHandle reflectionProbeArray;
			u32 reflectionProbeArrayCubes = 0;
			i32 reflectionProbeArrayFace = 0;
			Vector<ProbeSliceKey> reflectionProbeArrayKeys{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<ReflectionProbe>> reflectionProbeSlices{GetMemoryResource(MemTag::Graphics)};
			struct SkippedProbe {
				Handle<ReflectionProbe> probe;
				u64 cube = 0;
				u32 version = 0;
				i32 face = 0;
				RHI::Format format = RHI::Format::Unknown;
			};
			Vector<SkippedProbe> skippedProbes{GetMemoryResource(MemTag::Graphics)};
			RHI::TextureHandle defaultAmbientOcclusion;
			RHI::SamplerHandle linearClampSampler;
			struct BakedSky {
				Handle<Environment> environment;
				u64 key = 0;
				RHI::TextureHandle cube;
				u32 mipCount = 1;
				Vec3 sh[9]{};
			};
			Vector<BakedSky> bakedSkies{GetMemoryResource(MemTag::Graphics)};

			struct SkyCapture {
				Handle<Environment> environment;
				RHI::TextureHandle source;
				RHI::TextureHandle chain;
				u32 faceSize = 0;
				u32 sourceMips = 0;
				u32 chainMips = 0;
				u64 key = 0;
				u32 servedRequests = 0;
				bool filled = false;
				bool seen = false;
				bool reached = false;
				bool hasClock = false;
				f64 clock = 0.0;
				f32 viewExposure = 0.0f;
				Handle<MaterialAsset> material;
				Vec3 sunDirection{0.0f, 1.0f, 0.0f};
				Vec3 sunColor{};
				f32 sunCosSize = 2.0f;
				f32 sunAngularDiameter = 0.0f;
				RHI::BufferHandle readback;
				u32 shMip = 0;
				u32 shSide = 0;
				u64 slotFrame[RHI::FRAMES_IN_FLIGHT]{};
				bool slotPending[RHI::FRAMES_IN_FLIGHT]{};
				u32 writeSlot = RHI::FRAMES_IN_FLIGHT;
				Vec3 sh[9]{};
				bool haveSH = false;
				f32 chainExposure = 1.0f;
				f32 slotExposure[RHI::FRAMES_IN_FLIGHT]{};
			};
			Vector<SkyCapture> skyCaptures{GetMemoryResource(MemTag::Graphics)};

			Handle<ShaderAsset> skyShader;
			bool skyShaderTried = false;
			Handle<ShaderAsset> skyPrefilterShader;
			bool skyPrefilterShaderTried = false;
			Handle<ShaderAsset> skinningShader;
			bool skinningShaderTried = false;

			struct ShadowAllocation {
				u64 lightKey = 0;
				u32 firstMap = 0;
				u32 mapCount = 0;
				u32 objectFirst = 0;
				u32 objectCount = 0;
				f32 strength = 1.0f;
			};
			struct ShadowSetup {
				u64 owner = 0;
				u64 mapAddress = 0;
				u32 mapCount = 0;
				u32 cascadeCount = 0;
				f32 fadeStart = 0.0f;
				f32 fadeEnd = 0.0f;
			};
			ShadowSetup pendingShadow;
			Vector<ShadowAllocation> shadowAllocations{GetMemoryResource(MemTag::Graphics)};

			struct ObjectShadowRecord {
				Handle<ObjectShadow> shadow;
				f32 radius = 0.0f;
			};
			Vector<ObjectShadowRecord> objectShadows{GetMemoryResource(MemTag::Graphics)};
			struct ObjectShadowPick {
				u32 record = 0;
				i32 priority = 0;
				f32 distanceSquared = 0.0f;
				f32 normalOffset = 0.0f;
				u64 id = 0;
				AABB bounds = AABB::Empty();
				u32 firstMember = 0;
				u32 memberCount = 0;
			};
			Vector<ObjectShadowPick> objectShadowPicks{GetMemoryResource(MemTag::Graphics)};
			Vector<u64> objectShadowMemberIds{GetMemoryResource(MemTag::Graphics)};
			HashMap<u64, u32> objectShadowMembers{GetMemoryResource(MemTag::Graphics)};
			Vector<SceneObject*> objectShadowStack{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<MeshRenderer>> objectShadowMeshes{GetMemoryResource(MemTag::Graphics)};
			Vector<Handle<SkinnedMeshRenderer>> objectShadowSkinned{GetMemoryResource(MemTag::Graphics)};
			u32 objectShadowBudget = 4;
			u32 objectShadowResolution = 1024;

			struct ShadowKeep {
				u64 owner = 0;
				u64 frame = 0;
				Vector<u64> lights{GetMemoryResource(MemTag::Graphics)};
			};
			Vector<ShadowKeep> shadowKeeps{GetMemoryResource(MemTag::Graphics)};

			ShadowKeep& ShadowKeepFor(u64 owner) {
				for (ShadowKeep& keep : shadowKeeps) {
					if (keep.owner == owner) {
						return keep;
					}
				}
				ShadowKeep& added = shadowKeeps.emplace_back();
				added.owner = owner;
				return added;
			}

			u32 ShadowAtlasSize() const { return shadowDistance > 0.0f ? shadowAtlasSize : 1u; }
			u32 PunctualAtlasSize() const { return ShadowAtlasSize(); }

			Vec2I ViewRenderSize(const PendingView& pending, Vec2I native) const {
				return pending.radianceCapture ? native : ScaledSize(native);
			}

			Vec2I ScaledSize(Vec2I native) const {
				if (renderScale == 1.0f) {
					return native;
				}
				return {Math::Max(static_cast<i32>(std::lround(static_cast<f32>(native.x) * renderScale)), 1),
						Math::Max(static_cast<i32>(std::lround(static_cast<f32>(native.y) * renderScale)), 1)};
			}
			bool ShadowsEnabled() const {
				return shadowDistance > 0.0f && shadowAtlas.IsValid() && punctualShadowAtlas.IsValid();
			}
			Vector<GraphicsInternal::CascadeView> cascadeViews{GetMemoryResource(MemTag::Graphics)};
			Vector<GraphicsInternal::ShadowRequest> shadowRequests{GetMemoryResource(MemTag::Graphics)};
			Vector<GraphicsInternal::PunctualView> punctualViews{GetMemoryResource(MemTag::Graphics)};
			Vector<GraphicsInternal::CullVolume> casterVolumes{GetMemoryResource(MemTag::Graphics)};
			GraphicsInternal::ShadowAtlasAllocator punctualAllocator;

			u32 lastDrawCalls = 0;
			u32 lastTriangles = 0;
			GraphicsInternal::PresentCounts lastPresents;
			Vector<RHI::SwapChain*> presentedSwapChains{GetMemoryResource(MemTag::Graphics)};

			u32 frameLightsBinned = 0;
			u32 frameLightsDropped = 0;
			u32 frameClusterLightsDropped = 0;
			u32 lastLightsBinned = 0;
			u32 lastLightsDropped = 0;
			u32 lastClusterLightsDropped = 0;
			u32 frameShadowMapsRendered = 0;
			u32 frameShadowLights = 0;
			u32 frameShadowLightsDropped = 0;
			u32 lastShadowMapsRendered = 0;
			u32 lastShadowLights = 0;
			u32 lastShadowLightsDropped = 0;
			u32 frameProceduralDrawn = 0;
			u32 frameProceduralCulled = 0;
			u32 lastProceduralDrawn = 0;
			u32 lastProceduralCulled = 0;
			u32 frameDrawCallsSaved = 0;
			u32 lastDrawCallsSaved = 0;
			u32 frameExtractionLanes = 0;
			u32 lastExtractionLanes = 0;

			Vector<GraphicsInternal::LightCandidate> lightCandidates{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> clusterTable{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> clusterIndices{GetMemoryResource(MemTag::Graphics)};
			Vector<f32> clusterKeys{GetMemoryResource(MemTag::Graphics)};
			struct DecalCandidate {
				GPUDecal record;
				GraphicsInternal::DecalBound bound;
			};
			Vector<DecalCandidate> decalCandidates{GetMemoryResource(MemTag::Graphics)};
			// priority << 32 | candidate idx
			Vector<u64> decalOrder{GetMemoryResource(MemTag::Graphics)};
			Vector<GraphicsInternal::DecalBound> decalBounds{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> decalTable{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> decalIndices{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> mergedClusterHeader{GetMemoryResource(MemTag::Graphics)};
			Vector<u32> mergedClusterIndices{GetMemoryResource(MemTag::Graphics)};

			void OnWindowResized(const WindowResizedEvent& event) {
				if (event.live) {
					return;
				}
				auto it = windowContexts.find(event.window.GetInstanceId());
				if (it == windowContexts.end() || !it->second.swapChain) {
					return;
				}
				bool vsync = ResolveVSync(event.window);
				it->second.swapChain->Recreate(event.clientSizePx, vsync);
			}

			void OnWindowDestroying(const WindowDestroyingEvent& event) {
				DestroyWindowContext(event.window.GetInstanceId());
			}

			void OnSettingChanged(const SettingChangedEvent& event) {
				if (event.categoryPath == "Graphics") {
					profileSelectionDirty = true;
				}
			}

			bool ResolveVSync(Handle<Window> window) const {
				if (vsyncOverride >= 0) {
					return vsyncOverride != 0;
				}
				Window* resolved = window.Get();
				return resolved ? resolved->GetVSyncHint() : true;
			}

			void DestroyWindowContext(u64 windowId) {
				auto it = windowContexts.find(windowId);
				if (it == windowContexts.end()) {
					return;
				}
				WindowContext& context = it->second;
				if (device) {
					if (context.sceneColor.IsValid()) {
						device->DestroyResource(context.sceneColor);
					}
					if (context.frameColor.IsValid()) {
						device->DestroyResource(context.frameColor);
					}
					if (context.swapChain) {
						device->DestroySwapChain(context.swapChain);
					}
				}
				windowContexts.erase(it);
			}
		};

		RenderState* _state = nullptr;

		template <typename T>
		void SwapRemoveHandle(Vector<Handle<T>>& list, Handle<T> handle) {
			for (size_t i = 0; i < list.size(); ++i) {
				if (list[i] == handle) {
					list[i] = list.back();
					list.pop_back();
					return;
				}
			}
		}

		template <typename T>
		bool ContainsHandle(const Vector<Handle<T>>& list, Handle<T> handle) {
			for (const Handle<T>& entry : list) {
				if (entry == handle) {
					return true;
				}
			}
			return false;
		}

		Handle<ShaderAsset> LoadEngineShader(const Guid& guid, const char* name) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			if (!asset) {
				SLT_LOG_WARN("Graphics", "engine shader '{}' unavailable", name);
				return {};
			}
			if (asset->GetAssetTypeId() != StringHash("ShaderAsset")) {
				AssetDatabase::UnloadAsset(asset);
				return {};
			}
			return Handle<ShaderAsset>(static_cast<ShaderAsset*>(asset));
		}

		ShaderAsset* EnsureWireframeShader() {
			RenderState& state = *_state;
			if (!state.wireframeShaderTried) {
				state.wireframeShaderTried = true;
				state.wireframeShader = LoadEngineShader(EngineBuiltins::WireframeShader, "Wireframe");
			}
			return state.wireframeShader.Get();
		}

		ShaderAsset* EnsureBlitShader() {
			RenderState& state = *_state;
			if (!state.blitShaderTried) {
				state.blitShaderTried = true;
				state.blitShader = LoadEngineShader(EngineBuiltins::BlitShader, "Blit");
			}
			return state.blitShader.Get();
		}

		ShaderAsset* EnsureDepthCopyShader() {
			RenderState& state = *_state;
			if (!state.depthCopyShaderTried) {
				state.depthCopyShaderTried = true;
				state.depthCopyShader = LoadEngineShader(EngineBuiltins::DepthCopyShader, "DepthCopy");
			}
			return state.depthCopyShader.Get();
		}

		ShaderAsset* EnsureDebugViewShader() {
			RenderState& state = *_state;
			if (!state.debugViewShaderTried) {
				state.debugViewShaderTried = true;
				state.debugViewShader = LoadEngineShader(EngineBuiltins::DebugViewShader, "DebugView");
			}
			return state.debugViewShader.Get();
		}

		ShaderAsset* EnsureDebugOverlayShader() {
			RenderState& state = *_state;
			if (!state.debugOverlayShaderTried) {
				state.debugOverlayShaderTried = true;
				state.debugOverlayShader = LoadEngineShader(EngineBuiltins::DebugOverlayShader, "DebugOverlay");
			}
			return state.debugOverlayShader.Get();
		}

		ShaderAsset* EnsureOutlineShader() {
			RenderState& state = *_state;
			if (!state.outlineShaderTried) {
				state.outlineShaderTried = true;
				state.outlineShader = LoadEngineShader(EngineBuiltins::OutlineShader, "Outline");
			}
			return state.outlineShader.Get();
		}

		ShaderAsset* EnsureSkyShader() {
			RenderState& state = *_state;
			if (!state.skyShaderTried) {
				state.skyShaderTried = true;
				state.skyShader = LoadEngineShader(EngineBuiltins::SkyShader, "Sky");
			}
			return state.skyShader.Get();
		}

		ShaderAsset* EnsureSkyPrefilterShader() {
			RenderState& state = *_state;
			if (!state.skyPrefilterShaderTried) {
				state.skyPrefilterShaderTried = true;
				state.skyPrefilterShader = LoadEngineShader(EngineBuiltins::SkyPrefilterShader, "SkyPrefilter");
			}
			return state.skyPrefilterShader.Get();
		}

		bool FitsDepth() {
			RenderState& state = *_state;
			if (state.shadowCascadeFit != ShadowCascadeFit::VisibleDepth) {
				return false;
			}
			if (!state.depthRangeShadersTried) {
				state.depthRangeShadersTried = true;
				state.depthRangeShader = LoadEngineShader(EngineBuiltins::DepthRangeShader, "DepthRange");
				state.depthRangeShaderMultisampled = LoadEngineShader(EngineBuiltins::DepthRangeMultisampledShader, "DepthRangeMS");
			}
			return state.depthRangeShader.Get() && state.depthRangeShaderMultisampled.Get();
		}

		ShaderAsset* EnsureSkinningShader() {
			RenderState& state = *_state;
			if (!state.skinningShaderTried) {
				state.skinningShaderTried = true;
				state.skinningShader = LoadEngineShader(EngineBuiltins::SkinningShader, "Skinning");
			}
			return state.skinningShader.Get();
		}

		void ReleaseEngineShader(Handle<ShaderAsset>& handle) {
			if (Asset* asset = handle.Get()) {
				AssetDatabase::UnloadAsset(asset);
			}
			handle = {};
		}

		void ApplyCasterBias(RHI::ShaderRenderState& state, const Vec2& casterBias) {
			if (casterBias.x == 0.0f && casterBias.y == 0.0f) {
				return;
			}
			state.dynamicDepthBias = true;
			// reverse Z (away from light = lower depth)
			state.depthBiasConstant -= casterBias.x;
			state.depthBiasSlope -= casterBias.y;
		}

		u64 RenderStateKey(const RHI::ShaderRenderState& state) {
			u64 key = static_cast<u64>(state.blend);
			key = key * 31 + static_cast<u64>(state.depth);
			key = key * 31 + static_cast<u64>(state.cull);
			key = key * 31 + static_cast<u64>(state.topology);
			key = key * 31 + static_cast<u64>(state.fill);
			key = key * 31 + (state.dynamicDepthBias ? 1u : 0u);
			u32 bits = 0;
			if (!state.dynamicDepthBias) {
				std::memcpy(&bits, &state.depthBiasConstant, sizeof(bits));
				key = key * 31 + bits;
				std::memcpy(&bits, &state.depthBiasSlope, sizeof(bits));
				key = key * 31 + bits;
			}
			key = key * 31 + static_cast<u64>(state.depthCompare);
			return key;
		}

		void SweepDeadPipelines() {
			RenderState& state = *_state;
			constexpr u32 FIRST_SWEEP = 64;
			const u32 size = static_cast<u32>(state.pipelineCache.size());
			if (size < FIRST_SWEEP || size < state.pipelineCacheSwept * 2) {
				return;
			}
			for (auto it = state.pipelineCache.begin(); it != state.pipelineCache.end();) {
				const auto* shader = static_cast<const ShaderAsset*>(ObjectRegistry::Resolve(it->second.shaderId));
				if (shader == nullptr || shader->version != it->second.shaderVersion) {
					state.device->DestroyResource(it->second.pipeline);
					it = state.pipelineCache.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = state.shaderRegistry.begin(); it != state.shaderRegistry.end();) {
				it = it->second.Get() == nullptr ? state.shaderRegistry.erase(it) : std::next(it);
			}
			state.pipelineCacheSwept = static_cast<u32>(state.pipelineCache.size());
		}

		u64 PipelineKey(u64 shaderId, u32 shaderVersion, u32 variant, ShaderProgramKind kind,
						RHI::Format colorFormat, RHI::Format depthFormat, u64 stateKey, u32 samples, bool lodFade, bool moments,
						bool gamma) {
			u64 key = shaderId * 1099511628211ull;
			key ^= lodFade ? 0x9E3779B97F4A7C15ull : 0ull;
			key ^= moments ? 0xC2B2AE3D27D4EB4Full : 0ull;
			key ^= gamma ? 0x165667B19E3779F9ull : 0ull;
			key ^= static_cast<u64>(shaderVersion) << 32;
			key ^= static_cast<u64>(kind) << 62;
			key ^= static_cast<u64>(variant) << 40;
			key ^= static_cast<u64>(samples) << 24;
			key ^= static_cast<u64>(colorFormat) << 8;
			key ^= static_cast<u64>(depthFormat);
			key ^= stateKey * 1099511628211ull;
			return key;
		}

		bool ReadsInstanceIndex(std::span<const u8> bytecode, RHI::ShaderBytecodeFormat format) {
			if (format == RHI::ShaderBytecodeFormat::WGSL) {
				std::string_view text(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
				return text.find("instance_index") != std::string_view::npos;
			}
			constexpr u32 OP_LOAD = 61;
			constexpr u32 OP_DECORATE = 71;
			constexpr u32 DECORATION_BUILT_IN = 11;
			constexpr u32 BUILT_IN_INSTANCE_INDEX = 43;
			constexpr size_t HEADER_WORDS = 5;
			if (bytecode.size() % 4 != 0 || bytecode.size() / 4 <= HEADER_WORDS) {
				return false;
			}
			const size_t count = bytecode.size() / 4;
			auto word = [&](size_t at) {
				u32 value = 0;
				std::memcpy(&value, bytecode.data() + at * 4, 4);
				return value;
			};
			u32 instanceIndex = 0;
			for (size_t at = HEADER_WORDS; at < count;) {
				const u32 first = word(at);
				const u32 length = first >> 16;
				const u32 op = first & 0xFFFFu;
				if (length == 0 || at + length > count) {
					return false;
				}
				if (op == OP_DECORATE && length >= 4 && word(at + 2) == DECORATION_BUILT_IN && word(at + 3) == BUILT_IN_INSTANCE_INDEX) {
					instanceIndex = word(at + 1);
				} else if (op == OP_LOAD && length >= 4 && instanceIndex != 0 && word(at + 3) == instanceIndex) {
					return true;
				}
				at += length;
			}
			return false;
		}

		bool ShaderDeclaresBinding(const ShaderAsset& shader, u32 set, u32 binding) {
			for (const RHI::DescriptorBindingDesc& declared : shader.bindings) {
				if (declared.set == set && declared.binding == binding) {
					return true;
				}
			}
			return false;
		}

		constexpr u32 SHADOW_ATLAS_BINDING = 1;
		constexpr u32 SHADOW_SAMPLER_BINDING = 2;
		constexpr u32 PUNCTUAL_ATLAS_BINDING = 3;
		constexpr u32 ENVIRONMENT_CUBE_BINDING = 4;
		constexpr u32 ENVIRONMENT_SAMPLER_BINDING = 5;
		constexpr u32 AMBIENT_OCCLUSION_BINDING = 6;
		constexpr u32 AMBIENT_OCCLUSION_SAMPLER_BINDING = 7;
		constexpr u32 REFLECTION_PROBE_BINDING = 8;
		constexpr u32 ENVIRONMENT_CUBE2_BINDING = 9;
		// must match ShalltearGlobals.hlsli
		constexpr u32 SHADER_GLOBALS_BINDING = 10;
		constexpr u32 SHADER_GLOBAL_RESOURCE_BINDING = 11;

		RHI::PipelineHandle GetOrCreatePipeline(ShaderAsset* shader, RHI::Format colorFormat, RHI::Format depthFormat,
												const RHI::ShaderRenderState* stateOverride = nullptr, u32 samples = 1,
												u32 variant = SHADER_BASE_VARIANT,
												ShaderProgramKind kind = ShaderProgramKind::Forward, bool* outInstanced = nullptr,
												bool lodFade = false) {
			RenderState& state = *_state;
			if (outInstanced) {
				*outInstanced = false;
			}
			const ShaderVariant* code = shader->GetVariant(variant);
			if (!code) {
				code = shader->GetVariant(SHADER_BASE_VARIANT);
				variant = SHADER_BASE_VARIANT;
			}
			if (!code) {
				return {};
			}
			if (!code->HasProgram(kind)) {
				return {};
			}
			u64 shaderId = shader->GetInstanceId();
			const RHI::ShaderRenderState& pipelineState = stateOverride ? *stateOverride : shader->renderState;
			lodFade = lodFade && kind != ShaderProgramKind::Compute;
			const bool moments = kind == ShaderProgramKind::Forward && state.MomentsActive() && ShaderDeclaresBinding(*shader, 0, SHADOW_ATLAS_BINDING);
			const bool gamma = kind != ShaderProgramKind::Compute && state.colorSpace == ColorSpace::Gamma;
			u64 key = PipelineKey(shaderId, shader->version, variant, kind, colorFormat, depthFormat, RenderStateKey(pipelineState), samples,
								  lodFade, moments, gamma);
			auto it = state.pipelineCache.find(key);
			if (it != state.pipelineCache.end()) {
				if (outInstanced) {
					*outInstanced = it->second.instanced;
				}
				return it->second.pipeline;
			}
			auto remember = [&](RHI::PipelineHandle pipeline, bool instanced = false) {
				if (pipeline.IsValid()) {
					SweepDeadPipelines();
					state.pipelineCache.emplace(key, RenderState::PipelineEntry{pipeline, shaderId, shader->version, instanced});
					state.shaderRegistry.emplace(shaderId, Handle<ShaderAsset>(shader));
					if (outInstanced) {
						*outInstanced = instanced;
					}
				}
				return pipeline;
			};

			if (kind == ShaderProgramKind::Compute) {
				RHI::ComputePipelineDesc desc;
				desc.spirv = {code->computeSpirv.data(), code->computeSpirv.size()};
				desc.bytecodeFormat = shader->bytecodeFormat;
				desc.bindings = {shader->bindings.data(), shader->bindings.size()};
				desc.pushConstantSize = shader->pushConstantSize;
				desc.debugName = shader->virtualPath.c_str();
				return remember(state.device->CreateComputePipeline(desc));
			}

			const bool depthOnly = kind == ShaderProgramKind::Depth;
			const bool noColor = colorFormat == RHI::Format::Unknown;
			auto blob = [](const Vector<u8>& bytes) { return std::span<const u8>{bytes.data(), bytes.size()}; };
			const bool valuePass = depthOnly && colorFormat == VALUE_FORMAT;
			if (valuePass && code->depthValuePixelSpirv.empty()) {
				return RHI::PipelineHandle{};
			}
			const bool clips = shader->integrator == ShaderTemplate::None
								   ? !code->depthClipPixelSpirv.empty()
								   : std::find(code->keywords.begin(), code->keywords.end(), ShaderAlphaClipKeyword) != code->keywords.end();
			RHI::GraphicsPipelineDesc desc;
			desc.vertexSpirv = depthOnly ? blob(code->depthVertexSpirv) : blob(code->vertexSpirv);
			desc.pixelSpirv = depthOnly ? (noColor	   ? (clips || lodFade ? blob(code->depthClipPixelSpirv) : std::span<const u8>{})
										   : valuePass ? blob(code->depthValuePixelSpirv)
													   : blob(code->depthPixelSpirv))
										: blob(code->pixelSpirv);
			desc.bytecodeFormat = shader->bytecodeFormat;
			RHI::SpecializationConstant constants[3];
			u32 constantCount = 0;
			if (lodFade) {
				constants[constantCount++] = LODFadeConstant;
			}
			if (moments) {
				constants[constantCount++] = ShadowMomentsConstant;
			}
			if (gamma) {
				constants[constantCount++] = GammaColorSpaceConstant;
			}
			desc.pixelConstants = {constants, constantCount};
			desc.bindings = {shader->bindings.data(), shader->bindings.size()};
			desc.pushConstantSize = shader->pushConstantSize;
			desc.blend = pipelineState.blend;
			desc.depth = pipelineState.depth;
			desc.cull = pipelineState.cull;
			desc.topology = pipelineState.topology;
			desc.fill = pipelineState.fill;
			desc.depthBiasConstant = pipelineState.depthBiasConstant;
			desc.depthBiasSlope = pipelineState.depthBiasSlope;
			desc.dynamicDepthBias = pipelineState.dynamicDepthBias;
			desc.depthCompare = pipelineState.depthCompare;
			desc.colorFormats[0] = colorFormat;
			desc.colorFormatCount = noColor ? 0u : 1u;
			desc.depthFormat = depthFormat;
			desc.samples = samples;
			desc.debugName = shader->virtualPath.c_str();

			return remember(state.device->CreateGraphicsPipeline(desc), ReadsInstanceIndex(desc.vertexSpirv, desc.bytecodeFormat));
		}

		void WriteShaderGlobals(RenderState& state, RHI::Device& device, RHI::TransientSetWriter& writer, const ShaderAsset& shader);

		RHI::TransientSet CreateViewSet(RHI::Device& device, RHI::PipelineHandle pipeline, const ShaderAsset& shader,
										const RHI::TransientAllocation& viewAllocation, RHI::TextureHandle atlas,
										RHI::TextureHandle punctualAtlas, RHI::TextureHandle environmentCube = {},
										RHI::TextureHandle ambientOcclusion = {}, RHI::TextureHandle environmentCube2 = {}) {
			RenderState& state = *_state;
			RHI::TransientSetWriter writer = device.CreateTransientSet(pipeline, 0);
			writer.WriteUniformBuffer(0, viewAllocation.buffer, viewAllocation.offset, sizeof(GPUViewUniforms));
			bool wroteAtlas = false;
			if (atlas.IsValid() && ShaderDeclaresBinding(shader, 0, SHADOW_ATLAS_BINDING)) {
				writer.WriteTexture(SHADOW_ATLAS_BINDING, atlas);
				wroteAtlas = true;
			}
			if (punctualAtlas.IsValid() && ShaderDeclaresBinding(shader, 0, PUNCTUAL_ATLAS_BINDING)) {
				writer.WriteTexture(PUNCTUAL_ATLAS_BINDING, punctualAtlas);
				wroteAtlas = true;
			}
			if (wroteAtlas && state.shadowSampler.IsValid() && ShaderDeclaresBinding(shader, 0, SHADOW_SAMPLER_BINDING)) {
				writer.WriteSampler(SHADOW_SAMPLER_BINDING, state.shadowSampler);
			}
			RHI::TextureHandle cube = environmentCube.IsValid() ? environmentCube : state.defaultEnvironmentCube;
			if (cube.IsValid() && ShaderDeclaresBinding(shader, 0, ENVIRONMENT_CUBE_BINDING)) {
				writer.WriteTexture(ENVIRONMENT_CUBE_BINDING, cube);
				if (state.linearClampSampler.IsValid() && ShaderDeclaresBinding(shader, 0, ENVIRONMENT_SAMPLER_BINDING)) {
					writer.WriteSampler(ENVIRONMENT_SAMPLER_BINDING, state.linearClampSampler);
				}
			}
			RHI::TextureHandle cube2 = environmentCube2.IsValid() ? environmentCube2 : state.defaultEnvironmentCube;
			if (cube2.IsValid() && ShaderDeclaresBinding(shader, 0, ENVIRONMENT_CUBE2_BINDING)) {
				writer.WriteTexture(ENVIRONMENT_CUBE2_BINDING, cube2);
			}
			RHI::TextureHandle probes = state.reflectionProbeArray.IsValid() ? state.reflectionProbeArray
																			 : state.defaultReflectionProbeArray;
			if (probes.IsValid() && ShaderDeclaresBinding(shader, 0, REFLECTION_PROBE_BINDING)) {
				writer.WriteTexture(REFLECTION_PROBE_BINDING, probes);
			}
			RHI::TextureHandle occlusion = ambientOcclusion.IsValid() ? ambientOcclusion : state.defaultAmbientOcclusion;
			if (occlusion.IsValid() && ShaderDeclaresBinding(shader, 0, AMBIENT_OCCLUSION_BINDING)) {
				writer.WriteTexture(AMBIENT_OCCLUSION_BINDING, occlusion);
				if (state.linearClampSampler.IsValid() && ShaderDeclaresBinding(shader, 0, AMBIENT_OCCLUSION_SAMPLER_BINDING)) {
					writer.WriteSampler(AMBIENT_OCCLUSION_SAMPLER_BINDING, state.linearClampSampler);
				}
			}
			WriteShaderGlobals(state, device, writer, shader);
			return writer.Commit();
		}

		void InvalidateShaderPipelines(u64 shaderId) {
			RenderState& state = *_state;
			for (auto it = state.pipelineCache.begin(); it != state.pipelineCache.end();) {
				if (it->second.shaderId == shaderId) {
					state.device->DestroyResource(it->second.pipeline);
					it = state.pipelineCache.erase(it);
				} else {
					++it;
				}
			}
		}

		str NormalizePathKey(std::string_view path) {
			str normalized(path);
			for (char& c : normalized) {
				if (c == '\\') {
					c = '/';
				} else {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
			return normalized;
		}

		str SourceOf(const ShaderAsset& shader) {
			if (!shader.virtualPath.empty()) {
				return FileSystem::Resolve(Path(shader.virtualPath)).ToString();
			}
			return shader.dependencies.empty() ? str() : shader.dependencies[0].physicalPath;
		}

		ShaderAsset* Reimport(const ShaderAsset& shader, const str& source) {
			Path sourcePath(source);
			if (shader.virtualPath.empty()) {
				ShaderAsset* scratch = New<ShaderAsset>(MemTag::Asset);
				if (scratch->Import(sourcePath)) {
					return scratch;
				}
				Delete(scratch);
				return nullptr;
			}
			IAssetImporter* importer = AssetImporterRegistry::FindByExtension(sourcePath.GetExtension());
			if (!importer || importer->GetAssetType() != StringHash("ShaderAsset")) {
				SLT_LOG_WARN("Graphics", "shader '{}' changed (no importer makes a shader from it now)", shader.virtualPath);
				return nullptr;
			}
			Asset* imported = importer->ImportWithSettings(sourcePath, Path(shader.virtualPath), JSONValue{});
			return static_cast<ShaderAsset*>(imported);
		}

		void DrainHotReloads() {
			RenderState& state = *_state;
			Vector<str> pending{GetMemoryResource(MemTag::Graphics)};
			{
				std::lock_guard lock(state.hotReloadMutex);
				pending.swap(state.hotReloadPaths);
			}
			if (pending.empty()) {
				return;
			}

			Path library = FileSystem::Resolve(Path("project://Library"));
			str libraryKey = library.ToString().find("://") == str::npos ? NormalizePathKey(library.ToString()) + "/" : str();

			Vector<str> changedKeys{GetMemoryResource(MemTag::Graphics)};
			for (const str& changed : pending) {
				str changedKey = NormalizePathKey(changed);
				if (libraryKey.empty() || !changedKey.starts_with(libraryKey)) {
					changedKeys.push_back(std::move(changedKey));
				}
			}
			auto changedNow = [&](std::string_view path) {
				str key = NormalizePathKey(path);
				return std::find(changedKeys.begin(), changedKeys.end(), key) != changedKeys.end();
			};

			for (auto& [shaderId, handle] : state.shaderRegistry) {
				ShaderAsset* shader = handle.Get();
				if (!shader) {
					continue;
				}
				str source = SourceOf(*shader);
				bool affected = !source.empty() && changedNow(source);
				for (const ShaderAsset::Dependency& dependency : shader->dependencies) {
					if (affected) {
						break;
					}
					affected = changedNow(dependency.physicalPath);
				}
				if (!affected || source.empty()) {
					continue;
				}

				ShaderAsset* scratch = Reimport(*shader, source);
				bool success = scratch != nullptr;
				if (success) {
					for (size_t v = SHADER_BASE_VARIANT + 1; v < shader->variants.size(); ++v) {
						const Vector<str>& wanted = shader->variants[v].keywords;
						scratch->GetOrCompileVariant({wanted.data(), wanted.size()});
					}
					state.device->WaitIdle();
					shader->AdoptImportedPayload(*scratch);
					++shader->version;
					InvalidateShaderPipelines(shaderId);
					SLT_LOG_INFO("Graphics", "shader '{}' hot-reloaded", shader->virtualPath);
				} else {
					SLT_LOG_ERROR("Graphics", "shader '{}' hot reload failed (using the last-good binary)", shader->virtualPath);
				}
				if (scratch) {
					Delete(scratch);
				}
			}
		}

		u32 CountMaterialTextureSlots(const ShaderAsset& shader) {
			u32 count = 0;
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set == MATERIAL_DESCRIPTOR_SET && resource.type == RHI::DescriptorType::SampledTexture) {
					++count;
				}
			}
			return count;
		}

		// i-th texture pairs with the i-th sampler by binding
		template <typename Fn>
		void ForEachMaterialTextureSlot(const ShaderAsset& shader, Fn&& visit) {
			u32 index = 0;
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set != MATERIAL_DESCRIPTOR_SET || resource.type != RHI::DescriptorType::SampledTexture) {
					continue;
				}
				u32 samplerIndex = 0;
				for (const ShaderResourceInfo& candidate : shader.resources) {
					if (candidate.set != MATERIAL_DESCRIPTOR_SET || candidate.type != RHI::DescriptorType::Sampler) {
						continue;
					}
					if (samplerIndex == index) {
						visit(resource, candidate.binding);
						break;
					}
					++samplerIndex;
				}
				++index;
			}
		}

		struct DrawTexture {
			RHI::TextureHandle texture;
			RHI::SamplerHandle sampler;
			u32 textureBinding = 0;
			u32 samplerBinding = 0;
		};

		enum class MeshDrawRefusal : u8;

		// applied on main thread, in walk order
		struct LaneDeferral {
			enum class Kind : u8 {
				Sampler,
				Fallback,
				Mismatch,
				Refusal,
			};
			Kind kind = Kind::Sampler;
			DrawTexture* out = nullptr;
			const TextureAsset* texture = nullptr;
			ShaderAsset* shader = nullptr;
			const MaterialAsset* material = nullptr;
			const ShaderResourceInfo* slot = nullptr;
			u64 key = 0;
			RHI::TextureDimension dimension = RHI::TextureDimension::Texture2D;
			ShaderTextureFallback fallback = ShaderTextureFallback::White;
			MeshDrawRefusal refusal{};
		};

		struct DrawMaterial {
			ShaderAsset* shader = nullptr;
			u32 variant = SHADER_BASE_VARIANT;
			u64 materialId = 0;
			u64 sortGroup = 0;
			const u8* materialBlock = nullptr;
			u32 materialBlockSize = 0;
			const DrawTexture* textures = nullptr;
			u32 textureCount = 0;
			RHI::ShaderRenderState renderState;
			f32 alphaClipThreshold = 0.0f;
			i32 sortingPriority = 0;
			bool alpha = false;
			PassRequests sceneReads = PassRequests::None;
		};

		struct DrawEntry : DrawMaterial {
			MeshAsset* mesh = nullptr;
			const SubMesh* submesh = nullptr;
			u64 vertexAddress = 0;
			Mat4 world;
			f32 depth = 0.0f;
			u32 renderingLayerMask = 1;
			Handle<SceneObject> object;
			bool mirrored = false;
			bool receiveShadows = true;
			ShadowCasting castShadows = ShadowCasting::On;
			u64 volumeMask = 0;
			u8 lodFade = 0;
			bool lodFadeAbove = false;
		};

		enum class DrawListUse : u8 {
			View,
			Casters
		};

		bool DrawsFor(ShadowCasting casting, DrawListUse use) {
			return use == DrawListUse::Casters ? casting != ShadowCasting::Off : casting != ShadowCasting::ShadowsOnly;
		}

		// 4 bands per distance octave (wider bands cost overdraw)
		u32 DepthBand(f32 depthSquared) {
			u32 bits = 0;
			std::memcpy(&bits, &depthSquared, sizeof(bits));
			return bits >> 22;
		}

		constexpr u32 LANE_ENTRY_BITS = 26;
		static_assert(JobSystem::MAX_LANES <= (1u << (32 - LANE_ENTRY_BITS)));

		struct DrawOrder {
			u64 key[3] = {};
			f32 depth = 0.0f;
			u32 entry = 0; // lane << LANE_ENTRY_BITS | index into lane's entries
		};

		// entry = last tie-break (order should not depend on lane count)
		struct DrawOrderLess {
			bool operator()(const DrawOrder& a, const DrawOrder& b) const {
				for (u32 word = 0; word < 3; ++word) {
					if (a.key[word] != b.key[word]) {
						return a.key[word] < b.key[word];
					}
				}
				if (a.depth != b.depth) {
					return a.depth < b.depth;
				}
				return a.entry < b.entry;
			}
		};

		// an id's low 32 bits = its live registry slot
		DrawOrder OrderOf(const DrawEntry& entry, u32 depthBand, u32 index) {
			DrawOrder order;
			order.entry = index;
			order.key[0] = (static_cast<u64>(entry.alpha) << 32) | (static_cast<u32>(entry.sortingPriority) ^ 0x80000000u);
			if (entry.alpha) {
				order.depth = -entry.depth;
				return order;
			}
			const u64 submesh = Math::Min<u64>(static_cast<u64>(entry.submesh - entry.mesh->submeshes.data()), 0xFFFF);
			order.key[1] = entry.sortGroup;
			order.key[2] = (static_cast<u64>(entry.lodFade != 0) << 60) | (static_cast<u64>(depthBand) << 50) |
						   (static_cast<u64>(static_cast<u32>(entry.mesh->GetInstanceId())) << 18) | (submesh << 2) |
						   (static_cast<u64>(entry.mirrored) << 1) | static_cast<u64>(entry.castShadows == ShadowCasting::TwoSided);
			order.depth = entry.depth;
			return order;
		}

		struct ExtractedDraws {
			DrawEntry** list = nullptr;
			u32 count = 0;
			u64 perDrawAddress = 0;
		};

		void PackMaterialBlock(DrawMaterial& entry, LinearAllocator& arena, ShaderAsset* target, const MaterialAsset* source) {
			const UniformBlockLayout* layout = target ? target->GetMaterialParamLayout() : nullptr;
			if (!layout || layout->sizeBytes == 0 || layout->sizeBytes > MAX_MATERIAL_BLOCK_BYTES) {
				return;
			}
			u8* block = arena.AllocateAs<u8>(layout->sizeBytes);
			if (source) {
				source->PackBlock(*layout, {block, layout->sizeBytes},
								  {target->memberDefaults.data(), target->memberDefaults.size()});
			} else {
				std::memset(block, 0, layout->sizeBytes);
			}
			entry.materialBlock = block;
			entry.materialBlockSize = layout->sizeBytes;
		}

		void FillFallbackTexture(RHI::Device& device, RHI::TextureHandle texture, RHI::TextureDimension dimension, ShaderTextureFallback fallback) {
			static constexpr u8 FALLBACK_RGBA[][4] = {{255, 255, 255, 255}, {0, 0, 0, 255}, {128, 128, 255, 255}, {0, 0, 0, 0}, {128, 128, 128, 255}};
			static_assert(std::size(FALLBACK_RGBA) == static_cast<size_t>(ShaderTextureFallback::Gray) + 1);
			const u32 layers = dimension == RHI::TextureDimension::Cube || dimension == RHI::TextureDimension::CubeArray ? 6u : 1u;
			for (u32 layer = 0; layer < layers; ++layer) {
				device.UploadTexture(texture, FALLBACK_RGBA[static_cast<u32>(fallback)], 0, layer);
			}
		}

		RHI::TextureHandle CreateFallbackTexture(RHI::Device& device, RHI::TextureDimension dimension, ShaderTextureFallback fallback,
												 const char* debugName) {
			RHI::TextureDesc desc;
			desc.size = {1, 1};
			desc.format = RHI::Format::RGBA8Unorm;
			desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			desc.dimension = dimension;
			desc.debugName = debugName;
			RHI::TextureHandle texture = device.CreateTextureResource(desc);
			if (texture.IsValid()) {
				FillFallbackTexture(device, texture, dimension, fallback);
			}
			return texture;
		}

		RHI::TextureHandle FallbackTexture(RenderState& state, RHI::TextureDimension dimension, ShaderTextureFallback fallback) {
			const u32 d = static_cast<u32>(dimension);
			const u32 k = static_cast<u32>(fallback);
			if (!state.fallbackTextures[d][k].IsValid() && !state.fallbackRefused[d][k]) {
				state.fallbackTextures[d][k] = CreateFallbackTexture(*state.device, dimension, fallback, "MaterialFallback");
				state.fallbackRefused[d][k] = !state.fallbackTextures[d][k].IsValid();
			}
			return state.fallbackTextures[d][k];
		}

		u32 TextureSamplerKey(const RenderState& state, const TextureAsset& texture, RHI::SamplerDesc& outDesc) {
			const bool volume = texture.GetDimension() == RHI::TextureDimension::Texture3D;
			outDesc = GraphicsInternal::TextureSamplerDesc(texture.settings, volume, state.anisotropicFiltering, state.maxAnisotropy);
			return static_cast<u32>(outDesc.filter) | static_cast<u32>(outDesc.mipFilter) << 1 | static_cast<u32>(outDesc.addressMode) << 2 |
				   static_cast<u32>(outDesc.maxAnisotropy) << 4;
		}

		const RenderState::CachedSampler* FindTextureSampler(const RenderState& state, u32 key) {
			for (const RenderState::CachedSampler& cached : state.textureSamplers) {
				if (cached.key == key) {
					return &cached;
				}
			}
			return nullptr;
		}

		RHI::SamplerHandle TextureSampler(RenderState& state, const TextureAsset& texture);

		void WriteShaderGlobals(RenderState& state, RHI::Device& device, RHI::TransientSetWriter& writer, const ShaderAsset& shader) {
			for (const UniformBlockLayout& block : shader.uniformBlocks) {
				if (block.set != 0 || block.binding != SHADER_GLOBALS_BINDING || block.sizeBytes == 0) {
					continue;
				}
				RHI::TransientAllocation allocation = device.AllocateTransient(block.sizeBytes);
				if (!allocation.IsValid()) {
					break;
				}
				const std::span<u8> bytes{static_cast<u8*>(allocation.cpu), block.sizeBytes};
				std::memset(bytes.data(), 0, bytes.size());
				for (const UniformMember& member : block.members) {
					const StringHash name(std::string_view{member.name});
					for (const MaterialAsset::Param& value : state.globalValues) {
						if (value.hash == name) {
							GraphicsInternal::WriteUniformMember(member, value, bytes);
							break;
						}
					}
				}
				writer.WriteUniformBuffer(block.binding, allocation.buffer, allocation.offset, block.sizeBytes);
			}

			const TextureAsset* paired[16] = {};
			u32 textures = 0;
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set != 0 || resource.binding < SHADER_GLOBAL_RESOURCE_BINDING || resource.type != RHI::DescriptorType::SampledTexture) {
					continue;
				}
				const StringHash name(std::string_view{resource.name});
				const TextureAsset* texture = nullptr;
				for (const RenderState::GlobalTexture& global : state.globalTextures) {
					if (global.hash == name) {
						texture = global.texture.Get();
						break;
					}
				}
				if (texture && (!texture->texture.IsValid() || texture->GetDimension() != resource.dimension)) {
					texture = nullptr;
				}
				writer.WriteTexture(resource.binding, texture ? texture->texture
															  : FallbackTexture(state, resource.dimension, ShaderTextureFallback::White));
				if (textures < std::size(paired)) {
					paired[textures] = texture;
				}
				++textures;
			}
			u32 samplers = 0;
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set != 0 || resource.binding < SHADER_GLOBAL_RESOURCE_BINDING || resource.type != RHI::DescriptorType::Sampler) {
					continue;
				}
				const TextureAsset* texture = samplers < std::size(paired) && samplers < textures ? paired[samplers] : nullptr;
				writer.WriteSampler(resource.binding, texture ? TextureSampler(state, *texture) : state.linearClampSampler);
				++samplers;
			}
		}

		bool DeclaresOnlyGlobalViewResources(const ShaderAsset& shader) {
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set == 0 && resource.binding < SHADER_GLOBALS_BINDING) {
					return false;
				}
			}
			return true;
		}

		void BindGlobalsOnlyViewSet(RHI::CommandList& cmd, RHI::Device& device, RHI::PipelineHandle pipeline, const ShaderAsset& shader) {
			if (!SceneRequests::DeclaresViewSet(shader) || !DeclaresOnlyGlobalViewResources(shader)) {
				return;
			}
			RHI::TransientSetWriter writer = device.CreateTransientSet(pipeline, 0);
			WriteShaderGlobals(*_state, device, writer, shader);
			cmd.BindTransientSet(0, writer.Commit());
		}

		RHI::SamplerHandle TextureSampler(RenderState& state, const TextureAsset& texture) {
			RHI::SamplerDesc desc;
			const u32 key = TextureSamplerKey(state, texture, desc);
			if (const RenderState::CachedSampler* cached = FindTextureSampler(state, key)) {
				return cached->handle;
			}
			const RHI::SamplerHandle handle = state.device->CreateSamplerResource(desc);
			state.textureSamplers.push_back({key, handle});
			return handle;
		}

		void WarnDimensionMismatch(RenderState& state, u64 key, const MaterialAsset& source, const ShaderResourceInfo& slot,
								   const TextureAsset& texture) {
			if (std::find(state.dimensionMismatchesWarned.begin(), state.dimensionMismatchesWarned.end(), key) !=
				state.dimensionMismatchesWarned.end()) {
				return;
			}
			state.dimensionMismatchesWarned.push_back(key);
			SLT_LOG_WARN("Graphics", "material '{}': slot '{}' samples a {} texture and '{}' is {} (drawing its fallback)",
						 source.virtualPath, slot.name, RHI::TextureDimensionName(slot.dimension), texture.virtualPath,
						 RHI::TextureDimensionName(texture.GetDimension()));
		}

		// defer set -> no state write/RHI call (safe on a lane)
		void ResolveMaterialTextures(DrawMaterial& entry, LinearAllocator& arena, ShaderAsset* target, const MaterialAsset* source,
									 Vector<LaneDeferral>* defer = nullptr) {
			RenderState& state = *_state;
			u32 slotCount = target ? CountMaterialTextureSlots(*target) : 0;
			if (slotCount == 0) {
				return;
			}
			DrawTexture* resolved = arena.AllocateAs<DrawTexture>(slotCount);
			u32 written = 0;
			ForEachMaterialTextureSlot(*target, [&](const ShaderResourceInfo& slot, u32 samplerBinding) {
				DrawTexture& out = resolved[written++];
				out = DrawTexture{};
				out.textureBinding = slot.binding;
				out.samplerBinding = samplerBinding;
				TextureAsset* texture = source ? source->GetTexture(StringHash(std::string_view{slot.name})).Get() : nullptr;
				if (texture && texture->texture.IsValid()) {
					if (texture->GetDimension() == slot.dimension) {
						out.texture = texture->texture;
						if (!defer) {
							out.sampler = TextureSampler(state, *texture);
						} else {
							RHI::SamplerDesc desc;
							if (const RenderState::CachedSampler* cached = FindTextureSampler(state, TextureSamplerKey(state, *texture, desc))) {
								out.sampler = cached->handle;
							} else {
								LaneDeferral& deferred = defer->emplace_back();
								deferred.kind = LaneDeferral::Kind::Sampler;
								deferred.out = &out;
								deferred.texture = texture;
							}
						}
					} else {
						auto mix = [](u64 hash, u64 value) { return (hash ^ value) * 0x100000001B3ull; };
						const u64 key = mix(mix(mix(mix(0xCBF29CE484222325ull, source->assetId.low), source->assetId.high),
												StringHash(std::string_view{slot.name}).GetValue()),
											texture->assetId.low ^ texture->assetId.high);
						if (!defer) {
							WarnDimensionMismatch(state, key, *source, slot, *texture);
						} else {
							LaneDeferral& deferred = defer->emplace_back();
							deferred.kind = LaneDeferral::Kind::Mismatch;
							deferred.texture = texture;
							deferred.material = source;
							deferred.slot = &slot;
							deferred.key = key;
						}
					}
				}
				if (!out.texture.IsValid()) {
					const ShaderTextureFallback fallback = target->GetTextureFallback(slot.name);
					if (!defer) {
						out.texture = FallbackTexture(state, slot.dimension, fallback);
					} else {
						const u32 d = static_cast<u32>(slot.dimension);
						const u32 k = static_cast<u32>(fallback);
						out.texture = state.fallbackTextures[d][k];
						if (!out.texture.IsValid() && !state.fallbackRefused[d][k]) {
							LaneDeferral& deferred = defer->emplace_back();
							deferred.kind = LaneDeferral::Kind::Fallback;
							deferred.out = &out;
							deferred.dimension = slot.dimension;
							deferred.fallback = fallback;
						}
					}
					out.sampler = state.repeatSampler;
				}
				if (!out.sampler.IsValid()) {
					out.sampler = state.repeatSampler;
				}
			});
			entry.textures = resolved;
			entry.textureCount = written;
		}

		RHI::TransientSet BindMaterialSet(RHI::CommandList& cmd, RHI::Device& device,
										  RHI::PipelineHandle pipeline, const DrawMaterial& entry) {
			if (entry.materialBlockSize == 0 && entry.textureCount == 0) {
				return {};
			}
			RHI::TransientSetWriter writer = device.CreateTransientSet(pipeline, 1);
			if (entry.materialBlockSize > 0) {
				RHI::TransientAllocation materialAllocation = device.AllocateTransient(entry.materialBlockSize);
				std::memcpy(materialAllocation.cpu, entry.materialBlock, entry.materialBlockSize);
				writer.WriteUniformBuffer(0, materialAllocation.buffer, materialAllocation.offset, entry.materialBlockSize);
			}
			for (u32 slot = 0; slot < entry.textureCount; ++slot) {
				const DrawTexture& resolved = entry.textures[slot];
				writer.WriteTexture(resolved.textureBinding, resolved.texture);
				writer.WriteSampler(resolved.samplerBinding, resolved.sampler);
			}
			RHI::TransientSet set = writer.Commit();
			cmd.BindTransientSet(1, set);
			return set;
		}

		inline constexpr u32 SCENE_DESCRIPTOR_SET = SceneRequests::SET;

		struct SceneRequestTextures {
			RHI::TextureHandle color;
			RHI::TextureHandle depth;
			RHI::TextureHandle normals;
			RHI::TextureHandle passInput0;
			RHI::TextureHandle passInput1;
			RHI::TextureHandle passVolume;
		};

		struct PassView {
			Mat4 view;
			Mat4 projection;
			Vec2I extent;
		};

		GPUViewData BuildViewData(const Mat4& view, const Mat4& projection, Vec2I extent);

		RHI::TextureHandle ResolveSceneTexture(std::string_view name, const SceneRequestTextures& textures) {
			switch (SceneRequests::RequestNamed(name)) {
			case PassRequests::SceneColor:
				return textures.color;
			case PassRequests::SceneDepth:
				return textures.depth;
			case PassRequests::SceneNormals:
				return textures.normals;
			default:
				break;
			}
			if (name == "sltPassInput0") {
				return textures.passInput0;
			}
			if (name == "sltPassInput1") {
				return textures.passInput1;
			}
			if (name == "sltPassVolume") {
				return textures.passVolume;
			}
			return {};
		}

		bool BindSceneTextureSet(RHI::CommandList& cmd, RHI::Device& device, RHI::PipelineHandle pipeline,
								 const ShaderAsset& shader, const SceneRequestTextures& textures, const PassView& passView) {
			RenderState& state = *_state;
			bool declared = false;
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set == SCENE_DESCRIPTOR_SET) {
					declared = true;
					break;
				}
			}
			if (!declared) {
				return true;
			}
			RHI::TransientSetWriter writer = device.CreateTransientSet(pipeline, SCENE_DESCRIPTOR_SET);
			for (const ShaderResourceInfo& resource : shader.resources) {
				if (resource.set != SCENE_DESCRIPTOR_SET) {
					continue;
				}
				if (resource.type == RHI::DescriptorType::Sampler) {
					writer.WriteSampler(resource.binding, state.clampSampler);
					continue;
				}
				if (resource.type == RHI::DescriptorType::UniformBuffer && SceneRequests::IsViewRecord(resource.name)) {
					RHI::TransientAllocation allocation = device.AllocateTransient(sizeof(GPUViewData));
					if (!allocation.IsValid()) {
						return false;
					}
					const GPUViewData record = BuildViewData(passView.view, passView.projection, passView.extent);
					std::memcpy(allocation.cpu, &record, sizeof(GPUViewData));
					writer.WriteUniformBuffer(resource.binding, allocation.buffer, allocation.offset, sizeof(GPUViewData));
					continue;
				}
				if (resource.type != RHI::DescriptorType::SampledTexture) {
					continue;
				}
				RHI::TextureHandle resolved = ResolveSceneTexture(std::string_view{resource.name}, textures);
				writer.WriteTexture(resource.binding,
									resolved.IsValid() ? resolved : FallbackTexture(state, RHI::TextureDimension::Texture2D, ShaderTextureFallback::Black));
			}
			RHI::TransientSet set = writer.Commit();
			if (!set.IsValid()) {
				return false;
			}
			cmd.BindTransientSet(SCENE_DESCRIPTOR_SET, set);
			return true;
		}

		enum class MeshDrawRefusal : u8 {
			None,
			ReadsNoView,
			PushBlockIsNotTheHeader,
			ReadsPassInputs,
			IsASky,
			OpaqueReadsScene,
		};

		MeshDrawRefusal ClassifyMeshDraw(const ShaderAsset& shader) {
			if (shader.integrator == ShaderTemplate::Sky) {
				return MeshDrawRefusal::IsASky;
			}
			const Vector<RHI::DescriptorBindingDesc>& bindings = shader.bindings;
			if (bindings.empty() || bindings.front().set != 0 || bindings.front().binding != 0 ||
				bindings.front().type != RHI::DescriptorType::UniformBuffer) {
				return MeshDrawRefusal::ReadsNoView;
			}
			if (shader.pushConstantSize != DRAW_HEADER_BYTES) {
				return MeshDrawRefusal::PushBlockIsNotTheHeader;
			}
			if (SceneRequests::DeclaresSceneSet(shader) && !SceneRequests::ReadsOnlyScene(shader)) {
				return MeshDrawRefusal::ReadsPassInputs;
			}
			return MeshDrawRefusal::None;
		}

		PassRequests MeshSceneReads(const ShaderAsset& shader) {
			return SceneRequests::DeclaresSceneSet(shader) ? SceneRequests::DeclaredBy(shader) : PassRequests::None;
		}

		MeshDrawRefusal ClassifyMeshDraw(const ShaderAsset& shader, const MaterialAsset& material) {
			const MeshDrawRefusal refusal = ClassifyMeshDraw(shader);
			if (refusal == MeshDrawRefusal::None && MeshSceneReads(shader) != PassRequests::None &&
				material.ResolveRenderState(shader.renderState).blend == RHI::BlendMode::Opaque) {
				return MeshDrawRefusal::OpaqueReadsScene;
			}
			return refusal;
		}

		bool FirstNamingOf(Vector<RenderState::NamedRefusal>& named, ShaderAsset& shader) {
			RenderState& state = *_state;
			const u64 shaderId = shader.GetInstanceId();
			for (const RenderState::NamedRefusal& entry : named) {
				if (entry.shader.GetInstanceId() == shaderId && entry.version == shader.version) {
					return false;
				}
			}
			std::erase_if(named, [](const RenderState::NamedRefusal& entry) { return !entry.shader.IsValid(); });
			named.push_back({Handle<ShaderAsset>(&shader), shader.version});
			state.shaderRegistry.emplace(shaderId, Handle<ShaderAsset>(&shader));
			return true;
		}

		void NameMeshDrawRefusal(ShaderAsset& shader, const MaterialAsset& material, MeshDrawRefusal refusal) {
			if (!FirstNamingOf(_state->namedMeshRefusals, shader)) {
				return;
			}

			switch (refusal) {
			case MeshDrawRefusal::ReadsNoView:
				SLT_LOG_WARN("Graphics", "material '{}' can't be drawn on a mesh. Its shader '{}' never reads the view "
										 "(set 0, binding 0) and is for a render pass -> error material",
							 material.virtualPath, shader.virtualPath);
				break;
			case MeshDrawRefusal::PushBlockIsNotTheHeader:
				SLT_LOG_WARN("Graphics", "material '{}' can't be drawn on a mesh. Its shader '{}' declares a {}-byte push "
										 "block and a mesh draw pushes only the {}-byte ShalltearDrawHeader -> error material",
							 material.virtualPath, shader.virtualPath, shader.pushConstantSize, DRAW_HEADER_BYTES);
				break;
			case MeshDrawRefusal::ReadsPassInputs:
				SLT_LOG_WARN("Graphics", "material '{}' can't be drawn on a mesh. Its shader '{}' reads pass inputs "
										 "(set {}) only a render pass binds -> error material",
							 material.virtualPath, shader.virtualPath, SCENE_DESCRIPTOR_SET);
				break;
			case MeshDrawRefusal::IsASky:
				SLT_LOG_WARN("Graphics", "material '{}' can't be drawn on a mesh. Its shader '{}' is a sky (drawn behind "
										 "everything from an Environment's Sky Material) -> error material",
							 material.virtualPath, shader.virtualPath);
				break;
			case MeshDrawRefusal::OpaqueReadsScene:
				SLT_LOG_WARN("Graphics", "material '{}' can't be drawn on a mesh. It draws opaque, its shader '{}' reads the scene "
										 "(transparent only) -> error material",
							 material.virtualPath, shader.virtualPath);
				break;
			case MeshDrawRefusal::None:
				break;
			}
		}

		bool IsProceduralShader(const ShaderAsset& shader) { return shader.integrator == ShaderTemplate::None && !shader.IsKernel(); }

		bool IsMeshSurface(const ShaderAsset& shader, const MaterialAsset& material) {
			return !shader.IsKernel() && ClassifyMeshDraw(shader, material) == MeshDrawRefusal::None;
		}

		bool DrawsProceduralItem(const ShaderAsset& shader, const MaterialAsset& material) {
			return IsProceduralShader(shader) || IsMeshSurface(shader, material);
		}

		void NameProceduralDrawRefusal(ShaderAsset& shader, const MaterialAsset& material) {
			if (!FirstNamingOf(_state->namedProceduralRefusals, shader)) {
				return;
			}
			SLT_LOG_WARN("Graphics", "material '{}' can't draw a procedural item. Its shader '{}' is neither a procedural program "
									 "nor a mesh surface -> item draws nothing",
						 material.virtualPath, shader.virtualPath);
		}

		void NameProceduralCallRefusal(ShaderAsset& shader, const MaterialAsset& material, bool surface) {
			if (!FirstNamingOf(_state->namedProceduralRefusals, shader)) {
				return;
			}
			if (surface) {
				SLT_LOG_WARN("Graphics", "material '{}' has the mesh surface '{}'. Draw it with ProceduralContext::DrawTriangles",
							 material.virtualPath, shader.virtualPath);
				return;
			}
			SLT_LOG_WARN("Graphics", "material '{}' has the procedural program '{}' (not a mesh surface). Draw it with "
									 "ProceduralContext::Draw/DrawIndirect",
						 material.virtualPath, shader.virtualPath);
		}

		void NameProceduralCasterRefusal(ShaderAsset& shader, const MaterialAsset& material) {
			if (!FirstNamingOf(_state->namedProceduralRefusals, shader)) {
				return;
			}
			SLT_LOG_WARN("Graphics", "material '{}' casts no shadow: its shader '{}' has no depth program. "
									 "Declare one with #pragma shalltear_depth_program",
						 material.virtualPath, shader.virtualPath);
		}

		void RecordSkinning(RHI::Device& device, RHI::CommandList& cmd, GraphicsInternal::SkinDeformer& deformer,
							const Vector<Handle<SkinnedMeshRenderer>>& renderers,
							Vector<RenderState::SkinnedOutput>& outputs) {
			if (renderers.empty()) {
				return;
			}
			ShaderAsset* kernel = EnsureSkinningShader();
			const RHI::PipelineHandle pipeline = kernel ? GetOrCreatePipeline(kernel, RHI::Format::Unknown, RHI::Format::Unknown, nullptr, 1,
																			  SHADER_BASE_VARIANT, ShaderProgramKind::Compute)
														: RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return;
			}

			Vector<Mat4>& skinMatrices = _state->skinMatrices;
			for (size_t i = 0; i < renderers.size(); ++i) {
				SkinnedMeshRenderer* renderer = renderers[i].Get();
				if (!renderer) {
					continue;
				}
				MeshAsset* mesh = renderer->GetMesh().Get();
				if (!mesh || mesh->vertices.empty()) {
					continue;
				}

				SkinAsset* skin = renderer->GetSkin().Get();
				Skeleton* skeleton = renderer->GetSkeleton().Get();
				const bool skinned = skin != nullptr && skeleton != nullptr && !mesh->skinInfluences.empty();
				const std::span<const f32> weights = renderer->GetBlendShapeWeights();

				std::span<const Mat4> jointMatrices;
				if (skinned) {
					skeleton->UpdatePose();
					const std::span<const Mat4> modelPose = skeleton->GetModelPose();
					if (!modelPose.empty()) {
						const u32 jointCount = skin->GetJointCount();
						skinMatrices.resize(jointCount);
						GraphicsInternal::BuildSkinMatrices(*skin, modelPose, skinMatrices.data(), jointCount);
						jointMatrices = {skinMatrices.data(), jointCount};
					}
				}

				outputs[i].vertexAddress = deformer.Add(device, *mesh, jointMatrices, weights);
				if (outputs[i].vertexAddress != 0) {
					outputs[i].bounds = GraphicsInternal::SkinBounds::Pose(*mesh, jointMatrices, weights);
				}
			}
			deformer.Record(cmd, pipeline);
		}

		struct PassTargets {
			RHI::TextureHandle color;
			RHI::TextureHandle depth;
			RHI::TextureHandle resolveColor;
			RHI::Format colorFormat = SCENE_COLOR_FORMAT;
			RHI::Format depthFormat = SCENE_DEPTH_FORMAT;
			bool clear = true;
			bool drawSky = true;
			bool discardColor = false;
			bool discardDepth = false;
			u32 samples = 1;
			Vec2I extent{1, 1};
		};

		// results valid until arena reset
		class DrawMaterials {
		public:
			DrawMaterials(LinearAllocator& arena, ShaderAsset* errorShader, const MaterialAsset& errorMaterial, Vector<LaneDeferral>& defer)
				: _arena(arena), _errorShader(errorShader), _errorMaterial(errorMaterial), _defer(defer) {}

			bool Resolve(MaterialAsset* material, DrawMaterial& out) {
				if (!material) {
					return Error(out);
				}
				if (material == _lastKey) {
					out = *_lastValue;
					return true;
				}
				if (!_slots) {
					_slots = _arena.AllocateAs<Slot>(SLOT_COUNT);
					std::fill(_slots, _slots + SLOT_COUNT, Slot{});
				}
				Slot& slot = _slots[SlotOf(material)];
				if (slot.key == material) {
					out = *slot.value;
				} else {
					if (!Build(*material, out)) {
						return false;
					}
					slot = Slot{material, &out};
				}
				_lastKey = material;
				_lastValue = slot.value;
				return true;
			}
		private:
			struct Slot {
				const MaterialAsset* key = nullptr;
				const DrawMaterial* value = nullptr;
			};

			static constexpr u32 SLOT_BITS = 10;
			static constexpr u32 SLOT_COUNT = 1u << SLOT_BITS;

			static u32 SlotOf(const MaterialAsset* material) {
				return static_cast<u32>((reinterpret_cast<uintptr_t>(material) * 0x9E3779B97F4A7C15ull) >> (64 - SLOT_BITS));
			}

			static u64 SortGroupOf(const ShaderAsset& shader, u64 materialId) {
				return (static_cast<u64>(static_cast<u32>(shader.GetInstanceId())) << 32) | static_cast<u32>(materialId);
			}

			bool Build(MaterialAsset& material, DrawMaterial& out) {
				ShaderAsset* shader = material.shader.Get();
				if (shader) {
					if (const MeshDrawRefusal refusal = ClassifyMeshDraw(*shader, material); refusal != MeshDrawRefusal::None) {
						LaneDeferral& deferred = _defer.emplace_back();
						deferred.kind = LaneDeferral::Kind::Refusal;
						deferred.shader = shader;
						deferred.material = &material;
						deferred.refusal = refusal;
						shader = nullptr;
					}
				}
				if (!shader) {
					return Error(out);
				}
				out.shader = shader;
				out.variant = shader->FindVariant(material.keywordHash);
				out.materialId = material.GetInstanceId();
				out.sortGroup = SortGroupOf(*shader, out.materialId);
				PackMaterialBlock(out, _arena, shader, &material);
				ResolveMaterialTextures(out, _arena, shader, &material, &_defer);
				out.renderState = material.ResolveRenderState(shader->renderState);
				out.alphaClipThreshold = material.alphaClipThreshold;
				out.sortingPriority = material.sortingPriority;
				out.alpha = out.renderState.blend != RHI::BlendMode::Opaque;
				out.sceneReads = MeshSceneReads(*shader);
				return true;
			}

			bool Error(DrawMaterial& out) {
				if (!_error && _errorShader) {
					DrawMaterial* error = _arena.Construct<DrawMaterial>();
					error->shader = _errorShader;
					error->sortGroup = SortGroupOf(*_errorShader, 0);
					PackMaterialBlock(*error, _arena, _errorShader, &_errorMaterial);
					ResolveMaterialTextures(*error, _arena, _errorShader, nullptr, &_defer);
					error->renderState = _errorShader->renderState;
					_error = error;
				}
				if (!_error) {
					return false;
				}
				out = *_error;
				return true;
			}

			LinearAllocator& _arena;
			ShaderAsset* _errorShader = nullptr;
			const MaterialAsset& _errorMaterial;
			Vector<LaneDeferral>& _defer;
			const DrawMaterial* _error = nullptr;
			Slot* _slots = nullptr;
			const MaterialAsset* _lastKey = nullptr;
			const DrawMaterial* _lastValue = nullptr;
		};

		struct ExtractionLane {
			LinearAllocator* arena = nullptr;
			bool ownsArena = false;
			Vector<DrawEntry*> entries{GetMemoryResource(MemTag::Graphics)};
			Vector<DrawOrder> orders{GetMemoryResource(MemTag::Graphics)};
			Vector<DrawOrder> bucket{GetMemoryResource(MemTag::Graphics)};
			Vector<LaneDeferral> deferred{GetMemoryResource(MemTag::Graphics)};
		};

		struct CasterSplit {
			const HashMap<u64, u32>* members = nullptr;
			u64 cascadeBits = 0;
			u64 objectBits = 0;
			u32 firstObject = 0;
		};

		struct DrawWalk {
			DrawListUse use = DrawListUse::View;
			std::span<const u64> onlyScenes;
			std::span<const GraphicsInternal::CullVolume> volumes;
			Vec3 sortOrigin;
			u32 renderingLayerMask = ~0u;
			MaterialAsset* overrideMaterial = nullptr;
			const Vector<Handle<MeshRenderer>>* meshRenderers = nullptr;
			const Vector<Handle<SkinnedMeshRenderer>>* skinnedRenderers = nullptr;
			const Vector<RenderState::SkinnedOutput>* skinnedOutputs = nullptr;
			std::span<const LODChoice> lodChoices;
			const HashMap<u64, RenderState::LODMember>* lodMembers = nullptr;
			const CasterSplit* split = nullptr;
			ShaderAsset* errorShader = nullptr;
			const MaterialAsset* errorMaterial = nullptr;
		};

		bool LODDraws(const LODChoice& choice, u64 levels, u8& outFade, bool& outAbove) {
			outFade = 0;
			outAbove = false;
			const bool atLevel = choice.level < LODChoice::MAX_LEVELS && ((levels >> choice.level) & 1u) != 0;
			const bool atNext = choice.next < LODChoice::MAX_LEVELS && ((levels >> choice.next) & 1u) != 0;
			if (atLevel && atNext) {
				return true;
			}
			if (atLevel) {
				outFade = static_cast<u8>(255u - choice.keep);
				return choice.keep != 0;
			}
			if (atNext) {
				outFade = choice.keep;
				outAbove = true;
				return choice.keep != 255;
			}
			return false;
		}

		bool LODDraws(const DrawWalk& walk, u64 renderer, u8& outFade, bool& outAbove) {
			const auto found = walk.lodMembers->find(renderer);
			if (found == walk.lodMembers->end() || found->second.group >= walk.lodChoices.size()) {
				outFade = 0;
				outAbove = false;
				return true;
			}
			return LODDraws(walk.lodChoices[found->second.group], found->second.levels, outFade, outAbove);
		}

		// called on a job lane (no state write/RHI call)
		void WalkRenderers(const DrawWalk& walk, ExtractionLane& lane, u32 laneIndex, u32 first, u32 end) {
			lane.entries.clear();
			lane.orders.clear();
			lane.deferred.clear();
			if (lane.ownsArena) {
				lane.arena->Reset();
			}
			LinearAllocator& arena = *lane.arena;
			DrawMaterials materials(arena, walk.errorShader, *walk.errorMaterial, lane.deferred);
			const std::span<const GraphicsInternal::CullVolume> volumes = walk.volumes;
			const u32 laneBits = laneIndex << LANE_ENTRY_BITS;

			u8 lodFade = 0;
			bool lodFadeAbove = false;
			auto emitDraws = [&](MeshAsset* mesh, u64 vertexAddress, const AABB& bounds, const Mat4& world, u32 renderingLayerMask,
								 ShadowCasting castShadows, bool receiveShadows, Handle<SceneObject> object, u64 renderer,
								 auto&& materialForSlot) {
				AABB worldBounds = bounds.IsValid() ? bounds.Transformed(world) : AABB{};
				u64 volumeMask = 0;
				for (u32 volume = 0; volume < static_cast<u32>(volumes.size()); ++volume) {
					if ((renderingLayerMask & volumes[volume].layerMask) != 0 && (!bounds.IsValid() || volumes[volume].Intersects(worldBounds))) {
						volumeMask |= 1ull << volume;
					}
				}
				if (walk.split && (volumeMask & walk.split->objectBits) != 0) {
					const CasterSplit& split = *walk.split;
					const auto member = split.members->find(renderer);
					const u64 own = member != split.members->end() ? 1ull << (split.firstObject + member->second) : 0ull;
					volumeMask &= (volumeMask & own) != 0 ? ~(split.cascadeBits | split.objectBits) | own : ~split.objectBits;
				}
				if (volumeMask == 0) {
					return;
				}
				f32 depth = Vec3::DistanceSquared(walk.sortOrigin, worldBounds.Center());
				const u32 depthBand = walk.use == DrawListUse::View ? DepthBand(depth) : 0u;
				const bool mirrored = world.ToMat3().Determinant() < 0.0f;

				for (const SubMesh& submesh : mesh->submeshes) {
					DrawEntry* entry = arena.Construct<DrawEntry>();
					if (!materials.Resolve(materialForSlot(submesh.materialSlot), *entry)) {
						continue;
					}
					entry->mesh = mesh;
					entry->submesh = &submesh;
					entry->vertexAddress = vertexAddress;
					entry->world = world;
					entry->mirrored = mirrored;
					entry->depth = depth;
					entry->volumeMask = volumeMask;
					entry->renderingLayerMask = renderingLayerMask;
					entry->castShadows = castShadows;
					entry->receiveShadows = receiveShadows;
					entry->object = object;
					entry->lodFade = lodFade;
					entry->lodFadeAbove = lodFadeAbove;
					lane.orders.push_back(OrderOf(*entry, depthBand, laneBits | static_cast<u32>(lane.entries.size())));
					lane.entries.push_back(entry);
				}
			};

			auto passesSceneFilter = [&](const SceneObject& owner) {
				if (walk.onlyScenes.empty()) {
					return true;
				}
				u64 sceneId = owner.GetScene().GetInstanceId();
				return std::find(walk.onlyScenes.begin(), walk.onlyScenes.end(), sceneId) != walk.onlyScenes.end();
			};

			const u32 meshCount = static_cast<u32>(walk.meshRenderers->size());
			for (u32 index = first; index < Math::Min(end, meshCount); ++index) {
				MeshRenderer* renderer = (*walk.meshRenderers)[index].Get();
				if (!renderer) {
					continue;
				}
				MeshAsset* mesh = renderer->GetMesh().Get();
				if (!mesh || mesh->vertexBufferAddress == 0 || !mesh->indexBuffer.IsValid()) {
					continue;
				}
				SceneObject* owner = renderer->GetOwner().Get();
				if (!owner || !passesSceneFilter(*owner)) {
					continue;
				}
				if ((renderer->GetRenderingLayerMask() & walk.renderingLayerMask) == 0 || !DrawsFor(renderer->GetCastShadows(), walk.use)) {
					continue;
				}
				if (walk.lodMembers && !LODDraws(walk, renderer->GetInstanceId(), lodFade, lodFadeAbove)) {
					continue;
				}
				emitDraws(mesh, mesh->vertexBufferAddress, mesh->bounds, owner->GetWorldMatrix(), renderer->GetRenderingLayerMask(),
						  renderer->GetCastShadows(), renderer->GetReceiveShadows(), renderer->GetOwner(), renderer->GetInstanceId(),
						  [&](u32 slot) { return walk.overrideMaterial ? walk.overrideMaterial : renderer->GetRenderedMaterial(slot).Get(); });
			}

			const Vector<RenderState::SkinnedOutput>& skinnedOutputs = *walk.skinnedOutputs;
			for (u32 index = Math::Max(first, meshCount); index < end; ++index) {
				const u32 i = index - meshCount;
				SkinnedMeshRenderer* renderer = (*walk.skinnedRenderers)[i].Get();
				if (!renderer) {
					continue;
				}
				MeshAsset* mesh = renderer->GetMesh().Get();
				if (!mesh || mesh->vertexBufferAddress == 0 || !mesh->indexBuffer.IsValid()) {
					continue;
				}
				SceneObject* owner = renderer->GetOwner().Get();
				if (!owner || !passesSceneFilter(*owner)) {
					continue;
				}
				if ((renderer->GetRenderingLayerMask() & walk.renderingLayerMask) == 0 || !DrawsFor(renderer->GetCastShadows(), walk.use)) {
					continue;
				}
				if (walk.lodMembers && !LODDraws(walk, renderer->GetInstanceId(), lodFade, lodFadeAbove)) {
					continue;
				}

				Skeleton* skeleton = renderer->GetSkeleton().Get();
				SceneObject* skeletonOwner = skeleton ? skeleton->GetOwner().Get() : nullptr;
				const Mat4 world = skeletonOwner ? skeletonOwner->GetWorldMatrix() : owner->GetWorldMatrix();

				const RenderState::SkinnedOutput* skinned = i < skinnedOutputs.size() && skinnedOutputs[i].vertexAddress ? &skinnedOutputs[i] : nullptr;
				emitDraws(mesh, skinned ? skinned->vertexAddress : mesh->vertexBufferAddress, skinned ? skinned->bounds : mesh->bounds, world,
						  renderer->GetRenderingLayerMask(), renderer->GetCastShadows(), renderer->GetReceiveShadows(), renderer->GetOwner(),
						  renderer->GetInstanceId(),
						  [&](u32 slot) { return walk.overrideMaterial ? walk.overrideMaterial : renderer->GetRenderedMaterial(slot).Get(); });
			}
			std::sort(lane.orders.begin(), lane.orders.end(), DrawOrderLess{});
		}

		void ApplyLaneDeferrals(RenderState& state, Vector<LaneDeferral>& deferred) {
			for (const LaneDeferral& entry : deferred) {
				switch (entry.kind) {
				case LaneDeferral::Kind::Sampler: {
					const RHI::SamplerHandle sampler = TextureSampler(state, *entry.texture);
					entry.out->sampler = sampler.IsValid() ? sampler : state.repeatSampler;
					break;
				}
				case LaneDeferral::Kind::Fallback:
					entry.out->texture = FallbackTexture(state, entry.dimension, entry.fallback);
					break;
				case LaneDeferral::Kind::Mismatch:
					WarnDimensionMismatch(state, entry.key, *entry.material, *entry.slot, *entry.texture);
					break;
				case LaneDeferral::Kind::Refusal:
					NameMeshDrawRefusal(*entry.shader, *entry.material, entry.refusal);
					break;
				}
			}
			deferred.clear();
		}

		void PackPerDraw(GPUPerDraw& out, const DrawEntry& entry, bool transparentReceiveShadows) {
			GPUPerDraw element;
			element.world = entry.world;
			Mat3 rotationScale = entry.world.ToMat3();
			Mat3 normalMatrix;
			if (!rotationScale.TryInvert(normalMatrix)) {
				normalMatrix = Mat3::Identity();
			}
			normalMatrix = normalMatrix.Transposed();
			element.normalMatrix0 = Vec4(normalMatrix.GetColumn(0), 0.0f);
			element.normalMatrix1 = Vec4(normalMatrix.GetColumn(1), 0.0f);
			element.normalMatrix2 = Vec4(normalMatrix.GetColumn(2), 0.0f);
			element.vertexAddress = entry.vertexAddress;
			element.alphaClipThreshold = entry.alphaClipThreshold;
			if (entry.receiveShadows && (!entry.alpha || transparentReceiveShadows)) {
				element.flags |= DRAW_RECEIVES_SHADOWS;
			}
			element.flags |= static_cast<u32>(entry.renderState.blend) << DRAW_BLEND_SHIFT;
			element.flags |= (entry.renderingLayerMask & LIGHT_LAYERS) << DRAW_LAYERS_SHIFT;
			element.flags |= static_cast<u32>(entry.lodFade) << DRAW_FADE_SHIFT;
			if (entry.lodFadeAbove) {
				element.flags |= DRAW_FADE_ABOVE;
			}
			out = element;
		}

		u64 LODSignature(const LODGroup& group) {
			u64 hash = 0xcbf29ce484222325ull;
			auto mix = [&hash](u64 value) {
				hash = (hash ^ value) * 0x100000001b3ull;
				hash ^= hash >> 31;
			};
			const u32 levels = group.GetLevelCount();
			mix(levels);
			for (u32 level = 0; level < levels; ++level) {
				const Vector<Handle<SceneObject>>& objects = group.GetLevel(level).objects;
				mix(objects.size());
				for (const Handle<SceneObject>& handle : objects) {
					const SceneObject* object = handle.Get();
					mix(handle.GetInstanceId());
					mix(object ? object->GetComponentCount() : ~0ull);
				}
			}
			return hash;
		}

		void ForgetLODRecord(RenderState& state, u32 index) {
			RenderState::LODRecord& record = state.lodRecords[index];
			for (u64 id : record.renderers) {
				const auto found = state.lodMembers.find(id);
				if (found != state.lodMembers.end() && found->second.group == index) {
					state.lodProceduralMembers -= found->second.procedural ? 1u : 0u;
					state.lodMembers.erase(found);
				}
			}
			record.renderers.clear();
		}

		void MeasureLODRecord(RenderState& state, u32 index) {
			ForgetLODRecord(state, index);
			RenderState::LODRecord& record = state.lodRecords[index];
			record.dirty = false;
			record.localSize = 0.0f;
			const LODGroup* group = record.group.Get();
			const SceneObject* owner = group ? group->GetOwner().Get() : nullptr;
			record.signature = group ? LODSignature(*group) : 0;
			if (!owner) {
				return;
			}
			const Mat4 toLocal = owner->GetWorldMatrix().Inverse();
			AABB bounds = AABB::Empty();
			Vector<Handle<ProceduralRenderer>> procedural{GetMemoryResource(MemTag::Graphics)};
			const u32 levels = Math::Min(group->GetLevelCount(), LODChoice::MAX_LEVELS);
			for (u32 level = 0; level < levels; ++level) {
				for (const Handle<SceneObject>& objectHandle : group->GetLevel(level).objects) {
					const SceneObject* object = objectHandle.Get();
					if (!object) {
						continue;
					}
					auto join = [&](const Component* renderer, const MeshAsset* mesh, bool isProcedural = false) {
						if (!renderer) {
							return;
						}
						RenderState::LODMember& member = state.lodMembers[renderer->GetInstanceId()];
						if (member.group != index || member.levels == 0) {
							state.lodProceduralMembers += isProcedural && member.levels == 0 ? 1u : 0u;
							member = {index, 0, isProcedural};
							record.renderers.push_back(renderer->GetInstanceId());
						}
						member.levels |= 1ull << level;
						if (mesh && mesh->bounds.IsValid() && renderer->IsEffectivelyEnabled()) {
							bounds.Encapsulate(mesh->bounds.Transformed(toLocal * object->GetWorldMatrix()));
						}
					};
					const MeshRenderer* renderer = object->GetComponent<MeshRenderer>().Get();
					join(renderer, renderer ? renderer->GetMesh().Get() : nullptr);
					const SkinnedMeshRenderer* skinned = object->GetComponent<SkinnedMeshRenderer>().Get();
					join(skinned, skinned ? skinned->GetMesh().Get() : nullptr);
					procedural.clear();
					object->GetComponents(procedural);
					for (const Handle<ProceduralRenderer>& item : procedural) {
						join(item.Get(), nullptr, true);
					}
				}
			}
			if (bounds.IsValid()) {
				const Vec3 extent = bounds.max - bounds.min;
				record.localCenter = bounds.Center();
				record.localSize = Math::Max(extent.x, Math::Max(extent.y, extent.z));
			}
		}

		void MarkLODRecordDirty(RenderState& state, u64 renderer) {
			const auto member = state.lodMembers.find(renderer);
			if (member != state.lodMembers.end() && member->second.group < state.lodRecords.size()) {
				state.lodRecords[member->second.group].dirty = true;
			}
		}

		void BuildLODFrame(RenderState& state) {
			++state.lodFrameIndex;
			if ((state.lodFrameIndex & 63) == 0) {
				std::erase_if(state.lodFades, [&](const auto& entry) { return entry.second.seen + 120 < state.lodFrameIndex; });
			}
			const u32 count = static_cast<u32>(state.lodRecords.size());
			state.lodFrame.resize(count);
			for (u32 i = 0; i < count; ++i) {
				RenderState::LODRecord& record = state.lodRecords[i];
				RenderState::LODFrameGroup& frame = state.lodFrame[i];
				frame = {};
				const LODGroup* group = record.group.Get();
				if (!group && record.group.GetInstanceId() != 0) {
					ForgetLODRecord(state, i);
					record.group = {};
				}
				if (group && (record.dirty || LODSignature(*group) != record.signature)) {
					MeasureLODRecord(state, i);
				}
				const SceneObject* owner = group ? group->GetOwner().Get() : nullptr;
				if (!owner || group->GetLevelCount() == 0) {
					continue;
				}
				frame.group = group;
				frame.id = group->GetInstanceId();
				const Mat4& world = owner->GetWorldMatrix();
				if (record.localSize > 0.0f) {
					const f32 scale = Math::Max(Vec3(world(0, 0), world(1, 0), world(2, 0)).Length(),
												Math::Max(Vec3(world(0, 1), world(1, 1), world(2, 1)).Length(),
														  Vec3(world(0, 2), world(1, 2), world(2, 2)).Length()));
					frame.center = world.TransformPoint(record.localCenter);
					frame.size = record.localSize * scale;
				} else {
					frame.center = owner->GetWorldPosition();
				}
			}
		}

		LODChoice FloorLOD(LODChoice choice, u32 levels, u32 finest) {
			if (finest == 0 || levels == 0) {
				return choice;
			}
			const u8 floor = static_cast<u8>(Math::Min(finest, levels - 1));
			auto lift = [floor](u8 level) { return level != LODChoice::NONE && level < floor ? floor : level; };
			choice.level = lift(choice.level);
			choice.next = lift(choice.next);
			if (choice.level == choice.next) {
				choice.next = LODChoice::NONE;
				choice.keep = 255;
			}
			return choice;
		}

		struct LODViewpoint {
			Vec3 origin{};
			bool perspective = true;
			f32 scale = 1.0f;
			u64 view = 0;
			f64 now = 0.0;
		};

		LODViewpoint LODViewpointOf(const RenderState& state, const Vec3& origin, const Mat4& projection, u64 view, bool hasTime, f64 time) {
			LODViewpoint from;
			from.origin = origin;
			from.perspective = projection(3, 2) != 0.0f;
			from.scale = std::abs(projection(1, 1)) * 0.5f * state.lodBias;
			from.view = view;
			from.now = hasTime ? time : Time::GetUnscaledTime();
			return from;
		}

		LODChoice ChooseLOD(RenderState& state, u32 index, const LODViewpoint& from) {
			const RenderState::LODFrameGroup& frame = state.lodFrame[index];
			if (!frame.group) {
				return {};
			}
			const f32 reach = from.perspective ? Math::Max(Vec3::Distance(from.origin, frame.center), 1e-4f) : 1.0f;
			const f32 height = frame.size * from.scale / reach;
			LODChoice choice;
			if (from.view != 0 && state.lodCrossFade && frame.group->GetFadeMode() == LODFadeMode::AnimatedCrossFade) {
				u64 key = (from.view ^ (frame.id * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull;
				key ^= key >> 31;
				RenderState::LODFadeSlot& slot = state.lodFades[key];
				if (slot.seen != state.lodFrameIndex) {
					slot.seen = state.lodFrameIndex;
					slot.choice = frame.group->Choose(height, slot.fade, from.now);
				}
				choice = slot.choice;
			} else {
				choice = frame.group->Choose(height);
				if (!state.lodCrossFade) {
					choice.next = LODChoice::NONE;
					choice.keep = 255;
				}
			}
			return FloorLOD(choice, frame.group->GetLevelCount(), state.lodFinestLevel);
		}

		std::span<const LODChoice> ChooseLODs(RenderState& state, LinearAllocator& arena, const Vec3& origin, const Mat4& projection) {
			const u32 count = static_cast<u32>(state.lodFrame.size());
			if (count == 0) {
				return {};
			}
			LODChoice* choices = arena.AllocateAs<LODChoice>(count);
			const LODViewpoint from = LODViewpointOf(state, origin, projection, state.lodView, state.hasViewTime, state.viewTime);
			for (u32 i = 0; i < count; ++i) {
				choices[i] = ChooseLOD(state, i, from);
			}
			return {choices, count};
		}

		bool ProceduralLODDraws(RenderState& state, const LODViewpoint& from, u64 renderer) {
			const auto found = state.lodMembers.find(renderer);
			if (found == state.lodMembers.end() || found->second.group >= state.lodFrame.size()) {
				return true;
			}
			u8 fade = 0;
			bool above = false;
			return LODDraws(ChooseLOD(state, found->second.group, from), found->second.levels, fade, above);
		}

		ExtractedDraws ExtractDrawList(LinearAllocator& arena, DrawListUse use, SceneKind kind, std::span<const u64> onlyScenes,
									   std::span<const GraphicsInternal::CullVolume> volumes, const Vec3& sortOrigin, const Mat4& lodProjection,
									   u32 renderingLayerMask = ~0u, MaterialAsset* overrideMaterial = nullptr,
									   const CasterSplit* split = nullptr) {
			SLT_CORE_ASSERT_MSG(!volumes.empty() && volumes.size() <= GraphicsInternal::MAX_SHADOW_MAPS,
								"a draw's volume mask is 1 bit per shadow map");
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			const bool isolated = kind != SceneKind::World;

			if (!state.errorMaterial) {
				state.errorMaterial = New<MaterialAsset>(MemTag::Graphics);
				state.errorMaterial->SetColor("baseColor", Color::Magenta());
			}
			DrawWalk walk;
			walk.use = use;
			walk.onlyScenes = onlyScenes;
			walk.volumes = volumes;
			walk.sortOrigin = sortOrigin;
			walk.renderingLayerMask = renderingLayerMask;
			walk.overrideMaterial = overrideMaterial;
			walk.split = split;
			walk.meshRenderers = isolated ? &state.isolatedMeshRenderers : &state.meshRenderers;
			walk.skinnedRenderers = isolated ? &state.isolatedSkinnedMeshRenderers : &state.skinnedMeshRenderers;
			walk.skinnedOutputs = isolated ? &state.isolatedSkinnedOutputs : &state.skinnedOutputs;
			walk.errorShader = state.standardShader.Get();
			walk.errorMaterial = state.errorMaterial;

			arena.Reset();
			walk.lodChoices = ChooseLODs(state, arena, sortOrigin, lodProjection);
			walk.lodMembers = walk.lodChoices.empty() || state.lodMembers.empty() ? nullptr : &state.lodMembers;
			const u32 rendererCount = static_cast<u32>(walk.meshRenderers->size() + walk.skinnedRenderers->size());
			const u32 lanes = JobSystem::LaneCount(rendererCount, RENDERERS_PER_LANE);
			Vector<ExtractionLane*>& laneStates = state.extractionLanes;
			while (laneStates.size() < lanes) {
				ExtractionLane* lane = New<ExtractionLane>(MemTag::Graphics);
				if (!laneStates.empty()) {
					lane->arena = New<LinearAllocator>(MemTag::Graphics, LANE_ARENA_BYTES, MemTag::Graphics);
					lane->ownsArena = true;
				}
				laneStates.push_back(lane);
			}
			laneStates[0]->arena = &arena;
			state.frameExtractionLanes = Math::Max(state.frameExtractionLanes, lanes);

			JobSystem::ParallelFor(lanes, rendererCount,
								   [&](u32 lane, u32 first, u32 end) { WalkRenderers(walk, *laneStates[lane], lane, first, end); });
			u32 drawCount = 0;
			for (u32 lane = 0; lane < lanes; ++lane) {
				ApplyLaneDeferrals(state, laneStates[lane]->deferred);
				drawCount += static_cast<u32>(laneStates[lane]->orders.size());
			}

			ExtractedDraws extracted;
			extracted.list = arena.AllocateAs<DrawEntry*>(drawCount ? drawCount : 1);
			if (drawCount == 0) {
				return extracted;
			}
			const RHI::TransientAllocation allocation = device.AllocateTransient(static_cast<u64>(drawCount) * sizeof(GPUPerDraw), 16);
			if (!allocation.IsValid()) {
				return extracted;
			}
			GPUPerDraw* perDraw = static_cast<GPUPerDraw*>(allocation.cpu);
			const bool transparentReceiveShadows = state.transparentReceiveShadows;
			auto writeDraws = [&](const DrawOrder* orders, u32 count, u32 offset) {
				for (u32 i = 0; i < count; ++i) {
					const u32 entry = orders[i].entry;
					DrawEntry* drawn = laneStates[entry >> LANE_ENTRY_BITS]->entries[entry & ((1u << LANE_ENTRY_BITS) - 1)];
					extracted.list[offset + i] = drawn;
					PackPerDraw(perDraw[offset + i], *drawn, transparentReceiveShadows);
				}
			};

			if (lanes == 1) {
				writeDraws(laneStates[0]->orders.data(), drawCount, 0);
			} else {
				constexpr u32 SAMPLES_PER_LANE = 8;
				DrawOrder* samples = arena.AllocateAs<DrawOrder>(lanes * SAMPLES_PER_LANE);
				u32 sampleCount = 0;
				for (u32 lane = 0; lane < lanes; ++lane) {
					const Vector<DrawOrder>& orders = laneStates[lane]->orders;
					const u64 size = orders.size();
					const u32 taken = static_cast<u32>(Math::Min<u64>(size, SAMPLES_PER_LANE));
					for (u32 sample = 0; sample < taken; ++sample) {
						samples[sampleCount++] = orders[(2 * sample + 1) * size / (2 * taken)];
					}
				}
				std::sort(samples, samples + sampleCount, DrawOrderLess{});
				DrawOrder* splitters = arena.AllocateAs<DrawOrder>(lanes - 1);
				for (u32 bucket = 1; bucket < lanes; ++bucket) {
					splitters[bucket - 1] = samples[static_cast<u64>(bucket) * sampleCount / lanes];
				}

				JobSystem::ParallelFor(lanes, lanes, [&](u32 bucket, u32, u32) {
					Vector<DrawOrder>& merged = laneStates[bucket]->bucket;
					merged.clear();
					u32 offset = 0;
					for (u32 source = 0; source < lanes; ++source) {
						const Vector<DrawOrder>& orders = laneStates[source]->orders;
						const DrawOrder* begin = orders.data();
						const DrawOrder* finish = begin + orders.size();
						const DrawOrder* low = bucket == 0 ? begin : std::lower_bound(begin, finish, splitters[bucket - 1], DrawOrderLess{});
						const DrawOrder* high = bucket + 1 == lanes ? finish : std::lower_bound(begin, finish, splitters[bucket], DrawOrderLess{});
						offset += static_cast<u32>(low - begin);
						merged.insert(merged.end(), low, high);
					}
					std::sort(merged.begin(), merged.end(), DrawOrderLess{});
					writeDraws(merged.data(), static_cast<u32>(merged.size()), offset);
				});
			}

			extracted.count = drawCount;
			extracted.perDrawAddress = device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
			return extracted;
		}

		struct ProceduralDepthView {
			ProceduralPass pass = ProceduralPass::Prepass;
			Mat4 view = Mat4::Identity();
			Mat4 projection = Mat4::Identity();
			Vec2I extent{1, 1};
			RHI::Format colorFormat = RHI::Format::Unknown;
			RHI::Format depthFormat = RHI::Format::Unknown;
			RHI::TransientAllocation viewBlock{};
			Vec2 casterBias{};
			u32 samples = 1;
		};

		u32 RecordProceduralDepth(RHI::CommandList& cmd, RHI::Device& device, std::span<const u32> renderers,
								  const ProceduralDepthView& depthView, const GraphicsInternal::CullVolume* volume,
								  std::span<const u32> values = {}) {
			RenderState& state = *_state;
			const GraphicsInternal::ProceduralFrame& frame = state.procedural;
			const Mat4 inverseView = depthView.view.Inverse();
			const Vec3 eye(inverseView(0, 3), inverseView(1, 3), inverseView(2, 3));
			u32 drawCalls = 0;
			for (u32 at = 0; at < static_cast<u32>(renderers.size()); ++at) {
				const u32 index = renderers[at];
				const u32 value = at < values.size() ? values[at] : 0u;
				if (depthView.pass == ProceduralPass::Value && value == 0) {
					continue;
				}
				const GraphicsInternal::ProceduralFrame::Renderer& declared = frame.renderers[index];
				if (volume && ((declared.renderingLayerMask & volume->layerMask) == 0 || (declared.bounded && !volume->Intersects(declared.bounds)))) {
					continue;
				}
				ProceduralRenderer* renderer = declared.renderer.Get();
				if (!renderer) {
					continue;
				}
				for (u32 item = declared.firstItem; item < declared.firstItem + declared.itemCount; ++item) {
					MaterialAsset* material = frame.items[item].material.Get();
					ShaderAsset* shader = material ? material->shader.Get() : nullptr;
					if (!shader || !DrawsProceduralItem(*shader, *material)) {
						continue;
					}
					const ShadowCasting casting = frame.items[item].castShadows;
					if (depthView.pass == ProceduralPass::Shadow) {
						if (casting == ShadowCasting::Off) {
							continue;
						}
					} else if (casting == ShadowCasting::ShadowsOnly) {
						continue;
					} else if (depthView.pass != ProceduralPass::Value &&
							   material->ResolveRenderState(shader->renderState).blend != RHI::BlendMode::Opaque) {
						continue;
					}
					const ShaderVariant* code = shader->GetVariant(shader->FindVariant(material->keywordHash));
					if (!code || !code->HasProgram(ShaderProgramKind::Depth)) {
						if (depthView.pass == ProceduralPass::Shadow) {
							NameProceduralCasterRefusal(*shader, *material);
						}
						continue;
					}
					GraphicsInternal::ProceduralContextInit init;
					init.cmd = &cmd;
					init.device = &device;
					init.frame = &frame;
					init.item = item;
					init.pass = depthView.pass;
					init.colorFormat = depthView.colorFormat;
					init.depthFormat = depthView.depthFormat;
					init.samples = depthView.samples;
					init.extent = depthView.extent;
					init.view = depthView.view;
					init.projection = depthView.projection;
					init.cameraPosition = eye;
					init.viewBlock = depthView.viewBlock;
					init.drawCalls = &drawCalls;
					init.value = value;
					init.casterBias = depthView.casterBias;
					ProceduralContext context(init);
					renderer->Record(context);
				}
			}
			return drawCalls;
		}

		f32 CameraNearDepth(const Mat4& projection) {
			constexpr f32 FLOOR = 0.1f;
			if (Math::Abs(projection(3, 2)) < 1e-6f) {
				return FLOOR;
			}
			const f32 denominator = 1.0f - projection(2, 2);
			if (Math::Abs(denominator) < 1e-9f) {
				return FLOOR;
			}
			return Math::Max(projection(2, 3) / denominator, 1e-3f);
		}

		u64 ShadowOwnerToken(const PassWork& work) {
			u64 token = work.key * 1099511628211ull;
			token ^= static_cast<u64>(work.index) << 32;
			token ^= static_cast<u64>(work.subIndex) << 8;
			return token | 1ull;
		}

		u64 SkyGradientKey(const GraphicsInternal::SkyGradient& gradient) {
			const f32 values[10] = {gradient.sky.x, gradient.sky.y, gradient.sky.z,
									gradient.equator.x, gradient.equator.y, gradient.equator.z,
									gradient.ground.x, gradient.ground.y, gradient.ground.z,
									gradient.flat ? 1.0f : 0.0f};
			u64 hash = 0xcbf29ce484222325ull;
			const u8* bytes = reinterpret_cast<const u8*>(values);
			for (size_t i = 0; i < sizeof(values); ++i) {
				hash = (hash ^ bytes[i]) * 0x100000001b3ull;
			}
			return hash | 1ull;
		}

		bool EnsureBakedSky(Handle<Environment> environment, const GraphicsInternal::SkyGradient& gradient,
							RenderState::BakedSky*& outBaked) {
			RenderState& state = *_state;
			RenderState::BakedSky* slot = nullptr;
			for (RenderState::BakedSky& candidate : state.bakedSkies) {
				if (candidate.environment == environment) {
					slot = &candidate;
					break;
				}
			}
			if (!slot) {
				state.bakedSkies.push_back(RenderState::BakedSky{});
				slot = &state.bakedSkies.back();
				slot->environment = environment;
			}
			RenderState::BakedSky& baked = *slot;
			outBaked = &baked;
			const u64 key = SkyGradientKey(gradient);
			if (baked.key == key && baked.cube.IsValid()) {
				return true;
			}

			GraphicsInternal::RadianceCube cube;
			GraphicsInternal::BuildCubeFromGradient(gradient, PROCEDURAL_SKY_FACE_SIZE, cube);
			GraphicsInternal::IrradianceSH sh;
			GraphicsInternal::PrefilterRadianceCube(cube, sh);
			if (!cube.IsValid()) {
				return false;
			}

			if (!baked.cube.IsValid()) {
				RHI::TextureDesc desc;
				desc.size = {static_cast<i32>(cube.faceSize), static_cast<i32>(cube.faceSize)};
				desc.format = RHI::Format::RGBA16Float;
				desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
				desc.mipLevels = cube.mipCount;
				desc.arrayLayers = GraphicsInternal::CUBE_FACE_COUNT;
				desc.cube = true;
				desc.debugName = "ProceduralSky";
				baked.cube = state.device->CreateTextureResource(desc);
				if (!baked.cube.IsValid()) {
					return false;
				}
			}
			for (u32 mip = 0; mip < cube.mipCount; ++mip) {
				const u32 side = Math::Max(cube.faceSize >> mip, 1u);
				const u64 faceBytes = static_cast<u64>(side) * side * 8;
				for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					const u64 offset = GraphicsInternal::RadianceCubeFaceOffset(cube.faceSize, cube.mipCount, mip, face);
					state.device->UploadTexture(baked.cube, {cube.pixels.data() + offset, static_cast<size_t>(faceBytes)}, mip, face);
				}
			}
			baked.mipCount = cube.mipCount;
			for (u32 i = 0; i < 9; ++i) {
				baked.sh[i] = sh.coefficients[i];
			}
			baked.key = key;
			return true;
		}

		constexpr u32 MIN_SKY_CAPTURE_FACE = 16;
		constexpr u32 MAX_SKY_CAPTURE_FACE = 1024;

		u32 SkyCaptureFaceSize(i32 requested) {
			const u32 clamped = static_cast<u32>(Math::Clamp(requested, static_cast<i32>(MIN_SKY_CAPTURE_FACE), static_cast<i32>(MAX_SKY_CAPTURE_FACE)));
			u32 face = MIN_SKY_CAPTURE_FACE;
			while (face * 2 <= clamped) {
				face *= 2;
			}
			return face;
		}

		RenderState::SkyCapture* FindSkyCapture(RenderState& state, Handle<Environment> environment) {
			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				if (capture.environment == environment) {
					return &capture;
				}
			}
			return nullptr;
		}

		void ReleaseSkyCapture(RHI::Device& device, RenderState::SkyCapture& capture) {
			if (capture.source.IsValid()) {
				device.DestroyResource(capture.source);
			}
			if (capture.chain.IsValid()) {
				device.DestroyResource(capture.chain);
			}
			if (capture.readback.IsValid()) {
				device.DestroyResource(capture.readback);
			}
			capture.source = {};
			capture.chain = {};
			capture.readback = {};
			capture.filled = false;
			capture.haveSH = false;
			for (u32 slot = 0; slot < RHI::FRAMES_IN_FLIGHT; ++slot) {
				capture.slotPending[slot] = false;
			}
		}

		constexpr u64 CAPTURE_PATIENCE_FRAMES = 30;

		void ReleaseCaptureResources(RenderState& state, RenderState::FrameCapture& capture) {
			if (capture.readback.IsValid()) {
				state.device->DestroyResource(capture.readback);
				capture.readback = {};
			}
			if (capture.target != INVALID_VIEW_TARGET) {
				RenderSystem::DestroyViewTarget(capture.target);
				capture.target = INVALID_VIEW_TARGET;
			}
		}

		void AnswerFrameCaptures(RenderState& state, RHI::Device& device) {
			if (state.frameCaptures.empty()) {
				return;
			}
			Vector<RenderState::FrameCapture> captures{GetMemoryResource(MemTag::Graphics)};
			captures.swap(state.frameCaptures);
			for (RenderState::FrameCapture& capture : captures) {
				if (capture.followUp) {
					if (!capture.followUp()) {
						state.frameCaptures.push_back(std::move(capture));
					}
					continue;
				}
				const bool ready = capture.copied && capture.copiedFrame + RHI::FRAMES_IN_FLIGHT <= state.renderedFrames;
				const bool stale = !capture.copied && (capture.failed || capture.requestedFrame + CAPTURE_PATIENCE_FRAMES < state.renderedFrames);
				if (!ready && !stale) {
					state.frameCaptures.push_back(std::move(capture));
					continue;
				}
				Vector<u8> rgba8{GetMemoryResource(MemTag::Graphics)};
				bool captured = false;
				if (ready && !device.IsLost()) {
					if (const u8* mapped = static_cast<const u8*>(device.GetMappedPointer(capture.readback))) {
						const size_t bytes = static_cast<size_t>(capture.size.x) * static_cast<size_t>(capture.size.y) * 4;
						captured = GraphicsInternal::ToRGBA8({mapped, bytes}, capture.format, capture.size, rgba8);
					}
				}
				ReleaseCaptureResources(state, capture);
				if (capture.deliver) {
					capture.followUp = capture.deliver(captured, std::move(rgba8), capture.size);
					capture.deliver = {};
				}
				if (capture.followUp) {
					state.frameCaptures.push_back(std::move(capture));
				}
			}
		}

		void AnswerSkyReadbacks(RenderState& state, RHI::Device& device) {
			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				if (!capture.readback.IsValid()) {
					continue;
				}
				u32 newest = RHI::FRAMES_IN_FLIGHT;
				for (u32 slot = 0; slot < RHI::FRAMES_IN_FLIGHT; ++slot) {
					if (capture.slotPending[slot] && capture.slotFrame[slot] + RHI::FRAMES_IN_FLIGHT <= state.renderedFrames) {
						if (newest == RHI::FRAMES_IN_FLIGHT || capture.slotFrame[slot] > capture.slotFrame[newest]) {
							newest = slot;
						}
						capture.slotPending[slot] = false;
					}
				}
				if (newest == RHI::FRAMES_IN_FLIGHT) {
					continue;
				}
				const u8* mapped = static_cast<const u8*>(device.GetMappedPointer(capture.readback));
				if (!mapped) {
					continue;
				}
				const u64 slotBytes = static_cast<u64>(capture.shSide) * capture.shSide * 8 * GraphicsInternal::CUBE_FACE_COUNT;
				GraphicsInternal::IrradianceSH sh;
				GraphicsInternal::ProjectIrradianceFromFaces({mapped + slotBytes * newest, static_cast<size_t>(slotBytes)}, capture.shSide, sh);
				const f32 stored = capture.slotExposure[newest] > 0.0f ? capture.slotExposure[newest] : 1.0f;
				for (u32 i = 0; i < 9; ++i) {
					capture.sh[i] = sh.coefficients[i] * (1.0f / stored);
				}
				capture.haveSH = true;
			}
		}

		bool FirstSkySourceWarning(Handle<Environment> environment, u64 source) {
			RenderState& state = *_state;
			auto warned = std::find_if(state.warnedSkySources.begin(), state.warnedSkySources.end(),
									   [&](const RenderState::WarnedSkySource& entry) { return entry.environment == environment; });
			if (warned == state.warnedSkySources.end()) {
				state.warnedSkySources.push_back({environment, source});
				return true;
			}
			if (warned->source != source) {
				warned->source = source;
				return true;
			}
			return false;
		}

		const Vector<Handle<Environment>>& EnvironmentList(const RenderState& state, SceneKind kind) {
			return kind == SceneKind::World ? state.environments : state.isolatedEnvironments;
		}

		bool InViewPartition(const Environment& environment, SceneKind kind, std::span<const u64> onlyScenes) {
			SceneObject* owner = environment.GetOwner().Get();
			if (!owner) {
				return false;
			}
			if (kind == SceneKind::World || onlyScenes.empty()) {
				return true;
			}
			const u64 sceneId = owner->GetScene().GetInstanceId();
			return std::find(onlyScenes.begin(), onlyScenes.end(), sceneId) != onlyScenes.end();
		}

		RenderState::EnvironmentSetup NeutralEnvironment() {
			RenderState& state = *_state;
			RenderState::EnvironmentSetup setup;
			setup.cube = state.defaultEnvironmentCube;
			setup.sh[0] = DefaultAmbient * (1.0f / 0.282095f);
			return setup;
		}

		RenderState::EnvironmentSetup ResolveOneEnvironment(Environment* chosen, const Vec3& sunColor) {
			RenderState& state = *_state;
			RenderState::EnvironmentSetup setup = NeutralEnvironment();
			const Color tint = chosen->GetSunTint();
			setup.sunTint = Vec3{tint.r, tint.g, tint.b} * Math::Max(chosen->GetSunIntensityMultiplier(), 0.0f);

			setup.skyIntensity = Math::Max(chosen->GetSkyIntensity(), 0.0f);
			setup.reflectionIntensity = Math::Clamp(chosen->GetReflectionIntensity(), 0.0f, 1.0f);

			const SkyMode skyMode = chosen->GetSkyMode();
			bool haveSky = false;
			bool skyIrradiance = true;
			const f32 discDeg = chosen->GetSunDiscSizeDeg();
			if (skyMode == SkyMode::None) {
				setup.mipCount = 0;
			} else if (skyMode == SkyMode::Cubemap) {
				const Handle<TextureAsset> cubemapHandle = chosen->GetSkyCubemap();
				TextureAsset* cubemap = cubemapHandle.Get();
				if (cubemap && cubemap->isCube && cubemap->texture.IsValid()) {
					setup.cube = cubemap->texture;
					setup.mipCount = Math::Max(cubemap->storedMipLevels, 1u);
					if (cubemap->irradianceSH.size() == 9) {
						for (u32 i = 0; i < 9; ++i) {
							setup.sh[i] = cubemap->irradianceSH[i];
						}
					}
					haveSky = true;
				} else if (cubemap && FirstSkySourceWarning(Handle<Environment>(chosen), cubemap->GetInstanceId())) {
					SLT_LOG_WARN("Graphics", "Environment sky texture isn't a loaded cubemap (no sky drawn)");
				}
			} else if (skyMode == SkyMode::Material) {
				const Handle<MaterialAsset> materialHandle = chosen->GetSkyMaterial();
				MaterialAsset* material = materialHandle.Get();
				const ShaderAsset* shader = material ? material->shader.Get() : nullptr;
				if (shader && shader->integrator == ShaderTemplate::Sky) {
					setup.skyMaterial = materialHandle;
					setup.drawSky = true;
					setup.sunAngularDiameter = Math::DEG_TO_RAD * Math::Max(discDeg, 0.0f);
					if (discDeg > 0.0f && sunColor.LengthSquared() > 0.0f) {
						setup.sunCosSize = std::cos(Math::DEG_TO_RAD * discDeg * 0.5f);
					}
					const RenderState::SkyCapture* capture = FindSkyCapture(state, Handle<Environment>(chosen));
					if (capture && capture->filled && capture->chain.IsValid()) {
						setup.cube = capture->chain;
						setup.mipCount = capture->chainMips;
						setup.radianceScale = 1.0f / capture->chainExposure;
						if (capture->haveSH) {
							for (u32 i = 0; i < 9; ++i) {
								setup.sh[i] = capture->sh[i];
							}
						} else {
							skyIrradiance = false;
						}
						haveSky = true;
					}
				} else if (material && FirstSkySourceWarning(Handle<Environment>(chosen), material->GetInstanceId())) {
					SLT_LOG_WARN("Graphics", "Environment sky material '{}' isn't a sky. Its shader declares no "
											 "`shalltear_template sky` -> no sky is drawn",
								 material->virtualPath);
				}
				if (!haveSky) {
					setup.mipCount = 0;
				}
			} else {
				GraphicsInternal::SkyGradient gradient;
				const Color sky = chosen->GetSkyColor();
				const Color equator = chosen->GetEquatorColor();
				const Color ground = chosen->GetGroundColor();
				gradient.sky = Vec3{sky.r, sky.g, sky.b};
				gradient.equator = Vec3{equator.r, equator.g, equator.b};
				gradient.ground = Vec3{ground.r, ground.g, ground.b};
				gradient.flat = skyMode == SkyMode::Color;
				RenderState::BakedSky* baked = nullptr;
				if (EnsureBakedSky(Handle<Environment>(chosen), gradient, baked) && baked) {
					setup.cube = baked->cube;
					setup.mipCount = baked->mipCount;
					for (u32 i = 0; i < 9; ++i) {
						setup.sh[i] = baked->sh[i];
					}
					haveSky = true;
				}
				if (haveSky && discDeg > 0.0f && sunColor.LengthSquared() > 0.0f) {
					setup.sunCosSize = std::cos(Math::DEG_TO_RAD * discDeg * 0.5f);
				}
			}
			setup.drawSky = setup.drawSky || haveSky;

			const Color ambientColor = chosen->GetAmbientColor();
			Vec3 ambientRadiance{ambientColor.r, ambientColor.g, ambientColor.b};
			if (chosen->GetAmbientMode() == AmbientMode::Gradient) {
				const Color sky = chosen->GetAmbientSkyColor();
				const Color equator = chosen->GetAmbientEquatorColor();
				const Color ground = chosen->GetAmbientGroundColor();
				GraphicsInternal::SkyGradient gradient;
				gradient.sky = Vec3{sky.r, sky.g, sky.b};
				gradient.equator = Vec3{equator.r, equator.g, equator.b};
				gradient.ground = Vec3{ground.r, ground.g, ground.b};
				GraphicsInternal::IrradianceSH irradiance;
				GraphicsInternal::ProjectGradientIrradiance(gradient, irradiance);
				for (u32 i = 0; i < 9; ++i) {
					setup.sh[i] = irradiance.coefficients[i];
				}
				ambientRadiance = irradiance.coefficients[0] * 0.282095f;
			} else if (chosen->GetAmbientMode() == AmbientMode::Color || !haveSky || !skyIrradiance) {
				setup.sh[0] = ambientRadiance * (1.0f / 0.282095f);
				for (u32 i = 1; i < 9; ++i) {
					setup.sh[i] = Vec3{};
				}
			}
			const f32 diffuseScale = Math::Max(chosen->GetAmbientIntensity(), 0.0f) * (haveSky && skyIrradiance ? setup.skyIntensity : 1.0f);
			for (u32 i = 0; i < 9; ++i) {
				setup.sh[i] = setup.sh[i] * diffuseScale;
			}
			setup.reflectionIntensity *= haveSky ? setup.skyIntensity : 1.0f;
			setup.flatRadiance = ambientRadiance;
			return setup;
		}

		struct EnvironmentLayers {
			static constexpr u32 MAX_LAYERS = GraphicsInternal::MAX_ENVIRONMENT_LAYERS;
			Environment* layers[MAX_LAYERS]{};
			GraphicsInternal::EnvironmentLayerInput inputs[MAX_LAYERS];
			f32 weights[MAX_LAYERS]{};
			u32 order[MAX_LAYERS]{};
			u32 count = 0;
			f32 remainder = 1.0f;
		};

		void GatherEnvironmentLayers(RenderState& state, SceneKind kind, std::span<const u64> onlyScenes, const Vec3& cameraPosition,
									 EnvironmentLayers& out) {
			constexpr u32 MAX_LAYERS = EnvironmentLayers::MAX_LAYERS;
			Environment** layers = out.layers;
			GraphicsInternal::EnvironmentLayerInput* inputs = out.inputs;
			u32 count = 0;
			const Vector<Handle<Environment>>& list = EnvironmentList(state, kind);
			for (u32 registration = 0; registration < static_cast<u32>(list.size()); ++registration) {
				Environment* environment = list[registration].Get();
				if (!environment || !InViewPartition(*environment, kind, onlyScenes)) {
					continue;
				}
				const SceneObject* owner = environment->GetOwner().Get();
				const f32 contribution = VolumeContribution(environment->GetIsGlobal(), owner->GetWorldMatrix(), environment->GetSize(),
															environment->GetBlendDistance(), environment->GetWeight(), cameraPosition);
				if (contribution <= 0.0f) {
					continue;
				}
				if (environment->GetIsGlobal()) {
					for (u32 i = 0; i < count; ++i) {
						if (layers[i]->GetIsGlobal() && inputs[i].priority == environment->GetPriority()) {
							const Handle<Environment> handle(environment);
							if (!ContainsHandle(state.warnedDuplicateEnvironments, handle)) {
								state.warnedDuplicateEnvironments.push_back(handle);
								SLT_LOG_WARN("Graphics", "a 2nd Environment on '{}' is beneath a global one of the same priority "
														 "(changes nothing). Give it another priority to blend",
											 owner->name);
							}
							break;
						}
					}
				}
				GraphicsInternal::EnvironmentLayerInput input;
				input.priority = environment->GetPriority();
				input.contribution = contribution;
				input.registration = registration;
				if (count < MAX_LAYERS) {
					layers[count] = environment;
					inputs[count++] = input;
					continue;
				}
				u32 lowest = 0;
				for (u32 i = 1; i < count; ++i) {
					if (inputs[i].priority < inputs[lowest].priority) {
						lowest = i;
					}
				}
				if (input.priority > inputs[lowest].priority) {
					layers[lowest] = environment;
					inputs[lowest] = input;
				}
			}
			out.count = count;
			out.remainder = GraphicsInternal::ComputeEnvironmentWeights({inputs, count}, {out.weights, count}, {out.order, count});
		}

		RenderState::BlendedEnvironment ResolveEnvironment(SceneKind kind, std::span<const u64> onlyScenes, const Vec3& cameraPosition,
														   const Vec3& sunColor) {
			RenderState& state = *_state;
			constexpr u32 MAX_LAYERS = EnvironmentLayers::MAX_LAYERS;
			EnvironmentLayers gathered;
			GatherEnvironmentLayers(state, kind, onlyScenes, cameraPosition, gathered);
			Environment* const* layers = gathered.layers;
			const GraphicsInternal::EnvironmentLayerInput* inputs = gathered.inputs;
			const f32* weights = gathered.weights;
			const u32* order = gathered.order;
			const u32 count = gathered.count;
			const f32 remainder = gathered.remainder;

			RenderState::EnvironmentSetup answers[MAX_LAYERS];
			for (u32 i = 0; i < count; ++i) {
				if (weights[i] > 0.0f) {
					answers[i] = ResolveOneEnvironment(layers[i], sunColor);
				}
			}
			const RenderState::EnvironmentSetup neutral = NeutralEnvironment();

			RenderState::BlendedEnvironment blended;
			for (u32 k = 0; k < 9; ++k) {
				blended.sh[k] = neutral.sh[k] * remainder;
			}
			blended.sunTint = Vec3{1.0f, 1.0f, 1.0f} * remainder;
			for (u32 i = 0; i < count; ++i) {
				if (weights[i] <= 0.0f) {
					continue;
				}
				for (u32 k = 0; k < 9; ++k) {
					blended.sh[k] += answers[i].sh[k] * weights[i];
				}
				blended.sunTint += answers[i].sunTint * weights[i];
			}

			for (u32 i = 0; i < count; ++i) {
				const Environment& layer = *layers[i];
				if (weights[i] <= 0.0f || !layer.GetFog()) {
					continue;
				}
				RenderState::BlendedEnvironment::FogLayer& fog = blended.fog[blended.fogCount++];
				const Color color = layer.GetFogColor();
				fog.color = Vec3{color.r, color.g, color.b};
				fog.share = weights[i];
				const FogMode mode = layer.GetFogMode();
				if (mode == FogMode::Linear) {
					const f32 range = Math::Max(layer.GetFogEnd() - layer.GetFogStart(), 1e-4f);
					fog.params = Vec3{static_cast<f32>(mode), -1.0f / range, layer.GetFogEnd() / range};
				} else {
					fog.params = Vec3{static_cast<f32>(mode), Math::Max(layer.GetFogDensity(), 0.0f), 0.0f};
				}
			}

			f32 cubeWeights[MAX_LAYERS + 1]{};
			bool hasCube[MAX_LAYERS + 1]{};
			const RenderState::EnvironmentSetup* candidates[MAX_LAYERS + 1]{};
			blended.reflectionIntensity = remainder;
			for (u32 i = 0; i < count; ++i) {
				candidates[i] = &answers[i];
				if (weights[i] <= 0.0f) {
					continue;
				}
				blended.reflectionIntensity += weights[i] * answers[i].reflectionIntensity;
				if (answers[i].mipCount > 0) {
					hasCube[i] = true;
					cubeWeights[i] = weights[i];
				} else {
					blended.flatRadiance += answers[i].flatRadiance * (weights[i] * answers[i].reflectionIntensity);
				}
			}
			candidates[count] = &neutral;
			hasCube[count] = remainder > 0.0f;
			cubeWeights[count] = remainder;
			u32 chosen[2]{};
			f32 chosenWeights[2]{};
			GraphicsInternal::SelectReflectionCubes({cubeWeights, count + 1}, {hasCube, count + 1}, chosen, chosenWeights);
			for (u32 slot = 0; slot < 2; ++slot) {
				blended.cube[slot] = state.defaultEnvironmentCube;
				if (chosen[slot] == GraphicsInternal::NO_REFLECTION_LAYER) {
					continue;
				}
				const RenderState::EnvironmentSetup& source = *candidates[chosen[slot]];
				blended.cube[slot] = source.cube;
				blended.mipCount[slot] = source.mipCount;
				blended.cubeWeight[slot] = chosenWeights[slot] * source.reflectionIntensity * source.radianceScale;
			}

			u32 drawn[RenderState::MAX_SKY_LAYERS]{};
			u32 drawnCount = 0;
			for (u32 i = count; i > 0 && drawnCount < RenderState::MAX_SKY_LAYERS; --i) {
				const u32 layer = order[i - 1];
				if (weights[layer] <= 0.0f) {
					break;
				}
				if (answers[layer].drawSky) {
					drawn[drawnCount++] = layer;
				}
				if (inputs[layer].contribution >= 1.0f) {
					break;
				}
			}
			for (u32 i = 0; i < drawnCount; ++i) {
				const u32 layer = drawn[drawnCount - 1 - i];
				blended.skies[i] = answers[layer];
				blended.skyAlpha[i] = inputs[layer].contribution;
			}
			blended.skyCount = drawnCount;
			return blended;
		}

		u64 WriteExposureWord(RHI::Device& device, f32 exposure) {
			RHI::TransientAllocation allocation = device.AllocateTransient(16, 16);
			if (!allocation.IsValid()) {
				return 0;
			}
			std::memcpy(allocation.cpu, &exposure, sizeof(f32));
			return device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
		}

		f32 ResolveViewExposure(RenderState& state, SceneKind kind, std::span<const u64> onlyScenes, const Vec3& cameraPosition,
								const Camera* camera) {
			EnvironmentLayers gathered;
			GatherEnvironmentLayers(state, kind, onlyScenes, cameraPosition, gathered);
			GraphicsInternal::ExposureLayerInput inputs[EnvironmentLayers::MAX_LAYERS];
			for (u32 i = 0; i < gathered.count; ++i) {
				const Environment& layer = *gathered.layers[i];
				inputs[i].mode = layer.GetExposureMode();
				inputs[i].fixedEV100 = layer.GetFixedExposure();
				inputs[i].compensation = layer.GetExposureCompensation();
			}
			const f32 aperture = camera ? camera->GetAperture() : GraphicsInternal::DEFAULT_CAMERA_APERTURE;
			const f32 shutterSpeed = camera ? camera->GetShutterSpeed() : GraphicsInternal::DEFAULT_CAMERA_SHUTTER_SPEED;
			const f32 iso = camera ? static_cast<f32>(camera->GetIso()) : GraphicsInternal::DEFAULT_CAMERA_ISO;
			return GraphicsInternal::BlendExposure({inputs, gathered.count}, {gathered.weights, gathered.count}, gathered.remainder,
												   state.physicalLightUnits, GraphicsInternal::PhysicalCameraEV100(aperture, shutterSpeed, iso));
		}

		void ResolveFrameExposures(RenderState& state, RHI::Device& device, u32 workCount) {
			for (const RenderState::PendingView& pending : state.pendingViews) {
				RenderState::PassScope& scope = state.passScopes[pending.scopeIndex];
				scope.exposure = 1.0f;
				if (pending.radianceCapture) {
					scope.exposure = GraphicsInternal::ReferenceExposure(state.physicalLightUnits);
				} else if (pending.tonemap) {
					scope.exposure = ResolveViewExposure(state, pending.kind, {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
														 pending.cameraPosition, pending.camera.Get());
				}
				scope.exposureAddress = WriteExposureWord(device, scope.exposure);
			}
			for (u32 w = 0; w < workCount; ++w) {
				for (const RenderState::CameraWork& cameraWork : state.frameWork[w].cameras) {
					RenderState::PassScope& scope = state.passScopes[cameraWork.scopeIndex];
					const SceneObject* owner = cameraWork.camera ? cameraWork.camera->GetOwner().Get() : nullptr;
					scope.exposure = owner ? ResolveViewExposure(state, SceneKind::World, {}, owner->GetWorldPosition(), cameraWork.camera) : 1.0f;
					scope.exposureAddress = WriteExposureWord(device, scope.exposure);
				}
			}
		}

		bool FirstProbeSkip(RenderState& state, Handle<ReflectionProbe> probe, const TextureAsset& cube, i32 face, RHI::Format format) {
			const RenderState::SkippedProbe now{probe, cube.GetInstanceId(), cube.version, face, format};
			auto skipped = std::find_if(state.skippedProbes.begin(), state.skippedProbes.end(),
										[&](const RenderState::SkippedProbe& entry) { return entry.probe == probe; });
			if (skipped == state.skippedProbes.end()) {
				state.skippedProbes.push_back(now);
				return true;
			}
			if (skipped->cube != now.cube || skipped->version != now.version || skipped->face != now.face || skipped->format != now.format) {
				*skipped = now;
				return true;
			}
			return false;
		}

		const char* ProbeCubeStorage(RHI::Format format) {
			return format == RHI::Format::BC6HUFloat ? "BC6H" : "uncompressed";
		}

		void ReconcileReflectionProbeArray(RenderState& state) {
			RHI::Device& device = *state.device;
			const i32 requestedFace = state.reflectionProbeResolution;

			state.reflectionProbeSlices.clear();
			RHI::Format arrayFormat = RHI::Format::Unknown;
			for (const Handle<ReflectionProbe>& handle : state.reflectionProbes) {
				ReflectionProbe* probe = handle.Get();
				if (!probe) {
					continue;
				}
				TextureAsset* cube = probe->GetBakedCubemap().Get();
				if (!cube || !cube->isCube || cube->pixels.empty()) {
					continue;
				}
				if (cube->size.x != requestedFace || cube->size.y != requestedFace) {
					if (FirstProbeSkip(state, handle, *cube, requestedFace, RHI::Format::Unknown)) {
						SLT_LOG_WARN("Graphics",
									 "a reflection probe's cube is {}x{} where the profile asks for {}. Rebake it",
									 cube->size.x, cube->size.y, requestedFace);
					}
					continue;
				}
				if (arrayFormat == RHI::Format::Unknown) {
					arrayFormat = cube->format;
				} else if (cube->format != arrayFormat) {
					if (FirstProbeSkip(state, handle, *cube, requestedFace, arrayFormat)) {
						SLT_LOG_WARN("Graphics",
									 "a reflection probe's cube is {} where the probes before it are {}. Give its import settings "
									 "the same compression",
									 ProbeCubeStorage(cube->format), ProbeCubeStorage(arrayFormat));
					}
					continue;
				}
				state.reflectionProbeSlices.push_back(handle);
			}

			const u32 cubes = static_cast<u32>(state.reflectionProbeSlices.size());
			bool changed = cubes != state.reflectionProbeArrayCubes || requestedFace != state.reflectionProbeArrayFace;
			if (!changed) {
				for (u32 slice = 0; slice < cubes; ++slice) {
					TextureAsset* cube = state.reflectionProbeSlices[slice]->GetBakedCubemap().Get();
					if (state.reflectionProbeArrayKeys[slice] != RenderState::ProbeSliceKey{cube->GetInstanceId(), cube->version}) {
						changed = true;
						break;
					}
				}
			}
			if (!changed) {
				return;
			}

			if (state.reflectionProbeArray.IsValid()) {
				device.DestroyResource(state.reflectionProbeArray);
				state.reflectionProbeArray = {};
			}
			state.reflectionProbeArrayCubes = 0;
			state.reflectionProbeArrayFace = requestedFace;
			state.reflectionProbeArrayKeys.clear();
			if (cubes == 0) {
				return;
			}

			u32 mipLevels = 0xFFFFFFFFu;
			for (const Handle<ReflectionProbe>& handle : state.reflectionProbeSlices) {
				mipLevels = Math::Min(mipLevels, Math::Max(1u, handle->GetBakedCubemap()->storedMipLevels));
			}

			RHI::TextureDesc desc;
			desc.size = {requestedFace, requestedFace};
			desc.format = arrayFormat;
			desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			desc.mipLevels = mipLevels;
			desc.arrayLayers = cubes * GraphicsInternal::CUBE_FACE_COUNT;
			desc.cube = true;
			desc.cubeArray = true;
			desc.debugName = "ReflectionProbes";
			state.reflectionProbeArray = device.CreateTextureResource(desc);
			if (!state.reflectionProbeArray.IsValid()) {
				state.reflectionProbeSlices.clear();
				return;
			}

			// mip major, face minor (like TextureAsset::PIXELS)
			for (u32 slice = 0; slice < cubes; ++slice) {
				TextureAsset* cube = state.reflectionProbeSlices[slice]->GetBakedCubemap().Get();
				size_t offset = 0;
				for (u32 mip = 0; mip < mipLevels; ++mip) {
					const i32 edge = Math::Max(1, requestedFace >> mip);
					const size_t faceBytes = static_cast<size_t>(RHI::FormatLevelBytes(arrayFormat, {edge, edge}));
					for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
						if (offset + faceBytes > cube->pixels.size()) {
							break;
						}
						device.UploadTexture(state.reflectionProbeArray, {cube->pixels.data() + offset, faceBytes},
											 mip, slice * GraphicsInternal::CUBE_FACE_COUNT + face);
						offset += faceBytes;
					}
				}
				state.reflectionProbeArrayKeys.push_back({cube->GetInstanceId(), cube->version});
			}
			state.reflectionProbeArrayCubes = cubes;
		}

		u32 ExtractLightCandidates(const Mat4& view, const Mat4& projection, const Vec3& cameraPosition,
								   SceneKind kind, std::span<const u64> onlyScenes,
								   u32& outDirectionalCount, u32& outDropped) {
			RenderState& state = *_state;
			const bool isolated = kind != SceneKind::World;
			const Vector<Handle<Light>>& lights = isolated ? state.isolatedLights : state.lights;
			std::span<const u64> lightScenes = isolated ? onlyScenes : std::span<const u64>{};
			const Frustum frustum = Frustum::FromMatrix(projection * view);

			Vector<GraphicsInternal::LightCandidate>& candidates = state.lightCandidates;
			candidates.clear();
			for (Handle<Light> lightHandle : lights) {
				Light* light = lightHandle.Get();
				if (!light) {
					continue;
				}
				SceneObject* lightOwner = light->GetOwner().Get();
				if (!lightOwner) {
					continue;
				}
				if (!lightScenes.empty()) {
					u64 sceneId = lightOwner->GetScene().GetInstanceId();
					if (std::find(lightScenes.begin(), lightScenes.end(), sceneId) == lightScenes.end()) {
						continue;
					}
				}
				const LightType type = light->GetLightType();
				const Vec3 position = lightOwner->GetWorldPosition();
				if (type != LightType::Directional && !frustum.Intersects(Sphere(position, light->GetRange()))) {
					continue;
				}
				Color lightColor = light->GetColor();

				GraphicsInternal::LightCandidate& candidate = candidates.emplace_back();
				candidate.position = position;
				candidate.color = Vec3{lightColor.r, lightColor.g, lightColor.b} *
								  GraphicsInternal::SolveLightIntensity(type, light->GetIntensity(), state.physicalLightUnits);
				candidate.direction = lightOwner->TransformDirection(Vec3::Forward()).Normalized();
				candidate.type = type;
				candidate.range = type == LightType::Directional ? 0.0f : Math::Max(light->GetRange(), 0.001f);
				candidate.outerAngleDeg = light->GetSpotAngleDeg();
				if (type == LightType::Spot) {
					GraphicsInternal::SolveSpotCone(light->GetSpotAngleDeg(), light->GetInnerSpotAngleDeg(),
													candidate.spotScale, candidate.spotOffset);
				}
				candidate.castShadows = light->GetCastShadows();
				candidate.shadowStrength = Math::Clamp(light->GetShadowStrength(), 0.0f, 1.0f);
				candidate.shadowBias = Math::Max(light->GetShadowBias(), 0.0f);
				candidate.shadowNormalBias = Math::Max(light->GetShadowNormalBias(), 0.0f);
				candidate.shadowNearPlane = light->GetShadowNearPlane();
				candidate.shadowCasterBias = Vec2(Math::Max(light->GetShadowCasterBias(), 0.0f),
												  Math::Max(light->GetShadowCasterSlopeBias(), 0.0f));
				candidate.stableKey = light->GetInstanceId();
				candidate.renderingLayerMask = light->GetRenderingLayerMask();
				candidate.shadowLayerMask = light->GetShadowLayerMask();
				candidate.cookie = light->GetCookie().Get();
				if (candidate.cookie) {
					candidate.right = lightOwner->TransformDirection(Vec3::Right()).Normalized();
					candidate.up = lightOwner->TransformDirection(Vec3::Up()).Normalized();
					candidate.cookieSize = light->GetCookieSize();
					candidate.cookieOffset = light->GetCookieOffset();
				}
			}

			return GraphicsInternal::SelectViewLights({candidates.data(), candidates.size()}, cameraPosition,
													  state.lightsPerView, outDirectionalCount, outDropped);
		}

		u32 TableEntryOf(RenderState& state, const TextureAsset* texture) {
			if (!texture || !texture->texture.IsValid()) {
				return RHI::TABLE_NONE;
			}
			const RHI::SamplerHandle sampler = TextureSampler(state, *texture);
			const u32 image = state.device->GetTableIndex(texture->texture);
			const u32 slot = sampler.IsValid() ? state.device->GetTableIndex(sampler) : RHI::TABLE_NONE;
			return image == RHI::TABLE_NONE || slot == RHI::TABLE_NONE ? RHI::TABLE_NONE : image | (slot << TABLE_SAMPLER_SHIFT);
		}

		bool PackLightCookie(RenderState& state, const GraphicsInternal::LightCandidate& candidate, GPULightCookie& out) {
			const TextureAsset* cookie = candidate.cookie;
			if (!cookie || !cookie->texture.IsValid()) {
				return false;
			}
			const RHI::TextureDimension wanted = candidate.type == LightType::Point ? RHI::TextureDimension::Cube : RHI::TextureDimension::Texture2D;
			if (cookie->GetDimension() != wanted) {
				const u64 key = candidate.stableKey * 0x9E3779B97F4A7C15ull ^ cookie->GetInstanceId();
				if (std::find(state.dimensionMismatchesWarned.begin(), state.dimensionMismatchesWarned.end(), key) ==
					state.dimensionMismatchesWarned.end()) {
					state.dimensionMismatchesWarned.push_back(key);
					SLT_LOG_WARN("Graphics", "light cookie '{}' is {} (expected {}, ignoring)", cookie->virtualPath,
								 RHI::TextureDimensionName(cookie->GetDimension()), RHI::TextureDimensionName(wanted));
				}
				return false;
			}
			const u32 entry = TableEntryOf(state, cookie);
			if (entry == RHI::TABLE_NONE) {
				return false;
			}
			out = GPULightCookie{};
			out.entry = entry;

			const Vec3& origin = candidate.position;
			const Vec3& right = candidate.right;
			const Vec3& up = candidate.up;
			const Vec3& forward = candidate.direction;
			auto row = [&](const Vec3& axis, f32 bias) { return Vec4(axis, bias - Vec3::Dot(axis, origin)); };
			switch (candidate.type) {
			case LightType::Directional: {
				auto safe = [](f32 value) { return std::abs(value) > 1e-4f ? value : std::copysign(1e-4f, value); };
				const f32 width = safe(candidate.cookieSize.x);
				const f32 height = safe(candidate.cookieSize.y);
				out.projection = COOKIE_ORTHOGRAPHIC;
				out.rows[0] = row(right / width, 0.5f - candidate.cookieOffset.x / width);
				out.rows[1] = row(-up / height, 0.5f + candidate.cookieOffset.y / height);
				break;
			}
			case LightType::Spot: {
				const f32 slope = std::tan(Math::Clamp(candidate.outerAngleDeg, 1.0f, 179.0f) * 0.5f * Math::DEG_TO_RAD);
				out.projection = COOKIE_PERSPECTIVE;
				out.rows[0] = row(forward * (0.5f * slope) + right * 0.5f, 0.0f);
				out.rows[1] = row(forward * (0.5f * slope) - up * 0.5f, 0.0f);
				out.rows[2] = row(forward * slope, 0.0f);
				break;
			}
			case LightType::Point:
				out.projection = COOKIE_CUBE;
				out.rows[0] = row(right, 0.0f);
				out.rows[1] = row(up, 0.0f);
				out.rows[2] = row(forward, 0.0f);
				break;
			}
			return true;
		}

		u32 LightInfo(const GraphicsInternal::LightCandidate& candidate) {
			u32 info = candidate.renderingLayerMask & LIGHT_LAYERS;
			if (candidate.renderingLayerMask == ~0u) {
				info |= LIGHT_FLAG_EVERY_LAYER;
			}
			return info;
		}

		void ExtractDecals(const Mat4& viewProjection, const Vec3& cameraPosition, SceneKind kind, std::span<const u64> onlyScenes) {
			RenderState& state = *_state;
			state.decalCandidates.clear();
			state.decalOrder.clear();
			state.decalBounds.clear();
			const bool isolated = kind != SceneKind::World;
			const Vector<Handle<Decal>>& decals = isolated ? state.isolatedDecals : state.decals;
			if (decals.empty()) {
				return;
			}
			const std::span<const u64> scenes = isolated ? onlyScenes : std::span<const u64>{};
			const Frustum frustum = Frustum::FromMatrix(viewProjection);
			for (Handle<Decal> handle : decals) {
				const Decal* decal = handle.Get();
				const SceneObject* owner = decal ? decal->GetOwner().Get() : nullptr;
				const MaterialAsset* material = decal ? decal->GetMaterial().Get() : nullptr;
				if (!owner || !material) {
					continue;
				}
				if (!scenes.empty() && std::find(scenes.begin(), scenes.end(), owner->GetScene().GetInstanceId()) == scenes.end()) {
					continue;
				}
				if (material->shaderGuid != EngineBuiltins::DecalShader) {
					const u64 key = decal->GetInstanceId() * 0x9E3779B97F4A7C15ull ^ material->GetInstanceId();
					if (std::find(state.dimensionMismatchesWarned.begin(), state.dimensionMismatchesWarned.end(), key) ==
						state.dimensionMismatchesWarned.end()) {
						state.dimensionMismatchesWarned.push_back(key);
						SLT_LOG_WARN("Graphics", "decal material '{}' doesn't use Decal shader (ignoring)", material->virtualPath);
					}
					continue;
				}
				const Mat4 decalToWorld = owner->GetWorldMatrix() * Mat4::Translation(decal->GetPivot()) * Mat4::Scale(decal->GetSize());
				Mat4 worldToDecal;
				if (!decalToWorld.TryInvert(worldToDecal)) {
					continue;
				}
				const Vec3 center = decalToWorld.TransformPoint(Vec3());
				f32 radius = 0.0f;
				for (const Vec3& corner : {Vec3(0.5f, 0.5f, 0.5f), Vec3(-0.5f, 0.5f, 0.5f), Vec3(0.5f, -0.5f, 0.5f), Vec3(-0.5f, -0.5f, 0.5f)}) {
					radius = Math::Max(radius, decalToWorld.TransformVector(corner).Length());
				}
				const f32 drawDistance = Math::Max(decal->GetDrawDistance(), 0.0f);
				const f32 distance = (center - cameraPosition).Length();
				if (distance > drawDistance || !frustum.Intersects(Sphere(center, radius))) {
					continue;
				}
				const f32 fadeRange = drawDistance * (1.0f - Math::Clamp(decal->GetStartFade(), 0.0f, 1.0f));
				const f32 fade = fadeRange > 1e-4f ? Math::Clamp((drawDistance - distance) / fadeRange, 0.0f, 1.0f) : 1.0f;

				state.decalOrder.push_back((static_cast<u64>(static_cast<u32>(decal->GetPriority()) ^ 0x80000000u) << 32) |
										   static_cast<u64>(state.decalCandidates.size()));
				RenderState::DecalCandidate& candidate = state.decalCandidates.emplace_back();
				candidate.bound = {center, radius};
				GPUDecal& record = candidate.record;
				for (u32 row = 0; row < 3; ++row) {
					record.rows[row] = Vec4(worldToDecal(row, 0), worldToDecal(row, 1), worldToDecal(row, 2), worldToDecal(row, 3));
				}
				const Vec2 tiling = decal->GetTiling();
				const Vec2 offset = decal->GetOffset();
				record.uvScaleBias = Vec4(tiling.x, tiling.y, offset.x, offset.y);
				const Color tint = material->GetColor(StringHash("baseColor"), Color::White());
				record.baseColor = Vec4(tint.r, tint.g, tint.b, tint.a * Math::Clamp(decal->GetOpacity(), 0.0f, 1.0f) * fade);
				if (const MaterialAsset::Param* emissive = material->FindParam(StringHash("emissive"))) {
					record.emissive = Vec3(emissive->numbers[0], emissive->numbers[1], emissive->numbers[2]);
				}
				record.normalScale = material->GetFloat(StringHash("normalScale"), 1.0f);
				record.baseColorEntry = TableEntryOf(state, material->GetTexture(StringHash("baseColorTexture")).Get());
				record.normalEntry = TableEntryOf(state, material->GetTexture(StringHash("normalTexture")).Get());
				record.maskEntry = TableEntryOf(state, material->GetTexture(StringHash("maskTexture")).Get());
				record.emissiveEntry = TableEntryOf(state, material->GetTexture(StringHash("emissiveTexture")).Get());
				const Vec2 angleFade = decal->GetAngleFade();
				const f32 fadeStart = Math::Clamp(angleFade.x, 0.0f, 180.0f) * Math::DEG_TO_RAD;
				const f32 fadeEnd = Math::Clamp(Math::Max(angleFade.y, angleFade.x), 0.0f, 180.0f) * Math::DEG_TO_RAD;
				if (fadeStart < Math::PI) {
					const f32 range = Math::Max(fadeEnd - fadeStart, 1e-4f);
					record.angleFadeScale = -1.0f / range;
					record.angleFadeBias = (fadeStart + range) / range;
				}
				const u32 mask = decal->GetRenderingLayerMask();
				record.layers = (mask & LIGHT_LAYERS) | (mask == ~0u ? DECAL_EVERY_LAYER : 0u);
			}
			std::sort(state.decalOrder.begin(), state.decalOrder.end());
			for (u64 key : state.decalOrder) {
				state.decalBounds.push_back(state.decalCandidates[static_cast<u32>(key)].bound);
			}
		}

		constexpr u64 PENDING_VIEW_LOD_KEY = 1ull << 63;

		u64 LODViewKey(const RenderState::PendingView& view) {
			return PENDING_VIEW_LOD_KEY | view.targetId;
		}

		u64 LODViewKey(const Camera* camera) {
			return camera ? camera->GetInstanceId() & ~PENDING_VIEW_LOD_KEY : 0;
		}

		class ViewRecordScope {
		public:
			explicit ViewRecordScope(const RenderState::PendingView& view)
				: ViewRecordScope(view.hasTime, view.time, _state->passScopes[view.scopeIndex].exposureAddress, LODViewKey(view)) {}
			explicit ViewRecordScope(const RenderState::CameraWork& camera)
				: ViewRecordScope(false, 0.0, _state->passScopes[camera.scopeIndex].exposureAddress, LODViewKey(camera.camera)) {}
			ViewRecordScope(bool hasTime, f64 time, u64 exposureAddress, u64 lodView = 0) {
				RenderState& state = *_state;
				state.hasViewTime = hasTime;
				state.viewTime = time;
				state.viewExposureAddress = exposureAddress;
				state.lodView = lodView;
			}
			~ViewRecordScope() {
				_state->hasViewTime = false;
				_state->viewTime = 0.0;
				_state->viewExposureAddress = 0;
				_state->lodView = 0;
			}
			ViewRecordScope(const ViewRecordScope&) = delete;
			ViewRecordScope& operator=(const ViewRecordScope&) = delete;
		};

		GPUViewData BuildViewData(const Mat4& view, const Mat4& projection, Vec2I extent) {
			GPUViewData record;
			record.view = view;
			record.inverseView = view.Inverse();
			record.projection = projection;
			record.inverseProjection = projection.Inverse();
			record.inverseViewProjection = (projection * view).Inverse();
			const RenderState& state = *_state;
			const f64 time = state.hasViewTime ? state.viewTime : Time::GetTime();
			record.time = Vec4(static_cast<f32>(time), static_cast<f32>(std::sin(time)), static_cast<f32>(std::cos(time)),
							   state.hasViewTime ? 0.0f : Time::GetDeltaTime());
			const bool orthographic = projection(3, 3) == 1.0f && projection(3, 2) == 0.0f;
			const f32 a = projection(2, 2);
			const f32 b = projection(2, 3);
			f32 nearPlane = 0.0f;
			f32 farPlane = 0.0f;
			if (a != 0.0f) {
				farPlane = -b / a;
				nearPlane = orthographic ? farPlane + 1.0f / a : b / (1.0f - a);
			}
			record.projectionParams = Vec4(nearPlane, farPlane, orthographic ? 1.0f : 0.0f, 0.0f);
			record.screenParams = Vec4(static_cast<f32>(extent.x), static_cast<f32>(extent.y),
									   projection(0, 0) != 0.0f ? 1.0f / projection(0, 0) : 0.0f,
									   projection(1, 1) != 0.0f ? 1.0f / projection(1, 1) : 0.0f);
			record.exposureAddress = state.viewExposureAddress;
			return record;
		}

		u64 WriteViewData(RHI::Device& device, const Mat4& view, const Mat4& projection, Vec2I extent) {
			RHI::TransientAllocation allocation = device.AllocateTransient(sizeof(GPUViewData), 16);
			if (!allocation.IsValid()) {
				return 0;
			}
			const GPUViewData record = BuildViewData(view, projection, extent);
			std::memcpy(allocation.cpu, &record, sizeof(GPUViewData));
			return device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
		}

		void DrawMesh(RHI::CommandList& cmd, const DrawEntry& entry, u32 instances = 1) {
			cmd.SetFrontFace(entry.mirrored ? RHI::FrontFace::CounterClockwise : RHI::FrontFace::Clockwise);
			cmd.BindIndexBuffer(entry.mesh->indexBuffer);
			cmd.DrawIndexed(entry.submesh->indexCount, entry.submesh->firstIndex, instances);
		}

		bool SharesInstancedDraw(const DrawEntry& first, const DrawEntry& next) {
			return next.mesh == first.mesh && next.submesh == first.submesh && next.shader == first.shader &&
				   next.variant == first.variant && next.materialId == first.materialId && next.mirrored == first.mirrored &&
				   (next.lodFade != 0) == (first.lodFade != 0);
		}

		// record = first + instance, a run can't skip entries
		template <typename Joins>
		u32 InstanceRunLength(DrawEntry* const* list, u32 first, u32 end, const Joins& joins) {
			u32 last = first + 1;
			while (last < end && SharesInstancedDraw(*list[first], *list[last]) && joins(*list[last])) {
				++last;
			}
			return last - first;
		}

		struct SingleInstance {
			u32 operator()() const { return 1; }
		};

		struct DepthDrawBinding {
			RHI::PipelineHandle pipeline{};
			u64 material = ~0ull;
			RHI::TransientSet materialSet{};
			bool biasSet = false;
			Vec2 bias{};
		};

		bool BindDepthDraw(RHI::CommandList& cmd, RHI::Device& device, DepthDrawBinding& bound, const DrawEntry& entry,
						   const RHI::ShaderRenderState& drawState, RHI::Format colorFormat, RHI::Format depthFormat,
						   const RHI::TransientAllocation& viewAllocation, u64 drawAddress, u32 value, bool& instanced,
						   u32 samples = 1) {
			RHI::PipelineHandle pipeline = GetOrCreatePipeline(entry.shader, colorFormat, depthFormat, &drawState, samples, entry.variant,
															   ShaderProgramKind::Depth, &instanced, entry.lodFade != 0);
			if (!pipeline.IsValid()) {
				return false;
			}
			if (!(pipeline == bound.pipeline)) {
				cmd.SetPipeline(pipeline);
				bound.pipeline = pipeline;
				bound.material = ~0ull;
				bound.biasSet = false;
				cmd.BindTransientSet(0, CreateViewSet(device, pipeline, *entry.shader, viewAllocation,
													  RHI::TextureHandle{}, RHI::TextureHandle{}));
				if (SceneRequests::DeclaresSceneSet(*entry.shader)) {
					BindSceneTextureSet(cmd, device, pipeline, *entry.shader, SceneRequestTextures{}, PassView{});
				}
			}
			if (entry.materialId != bound.material || !bound.materialSet.IsValid()) {
				if (RHI::TransientSet set = BindMaterialSet(cmd, device, pipeline, entry); set.IsValid()) {
					bound.materialSet = set;
				}
				bound.material = entry.materialId;
			}
			const Vec2 bias(drawState.depthBiasConstant, drawState.depthBiasSlope);
			if (drawState.dynamicDepthBias && (!bound.biasSet || bound.bias != bias)) {
				cmd.SetDepthBias(bias.x, bias.y);
				bound.bias = bias;
				bound.biasSet = true;
			}
			GPUDrawHeader header;
			header.drawAddress = drawAddress;
			header.value = value;
			cmd.PushConstants({reinterpret_cast<const u8*>(&header), DRAW_HEADER_BYTES});
			return true;
		}

		template <typename RunLength = SingleInstance>
		u32 RecordDepthDraw(RHI::CommandList& cmd, RHI::Device& device, DepthDrawBinding& bound, DrawEntry& entry,
							const RHI::ShaderRenderState& drawState, RHI::Format colorFormat, RHI::Format depthFormat,
							const RHI::TransientAllocation& viewAllocation, u64 drawAddress, u32 value = 0,
							const RunLength& runLength = {}, u32 samples = 1) {
			bool instanced = false;
			if (!BindDepthDraw(cmd, device, bound, entry, drawState, colorFormat, depthFormat, viewAllocation, drawAddress, value,
							   instanced, samples)) {
				return 0;
			}
			const u32 instances = instanced ? runLength() : 1u;
			DrawMesh(cmd, entry, instances);
			return instances;
		}

		struct GPUMomentResolvePush {
			i32 tileOriginX = 0;
			i32 tileOriginY = 0;
			i32 tileSizeX = 0;
			i32 tileSizeY = 0;
			f32 positiveExponent = 0.0f;
			f32 negativeExponent = 0.0f;
			i32 radius = 0;
			u32 samples = 1;
		};

		bool ResolveMoments(RHI::CommandList& cmd, RHI::Device& device, RHI::TextureHandle momentAtlas, const RectI& tile) {
			RenderState& state = *_state;
			ShaderAsset* shader = state.momentResolveShader.Get();
			const RHI::PipelineHandle pipeline =
				shader ? GetOrCreatePipeline(shader, RHI::Format::RGBA32Float, RHI::Format::Unknown) : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return false;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = momentAtlas;
			attachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			cmd.TransitionTexture(momentAtlas, RHI::ResourceState::RenderTarget);
			cmd.BeginRenderPass(desc);
			cmd.SetViewportScissor(tile);
			cmd.SetPipeline(pipeline);
			cmd.BindTransientSet(1, device.CreateTransientSet(pipeline, 1).WriteTexture(0, state.momentDepth).WriteSampler(1, state.clampSampler).Commit());
			GPUMomentResolvePush push;
			push.tileOriginX = tile.Left();
			push.tileOriginY = tile.Top();
			push.tileSizeX = tile.size.x;
			push.tileSizeY = tile.size.y;
			push.positiveExponent = state.shadowMomentParams.x;
			push.negativeExponent = state.shadowMomentParams.y;
			push.radius = static_cast<i32>(state.shadowMomentBlur);
			push.samples = state.momentSamples;
			cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			cmd.Draw(3);
			cmd.EndRenderPass();
			return true;
		}

		void GatherObjectShadowMembers(RenderState& state, SceneObject& root, u32 layerMask, AABB& bounds) {
			Vector<SceneObject*>& stack = state.objectShadowStack;
			stack.clear();
			stack.push_back(&root);
			while (!stack.empty()) {
				SceneObject* object = stack.back();
				stack.pop_back();
				if (!object->IsActiveInHierarchy()) {
					continue;
				}
				if (object != &root) {
					if (const Handle<ObjectShadow> nested = object->GetComponent<ObjectShadow>(); nested && nested->IsEnabledSelf()) {
						continue;
					}
				}
				state.objectShadowMeshes.clear();
				object->GetComponents<MeshRenderer>(state.objectShadowMeshes);
				for (const Handle<MeshRenderer>& handle : state.objectShadowMeshes) {
					const MeshRenderer* renderer = handle.Get();
					const MeshAsset* mesh = renderer ? renderer->GetMesh().Get() : nullptr;
					if (!mesh || !mesh->bounds.IsValid() || !renderer->IsEnabledSelf() || renderer->GetCastShadows() == ShadowCasting::Off ||
						(renderer->GetRenderingLayerMask() & layerMask) == 0) {
						continue;
					}
					bounds.Encapsulate(mesh->bounds.Transformed(object->GetWorldMatrix()));
					state.objectShadowMemberIds.push_back(renderer->GetInstanceId());
				}
				state.objectShadowSkinned.clear();
				object->GetComponents<SkinnedMeshRenderer>(state.objectShadowSkinned);
				for (const Handle<SkinnedMeshRenderer>& handle : state.objectShadowSkinned) {
					const SkinnedMeshRenderer* renderer = handle.Get();
					const MeshAsset* mesh = renderer ? renderer->GetMesh().Get() : nullptr;
					if (!mesh || !renderer->IsEnabledSelf() || renderer->GetCastShadows() == ShadowCasting::Off ||
						(renderer->GetRenderingLayerMask() & layerMask) == 0) {
						continue;
					}
					const size_t index = static_cast<size_t>(
						std::find(state.skinnedMeshRenderers.begin(), state.skinnedMeshRenderers.end(), handle) - state.skinnedMeshRenderers.begin());
					const bool skinned = index < state.skinnedOutputs.size() && state.skinnedOutputs[index].vertexAddress != 0;
					const AABB& local = skinned ? state.skinnedOutputs[index].bounds : mesh->bounds;
					if (!local.IsValid()) {
						continue;
					}
					const Skeleton* skeleton = renderer->GetSkeleton().Get();
					const SceneObject* skeletonOwner = skeleton ? skeleton->GetOwner().Get() : nullptr;
					bounds.Encapsulate(local.Transformed(skeletonOwner ? skeletonOwner->GetWorldMatrix() : object->GetWorldMatrix()));
					state.objectShadowMemberIds.push_back(renderer->GetInstanceId());
				}
				for (u32 child = 0; child < object->GetChildCount(); ++child) {
					if (SceneObject* next = object->GetChild(child).Get()) {
						stack.push_back(next);
					}
				}
			}
		}

		u32 PickObjectShadows(RenderState& state, const Vec3& cameraPosition, std::span<const u64> onlyScenes, f32 reach, u32 layerMask) {
			Vector<RenderState::ObjectShadowPick>& picks = state.objectShadowPicks;
			picks.clear();
			state.objectShadowMemberIds.clear();
			state.objectShadowMembers.clear();
			if (state.objectShadowBudget == 0 || !(reach > 0.0f) || layerMask == 0) {
				return 0;
			}
			for (u32 i = 0; i < static_cast<u32>(state.objectShadows.size()); ++i) {
				const ObjectShadow* shadow = state.objectShadows[i].shadow.Get();
				const SceneObject* owner = shadow ? shadow->GetOwner().Get() : nullptr;
				if (!owner || !owner->IsActiveInHierarchy() || !shadow->IsEnabledSelf()) {
					continue;
				}
				if (!onlyScenes.empty() &&
					std::find(onlyScenes.begin(), onlyScenes.end(), owner->GetScene().GetInstanceId()) == onlyScenes.end()) {
					continue;
				}
				const f32 distanceSquared = Vec3::DistanceSquared(cameraPosition, owner->GetWorldPosition());
				if (distanceSquared > reach * reach) {
					continue;
				}
				RenderState::ObjectShadowPick& pick = picks.emplace_back();
				pick.record = i;
				pick.priority = shadow->GetPriority();
				pick.distanceSquared = distanceSquared;
				pick.normalOffset = Math::Max(shadow->GetNormalOffset(), 0.0f);
				pick.id = shadow->GetInstanceId();
			}
			std::sort(picks.begin(), picks.end(), [](const RenderState::ObjectShadowPick& a, const RenderState::ObjectShadowPick& b) {
				if (a.priority != b.priority) {
					return a.priority > b.priority;
				}
				if (a.distanceSquared != b.distanceSquared) {
					return a.distanceSquared < b.distanceSquared;
				}
				return a.id < b.id;
			});
			u32 kept = 0;
			for (u32 p = 0; p < static_cast<u32>(picks.size()) && kept < state.objectShadowBudget; ++p) {
				RenderState::ObjectShadowPick pick = picks[p];
				SceneObject* owner = state.objectShadows[pick.record].shadow->GetOwner().Get();
				pick.firstMember = static_cast<u32>(state.objectShadowMemberIds.size());
				pick.bounds = AABB::Empty();
				GatherObjectShadowMembers(state, *owner, layerMask, pick.bounds);
				pick.memberCount = static_cast<u32>(state.objectShadowMemberIds.size()) - pick.firstMember;
				if (pick.memberCount == 0 || !pick.bounds.IsValid()) {
					state.objectShadowMemberIds.resize(pick.firstMember);
					continue;
				}
				picks[kept++] = pick;
			}
			picks.resize(kept);
			return kept;
		}

		u32 RecordShadowPass(RHI::CommandList& cmd, LinearAllocator& arena,
							 const Mat4& view, const Mat4& projection, const Vec3& cameraPosition,
							 RHI::TextureHandle atlas, RHI::TextureHandle punctualAtlas, u64 owner, u32& outTriangles,
							 std::span<const u64> onlyScenes = {}, SceneKind kind = SceneKind::World,
							 const RenderState::ShadowFitCamera* fitCamera = nullptr,
							 Vector<GraphicsInternal::CascadeView>* outCascadeFit = nullptr,
							 std::span<const u32> casters = {}, u32 renderingLayerMask = ~0u) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			state.pendingShadow = RenderState::ShadowSetup{};
			state.shadowAllocations.clear();
			if (outCascadeFit) {
				outCascadeFit->clear();
			}
			if (!state.ShadowsEnabled() || !atlas.IsValid() || !punctualAtlas.IsValid()) {
				return 0;
			}

			u32 orderedDirectional = 0;
			u32 ignoredDrop = 0;
			const u32 candidateCount = ExtractLightCandidates(view, projection, cameraPosition, kind, onlyScenes,
															  orderedDirectional, ignoredDrop);
			Vector<GraphicsInternal::LightCandidate>& candidates = state.lightCandidates;
			if (candidateCount == 0) {
				return 0;
			}

			Vector<GraphicsInternal::CullVolume>& volumes = state.casterVolumes;
			volumes.clear();
			u32 mapCount = 0;

			u32 casterIndex = candidateCount;
			for (u32 i = 0; i < orderedDirectional; ++i) {
				if (candidates[i].castShadows) {
					casterIndex = i;
					break;
				}
			}

			u32 cascadeCount = 0;
			GraphicsInternal::CascadeSplits splits{};
			const u32 atlasSize = state.ShadowAtlasSize();
			if (casterIndex < candidateCount) {
				const Mat4& fitView = fitCamera ? fitCamera->view : view;
				const Mat4& fitProjection = fitCamera ? fitCamera->projection : projection;
				f32 distance = state.shadowDistance;
				f32 firstDistance = state.shadowFirstCascadeDistance;
				if (!fitCamera && state.lodView != 0 && state.shadowCascadeFit == ShadowCascadeFit::VisibleDepth) {
					const auto fit = state.depthFits.find(state.lodView);
					if (fit != state.depthFits.end() && fit->second.applied > 0.0f && fit->second.applied < distance) {
						firstDistance *= fit->second.applied / distance;
						distance = fit->second.applied;
					}
				}
				splits = GraphicsInternal::SolveCascadeSplits(CameraNearDepth(fitProjection), distance, state.shadowCascades, firstDistance);
				const u32 tileResolution = atlasSize / GraphicsInternal::CascadeTilesPerAxis(splits.count);
				state.cascadeViews.assign(splits.count, GraphicsInternal::CascadeView{});
				cascadeCount = GraphicsInternal::BuildCascadeViews(
					fitView, fitProjection, candidates[casterIndex].direction, splits, tileResolution,
					SHADOW_CASTER_DISTANCE, {state.cascadeViews.data(), state.cascadeViews.size()});
				if (cascadeCount > 0) {
					if (outCascadeFit) {
						outCascadeFit->assign(state.cascadeViews.begin(),
											  state.cascadeViews.begin() + cascadeCount);
					}
					++state.frameShadowLights;
					RenderState::ShadowAllocation& allocation = state.shadowAllocations.emplace_back();
					allocation.lightKey = candidates[casterIndex].stableKey;
					allocation.firstMap = 0;
					allocation.mapCount = cascadeCount;
					allocation.strength = candidates[casterIndex].shadowStrength;
					const Vec4 towardLight(-candidates[casterIndex].direction, 0.0f);
					const f32 marginTexels = GraphicsInternal::ShadowReceiverMarginTexels(
						state.shadowPcfTaps, candidates[casterIndex].shadowNormalBias);
					for (u32 i = 0; i < cascadeCount; ++i) {
						const GraphicsInternal::CascadeView& cascade = state.cascadeViews[i];
						volumes.push_back(GraphicsInternal::BuildCasterVolume(
							cascade.viewProjection, fitView, fitProjection, GraphicsInternal::CascadeReceiverDepths(splits, i),
							towardLight, cascade.worldTexelSize * marginTexels));
						volumes.back().layerMask = candidates[casterIndex].shadowLayerMask;
						++mapCount;
					}
				}
			}

			const f32 objectReach = cascadeCount > 0 && kind == SceneKind::World ? splits.depths[splits.count] : 0.0f;
			const u32 objectPicks = PickObjectShadows(state, cameraPosition, onlyScenes, objectReach,
													  cascadeCount > 0 ? candidates[casterIndex].shadowLayerMask & renderingLayerMask : 0u);

			Vector<GraphicsInternal::ShadowRequest>& requests = state.shadowRequests;
			requests.clear();
			for (u32 i = orderedDirectional; i < candidateCount; ++i) {
				const GraphicsInternal::LightCandidate& candidate = candidates[i];
				if (!candidate.castShadows) {
					continue;
				}
				GraphicsInternal::ShadowRequest& request = requests.emplace_back();
				request.stableKey = candidate.stableKey;
				request.position = candidate.position;
				request.direction = candidate.direction;
				request.range = candidate.range;
				request.outerAngleDeg = candidate.outerAngleDeg;
				request.type = candidate.type;
				request.strength = candidate.shadowStrength;
				request.depthBias = candidate.shadowBias;
				request.normalBias = candidate.shadowNormalBias;
				request.nearPlane = candidate.shadowNearPlane;
				request.casterBias = candidate.shadowCasterBias;
				request.layerMask = candidate.shadowLayerMask;
				const f32 coverage = GraphicsInternal::ShadowScreenCoverage(projection, cameraPosition,
																			candidate.position, candidate.range);
				request.importance = GraphicsInternal::ShadowImportance(coverage, candidate.color);
				request.tileSize = GraphicsInternal::PunctualTileSize(coverage, state.PunctualAtlasSize(), request.MapCount());
			}
			RenderState::ShadowKeep& keep = state.ShadowKeepFor(owner);
			GraphicsInternal::FavorKeptShadows({requests.data(), requests.size()}, {keep.lights.data(), keep.lights.size()});
			const u32 punctualAtlasSize = state.PunctualAtlasSize();
			const u32 objectTile = objectPicks > 0 ? GraphicsInternal::ObjectShadowTileSize(punctualAtlasSize, state.objectShadowBudget,
																							state.objectShadowResolution)
												   : 0u;
			GraphicsInternal::FitShadowRequests({requests.data(), requests.size()}, punctualAtlasSize,
												GraphicsInternal::MAX_SHADOW_MAPS - mapCount - objectPicks,
												static_cast<u64>(objectPicks) * objectTile * objectTile);
			keep.lights.clear();
			keep.frame = state.renderedFrames;

			state.punctualAllocator.Reset(punctualAtlasSize);
			state.punctualViews.clear();
			RectI punctualTiles[GraphicsInternal::MAX_SHADOW_MAPS];
			Vec2 punctualBias[GraphicsInternal::MAX_SHADOW_MAPS];
			Vec2 casterBias[GraphicsInternal::MAX_SHADOW_MAPS];
			for (u32 i = 0; i < mapCount; ++i) {
				casterBias[i] = candidates[casterIndex].shadowCasterBias;
			}
			const u32 firstPunctualMap = mapCount;
			u32 objectFirst = 0;
			u32 objectCount = 0;
			bool objectsPlaced = objectPicks == 0;
			auto placeObjectMaps = [&] {
				objectsPlaced = true;
				objectFirst = mapCount;
				const GraphicsInternal::LightCandidate& sun = candidates[casterIndex];
				const f32 marginTexels = GraphicsInternal::ShadowReceiverMarginTexels(state.shadowPcfTaps, sun.shadowNormalBias);
				for (u32 p = 0; p < objectPicks; ++p) {
					const RectI tile = state.punctualAllocator.Allocate(objectTile);
					if (tile.size.x <= 0) {
						break;
					}
					const RenderState::ObjectShadowPick& pick = state.objectShadowPicks[p];
					f32& radius = state.objectShadows[pick.record].radius;
					radius = GraphicsInternal::ObjectShadowRadius(pick.bounds.Extents().Length(), radius);
					const GraphicsInternal::CascadeView fitted = GraphicsInternal::BuildObjectShadowView(
						sun.direction, pick.bounds.Center(), radius, marginTexels, 2.0f * objectReach, static_cast<u32>(tile.size.x));
					punctualTiles[mapCount] = tile;
					punctualBias[mapCount] = Vec2(sun.shadowBias, sun.shadowNormalBias + pick.normalOffset / fitted.worldTexelSize);
					casterBias[mapCount] = sun.shadowCasterBias;
					// absolute texel size (not per unit distance)
					state.punctualViews.push_back({fitted.viewProjection, fitted.view, fitted.projection, fitted.worldTexelSize});
					volumes.push_back(GraphicsInternal::CullVolume(Frustum::FromMatrix(fitted.viewProjection)));
					volumes.back().layerMask = sun.shadowLayerMask;
					for (u32 m = 0; m < pick.memberCount; ++m) {
						state.objectShadowMembers[state.objectShadowMemberIds[pick.firstMember + m]] = objectCount;
					}
					++objectCount;
					++mapCount;
				}
			};
			const Vec3 forward(view(2, 0), view(2, 1), view(2, 2));
			for (GraphicsInternal::ShadowRequest& request : requests) {
				if (!objectsPlaced && request.tileSize < objectTile) {
					placeObjectMaps();
				}
				++state.frameShadowLights;
				const u32 wanted = request.MapCount();
				if (request.tileSize == 0) {
					++state.frameShadowLightsDropped;
					continue;
				}
				RectI granted[GraphicsInternal::POINT_SHADOW_FACES];
				bool complete = true;
				for (u32 face = 0; face < wanted; ++face) {
					granted[face] = state.punctualAllocator.Allocate(request.tileSize);
					if (granted[face].size.x <= 0) {
						complete = false;
						break;
					}
				}
				if (!complete) {
					++state.frameShadowLightsDropped;
					continue;
				}
				const f32 lightDepth = Vec3::Dot(forward, request.position) + view(2, 3);
				const f32 depthReach = request.range * forward.Length();
				const f32 marginTexels = GraphicsInternal::ShadowReceiverMarginTexels(state.shadowPcfTaps, request.normalBias);
				for (u32 face = 0; face < wanted; ++face) {
					const GraphicsInternal::PunctualView built =
						request.type == LightType::Point
							? GraphicsInternal::BuildPointFaceView(request.position, face, request.range,
																   static_cast<u32>(granted[face].size.x), state.shadowPcfTaps,
																   request.nearPlane)
							: GraphicsInternal::BuildSpotView(request.position, request.direction, request.outerAngleDeg,
															  request.range, static_cast<u32>(granted[face].size.x), request.nearPlane);
					punctualTiles[mapCount] = granted[face];
					punctualBias[mapCount] = Vec2(request.depthBias, request.normalBias);
					casterBias[mapCount] = request.casterBias;
					state.punctualViews.push_back(built);
					volumes.push_back(GraphicsInternal::BuildCasterVolume(
						built.viewProjection, view, projection, Vec2(lightDepth - depthReach, lightDepth + depthReach),
						Vec4(request.position, 1.0f), built.worldTexelSizePerUnit * request.range * marginTexels));
					volumes.back().layerMask = request.layerMask;
					++mapCount;
				}
				RenderState::ShadowAllocation& allocation = state.shadowAllocations.emplace_back();
				allocation.lightKey = request.stableKey;
				allocation.firstMap = mapCount - wanted;
				allocation.mapCount = wanted;
				allocation.strength = request.strength;
				keep.lights.push_back(request.stableKey);
			}
			if (!objectsPlaced) {
				placeObjectMaps();
			}
			if (objectCount > 0) {
				state.shadowAllocations.front().objectFirst = objectFirst;
				state.shadowAllocations.front().objectCount = objectCount;
			}
			std::sort(keep.lights.begin(), keep.lights.end());

			if (mapCount == 0) {
				return 0;
			}

			CasterSplit split;
			if (objectCount > 0) {
				split.members = &state.objectShadowMembers;
				split.cascadeBits = (1ull << cascadeCount) - 1;
				split.objectBits = ((1ull << objectCount) - 1) << objectFirst;
				split.firstObject = objectFirst;
			}
			const ExtractedDraws extracted =
				ExtractDrawList(arena, DrawListUse::Casters, kind, onlyScenes, {volumes.data(), mapCount}, cameraPosition, projection,
								renderingLayerMask, nullptr, split.members ? &split : nullptr);

			RHI::TransientAllocation mapAllocation =
				device.AllocateTransient(static_cast<u64>(mapCount) * sizeof(GPUShadowMap), 16);
			if (!mapAllocation.IsValid()) {
				return 0;
			}
			GPUShadowMap* records = static_cast<GPUShadowMap*>(mapAllocation.cpu);

			auto writeRecord = [](GPUShadowMap& record, const Mat4& viewProjection, const RectI& tile,
								  u32 size, f32 texel, f32 farDepth, f32 depthBias, f32 normalBias) {
				record = GPUShadowMap{};
				record.worldToAtlas = GraphicsInternal::CascadeAtlasMatrix(viewProjection, tile, size);
				const f32 inset = 0.5f / static_cast<f32>(size);
				record.uvBounds = Vec4(static_cast<f32>(tile.Left()) / static_cast<f32>(size) + inset,
									   static_cast<f32>(tile.Top()) / static_cast<f32>(size) + inset,
									   static_cast<f32>(tile.Right()) / static_cast<f32>(size) - inset,
									   static_cast<f32>(tile.Bottom()) / static_cast<f32>(size) - inset);
				record.farDepth = farDepth;
				record.worldTexelSize = texel;
				record.depthBias = depthBias;
				record.normalBias = normalBias;
			};

			const GraphicsInternal::LightCandidate* caster =
				casterIndex < candidateCount ? &candidates[casterIndex] : nullptr;
			for (u32 i = 0; i < cascadeCount; ++i) {
				writeRecord(records[i], state.cascadeViews[i].viewProjection,
							GraphicsInternal::CascadeAtlasRect(i, atlasSize, cascadeCount), atlasSize,
							state.cascadeViews[i].worldTexelSize, state.cascadeViews[i].farDepth,
							caster->shadowBias, caster->shadowNormalBias);
			}
			for (u32 i = 0; i < state.punctualViews.size(); ++i) {
				const u32 slot = firstPunctualMap + i;
				writeRecord(records[slot], state.punctualViews[i].viewProjection, punctualTiles[slot],
							punctualAtlasSize, state.punctualViews[i].worldTexelSizePerUnit, 0.0f,
							punctualBias[slot].x, punctualBias[slot].y);
			}

			u32 drawCalls = 0;
			const u64 casterViewData = WriteViewData(device, view, projection, Vec2I(static_cast<i32>(atlasSize), static_cast<i32>(atlasSize)));
			auto drawMap = [&](u32 map, const RectI& tile, u32 samples) {
				const bool cascade = map < cascadeCount;
				const Mat4& mapViewProjection = cascade ? state.cascadeViews[map].viewProjection
														: state.punctualViews[map - firstPunctualMap].viewProjection;
				cmd.SetViewportScissor(tile);

				RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
				if (!viewAllocation.IsValid()) {
					return false;
				}
				GPUViewUniforms casterUniforms;
				casterUniforms.viewProjection = mapViewProjection;
				casterUniforms.cameraPosition = cameraPosition;
				casterUniforms.viewDataAddress = casterViewData;
				std::memcpy(viewAllocation.cpu, &casterUniforms, sizeof(GPUViewUniforms));

				DepthDrawBinding bound;
				const u64 mapBit = 1ull << map;
				for (u32 i = 0; i < extracted.count;) {
					DrawEntry& entry = *extracted.list[i];
					if ((entry.volumeMask & mapBit) == 0 || entry.alpha) {
						++i;
						continue;
					}

					const bool twoSided = entry.castShadows == ShadowCasting::TwoSided;
					RHI::ShaderRenderState casterState = entry.renderState;
					casterState.blend = RHI::BlendMode::Opaque;
					casterState.depth = RHI::DepthMode::TestWrite;
					casterState.depthCompare = RHI::DepthCompare::NearerOrEqual;
					casterState.fill = RHI::FillMode::Solid;
					ApplyCasterBias(casterState, casterBias[map]);
					if (twoSided) {
						casterState.cull = RHI::CullMode::Off;
					}
					const u32 drawn = RecordDepthDraw(
						cmd, device, bound, entry, casterState, RHI::Format::Unknown, SHADOW_ATLAS_FORMAT, viewAllocation,
						extracted.perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw), 0, [&] {
							return InstanceRunLength(extracted.list, i, extracted.count, [&](const DrawEntry& next) {
								return (next.volumeMask & mapBit) != 0 && (next.castShadows == ShadowCasting::TwoSided) == twoSided;
							});
						},
						samples);
					if (drawn == 0) {
						++i;
						continue;
					}
					++drawCalls;
					state.frameDrawCallsSaved += drawn - 1;
					outTriangles += entry.submesh->indexCount / 3 * drawn;
					i += drawn;
				}
				if (!casters.empty() && (map < objectFirst || map >= objectFirst + objectCount)) {
					ProceduralDepthView itemView;
					itemView.pass = ProceduralPass::Shadow;
					itemView.view = cascade ? state.cascadeViews[map].view : state.punctualViews[map - firstPunctualMap].view;
					itemView.projection = cascade ? state.cascadeViews[map].projection : state.punctualViews[map - firstPunctualMap].projection;
					itemView.extent = tile.size;
					itemView.depthFormat = SHADOW_ATLAS_FORMAT;
					itemView.viewBlock = viewAllocation;
					itemView.casterBias = casterBias[map];
					itemView.samples = samples;
					drawCalls += RecordProceduralDepth(cmd, device, casters, itemView, &volumes[map]);
				}
				++state.frameShadowMapsRendered;
				return true;
			};
			auto drawMaps = [&](RHI::TextureHandle target, u32 first, u32 count) {
				if (count == 0) {
					return;
				}
				RHI::RenderPassDesc::DepthAttachment depthAttachment;
				depthAttachment.texture = target;
				depthAttachment.load = RHI::LoadOp::Clear;
				depthAttachment.clearDepth = 0.0f;
				RHI::RenderPassDesc atlasPass;
				atlasPass.depth = &depthAttachment;
				cmd.BeginRenderPass(atlasPass);
				for (u32 map = first; map < first + count; ++map) {
					const RectI tile = map < cascadeCount ? GraphicsInternal::CascadeAtlasRect(map, atlasSize, cascadeCount) : punctualTiles[map];
					if (!drawMap(map, tile, 1)) {
						break;
					}
				}
				cmd.EndRenderPass();
			};
			const bool moments = state.MomentsActive() && atlas == state.momentAtlas;
			if (moments && cascadeCount > 0) {
				const Vec2I depthSize = device.GetTextureSize(state.momentDepth);
				for (u32 map = 0; map < cascadeCount; ++map) {
					RHI::RenderPassDesc::DepthAttachment depthAttachment;
					depthAttachment.texture = state.momentDepth;
					depthAttachment.load = RHI::LoadOp::Clear;
					depthAttachment.clearDepth = 0.0f;
					RHI::RenderPassDesc samplesPass;
					samplesPass.depth = &depthAttachment;
					cmd.TransitionTexture(state.momentDepth, RHI::ResourceState::DepthWrite);
					cmd.BeginRenderPass(samplesPass);
					const bool drawn = drawMap(map, RectI(0, 0, depthSize.x, depthSize.y), state.momentSamples);
					cmd.EndRenderPass();
					if (!drawn) {
						break;
					}
					cmd.TransitionTexture(state.momentDepth, RHI::ResourceState::ShaderResource);
					if (map > 0) {
						cmd.TransitionTexture(atlas, RHI::ResourceState::ShaderResource);
					}
					ResolveMoments(cmd, device, atlas, GraphicsInternal::CascadeAtlasRect(map, atlasSize, cascadeCount));
				}
				cmd.TransitionTexture(state.momentDepth, RHI::ResourceState::DepthWrite);
			} else if (!moments) {
				drawMaps(atlas, 0, cascadeCount);
			}
			drawMaps(punctualAtlas, firstPunctualMap, mapCount - firstPunctualMap);

			RenderState::ShadowSetup& setup = state.pendingShadow;
			setup.owner = owner;
			setup.mapCount = mapCount;
			setup.cascadeCount = cascadeCount;
			setup.mapAddress = device.GetBufferDeviceAddress(mapAllocation.buffer) + mapAllocation.offset;
			setup.fadeEnd = cascadeCount > 0 ? splits.depths[splits.count] : 0.0f;
			setup.fadeStart = setup.fadeEnd * 0.9f;
			return drawCalls;
		}

		u32 RecordDepthNormalsPass(RHI::CommandList& cmd, LinearAllocator& arena,
								   const Mat4& view, const Mat4& projection, const Vec3& cameraPosition,
								   RHI::TextureHandle normals, RHI::TextureHandle depth, Vec2I extent,
								   u32& outTriangles, std::span<const u64> onlyScenes = {},
								   SceneKind kind = SceneKind::World, std::span<const u32> procedural = {},
								   u32 renderingLayerMask = ~0u) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			const Mat4 viewProjection = projection * view;
			const GraphicsInternal::CullVolume frustum(Frustum::FromMatrix(viewProjection));
			const ExtractedDraws extracted =
				ExtractDrawList(arena, DrawListUse::View, kind, onlyScenes, {&frustum, 1}, cameraPosition, projection, renderingLayerMask);

			RHI::RenderPassDesc::Attachment normalAttachment;
			normalAttachment.texture = normals;
			normalAttachment.load = RHI::LoadOp::Clear;
			normalAttachment.clearColor = Color(0.5f, 0.5f, 1.0f, 1.0f);
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = depth;
			depthAttachment.load = RHI::LoadOp::Clear;
			depthAttachment.clearDepth = 0.0f;

			RHI::RenderPassDesc desc;
			const bool hasNormals = normals.IsValid();
			if (hasNormals) {
				desc.colorAttachments = {&normalAttachment, 1};
			}
			desc.depth = &depthAttachment;
			cmd.BeginRenderPass(desc);
			cmd.SetViewportScissor(RectI{0, 0, extent.x, extent.y});

			RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
			if (!viewAllocation.IsValid()) {
				cmd.EndRenderPass();
				return 0;
			}
			GPUViewUniforms prepassUniforms;
			prepassUniforms.viewProjection = viewProjection;
			prepassUniforms.cameraPosition = cameraPosition;
			prepassUniforms.viewDataAddress = WriteViewData(device, view, projection, extent);
			std::memcpy(viewAllocation.cpu, &prepassUniforms, sizeof(GPUViewUniforms));

			u32 drawCalls = 0;
			DepthDrawBinding bound;
			for (u32 i = 0; i < extracted.count;) {
				DrawEntry& entry = *extracted.list[i];
				if (entry.alpha) {
					++i;
					continue;
				}
				RHI::ShaderRenderState prepassState = entry.renderState;
				prepassState.blend = RHI::BlendMode::Opaque;
				prepassState.depth = RHI::DepthMode::TestWrite;
				prepassState.depthCompare = RHI::DepthCompare::NearerOrEqual;
				prepassState.fill = RHI::FillMode::Solid;
				const u32 drawn = RecordDepthDraw(
					cmd, device, bound, entry, prepassState, hasNormals ? SCENE_NORMALS_FORMAT : RHI::Format::Unknown, SCENE_DEPTH_FORMAT,
					viewAllocation, extracted.perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw), 0, [&] {
						return InstanceRunLength(extracted.list, i, extracted.count, [](const DrawEntry&) { return true; });
					});
				if (drawn == 0) {
					++i;
					continue;
				}
				++drawCalls;
				state.frameDrawCallsSaved += drawn - 1;
				outTriangles += entry.submesh->indexCount / 3 * drawn;
				i += drawn;
			}
			if (!procedural.empty()) {
				ProceduralDepthView itemView;
				itemView.pass = ProceduralPass::Prepass;
				itemView.view = view;
				itemView.projection = projection;
				itemView.extent = extent;
				itemView.colorFormat = hasNormals ? SCENE_NORMALS_FORMAT : RHI::Format::Unknown;
				itemView.depthFormat = SCENE_DEPTH_FORMAT;
				itemView.viewBlock = viewAllocation;
				drawCalls += RecordProceduralDepth(cmd, device, procedural, itemView, nullptr);
			}
			cmd.EndRenderPass();
			return drawCalls;
		}

		u32 RecordOutlineMask(RHI::CommandList& cmd, LinearAllocator& arena, const RenderState::PendingView& pending,
							  RHI::TextureHandle mask, RHI::TextureHandle depth, Vec2I extent, u32& outTriangles) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			const Mat4 viewProjection = pending.projection * pending.view;
			const GraphicsInternal::CullVolume frustum(Frustum::FromMatrix(viewProjection));
			const ExtractedDraws extracted =
				ExtractDrawList(arena, DrawListUse::View, pending.kind, {state.viewScenes.data() + pending.firstScene, pending.sceneCount}, {&frustum, 1},
								pending.cameraPosition, pending.projection, pending.cullingMask);

			RHI::RenderPassDesc::Attachment maskAttachment;
			maskAttachment.texture = mask;
			maskAttachment.load = RHI::LoadOp::Clear;
			maskAttachment.clearColor = Color(0.0f, 0.0f, 0.0f, 0.0f);
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = depth;
			depthAttachment.load = RHI::LoadOp::Clear;
			depthAttachment.clearDepth = 0.0f;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&maskAttachment, 1};
			desc.depth = &depthAttachment;
			cmd.BeginRenderPass(desc);
			cmd.SetViewportScissor(RectI{0, 0, extent.x, extent.y});

			RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
			if (!viewAllocation.IsValid()) {
				cmd.EndRenderPass();
				return 0;
			}
			GPUViewUniforms maskUniforms;
			maskUniforms.viewProjection = viewProjection;
			maskUniforms.cameraPosition = pending.cameraPosition;
			maskUniforms.viewDataAddress = WriteViewData(device, pending.view, pending.projection, extent);
			std::memcpy(viewAllocation.cpu, &maskUniforms, sizeof(GPUViewUniforms));

			const std::span<const RenderState::OutlineEntry> outlines{state.viewOutlines.data() + pending.firstOutline, pending.outlineCount};
			u32 drawCalls = 0;
			DepthDrawBinding bound;
			for (u32 i = 0; i < extracted.count; ++i) {
				DrawEntry& entry = *extracted.list[i];
				const u64 object = entry.object.GetInstanceId();
				const auto found = std::lower_bound(outlines.begin(), outlines.end(), object,
													[](const RenderState::OutlineEntry& outline, u64 id) { return outline.object < id; });
				if (found == outlines.end() || found->object != object) {
					continue;
				}
				const u32 value = found->value;
				RHI::ShaderRenderState maskState = entry.renderState;
				maskState.blend = RHI::BlendMode::Opaque;
				maskState.depth = RHI::DepthMode::TestWrite;
				maskState.depthCompare = RHI::DepthCompare::NearerOrEqual;
				maskState.fill = RHI::FillMode::Solid;
				maskState.cull = RHI::CullMode::Off;
				if (!RecordDepthDraw(cmd, device, bound, entry, maskState, VALUE_FORMAT, SCENE_DEPTH_FORMAT, viewAllocation,
									 extracted.perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw), value)) {
					continue;
				}
				++drawCalls;
				outTriangles += entry.submesh->indexCount / 3;
			}
			const std::span<const u32> procedural{state.proceduralVisible.data() + pending.firstProcedural, pending.proceduralCount};
			if (u32* values = procedural.empty() ? nullptr : arena.AllocateAs<u32>(procedural.size())) {
				for (size_t at = 0; at < procedural.size(); ++at) {
					values[at] = 0;
					const ProceduralRenderer* renderer = state.procedural.renderers[procedural[at]].renderer.Get();
					const u64 object = renderer ? renderer->GetOwner().GetInstanceId() : 0;
					const auto found = std::lower_bound(outlines.begin(), outlines.end(), object,
														[](const RenderState::OutlineEntry& outline, u64 id) { return outline.object < id; });
					if (object != 0 && found != outlines.end() && found->object == object) {
						values[at] = found->value;
					}
				}
				ProceduralDepthView itemView;
				itemView.pass = ProceduralPass::Value;
				itemView.view = pending.view;
				itemView.projection = pending.projection;
				itemView.extent = extent;
				itemView.colorFormat = VALUE_FORMAT;
				itemView.depthFormat = SCENE_DEPTH_FORMAT;
				itemView.viewBlock = viewAllocation;
				drawCalls += RecordProceduralDepth(cmd, device, procedural, itemView, nullptr, {values, procedural.size()});
			}
			cmd.EndRenderPass();
			return drawCalls;
		}

		u32 ClaimPickSlot(RenderState& state, RHI::Device& device, const RenderState::PendingView& pending) {
			if (!state.pickRing.IsValid()) {
				RHI::BufferDesc desc;
				desc.size = RenderState::PICK_SLOTS * sizeof(u32);
				desc.usage = RHI::BufferUsage::TransferDst;
				desc.access = RHI::MemoryAccess::Readback;
				desc.debugName = "PickRing";
				state.pickRing = device.CreateBufferResource(desc);
				if (!state.pickRing.IsValid()) {
					return RenderState::PICK_SLOTS;
				}
			}
			for (u32 slot = 0; slot < RenderState::PICK_SLOTS; ++slot) {
				RenderState::PickSlot& claimed = state.pickSlots[slot];
				if (!claimed.inUse) {
					claimed.inUse = true;
					claimed.frame = state.renderedFrames;
					claimed.targetId = pending.targetId;
					claimed.tag = pending.pick.tag;
					claimed.objects.clear();
					return slot;
				}
			}
			SLT_LOG_WARN("Graphics", "a view's pick wasn't asked (all {} pick slots are in flight)", RenderState::PICK_SLOTS);
			return RenderState::PICK_SLOTS;
		}

		u32 ClaimDepthRangeSlot(RenderState& state, RHI::Device& device, u64 view) {
			if (!state.depthRangeRing.IsValid()) {
				RHI::BufferDesc desc;
				desc.size = RenderState::DEPTH_RANGE_SLOTS * 2 * sizeof(f32);
				desc.usage = RHI::BufferUsage::TransferDst;
				desc.access = RHI::MemoryAccess::Readback;
				desc.debugName = "DepthRangeRing";
				state.depthRangeRing = device.CreateBufferResource(desc);
				desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst | RHI::BufferUsage::TransferSrc;
				desc.access = RHI::MemoryAccess::GPUOnly;
				desc.debugName = "DepthRangeScratch";
				state.depthRangeScratch = device.CreateBufferResource(desc);
				desc.size = 2 * sizeof(u32);
				desc.usage = RHI::BufferUsage::TransferSrc;
				desc.access = RHI::MemoryAccess::CPUToGPU;
				desc.debugName = "DepthRangeSeed";
				state.depthRangeSeed = device.CreateBufferResource(desc);
				if (auto* seed = static_cast<u32*>(state.depthRangeSeed.IsValid() ? device.GetMappedPointer(state.depthRangeSeed) : nullptr)) {
					seed[0] = 0x7F7FFFFFu;
					seed[1] = 0u;
				}
			}
			if (!state.depthRangeRing.IsValid() || !state.depthRangeScratch.IsValid() || !state.depthRangeSeed.IsValid()) {
				return RenderState::DEPTH_RANGE_SLOTS;
			}
			for (u32 slot = 0; slot < RenderState::DEPTH_RANGE_SLOTS; ++slot) {
				RenderState::DepthRangeSlot& claimed = state.depthRangeSlots[slot];
				if (!claimed.inUse) {
					claimed = {true, state.renderedFrames, view};
					return slot;
				}
			}
			return RenderState::DEPTH_RANGE_SLOTS;
		}

		void HarvestDepthRanges(RenderState& state, RHI::Device& device) {
			if (!state.depthRangeRing.IsValid()) {
				return;
			}
			const auto* mapped = static_cast<const f32*>(device.GetMappedPointer(state.depthRangeRing));
			for (;;) {
				u32 oldest = RenderState::DEPTH_RANGE_SLOTS;
				for (u32 slot = 0; slot < RenderState::DEPTH_RANGE_SLOTS; ++slot) {
					const RenderState::DepthRangeSlot& flight = state.depthRangeSlots[slot];
					if (flight.inUse && flight.frame + RHI::FRAMES_IN_FLIGHT <= state.renderedFrames &&
						(oldest == RenderState::DEPTH_RANGE_SLOTS || flight.frame < state.depthRangeSlots[oldest].frame)) {
						oldest = slot;
					}
				}
				if (oldest == RenderState::DEPTH_RANGE_SLOTS) {
					break;
				}
				RenderState::DepthRangeSlot& flight = state.depthRangeSlots[oldest];
				if (mapped) {
					GraphicsInternal::CascadeDepthFit& fit = state.depthFits[flight.view];
					GraphicsInternal::UpdateCascadeDepthFit(fit, mapped[oldest * 2 + 1]);
					fit.seen = state.renderedFrames;
				}
				flight.inUse = false;
			}
			if ((state.renderedFrames & 63) == 0) {
				std::erase_if(state.depthFits, [&](const auto& entry) { return entry.second.seen + 120 < state.renderedFrames; });
			}
		}

		void AnswerFinishedPicks(RenderState& state, RHI::Device& device) {
			if (!state.pickRing.IsValid()) {
				return;
			}
			const auto* mapped = static_cast<const u32*>(device.GetMappedPointer(state.pickRing));
			while (true) {
				u32 slot = RenderState::PICK_SLOTS;
				for (u32 candidate = 0; candidate < RenderState::PICK_SLOTS; ++candidate) {
					const RenderState::PickSlot& flight = state.pickSlots[candidate];
					if (flight.inUse && flight.frame + RHI::FRAMES_IN_FLIGHT <= state.renderedFrames &&
						(slot == RenderState::PICK_SLOTS || flight.frame < state.pickSlots[slot].frame)) {
						slot = candidate;
					}
				}
				if (slot == RenderState::PICK_SLOTS) {
					break;
				}
				RenderState::PickSlot& flight = state.pickSlots[slot];
				auto targetIt = state.viewTargets.find(flight.targetId);
				if (targetIt != state.viewTargets.end() && mapped) {
					const u32 value = mapped[slot];
					PickAnswer answer;
					answer.tag = flight.tag;
					if (value > 0 && value <= flight.objects.size()) {
						answer.object = flight.objects[value - 1];
					}
					targetIt->second.pickAnswers.push_back(answer);
				}
				flight.inUse = false;
				flight.objects.clear();
			}
		}

		// clip y +1 = row 0
		Mat4 PickProjection(const Mat4& projection, Vec2I pixel, Vec2I extent) {
			const f32 width = static_cast<f32>(extent.x);
			const f32 height = static_cast<f32>(extent.y);
			const f32 ndcX = 2.0f * (static_cast<f32>(pixel.x) + 0.5f) / width - 1.0f;
			const f32 ndcY = 1.0f - 2.0f * (static_cast<f32>(pixel.y) + 0.5f) / height;
			Mat4 pick = Mat4::Identity();
			pick(0, 0) = width;
			pick(0, 3) = -width * ndcX;
			pick(1, 1) = height;
			pick(1, 3) = -height * ndcY;
			return pick * projection;
		}

		// each draw writes its extraction idx + 1
		u32 RecordPick(RHI::CommandList& cmd, LinearAllocator& arena, const RenderState::PendingView& pending, RHI::TextureHandle target,
					   RHI::TextureHandle depth, Vec2I pixel, Vec2I extent, Vector<Handle<SceneObject>>& objects, u32& outTriangles) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			objects.clear();
			const Mat4 projection = PickProjection(pending.projection, pixel, extent);
			const Mat4 viewProjection = projection * pending.view;
			const GraphicsInternal::CullVolume frustum(Frustum::FromMatrix(pending.projection * pending.view));
			const ExtractedDraws extracted =
				ExtractDrawList(arena, DrawListUse::View, pending.kind, {state.viewScenes.data() + pending.firstScene, pending.sceneCount}, {&frustum, 1},
								pending.cameraPosition, pending.projection, pending.cullingMask);

			RHI::RenderPassDesc::Attachment valueAttachment;
			valueAttachment.texture = target;
			valueAttachment.load = RHI::LoadOp::Clear;
			valueAttachment.clearColor = Color(0.0f, 0.0f, 0.0f, 0.0f);
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = depth;
			depthAttachment.load = RHI::LoadOp::Clear;
			depthAttachment.clearDepth = 0.0f;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&valueAttachment, 1};
			desc.depth = &depthAttachment;
			cmd.BeginRenderPass(desc);
			cmd.SetViewportScissor(RectI{0, 0, 1, 1});

			RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
			if (!viewAllocation.IsValid()) {
				cmd.EndRenderPass();
				return 0;
			}
			GPUViewUniforms pickUniforms;
			pickUniforms.viewProjection = viewProjection;
			pickUniforms.cameraPosition = pending.cameraPosition;
			pickUniforms.viewDataAddress = WriteViewData(device, pending.view, projection, Vec2I(1, 1));
			std::memcpy(viewAllocation.cpu, &pickUniforms, sizeof(GPUViewUniforms));

			objects.resize(extracted.count);
			u32 drawCalls = 0;
			DepthDrawBinding bound;
			for (u32 i = 0; i < extracted.count; ++i) {
				DrawEntry& entry = *extracted.list[i];
				objects[i] = entry.object;
				RHI::ShaderRenderState pickState = entry.renderState;
				pickState.blend = RHI::BlendMode::Opaque;
				pickState.depth = RHI::DepthMode::TestWrite;
				pickState.depthCompare = RHI::DepthCompare::NearerOrEqual;
				pickState.fill = RHI::FillMode::Solid;
				if (!RecordDepthDraw(cmd, device, bound, entry, pickState, VALUE_FORMAT, SCENE_DEPTH_FORMAT, viewAllocation,
									 extracted.perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw), i + 1)) {
					continue;
				}
				++drawCalls;
				outTriangles += entry.submesh->indexCount / 3;
			}
			const std::span<const u32> procedural{state.proceduralVisible.data() + pending.firstProcedural, pending.proceduralCount};
			if (u32* values = procedural.empty() ? nullptr : arena.AllocateAs<u32>(procedural.size())) {
				for (size_t at = 0; at < procedural.size(); ++at) {
					const ProceduralRenderer* renderer = state.procedural.renderers[procedural[at]].renderer.Get();
					objects.push_back(renderer ? renderer->GetOwner() : Handle<SceneObject>{});
					values[at] = static_cast<u32>(objects.size());
				}
				ProceduralDepthView itemView;
				itemView.pass = ProceduralPass::Value;
				itemView.view = pending.view;
				itemView.projection = projection;
				itemView.extent = Vec2I(1, 1);
				itemView.colorFormat = VALUE_FORMAT;
				itemView.depthFormat = SCENE_DEPTH_FORMAT;
				itemView.viewBlock = viewAllocation;
				drawCalls += RecordProceduralDepth(cmd, device, procedural, itemView, nullptr, {values, procedural.size()});
			}
			cmd.EndRenderPass();
			return drawCalls;
		}

		u32 RecordPrepassItemsOverCopy(RHI::CommandList& cmd, const Mat4& view, const Mat4& projection, const Vec3& cameraPosition,
									   RHI::TextureHandle kept, RHI::TextureHandle depth, RHI::TextureHandle normals, Vec2I extent,
									   std::span<const u32> procedural) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;
			ShaderAsset* copy = state.depthCopyShader.Get();
			RHI::PipelineHandle copyPipeline = copy ? GetOrCreatePipeline(copy, RHI::Format::Unknown, SCENE_DEPTH_FORMAT) : RHI::PipelineHandle{};
			if (!copyPipeline.IsValid()) {
				return 0;
			}
			RHI::RenderPassDesc::DepthAttachment copyAttachment;
			copyAttachment.texture = depth;
			copyAttachment.load = RHI::LoadOp::Clear;
			copyAttachment.clearDepth = 0.0f;
			RHI::RenderPassDesc copyPass;
			copyPass.depth = &copyAttachment;
			cmd.BeginRenderPass(copyPass);
			cmd.SetViewportScissor(RectI{0, 0, extent.x, extent.y});
			cmd.SetPipeline(copyPipeline);
			cmd.BindTransientSet(1, device.CreateTransientSet(copyPipeline, 1).WriteTexture(0, kept).WriteSampler(1, state.clampSampler).Commit());
			cmd.Draw(3);
			cmd.EndRenderPass();
			u32 drawCalls = 1;

			RHI::RenderPassDesc::Attachment normalAttachment;
			normalAttachment.texture = normals;
			normalAttachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = depth;
			depthAttachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc itemPass;
			if (normals.IsValid()) {
				itemPass.colorAttachments = {&normalAttachment, 1};
			}
			itemPass.depth = &depthAttachment;
			cmd.BeginRenderPass(itemPass);
			cmd.SetViewportScissor(RectI{0, 0, extent.x, extent.y});
			RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
			if (viewAllocation.IsValid()) {
				GPUViewUniforms uniforms;
				uniforms.viewProjection = projection * view;
				uniforms.cameraPosition = cameraPosition;
				uniforms.viewDataAddress = WriteViewData(device, view, projection, extent);
				std::memcpy(viewAllocation.cpu, &uniforms, sizeof(GPUViewUniforms));
				ProceduralDepthView itemView;
				itemView.pass = ProceduralPass::Prepass;
				itemView.view = view;
				itemView.projection = projection;
				itemView.extent = extent;
				itemView.colorFormat = normals.IsValid() ? SCENE_NORMALS_FORMAT : RHI::Format::Unknown;
				itemView.depthFormat = SCENE_DEPTH_FORMAT;
				itemView.viewBlock = viewAllocation;
				drawCalls += RecordProceduralDepth(cmd, device, procedural, itemView, nullptr);
			}
			cmd.EndRenderPass();
			return drawCalls;
		}

		struct ProceduralDraw {
			u32 renderer = 0;
			u32 item = 0;
			f32 depth = 0.0f;
			i32 sortingPriority = 0;
			bool alpha = false;
			bool readsColor = false;
		};

		u64 ProceduralSortKey(const ProceduralDraw& draw, u32 place) {
			const u64 blended = draw.alpha ? 1ull : 0ull;
			const u64 priority = static_cast<u64>(Math::Clamp(draw.sortingPriority, -32768, 32767) + 32768);
			const u64 depth = draw.alpha ? static_cast<u64>(~std::bit_cast<u32>(draw.depth) >> 8) : 0ull;
			return (blended << 63) | (priority << 47) | (depth << 23) | (place & 0x7FFFFFu);
		}

		u32 SortProceduralDraws(LinearAllocator& arena, std::span<const u32> visible, const Vec3& cameraPosition,
								ProceduralDraw*& outDraws) {
			const GraphicsInternal::ProceduralFrame& frame = _state->procedural;
			u32 capacity = 0;
			for (u32 index : visible) {
				capacity += frame.renderers[index].itemCount;
			}
			if (capacity == 0) {
				return 0;
			}
			ProceduralDraw* draws = arena.AllocateAs<ProceduralDraw>(capacity);
			u32 count = 0;
			for (u32 index : visible) {
				const GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[index];
				for (u32 item = renderer.firstItem; item < renderer.firstItem + renderer.itemCount; ++item) {
					MaterialAsset* material = frame.items[item].material.Get();
					ShaderAsset* shader = material ? material->shader.Get() : nullptr;
					if (!shader || frame.items[item].castShadows == ShadowCasting::ShadowsOnly) {
						continue;
					}
					if (!DrawsProceduralItem(*shader, *material)) {
						NameProceduralDrawRefusal(*shader, *material);
						continue;
					}
					ProceduralDraw& draw = draws[count++];
					draw = ProceduralDraw{};
					draw.renderer = index;
					draw.item = item;
					draw.alpha = material->ResolveRenderState(shader->renderState).blend != RHI::BlendMode::Opaque;
					draw.readsColor = draw.alpha && HasFlag(SceneRequests::DeclaredBy(*shader), PassRequests::SceneColor);
					draw.sortingPriority = material->sortingPriority;
					draw.depth = Vec3::DistanceSquared(cameraPosition, frame.items[item].sortPoint);
				}
			}
			outDraws = draws;
			if (count < 2) {
				return count;
			}
			u64* keys = arena.AllocateAs<u64>(count);
			ProceduralDraw* sorted = arena.AllocateAs<ProceduralDraw>(count);
			for (u32 place = 0; place < count; ++place) {
				keys[place] = ProceduralSortKey(draws[place], place);
			}
			std::sort(keys, keys + count);
			for (u32 i = 0; i < count; ++i) {
				sorted[i] = draws[keys[i] & 0x7FFFFFu];
			}
			outDraws = sorted;
			return count;
		}

		bool ProceduralSortsBefore(const ProceduralDraw& draw, const DrawEntry& entry) {
			if (draw.sortingPriority != entry.sortingPriority) {
				return draw.sortingPriority < entry.sortingPriority;
			}
			return draw.depth > entry.depth;
		}

		// states set by caller
		u32 RecordSceneColorCopy(RHI::CommandList& cmd, RHI::Device& device, RHI::TextureHandle source, RHI::TextureHandle destination,
								 bool resolve) {
			if (resolve) {
				cmd.ResolveTexture(source, destination);
				return 0;
			}
			ShaderAsset* blit = _state->blitShader.Get();
			RHI::PipelineHandle pipeline = blit ? GetOrCreatePipeline(blit, SCENE_COLOR_FORMAT, RHI::Format::Unknown) : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return 0;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = destination;
			attachment.load = RHI::LoadOp::DontCare;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			cmd.BeginRenderPass(desc);
			cmd.SetPipeline(pipeline);
			RHI::TransientSet set = device.CreateTransientSet(pipeline, 1).WriteTexture(0, source).WriteSampler(1, _state->clampSampler).Commit();
			cmd.BindTransientSet(1, set);
			cmd.Draw(3);
			cmd.EndRenderPass();
			return 1;
		}

		struct ScenePassReads {
			RHI::TextureHandle color;
			RHI::TextureHandle depth;
			RHI::TextureHandle normals;
			bool copiesColor = false;
		};

		bool TransparentReadsColor(DrawEntry* const* drawList, u32 first, u32 count, const ProceduralDraw* proceduralDraws, u32 firstProcedural,
								   u32 proceduralCount) {
			for (u32 i = first; i < count; ++i) {
				if (HasFlag(drawList[i]->sceneReads, PassRequests::SceneColor)) {
					return true;
				}
			}
			for (u32 i = firstProcedural; i < proceduralCount; ++i) {
				if (proceduralDraws[i].readsColor) {
					return true;
				}
			}
			return false;
		}

		u32 RecordScenePass(RHI::CommandList& cmd, LinearAllocator& arena,
							const Mat4& view, const Mat4& projection, const Vec3& cameraPosition,
							const Color& clearColor, const PassTargets& targets, u32& outTriangles,
							std::span<const u64> onlyScenes = {},
							ViewFill fill = ViewFill::Solid, Color32 wireColor = Color32::White(),
							SceneKind kind = SceneKind::World, u64 shadowOwner = 0,
							u32 renderingLayerMask = ~0u, MaterialAsset* overrideMaterial = nullptr,
							RHI::TextureHandle ambientOcclusion = {},
							RHI::TransientAllocation* outViewAllocation = nullptr,
							std::span<const u32> procedural = {},
							const ScenePassReads& reads = {},
							bool reflectionProbes = true) {
			RenderState& state = *_state;
			RHI::Device& device = *state.device;

			SceneRequestTextures bound{reads.copiesColor ? RHI::TextureHandle{} : reads.color, reads.depth, reads.normals, {}, {}, {}};
			const PassView passView{view, projection, targets.extent};

			const bool isolated = kind != SceneKind::World;
			Mat4 viewProjection = projection * view;
			const GraphicsInternal::CullVolume frustum(Frustum::FromMatrix(viewProjection));

			u32 directionalCount = 0;
			u32 droppedLights = 0;
			const u32 lightCount = ExtractLightCandidates(view, projection, cameraPosition, kind, onlyScenes,
														  directionalCount, droppedLights);
			Vector<GraphicsInternal::LightCandidate>& candidates = state.lightCandidates;
			state.frameLightsBinned += lightCount;
			state.frameLightsDropped += droppedLights;

			Vec3 sunDirection{0.0f, 1.0f, 0.0f};
			Vec3 sunColor{};
			if (directionalCount > 0 && !candidates.empty()) {
				sunDirection = -candidates[0].direction;
				sunColor = candidates[0].color;
			}
			const RenderState::BlendedEnvironment environment = ResolveEnvironment(kind, onlyScenes, cameraPosition, sunColor);
			if (directionalCount > 0 && !candidates.empty()) {
				candidates[0].color = candidates[0].color * environment.sunTint;
				sunColor = candidates[0].color;
			}

			GPUViewUniforms viewUniforms;
			viewUniforms.viewProjection = viewProjection;
			viewUniforms.cameraPosition = cameraPosition;
			viewUniforms.ambientColor = DefaultAmbient;
			viewUniforms.viewForwardRow = Vec4(view(2, 0), view(2, 1), view(2, 2), view(2, 3));
			viewUniforms.directionalLightCount = static_cast<f32>(directionalCount);
			viewUniforms.punctualLightCount = static_cast<f32>(lightCount - directionalCount);

			const bool ownShadow = shadowOwner != 0 && state.pendingShadow.owner == shadowOwner;
			const RenderState::ShadowSetup shadow = ownShadow ? state.pendingShadow : RenderState::ShadowSetup{};
			std::span<const RenderState::ShadowAllocation> allocations =
				ownShadow ? std::span<const RenderState::ShadowAllocation>{state.shadowAllocations.data(),
																		   state.shadowAllocations.size()}
						  : std::span<const RenderState::ShadowAllocation>{};
			state.pendingShadow = RenderState::ShadowSetup{};

			if (lightCount > 0) {
				u32 cookieCount = 0;
				for (u32 i = 0; i < lightCount; ++i) {
					cookieCount += candidates[i].cookie ? 1u : 0u;
				}
				RHI::TransientAllocation lightAllocation =
					device.AllocateTransient(static_cast<u64>(lightCount + cookieCount) * sizeof(GPULight), 16);
				if (lightAllocation.IsValid()) {
					viewUniforms.lightAddress = device.GetBufferDeviceAddress(lightAllocation.buffer) + lightAllocation.offset;
					GPULight* records = static_cast<GPULight*>(lightAllocation.cpu);
					u32 nextExtension = lightCount;
					for (u32 i = 0; i < lightCount; ++i) {
						const GraphicsInternal::LightCandidate& candidate = candidates[i];
						GPULight& record = records[i];
						record = GPULight{};
						record.info = LightInfo(candidate);
						GPULightCookie cookie;
						if (candidate.cookie && PackLightCookie(state, candidate, cookie)) {
							std::memcpy(&records[nextExtension], &cookie, sizeof(cookie));
							record.info |= LIGHT_FLAG_COOKIE | (nextExtension << LIGHT_EXTENSION_SHIFT);
							++nextExtension;
						}
						record.position = candidate.position;
						if (candidate.range > 0.0f) {
							record.invRangeSquared = 1.0f / (candidate.range * candidate.range);
						}
						record.color = candidate.color;
						record.spotScale = candidate.spotScale;
						record.direction = candidate.direction;
						record.spotOffset = candidate.spotOffset;
						for (const RenderState::ShadowAllocation& allocation : allocations) {
							if (allocation.lightKey == candidate.stableKey) {
								record.shadowIndex = allocation.firstMap;
								// directional: bits 0-7 cascades, 8-15 object maps, 16-31 1st object map
								record.shadowCount = allocation.mapCount | allocation.objectCount << 8 | allocation.objectFirst << 16;
								record.shadowStrength = allocation.strength;
								break;
							}
						}
					}
				} else {
					viewUniforms.directionalLightCount = 0.0f;
					viewUniforms.punctualLightCount = 0.0f;
				}
			} else if (RHI::TransientAllocation spare = device.AllocateTransient(sizeof(GPULight), 16); spare.IsValid()) {
				// masked-off light load still executed by damn lavapipe (null address crashes)
				std::memset(spare.cpu, 0, sizeof(GPULight));
				viewUniforms.lightAddress = device.GetBufferDeviceAddress(spare.buffer) + spare.offset;
			}

			const u32 punctualCount = lightCount - directionalCount;
			f32 lightsNearDepth = 0.0f;
			f32 lightsFarDepth = 0.0f;
			const bool haveSpan = punctualCount > 0 && viewUniforms.lightAddress != 0 &&
								  GraphicsInternal::PunctualDepthSpan(
									  view, {candidates.data() + directionalCount, punctualCount},
									  lightsNearDepth, lightsFarDepth);
			if (punctualCount > 0 && !haveSpan) {
				viewUniforms.punctualLightCount = 0.0f;
			}
			ExtractDecals(viewProjection, cameraPosition, kind, onlyScenes);
			const std::span<const GraphicsInternal::DecalBound> decalBounds{state.decalBounds.data(), state.decalBounds.size()};
			f32 decalsNearDepth = 0.0f;
			f32 decalsFarDepth = 0.0f;
			const bool haveDecals = !decalBounds.empty() && GraphicsInternal::DecalDepthSpan(view, decalBounds, decalsNearDepth, decalsFarDepth);
			if (haveSpan || haveDecals) {
				const f32 nearDepth = haveSpan && haveDecals ? Math::Min(lightsNearDepth, decalsNearDepth) : haveSpan ? lightsNearDepth
																													  : decalsNearDepth;
				const f32 farDepth = haveSpan && haveDecals ? Math::Max(lightsFarDepth, decalsFarDepth) : haveSpan ? lightsFarDepth
																												   : decalsFarDepth;
				const GraphicsInternal::ClusterGrid grid = GraphicsInternal::BuildClusterGrid(projection, nearDepth, farDepth);
				state.frameClusterLightsDropped += GraphicsInternal::BinLights(
					grid, view, {candidates.data() + directionalCount, haveSpan ? punctualCount : 0u}, directionalCount,
					state.lightsPerCluster, state.clusterTable, state.clusterIndices, state.clusterKeys);
				const Vector<u32>* header = &state.clusterTable;
				const Vector<u32>* indices = &state.clusterIndices;
				if (haveDecals) {
					GraphicsInternal::BinDecals(grid, view, decalBounds, state.decalsPerCluster, state.decalTable, state.decalIndices);
					GraphicsInternal::MergeClusterLists(state.clusterTable, state.clusterIndices, state.decalTable, state.decalIndices,
														state.mergedClusterHeader, state.mergedClusterIndices);
					header = &state.mergedClusterHeader;
					indices = &state.mergedClusterIndices;
					const u64 decalBytes = static_cast<u64>(state.decalCandidates.size()) * sizeof(GPUDecal);
					if (RHI::TransientAllocation decalAllocation = device.AllocateTransient(decalBytes, 16); decalAllocation.IsValid()) {
						GPUDecal* records = static_cast<GPUDecal*>(decalAllocation.cpu);
						for (size_t i = 0; i < state.decalOrder.size(); ++i) {
							records[i] = state.decalCandidates[static_cast<u32>(state.decalOrder[i])].record;
						}
						viewUniforms.decalAddress = device.GetBufferDeviceAddress(decalAllocation.buffer) + decalAllocation.offset;
					}
				}

				const u64 tableBytes = header->size() * sizeof(u32);
				const u64 indexBytes = indices->size() * sizeof(u32);
				RHI::TransientAllocation clusterAllocation = device.AllocateTransient(tableBytes + indexBytes, 16);
				if (clusterAllocation.IsValid()) {
					viewUniforms.clusterAddress = device.GetBufferDeviceAddress(clusterAllocation.buffer) + clusterAllocation.offset;
					u8* destination = static_cast<u8*>(clusterAllocation.cpu);
					std::memcpy(destination, header->data(), tableBytes);
					if (indexBytes > 0) {
						std::memcpy(destination + tableBytes, indices->data(), indexBytes);
					}
					viewUniforms.clusterDimX = grid.dimX;
					viewUniforms.clusterDimY = grid.dimY;
					viewUniforms.clusterDimZ = grid.dimZ;
					viewUniforms.clusterTileScaleX = static_cast<f32>(grid.dimX) / static_cast<f32>(Math::Max(targets.extent.x, 1));
					viewUniforms.clusterTileScaleY = static_cast<f32>(grid.dimY) / static_cast<f32>(Math::Max(targets.extent.y, 1));
					viewUniforms.clusterSliceScale = grid.sliceScale;
					viewUniforms.clusterSliceBias = grid.sliceBias;
				} else {
					viewUniforms.punctualLightCount = 0.0f;
					viewUniforms.decalAddress = 0;
				}
			}

			viewUniforms.shadowMapAddress = shadow.mapCount > 0 ? shadow.mapAddress : 0;
			viewUniforms.shadowCascadeCount = static_cast<f32>(shadow.cascadeCount);
			viewUniforms.shadowFadeStart = shadow.fadeStart;
			viewUniforms.shadowFadeEnd = shadow.fadeEnd;
			viewUniforms.transparentReceivesShadows = state.transparentReceiveShadows ? 1.0f : 0.0f;
			viewUniforms.shadowAtlasTexel = 1.0f / static_cast<f32>(Math::Max(state.ShadowAtlasSize(), 1u));
			viewUniforms.shadowPcfTaps = static_cast<f32>(state.shadowPcfTaps);
			if (state.MomentsActive() && shadow.cascadeCount > 0) {
				viewUniforms.shadowMoments = Vec4(state.shadowMomentParams, 1.0f);
			}

			// before the view uniforms (its address is one of them)
			const u64 environmentBytes = sizeof(GPUEnvironment) + static_cast<u64>(environment.fogCount) * sizeof(GPUFogLayer);
			if (RHI::TransientAllocation environmentAllocation = device.AllocateTransient(environmentBytes, 16);
				environmentAllocation.IsValid()) {
				GPUEnvironment record;
				for (u32 i = 0; i < 9; ++i) {
					record.sh[i] = Vec4(environment.sh[i].x, environment.sh[i].y, environment.sh[i].z, 0.0f);
				}
				record.mipCount0 = static_cast<f32>(environment.mipCount[0]);
				record.weight0 = environment.cubeWeight[0];
				record.mipCount1 = static_cast<f32>(environment.mipCount[1]);
				record.weight1 = environment.cubeWeight[1];
				record.flat = Vec4(environment.flatRadiance.x, environment.flatRadiance.y, environment.flatRadiance.z,
								   environment.reflectionIntensity / GraphicsInternal::ReferenceExposure(state.physicalLightUnits));
				std::memcpy(environmentAllocation.cpu, &record, sizeof(GPUEnvironment));
				GPUFogLayer* fogLayers = reinterpret_cast<GPUFogLayer*>(static_cast<u8*>(environmentAllocation.cpu) + sizeof(GPUEnvironment));
				for (u32 i = 0; i < environment.fogCount; ++i) {
					const RenderState::BlendedEnvironment::FogLayer& fog = environment.fog[i];
					fogLayers[i].colorShare = Vec4(fog.color.x, fog.color.y, fog.color.z, fog.share);
					fogLayers[i].params = Vec4(fog.params.x, fog.params.y, fog.params.z, 0.0f);
				}
				viewUniforms.environmentAddress =
					device.GetBufferDeviceAddress(environmentAllocation.buffer) + environmentAllocation.offset;
				viewUniforms.fogLayerCount = static_cast<f32>(environment.fogCount);
			}

			const u32 probeCount =
				kind == SceneKind::World && reflectionProbes ? static_cast<u32>(state.reflectionProbeSlices.size()) : 0u;
			if (probeCount > 0) {
				if (RHI::TransientAllocation probeAllocation =
						device.AllocateTransient(sizeof(GPUReflectionProbe) * probeCount, 16);
					probeAllocation.IsValid()) {
					GPUReflectionProbe* records = static_cast<GPUReflectionProbe*>(probeAllocation.cpu);
					for (u32 slice = 0; slice < probeCount; ++slice) {
						ReflectionProbe* probe = state.reflectionProbeSlices[slice].Get();
						SceneObject* probeOwner = probe->GetOwner().Get();
						if (!probeOwner) {
							continue;
						}
						const Vec3 capture = probeOwner->GetWorldPosition();
						const Vec3 center = capture + probe->GetBoxOffset();
						const Vec3 half = probe->GetBoxSize() * 0.5f;
						records[slice].boxMin = Vec4(center.x - half.x, center.y - half.y, center.z - half.z,
													 probe->GetBoxProjection() ? 1.0f : 0.0f);
						records[slice].boxMax = Vec4(center.x + half.x, center.y + half.y, center.z + half.z,
													 probe->GetImportance());
						records[slice].captureWS =
							Vec4(capture.x, capture.y, capture.z,
								 static_cast<f32>(Math::Max(1u, probe->GetBakedCubemap()->storedMipLevels)));
					}
					viewUniforms.probeAddress =
						device.GetBufferDeviceAddress(probeAllocation.buffer) + probeAllocation.offset;
					viewUniforms.probeCount = static_cast<f32>(probeCount);
				}
			}

			viewUniforms.renderExtentInverseX = targets.extent.x > 0 ? 1.0f / static_cast<f32>(targets.extent.x) : 0.0f;
			viewUniforms.renderExtentInverseY = targets.extent.y > 0 ? 1.0f / static_cast<f32>(targets.extent.y) : 0.0f;
			viewUniforms.viewDataAddress = WriteViewData(device, view, projection, targets.extent);

			RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
			std::memcpy(viewAllocation.cpu, &viewUniforms, sizeof(GPUViewUniforms));
			if (outViewAllocation) {
				*outViewAllocation = viewAllocation;
			}

			const ExtractedDraws extracted = ExtractDrawList(arena, DrawListUse::View, kind, onlyScenes, {&frustum, 1}, cameraPosition,
															 projection, renderingLayerMask, overrideMaterial);
			DrawEntry** const drawList = extracted.list;
			const u32 drawCount = extracted.count;
			const u64 perDrawAddress = extracted.perDrawAddress;

			const u32 shadedDrawCount = fill == ViewFill::Wireframe ? 0 : drawCount;
			u32 firstBlended = shadedDrawCount;
			for (u32 i = 0; i < shadedDrawCount; ++i) {
				if (drawList[i]->alpha) {
					firstBlended = i;
					break;
				}
			}
			ProceduralDraw* proceduralDraws = nullptr;
			const u32 proceduralDrawCount =
				fill == ViewFill::Wireframe ? 0 : SortProceduralDraws(arena, procedural, cameraPosition, proceduralDraws);
			u32 firstBlendedProcedural = 0;
			while (firstBlendedProcedural < proceduralDrawCount && !proceduralDraws[firstBlendedProcedural].alpha) {
				++firstBlendedProcedural;
			}
			const bool willSplitForOpaqueCopy =
				reads.copiesColor && reads.color.IsValid() &&
				TransparentReadsColor(drawList, firstBlended, shadedDrawCount, proceduralDraws, firstBlendedProcedural, proceduralDrawCount);

			RHI::RenderPassDesc::Attachment colorAttachment;
			colorAttachment.texture = targets.color;
			colorAttachment.load = targets.clear ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
			colorAttachment.store = (!willSplitForOpaqueCopy && targets.discardColor) ? RHI::StoreOp::DontCare : RHI::StoreOp::Store;
			colorAttachment.clearColor = clearColor;
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = targets.depth;
			depthAttachment.load = targets.clear ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
			depthAttachment.store = (!willSplitForOpaqueCopy && targets.discardDepth) ? RHI::StoreOp::DontCare : RHI::StoreOp::Store;
			depthAttachment.clearDepth = 0.0f;

			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&colorAttachment, 1};
			pass.depth = &depthAttachment;
			if (!willSplitForOpaqueCopy && targets.resolveColor.IsValid()) {
				pass.resolveTargets = {&targets.resolveColor, 1};
			}
			cmd.BeginRenderPass(pass);

			u32 drawCalls = 0;
			RHI::PipelineHandle boundPipeline{};
			u64 boundMaterial = ~0ull;
			RHI::TransientSet viewSet{};
			RHI::TransientSet materialSet{};

			auto bindShaded = [&](const DrawEntry& entry, u64 drawAddress, bool& instanced) {
				RHI::PipelineHandle pipeline =
					GetOrCreatePipeline(entry.shader, targets.colorFormat, targets.depthFormat, &entry.renderState, targets.samples,
										entry.variant, ShaderProgramKind::Forward, &instanced, entry.lodFade != 0);
				if (!pipeline.IsValid()) {
					return false;
				}
				if (!(pipeline == boundPipeline)) {
					cmd.SetPipeline(pipeline);
					boundPipeline = pipeline;
					boundMaterial = ~0ull;
					viewSet = CreateViewSet(device, pipeline, *entry.shader, viewAllocation, state.DirectionalShadowTexture(),
											state.punctualShadowAtlas, environment.cube[0], entry.alpha ? RHI::TextureHandle{} : ambientOcclusion,
											environment.cube[1]);
					cmd.BindTransientSet(0, viewSet);
					if (SceneRequests::DeclaresSceneSet(*entry.shader)) {
						BindSceneTextureSet(cmd, device, pipeline, *entry.shader, bound, passView);
					}
				}

				if (entry.materialId != boundMaterial || !materialSet.IsValid()) {
					if (RHI::TransientSet set = BindMaterialSet(cmd, device, pipeline, entry); set.IsValid()) {
						materialSet = set;
					}
					boundMaterial = entry.materialId;
				}

				GPUDrawHeader header;
				header.drawAddress = drawAddress;
				cmd.PushConstants({reinterpret_cast<const u8*>(&header), DRAW_HEADER_BYTES});
				return true;
			};
			auto drawShaded = [&](u32 i, u32 end, const auto& joins) -> u32 {
				DrawEntry& entry = *drawList[i];
				bool instanced = false;
				if (!bindShaded(entry, perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw), instanced)) {
					return 1;
				}
				const u32 instances = instanced ? InstanceRunLength(drawList, i, end, joins) : 1u;
				DrawMesh(cmd, entry, instances);
				++drawCalls;
				state.frameDrawCallsSaved += instances - 1;
				outTriangles += entry.submesh->indexCount / 3 * instances;
				return instances;
			};

			u32 nextProcedural = 0;
			auto drawProcedural = [&](const ProceduralDraw& draw) {
				ProceduralRenderer* renderer = state.procedural.renderers[draw.renderer].renderer.Get();
				if (!renderer) {
					return;
				}
				GraphicsInternal::ProceduralContextInit init;
				init.cmd = &cmd;
				init.device = &device;
				init.frame = &state.procedural;
				init.item = draw.item;
				init.colorFormat = targets.colorFormat;
				init.depthFormat = targets.depthFormat;
				init.samples = targets.samples;
				init.extent = targets.extent;
				init.view = view;
				init.projection = projection;
				init.cameraPosition = cameraPosition;
				init.sceneColor = bound.color;
				init.sceneDepth = bound.depth;
				init.sceneNormals = bound.normals;
				init.viewBlock = viewAllocation;
				init.shadowAtlas = state.DirectionalShadowTexture();
				init.punctualAtlas = state.punctualShadowAtlas;
				init.environmentCube = environment.cube[0];
				init.environmentCube2 = environment.cube[1];
				init.ambientOcclusion = draw.alpha ? RHI::TextureHandle{} : ambientOcclusion;
				init.drawCalls = &drawCalls;
				ProceduralContext context(init);
				renderer->Record(context);
				boundPipeline = RHI::PipelineHandle{};
				boundMaterial = ~0ull;
			};

			for (u32 i = 0; i < firstBlended;) {
				i += drawShaded(i, firstBlended, [](const DrawEntry&) { return true; });
			}
			while (nextProcedural < proceduralDrawCount && !proceduralDraws[nextProcedural].alpha) {
				drawProcedural(proceduralDraws[nextProcedural++]);
			}

			const u32 skyCount = targets.drawSky ? environment.skyCount : 0;
			for (u32 layer = 0; layer < skyCount; ++layer) {
				const RenderState::EnvironmentSetup& sky = environment.skies[layer];
				const f32 alpha = environment.skyAlpha[layer];
				MaterialAsset* skyMaterial = sky.skyMaterial.Get();
				ShaderAsset* skyShader = skyMaterial ? skyMaterial->shader.Get() : EnsureSkyShader();
				if (!skyShader) {
					continue;
				}
				const u32 skyVariant = skyMaterial ? skyShader->FindVariant(skyMaterial->keywordHash) : SHADER_BASE_VARIANT;
				RHI::ShaderRenderState skyState = skyShader->renderState;
				skyState.blend = alpha >= 1.0f ? RHI::BlendMode::Opaque : RHI::BlendMode::Alpha;
				RHI::PipelineHandle skyPipeline =
					GetOrCreatePipeline(skyShader, targets.colorFormat, targets.depthFormat, &skyState, targets.samples, skyVariant);
				if (skyPipeline.IsValid()) {
					cmd.SetPipeline(skyPipeline);
					boundPipeline = RHI::PipelineHandle{};
					boundMaterial = ~0ull;
					RHI::TransientSet skySet = CreateViewSet(device, skyPipeline, *skyShader, viewAllocation,
															 state.DirectionalShadowTexture(), state.punctualShadowAtlas, sky.cube);
					cmd.BindTransientSet(0, skySet);
					if (skyMaterial) {
						DrawEntry skyEntry;
						PackMaterialBlock(skyEntry, *state.extractionArena, skyShader, skyMaterial);
						ResolveMaterialTextures(skyEntry, *state.extractionArena, skyShader, skyMaterial);
						BindMaterialSet(cmd, device, skyPipeline, skyEntry);
					}
					GPUSkyPush push;
					push.invViewProjection = viewProjection.Inverse();
					push.sunDirection = sunDirection;
					push.sunCosSize = sky.sunCosSize;
					push.sunColor = sunColor;
					push.skyIntensity = sky.skyIntensity;
					push.sunAngularDiameter = sky.sunAngularDiameter;
					push.weight = alpha;
					cmd.PushConstants({reinterpret_cast<const u8*>(&push), SKY_PUSH_BYTES});
					cmd.Draw(3);
					++drawCalls;
				}
			}

			if (willSplitForOpaqueCopy) {
				cmd.EndRenderPass();
				const bool resolve = targets.samples > 1;
				cmd.TransitionTexture(targets.color, resolve ? RHI::ResourceState::TransferSrc : RHI::ResourceState::ShaderResource);
				cmd.TransitionTexture(reads.color, resolve ? RHI::ResourceState::TransferDst : RHI::ResourceState::RenderTarget);
				drawCalls += RecordSceneColorCopy(cmd, device, targets.color, reads.color, resolve);
				cmd.TransitionTexture(reads.color, RHI::ResourceState::ShaderResource);
				cmd.TransitionTexture(targets.color, RHI::ResourceState::RenderTarget);
				colorAttachment.load = RHI::LoadOp::Load;
				colorAttachment.store = targets.discardColor ? RHI::StoreOp::DontCare : RHI::StoreOp::Store;
				depthAttachment.load = RHI::LoadOp::Load;
				depthAttachment.store = targets.discardDepth ? RHI::StoreOp::DontCare : RHI::StoreOp::Store;
				if (targets.resolveColor.IsValid()) {
					pass.resolveTargets = {&targets.resolveColor, 1};
				}
				cmd.BeginRenderPass(pass);
				boundPipeline = RHI::PipelineHandle{};
				boundMaterial = ~0ull;
				bound.color = reads.color;
			}

			for (u32 i = firstBlended; i < shadedDrawCount;) {
				while (nextProcedural < proceduralDrawCount && ProceduralSortsBefore(proceduralDraws[nextProcedural], *drawList[i])) {
					drawProcedural(proceduralDraws[nextProcedural++]);
				}
				i += drawShaded(i, shadedDrawCount, [&](const DrawEntry& next) {
					return nextProcedural >= proceduralDrawCount || !ProceduralSortsBefore(proceduralDraws[nextProcedural], next);
				});
			}
			while (nextProcedural < proceduralDrawCount) {
				drawProcedural(proceduralDraws[nextProcedural++]);
			}

			if (fill != ViewFill::Solid && drawCount > 0) {
				ShaderAsset* wireShader = EnsureWireframeShader();
				RHI::PipelineHandle wirePipeline = wireShader
													   ? GetOrCreatePipeline(wireShader, targets.colorFormat, targets.depthFormat, nullptr, targets.samples)
													   : RHI::PipelineHandle{};
				if (wirePipeline.IsValid()) {
					cmd.SetPipeline(wirePipeline);
					RHI::TransientSet wireViewSet = CreateViewSet(device, wirePipeline, *wireShader, viewAllocation,
																  state.DirectionalShadowTexture(), state.punctualShadowAtlas);
					cmd.BindTransientSet(0, wireViewSet);
					for (u32 i = 0; i < drawCount; ++i) {
						DrawEntry& entry = *drawList[i];
						GPUWirePerDraw perDraw;
						perDraw.draw.drawAddress = perDrawAddress + static_cast<u64>(i) * sizeof(GPUPerDraw);
						perDraw.color = wireColor.ToU32();
						cmd.PushConstants({reinterpret_cast<const u8*>(&perDraw), WIRE_PER_DRAW_BYTES});
						DrawMesh(cmd, entry);
						++drawCalls;
						outTriangles += entry.submesh->indexCount / 3;
					}
				}
			}

			cmd.EndRenderPass();
			return drawCalls;
		}

		u32 RecordDebugLinePass(RHI::CommandList& cmd, const Mat4& viewProjection,
								std::span<const DebugLine> lines, const PassTargets& targets, bool depthTested) {
			RenderState& state = *_state;
			ShaderAsset* shader = state.debugLineShader.Get();
			if (lines.empty() || !shader) {
				return 0;
			}
			RHI::ShaderRenderState alwaysOnTop = shader->renderState;
			alwaysOnTop.depth = RHI::DepthMode::Off;
			RHI::PipelineHandle pipeline = GetOrCreatePipeline(shader, targets.colorFormat, targets.depthFormat,
															   depthTested ? nullptr : &alwaysOnTop, targets.samples);
			if (!pipeline.IsValid()) {
				return 0;
			}

			RHI::Device& device = *state.device;
			u64 bytes = static_cast<u64>(lines.size()) * 2 * sizeof(GPUDebugVertex);
			RHI::TransientAllocation allocation = device.AllocateTransient(bytes, 16);
			if (!allocation.IsValid()) {
				return 0;
			}
			GPUDebugVertex* vertices = static_cast<GPUDebugVertex*>(allocation.cpu);
			for (size_t i = 0; i < lines.size(); ++i) {
				u32 packed = lines[i].color.ToU32();
				vertices[i * 2 + 0] = {lines[i].a, packed};
				vertices[i * 2 + 1] = {lines[i].b, packed};
			}

			GPUDebugLinePush push;
			push.viewProjection = viewProjection;
			push.vertexAddress = device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;

			RHI::RenderPassDesc::Attachment colorAttachment;
			colorAttachment.texture = targets.color;
			colorAttachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc::DepthAttachment depthAttachment;
			depthAttachment.texture = targets.depth;
			depthAttachment.load = RHI::LoadOp::Load;

			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&colorAttachment, 1};
			pass.depth = &depthAttachment;
			cmd.BeginRenderPass(pass);
			cmd.SetPipeline(pipeline);
			cmd.PushConstants({reinterpret_cast<const u8*>(&push), DEBUG_LINE_PUSH_BYTES});
			cmd.Draw(static_cast<u32>(lines.size()) * 2);
			cmd.EndRenderPass();
			return 1;
		}

		GraphResourceId DeclareSkinOutput(RHI::Device& device, GraphicsInternal::RenderGraph& graph, GraphicsInternal::SkinDeformer& deformer,
										  const Vector<Handle<SkinnedMeshRenderer>>& renderers) {
			u64 maxVertices = 0;
			for (Handle<SkinnedMeshRenderer> handle : renderers) {
				SkinnedMeshRenderer* renderer = handle.Get();
				MeshAsset* mesh = renderer ? renderer->GetMesh().Get() : nullptr;
				maxVertices += mesh ? mesh->vertices.size() : 0;
			}
			deformer.Begin(device, maxVertices);
			return deformer.GetOutput().IsValid() ? graph.ImportBuffer(deformer.GetOutput()) : graph.CreateVirtual();
		}

		void PassRecordSkinning(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			if (pass.work.flag) {
				RecordSkinning(*exec.device, *exec.cmd, state.isolatedSkinDeformer, state.isolatedSkinnedMeshRenderers, state.isolatedSkinnedOutputs);
			} else {
				RecordSkinning(*exec.device, *exec.cmd, state.skinDeformer, state.skinnedMeshRenderers, state.skinnedOutputs);
			}
		}

		void PassRecordFramePass(GraphExecution& exec, const GraphPass& pass) {
			FramePass* framePass = GraphicsInternal::GetFramePass(pass.work.index);
			if (!framePass) {
				return;
			}
			ComputeContext context(GraphicsInternal::ComputeContextInit{&exec, &pass, pass.work.subIndex});
			framePass->Record(context);
		}

		void PassRecordViewCold(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Result = 0 };
			RenderState& state = *_state;
			auto it = state.viewTargets.find(static_cast<u32>(pass.work.key));
			if (it == state.viewTargets.end()) {
				return;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Result];
			attachment.load = RHI::LoadOp::Clear;
			attachment.clearColor = Color::Black();
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->EndRenderPass();
			it->second.colorSampleable = true;
		}

		bool PartitionHasShadowCaster(const RenderState& state, SceneKind kind, std::span<const u64> onlyScenes) {
			if (!state.ShadowsEnabled()) {
				return false;
			}
			const bool isolated = kind != SceneKind::World;
			const Vector<Handle<Light>>& lights = isolated ? state.isolatedLights : state.lights;
			for (Handle<Light> lightHandle : lights) {
				Light* light = lightHandle.Get();
				if (!light || !light->GetCastShadows()) {
					continue;
				}
				if (isolated && !onlyScenes.empty()) {
					SceneObject* owner = light->GetOwner().Get();
					if (!owner) {
						continue;
					}
					u64 sceneId = owner->GetScene().GetInstanceId();
					if (std::find(onlyScenes.begin(), onlyScenes.end(), sceneId) == onlyScenes.end()) {
						continue;
					}
				}
				return true;
			}
			return false;
		}

		void PassRecordViewShadow(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Atlas = 0,
						 PunctualAtlas = 1 };
			RenderState& state = *_state;
			RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			Vector<GraphicsInternal::CascadeView>* fit = nullptr;
			if (pending.retainCascadeFit) {
				auto it = state.viewTargets.find(pending.targetId);
				if (it != state.viewTargets.end()) {
					fit = &it->second.cascadeFit;
				}
			}
			exec.drawCalls += RecordShadowPass(*exec.cmd, *state.extractionArena, pending.view, pending.projection,
											   pending.cameraPosition, pass.textures[Atlas], pass.textures[PunctualAtlas],
											   ShadowOwnerToken(pass.work), exec.triangles,
											   {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
											   pending.kind, pending.hasShadowFit ? &pending.shadowFit : nullptr, fit,
											   {state.proceduralVisible.data() + pending.firstCaster, pending.casterCount},
											   pending.cullingMask);
			pending.cascadeCount = state.pendingShadow.cascadeCount;
		}

		constexpr u32 SCENE_PASS_FIRST_READ = 5;

		struct ScenePassSlots {
			u32 ambientOcclusion = 0;
			u32 depth = 0;
			u32 normals = 0;
			u32 color = 0;
			u32 inlineResolve = 0;
		};

		// must match DeclareScenePassReads
		ScenePassSlots ResolveScenePassSlots(bool produced, PassRequests reads, bool hasInlineResolve = false) {
			ScenePassSlots slots;
			u32 next = SCENE_PASS_FIRST_READ;
			if (produced) {
				slots.ambientOcclusion = next++;
			}
			if (HasFlag(reads, PassRequests::SceneDepth)) {
				slots.depth = next++;
			}
			if (HasFlag(reads, PassRequests::SceneNormals)) {
				slots.normals = next++;
			}
			if (HasFlag(reads, PassRequests::SceneColor)) {
				slots.color = next++;
			}
			if (hasInlineResolve) {
				slots.inlineResolve = next++;
			}
			return slots;
		}

		GraphResourceId DeclareOpaqueColor(GraphicsInternal::RenderGraph& graph, const RHI::TextureDesc& colorDesc, PassRequests reads) {
			if (!HasFlag(reads, PassRequests::SceneColor)) {
				return GraphicsInternal::INVALID_GRAPH_RESOURCE;
			}
			RHI::TextureDesc desc = colorDesc;
			desc.usage = desc.usage | RHI::TextureUsage::TransferDst;
			desc.debugName = "SceneOpaqueColor";
			if (_state->msaaSamples <= 1) {
				EnsureBlitShader();
			}
			return graph.CreateTransient(desc);
		}

		void DeclareScenePassReads(GraphicsInternal::RenderGraph& graph, GraphResourceId ambientOcclusion, PassRequests reads,
								   GraphResourceId prepassDepth, GraphResourceId prepassNormals, GraphResourceId colorCopy) {
			if (ambientOcclusion != GraphicsInternal::INVALID_GRAPH_RESOURCE) {
				graph.Read(ambientOcclusion, RHI::ResourceState::ShaderResource);
			}
			if (HasFlag(reads, PassRequests::SceneDepth)) {
				graph.Read(prepassDepth, RHI::ResourceState::ShaderResource);
			}
			if (HasFlag(reads, PassRequests::SceneNormals)) {
				graph.Read(prepassNormals, RHI::ResourceState::ShaderResource);
			}
			if (HasFlag(reads, PassRequests::SceneColor)) {
				graph.Write(colorCopy, RHI::ResourceState::RenderTarget);
			}
		}

		RHI::TextureHandle ScenePassOcclusion(const GraphPass& pass) {
			return pass.work.produced ? pass.textures[ResolveScenePassSlots(true, PassRequests::None).ambientOcclusion] : RHI::TextureHandle{};
		}

		ScenePassReads ScenePassReadsOf(const GraphPass& pass, PassRequests reads) {
			const ScenePassSlots slots = ResolveScenePassSlots(pass.work.produced, reads);
			ScenePassReads out;
			if (HasFlag(reads, PassRequests::SceneDepth)) {
				out.depth = pass.textures[slots.depth];
			}
			if (HasFlag(reads, PassRequests::SceneNormals)) {
				out.normals = pass.textures[slots.normals];
			}
			if (HasFlag(reads, PassRequests::SceneColor)) {
				out.color = pass.textures[slots.color];
				out.copiesColor = true;
			}
			return out;
		}

		void PassRecordViewScene(GraphExecution& exec, const GraphPass& pass) {
			// builder's declaration order = this table
			enum : u32 { Color = 0,
						 Depth = 1 };
			RenderState& state = *_state;
			RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			PassTargets targets;
			targets.color = pass.textures[Color];
			targets.depth = pass.textures[Depth];
			targets.clear = true;
			targets.samples = state.msaaSamples;
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt != state.viewTargets.end()) {
				targets.extent = state.ViewRenderSize(pending, targetIt->second.size);
			}
			exec.drawCalls += RecordScenePass(*exec.cmd, *state.extractionArena, pending.view, pending.projection,
											  pending.cameraPosition, pending.clearColor, targets, exec.triangles,
											  {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
											  pending.fill, pending.wireColor, pending.kind, ShadowOwnerToken(pass.work),
											  pending.cullingMask, nullptr, ScenePassOcclusion(pass), &pending.viewAllocation,
											  {state.proceduralVisible.data() + pending.firstProcedural, pending.proceduralCount},
											  ScenePassReadsOf(pass, pending.sceneReads), !pending.radianceCapture);
		}

		struct RowTextureSlots {
			u32 sceneColor = 0;
			u32 sceneDepth = 0;
			u32 sceneNormals = 0;
			u32 passInput0 = 0;
			u32 passInput1 = 0;
		};

		constexpr PassRequests RequestsForDebugView(DebugView mode) {
			switch (mode) {
			case DebugView::Normals:
				return PassRequests::SceneNormals;
			case DebugView::Depth:
			case DebugView::Cascades:
				return PassRequests::SceneDepth;
			default:
				return PassRequests::None;
			}
		}

		RowTextureSlots ResolveRowTextureSlots(PassRequests requests, bool writesSceneTarget, bool hasSceneDepth, const PassStep& step) {
			RowTextureSlots slots;
			u32 next = writesSceneTarget ? (hasSceneDepth ? 2 : 1) : 1;
			if (HasFlag(requests, PassRequests::SceneColor)) {
				slots.sceneColor = next++;
			}
			if (HasFlag(requests, PassRequests::SceneDepth)) {
				slots.sceneDepth = next++;
			}
			if (HasFlag(requests, PassRequests::SceneNormals)) {
				slots.sceneNormals = next++;
			}
			if (step.reads[0] != PASS_NO_READ) {
				slots.passInput0 = next++;
			}
			if (step.reads[1] != PASS_NO_READ) {
				slots.passInput1 = next++;
			}
			return slots;
		}

		PassStep ResolveRowStep(const RenderState& state, u32 flatRow, u32 step) {
			const RenderState::RowPlan& plan = state.rowPlans[flatRow];
			if (step >= plan.stepCount) {
				return PassStep{};
			}
			return state.rowPlanSteps[plan.firstStep + step];
		}

		struct RowProductions {
			GraphicsInternal::GraphResourceId ambientOcclusion = GraphicsInternal::INVALID_GRAPH_RESOURCE;
		};

		RHI::TextureHandle ResolveRowVolume(RenderState& state, const Handle<RenderPass>& row, const PassVolume& declared) {
			if (!state.device) {
				return {};
			}
			RenderState::RowVolume& kept = state.rowVolumes[row];
			if (kept.texture.IsValid() && (kept.size != declared.size || kept.format != declared.format)) {
				state.device->DestroyResource(kept.texture);
				kept = {};
			}
			if (!kept.texture.IsValid()) {
				RHI::TextureDesc desc;
				desc.size = Vec2I{declared.size.x, declared.size.y};
				desc.depth = static_cast<u32>(declared.size.z);
				desc.format = declared.format;
				desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
				desc.debugName = "PassVolume";
				kept.texture = state.device->CreateTextureResource(desc);
				if (!kept.texture.IsValid()) {
					return {};
				}
				kept.size = declared.size;
				kept.format = declared.format;
				kept.uploaded = false;
			}
			if (!kept.uploaded || kept.revision != declared.revision) {
				state.device->UploadTexture(kept.texture, declared.contents);
				kept.revision = declared.revision;
				kept.uploaded = true;
			}
			return kept.texture;
		}

		void SweepRowVolumes(RenderState& state) {
			if (state.rowVolumes.empty() || !state.device) {
				return;
			}
			for (auto it = state.rowVolumes.begin(); it != state.rowVolumes.end();) {
				if (it->first.IsValid()) {
					++it;
					continue;
				}
				if (it->second.texture.IsValid()) {
					state.device->DestroyResource(it->second.texture);
				}
				it = state.rowVolumes.erase(it);
			}
		}

		RowProductions BuildRowPlans(RenderState& state, GraphicsInternal::RenderGraph& graph,
									 u32 firstRow, u32 lastRow, Vec2I extent, const Vec3& cameraPosition) {
			RowProductions productions;
			for (u32 index = firstRow; index < lastRow; ++index) {
				RenderState::RowPlan& slot = state.rowPlans[index];
				slot = {};
				slot.firstStep = static_cast<u32>(state.rowPlanSteps.size());
				slot.firstTarget = static_cast<u32>(state.rowPlanTargets.size());
				RenderPass* row = state.viewPassRows[index].Get();
				if (!row) {
					continue;
				}
				PassPlan plan(extent, cameraPosition);
				row->DeclarePlan(plan);
				if (plan.HasVolume()) {
					slot.volume = ResolveRowVolume(state, state.viewPassRows[index], plan.GetVolume());
				}
				if (plan.GetTargetCount() == 0 || plan.GetStepCount() == 0) {
					continue;
				}
				for (u32 target = 0; target < plan.GetTargetCount(); ++target) {
					const PassTarget& declared = plan.GetTarget(target);
					RenderState::RowTarget resolved;
					resolved.format = declared.format;
					resolved.size = Vec2I{Math::Max(extent.x / declared.divisor, 1),
										  Math::Max(extent.y / declared.divisor, 1)};
					RHI::TextureDesc desc;
					desc.size = resolved.size;
					desc.format = declared.format;
					desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
					desc.debugName = "PassTarget";
					resolved.resource = graph.CreateTransient(desc);
					state.rowPlanTargets.push_back(resolved);
				}
				for (u32 step = 0; step < plan.GetStepCount(); ++step) {
					state.rowPlanSteps.push_back(plan.GetStep(step));
				}
				slot.targetCount = static_cast<u16>(plan.GetTargetCount());
				slot.stepCount = static_cast<u16>(plan.GetStepCount());

				if (HasFlag(plan.GetProduced(), PassProduces::AmbientOcclusion)) {
					if (row->point >= PassPoint::Opaque) {
						SLT_LOG_WARN("Graphics",
									 "a row at {} produces ambient occlusion after the shading consumed it. "
									 "Move it before Opaque",
									 GetPassPointName(row->point));
					} else if (productions.ambientOcclusion != GraphicsInternal::INVALID_GRAPH_RESOURCE) {
						SLT_LOG_WARN("Graphics", "a 2nd row at {} produces ambient occlusion (1st one wins)",
									 GetPassPointName(row->point));
					} else {
						const u8 target = plan.GetProducedTarget(PassProduces::AmbientOcclusion);
						productions.ambientOcclusion = state.rowPlanTargets[slot.firstTarget + target].resource;
					}
				}
			}
			return productions;
		}

		struct RowFrameResources {
			GraphicsInternal::GraphResourceId sceneTarget = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphicsInternal::GraphResourceId depth = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphicsInternal::GraphResourceId sceneColorCopy = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphicsInternal::GraphResourceId prepassDepth = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphicsInternal::GraphResourceId prepassNormals = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphicsInternal::GraphResourceId skinBuffer = GraphicsInternal::INVALID_GRAPH_RESOURCE;
		};

		// must match ResolveRowTextureSlots
		void DeclareRowPasses(RenderState& state, GraphicsInternal::RenderGraph& graph,
							  const char* name, u32 zoneSite, GraphicsInternal::PassRecordFn record,
							  GraphicsInternal::PassWork& work, u32 flatRow, const RowFrameResources& frame) {
			const RenderPass* row = state.viewPassRows[flatRow].Get();
			if (!row) {
				return;
			}
			const RenderState::RowPlan& plan = state.rowPlans[flatRow];
			const PassRequests requests = row->GetRequests();
			const u32 stepCount = plan.stepCount > 0 ? plan.stepCount : 1;
			for (u32 step = 0; step < stepCount; ++step) {
				const PassStep declared = ResolveRowStep(state, flatRow, step);
				work.step = static_cast<u8>(step);
				graph.AddPass(name, zoneSite, record, work);
				if (declared.target == PASS_SCENE_TARGET) {
					graph.Write(frame.sceneTarget, RHI::ResourceState::RenderTarget);
					if (frame.depth != GraphicsInternal::INVALID_GRAPH_RESOURCE) {
						graph.Write(frame.depth, RHI::ResourceState::DepthWrite);
					}
				} else {
					graph.Write(state.rowPlanTargets[plan.firstTarget + declared.target].resource,
								RHI::ResourceState::RenderTarget);
				}
				if (HasFlag(requests, PassRequests::SceneColor)) {
					graph.Read(frame.sceneColorCopy, RHI::ResourceState::ShaderResource);
				}
				if (HasFlag(requests, PassRequests::SceneDepth)) {
					graph.Read(frame.prepassDepth, RHI::ResourceState::ShaderResource);
				}
				if (HasFlag(requests, PassRequests::SceneNormals)) {
					graph.Read(frame.prepassNormals, RHI::ResourceState::ShaderResource);
				}
				for (const u8 slot : declared.reads) {
					if (slot != PASS_NO_READ) {
						graph.Read(state.rowPlanTargets[plan.firstTarget + slot].resource,
								   RHI::ResourceState::ShaderResource);
					}
				}
				if (frame.skinBuffer != GraphicsInternal::INVALID_GRAPH_RESOURCE) {
					graph.Read(frame.skinBuffer, RHI::ResourceState::ShaderResource);
				}
			}
			work.step = 0;
		}

		void BindRowStep(GraphicsInternal::PassContextInit& init, const RenderState& state,
						 const RenderPass& row, const GraphPass& pass, u32 flatRow) {
			const RenderState::RowPlan& plan = state.rowPlans[flatRow];
			const PassStep step = ResolveRowStep(state, flatRow, pass.work.step);
			const bool writesSceneTarget = step.target == PASS_SCENE_TARGET;
			const bool hasSceneDepth = writesSceneTarget && init.depthFormat != RHI::Format::Unknown;
			init.step = pass.work.step;
			init.stepCount = plan.stepCount > 0 ? plan.stepCount : 1;
			init.requests = row.GetRequests();
			init.writesSceneTarget = writesSceneTarget;
			const RowTextureSlots slots = ResolveRowTextureSlots(init.requests, writesSceneTarget, hasSceneDepth, step);
			init.color = pass.textures[0];
			init.depth = hasSceneDepth ? pass.textures[1] : RHI::TextureHandle{};
			if (!writesSceneTarget) {
				const RenderState::RowTarget& target = state.rowPlanTargets[plan.firstTarget + step.target];
				init.colorFormat = target.format;
				init.depthFormat = RHI::Format::Unknown;
				init.samples = 1;
				init.extent = target.size;
			}
			if (HasFlag(init.requests, PassRequests::SceneColor)) {
				init.sceneColor = pass.textures[slots.sceneColor];
			}
			if (HasFlag(init.requests, PassRequests::SceneDepth)) {
				init.sceneDepth = pass.textures[slots.sceneDepth];
			}
			if (HasFlag(init.requests, PassRequests::SceneNormals)) {
				init.sceneNormals = pass.textures[slots.sceneNormals];
			}
			if (step.reads[0] != PASS_NO_READ) {
				init.passInput0 = pass.textures[slots.passInput0];
			}
			if (step.reads[1] != PASS_NO_READ) {
				init.passInput1 = pass.textures[slots.passInput1];
			}
			init.passVolume = plan.volume;
		}

		void PassRecordViewPrepass(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt == state.viewTargets.end()) {
				return;
			}
			const bool hasNormals = pass.work.flag;
			exec.drawCalls += RecordDepthNormalsPass(
				*exec.cmd, *state.extractionArena, pending.view, pending.projection, pending.cameraPosition,
				hasNormals ? pass.textures[0] : RHI::TextureHandle{}, pass.textures[hasNormals ? 1 : 0],
				state.ViewRenderSize(pending, targetIt->second.size), exec.triangles,
				{state.viewScenes.data() + pending.firstScene, pending.sceneCount}, pending.kind,
				pass.work.produced ? std::span<const u32>{state.proceduralVisible.data() + pending.firstProcedural, pending.proceduralCount}
								   : std::span<const u32>{},
				pending.cullingMask);
		}

		void PassRecordViewPrepassItems(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Kept = 0,
						 Depth = 1,
						 Normals = 2 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt == state.viewTargets.end()) {
				return;
			}
			exec.drawCalls += RecordPrepassItemsOverCopy(
				*exec.cmd, pending.view, pending.projection, pending.cameraPosition, pass.textures[Kept], pass.textures[Depth],
				pass.work.flag ? pass.textures[Normals] : RHI::TextureHandle{}, state.ViewRenderSize(pending, targetIt->second.size),
				{state.proceduralVisible.data() + pending.firstProcedural, pending.proceduralCount});
		}

		f32 CameraAspect(const RenderState::CameraWork& work) {
			return static_cast<f32>(work.pixels.size.x) / static_cast<f32>(work.pixels.size.y);
		}

		void PassRecordWindowPrepass(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			const RenderState::CameraWork& cameraWork = work.cameras[pass.work.subIndex];
			Camera* camera = cameraWork.camera;
			SceneObject* cameraOwner = camera ? camera->GetOwner().Get() : nullptr;
			if (!cameraOwner) {
				return;
			}
			const Vec2I renderSize = cameraWork.pixels.size;
			const f32 aspect = CameraAspect(cameraWork);
			const Mat4 cameraWorld = cameraOwner->GetWorldMatrix();
			const bool hasNormals = pass.work.flag;
			const ViewRecordScope viewScope(cameraWork);
			exec.drawCalls += RecordDepthNormalsPass(
				*exec.cmd, *state.extractionArena, cameraWorld.Inverse(), camera->BuildProjection(aspect),
				cameraOwner->GetWorldPosition(),
				hasNormals ? pass.textures[0] : RHI::TextureHandle{}, pass.textures[hasNormals ? 1 : 0],
				renderSize, exec.triangles, {}, SceneKind::World,
				pass.work.produced ? std::span<const u32>{state.proceduralVisible.data() + cameraWork.firstProcedural, cameraWork.proceduralCount}
								   : std::span<const u32>{},
				camera->GetCullingMask());
		}

		void PassRecordWindowPrepassItems(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Kept = 0,
						 Depth = 1,
						 Normals = 2 };
			RenderState& state = *_state;
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			const RenderState::CameraWork& cameraWork = work.cameras[pass.work.subIndex];
			Camera* camera = cameraWork.camera;
			SceneObject* cameraOwner = camera ? camera->GetOwner().Get() : nullptr;
			if (!cameraOwner) {
				return;
			}
			const Vec2I renderSize = cameraWork.pixels.size;
			const f32 aspect = CameraAspect(cameraWork);
			const ViewRecordScope viewScope(cameraWork);
			exec.drawCalls += RecordPrepassItemsOverCopy(
				*exec.cmd, cameraOwner->GetWorldMatrix().Inverse(), camera->BuildProjection(aspect), cameraOwner->GetWorldPosition(),
				pass.textures[Kept], pass.textures[Depth], pass.work.flag ? pass.textures[Normals] : RHI::TextureHandle{}, renderSize,
				{state.proceduralVisible.data() + cameraWork.firstProcedural, cameraWork.proceduralCount});
		}

		void PassRecordViewRow(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			const u32 flatRow = pending.firstRow + pass.work.subIndex;
			RenderPass* row = state.viewPassRows[flatRow].Get();
			if (!row) {
				return;
			}
			GraphicsInternal::PassContextInit init;
			init.exec = &exec;
			init.colorFormat = SCENE_COLOR_FORMAT;
			init.depthFormat = SCENE_DEPTH_FORMAT;
			init.samples = state.msaaSamples;
			init.view = pending.view;
			init.projection = pending.projection;
			init.cameraPosition = pending.cameraPosition;
			init.scopeIndex = pending.scopeIndex;
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt != state.viewTargets.end()) {
				init.extent = state.ViewRenderSize(pending, targetIt->second.size);
			}
			BindRowStep(init, state, *row, pass, flatRow);
			PassContext context(init);
			row->Record(context);
		}

		constexpr u32 RowSortKey(PassPoint point, FrameStagePosition position) {
			return static_cast<u32>(point) * 3u + static_cast<u32>(position);
		}

		constexpr u32 ROW_KEY_AFTER_LAST = RowSortKey(PassPoint::Present, FrameStagePosition::Late) + 1u;

		void PassRecordWindowRow(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			const RenderState::CameraWork& cameraWork = work.cameras[pass.work.subIndex];
			Camera* camera = cameraWork.camera;
			SceneObject* cameraOwner = camera ? camera->GetOwner().Get() : nullptr;
			if (!cameraOwner) {
				return;
			}
			const u32 flatRow = cameraWork.firstRow + pass.work.element;
			RenderPass* row = state.viewPassRows[flatRow].Get();
			if (!row) {
				return;
			}
			const Vec2I renderSize = cameraWork.pixels.size;
			const f32 aspect = CameraAspect(cameraWork);
			Mat4 cameraWorld = cameraOwner->GetWorldMatrix();

			const bool resolvedRow = cameraWork.resolvedPost && row->PrefersResolvedTarget() &&
									 RowSortKey(row->point, row->position) >= RowSortKey(PassPoint::Opaque, FrameStagePosition::Normal);
			GraphicsInternal::PassContextInit init;
			init.exec = &exec;
			init.colorFormat = SCENE_COLOR_FORMAT;
			init.depthFormat = resolvedRow ? RHI::Format::Unknown : SCENE_DEPTH_FORMAT;
			init.samples = resolvedRow ? 1u : state.msaaSamples;
			init.view = cameraWorld.Inverse();
			init.projection = camera->BuildProjection(aspect);
			init.cameraPosition = cameraOwner->GetWorldPosition();
			init.extent = renderSize;
			init.scopeIndex = cameraWork.scopeIndex;
			BindRowStep(init, state, *row, pass, flatRow);
			const ViewRecordScope viewScope(cameraWork);
			PassContext context(init);
			row->Record(context);
		}

		void PassRecordViewLines(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Color = 0,
						 Depth = 1 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const bool overlay = pass.work.flag;
			const u32 first = overlay ? pending.firstOverlayLine : pending.firstLine;
			const u32 count = overlay ? pending.overlayLineCount : pending.lineCount;
			PassTargets targets;
			targets.color = pass.textures[Color];
			targets.depth = pass.textures[Depth];
			targets.clear = false;
			targets.samples = state.msaaSamples;
			exec.drawCalls += RecordDebugLinePass(*exec.cmd, pending.projection * pending.view,
												  {state.viewLines.data() + first, count}, targets, !overlay);
		}

		void PassRecordSceneColorCopy(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Source = 0,
						 Destination = 1 };
			exec.drawCalls += RecordSceneColorCopy(*exec.cmd, *exec.device, pass.textures[Source], pass.textures[Destination], pass.work.flag);
		}

		GraphResourceId DeclareSceneColorCopy(GraphicsInternal::RenderGraph& graph, const char* name, u32 zoneSite, PassWork work,
											  GraphResourceId sceneTarget, const RHI::TextureDesc& colorDesc, bool resolve) {
			if (!resolve) {
				EnsureBlitShader();
			}
			RHI::TextureDesc copyDesc = colorDesc;
			copyDesc.usage = copyDesc.usage | RHI::TextureUsage::TransferDst;
			copyDesc.debugName = "SceneColorCopy";
			const GraphResourceId copy = graph.CreateTransient(copyDesc);
			work.flag = resolve;
			graph.AddPass(name, zoneSite, &PassRecordSceneColorCopy, work);
			graph.Read(sceneTarget, resolve ? RHI::ResourceState::TransferSrc : RHI::ResourceState::ShaderResource);
			graph.Write(copy, resolve ? RHI::ResourceState::TransferDst : RHI::ResourceState::RenderTarget);
			return copy;
		}

		struct GPUDebugViewPush {
			u32 mode = 0;
			u32 orthographic = 0;
			f32 depthLinearize[2] = {0.0f, 0.0f};
			f32 cascadeTint[MAX_SHADOW_CASCADES][4] = {};
		};
		static_assert(sizeof(GPUDebugViewPush) == 80, "one scalar row plus one float4 per cascade, under the 128 B budget");

		constexpr f32 CASCADE_TINT_TABLE[MAX_SHADOW_CASCADES][4] = {
			{0.90f, 0.22f, 0.22f, 1.0f},
			{0.28f, 0.80f, 0.32f, 1.0f},
			{0.30f, 0.50f, 0.95f, 1.0f},
			{0.92f, 0.82f, 0.25f, 1.0f},
		};

		void PassRecordDebugView(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Source = 0,
						 Destination = 1 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			ShaderAsset* shader = state.debugViewShader.Get();
			RHI::PipelineHandle pipeline =
				shader ? GetOrCreatePipeline(shader, SCENE_COLOR_FORMAT, RHI::Format::Unknown, nullptr, state.msaaSamples)
					   : RHI::PipelineHandle{};
			if (!pipeline.IsValid() || !pending.viewAllocation.IsValid()) {
				return;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Destination];
			attachment.load = RHI::LoadOp::DontCare;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->SetPipeline(pipeline);
			RHI::TransientSet set = exec.device->CreateTransientSet(pipeline, 1)
										.WriteTexture(0, pass.textures[Source])
										.WriteSampler(1, state.clampSampler)
										.Commit();
			exec.cmd->BindTransientSet(1, set);

			exec.cmd->BindTransientSet(0, CreateViewSet(*exec.device, pipeline, *shader, pending.viewAllocation,
														RHI::TextureHandle{}, RHI::TextureHandle{}));

			GPUDebugViewPush push;
			push.mode = pass.work.subIndex;
			const Mat4& projection = pending.projection;
			const bool perspective = Math::Abs(projection(3, 2)) > 1e-6f;
			push.orthographic = perspective ? 0u : 1u;
			if (perspective) {
				push.depthLinearize[0] = projection(2, 3);
				push.depthLinearize[1] = -projection(2, 2);
			} else {
				const f32 scale = Math::Abs(projection(2, 2)) > 1e-9f ? 1.0f / projection(2, 2) : 0.0f;
				push.depthLinearize[0] = scale;
				push.depthLinearize[1] = -projection(2, 3) * scale;
			}
			std::memcpy(push.cascadeTint, CASCADE_TINT_TABLE, sizeof(push.cascadeTint));
			exec.cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			exec.cmd->Draw(3);
			++exec.drawCalls;
			exec.cmd->EndRenderPass();
		}

		void PassRecordViewOutlineMask(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Mask = 0,
						 Depth = 1 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt == state.viewTargets.end()) {
				return;
			}
			exec.drawCalls += RecordOutlineMask(*exec.cmd, *state.extractionArena, pending, pass.textures[Mask], pass.textures[Depth],
												state.ViewRenderSize(pending, targetIt->second.size), exec.triangles);
		}

		void PassRecordViewPick(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Target = 0,
						 Depth = 1 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const ViewRecordScope viewScope(pending);
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt == state.viewTargets.end() || pending.pickSlot >= RenderState::PICK_SLOTS) {
				return;
			}
			const Vec2I size = targetIt->second.size;
			const Vec2I extent = state.ViewRenderSize(pending, size);
			if (size.x <= 0 || size.y <= 0 || extent.x <= 0 || extent.y <= 0) {
				return;
			}
			const Vec2I pixel(Math::Clamp(pending.pick.pixel.x * extent.x / size.x, 0, extent.x - 1),
							  Math::Clamp(pending.pick.pixel.y * extent.y / size.y, 0, extent.y - 1));
			exec.drawCalls += RecordPick(*exec.cmd, *state.extractionArena, pending, pass.textures[Target], pass.textures[Depth], pixel,
										 extent, state.pickSlots[pending.pickSlot].objects, exec.triangles);
		}

		void PassRecordViewPickCopy(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			if (pending.pickSlot >= RenderState::PICK_SLOTS || !state.pickRing.IsValid()) {
				return;
			}
			exec.cmd->CopyTextureToBuffer(pass.textures[0], state.pickRing, static_cast<u64>(pending.pickSlot) * sizeof(u32));
		}

		// must match ShalltearOutlinePush in Outline.hlsl
		struct GPUOutlinePush {
			u32 colors[MAX_OUTLINE_COLORS] = {}; // SRGB RGBA8 from Color32::ToU32
			f32 width = 2.0f;
			f32 hiddenOpacity = 0.3f;
			u32 pad[2] = {};
		};
		static_assert(sizeof(GPUOutlinePush) == 32);

		void PassRecordViewOutline(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Mask = 0,
						 MaskDepth = 1,
						 SceneDepth = 2,
						 Destination = 3 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			ShaderAsset* shader = state.outlineShader.Get();
			RHI::PipelineHandle pipeline =
				shader ? GetOrCreatePipeline(shader, SCENE_COLOR_FORMAT, RHI::Format::Unknown, nullptr, state.msaaSamples)
					   : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Destination];
			attachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->SetPipeline(pipeline);
			RHI::TransientSet set = exec.device->CreateTransientSet(pipeline, 1)
										.WriteTexture(0, pass.textures[Mask])
										.WriteSampler(1, state.clampSampler)
										.WriteTexture(2, pass.textures[MaskDepth])
										.WriteSampler(3, state.clampSampler)
										.WriteTexture(4, pass.textures[SceneDepth])
										.WriteSampler(5, state.clampSampler)
										.Commit();
			exec.cmd->BindTransientSet(1, set);

			GPUOutlinePush push;
			for (u32 index = 0; index < pending.outlineColorCount; ++index) {
				push.colors[index] = pending.outlineColors[index].ToU32();
			}
			push.width = pending.outlineWidth;
			push.hiddenOpacity = pending.outlineHiddenOpacity;
			exec.cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			exec.cmd->Draw(3);
			++exec.drawCalls;
			exec.cmd->EndRenderPass();
		}

		// must match ShalltearDebugOverlayPush in DebugOverlay.hlsl
		struct GPUDebugOverlayPush {
			f32 rect[4];
			u32 mode = 0;
			u32 tilesPerAxis = 0;
			f32 sidePx = 0.0f;
			f32 pad = 0.0f;
		};
		static_assert(sizeof(GPUDebugOverlayPush) == 32, "1 clip rect + 2 scalars (under the 128 B budget)");

		constexpr f32 DEBUG_OVERLAY_FRACTION = 0.3f;
		constexpr f32 DEBUG_OVERLAY_MARGIN = 0.02f;

		void PassRecordDebugOverlay(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Atlas = 0,
						 Destination = 1 };
			RenderState& state = *_state;
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			ShaderAsset* shader = state.debugOverlayShader.Get();
			RHI::PipelineHandle pipeline =
				shader ? GetOrCreatePipeline(shader, SCENE_COLOR_FORMAT, RHI::Format::Unknown, nullptr, state.msaaSamples)
					   : RHI::PipelineHandle{};
			if (!pipeline.IsValid() || !pending.viewAllocation.IsValid()) {
				return;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Destination];
			attachment.load = RHI::LoadOp::Load;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->SetPipeline(pipeline);
			exec.cmd->BindTransientSet(0, CreateViewSet(*exec.device, pipeline, *shader, pending.viewAllocation,
														state.DirectionalShadowTexture(), state.punctualShadowAtlas));

			GPUDebugOverlayPush push;
			push.mode = static_cast<u32>(pending.debugOverlay);
			Vec2I extent{1, 1};
			auto targetIt = state.viewTargets.find(pending.targetId);
			if (targetIt != state.viewTargets.end()) {
				extent = state.ViewRenderSize(pending, targetIt->second.size);
			}
			const f32 width = static_cast<f32>(Math::Max(extent.x, 1));
			const f32 height = static_cast<f32>(Math::Max(extent.y, 1));
			const f32 sidePx = Math::Min(width, height) * DEBUG_OVERLAY_FRACTION;
			const f32 marginPx = Math::Min(width, height) * DEBUG_OVERLAY_MARGIN;
			push.sidePx = sidePx;
			push.rect[0] = marginPx / width * 2.0f - 1.0f;
			push.rect[1] = 1.0f - marginPx / height * 2.0f;
			push.rect[2] = (marginPx + sidePx) / width * 2.0f - 1.0f;
			push.rect[3] = 1.0f - (marginPx + sidePx) / height * 2.0f;
			if (pending.debugOverlay == DebugOverlay::DirectionalAtlas && pending.cascadeCount > 0) {
				push.tilesPerAxis = GraphicsInternal::CascadeTilesPerAxis(pending.cascadeCount);
			}
			exec.cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			exec.cmd->Draw(6);
			++exec.drawCalls;
			exec.cmd->EndRenderPass();
		}

		void PassRecordResolve(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Source = 0,
						 Destination = 1 };
			exec.cmd->ResolveTexture(pass.textures[Source], pass.textures[Destination]);
		}

		void PassRecordPresent(GraphExecution& exec, const GraphPass& pass, RHI::PipelineHandle pipeline, TonemapCurve curve) {
			enum : u32 { Source = 0,
						 Destination = 1 };
			RenderState& state = *_state;
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Destination];
			attachment.load = RHI::LoadOp::DontCare;
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->SetPipeline(pipeline);
			RHI::TransientSet set = exec.device->CreateTransientSet(pipeline, 1)
										.WriteTexture(0, pass.textures[Source])
										.WriteSampler(1, state.clampSampler)
										.Commit();
			exec.cmd->BindTransientSet(1, set);
			GPUPresentPush push;
			push.tonemap = static_cast<u32>(curve);
			exec.cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(GPUPresentPush)});
			exec.cmd->Draw(3);
			++exec.drawCalls;
			exec.cmd->EndRenderPass();
		}

		void PassRecordViewPresent(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			ShaderAsset* presentShader = state.presentShader.Get();
			RHI::PipelineHandle pipeline = presentShader
											   ? GetOrCreatePipeline(presentShader, RHI::Format::RGBA8Unorm, RHI::Format::Unknown)
											   : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return;
			}
			const RenderState::PendingView& pending = state.pendingViews[pass.work.index];
			const bool displayingIntermediate = pending.debugView != DebugView::None;
			PassRecordPresent(exec, pass, pipeline, displayingIntermediate || !pending.tonemap ? TonemapCurve::None : state.tonemap);
		}

		void PassRecordOffscreen(GraphExecution& exec, const GraphPass& pass) {
			(void)pass;
			RenderState& state = *_state;
			if (state.overlay) {
				exec.drawCalls += state.overlay->RecordOffscreen(*exec.cmd);
			}
		}

		void PassRecordWindowShadow(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Atlas = 0,
						 PunctualAtlas = 1 };
			RenderState& state = *_state;
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			const RenderState::CameraWork& cameraWork = work.cameras[pass.work.subIndex];
			Camera* camera = cameraWork.camera;
			SceneObject* cameraOwner = camera ? camera->GetOwner().Get() : nullptr;
			if (!cameraOwner) {
				return;
			}
			const f32 aspect = CameraAspect(cameraWork);
			Mat4 cameraWorld = cameraOwner->GetWorldMatrix();
			const ViewRecordScope viewScope(cameraWork);
			exec.drawCalls += RecordShadowPass(*exec.cmd, *state.extractionArena, cameraWorld.Inverse(),
											   camera->BuildProjection(aspect), cameraOwner->GetWorldPosition(),
											   pass.textures[Atlas], pass.textures[PunctualAtlas],
											   ShadowOwnerToken(pass.work), exec.triangles, {}, SceneKind::World, nullptr, nullptr,
											   {state.proceduralVisible.data() + cameraWork.firstCaster, cameraWork.casterCount},
											   camera->GetCullingMask());
		}

		struct GPUDepthRangePush {
			u32 resultOffset = 0;
			f32 a = 0.0f;
			f32 b = 0.0f;
			u32 width = 0;
			u32 height = 0;
			u32 orthographic = 0;
		};
		constexpr u32 DEPTH_RANGE_TILE = 64;

		void RecordDepthRange(GraphExecution& exec, RHI::TextureHandle depth, const Mat4& projection, u32 slot) {
			RenderState& state = *_state;
			ShaderAsset* kernel = state.msaaSamples > 1 ? state.depthRangeShaderMultisampled.Get() : state.depthRangeShader.Get();
			if (!kernel || !depth.IsValid() || slot >= RenderState::DEPTH_RANGE_SLOTS || !state.depthRangeScratch.IsValid()) {
				return;
			}
			const RHI::PipelineHandle pipeline =
				GetOrCreatePipeline(kernel, RHI::Format::Unknown, RHI::Format::Unknown, nullptr, 1, SHADER_BASE_VARIANT, ShaderProgramKind::Compute);
			if (!pipeline.IsValid()) {
				return;
			}
			const Vec2I size = exec.device->GetTextureSize(depth);
			const u64 offset = static_cast<u64>(slot) * 2 * sizeof(f32);
			GPUDepthRangePush push;
			push.resultOffset = static_cast<u32>(offset);
			push.a = projection(2, 2);
			push.b = projection(2, 3);
			push.width = static_cast<u32>(Math::Max(size.x, 0));
			push.height = static_cast<u32>(Math::Max(size.y, 0));
			push.orthographic = projection(3, 2) == 0.0f ? 1u : 0u;
			RHI::CommandList& cmd = *exec.cmd;
			cmd.TransitionBuffer(state.depthRangeScratch, RHI::ResourceState::TransferDst);
			cmd.CopyBufferRegion(state.depthRangeSeed, state.depthRangeScratch, 0, offset, 2 * sizeof(f32));
			cmd.TransitionBuffer(state.depthRangeScratch, RHI::ResourceState::UnorderedAccess);
			cmd.SetPipeline(pipeline);
			RHI::TransientSetWriter writer = exec.device->CreateTransientSet(pipeline, 0);
			writer.WriteTexture(0, depth);
			writer.WriteStorageBuffer(1, state.depthRangeScratch, 0, RHI::WHOLE_BUFFER);
			cmd.BindTransientSet(0, writer.Commit());
			cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			cmd.Dispatch((push.width + DEPTH_RANGE_TILE - 1) / DEPTH_RANGE_TILE, (push.height + DEPTH_RANGE_TILE - 1) / DEPTH_RANGE_TILE);
			cmd.TransitionBuffer(state.depthRangeScratch, RHI::ResourceState::TransferSrc);
			cmd.TransitionBuffer(state.depthRangeRing, RHI::ResourceState::TransferDst);
			cmd.CopyBufferRegion(state.depthRangeScratch, state.depthRangeRing, offset, offset, 2 * sizeof(f32));
		}

		void PassRecordViewDepthRange(GraphExecution& exec, const GraphPass& pass) {
			const RenderState::PendingView& pending = _state->pendingViews[pass.work.index];
			RecordDepthRange(exec, pass.textures[0], pending.projection, pass.work.element);
		}

		void PassRecordWindowDepthRange(GraphExecution& exec, const GraphPass& pass) {
			const RenderState::CameraWork& cameraWork = _state->frameWork[pass.work.index].cameras[pass.work.subIndex];
			if (cameraWork.camera) {
				RecordDepthRange(exec, pass.textures[0], cameraWork.camera->BuildProjection(CameraAspect(cameraWork)), pass.work.element);
			}
		}

		void PassRecordWindowScene(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Color = 0,
						 Depth = 1 };
			RenderState& state = *_state;
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			const RenderState::CameraWork& cameraWork = work.cameras[pass.work.subIndex];
			Camera* camera = cameraWork.camera;
			SceneObject* cameraOwner = camera->GetOwner().Get();
			if (!cameraOwner) {
				return;
			}
			const Vec2I renderSize = cameraWork.pixels.size;
			const f32 aspect = CameraAspect(cameraWork);

			PassTargets targets;
			targets.color = pass.textures[Color];
			targets.depth = pass.textures[Depth];
			if (cameraWork.inlineResolve) {
				const ScenePassSlots slots = ResolveScenePassSlots(pass.work.produced, cameraWork.sceneReads, true);
				targets.resolveColor = pass.textures[slots.inlineResolve];
			}
			targets.clear = pass.work.flag;
			targets.drawSky = pass.work.flag && camera->GetBackgroundType() == BackgroundType::Sky;
			targets.discardColor = cameraWork.discardColor;
			targets.discardDepth = cameraWork.discardDepth;
			targets.samples = state.msaaSamples;
			targets.extent = renderSize;
			Mat4 cameraWorld = cameraOwner->GetWorldMatrix();
			const ViewRecordScope viewScope(cameraWork);
			exec.drawCalls += RecordScenePass(*exec.cmd, *state.extractionArena, cameraWorld.Inverse(),
											  camera->BuildProjection(aspect), cameraOwner->GetWorldPosition(),
											  camera->GetClearColor(), targets, exec.triangles, {}, ViewFill::Solid,
											  Color32::White(), SceneKind::World, ShadowOwnerToken(pass.work),
											  camera->GetCullingMask(), nullptr, ScenePassOcclusion(pass), nullptr,
											  {state.proceduralVisible.data() + cameraWork.firstProcedural, cameraWork.proceduralCount},
											  ScenePassReadsOf(pass, cameraWork.sceneReads));
		}

		void PassRecordCameraComposite(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Source = 0,
						 Destination = 1,
						 Depth = 2 };
			RenderState& state = *_state;
			const RenderState::CameraWork& cameraWork = state.frameWork[pass.work.index].cameras[pass.work.subIndex];
			const bool firstOnChain = pass.work.flag;
			if (firstOnChain) {
				RHI::RenderPassDesc::DepthAttachment depth;
				depth.texture = pass.textures[Depth];
				depth.load = RHI::LoadOp::Clear;
				depth.clearDepth = 0.0f;
				RHI::RenderPassDesc clearDesc;
				clearDesc.depth = &depth;
				exec.cmd->BeginRenderPass(clearDesc);
				exec.cmd->EndRenderPass();
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Destination];
			attachment.load = firstOnChain ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
			attachment.clearColor = Color::Black();
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			ShaderAsset* blit = state.blitShader.Get();
			RHI::PipelineHandle pipeline =
				blit ? GetOrCreatePipeline(blit, SCENE_COLOR_FORMAT, RHI::Format::Unknown, nullptr, state.msaaSamples)
					 : RHI::PipelineHandle{};
			if (pipeline.IsValid()) {
				exec.cmd->SetViewportScissor(cameraWork.pixels);
				exec.cmd->SetPipeline(pipeline);
				RHI::TransientSet set = exec.device->CreateTransientSet(pipeline, 1)
											.WriteTexture(0, pass.textures[Source])
											.WriteSampler(1, state.clampSampler)
											.Commit();
				exec.cmd->BindTransientSet(1, set);
				exec.cmd->Draw(3);
				++exec.drawCalls;
			}
			exec.cmd->EndRenderPass();
		}

		void PassRecordTargetPresent(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			ShaderAsset* presentShader = state.presentShader.Get();
			RHI::PipelineHandle pipeline = presentShader
											   ? GetOrCreatePipeline(presentShader, RHI::Format::RGBA8Unorm, RHI::Format::Unknown)
											   : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return;
			}
			PassRecordPresent(exec, pass, pipeline, state.tonemap);
		}

		void PassRecordWindowPresent(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			auto contextIt = state.windowContexts.find(pass.work.key);
			if (contextIt == state.windowContexts.end()) {
				return;
			}
			ShaderAsset* presentShader = state.presentShader.Get();
			RHI::PipelineHandle pipeline = presentShader
											   ? GetOrCreatePipeline(presentShader, contextIt->second.swapChain->GetFormat(), RHI::Format::Unknown)
											   : RHI::PipelineHandle{};
			if (!pipeline.IsValid()) {
				return;
			}
			PassRecordPresent(exec, pass, pipeline, state.tonemap);
			contextIt->second.presentedThisFrame = true;
		}

		void PassRecordWindowClear(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Backbuffer = 0 };
			RenderState& state = *_state;
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Backbuffer];
			attachment.load = RHI::LoadOp::Clear;
			attachment.clearColor = Color::Black();
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			exec.cmd->EndRenderPass();
			auto contextIt = state.windowContexts.find(pass.work.key);
			if (contextIt != state.windowContexts.end()) {
				contextIt->second.presentedThisFrame = true;
			}
		}

		void PassRecordFrameCapture(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			if (pass.work.subIndex >= state.frameCaptures.size()) {
				return;
			}
			RenderState::FrameCapture& capture = state.frameCaptures[pass.work.subIndex];
			RHI::Device& device = *state.device;
			const RHI::TextureHandle source = pass.textures[0];
			const Vec2I size = device.GetTextureSize(source);
			const RHI::Format format = device.GetTextureFormat(source);
			const bool copyable = HasAnyFlag(device.GetTextureUsage(source), RHI::TextureUsage::TransferSrc);
			const bool rgba8 = format == RHI::Format::RGBA8Unorm || format == RHI::Format::RGBA8SRGB ||
							   format == RHI::Format::BGRA8Unorm || format == RHI::Format::BGRA8SRGB;
			if (capture.copied || !copyable || !rgba8 || size.x <= 0 || size.y <= 0) {
				capture.failed = !capture.copied;
				return;
			}
			RHI::BufferDesc desc;
			desc.size = static_cast<u64>(size.x) * static_cast<u64>(size.y) * 4;
			desc.usage = RHI::BufferUsage::TransferDst;
			desc.access = RHI::MemoryAccess::Readback;
			desc.debugName = "FrameCapture";
			capture.readback = device.CreateBufferResource(desc);
			if (!capture.readback.IsValid()) {
				capture.failed = true;
				return;
			}
			exec.cmd->CopyTextureToBuffer(source, capture.readback);
			capture.size = size;
			capture.format = format;
			capture.copied = true;
			capture.copiedFrame = state.renderedFrames;
		}

		void PassRecordOverlay(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Backbuffer = 0 };
			RenderState& state = *_state;
			if (!state.overlay) {
				return;
			}
			RenderState::WindowWork& work = state.frameWork[pass.work.index];
			auto contextIt = state.windowContexts.find(pass.work.key);
			if (contextIt == state.windowContexts.end()) {
				return;
			}
			SLT_PROFILE_ZONE("Render.Overlay");
			exec.drawCalls += state.overlay->RecordOverlay(work.window, *exec.cmd, pass.textures[Backbuffer],
														   contextIt->second.frameSize);
		}

		void PassRecordWindowLetterbox(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Frame = 0,
						 Backbuffer = 1 };
			RenderState& state = *_state;
			auto contextIt = state.windowContexts.find(pass.work.key);
			if (contextIt == state.windowContexts.end()) {
				return;
			}
			RenderState::WindowContext& context = contextIt->second;
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = pass.textures[Backbuffer];
			attachment.load = RHI::LoadOp::Clear;
			attachment.clearColor = Color::Black();
			RHI::RenderPassDesc desc;
			desc.colorAttachments = {&attachment, 1};
			exec.cmd->BeginRenderPass(desc);
			ShaderAsset* blit = state.blitShader.Get();
			RHI::PipelineHandle pipeline = blit ? GetOrCreatePipeline(blit, context.swapChain->GetFormat(), RHI::Format::Unknown)
												: RHI::PipelineHandle{};
			if (pipeline.IsValid()) {
				exec.cmd->SetViewportScissor(WindowInternal::LetterboxRect(context.swapChain->GetSize(), context.frameSize));
				exec.cmd->SetPipeline(pipeline);
				RHI::TransientSet set = exec.device->CreateTransientSet(pipeline, 1)
											.WriteTexture(0, pass.textures[Frame])
											.WriteSampler(1, state.clampSampler)
											.Commit();
				exec.cmd->BindTransientSet(1, set);
				exec.cmd->Draw(3);
				++exec.drawCalls;
			}
			exec.cmd->EndRenderPass();
			context.presentedThisFrame = true;
		}

		void SceneChainDescs(Vec2I size, u32 samples,
							 RHI::TextureDesc& outHDR, RHI::TextureDesc& outHDRMultisample, RHI::TextureDesc& outDepth) {
			outHDR = {};
			outHDR.size = size;
			outHDR.format = SCENE_COLOR_FORMAT;
			outHDR.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
			if (samples > 1) {
				outHDR.usage = outHDR.usage | RHI::TextureUsage::TransferDst;
			}
			outHDR.debugName = "ViewTargetHDR";

			outHDRMultisample = {};
			outHDRMultisample.size = size;
			outHDRMultisample.format = SCENE_COLOR_FORMAT;
			outHDRMultisample.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
			outHDRMultisample.samples = samples;
			outHDRMultisample.debugName = "ViewTargetHDRMS";

			outDepth = {};
			outDepth.size = size;
			outDepth.format = SCENE_DEPTH_FORMAT;
			outDepth.usage = RHI::TextureUsage::DepthStencil;
			if (FitsDepth()) {
				outDepth.usage = outDepth.usage | RHI::TextureUsage::Sampled;
			}
			outDepth.samples = samples;
			outDepth.debugName = "ViewTargetDepth";
		}

#if defined(SHALLTEAR_CONFIG_RELEASE)
#define SLT_GRAPH_SITE(variable, name) constexpr u32 variable = 0
#else
#define SLT_GRAPH_SITE(variable, name) static const u32 variable = ::Shalltear::Profiler::RegisterSite(name)
#endif

		void DeclareFrameCaptures(RenderState& state, GraphicsInternal::RenderGraph& graph, Handle<Window> window,
								  ViewTargetId target, GraphResourceId source, const PassWork& work) {
			for (u32 index = 0; index < state.frameCaptures.size(); ++index) {
				const RenderState::FrameCapture& capture = state.frameCaptures[index];
				const bool mine = target != INVALID_VIEW_TARGET ? capture.target == target
																: capture.target == INVALID_VIEW_TARGET && capture.window == window;
				if (!mine || capture.copied || capture.failed || capture.followUp) {
					continue;
				}
				PassWork captureWork = work;
				captureWork.subIndex = index;
				SLT_GRAPH_SITE(captureSite, "Render.Capture");
				graph.AddPass("Render.Capture", captureSite, &PassRecordFrameCapture, captureWork);
				graph.Read(source, RHI::ResourceState::TransferSrc);
				graph.Write(graph.CreateVirtual(true), RHI::ResourceState::Undefined);
			}
		}

		void DeclareProceduralRenderers(RenderState& state) {
			GraphicsInternal::ProceduralFrame& frame = state.procedural;
			frame.renderers.clear();
			frame.items.clear();
			frame.reads.clear();
			frame.steps.clear();
			frame.stepAccesses.clear();
			state.proceduralVisible.clear();
			state.proceduralViewSteps.clear();
			auto declareAll = [&](const Vector<Handle<ProceduralRenderer>>& list, bool isolated) {
				for (size_t i = 0; i < list.size(); ++i) {
					ProceduralRenderer* renderer = list[i].Get();
					const SceneObject* owner = renderer ? renderer->GetOwner().Get() : nullptr;
					if (!owner) {
						continue;
					}
					GraphicsInternal::ProceduralFrame::Renderer& entry = frame.renderers.emplace_back();
					entry.renderer = list[i];
					entry.isolated = isolated;
					entry.sceneId = owner->GetScene().GetInstanceId();
					entry.type = renderer->GetType();
					entry.firstItem = static_cast<u32>(frame.items.size());
					entry.firstStep = static_cast<u32>(frame.steps.size());
					ProceduralPlan plan(GraphicsInternal::ProceduralPlanInit{&frame, static_cast<u32>(frame.renderers.size() - 1)});
					renderer->Declare(plan);
					entry.renderingLayerMask = renderer->GetRenderingLayerMask();
					for (u32 step = entry.firstStep; step < entry.firstStep + entry.stepCount; ++step) {
						if (!frame.steps[step].writes) {
							SLT_LOG_WARN("Graphics", "a procedural renderer's view step {} writes nothing. Dropped", frame.steps[step].index);
						}
					}
				}
			};
			declareAll(state.proceduralRenderers, false);
			declareAll(state.isolatedProceduralRenderers, true);
		}

		bool IsSkippedType(const TypeInfo* type, std::span<const u64> skipped) {
			for (const TypeInfo* walk = type; walk; walk = walk->parent) {
				if (std::find(skipped.begin(), skipped.end(), walk->name.GetValue()) != skipped.end()) {
					return true;
				}
			}
			return false;
		}

		struct ProceduralSlice {
			u32 first = 0;
			u32 count = 0;
			PassRequests reads = PassRequests::None;
			bool opaqueItems = false;
		};

		bool DrawsInPrepass(const GraphicsInternal::ProceduralFrame::Item& item) {
			const MaterialAsset* material = item.material.Get();
			const ShaderAsset* shader = material ? material->shader.Get() : nullptr;
			if (!shader || !DrawsProceduralItem(*shader, *material) || item.castShadows == ShadowCasting::ShadowsOnly ||
				material->ResolveRenderState(shader->renderState).blend != RHI::BlendMode::Opaque) {
				return false;
			}
			const ShaderVariant* code = shader->GetVariant(shader->FindVariant(material->keywordHash));
			return code && code->HasProgram(ShaderProgramKind::Depth);
		}

		ProceduralSlice CullProceduralRenderers(RenderState& state, bool isolated, std::span<const u64> onlyScenes,
												std::span<const u64> skippedTypes, const Mat4& viewProjection, u32 cullingMask,
												const LODViewpoint& from) {
			GraphicsInternal::ProceduralFrame& frame = state.procedural;
			ProceduralSlice slice;
			slice.first = static_cast<u32>(state.proceduralVisible.size());
			if (frame.renderers.empty()) {
				return slice;
			}
			const Frustum frustum = Frustum::FromMatrix(viewProjection);
			for (u32 index = 0; index < static_cast<u32>(frame.renderers.size()); ++index) {
				GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[index];
				if (renderer.isolated != isolated || renderer.itemCount == 0) {
					continue;
				}
				if (!onlyScenes.empty() && std::find(onlyScenes.begin(), onlyScenes.end(), renderer.sceneId) == onlyScenes.end()) {
					continue;
				}
				if (!skippedTypes.empty() && IsSkippedType(renderer.type, skippedTypes)) {
					continue;
				}
				renderer.tested = true;
				if ((renderer.renderingLayerMask & cullingMask) == 0 || (renderer.bounded && !frustum.Intersects(renderer.bounds)) ||
					(state.lodProceduralMembers != 0 && !ProceduralLODDraws(state, from, renderer.renderer.GetInstanceId()))) {
					++state.frameProceduralCulled;
					continue;
				}
				renderer.drawn = true;
				++state.frameProceduralDrawn;
				state.proceduralVisible.push_back(index);
				for (u32 item = renderer.firstItem; item < renderer.firstItem + renderer.itemCount; ++item) {
					const MaterialAsset* material = frame.items[item].material.Get();
					const ShaderAsset* shader = material ? material->shader.Get() : nullptr;
					if (shader) {
						const PassRequests declared = SceneRequests::DeclaredBy(*shader);
						slice.reads |= declared & (PassRequests::SceneDepth | PassRequests::SceneNormals);
						if (material->ResolveRenderState(shader->renderState).blend != RHI::BlendMode::Opaque) {
							slice.reads |= declared & PassRequests::SceneColor;
						}
					}
					slice.opaqueItems = slice.opaqueItems || DrawsInPrepass(frame.items[item]);
				}
			}
			slice.count = static_cast<u32>(state.proceduralVisible.size()) - slice.first;
			return slice;
		}

		ProceduralSlice CollectProceduralCasters(RenderState& state, bool isolated, std::span<const u64> onlyScenes,
												 std::span<const u64> skippedTypes, u32 cullingMask, const LODViewpoint& from) {
			const GraphicsInternal::ProceduralFrame& frame = state.procedural;
			ProceduralSlice slice;
			slice.first = static_cast<u32>(state.proceduralVisible.size());
			for (u32 index = 0; index < static_cast<u32>(frame.renderers.size()); ++index) {
				const GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[index];
				if (renderer.isolated != isolated || (renderer.renderingLayerMask & cullingMask) == 0) {
					continue;
				}
				if (!onlyScenes.empty() && std::find(onlyScenes.begin(), onlyScenes.end(), renderer.sceneId) == onlyScenes.end()) {
					continue;
				}
				if (!skippedTypes.empty() && IsSkippedType(renderer.type, skippedTypes)) {
					continue;
				}
				for (u32 item = renderer.firstItem; item < renderer.firstItem + renderer.itemCount; ++item) {
					if (frame.items[item].castShadows != ShadowCasting::Off) {
						if (state.lodProceduralMembers == 0 || ProceduralLODDraws(state, from, renderer.renderer.GetInstanceId())) {
							state.proceduralVisible.push_back(index);
						}
						break;
					}
				}
			}
			slice.count = static_cast<u32>(state.proceduralVisible.size()) - slice.first;
			return slice;
		}

		PassRequests TransparentSceneReads(const MaterialAsset* material) {
			const ShaderAsset* shader = material ? material->shader.Get() : nullptr;
			if (!shader || !SceneRequests::DeclaresSceneSet(*shader) || ClassifyMeshDraw(*shader, *material) != MeshDrawRefusal::None) {
				return PassRequests::None;
			}
			return SceneRequests::DeclaredBy(*shader);
		}

		void GatherSceneReaders(RenderState& state) {
			state.sceneReaders.clear();
			if (SceneRequests::CountLiveMeshReaders() == 0) {
				return;
			}
			auto gather = [&state](const auto& renderers, bool isolated, bool skinned) {
				for (const auto& handle : renderers) {
					const auto* renderer = handle.Get();
					if (!renderer || renderer->GetCastShadows() == ShadowCasting::ShadowsOnly) {
						continue;
					}
					PassRequests reads = PassRequests::None;
					for (u32 slot = 0; slot < renderer->GetMaterialCount(); ++slot) {
						reads |= TransparentSceneReads(renderer->GetRenderedMaterial(slot).Get());
					}
					const SceneObject* owner = reads != PassRequests::None ? renderer->GetOwner().Get() : nullptr;
					const MeshAsset* mesh = owner ? renderer->GetMesh().Get() : nullptr;
					if (!mesh) {
						continue;
					}
					RenderState::SceneReader reader;
					// posed bounds unknown until Render.Skinning
					reader.bounded = !skinned && mesh->bounds.IsValid();
					if (reader.bounded) {
						reader.bounds = mesh->bounds.Transformed(owner->GetWorldMatrix());
					}
					reader.sceneId = owner->GetScene().GetInstanceId();
					reader.renderingLayerMask = renderer->GetRenderingLayerMask();
					reader.reads = reads;
					reader.isolated = isolated;
					state.sceneReaders.push_back(reader);
				}
			};
			gather(state.meshRenderers, false, false);
			gather(state.skinnedMeshRenderers, false, true);
			gather(state.isolatedMeshRenderers, true, false);
			gather(state.isolatedSkinnedMeshRenderers, true, true);
		}

		PassRequests SceneReadsInView(const RenderState& state, bool isolated, std::span<const u64> onlyScenes, const Mat4& viewProjection,
									  u32 cullingMask) {
			PassRequests reads = PassRequests::None;
			if (state.sceneReaders.empty()) {
				return reads;
			}
			const GraphicsInternal::CullVolume frustum(Frustum::FromMatrix(viewProjection));
			for (const RenderState::SceneReader& reader : state.sceneReaders) {
				if (reader.isolated != isolated || (reader.renderingLayerMask & cullingMask) == 0) {
					continue;
				}
				if (!onlyScenes.empty() && std::find(onlyScenes.begin(), onlyScenes.end(), reader.sceneId) == onlyScenes.end()) {
					continue;
				}
				if (reader.bounded && !frustum.Intersects(reader.bounds)) {
					continue;
				}
				reads |= reader.reads;
			}
			return reads;
		}

		void DeclareProceduralReads(const RenderState& state, GraphicsInternal::RenderGraph& graph, const ProceduralSlice& slice) {
			const GraphicsInternal::ProceduralFrame& frame = state.procedural;
			for (u32 visible = slice.first; visible < slice.first + slice.count; ++visible) {
				const GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[state.proceduralVisible[visible]];
				for (u32 item = renderer.firstItem; item < renderer.firstItem + renderer.itemCount; ++item) {
					const GraphicsInternal::ProceduralFrame::Item& declared = frame.items[item];
					for (u32 read = declared.firstRead; read < declared.firstRead + declared.readCount; ++read) {
						graph.Read(graph.ImportBuffer(frame.reads[read].buffer), frame.reads[read].state);
					}
				}
			}
		}

		void PassRecordProceduralViewStep(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			const RenderState::ProceduralViewStep& entry = state.proceduralViewSteps[pass.work.index];
			ProceduralRenderer* renderer = state.procedural.renderers[entry.renderer].renderer.Get();
			if (!renderer) {
				return;
			}
			ComputeContext context(GraphicsInternal::ComputeContextInit{&exec, &pass, state.procedural.steps[entry.step].index});
			renderer->RecordView(context, entry.view);
		}

		void DeclareProceduralViewSteps(RenderState& state, GraphicsInternal::RenderGraph& graph, const ProceduralSlice& slice,
										const ProceduralView& view) {
			const GraphicsInternal::ProceduralFrame& frame = state.procedural;
			if (frame.steps.empty()) {
				return;
			}
			SLT_GRAPH_SITE(proceduralStepSite, "Render.View.Procedural");
			for (u32 visible = slice.first; visible < slice.first + slice.count; ++visible) {
				const u32 index = state.proceduralVisible[visible];
				const GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[index];
				for (u32 step = renderer.firstStep; step < renderer.firstStep + renderer.stepCount; ++step) {
					const GraphicsInternal::ProceduralFrame::Step& declared = frame.steps[step];
					if (!declared.writes) {
						continue;
					}
					PassWork work;
					work.index = static_cast<u32>(state.proceduralViewSteps.size());
					state.proceduralViewSteps.push_back({index, step, view});
					graph.AddPass("Render.View.Procedural", proceduralStepSite, &PassRecordProceduralViewStep, work);
					for (u32 access = declared.firstAccess; access < declared.firstAccess + declared.accessCount; ++access) {
						const GraphicsInternal::ProceduralFrame::StepAccess& named = frame.stepAccesses[access];
						const GraphResourceId resource = graph.ImportBuffer(named.buffer);
						if (named.write) {
							graph.Write(resource, named.state);
						} else {
							graph.Read(resource, named.state);
						}
					}
				}
			}
		}

		RenderState::CameraDepthRecord* DepthToKeep(RenderState& state, const Camera* camera) {
			if (state.cameraDepths.empty() || !camera) {
				return nullptr;
			}
			for (RenderState::CameraDepthRecord& record : state.cameraDepths) {
				if (record.asked && !record.kept && record.camera.Get() == camera) {
					return &record;
				}
			}
			return nullptr;
		}

		GraphResourceId KeepCameraDepth(RenderState& state, RHI::Device& device, GraphicsInternal::RenderGraph& graph,
										RenderState::CameraDepthRecord& record, Vec2I size, const Mat4& view, const Mat4& projection) {
			if (!record.image.IsValid() || record.size != size) {
				if (record.image.IsValid()) {
					state.retiredCameraDepths.push_back(record.image);
				}
				RHI::TextureDesc desc;
				desc.size = size;
				desc.format = SCENE_DEPTH_FORMAT;
				desc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled;
				desc.debugName = "CameraDepth";
				record.image = device.CreateTextureResource(desc);
				record.size = size;
				record.drawn = false;
				if (!record.image.IsValid()) {
					return GraphicsInternal::INVALID_GRAPH_RESOURCE;
				}
			}
			record.kept = true;
			record.drawn = true;
			record.view = view;
			record.projection = projection;
			return graph.Import(record.image, true, RHI::ResourceState::ShaderResource);
		}

		struct PrepassImages {
			GraphResourceId target = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			GraphResourceId read = GraphicsInternal::INVALID_GRAPH_RESOURCE;
			bool items = false;
			bool itemsOverCopy = false;
		};

		PrepassImages DeclarePrepassImages(RenderState& state, RHI::Device& device, GraphicsInternal::RenderGraph& graph,
										   RenderState::CameraDepthRecord* kept, Vec2I size, const Mat4& view, const Mat4& projection,
										   bool items) {
			PrepassImages out;
			if (kept) {
				out.target = KeepCameraDepth(state, device, graph, *kept, size, view, projection);
			}
			RHI::TextureDesc desc;
			desc.size = size;
			desc.format = SCENE_DEPTH_FORMAT;
			desc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled;
			desc.debugName = "ScenePrepassDepth";
			if (out.target == GraphicsInternal::INVALID_GRAPH_RESOURCE) {
				out.target = graph.CreateTransient(desc);
				out.read = out.target;
				out.items = items;
				return out;
			}
			out.read = out.target;
			if (items && EnsureDepthCopyShader()) {
				out.read = graph.CreateTransient(desc);
				out.itemsOverCopy = true;
			}
			return out;
		}

		void DeclarePrepassPasses(RenderState& state, GraphicsInternal::RenderGraph& graph, const char* name, u32 site,
								  GraphicsInternal::PassRecordFn record, const char* itemsName, u32 itemsSite,
								  GraphicsInternal::PassRecordFn recordItems, PassWork work, const PrepassImages& images,
								  GraphResourceId normals, GraphResourceId skin, const ProceduralSlice& procedural) {
			const bool hasNormals = normals != GraphicsInternal::INVALID_GRAPH_RESOURCE;
			work.flag = hasNormals;
			work.produced = images.items;
			graph.AddPass(name, site, record, work);
			if (hasNormals) {
				graph.Write(normals, RHI::ResourceState::RenderTarget);
			}
			graph.Write(images.target, RHI::ResourceState::DepthWrite);
			graph.Read(skin, RHI::ResourceState::ShaderResource);
			if (images.items) {
				DeclareProceduralReads(state, graph, procedural);
			}
			if (images.itemsOverCopy) {
				work.produced = false;
				graph.AddPass(itemsName, itemsSite, recordItems, work);
				graph.Read(images.target, RHI::ResourceState::ShaderResource);
				graph.Write(images.read, RHI::ResourceState::DepthWrite);
				if (hasNormals) {
					graph.Write(normals, RHI::ResourceState::RenderTarget);
				}
				DeclareProceduralReads(state, graph, procedural);
			}
		}

		void AgeCameraDepths(RenderState& state, RHI::Device& device) {
			for (RHI::TextureHandle image : state.retiredCameraDepths) {
				device.DestroyResource(image);
			}
			state.retiredCameraDepths.clear();
			for (size_t i = 0; i < state.cameraDepths.size();) {
				RenderState::CameraDepthRecord& record = state.cameraDepths[i];
				record.idleFrames = record.asked ? 0u : record.idleFrames + 1u;
				if (record.camera.Get() && record.idleFrames <= GraphicsInternal::RenderGraph::TRANSIENT_IDLE_FRAMES) {
					++i;
					continue;
				}
				if (record.image.IsValid()) {
					device.DestroyResource(record.image);
				}
				state.cameraDepths.erase(state.cameraDepths.begin() + static_cast<std::ptrdiff_t>(i));
			}
		}

		constexpr f32 SKY_CAPTURE_NEAR = 0.1f;
		constexpr f32 SKY_CAPTURE_FAR = 10.0f;

		u64 SkyCaptureKey(RenderState& state, ShaderAsset& shader, u32 variant, MaterialAsset& material, const RenderState::SkyCapture& capture) {
			auto mix = [](u64 hash, u64 value) { return (hash ^ value) * 0x100000001B3ull; };
			u64 key = 0xCBF29CE484222325ull;
			key = mix(key, shader.GetInstanceId());
			key = mix(key, shader.version);
			key = mix(key, variant);
			key = mix(key, material.GetInstanceId());
			key = mix(key, capture.faceSize);
			DrawEntry probe;
			PackMaterialBlock(probe, *state.extractionArena, &shader, &material);
			for (u32 i = 0; i < probe.materialBlockSize; ++i) {
				key = mix(key, static_cast<const u8*>(probe.materialBlock)[i]);
			}
			ForEachMaterialTextureSlot(shader, [&](const ShaderResourceInfo& slot, u32) {
				const TextureAsset* texture = material.GetTexture(StringHash(std::string_view{slot.name})).Get();
				key = mix(key, texture ? texture->GetInstanceId() : 0);
				key = mix(key, texture ? texture->version : 0);
			});
			const f32 sun[8] = {capture.sunDirection.x, capture.sunDirection.y, capture.sunDirection.z,
								capture.sunColor.x, capture.sunColor.y, capture.sunColor.z,
								capture.sunCosSize, capture.sunAngularDiameter};
			for (f32 value : sun) {
				u32 bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				key = mix(key, bits);
			}
			return key | 1ull;
		}

		bool AllocateSkyCapture(RHI::Device& device, RenderState::SkyCapture& capture, u32 faceSize) {
			ReleaseSkyCapture(device, capture);
			capture.faceSize = faceSize;
			capture.sourceMips = GraphicsInternal::BoxChainMipCount(faceSize);
			capture.chainMips = GraphicsInternal::RadianceCubeMipCount(faceSize);
			capture.shMip = GraphicsInternal::IrradianceSourceMip(faceSize);
			capture.shSide = Math::Max(faceSize >> capture.shMip, 1u);

			RHI::TextureDesc desc;
			desc.size = {static_cast<i32>(faceSize), static_cast<i32>(faceSize)};
			desc.format = RHI::Format::RGBA16Float;
			desc.arrayLayers = GraphicsInternal::CUBE_FACE_COUNT;
			desc.cube = true;
			desc.mipLevels = capture.sourceMips;
			desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc |
						 RHI::TextureUsage::TransferDst;
			desc.debugName = "SkyCapture";
			capture.source = device.CreateTextureResource(desc);
			desc.mipLevels = capture.chainMips;
			desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
			desc.debugName = "SkyLight";
			capture.chain = device.CreateTextureResource(desc);

			RHI::BufferDesc readback;
			readback.size = static_cast<u64>(capture.shSide) * capture.shSide * 8 * GraphicsInternal::CUBE_FACE_COUNT * RHI::FRAMES_IN_FLIGHT;
			readback.usage = RHI::BufferUsage::TransferDst;
			readback.access = RHI::MemoryAccess::Readback;
			readback.debugName = "SkyIrradiance";
			capture.readback = device.CreateBufferResource(readback);
			return capture.source.IsValid() && capture.chain.IsValid() && capture.readback.IsValid();
		}

		void PassRecordSkyCapture(GraphExecution& exec, const GraphPass& pass);
		void PassRecordSkyPrefilter(GraphExecution& exec, const GraphPass& pass);

		MaterialAsset* SkyMaterialOf(Environment* environment) {
			if (!environment || !environment->GetOwner().Get() || environment->GetSkyMode() != SkyMode::Material) {
				return nullptr;
			}
			MaterialAsset* material = environment->GetSkyMaterial().Get();
			ShaderAsset* shader = material ? material->shader.Get() : nullptr;
			return shader && shader->integrator == ShaderTemplate::Sky ? material : nullptr;
		}

		void DeclareSkyCaptures(RenderState& state, RHI::Device& device, GraphicsInternal::RenderGraph& graph, u32 workCount) {
			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				capture.seen = false;
				capture.writeSlot = RHI::FRAMES_IN_FLIGHT;
			}
			for (const Vector<Handle<Environment>>* list : {&state.environments, &state.isolatedEnvironments}) {
				for (Handle<Environment> handle : *list) {
					if (!SkyMaterialOf(handle.Get())) {
						continue;
					}
					RenderState::SkyCapture* capture = FindSkyCapture(state, handle);
					if (!capture) {
						state.skyCaptures.push_back(RenderState::SkyCapture{});
						capture = &state.skyCaptures.back();
						capture->environment = handle;
					}
					capture->seen = true;
				}
			}
			for (size_t i = 0; i < state.skyCaptures.size();) {
				if (!state.skyCaptures[i].seen) {
					ReleaseSkyCapture(device, state.skyCaptures[i]);
					state.skyCaptures.erase(state.skyCaptures.begin() + static_cast<ptrdiff_t>(i));
				} else {
					++i;
				}
			}
			for (size_t i = 0; i < state.bakedSkies.size();) {
				const RenderState::BakedSky& baked = state.bakedSkies[i];
				Environment* environment = baked.environment.Get();
				const bool live = environment &&
								  (ContainsHandle(state.environments, baked.environment) || ContainsHandle(state.isolatedEnvironments, baked.environment)) &&
								  (environment->GetSkyMode() == SkyMode::Gradient || environment->GetSkyMode() == SkyMode::Color);
				if (live) {
					++i;
					continue;
				}
				if (baked.cube.IsValid()) {
					device.DestroyResource(baked.cube);
				}
				state.bakedSkies.erase(state.bakedSkies.begin() + static_cast<ptrdiff_t>(i));
			}

			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				capture.reached = false;
				capture.hasClock = false;
				capture.clock = 0.0;
				capture.viewExposure = 0.0f;
			}
			auto reach = [&](SceneKind kind, std::span<const u64> viewScenes, const Vec3& cameraPosition, const f64* clock, f32 exposure) {
				EnvironmentLayers layers;
				GatherEnvironmentLayers(state, kind, viewScenes, cameraPosition, layers);
				for (u32 i = 0; i < layers.count; ++i) {
					RenderState::SkyCapture* capture = layers.weights[i] > 0.0f ? FindSkyCapture(state, Handle<Environment>(layers.layers[i])) : nullptr;
					if (!capture) {
						continue;
					}
					capture->reached = true;
					if (!capture->hasClock && clock) {
						capture->hasClock = true;
						capture->clock = *clock;
					}
					if (capture->viewExposure <= 0.0f) {
						capture->viewExposure = exposure;
					}
				}
			};
			if (!state.skyCaptures.empty()) {
				for (const RenderState::PendingView& pending : state.pendingViews) {
					reach(pending.kind, {state.viewScenes.data() + pending.firstScene, pending.sceneCount}, pending.cameraPosition,
						  pending.hasTime ? &pending.time : nullptr, state.passScopes[pending.scopeIndex].exposure);
				}
				for (u32 w = 0; w < workCount; ++w) {
					for (const RenderState::CameraWork& cameraWork : state.frameWork[w].cameras) {
						const SceneObject* cameraOwner = cameraWork.camera ? cameraWork.camera->GetOwner().Get() : nullptr;
						if (cameraOwner) {
							reach(SceneKind::World, {}, cameraOwner->GetWorldPosition(), nullptr, state.passScopes[cameraWork.scopeIndex].exposure);
						}
					}
				}
			}

			SLT_GRAPH_SITE(skyCaptureSite, "Render.Sky.Capture");
			SLT_GRAPH_SITE(skyPrefilterSite, "Render.Sky.Prefilter");
			for (const SceneKind kind : {SceneKind::World, SceneKind::Isolated}) {
				const Vector<Handle<Environment>>& list = kind == SceneKind::World ? state.environments : state.isolatedEnvironments;
				for (Handle<Environment> handle : list) {
					Environment* environment = handle.Get();
					MaterialAsset* material = SkyMaterialOf(environment);
					RenderState::SkyCapture* capture = material ? FindSkyCapture(state, handle) : nullptr;
					if (!capture || !capture->reached) {
						continue;
					}
					SceneObject* owner = environment->GetOwner().Get();
					const Handle<MaterialAsset> materialHandle(material);
					ShaderAsset* shader = material->shader.Get();
					const u32 faceSize = SkyCaptureFaceSize(environment->GetReflectionResolution());
					if (capture->faceSize != faceSize || !capture->source.IsValid()) {
						if (!AllocateSkyCapture(device, *capture, faceSize)) {
							continue;
						}
					}

					const u64 sceneId = owner->GetScene().GetInstanceId();
					const std::span<const u64> onlyScenes = kind == SceneKind::World ? std::span<const u64>{} : std::span<const u64>{&sceneId, 1};
					u32 directionalCount = 0;
					u32 dropped = 0;
					ExtractLightCandidates(Mat4::Identity(), Mat4::PerspectiveReverseZ(90.0f, 1.0f, SKY_CAPTURE_NEAR, SKY_CAPTURE_FAR),
										   Vec3{}, kind, onlyScenes, directionalCount, dropped);
					capture->sunDirection = Vec3{0.0f, 1.0f, 0.0f};
					capture->sunColor = Vec3{};
					if (directionalCount > 0 && !state.lightCandidates.empty()) {
						capture->sunDirection = -state.lightCandidates[0].direction;
						const Color tint = environment->GetSunTint();
						capture->sunColor = state.lightCandidates[0].color * Vec3{tint.r, tint.g, tint.b} *
											Math::Max(environment->GetSunIntensityMultiplier(), 0.0f);
					}
					const f32 discDeg = environment->GetSunDiscSizeDeg();
					capture->sunAngularDiameter = Math::DEG_TO_RAD * Math::Max(discDeg, 0.0f);
					capture->sunCosSize = discDeg > 0.0f && capture->sunColor.LengthSquared() > 0.0f
											  ? std::cos(Math::DEG_TO_RAD * discDeg * 0.5f)
											  : 2.0f;

					const u32 variant = shader->FindVariant(material->keywordHash);
					const u64 key = SkyCaptureKey(state, *shader, variant, *material, *capture);
					const u32 requests = environment->GetSkyUpdateRequests();
					const SkyUpdateMode mode = environment->GetSkyUpdateMode();
					const bool update = !capture->filled || mode == SkyUpdateMode::Realtime ||
										(mode == SkyUpdateMode::OnChanged && key != capture->key) ||
										(mode == SkyUpdateMode::OnDemand && requests != capture->servedRequests);
					if (!update || !EnsureSkyPrefilterShader()) {
						continue;
					}
					capture->key = key;
					capture->servedRequests = requests;
					capture->material = materialHandle;
					const u32 slot = static_cast<u32>(state.renderedFrames % RHI::FRAMES_IN_FLIGHT);
					capture->writeSlot = capture->slotPending[slot] ? RHI::FRAMES_IN_FLIGHT : slot;

					PassWork work;
					work.index = static_cast<u32>(capture - state.skyCaptures.data());
					const GraphResourceId source = graph.Import(capture->source);
					const GraphResourceId chain = graph.Import(capture->chain, true, RHI::ResourceState::ShaderResource);
					const GraphResourceId irradiance = graph.CreateVirtual(true);
					graph.AddPass("Render.Sky.Capture", skyCaptureSite, &PassRecordSkyCapture, work);
					graph.Write(source, RHI::ResourceState::RenderTarget);
					graph.Write(irradiance, RHI::ResourceState::Undefined);
					graph.AddPass("Render.Sky.Prefilter", skyPrefilterSite, &PassRecordSkyPrefilter, work);
					graph.Read(source, RHI::ResourceState::ShaderResource);
					graph.Write(chain, RHI::ResourceState::RenderTarget);
				}
			}
		}

		void PassRecordSkyCapture(GraphExecution& exec, const GraphPass& pass) {
			RenderState& state = *_state;
			RenderState::SkyCapture& capture = state.skyCaptures[pass.work.index];
			MaterialAsset* material = capture.material.Get();
			ShaderAsset* shader = material ? material->shader.Get() : nullptr;
			if (!shader) {
				return;
			}
			RHI::CommandList& cmd = *exec.cmd;
			RHI::Device& device = *exec.device;
			RHI::ShaderRenderState captureState = shader->renderState;
			captureState.depth = RHI::DepthMode::Off;
			const u32 variant = shader->FindVariant(material->keywordHash);
			RHI::PipelineHandle pipeline = GetOrCreatePipeline(shader, RHI::Format::RGBA16Float, RHI::Format::Unknown, &captureState, 1, variant);
			if (!pipeline.IsValid()) {
				return;
			}
			DrawEntry entry;
			PackMaterialBlock(entry, *state.extractionArena, shader, material);
			ResolveMaterialTextures(entry, *state.extractionArena, shader, material);

			const Mat4 projection = Mat4::PerspectiveReverseZ(90.0f, 1.0f, SKY_CAPTURE_NEAR, SKY_CAPTURE_FAR);
			const Vec2I extent{static_cast<i32>(capture.faceSize), static_cast<i32>(capture.faceSize)};
			const Environment* owner = capture.environment.Get();
			const f32 intensity = owner && owner->GetSkyIntensity() > 0.0f ? owner->GetSkyIntensity() : 1.0f;
			const f32 exposure = capture.viewExposure > 0.0f ? capture.viewExposure : GraphicsInternal::ReferenceExposure(state.physicalLightUnits);
			capture.chainExposure = exposure * intensity;
			if (capture.writeSlot < RHI::FRAMES_IN_FLIGHT) {
				capture.slotExposure[capture.writeSlot] = capture.chainExposure;
			}
			const ViewRecordScope viewScope(capture.hasClock, capture.clock, WriteExposureWord(device, capture.chainExposure));
			for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
				const Mat4 view = CubeFaceView(face, Vec3{});
				RHI::RenderPassDesc::Attachment attachment;
				attachment.texture = pass.textures[0];
				attachment.layer = face;
				attachment.load = RHI::LoadOp::DontCare;
				RHI::RenderPassDesc desc;
				desc.colorAttachments = {&attachment, 1};
				cmd.BeginRenderPass(desc);
				cmd.SetPipeline(pipeline);

				RHI::TransientAllocation viewAllocation = device.AllocateTransient(sizeof(GPUViewUniforms));
				if (viewAllocation.IsValid()) {
					GPUViewUniforms uniforms{};
					uniforms.viewProjection = projection * view;
					uniforms.renderExtentInverseX = 1.0f / static_cast<f32>(capture.faceSize);
					uniforms.renderExtentInverseY = uniforms.renderExtentInverseX;
					uniforms.viewDataAddress = WriteViewData(device, view, projection, extent);
					std::memcpy(viewAllocation.cpu, &uniforms, sizeof(GPUViewUniforms));
					cmd.BindTransientSet(0, CreateViewSet(device, pipeline, *shader, viewAllocation, {}, {}));
				}
				BindMaterialSet(cmd, device, pipeline, entry);
				GPUSkyPush push;
				push.invViewProjection = (projection * view).Inverse();
				push.sunDirection = capture.sunDirection;
				push.sunCosSize = capture.sunCosSize;
				push.sunColor = capture.sunColor;
				push.skyIntensity = 1.0f;
				push.sunAngularDiameter = capture.sunAngularDiameter;
				push.capturing = 1;
				cmd.PushConstants({reinterpret_cast<const u8*>(&push), SKY_PUSH_BYTES});
				cmd.Draw(3);
				++exec.drawCalls;
				cmd.EndRenderPass();
			}

			cmd.GenerateMips(pass.textures[0]);
			if (capture.writeSlot < RHI::FRAMES_IN_FLIGHT) {
				const u64 faceBytes = static_cast<u64>(capture.shSide) * capture.shSide * 8;
				const u64 slotOffset = faceBytes * GraphicsInternal::CUBE_FACE_COUNT * capture.writeSlot;
				cmd.TransitionTexture(pass.textures[0], RHI::ResourceState::TransferSrc);
				for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					cmd.CopyTextureToBuffer(pass.textures[0], capture.readback, slotOffset + faceBytes * face, capture.shMip, face);
				}
				cmd.TransitionTexture(pass.textures[0], RHI::ResourceState::ShaderResource);
				capture.slotPending[capture.writeSlot] = true;
				capture.slotFrame[capture.writeSlot] = state.renderedFrames;
			}
		}

		// must match ShalltearPrefilterPush in SkyPrefilter.hlsl
		struct GPUPrefilterPush {
			u32 face = 0;
			f32 roughness = 0.0f;
			f32 sourceFaceSize = 0.0f;
			f32 sourceMipCount = 0.0f;
			f32 targetSizeX = 0.0f;
			f32 targetSizeY = 0.0f;
		};
		static_assert(sizeof(GPUPrefilterPush) == 24);

		void PassRecordSkyPrefilter(GraphExecution& exec, const GraphPass& pass) {
			enum : u32 { Source = 0,
						 Chain = 1 };
			RenderState& state = *_state;
			RenderState::SkyCapture& capture = state.skyCaptures[pass.work.index];
			ShaderAsset* shader = state.skyPrefilterShader.Get();
			RHI::PipelineHandle pipeline = shader ? GetOrCreatePipeline(shader, RHI::Format::RGBA16Float, RHI::Format::Unknown) : RHI::PipelineHandle{};
			if (!pipeline.IsValid() || !state.linearClampSampler.IsValid()) {
				return;
			}
			RHI::CommandList& cmd = *exec.cmd;
			RHI::Device& device = *exec.device;
			RHI::TransientSetWriter writer = device.CreateTransientSet(pipeline, 0);
			writer.WriteTexture(0, pass.textures[Source]);
			writer.WriteSampler(1, state.linearClampSampler);
			const RHI::TransientSet set = writer.Commit();
			for (u32 level = 0; level < capture.chainMips; ++level) {
				const u32 side = Math::Max(capture.faceSize >> level, 1u);
				for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					RHI::RenderPassDesc::Attachment attachment;
					attachment.texture = pass.textures[Chain];
					attachment.layer = face;
					attachment.mip = level;
					attachment.load = RHI::LoadOp::DontCare;
					RHI::RenderPassDesc desc;
					desc.colorAttachments = {&attachment, 1};
					cmd.BeginRenderPass(desc);
					cmd.SetPipeline(pipeline);
					cmd.BindTransientSet(0, set);
					GPUPrefilterPush push;
					push.face = face;
					push.roughness = capture.chainMips > 1 ? static_cast<f32>(level) / static_cast<f32>(capture.chainMips - 1) : 0.0f;
					push.sourceFaceSize = static_cast<f32>(capture.faceSize);
					push.sourceMipCount = static_cast<f32>(capture.sourceMips);
					push.targetSizeX = static_cast<f32>(side);
					push.targetSizeY = static_cast<f32>(side);
					cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(GPUPrefilterPush)});
					cmd.Draw(3);
					++exec.drawCalls;
					cmd.EndRenderPass();
				}
			}
			capture.filled = true;
		}

		void PrepareCameraWork(RenderState::WindowWork& work, Vec2I renderSize) {
			for (RenderState::CameraWork& camera : work.cameras) {
				camera.pixels = GraphicsInternal::ViewportPixels(camera.camera->GetViewportRect(), renderSize);
				camera.composited = camera.pixels != RectI(0, 0, renderSize.x, renderSize.y);
			}
			std::erase_if(work.cameras, [](const RenderState::CameraWork& camera) {
				return camera.pixels.size.x <= 0 || camera.pixels.size.y <= 0;
			});
			std::stable_sort(work.cameras.begin(), work.cameras.end(),
							 [](const RenderState::CameraWork& a, const RenderState::CameraWork& b) {
								 return a.camera->GetPriority() < b.camera->GetPriority();
							 });
		}

		void DeclareCameraComposition(RenderState& state, RHI::Device& device, GraphicsInternal::RenderGraph& graph, PassWork work,
									  GraphResourceId sceneTarget, GraphResourceId sceneDepth, GraphResourceId resolvedColor,
									  GraphResourceId worldSkin, GraphResourceId worldSkinBuffer, bool& outResolvedInline) {
			outResolvedInline = false;
			RenderState::WindowWork& windowWork = state.frameWork[work.index];
			GraphResourceId windowShadowAtlas = graph.Import(state.DirectionalShadowTexture());
			GraphResourceId windowPunctualAtlas = graph.Import(state.punctualShadowAtlas);
			const bool windowCasts = PartitionHasShadowCaster(state, SceneKind::World, {});

			SLT_GRAPH_SITE(windowShadowSite, "Render.Window.Shadow");
			SLT_GRAPH_SITE(windowSceneSite, "Render.Window.Scene");
			SLT_GRAPH_SITE(windowRowSite, "Render.Window.Row");
			SLT_GRAPH_SITE(windowCopySite, "Render.Window.ColorCopy");
			SLT_GRAPH_SITE(windowPrepassSite, "Render.Window.Prepass");
			SLT_GRAPH_SITE(windowPrepassItemsSite, "Render.Window.PrepassItems");
			SLT_GRAPH_SITE(cameraResolveSite, "Render.Window.CameraResolve");
			SLT_GRAPH_SITE(compositeSite, "Render.Window.Composite");
			const u32 sceneAnchorKey = RowSortKey(PassPoint::Opaque, FrameStagePosition::Normal);
			bool chainWritten = false;
			for (u32 cameraIndex = 0; cameraIndex < static_cast<u32>(windowWork.cameras.size()); ++cameraIndex) {
				work.subIndex = cameraIndex;

				const RenderState::CameraWork& cameraRows = windowWork.cameras[cameraIndex];
				const Vec2I extent = cameraRows.pixels.size;
				const f32 aspect = CameraAspect(cameraRows);

				RHI::TextureDesc cameraHDRDesc;
				RHI::TextureDesc cameraHDRMultisampledDesc;
				RHI::TextureDesc cameraDepthDesc;
				SceneChainDescs(extent, state.msaaSamples, cameraHDRDesc, cameraHDRMultisampledDesc, cameraDepthDesc);
				GraphResourceId cameraTarget = sceneTarget;
				GraphResourceId cameraDepth = sceneDepth;
				GraphResourceId cameraColor = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				if (cameraRows.composited) {
					cameraHDRDesc.debugName = "CameraColorHDR";
					cameraHDRMultisampledDesc.debugName = "CameraColorHDRMS";
					cameraDepthDesc.debugName = "CameraDepth";
					cameraColor = graph.CreateTransient(cameraHDRDesc);
					cameraTarget = state.msaaSamples > 1 ? graph.CreateTransient(cameraHDRMultisampledDesc) : cameraColor;
					cameraDepth = graph.CreateTransient(cameraDepthDesc);
					EnsureBlitShader();
				}
				const bool clearsTarget = cameraRows.composited || !chainWritten;
				work.flag = clearsTarget;

				u32 prepassKey = ROW_KEY_AFTER_LAST;
				PassRequests cameraRequests = PassRequests::None;
				for (u32 slot = 0; slot < cameraRows.rowCount; ++slot) {
					const RenderPass* row = state.viewPassRows[cameraRows.firstRow + slot].Get();
					if (!row) {
						continue;
					}
					const PassRequests requests = row->GetRequests();
					cameraRequests |= requests;
					const u32 key = RowSortKey(row->point, row->position);
					if (HasAnyFlag(requests, PassRequests::SceneDepth | PassRequests::SceneNormals)) {
						prepassKey = Math::Min(prepassKey, key);
					}
				}
				RowProductions productions;
				{
					const SceneObject* planOwner = cameraRows.camera ? cameraRows.camera->GetOwner().Get() : nullptr;
					productions = BuildRowPlans(state, graph, cameraRows.firstRow, cameraRows.firstRow + cameraRows.rowCount,
												extent, planOwner ? planOwner->GetWorldPosition() : Vec3{});
				}
				ProceduralSlice procedural;
				PassRequests sceneReads = PassRequests::None;
				ProceduralView proceduralView;
				proceduralView.extent = extent;
				LODViewpoint lodFrom;
				if (const SceneObject* cullOwner = cameraRows.camera ? cameraRows.camera->GetOwner().Get() : nullptr) {
					proceduralView.view = cullOwner->GetWorldMatrix().Inverse();
					proceduralView.projection = cameraRows.camera->BuildProjection(aspect);
					proceduralView.cameraPosition = cullOwner->GetWorldPosition();
					lodFrom = LODViewpointOf(state, proceduralView.cameraPosition, proceduralView.projection, LODViewKey(cameraRows.camera),
											 false, 0.0);
					procedural = CullProceduralRenderers(state, false, {}, {}, proceduralView.projection * proceduralView.view,
														 cameraRows.camera->GetCullingMask(), lodFrom);
					sceneReads = procedural.reads | SceneReadsInView(state, false, {}, proceduralView.projection * proceduralView.view,
																	 cameraRows.camera->GetCullingMask());
				}
				{
					RenderState::CameraWork& slices = windowWork.cameras[cameraIndex];
					slices.firstProcedural = procedural.first;
					slices.proceduralCount = procedural.count;
					slices.sceneReads = sceneReads;
				}
				if (HasAnyFlag(sceneReads, PassRequests::SceneDepth | PassRequests::SceneNormals)) {
					cameraRequests |= sceneReads & (PassRequests::SceneDepth | PassRequests::SceneNormals);
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				const bool prepassRead = prepassKey < ROW_KEY_AFTER_LAST;
				RenderState::CameraDepthRecord* keptDepth = DepthToKeep(state, cameraRows.camera);
				if (keptDepth) {
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				ProceduralSlice casters;
				casters.first = static_cast<u32>(state.proceduralVisible.size());
				if (windowCasts) {
					casters = CollectProceduralCasters(state, false, {}, {}, cameraRows.camera ? cameraRows.camera->GetCullingMask() : ~0u, lodFrom);
				}
				windowWork.cameras[cameraIndex].firstCaster = casters.first;
				windowWork.cameras[cameraIndex].casterCount = casters.count;

				u32 depthRangeSlot = RenderState::DEPTH_RANGE_SLOTS;
				if (windowCasts && FitsDepth() && cameraRows.camera) {
					depthRangeSlot = ClaimDepthRangeSlot(state, device, cameraRows.camera->GetInstanceId() & ~PENDING_VIEW_LOD_KEY);
				}
				const bool emitsDepthRange = depthRangeSlot < RenderState::DEPTH_RANGE_SLOTS;

				bool canResolvePost = windowWork.cameras.size() == 1 && !cameraRows.composited &&
									  resolvedColor != GraphicsInternal::INVALID_GRAPH_RESOURCE;
				u32 writingSceneColorRows = 0;
				if (canResolvePost) {
					for (u32 slot = 0; slot < cameraRows.rowCount; ++slot) {
						const RenderPass* row = state.viewPassRows[cameraRows.firstRow + slot].Get();
						if (!row || RowSortKey(row->point, row->position) < sceneAnchorKey) {
							continue;
						}
						if (!row->PrefersResolvedTarget()) {
							canResolvePost = false;
							break;
						}
						if (row->WillWriteSceneTarget() && HasFlag(row->GetRequests(), PassRequests::SceneColor)) {
							++writingSceneColorRows;
						}
					}
				}

				GraphResourceId inlineResolveTarget = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				GraphResourceId currentResolved = resolvedColor;
				GraphResourceId altResolved = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				if (canResolvePost) {
					RenderState::CameraWork& cameraState = windowWork.cameras[cameraIndex];
					cameraState.resolvedPost = true;
					cameraState.discardDepth = !emitsDepthRange;
					if (writingSceneColorRows > 0) {
						RHI::TextureDesc resolvedDesc = cameraHDRDesc;
						resolvedDesc.debugName = "SceneColorResolved";
						const GraphResourceId resolvedScratch = graph.CreateTransient(resolvedDesc);
						if ((writingSceneColorRows & 1u) != 0) {
							currentResolved = resolvedScratch;
							altResolved = resolvedColor;
						} else {
							currentResolved = resolvedColor;
							altResolved = resolvedScratch;
						}
					}
					if (state.msaaSamples > 1) {
						cameraState.inlineResolve = true;
						cameraState.discardColor = true;
						inlineResolveTarget = currentResolved;
						outResolvedInline = true;
					} else {
						cameraTarget = currentResolved;
					}
				}

				const GraphResourceId opaqueColor = DeclareOpaqueColor(graph, cameraHDRDesc, sceneReads);
				const bool wantsPrepass = prepassKey < ROW_KEY_AFTER_LAST;
				const bool wantsNormals = HasFlag(cameraRequests, PassRequests::SceneNormals);
				PrepassImages prepassImages;
				if (wantsPrepass) {
					const SceneObject* keptOwner = keptDepth ? cameraRows.camera->GetOwner().Get() : nullptr;
					prepassImages = DeclarePrepassImages(state, device, graph, keptOwner ? keptDepth : nullptr, extent,
														 keptOwner ? keptOwner->GetWorldMatrix().Inverse() : Mat4::Identity(),
														 keptOwner ? cameraRows.camera->BuildProjection(aspect) : Mat4::Identity(),
														 prepassRead && procedural.opaqueItems);
				}
				const GraphResourceId prepassDepth = prepassImages.read;
				GraphResourceId prepassNormals = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				if (wantsPrepass) {
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
					if (wantsNormals) {
						RHI::TextureDesc prepassNormalsDesc;
						prepassNormalsDesc.size = extent;
						prepassNormalsDesc.format = SCENE_NORMALS_FORMAT;
						prepassNormalsDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
						prepassNormalsDesc.debugName = "ScenePrepassNormals";
						prepassNormals = graph.CreateTransient(prepassNormalsDesc);
					}
				}

				DeclareProceduralViewSteps(state, graph, procedural, proceduralView);

				u32 nextCameraKey = 0;
				const bool nothingToEmit = cameraRows.rowCount == 0 && !wantsPrepass;
				auto flushCameraRowsBefore = [&](u32 anchorKey) {
					if (nothingToEmit) {
						nextCameraKey = anchorKey;
						return;
					}
					for (u32 key = nextCameraKey; key < anchorKey; ++key) {
						if (wantsPrepass && key == prepassKey) {
							DeclarePrepassPasses(state, graph, "Render.Window.Prepass", windowPrepassSite, &PassRecordWindowPrepass,
												 "Render.Window.PrepassItems", windowPrepassItemsSite, &PassRecordWindowPrepassItems,
												 work, prepassImages, prepassNormals, worldSkin, procedural);
						}
						for (u32 slot = 0; slot < cameraRows.rowCount; ++slot) {
							const RenderPass* row = state.viewPassRows[cameraRows.firstRow + slot].Get();
							if (!row || RowSortKey(row->point, row->position) != key) {
								continue;
							}
							work.element = slot;
							if (canResolvePost && key >= sceneAnchorKey) {
								if (!row->WillWriteSceneTarget()) {
									continue;
								}
								const bool readsSceneColor = HasFlag(row->GetRequests(), PassRequests::SceneColor);
								const GraphResourceId rowTarget = readsSceneColor ? altResolved : currentResolved;
								const GraphResourceId rowColorInput = readsSceneColor ? currentResolved : GraphicsInternal::INVALID_GRAPH_RESOURCE;
								DeclareRowPasses(state, graph, "Render.Window.Row", windowRowSite, &PassRecordWindowRow,
												 work, cameraRows.firstRow + slot,
												 {rowTarget, GraphicsInternal::INVALID_GRAPH_RESOURCE, rowColorInput, prepassDepth, prepassNormals,
												  worldSkinBuffer});
								if (readsSceneColor) {
									std::swap(currentResolved, altResolved);
								}
								continue;
							}
							const GraphResourceId sceneColor =
								HasFlag(row->GetRequests(), PassRequests::SceneColor)
									? DeclareSceneColorCopy(graph, "Render.Window.ColorCopy", windowCopySite, work, cameraTarget, cameraHDRDesc,
															state.msaaSamples > 1)
									: GraphicsInternal::INVALID_GRAPH_RESOURCE;
							DeclareRowPasses(state, graph, "Render.Window.Row", windowRowSite, &PassRecordWindowRow,
											 work, cameraRows.firstRow + slot,
											 {cameraTarget, cameraDepth, sceneColor, prepassDepth, prepassNormals, worldSkinBuffer});
						}
					}
					nextCameraKey = anchorKey;
					work.element = 0;
				};
				flushCameraRowsBefore(RowSortKey(PassPoint::Shadows, FrameStagePosition::Normal));

				if (windowCasts) {
					graph.AddPass("Render.Window.Shadow", windowShadowSite, &PassRecordWindowShadow, work);
					graph.Write(windowShadowAtlas, state.MomentsActive() ? RHI::ResourceState::RenderTarget : RHI::ResourceState::DepthWrite);
					graph.Write(windowPunctualAtlas, RHI::ResourceState::DepthWrite);
					graph.Read(worldSkin, RHI::ResourceState::ShaderResource);
					DeclareProceduralReads(state, graph, casters);
					if (state.MomentsActive()) {
						graph.Write(graph.Import(state.momentDepth), RHI::ResourceState::DepthWrite);
					}
				}

				flushCameraRowsBefore(sceneAnchorKey);
				work.produced = productions.ambientOcclusion != GraphicsInternal::INVALID_GRAPH_RESOURCE;
				graph.AddPass("Render.Window.Scene", windowSceneSite, &PassRecordWindowScene, work);
				graph.Write(cameraTarget, RHI::ResourceState::RenderTarget);
				graph.Write(cameraDepth, RHI::ResourceState::DepthWrite);
				graph.Read(worldSkin, RHI::ResourceState::ShaderResource);
				graph.Read(windowShadowAtlas, RHI::ResourceState::ShaderResource);
				graph.Read(windowPunctualAtlas, RHI::ResourceState::ShaderResource);
				DeclareScenePassReads(graph, productions.ambientOcclusion, sceneReads, prepassDepth, prepassNormals, opaqueColor);
				if (inlineResolveTarget != GraphicsInternal::INVALID_GRAPH_RESOURCE) {
					graph.Write(inlineResolveTarget, RHI::ResourceState::RenderTarget);
				}
				DeclareProceduralReads(state, graph, procedural);
				work.produced = false;
				if (emitsDepthRange) {
					SLT_GRAPH_SITE(windowDepthRangeSite, "Render.Window.DepthRange");
					PassWork rangeWork = work;
					rangeWork.element = depthRangeSlot;
					graph.AddPass("Render.Window.DepthRange", windowDepthRangeSite, &PassRecordWindowDepthRange, rangeWork);
					graph.Read(cameraDepth, RHI::ResourceState::ShaderResource);
					graph.Write(graph.CreateVirtual(true), RHI::ResourceState::Undefined);
				}

				flushCameraRowsBefore(ROW_KEY_AFTER_LAST);

				if (cameraRows.composited) {
					if (state.msaaSamples > 1) {
						graph.AddPass("Render.Window.CameraResolve", cameraResolveSite, &PassRecordResolve, work);
						graph.Read(cameraTarget, RHI::ResourceState::TransferSrc);
						graph.Write(cameraColor, RHI::ResourceState::TransferDst);
					}
					work.flag = !chainWritten;
					graph.AddPass("Render.Window.Composite", compositeSite, &PassRecordCameraComposite, work);
					graph.Read(cameraColor, RHI::ResourceState::ShaderResource);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
					if (work.flag) {
						graph.Write(sceneDepth, RHI::ResourceState::DepthWrite);
					}
				}
				chainWritten = true;
			}
		}

		void BuildFrame(RenderState& state, RHI::Device& device, u32 workCount,
						bool anyColdView, bool anyOffscreenWork, ShaderAsset* presentShader) {
			GraphicsInternal::RenderGraph& graph = state.graph;
			graph.Begin(&device);

			state.rowPlanSteps.clear();
			state.rowPlanTargets.clear();
			state.rowPlans.clear();
			state.rowPlans.resize(state.viewPassRows.size());

			SLT_GRAPH_SITE(framePassSite, "Render.BeforeFrame");
			for (RenderState::CameraDepthRecord& record : state.cameraDepths) {
				record.asked = false;
				record.kept = false;
			}
			for (u32 index = 0; index < GraphicsInternal::GetFramePassCount(); ++index) {
				if (FramePass* framePass = GraphicsInternal::GetFramePass(index)) {
					FramePassPlan plan(GraphicsInternal::FramePassPlanInit{&graph, index, framePassSite});
					framePass->Declare(plan);
				}
			}

			DeclareProceduralRenderers(state);
			GatherSceneReaders(state);

			state.skinnedOutputs.assign(state.skinnedMeshRenderers.size(), {});
			state.isolatedSkinnedOutputs.assign(state.isolatedSkinnedMeshRenderers.size(), {});
			GraphResourceId worldSkin = DeclareSkinOutput(device, graph, state.skinDeformer, state.skinnedMeshRenderers);
			GraphResourceId isolatedSkin = DeclareSkinOutput(device, graph, state.isolatedSkinDeformer, state.isolatedSkinnedMeshRenderers);
			const GraphResourceId worldSkinBuffer = state.skinDeformer.GetOutput().IsValid() ? worldSkin : GraphicsInternal::INVALID_GRAPH_RESOURCE;
			const GraphResourceId isolatedSkinBuffer =
				state.isolatedSkinDeformer.GetOutput().IsValid() ? isolatedSkin : GraphicsInternal::INVALID_GRAPH_RESOURCE;
			PassWork skinWork;
			SLT_GRAPH_SITE(worldSkinSite, "Render.Skinning");
			graph.AddPass("Render.Skinning", worldSkinSite, &PassRecordSkinning, skinWork);
			graph.Write(worldSkin, RHI::ResourceState::UnorderedAccess);
			skinWork.flag = true;
			SLT_GRAPH_SITE(isolatedSkinSite, "Render.Skinning.Isolated");
			graph.AddPass("Render.Skinning.Isolated", isolatedSkinSite, &PassRecordSkinning, skinWork);
			graph.Write(isolatedSkin, RHI::ResourceState::UnorderedAccess);

			DeclareSkyCaptures(state, device, graph, workCount);

			if (anyColdView) {
				for (auto& [id, target] : state.viewTargets) {
					if (target.colorSampleable || !target.color.IsValid()) {
						continue;
					}
					GraphResourceId result = graph.Import(target.color, true, RHI::ResourceState::ShaderResource);
					PassWork coldWork;
					coldWork.key = id;
					SLT_GRAPH_SITE(coldSite, "Render.View.Cold");
					graph.AddPass("Render.View.Cold", coldSite, &PassRecordViewCold, coldWork);
					graph.Write(result, RHI::ResourceState::RenderTarget);
				}
			}

			for (u32 viewIndex = 0; viewIndex < static_cast<u32>(state.pendingViews.size()); ++viewIndex) {
				const RenderState::PendingView& pending = state.pendingViews[viewIndex];
				auto targetIt = state.viewTargets.find(pending.targetId);
				if (targetIt == state.viewTargets.end() || !targetIt->second.color.IsValid()) {
					continue;
				}
				RenderState::ViewTarget& target = targetIt->second;
				target.drawn = true;
				target.colorLost = false;
				target.radianceLost = target.radianceLost && !pending.radianceCapture;

				const bool multisampled = state.msaaSamples > 1;
				RHI::TextureDesc hdrDesc;
				RHI::TextureDesc hdrMsDesc;
				RHI::TextureDesc depthDesc;
				const Vec2I viewRenderSize = state.ViewRenderSize(pending, target.size);
				SceneChainDescs(viewRenderSize, state.msaaSamples, hdrDesc, hdrMsDesc, depthDesc);
				GraphResourceId hdrColor{};
				if (pending.radianceCapture) {
					if (!target.radiance.IsValid() || device.GetTextureSize(target.radiance) != viewRenderSize) {
						if (target.radiance.IsValid()) {
							device.DestroyResource(target.radiance);
						}
						RHI::TextureDesc radianceDesc = hdrDesc;
						radianceDesc.usage = radianceDesc.usage | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
						radianceDesc.debugName = "ViewTargetRadiance";
						target.radiance = device.CreateTextureResource(radianceDesc);
					}
					hdrColor = target.radiance.IsValid() ? graph.Import(target.radiance) : graph.CreateTransient(hdrDesc);
				} else {
					hdrColor = graph.CreateTransient(hdrDesc);
				}
				GraphResourceId sceneTarget = multisampled ? graph.CreateTransient(hdrMsDesc) : hdrColor;
				GraphResourceId depth = graph.CreateTransient(depthDesc);
				GraphResourceId result = graph.Import(target.color, true, RHI::ResourceState::ShaderResource);
				GraphResourceId skin = pending.kind == SceneKind::World ? worldSkin : isolatedSkin;

				PassWork viewWork;
				viewWork.key = pending.targetId;
				viewWork.index = viewIndex;

				const u32 firstRow = pending.firstRow;
				const u32 lastRow = pending.firstRow + pending.rowCount;

				u32 prepassKey = ROW_KEY_AFTER_LAST;
				PassRequests viewRequests = PassRequests::None;
				for (u32 index = firstRow; index < lastRow; ++index) {
					const RenderPass* row = state.viewPassRows[index].Get();
					if (!row) {
						continue;
					}
					const PassRequests requests = row->GetRequests();
					viewRequests |= requests;
					const u32 key = RowSortKey(row->point, row->position);
					if (HasAnyFlag(requests, PassRequests::SceneDepth | PassRequests::SceneNormals)) {
						prepassKey = Math::Min(prepassKey, key);
					}
				}
				const PassRequests debugRequests = RequestsForDebugView(pending.debugView);
				if (debugRequests != PassRequests::None) {
					viewRequests |= debugRequests;
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				const bool viewOutlines = pending.outlineCount > 0 && EnsureOutlineShader();
				if (viewOutlines) {
					viewRequests |= PassRequests::SceneDepth;
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}

				const LODViewpoint lodFrom =
					LODViewpointOf(state, pending.cameraPosition, pending.projection, LODViewKey(pending), pending.hasTime, pending.time);
				const ProceduralSlice procedural = CullProceduralRenderers(
					state, pending.kind != SceneKind::World, {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
					{state.viewSkippedTypes.data() + pending.firstSkipped, pending.skippedCount}, pending.projection * pending.view,
					pending.cullingMask, lodFrom);
				const PassRequests sceneReads =
					procedural.reads | (pending.fill == ViewFill::Wireframe
											? PassRequests::None
											: SceneReadsInView(state, pending.kind != SceneKind::World,
															   {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
															   pending.projection * pending.view, pending.cullingMask));
				{
					RenderState::PendingView& slices = state.pendingViews[viewIndex];
					slices.firstProcedural = procedural.first;
					slices.proceduralCount = procedural.count;
					slices.sceneReads = sceneReads;
				}
				if (HasAnyFlag(sceneReads, PassRequests::SceneDepth | PassRequests::SceneNormals)) {
					viewRequests |= sceneReads & (PassRequests::SceneDepth | PassRequests::SceneNormals);
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				const bool prepassRead = prepassKey < ROW_KEY_AFTER_LAST;
				RenderState::CameraDepthRecord* keptDepth = DepthToKeep(state, pending.camera.Get());
				if (keptDepth) {
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				ProceduralSlice casters;
				casters.first = static_cast<u32>(state.proceduralVisible.size());
				const bool viewCasts = PartitionHasShadowCaster(state, pending.kind, {state.viewScenes.data() + pending.firstScene, pending.sceneCount});
				if (viewCasts) {
					casters = CollectProceduralCasters(state, pending.kind != SceneKind::World,
													   {state.viewScenes.data() + pending.firstScene, pending.sceneCount},
													   {state.viewSkippedTypes.data() + pending.firstSkipped, pending.skippedCount},
													   pending.cullingMask, lodFrom);
				}
				state.pendingViews[viewIndex].firstCaster = casters.first;
				state.pendingViews[viewIndex].casterCount = casters.count;

				const RowProductions productions =
					BuildRowPlans(state, graph, firstRow, lastRow, viewRenderSize, pending.cameraPosition);

				const GraphResourceId opaqueColor = DeclareOpaqueColor(graph, hdrDesc, sceneReads);

				const bool wantsPrepass = prepassKey < ROW_KEY_AFTER_LAST;
				const bool wantsNormals = HasFlag(viewRequests, PassRequests::SceneNormals);
				if (wantsPrepass) {
					prepassKey = Math::Min(prepassKey, RowSortKey(PassPoint::DepthPrepass, FrameStagePosition::Normal));
				}
				const PrepassImages prepassImages =
					wantsPrepass ? DeclarePrepassImages(state, device, graph, keptDepth, viewRenderSize, pending.view, pending.projection,
														prepassRead && procedural.opaqueItems)
								 : PrepassImages{};
				const GraphResourceId prepassDepth = prepassImages.read;
				GraphResourceId prepassNormals = GraphicsInternal::INVALID_GRAPH_RESOURCE;
				if (wantsPrepass) {
					if (wantsNormals) {
						RHI::TextureDesc prepassNormalsDesc;
						prepassNormalsDesc.size = viewRenderSize;
						prepassNormalsDesc.format = SCENE_NORMALS_FORMAT;
						prepassNormalsDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
						prepassNormalsDesc.debugName = "ScenePrepassNormals";
						prepassNormals = graph.CreateTransient(prepassNormalsDesc);
					}
				}

				if (pending.fill != ViewFill::Wireframe) {
					DeclareProceduralViewSteps(state, graph, procedural,
											   ProceduralView{pending.view, pending.projection, pending.cameraPosition, viewRenderSize});
				}

				u32 nextKey = 0;
				SLT_GRAPH_SITE(viewRowSite, "Render.View.Row");
				SLT_GRAPH_SITE(viewCopySite, "Render.View.ColorCopy");
				SLT_GRAPH_SITE(viewPrepassSite, "Render.View.Prepass");
				SLT_GRAPH_SITE(viewPrepassItemsSite, "Render.View.PrepassItems");
				const bool nothingToEmit = pending.rowCount == 0 && !wantsPrepass;
				auto flushRowsBefore = [&](u32 anchorKey) {
					if (nothingToEmit) {
						nextKey = anchorKey;
						return;
					}
					for (u32 key = nextKey; key < anchorKey; ++key) {
						if (wantsPrepass && key == prepassKey) {
							DeclarePrepassPasses(state, graph, "Render.View.Prepass", viewPrepassSite, &PassRecordViewPrepass,
												 "Render.View.PrepassItems", viewPrepassItemsSite, &PassRecordViewPrepassItems, viewWork,
												 prepassImages, prepassNormals, skin, procedural);
						}
						for (u32 index = firstRow; index < lastRow; ++index) {
							const RenderPass* row = state.viewPassRows[index].Get();
							if (!row || RowSortKey(row->point, row->position) != key) {
								continue;
							}
							viewWork.subIndex = index - firstRow;
							const GraphResourceId sceneColor =
								HasFlag(row->GetRequests(), PassRequests::SceneColor)
									? DeclareSceneColorCopy(graph, "Render.View.ColorCopy", viewCopySite, viewWork, sceneTarget, hdrDesc, multisampled)
									: GraphicsInternal::INVALID_GRAPH_RESOURCE;
							DeclareRowPasses(state, graph, "Render.View.Row", viewRowSite, &PassRecordViewRow,
											 viewWork, index,
											 {sceneTarget, depth, sceneColor, prepassDepth, prepassNormals,
											  pending.kind == SceneKind::World ? worldSkinBuffer : isolatedSkinBuffer});
						}
					}
					nextKey = anchorKey;
					viewWork.subIndex = 0;
				};
				flushRowsBefore(RowSortKey(PassPoint::Shadows, FrameStagePosition::Normal));

				GraphResourceId shadowAtlas = graph.Import(state.DirectionalShadowTexture());
				GraphResourceId punctualAtlas = graph.Import(state.punctualShadowAtlas);
				{
					auto fitIt = state.viewTargets.find(pending.targetId);
					if (fitIt != state.viewTargets.end()) {
						fitIt->second.cascadeFit.clear();
					}
				}
				if (viewCasts) {
					SLT_GRAPH_SITE(viewShadowSite, "Render.View.Shadow");
					graph.AddPass("Render.View.Shadow", viewShadowSite, &PassRecordViewShadow, viewWork);
					graph.Write(shadowAtlas, state.MomentsActive() ? RHI::ResourceState::RenderTarget : RHI::ResourceState::DepthWrite);
					graph.Write(punctualAtlas, RHI::ResourceState::DepthWrite);
					graph.Read(skin, RHI::ResourceState::ShaderResource);
					DeclareProceduralReads(state, graph, casters);
					if (state.MomentsActive()) {
						graph.Write(graph.Import(state.momentDepth), RHI::ResourceState::DepthWrite);
					}
				}

				flushRowsBefore(RowSortKey(PassPoint::Opaque, FrameStagePosition::Normal));
				SLT_GRAPH_SITE(viewSceneSite, "Render.View.Scene");
				viewWork.produced = productions.ambientOcclusion != GraphicsInternal::INVALID_GRAPH_RESOURCE;
				graph.AddPass("Render.View.Scene", viewSceneSite, &PassRecordViewScene, viewWork);
				graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
				graph.Write(depth, RHI::ResourceState::DepthWrite);
				graph.Read(skin, RHI::ResourceState::ShaderResource);
				graph.Read(shadowAtlas, RHI::ResourceState::ShaderResource);
				graph.Read(punctualAtlas, RHI::ResourceState::ShaderResource);
				DeclareScenePassReads(graph, productions.ambientOcclusion, sceneReads, prepassDepth, prepassNormals, opaqueColor);
				viewWork.produced = false;
				DeclareProceduralReads(state, graph, procedural);
				if (viewCasts && FitsDepth()) {
					const u32 slot = ClaimDepthRangeSlot(state, device, PENDING_VIEW_LOD_KEY | pending.targetId);
					if (slot < RenderState::DEPTH_RANGE_SLOTS) {
						SLT_GRAPH_SITE(viewDepthRangeSite, "Render.View.DepthRange");
						PassWork rangeWork = viewWork;
						rangeWork.element = slot;
						graph.AddPass("Render.View.DepthRange", viewDepthRangeSite, &PassRecordViewDepthRange, rangeWork);
						graph.Read(depth, RHI::ResourceState::ShaderResource);
						graph.Write(graph.CreateVirtual(true), RHI::ResourceState::Undefined);
					}
				}

				if (pending.debugView != DebugView::None && wantsPrepass) {
					flushRowsBefore(RowSortKey(PassPoint::Overlay, FrameStagePosition::Early));
					EnsureDebugViewShader();
					SLT_GRAPH_SITE(viewDebugSite, "Render.View.DebugView");
					viewWork.subIndex = static_cast<u32>(pending.debugView);
					graph.AddPass("Render.View.DebugView", viewDebugSite, &PassRecordDebugView, viewWork);
					graph.Read(HasFlag(debugRequests, PassRequests::SceneNormals) ? prepassNormals : prepassDepth,
							   RHI::ResourceState::ShaderResource);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
					viewWork.subIndex = 0;
				}

				flushRowsBefore(RowSortKey(PassPoint::Overlay, FrameStagePosition::Normal));

				if (viewOutlines && wantsPrepass) {
					RHI::TextureDesc maskDesc;
					maskDesc.size = viewRenderSize;
					maskDesc.format = VALUE_FORMAT;
					maskDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
					maskDesc.debugName = "OutlineMask";
					const GraphResourceId outlineMask = graph.CreateTransient(maskDesc);
					RHI::TextureDesc maskDepthDesc;
					maskDepthDesc.size = viewRenderSize;
					maskDepthDesc.format = SCENE_DEPTH_FORMAT;
					maskDepthDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled;
					maskDepthDesc.debugName = "OutlineDepth";
					const GraphResourceId outlineDepth = graph.CreateTransient(maskDepthDesc);

					SLT_GRAPH_SITE(viewOutlineMaskSite, "Render.View.OutlineMask");
					graph.AddPass("Render.View.OutlineMask", viewOutlineMaskSite, &PassRecordViewOutlineMask, viewWork);
					graph.Write(outlineMask, RHI::ResourceState::RenderTarget);
					graph.Write(outlineDepth, RHI::ResourceState::DepthWrite);
					graph.Read(skin, RHI::ResourceState::ShaderResource);
					DeclareProceduralReads(state, graph, procedural);
					SLT_GRAPH_SITE(viewOutlineSite, "Render.View.Outline");
					graph.AddPass("Render.View.Outline", viewOutlineSite, &PassRecordViewOutline, viewWork);
					graph.Read(outlineMask, RHI::ResourceState::ShaderResource);
					graph.Read(outlineDepth, RHI::ResourceState::ShaderResource);
					graph.Read(prepassDepth, RHI::ResourceState::ShaderResource);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
				}

				SLT_GRAPH_SITE(viewLineSite, "Render.View.Lines");
				if (pending.lineCount > 0) {
					graph.AddPass("Render.View.Lines", viewLineSite, &PassRecordViewLines, viewWork);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
					graph.Write(depth, RHI::ResourceState::DepthWrite);
				}
				SLT_GRAPH_SITE(viewOverlayLineSite, "Render.View.OverlayLines");
				if (pending.overlayLineCount > 0) {
					viewWork.flag = true;
					graph.AddPass("Render.View.OverlayLines", viewOverlayLineSite, &PassRecordViewLines, viewWork);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
					graph.Write(depth, RHI::ResourceState::DepthWrite);
					viewWork.flag = false;
				}

				flushRowsBefore(ROW_KEY_AFTER_LAST);

				if (pending.debugOverlay != DebugOverlay::None && EnsureDebugOverlayShader()) {
					SLT_GRAPH_SITE(viewOverlaySite, "Render.View.DebugOverlay");
					graph.AddPass("Render.View.DebugOverlay", viewOverlaySite, &PassRecordDebugOverlay, viewWork);
					graph.Read(pending.debugOverlay == DebugOverlay::PunctualAtlas ? punctualAtlas : shadowAtlas,
							   RHI::ResourceState::ShaderResource);
					graph.Write(sceneTarget, RHI::ResourceState::RenderTarget);
				}

				if (pending.hasPick) {
					const u32 slot = ClaimPickSlot(state, device, pending);
					state.pendingViews[viewIndex].pickSlot = slot;
					if (slot < RenderState::PICK_SLOTS) {
						RHI::TextureDesc pickDesc;
						pickDesc.size = {1, 1};
						pickDesc.format = VALUE_FORMAT;
						pickDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
						pickDesc.debugName = "PickTarget";
						const GraphResourceId pickTarget = graph.CreateTransient(pickDesc);
						RHI::TextureDesc pickDepthDesc;
						pickDepthDesc.size = {1, 1};
						pickDepthDesc.format = SCENE_DEPTH_FORMAT;
						pickDepthDesc.usage = RHI::TextureUsage::DepthStencil;
						pickDepthDesc.debugName = "PickDepth";
						const GraphResourceId pickDepth = graph.CreateTransient(pickDepthDesc);
						SLT_GRAPH_SITE(viewPickSite, "Render.View.Pick");
						graph.AddPass("Render.View.Pick", viewPickSite, &PassRecordViewPick, viewWork);
						graph.Write(pickTarget, RHI::ResourceState::RenderTarget);
						graph.Write(pickDepth, RHI::ResourceState::DepthWrite);
						graph.Read(skin, RHI::ResourceState::ShaderResource);
						DeclareProceduralReads(state, graph, procedural);
						const GraphResourceId pickSink = graph.CreateVirtual(true);
						SLT_GRAPH_SITE(viewPickCopySite, "Render.View.PickCopy");
						graph.AddPass("Render.View.PickCopy", viewPickCopySite, &PassRecordViewPickCopy, viewWork);
						graph.Read(pickTarget, RHI::ResourceState::TransferSrc);
						graph.Write(pickSink, RHI::ResourceState::Undefined);
					}
				}

				if (multisampled) {
					SLT_GRAPH_SITE(viewResolveSite, "Render.View.Resolve");
					graph.AddPass("Render.View.Resolve", viewResolveSite, &PassRecordResolve, viewWork);
					graph.Read(sceneTarget, RHI::ResourceState::TransferSrc);
					graph.Write(hdrColor, RHI::ResourceState::TransferDst);
				}

				RHI::PipelineHandle presentPipeline = GetOrCreatePipeline(presentShader, RHI::Format::RGBA8Unorm, RHI::Format::Unknown);
				if (presentPipeline.IsValid()) {
					SLT_GRAPH_SITE(viewPresentSite, "Render.View.Present");
					graph.AddPass("Render.View.Present", viewPresentSite, &PassRecordViewPresent, viewWork);
					graph.Read(hdrColor, RHI::ResourceState::ShaderResource);
					graph.Write(result, RHI::ResourceState::RenderTarget);
				}
			}

			for (u32 workIndex = 0; workIndex < workCount; ++workIndex) {
				RenderState::WindowWork& targetWork = state.frameWork[workIndex];
				if (targetWork.target == INVALID_VIEW_TARGET) {
					continue;
				}
				auto targetIt = state.viewTargets.find(targetWork.target);
				if (targetIt == state.viewTargets.end() || !targetIt->second.color.IsValid()) {
					continue;
				}
				const Vec2I renderSize = state.ScaledSize(targetIt->second.size);
				targetIt->second.drawn = targetIt->second.drawn || !targetWork.cameras.empty();
				targetIt->second.colorLost = targetIt->second.colorLost && targetWork.cameras.empty();
				PrepareCameraWork(targetWork, renderSize);
				GraphResourceId result = graph.Import(targetIt->second.color, true, RHI::ResourceState::ShaderResource);
				PassWork work;
				work.key = targetWork.target;
				work.index = workIndex;
				if (targetWork.cameras.empty()) {
					SLT_GRAPH_SITE(targetClearSite, "Render.View.Cold");
					graph.AddPass("Render.View.Cold", targetClearSite, &PassRecordViewCold, work);
					graph.Write(result, RHI::ResourceState::RenderTarget);
					continue;
				}

				RHI::TextureDesc hdrDesc;
				RHI::TextureDesc hdrMsDesc;
				RHI::TextureDesc depthDesc;
				SceneChainDescs(renderSize, state.msaaSamples, hdrDesc, hdrMsDesc, depthDesc);
				GraphResourceId hdrColor = graph.CreateTransient(hdrDesc);
				GraphResourceId sceneTarget = state.msaaSamples > 1 ? graph.CreateTransient(hdrMsDesc) : hdrColor;
				GraphResourceId sceneDepth = graph.CreateTransient(depthDesc);
				bool resolvedInline = false;
				DeclareCameraComposition(state, device, graph, work, sceneTarget, sceneDepth, hdrColor, worldSkin, worldSkinBuffer, resolvedInline);

				if (state.msaaSamples > 1 && !resolvedInline) {
					SLT_GRAPH_SITE(targetResolveSite, "Render.Window.Resolve");
					graph.AddPass("Render.Window.Resolve", targetResolveSite, &PassRecordResolve, work);
					graph.Read(sceneTarget, RHI::ResourceState::TransferSrc);
					graph.Write(hdrColor, RHI::ResourceState::TransferDst);
				}
				SLT_GRAPH_SITE(targetPresentSite, "Render.Window.TargetPresent");
				graph.AddPass("Render.Window.TargetPresent", targetPresentSite, &PassRecordTargetPresent, work);
				graph.Read(hdrColor, RHI::ResourceState::ShaderResource);
				graph.Write(result, RHI::ResourceState::RenderTarget);
				DeclareFrameCaptures(state, graph, {}, targetWork.target, result, work);
			}

			if (anyOffscreenWork) {
				GraphResourceId offscreen = graph.CreateVirtual(true);
				PassWork offscreenWork;
				SLT_GRAPH_SITE(offscreenSite, "Render.Offscreen");
				graph.AddPass("Render.Offscreen", offscreenSite, &PassRecordOffscreen, offscreenWork);
				graph.Write(offscreen, RHI::ResourceState::Undefined);
			}

			for (u32 workIndex = 0; workIndex < workCount; ++workIndex) {
				RenderState::WindowWork& windowWork = state.frameWork[workIndex];
				Window* window = windowWork.window.Get();
				if (!window) {
					continue;
				}
				u64 windowId = windowWork.window.GetInstanceId();

				auto contextIt = state.windowContexts.find(windowId);
				if (contextIt == state.windowContexts.end()) {
					RHI::NativeSwapChainDesc swapDesc;
					swapDesc.window = window->GetNativeHandle();
					swapDesc.sizePx = window->GetClientSize();
					swapDesc.vsync = state.ResolveVSync(windowWork.window);
					RHI::SwapChain* swapChain = device.CreateSwapChainForWindow(swapDesc);
					if (!swapChain) {
						continue;
					}
					RenderState::WindowContext context;
					context.swapChain = swapChain;
					context.window = windowWork.window;
					contextIt = state.windowContexts.emplace(windowId, context).first;
				}
				RenderState::WindowContext& context = contextIt->second;
				context.presentedThisFrame = false;

				RHI::TextureHandle backbuffer = context.swapChain->AcquireNextImage();
				if (!backbuffer.IsValid()) {
					continue;
				}

				const Vec2I swapSize = context.swapChain->GetSize();
				Vec2I frameSize = WindowInternal::GetRenderSizeOverride(*window);
				bool letterboxed = frameSize.x > 0 && frameSize.y > 0 && frameSize != swapSize;
				if (letterboxed && (!context.frameColor.IsValid() || context.frameColorSize != frameSize)) {
					if (context.frameColor.IsValid()) {
						device.DestroyResource(context.frameColor);
					}
					RHI::TextureDesc frameDesc;
					frameDesc.size = frameSize;
					frameDesc.format = context.swapChain->GetFormat();
					frameDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
					frameDesc.debugName = "WindowFrame";
					context.frameColor = device.CreateTextureResource(frameDesc);
					context.frameColorSize = frameSize;
					EnsureBlitShader();
				} else if (!letterboxed && context.frameColor.IsValid()) {
					device.DestroyResource(context.frameColor);
					context.frameColor = {};
					context.frameColorSize = {};
				}
				letterboxed = letterboxed && context.frameColor.IsValid();
				if (!letterboxed) {
					frameSize = swapSize;
				}
				context.frameSize = frameSize;

				Vec2I renderSize = state.ScaledSize(frameSize);
				PrepareCameraWork(windowWork, renderSize);
				if (!windowWork.cameras.empty() && (context.targetSize != renderSize || !context.sceneColor.IsValid())) {
					if (context.sceneColor.IsValid()) {
						device.DestroyResource(context.sceneColor);
					}
					RHI::TextureDesc colorDesc;
					colorDesc.size = renderSize;
					colorDesc.format = SCENE_COLOR_FORMAT;
					colorDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc |
									  RHI::TextureUsage::TransferDst;
					colorDesc.debugName = "SceneColorHDR";
					context.sceneColor = device.CreateTextureResource(colorDesc);
					context.targetSize = renderSize;
				}

				GraphResourceId backbufferId = graph.Import(backbuffer, true, RHI::ResourceState::Present);
				GraphResourceId frameId = letterboxed ? graph.Import(context.frameColor) : backbufferId;
				PassWork work;
				work.key = windowId;
				work.index = workIndex;
				bool backbufferReady = false;

				if (!windowWork.cameras.empty()) {
					RHI::TextureDesc windowHDRDesc;
					RHI::TextureDesc windowHDRMultisampledDesc;
					RHI::TextureDesc windowDepthDesc;
					SceneChainDescs(renderSize, state.msaaSamples, windowHDRDesc, windowHDRMultisampledDesc, windowDepthDesc);
					windowHDRMultisampledDesc.debugName = "SceneColorHDRMS";
					windowDepthDesc.debugName = "SceneDepth";
					GraphResourceId sceneColor = graph.Import(context.sceneColor);
					GraphResourceId sceneTarget = state.msaaSamples > 1 ? graph.CreateTransient(windowHDRMultisampledDesc) : sceneColor;
					GraphResourceId sceneDepth = graph.CreateTransient(windowDepthDesc);

					bool resolvedInline = false;
					DeclareCameraComposition(state, device, graph, work, sceneTarget, sceneDepth, sceneColor, worldSkin, worldSkinBuffer, resolvedInline);

					if (state.msaaSamples > 1 && !resolvedInline) {
						SLT_GRAPH_SITE(windowResolveSite, "Render.Window.Resolve");
						graph.AddPass("Render.Window.Resolve", windowResolveSite, &PassRecordResolve, work);
						graph.Read(sceneTarget, RHI::ResourceState::TransferSrc);
						graph.Write(sceneColor, RHI::ResourceState::TransferDst);
					}

					RHI::PipelineHandle presentPipeline = GetOrCreatePipeline(presentShader, context.swapChain->GetFormat(), RHI::Format::Unknown);
					if (presentPipeline.IsValid()) {
						SLT_GRAPH_SITE(windowPresentSite, "Render.Window.Present");
						graph.AddPass("Render.Window.Present", windowPresentSite, &PassRecordWindowPresent, work);
						graph.Read(sceneColor, RHI::ResourceState::ShaderResource);
						graph.Write(frameId, RHI::ResourceState::RenderTarget);
						backbufferReady = true;
					}
				} else {
					SLT_GRAPH_SITE(windowClearSite, "Render.Window.Clear");
					graph.AddPass("Render.Window.Clear", windowClearSite, &PassRecordWindowClear, work);
					graph.Write(frameId, RHI::ResourceState::RenderTarget);
					backbufferReady = true;
				}

				if (backbufferReady && state.overlay) {
					SLT_GRAPH_SITE(windowOverlaySite, "Render.Window.Overlay");
					graph.AddPass("Render.Window.Overlay", windowOverlaySite, &PassRecordOverlay, work);
					graph.Write(frameId, RHI::ResourceState::RenderTarget);
				}

				if (backbufferReady) {
					DeclareFrameCaptures(state, graph, windowWork.window, INVALID_VIEW_TARGET, frameId, work);
				}

				if (backbufferReady && letterboxed) {
					SLT_GRAPH_SITE(windowLetterboxSite, "Render.Window.Letterbox");
					graph.AddPass("Render.Window.Letterbox", windowLetterboxSite, &PassRecordWindowLetterbox, work);
					graph.Read(frameId, RHI::ResourceState::ShaderResource);
					graph.Write(backbufferId, RHI::ResourceState::RenderTarget);
				}
			}
		}

		void EnsureMomentTargets(RenderState& state) {
			auto release = [&state](RHI::TextureHandle& texture) {
				if (texture.IsValid()) {
					state.device->DestroyResource(texture);
					texture = {};
				}
			};
			if (state.shadowFilter != ShadowFilter::Moments || state.shadowDistance <= 0.0f) {
				release(state.momentAtlas);
				release(state.momentDepth);
				return;
			}
			if (!state.momentResolveShaderTried) {
				state.momentResolveShaderTried = true;
				state.momentResolveShader = LoadEngineShader(EngineBuiltins::MomentResolveShader, "MomentResolve");
			}
			const i32 atlas = static_cast<i32>(state.ShadowAtlasSize());
			const i32 tile = atlas / static_cast<i32>(GraphicsInternal::CascadeTilesPerAxis(state.shadowCascades));
			if (state.momentAtlas.IsValid() && state.device->GetTextureSize(state.momentAtlas) != Vec2I(atlas, atlas)) {
				release(state.momentAtlas);
			}
			if (state.momentDepth.IsValid() && state.device->GetTextureSize(state.momentDepth) != Vec2I(tile, tile)) {
				release(state.momentDepth);
			}
			if (!state.momentAtlas.IsValid()) {
				RHI::TextureDesc desc;
				desc.size = {atlas, atlas};
				desc.format = RHI::Format::RGBA32Float;
				desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
				desc.debugName = "ShadowMomentAtlas";
				state.momentAtlas = state.device->CreateTextureResource(desc);
			}
			if (!state.momentDepth.IsValid()) {
				const u32 offered =
					state.device->GetSupportedSampleCounts(SHADOW_ATLAS_FORMAT, RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled);
				state.momentSamples = (offered & 4u) != 0 ? 4u : ((offered & 2u) != 0 ? 2u : 1u);
				if (state.momentSamples > 1) {
					RHI::TextureDesc desc;
					desc.size = {tile, tile};
					desc.format = SHADOW_ATLAS_FORMAT;
					desc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled;
					desc.samples = state.momentSamples;
					desc.debugName = "ShadowMomentDepth";
					state.momentDepth = state.device->CreateTextureResource(desc);
				}
			}
		}

		void EnsureShadowAtlases(RenderState& state) {
			if (!state.device) {
				return;
			}
			const i32 size = static_cast<i32>(state.ShadowAtlasSize());
			if (state.shadowAtlas.IsValid() && state.shadowAtlasCreatedAt == size) {
				EnsureMomentTargets(state);
				return;
			}
			if (state.shadowAtlas.IsValid()) {
				state.device->DestroyResource(state.shadowAtlas);
			}
			if (state.punctualShadowAtlas.IsValid()) {
				state.device->DestroyResource(state.punctualShadowAtlas);
			}

			RHI::TextureDesc atlasDesc;
			atlasDesc.size = {size, size};
			atlasDesc.format = SHADOW_ATLAS_FORMAT;
			atlasDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled;
			atlasDesc.debugName = "ShadowAtlas";
			state.shadowAtlas = state.device->CreateTextureResource(atlasDesc);
			atlasDesc.debugName = "PunctualShadowAtlas";
			state.punctualShadowAtlas = state.device->CreateTextureResource(atlasDesc);
			state.shadowAtlasCreatedAt = size;
			EnsureMomentTargets(state);
		}

		u32 ResolveSampleCount(RenderState& state, i32 requested) {
			const u32 wanted = requested > 1 ? static_cast<u32>(requested) : 1u;
			if (wanted <= 1 || !state.device) {
				return 1;
			}
			const u32 offered = state.device->GetSupportedSampleCounts(
									SCENE_COLOR_FORMAT, RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc) &
								state.device->GetSupportedSampleCounts(SCENE_DEPTH_FORMAT, RHI::TextureUsage::DepthStencil);
			u32 resolved = 1;
			for (u32 count = 1; count <= wanted; count <<= 1) {
				if (offered & count) {
					resolved = count;
				}
			}
			if (resolved != wanted) {
				SLT_LOG_WARN("Graphics", "the active profile asks for {}x MSAA ('{}' doesn't support it, rendering at {}x)",
							 wanted, state.device->GetSelectedAdapter().name, resolved);
			}
			return resolved;
		}

		u32 ClampAtlasSize(i32 requested) {
			i32 size = Math::Clamp(requested, MIN_SHADOW_ATLAS_SIZE, MAX_SHADOW_ATLAS_SIZE);
			i32 rounded = MIN_SHADOW_ATLAS_SIZE;
			while (rounded < size) {
				rounded <<= 1;
			}
			return static_cast<u32>(rounded);
		}

		i32 ClampProbeResolution(i32 requested) {
			i32 size = Math::Clamp(requested, 32, 512);
			i32 rounded = 32;
			while (rounded < size) {
				rounded <<= 1;
			}
			return rounded;
		}

		void ApplyProfileToState(RenderState& state, const GraphicsProfile& profile) {
			state.lightsPerView = static_cast<u32>(Math::Clamp(profile.lightsPerView, 1, 4096));
			state.lightsPerCluster = static_cast<u32>(Math::Clamp(profile.lightsPerCluster, 1, static_cast<i32>(GraphicsInternal::MAX_CLUSTER_ITEMS)));
			state.decalsPerCluster = static_cast<u32>(Math::Clamp(profile.decalsPerCluster, 1, static_cast<i32>(GraphicsInternal::MAX_CLUSTER_ITEMS)));

			state.shadowDistance = Math::Max(profile.shadowDistance, 0.0f);
			state.shadowCascades = static_cast<u32>(
				Math::Clamp(profile.shadowCascades, 1, static_cast<i32>(MAX_SHADOW_CASCADES)));
			state.shadowFirstCascadeDistance = profile.shadowFirstCascadeDistance;
			state.shadowCascadeFit = profile.shadowCascadeFit;
			state.shadowPcfTaps = static_cast<u32>(Math::Clamp(profile.shadowPcfTaps, 1, 7)) | 1u;
			state.shadowFilter = profile.shadowFilter == ShadowFilter::Moments ? ShadowFilter::Moments : ShadowFilter::PCF;
			state.shadowMomentBlur = static_cast<u32>(Math::Clamp(profile.shadowMomentBlur, 0, 2));
			state.shadowMomentParams = Vec3(Math::Clamp(profile.shadowMomentPositiveExponent, 1.0f, 40.0f),
											Math::Clamp(profile.shadowMomentNegativeExponent, 1.0f, 40.0f),
											Math::Clamp(profile.shadowMomentBleedReduction, 0.0f, 0.99f));
			state.shadowAtlasSize = ClampAtlasSize(profile.shadowAtlasSize);
			state.transparentReceiveShadows = profile.transparentReceiveShadows;
			state.reflectionProbeResolution = ClampProbeResolution(profile.reflectionProbeResolution);
			state.renderScale = Math::Clamp(profile.renderScale, 0.25f, 2.0f);
			state.msaaSamples = ResolveSampleCount(state, profile.msaa);
			state.tonemap = profile.tonemap > TonemapCurve::AgX ? TonemapCurve::None : profile.tonemap;
			state.anisotropicFiltering = profile.anisotropicFiltering > TextureAnisotropy::ForcedOn ? TextureAnisotropy::PerTexture
																									: profile.anisotropicFiltering;
			state.maxAnisotropy = Math::Clamp(profile.maxAnisotropy, 1, 16);
			state.lodBias = Math::Clamp(profile.lodBias, 0.01f, 100.0f);
			state.lodFinestLevel = static_cast<u32>(Math::Clamp(profile.maximumLODLevel, 0, static_cast<i32>(LODChoice::MAX_LEVELS) - 1));
			state.lodCrossFade = profile.lodCrossFade;
			state.objectShadowBudget = static_cast<u32>(Math::Clamp(profile.objectShadows, 0, static_cast<i32>(GraphicsInternal::MAX_OBJECT_SHADOWS)));
			state.objectShadowResolution = static_cast<u32>(Math::Clamp(profile.objectShadowResolution, 64, 4096));
		}

		void CopyProfileFields(const GraphicsProfile& from, GraphicsProfile& to) {
			const TypeInfo* type = GraphicsProfile::GetReflectType();
			if (!type) {
				return;
			}
			for (u32 index = 0; index < type->fieldCount; ++index) {
				const FieldInfo& field = type->fields[index];
				if (!field.addrFn || (field.flags & FieldFlagTriviallyCopyable) == 0) {
					continue;
				}
				std::memcpy(field.addrFn(&to), field.addrFn(const_cast<GraphicsProfile*>(&from)), field.size);
			}
		}

		bool DiffProfiles(const GraphicsProfile& from, const GraphicsProfile& to, MutabilityClass& outHighestCost) {
			const TypeInfo* type = GraphicsProfile::GetReflectType();
			if (!type) {
				return false;
			}
			bool changed = false;
			outHighestCost = MutabilityClass::Hot;
			for (u32 index = 0; index < type->fieldCount; ++index) {
				const FieldInfo& field = type->fields[index];
				if (!field.addrFn || (field.flags & FieldFlagTriviallyCopyable) == 0) {
					continue;
				}
				const void* left = field.addrFn(const_cast<GraphicsProfile*>(&from));
				const void* right = field.addrFn(const_cast<GraphicsProfile*>(&to));
				if (std::memcmp(left, right, field.size) == 0) {
					continue;
				}
				changed = true;
				MutabilityClass mutability = MutabilityClass::Hot;
				if (Settings::GetFieldMutability(&field, mutability) && mutability > outHighestCost) {
					outHighestCost = mutability;
				}
			}
			return changed;
		}

		GraphicsProfile* ResolveSelectedProfile(RenderState& state) {
			const GraphicsSettings* settings = state.graphicsSettings;
			if (!settings || settings->qualityLevel < 0 || settings->qualityProfiles.empty()) {
				return nullptr;
			}

			std::string_view packed = settings->qualityProfiles;
			i32 remaining = settings->qualityLevel;
			std::string_view entry;
			for (size_t start = 0; start <= packed.size();) {
				const size_t separator = packed.find(';', start);
				const size_t end = separator == std::string_view::npos ? packed.size() : separator;
				if (remaining == 0) {
					entry = packed.substr(start, end - start);
					break;
				}
				--remaining;
				if (separator == std::string_view::npos) {
					break;
				}
				start = separator + 1;
			}
			if (entry.empty()) {
				return nullptr;
			}

			const Guid id = Guid::FromString(entry);
			if (!id.IsValid()) {
				if (state.warnedProfileId != Guid{1, 1}) {
					SLT_LOG_WARN("Graphics", "[Graphics] qualityProfiles entry {} isn't an asset guid (using the built-in profile)",
								 settings->qualityLevel);
					state.warnedProfileId = Guid{1, 1};
				}
				return nullptr;
			}

			if (id == state.activeProfileId && state.activeProfile.IsValid()) {
				return state.activeProfile.Get();
			}

			Asset* loaded = AssetDatabase::LoadAssetByGuid(id);
			GraphicsProfile* profile = nullptr;
			if (loaded && loaded->GetAssetTypeId() == StringHash("GraphicsProfile")) {
				profile = static_cast<GraphicsProfile*>(loaded);
			} else if (loaded) {
				SLT_LOG_WARN("Graphics", "[Graphics] quality level {} uses '{}' (not a GraphicsProfile)",
							 settings->qualityLevel, id.ToString());
				AssetDatabase::UnloadAsset(loaded);
			}
			if (!profile) {
				if (state.warnedProfileId != id) {
					SLT_LOG_WARN("Graphics", "[Graphics] quality level {} uses '{}' (the asset database can't load it yet)",
								 settings->qualityLevel, id.ToString());
					state.warnedProfileId = id;
				}
				return nullptr;
			}

			if (state.activeProfile.IsValid() && state.activeProfileId != id) {
				AssetDatabase::UnloadAsset(state.activeProfile.Get());
			}
			state.activeProfile = Handle<GraphicsProfile>(profile);
			state.activeProfileId = id;
			state.warnedProfileId = {};
			return profile;
		}

		i32 ResolveVSyncOverride(const RenderState& state) {
			if (!state.graphicsSettings || !ScreenInternal::HasGameWindow()) {
				return -1;
			}
			switch (state.graphicsSettings->vsync) {
			case VSyncMode::On:
				return 1;
			case VSyncMode::Off:
				return 0;
			case VSyncMode::WindowHint:
				break;
			}
			return -1;
		}

		void ReconcileGraphicsConfig(RenderState& state) {
			state.vsyncOverride = ResolveVSyncOverride(state);
			for (auto& [id, context] : state.windowContexts) {
				if (context.swapChain) {
					const bool vsync = state.ResolveVSync(context.window);
					if (vsync != context.swapChain->GetVSync()) {
						context.swapChain->Recreate(context.swapChain->GetSize(), vsync);
					}
				}
			}

			if (state.profileSelectionDirty) {
				GraphicsProfile* selected = ResolveSelectedProfile(state);
				if (selected) {
					state.profileSelectionDirty = false;
				} else if (state.activeProfile.IsValid()) {
					AssetDatabase::UnloadAsset(state.activeProfile.Get());
					state.activeProfile = {};
					state.activeProfileId = {};
					state.profileSelectionDirty = false;
				}
			}

			const GraphicsProfile* live = state.activeProfile.IsValid() ? state.activeProfile.Get() : state.builtIn;
			if (!live || !state.applied) {
				return;
			}
			MutabilityClass cost = MutabilityClass::Hot;
			if (!DiffProfiles(*state.applied, *live, cost)) {
				return;
			}

			if (cost >= MutabilityClass::DeviceIdle) {
				state.device->WaitIdle();
			}

			CopyProfileFields(*live, *state.applied);
			ApplyProfileToState(state, *state.applied);
			EnsureShadowAtlases(state);

			SLT_LOG_INFO("Graphics", "quality level {}: {}x MSAA, render scale {:.2f}, {}-texel shadow atlas",
						 state.graphicsSettings ? state.graphicsSettings->qualityLevel : -1,
						 state.msaaSamples, state.renderScale, state.ShadowAtlasSize());
		}

	} // anonymous namespace

	PassContext::PassContext(const GraphicsInternal::PassContextInit& init)
		: _exec(init.exec), _color(init.color), _depth(init.depth),
		  _colorFormat(init.colorFormat), _depthFormat(init.depthFormat),
		  _samples(init.samples), _extent(init.extent),
		  _view(init.view), _projection(init.projection), _cameraPosition(init.cameraPosition),
		  _scopeIndex(init.scopeIndex), _requests(init.requests), _sceneColor(init.sceneColor),
		  _sceneDepth(init.sceneDepth), _sceneNormals(init.sceneNormals),
		  _step(init.step), _stepCount(init.stepCount),
		  _passInput0(init.passInput0), _passInput1(init.passInput1), _passVolume(init.passVolume),
		  _writesSceneTarget(init.writesSceneTarget) {}

	bool PassContext::DrawProcedural(const Handle<ShaderAsset>& shader, std::span<const u8> pushConstants, u32 vertexCount, u32 instanceCount) {
		ShaderAsset* asset = shader.Get();
		if (!_exec || !_exec->cmd || !asset || vertexCount == 0 || instanceCount == 0) {
			return false;
		}
		if (pushConstants.size() > asset->pushConstantSize) {
			SLT_LOG_WARN("Graphics", "pass draw with '{}' pushed {} B -> a declared {} B. Dropped",
						 asset->virtualPath, pushConstants.size(), asset->pushConstantSize);
			return false;
		}
		const bool usesDepth = _depth.IsValid() && asset->renderState.depth != RHI::DepthMode::Off;
		const RHI::Format depthFormat = usesDepth ? _depthFormat : RHI::Format::Unknown;
		RHI::PipelineHandle pipeline = GetOrCreatePipeline(asset, _colorFormat, depthFormat, nullptr, _samples);
		if (!pipeline.IsValid()) {
			return false;
		}

		const bool discardPriorColor = !_writesSceneTarget && !_colorWritten &&
									   asset->renderState.blend == RHI::BlendMode::Opaque && !usesDepth;
		RHI::RenderPassDesc::Attachment colorAttachment;
		colorAttachment.texture = _color;
		colorAttachment.load = discardPriorColor ? RHI::LoadOp::DontCare : RHI::LoadOp::Load;
		RHI::RenderPassDesc::DepthAttachment depthAttachment;
		depthAttachment.texture = _depth;
		depthAttachment.load = RHI::LoadOp::Load;

		RHI::RenderPassDesc desc;
		desc.colorAttachments = {&colorAttachment, 1};
		desc.depth = usesDepth ? &depthAttachment : nullptr;
		RHI::CommandList& cmd = *_exec->cmd;
		cmd.BeginRenderPass(desc);
		cmd.SetPipeline(pipeline);
		BindGlobalsOnlyViewSet(cmd, *_exec->device, pipeline, *asset);
		BindSceneTextureSet(cmd, *_exec->device, pipeline, *asset, SceneRequestTextures{_sceneColor, _sceneDepth, _sceneNormals, _passInput0, _passInput1, _passVolume},
							PassView{_view, _projection, _extent});
		if (!pushConstants.empty()) {
			cmd.PushConstants(pushConstants);
		}
		cmd.Draw(vertexCount, instanceCount);
		cmd.EndRenderPass();
		_colorWritten = true;
		++_exec->drawCalls;
		return true;
	}

	bool PassContext::DrawFullScreen(const Handle<ShaderAsset>& shader, std::span<const u8> pushConstants) {
		ShaderAsset* asset = shader.Get();
		if (!_exec || !_exec->cmd || !asset) {
			return false;
		}
		if (pushConstants.size() > asset->pushConstantSize) {
			SLT_LOG_WARN("Graphics", "pass draw with '{}' pushed {} B -> a declared {} B. Dropped",
						 asset->virtualPath, pushConstants.size(), asset->pushConstantSize);
			return false;
		}
		const bool usesDepth = _depth.IsValid() && asset->renderState.depth != RHI::DepthMode::Off;
		const RHI::Format depthFormat = usesDepth ? _depthFormat : RHI::Format::Unknown;
		RHI::PipelineHandle pipeline = GetOrCreatePipeline(asset, _colorFormat, depthFormat, nullptr, _samples);
		if (!pipeline.IsValid()) {
			return false;
		}

		const bool overwritesTarget = asset->renderState.blend == RHI::BlendMode::Opaque && !usesDepth;
		RHI::RenderPassDesc::Attachment colorAttachment;
		colorAttachment.texture = _color;
		colorAttachment.load = overwritesTarget ? RHI::LoadOp::DontCare : RHI::LoadOp::Load;
		RHI::RenderPassDesc::DepthAttachment depthAttachment;
		depthAttachment.texture = _depth;
		depthAttachment.load = RHI::LoadOp::Load;

		RHI::RenderPassDesc desc;
		desc.colorAttachments = {&colorAttachment, 1};
		desc.depth = usesDepth ? &depthAttachment : nullptr;
		RHI::CommandList& cmd = *_exec->cmd;
		cmd.BeginRenderPass(desc);
		cmd.SetPipeline(pipeline);
		BindGlobalsOnlyViewSet(cmd, *_exec->device, pipeline, *asset);
		BindSceneTextureSet(cmd, *_exec->device, pipeline, *asset, SceneRequestTextures{_sceneColor, _sceneDepth, _sceneNormals, _passInput0, _passInput1, _passVolume},
							PassView{_view, _projection, _extent});
		if (!pushConstants.empty()) {
			cmd.PushConstants(pushConstants);
		}
		cmd.Draw(3, 1);
		cmd.EndRenderPass();
		_colorWritten = true;
		++_exec->drawCalls;
		return true;
	}

	bool PassContext::DrawFullScreen(const Handle<MaterialAsset>& material) {
		return DrawProcedural(material, {}, 3, 1);
	}

	bool PassContext::DrawProcedural(const Handle<MaterialAsset>& material, std::span<const u8> pushConstants, u32 vertexCount, u32 instanceCount) {
		RenderState* state = _state;
		MaterialAsset* asset = material.Get();
		ShaderAsset* shader = asset ? asset->shader.Get() : nullptr;
		if (!_exec || !_exec->cmd || !state || !shader || vertexCount == 0 || instanceCount == 0) {
			return false;
		}
		if (pushConstants.size() > shader->pushConstantSize) {
			SLT_LOG_WARN("Graphics", "pass draw with material '{}' pushed {} B -> a declared {} B. Dropped",
						 asset->virtualPath, pushConstants.size(), shader->pushConstantSize);
			return false;
		}
		if (SceneRequests::DeclaresViewSet(*shader) && !DeclaresOnlyGlobalViewResources(*shader)) {
			SLT_LOG_WARN("Graphics", "pass material '{}' uses shader '{}' (declares per view resources, a pass has no view set). "
									 "Use a shader with no `shalltear_template`",
						 asset->virtualPath, shader->virtualPath);
			return false;
		}
		const RHI::ShaderRenderState renderState = asset->ResolveRenderState(shader->renderState);
		const bool usesDepth = _depth.IsValid() && renderState.depth != RHI::DepthMode::Off;
		const RHI::Format depthFormat = usesDepth ? _depthFormat : RHI::Format::Unknown;
		const u32 variant = shader->FindVariant(asset->keywordHash);
		RHI::PipelineHandle pipeline = GetOrCreatePipeline(shader, _colorFormat, depthFormat, &renderState, _samples, variant);
		if (!pipeline.IsValid()) {
			return false;
		}

		DrawEntry entry;
		PackMaterialBlock(entry, *state->extractionArena, shader, asset);
		ResolveMaterialTextures(entry, *state->extractionArena, shader, asset);

		const bool discardPriorColor = !_writesSceneTarget && !_colorWritten &&
									   renderState.blend == RHI::BlendMode::Opaque && !usesDepth;
		RHI::RenderPassDesc::Attachment colorAttachment;
		colorAttachment.texture = _color;
		colorAttachment.load = discardPriorColor ? RHI::LoadOp::DontCare : RHI::LoadOp::Load;
		RHI::RenderPassDesc::DepthAttachment depthAttachment;
		depthAttachment.texture = _depth;
		depthAttachment.load = RHI::LoadOp::Load;

		RHI::RenderPassDesc desc;
		desc.colorAttachments = {&colorAttachment, 1};
		desc.depth = usesDepth ? &depthAttachment : nullptr;
		RHI::CommandList& cmd = *_exec->cmd;
		RHI::Device& device = *_exec->device;
		cmd.BeginRenderPass(desc);
		cmd.SetPipeline(pipeline);
		BindGlobalsOnlyViewSet(cmd, device, pipeline, *shader);
		BindMaterialSet(cmd, device, pipeline, entry);
		BindSceneTextureSet(cmd, device, pipeline, *shader, SceneRequestTextures{_sceneColor, _sceneDepth, _sceneNormals, _passInput0, _passInput1, _passVolume},
							PassView{_view, _projection, _extent});
		if (!pushConstants.empty()) {
			cmd.PushConstants(pushConstants);
		}
		cmd.Draw(vertexCount, instanceCount);
		cmd.EndRenderPass();
		_colorWritten = true;
		++_exec->drawCalls;
		return true;
	}

	bool PassContext::DrawObjects(u32 renderingLayerMask, const Handle<MaterialAsset>& overrideMaterial) {
		return DrawObjects(renderingLayerMask, overrideMaterial, _projection);
	}

	bool PassContext::DrawObjects(u32 renderingLayerMask, const Handle<MaterialAsset>& overrideMaterial, const Mat4& projection) {
		RenderState* state = _state;
		if (!_exec || !_exec->cmd || !state || renderingLayerMask == 0) {
			return false;
		}
		if (_scopeIndex >= state->passScopes.size()) {
			return false;
		}
		const RenderState::PassScope& scope = state->passScopes[_scopeIndex];

		PassTargets targets;
		targets.color = _color;
		targets.depth = _depth;
		targets.colorFormat = _colorFormat;
		targets.depthFormat = _depthFormat;
		targets.clear = false;
		targets.samples = _samples;
		targets.extent = _extent;

		u32 triangles = 0;
		const u32 drawCalls = RecordScenePass(*_exec->cmd, *state->extractionArena, _view, projection, _cameraPosition,
											  Color::Black(), targets, triangles,
											  {state->viewScenes.data() + scope.firstScene, scope.sceneCount},
											  ViewFill::Solid, Color32::White(), scope.kind, 0,
											  renderingLayerMask, overrideMaterial.Get(), {}, nullptr, {},
											  ScenePassReads{_sceneColor, _sceneDepth, _sceneNormals, false});
		_exec->drawCalls += drawCalls;
		_exec->triangles += triangles;
		_colorWritten = _colorWritten || drawCalls > 0;
		return drawCalls > 0;
	}

	ProceduralPlan::ProceduralPlan(const GraphicsInternal::ProceduralPlanInit& init)
		: _frame(init.frame), _renderer(init.renderer) {}

	void ProceduralPlan::SetBounds(const AABB& worldBounds) {
		GraphicsInternal::ProceduralFrame::Renderer& renderer = _frame->renderers[_renderer];
		renderer.bounds = worldBounds;
		renderer.bounded = worldBounds.IsValid();
	}

	u32 ProceduralPlan::AddItem(const Handle<MaterialAsset>& material, const Vec3& sortPoint, ShadowCasting castShadows) {
		GraphicsInternal::ProceduralFrame& frame = *_frame;
		GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[_renderer];
		GraphicsInternal::ProceduralFrame::Item& item = frame.items.emplace_back();
		item.material = material;
		item.sortPoint = sortPoint;
		item.firstRead = static_cast<u32>(frame.reads.size());
		item.index = renderer.itemCount;
		item.renderer = _renderer;
		item.castShadows = castShadows;
		_inStep = false;
		return renderer.itemCount++;
	}

	u32 ProceduralPlan::AddViewStep() {
		GraphicsInternal::ProceduralFrame& frame = *_frame;
		GraphicsInternal::ProceduralFrame::Renderer& renderer = frame.renderers[_renderer];
		GraphicsInternal::ProceduralFrame::Step& step = frame.steps.emplace_back();
		step.firstAccess = static_cast<u32>(frame.stepAccesses.size());
		step.index = renderer.stepCount;
		_inStep = true;
		return renderer.stepCount++;
	}

	namespace {

		bool StepAccessible(RHI::BufferHandle buffer, RHI::ResourceState state, bool write) {
			if (!buffer.IsValid()) {
				return false;
			}
			if (write) {
				return state == RHI::ResourceState::UnorderedAccess || state == RHI::ResourceState::TransferDst;
			}
			return state == RHI::ResourceState::UnorderedAccess || state == RHI::ResourceState::ShaderResource ||
				   state == RHI::ResourceState::IndirectArgument || state == RHI::ResourceState::TransferSrc;
		}

		void DeclareStepAccess(GraphicsInternal::ProceduralFrame& frame, RHI::BufferHandle buffer, RHI::ResourceState state, bool write) {
			if (!StepAccessible(buffer, state, write)) {
				SLT_LOG_WARN("Graphics", "a procedural view step declared a {} of {} buffer in a state a step can't {} it in. Dropped",
							 write ? "write" : "read", buffer.IsValid() ? "a" : "an invalid", write ? "write" : "read");
				return;
			}
			frame.stepAccesses.push_back({buffer, state, write});
			GraphicsInternal::ProceduralFrame::Step& step = frame.steps.back();
			++step.accessCount;
			step.writes = step.writes || write;
		}

	} // anonymous namespace

	void ProceduralPlan::Read(RHI::BufferHandle buffer, RHI::ResourceState state) {
		GraphicsInternal::ProceduralFrame& frame = *_frame;
		if (_inStep) {
			DeclareStepAccess(frame, buffer, state, false);
			return;
		}
		if (frame.renderers[_renderer].itemCount == 0) {
			SLT_LOG_WARN("Graphics", "a procedural renderer declared a read before its 1st item or step. Dropped");
			return;
		}
		if (!buffer.IsValid() || (state != RHI::ResourceState::ShaderResource && state != RHI::ResourceState::IndirectArgument)) {
			SLT_LOG_WARN("Graphics", "a procedural item declared a read of {} buffer in a state a draw can't read it in "
									 "(should be ShaderResource or IndirectArgument). Dropped",
						 buffer.IsValid() ? "a" : "an invalid");
			return;
		}
		frame.reads.push_back({buffer, state});
		++frame.items.back().readCount;
	}

	void ProceduralPlan::Write(RHI::BufferHandle buffer, RHI::ResourceState state) {
		if (!_inStep) {
			SLT_LOG_WARN("Graphics", "a procedural renderer declared a write outside a view step (draws write nothing). Dropped");
			return;
		}
		DeclareStepAccess(*_frame, buffer, state, true);
	}

	ProceduralContext::ProceduralContext(const GraphicsInternal::ProceduralContextInit& init)
		: _cmd(init.cmd), _device(init.device), _frame(init.frame), _flatItem(init.item),
		  _item(init.frame ? init.frame->items[init.item].index : 0), _pass(init.pass), _value(init.value),
		  _colorFormat(init.colorFormat), _depthFormat(init.depthFormat), _samples(init.samples),
		  _sceneColor(init.sceneColor), _sceneDepth(init.sceneDepth), _sceneNormals(init.sceneNormals), _viewBlock(init.viewBlock),
		  _shadowAtlas(init.shadowAtlas),
		  _punctualAtlas(init.punctualAtlas), _environmentCube(init.environmentCube), _environmentCube2(init.environmentCube2), _ambientOcclusion(init.ambientOcclusion),
		  _drawCalls(init.drawCalls), _view(init.view), _projection(init.projection), _cameraPosition(init.cameraPosition),
		  _extent(init.extent), _casterBias(init.casterBias) {}

	bool ProceduralContext::BindItem(std::span<const u8> pushConstants, bool surface) const {
		RenderState* state = _state;
		if (!state || !_cmd || !_frame) {
			return false;
		}
		const GraphicsInternal::ProceduralFrame::Item& item = _frame->items[_flatItem];
		MaterialAsset* material = item.material.Get();
		ShaderAsset* shader = material ? material->shader.Get() : nullptr;
		if (!shader) {
			return false;
		}
		if (surface ? !IsMeshSurface(*shader, *material) : !IsProceduralShader(*shader)) {
			NameProceduralCallRefusal(*shader, *material, !surface);
			return false;
		}
		if (pushConstants.size() > shader->pushConstantSize) {
			SLT_LOG_WARN("Graphics", "a procedural item with material '{}' pushed {} B -> a declared {} B. Dropped",
						 material->virtualPath, pushConstants.size(), shader->pushConstantSize);
			return false;
		}
		RHI::ShaderRenderState renderState = material->ResolveRenderState(shader->renderState);
		ShaderProgramKind program = ShaderProgramKind::Forward;
		if (_pass != ProceduralPass::Scene) {
			program = ShaderProgramKind::Depth;
			renderState.blend = RHI::BlendMode::Opaque;
			renderState.depth = RHI::DepthMode::TestWrite;
			renderState.depthCompare = RHI::DepthCompare::NearerOrEqual;
			renderState.fill = RHI::FillMode::Solid;
			if (_pass == ProceduralPass::Shadow && item.castShadows == ShadowCasting::TwoSided) {
				renderState.cull = RHI::CullMode::Off;
			}
			if (_pass == ProceduralPass::Shadow) {
				ApplyCasterBias(renderState, _casterBias);
			}
		}
		const u32 variant = shader->FindVariant(material->keywordHash);
		RHI::PipelineHandle pipeline = GetOrCreatePipeline(shader, _colorFormat, _depthFormat, &renderState, _samples, variant, program);
		if (!pipeline.IsValid()) {
			return false;
		}
		DrawEntry entry;
		PackMaterialBlock(entry, *state->extractionArena, shader, material);
		ResolveMaterialTextures(entry, *state->extractionArena, shader, material);
		_cmd->SetPipeline(pipeline);
		if (renderState.dynamicDepthBias) {
			_cmd->SetDepthBias(renderState.depthBiasConstant, renderState.depthBiasSlope);
		}
		if (SceneRequests::DeclaresViewSet(*shader) && _viewBlock.IsValid()) {
			_cmd->BindTransientSet(0, CreateViewSet(*_device, pipeline, *shader, _viewBlock, _shadowAtlas, _punctualAtlas,
													_environmentCube, _ambientOcclusion, _environmentCube2));
		}
		BindMaterialSet(*_cmd, *_device, pipeline, entry);
		BindSceneTextureSet(*_cmd, *_device, pipeline, *shader, SceneRequestTextures{_sceneColor, _sceneDepth, _sceneNormals, {}, {}, {}},
							PassView{_view, _projection, _extent});
		if (!pushConstants.empty()) {
			_cmd->PushConstants(pushConstants);
		}
		return true;
	}

	bool ProceduralContext::Draw(std::span<const u8> pushConstants, u32 vertexCount, u32 instanceCount) {
		if (vertexCount == 0 || instanceCount == 0 || !BindItem(pushConstants, false)) {
			return false;
		}
		_cmd->Draw(vertexCount, instanceCount);
		++*_drawCalls;
		return true;
	}

	bool ProceduralContext::DrawTriangles(std::span<const MeshVertex> vertices, bool receiveShadows) {
		RenderState* state = _state;
		MaterialAsset* material = _frame ? _frame->items[_flatItem].material.Get() : nullptr;
		ShaderAsset* shader = material ? material->shader.Get() : nullptr;
		if (!state || !_device || !shader || vertices.size() < 3) {
			return false;
		}
		RHI::TransientAllocation allocation = _device->AllocateTransient(sizeof(GPUPerDraw) + vertices.size_bytes(), 16);
		if (!allocation.IsValid()) {
			return false;
		}
		const u64 drawAddress = _device->GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
		GPUPerDraw element;
		element.world = Mat4::Identity();
		element.normalMatrix0 = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
		element.normalMatrix1 = Vec4(0.0f, 1.0f, 0.0f, 0.0f);
		element.normalMatrix2 = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
		element.vertexAddress = drawAddress + sizeof(GPUPerDraw);
		element.alphaClipThreshold = material->alphaClipThreshold;
		const RHI::BlendMode blend = material->ResolveRenderState(shader->renderState).blend;
		if (receiveShadows && (blend == RHI::BlendMode::Opaque || state->transparentReceiveShadows)) {
			element.flags |= DRAW_RECEIVES_SHADOWS;
		}
		element.flags |= static_cast<u32>(blend) << DRAW_BLEND_SHIFT;
		element.flags |= (_frame->renderers[_frame->items[_flatItem].renderer].renderingLayerMask & LIGHT_LAYERS) << DRAW_LAYERS_SHIFT;
		u8* cpu = static_cast<u8*>(allocation.cpu);
		std::memcpy(cpu, &element, sizeof(GPUPerDraw));
		std::memcpy(cpu + sizeof(GPUPerDraw), vertices.data(), vertices.size_bytes());

		GPUDrawHeader header;
		header.drawAddress = drawAddress;
		header.value = _value;
		if (!BindItem({reinterpret_cast<const u8*>(&header), DRAW_HEADER_BYTES}, true)) {
			return false;
		}
		_cmd->SetFrontFace(RHI::FrontFace::Clockwise);
		_cmd->Draw(static_cast<u32>(vertices.size()));
		++*_drawCalls;
		return true;
	}

	bool ProceduralContext::DrawIndirect(std::span<const u8> pushConstants, RHI::BufferHandle arguments, u64 offset) {
		if (!_frame) {
			return false;
		}
		const GraphicsInternal::ProceduralFrame& frame = *_frame;
		const GraphicsInternal::ProceduralFrame::Item& item = frame.items[_flatItem];
		bool declared = false;
		for (u32 read = item.firstRead; read < item.firstRead + item.readCount; ++read) {
			declared = declared || (frame.reads[read].buffer == arguments && frame.reads[read].state == RHI::ResourceState::IndirectArgument);
		}
		if (!declared) {
			SLT_LOG_WARN("Graphics", "a procedural item's indirect draw read arguments it didn't declare as an IndirectArgument read. Dropped");
			return false;
		}
		if (!BindItem(pushConstants, false)) {
			return false;
		}
		_cmd->DrawIndirect(arguments, offset);
		++*_drawCalls;
		return true;
	}

	namespace {

		void FillDefaultTextures(RenderState& state) {
			RHI::Device& device = *state.device;
			const u16 ambient[4] = {Math::FloatToHalf(DefaultAmbient.x),
									Math::FloatToHalf(DefaultAmbient.y),
									Math::FloatToHalf(DefaultAmbient.z),
									Math::FloatToHalf(1.0f)};
			for (RHI::TextureHandle cube : {state.defaultEnvironmentCube, state.defaultReflectionProbeArray}) {
				for (u32 face = 0; cube.IsValid() && face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					device.UploadTexture(cube, {reinterpret_cast<const u8*>(ambient), sizeof(ambient)}, 0, face);
				}
			}
			if (state.defaultAmbientOcclusion.IsValid()) {
				const u8 unoccluded = 0xFF;
				device.UploadTexture(state.defaultAmbientOcclusion, {&unoccluded, sizeof(unoccluded)}, 0, 0);
			}
		}

		void DropFrameSubmissions(RenderState& state) {
			state.pendingViews.clear();
			state.viewLines.clear();
			state.viewPassRows.clear();
			state.viewScenes.clear();
			state.viewSkippedTypes.clear();
			state.viewOutlines.clear();
			state.passScopes.clear();
			state.cameraSubmissions.clear();
			state.submittedCameras.clear();
		}

		void RestoreDeviceContent(RenderState& state) {
			RHI::Device& device = *state.device;
			for (auto& [key, entry] : state.pipelineCache) {
				device.DestroyResource(entry.pipeline);
			}
			state.pipelineCache.clear();
			state.pipelineCacheSwept = 0;

			for (u32 dimension = 0; dimension < RenderState::FALLBACK_DIMENSIONS; ++dimension) {
				for (u32 kind = 0; kind < RenderState::FALLBACK_KINDS; ++kind) {
					if (state.fallbackTextures[dimension][kind].IsValid()) {
						FillFallbackTexture(device, state.fallbackTextures[dimension][kind], static_cast<RHI::TextureDimension>(dimension),
											static_cast<ShaderTextureFallback>(kind));
					}
				}
			}
			FillDefaultTextures(state);

			state.reflectionProbeArrayFace = 0;
			for (RenderState::BakedSky& baked : state.bakedSkies) {
				baked.key = 0;
			}
			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				ReleaseSkyCapture(device, capture);
			}
			for (auto& [row, volume] : state.rowVolumes) {
				volume.uploaded = false;
			}
			for (RenderState::CameraDepthRecord& record : state.cameraDepths) {
				record.drawn = false;
			}
			for (RenderState::PickSlot& slot : state.pickSlots) {
				slot.inUse = false;
				slot.objects.clear();
			}
			for (auto& [id, target] : state.viewTargets) {
				target.colorSampleable = false;
				target.drawn = false;
				target.colorLost = true;
				target.radianceLost = target.radiance.IsValid();
			}

			ObjectRegistry::ForEachLiveObject(
				+[](Object* object, u64, void*) {
					const TypeInfo* type = object->GetType();
					if (TypeRegistry::IsDerivedFrom(type, TextureAsset::GetReflectType())) {
						GraphicsInternal::RestoreDeviceContent(static_cast<TextureAsset&>(*object));
					} else if (TypeRegistry::IsDerivedFrom(type, MeshAsset::GetReflectType())) {
						GraphicsInternal::RestoreDeviceContent(static_cast<MeshAsset&>(*object));
					}
				},
				nullptr);
		}

		bool RecoverDevice(RenderState& state) {
			if (!state.device->Recover()) {
				return false;
			}
			RestoreDeviceContent(state);
			if (state.device->IsLost()) {
				return false;
			}
			GraphicsEvents::DeviceRestored.Publish(EventBus::GetGlobal(), GraphicsDeviceRestoredEvent{});
			return !state.device->IsLost();
		}

	} // anonymous namespace

	void RenderSystem::Initialize() {
		if (_state) {
			return;
		}
		_state = New<RenderState>(MemTag::Graphics);
		RenderState& state = *_state;

		FrameStages::Register(
			FrameStage::Render, FrameStagePosition::Normal,
			[](const FrameStageArgs&, void*) { RenderSystem::RenderAll(); },
			nullptr, &_FRAME_STAGE_OWNER);
		FrameStages::Register(
			FrameStage::UIUpdate, FrameStagePosition::Early,
			[](const FrameStageArgs&, void*) {
				if (_state && _state->device && _state->device->IsLost()) {
					RecoverDevice(*_state);
				}
			},
			nullptr, &_FRAME_STAGE_OWNER);
		FrameStages::Register(
			FrameStage::EndFrame, FrameStagePosition::Early,
			[](const FrameStageArgs&, void*) { GraphicsInternal::DeliverFinishedCaptures(); },
			nullptr, &_FRAME_STAGE_OWNER);

		SLT_RENDER_SYSTEM_ANCHORS(SLT_ANCHOR_TYPE)

		static GraphicsSettings graphicsDefaults;
		const GraphicsSettings* declared =
			Settings::DeclareCategory<GraphicsSettings>("Graphics", SettingScope::Project, true);
		if (!declared) {
			declared = &graphicsDefaults;
		}
		const GraphicsSettings& graphics = *declared;
		state.graphicsSettings = declared;

		state.builtIn = New<GraphicsProfile>(MemTag::Graphics);
		state.applied = New<GraphicsProfile>(MemTag::Graphics);
		state.physicalLightUnits = graphics.physicalLightUnits;
		state.colorSpace = graphics.colorSpace == ColorSpace::Gamma ? ColorSpace::Gamma : ColorSpace::Linear;

		SettingsEvents::Changed.Subscribe(EventBus::GetGlobal(), Handle<RenderState>(&state), &RenderState::OnSettingChanged);

		RHI::DeviceDesc desc;
		desc.enableValidation = graphics.validation;
		desc.forceAdapterIndex = graphics.adapter;

		state.vsyncOverride = ResolveVSyncOverride(state);

		state.device = RHI::Device::Create(desc);
		state.renderingAvailable = state.device != nullptr;
		if (state.renderingAvailable) {
			ShaderAsset::SetDefaultImportBytecodeFormat(state.device->GetCapabilities().shaderFormat);
		}

		Handle<RenderState> self(&state);
		EventBus& bus = EventBus::GetGlobal();
		WindowEvents::Resized.Subscribe(bus, self, &RenderState::OnWindowResized);
		WindowEvents::Destroying.Subscribe(bus, self, &RenderState::OnWindowDestroying);

		if (state.renderingAvailable) {
			state.extractionArena = New<LinearAllocator>(MemTag::Graphics, EXTRACTION_ARENA_BYTES, MemTag::Graphics);

			MemoryManager::SetVramProvider(+[](u64& outUsed, u64& outBudget) -> bool {
				if (!_state || !_state->device) {
					return false;
				}
				RHI::VideoMemoryInfo info = _state->device->GetVideoMemoryInfo();
				outUsed = info.usedBytes;
				outBudget = info.budgetBytes;
				return true;
			});

			state.presentShader = LoadEngineShader(EngineBuiltins::PresentPassShader, "PresentPass");
			state.standardShader = LoadEngineShader(EngineBuiltins::LitShader, "Lit");
			state.debugLineShader = LoadEngineShader(EngineBuiltins::DebugLinesShader, "DebugLines");

			struct EagerFallback {
				ShaderTextureFallback fallback;
				const char* debugName;
			};
			constexpr EagerFallback EagerFallbacks[] = {
				{ShaderTextureFallback::White, "WhiteFallback"},
				{ShaderTextureFallback::Black, "BlackFallback"},
				{ShaderTextureFallback::Normal, "NormalFallback"},
				{ShaderTextureFallback::Transparent, "TransparentFallback"},
			};
			for (const EagerFallback& eager : EagerFallbacks) {
				state.fallbackTextures[static_cast<u32>(RHI::TextureDimension::Texture2D)][static_cast<u32>(eager.fallback)] =
					CreateFallbackTexture(*state.device, RHI::TextureDimension::Texture2D, eager.fallback, eager.debugName);
			}

			RHI::SamplerDesc repeatDesc;
			repeatDesc.debugName = "DefaultRepeat";
			state.repeatSampler = state.device->CreateSamplerResource(repeatDesc);
			RHI::SamplerDesc clampDesc;
			clampDesc.addressMode = RHI::AddressMode::ClampToEdge;
			clampDesc.debugName = "PresentClamp";
			state.clampSampler = state.device->CreateSamplerResource(clampDesc);

			ApplyProfileToState(state, *state.applied);
			EnsureShadowAtlases(state);

			RHI::SamplerDesc shadowDesc;
			shadowDesc.filter = RHI::FilterMode::Nearest;
			shadowDesc.mipFilter = RHI::FilterMode::Nearest;
			shadowDesc.addressMode = RHI::AddressMode::ClampToEdge;
			shadowDesc.debugName = "ShadowGather";
			state.shadowSampler = state.device->CreateSamplerResource(shadowDesc);

			RHI::TextureDesc environmentDesc;
			environmentDesc.size = {1, 1};
			environmentDesc.format = RHI::Format::RGBA16Float;
			environmentDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			environmentDesc.arrayLayers = GraphicsInternal::CUBE_FACE_COUNT;
			environmentDesc.cube = true;
			environmentDesc.debugName = "DefaultEnvironment";
			state.defaultEnvironmentCube = state.device->CreateTextureResource(environmentDesc);

			RHI::TextureDesc probeArrayDesc;
			probeArrayDesc.size = {1, 1};
			probeArrayDesc.format = RHI::Format::RGBA16Float;
			probeArrayDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			probeArrayDesc.arrayLayers = GraphicsInternal::CUBE_FACE_COUNT;
			probeArrayDesc.cube = true;
			probeArrayDesc.cubeArray = true;
			probeArrayDesc.debugName = "DefaultReflectionProbes";
			state.defaultReflectionProbeArray = state.device->CreateTextureResource(probeArrayDesc);

			RHI::TextureDesc occlusionDesc;
			occlusionDesc.size = {1, 1};
			occlusionDesc.format = AMBIENT_OCCLUSION_FORMAT;
			occlusionDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			occlusionDesc.debugName = "DefaultAmbientOcclusion";
			state.defaultAmbientOcclusion = state.device->CreateTextureResource(occlusionDesc);
			FillDefaultTextures(state);

			RHI::SamplerDesc linearClampDesc;
			linearClampDesc.filter = RHI::FilterMode::Linear;
			linearClampDesc.addressMode = RHI::AddressMode::ClampToEdge;
			linearClampDesc.debugName = "LinearClampSampler";
			state.linearClampSampler = state.device->CreateSamplerResource(linearClampDesc);

			SLT_LOG_INFO("Graphics", "RenderSystem initialized on '{}' ({}x MSAA)", state.device->GetSelectedAdapter().name, state.msaaSamples);
		} else {
			SLT_LOG_INFO("Graphics", "RenderSystem initialized headless (no rendering available)");
		}
	}

	void RenderSystem::Terminate() {
		if (!_state) {
			return;
		}
		RenderState& state = *_state;

		EventBus::GetGlobal().UnsubscribeAll(state.GetInstanceId());
		FrameStages::UnregisterOwnedBy(&_FRAME_STAGE_OWNER);
		GraphicsInternal::DropCaptureCallbacks();
		MemoryManager::SetVramProvider(nullptr);

		for (ExtractionLane* lane : state.extractionLanes) {
			if (lane->ownsArena) {
				Delete(lane->arena);
			}
			Delete(lane);
		}
		state.extractionLanes.clear();
		Delete(state.errorMaterial);
		state.errorMaterial = nullptr;
		Delete(state.extractionArena);
		state.extractionArena = nullptr;

		if (state.activeProfile.IsValid()) {
			AssetDatabase::UnloadAsset(state.activeProfile.Get());
			state.activeProfile = {};
		}
		state.activeProfileId = {};
		state.graphicsSettings = nullptr;
		Delete(state.applied);
		state.applied = nullptr;
		Delete(state.builtIn);
		state.builtIn = nullptr;

		if (state.device) {
			state.device->WaitIdle();

			ReleaseEngineShader(state.presentShader);
			ReleaseEngineShader(state.standardShader);
			ReleaseEngineShader(state.debugLineShader);
			ReleaseEngineShader(state.wireframeShader);
			ReleaseEngineShader(state.skyShader);
			ReleaseEngineShader(state.skyPrefilterShader);
			ReleaseEngineShader(state.blitShader);
			ReleaseEngineShader(state.depthCopyShader);
			ReleaseEngineShader(state.debugViewShader);
			ReleaseEngineShader(state.debugOverlayShader);
			ReleaseEngineShader(state.outlineShader);
			ReleaseEngineShader(state.skinningShader);
			ReleaseEngineShader(state.depthRangeShader);
			ReleaseEngineShader(state.depthRangeShaderMultisampled);
			ReleaseEngineShader(state.momentResolveShader);
			state.skinDeformer.Destroy(*state.device);
			state.isolatedSkinDeformer.Destroy(*state.device);
			for (RHI::BufferHandle* buffer : {&state.depthRangeRing, &state.depthRangeScratch, &state.depthRangeSeed}) {
				if (buffer->IsValid()) {
					state.device->DestroyResource(*buffer);
					*buffer = {};
				}
			}
			if (state.pickRing.IsValid()) {
				state.device->DestroyResource(state.pickRing);
				state.pickRing = {};
			}

			for (RenderState::FrameCapture& capture : state.frameCaptures) {
				if (capture.readback.IsValid()) {
					state.device->DestroyResource(capture.readback);
				}
			}
			state.frameCaptures.clear();

			for (auto& [id, target] : state.viewTargets) {
				if (target.color.IsValid()) {
					state.device->DestroyResource(target.color);
				}
			}
			state.viewTargets.clear();

			for (auto& [row, volume] : state.rowVolumes) {
				if (volume.texture.IsValid()) {
					state.device->DestroyResource(volume.texture);
				}
			}
			state.rowVolumes.clear();

			for (const RenderState::CameraDepthRecord& record : state.cameraDepths) {
				if (record.image.IsValid()) {
					state.device->DestroyResource(record.image);
				}
			}
			for (RHI::TextureHandle image : state.retiredCameraDepths) {
				state.device->DestroyResource(image);
			}
			state.cameraDepths.clear();
			state.retiredCameraDepths.clear();

			state.graph.DestroyPool();
			state.pendingViews.clear();
			state.viewLines.clear();
			state.viewPassRows.clear();
			state.viewScenes.clear();
			state.viewSkippedTypes.clear();
			state.viewOutlines.clear();
			state.passScopes.clear();
			state.cameraSubmissions.clear();
			state.submittedCameras.clear();

			for (auto& [key, entry] : state.pipelineCache) {
				state.device->DestroyResource(entry.pipeline);
			}
			state.pipelineCache.clear();

			Vector<u64> ids{GetMemoryResource(MemTag::Graphics)};
			for (auto& [windowId, context] : state.windowContexts) {
				ids.push_back(windowId);
			}
			for (u64 windowId : ids) {
				state.DestroyWindowContext(windowId);
			}

			for (auto& byDimension : state.fallbackTextures) {
				for (RHI::TextureHandle& fallback : byDimension) {
					if (fallback.IsValid()) {
						state.device->DestroyResource(fallback);
					}
				}
			}
			for (const RenderState::CachedSampler& cached : state.textureSamplers) {
				if (cached.handle.IsValid()) {
					state.device->DestroyResource(cached.handle);
				}
			}
			state.textureSamplers.clear();
			if (state.repeatSampler.IsValid()) {
				state.device->DestroyResource(state.repeatSampler);
			}
			if (state.clampSampler.IsValid()) {
				state.device->DestroyResource(state.clampSampler);
			}
			if (state.shadowAtlas.IsValid()) {
				state.device->DestroyResource(state.shadowAtlas);
			}
			if (state.punctualShadowAtlas.IsValid()) {
				state.device->DestroyResource(state.punctualShadowAtlas);
			}
			for (RHI::TextureHandle* texture : {&state.momentAtlas, &state.momentDepth}) {
				if (texture->IsValid()) {
					state.device->DestroyResource(*texture);
					*texture = {};
				}
			}
			if (state.shadowSampler.IsValid()) {
				state.device->DestroyResource(state.shadowSampler);
			}
			if (state.defaultAmbientOcclusion.IsValid()) {
				state.device->DestroyResource(state.defaultAmbientOcclusion);
				state.defaultAmbientOcclusion = {};
			}
			if (state.defaultReflectionProbeArray.IsValid()) {
				state.device->DestroyResource(state.defaultReflectionProbeArray);
				state.defaultReflectionProbeArray = {};
			}
			if (state.reflectionProbeArray.IsValid()) {
				state.device->DestroyResource(state.reflectionProbeArray);
				state.reflectionProbeArray = {};
			}
			if (state.defaultEnvironmentCube.IsValid()) {
				state.device->DestroyResource(state.defaultEnvironmentCube);
			}
			if (state.linearClampSampler.IsValid()) {
				state.device->DestroyResource(state.linearClampSampler);
			}
			for (RenderState::BakedSky& baked : state.bakedSkies) {
				if (baked.cube.IsValid()) {
					state.device->DestroyResource(baked.cube);
				}
			}
			state.bakedSkies.clear();
			for (RenderState::SkyCapture& capture : state.skyCaptures) {
				ReleaseSkyCapture(*state.device, capture);
			}
			state.skyCaptures.clear();

			RHI::Device::Destroy(state.device);
			state.device = nullptr;
		}
#ifdef SHALLTEAR_RHI_WEBGPU
		ShaderAsset::SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat::WGSL);
#else
		ShaderAsset::SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat::Spirv);
#endif

		Delete(_state);
		_state = nullptr;
	}

	bool RenderSystem::IsInitialized() {
		return _state != nullptr;
	}

	bool RenderSystem::IsRenderingAvailable() {
		return _state && _state->renderingAvailable;
	}

	GraphicsInternal::PresentCounts GraphicsInternal::GetLastFramePresents() {
		return RenderSystem::IsRenderingAvailable() ? _state->lastPresents : PresentCounts{};
	}

	RHI::Device* RenderSystem::GetDevice() {
		return _state ? _state->device : nullptr;
	}

	void RenderSystem::SetWindowOverlay(IWindowOverlay* overlay) {
		if (_state) {
			_state->overlay = overlay;
		}
	}

	u32 RenderSystem::GetSampleCount() {
		return _state ? _state->msaaSamples : 1u;
	}

	const GraphicsProfile* RenderSystem::GetActiveProfile() {
		return _state ? _state->applied : nullptr;
	}

	i32 RenderSystem::GetReflectionProbeResolution() {
		return _state ? _state->reflectionProbeResolution : 0;
	}

	GraphicsStats RenderSystem::GetStats() {
		GraphicsStats stats;
		if (!_state || !_state->device) {
			return stats;
		}
		RHI::VideoMemoryInfo memory = _state->device->GetVideoMemoryInfo();
		stats.vramUsedBytes = memory.usedBytes;
		stats.vramBudgetBytes = memory.budgetBytes;
		stats.liveBuffers = _state->device->GetLiveBufferCount();
		stats.liveTextures = _state->device->GetLiveTextureCount();
		stats.liveSamplers = _state->device->GetLiveSamplerCount();
		stats.livePipelines = _state->device->GetLivePipelineCount();
		stats.drawCalls = _state->lastDrawCalls;
		stats.triangles = _state->lastTriangles;
		stats.lightsBinned = _state->lastLightsBinned;
		stats.lightsDropped = _state->lastLightsDropped;
		stats.clusterLightsDropped = _state->lastClusterLightsDropped;
		stats.shadowMapsRendered = _state->lastShadowMapsRendered;
		stats.shadowLights = _state->lastShadowLights;
		stats.shadowLightsDropped = _state->lastShadowLightsDropped;
		stats.proceduralDrawn = _state->lastProceduralDrawn;
		stats.proceduralCulled = _state->lastProceduralCulled;
		stats.drawCallsSaved = _state->lastDrawCallsSaved;
		stats.extractionLanes = _state->lastExtractionLanes;
		return stats;
	}

	namespace {

		void AllocateViewTarget(RenderState::ViewTarget& target, Vec2I size) {
			RHI::Device& device = *_state->device;
			if (target.color.IsValid()) {
				device.DestroyResource(target.color);
			}
			if (target.radiance.IsValid()) {
				device.DestroyResource(target.radiance);
				target.radiance = {};
			}

			RHI::TextureDesc colorDesc;
			colorDesc.size = size;
			colorDesc.format = RHI::Format::RGBA8Unorm;
			colorDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
			colorDesc.debugName = "ViewTargetColor";
			target.color = device.CreateTextureResource(colorDesc);

			target.size = size;
			target.colorSampleable = false;
			target.drawn = false;
			target.colorLost = false;
			target.radianceLost = false;
		}

	} // anonymous namespace

	ViewTargetId RenderSystem::CreateViewTarget(Vec2I sizePx) {
		if (!IsRenderingAvailable() || sizePx.x <= 0 || sizePx.y <= 0) {
			return INVALID_VIEW_TARGET;
		}
		RenderState& state = *_state;
		u32 id = state.nextViewTargetId++;
		RenderState::ViewTarget& target = state.viewTargets[id];
		AllocateViewTarget(target, sizePx);
		return id;
	}

	void RenderSystem::ResizeViewTarget(ViewTargetId target, Vec2I sizePx) {
		if (!IsRenderingAvailable() || sizePx.x <= 0 || sizePx.y <= 0) {
			return;
		}
		auto it = _state->viewTargets.find(target);
		if (it == _state->viewTargets.end() || it->second.size == sizePx) {
			return;
		}
		AllocateViewTarget(it->second, sizePx);
	}

	void RenderSystem::DestroyViewTarget(ViewTargetId target) {
		if (!_state || !_state->device) {
			return;
		}
		RenderState& state = *_state;
		auto it = state.viewTargets.find(target);
		if (it == state.viewTargets.end()) {
			return;
		}
		if (it->second.color.IsValid()) {
			state.device->DestroyResource(it->second.color);
		}
		if (it->second.radiance.IsValid()) {
			state.device->DestroyResource(it->second.radiance);
		}
		state.viewTargets.erase(it);
		std::erase_if(state.pendingViews, [target](const RenderState::PendingView& view) { return view.targetId == target; });
	}

	RHI::TextureHandle RenderSystem::GetViewTargetTexture(ViewTargetId target) {
		if (!_state) {
			return {};
		}
		auto it = _state->viewTargets.find(target);
		return it == _state->viewTargets.end() ? RHI::TextureHandle{} : it->second.color;
	}

	Vec2I RenderSystem::GetViewTargetSize(ViewTargetId target) {
		if (!_state) {
			return {};
		}
		auto it = _state->viewTargets.find(target);
		return it == _state->viewTargets.end() ? Vec2I{} : it->second.size;
	}

	bool RenderSystem::IsViewTargetDrawn(ViewTargetId target) {
		if (!_state) {
			return false;
		}
		auto it = _state->viewTargets.find(target);
		return it != _state->viewTargets.end() && it->second.drawn;
	}

	u32 RenderSystem::GetCascadeFit(ViewTargetId target, std::span<CascadeFit> out) {
		if (!_state) {
			return 0;
		}
		auto it = _state->viewTargets.find(target);
		if (it == _state->viewTargets.end()) {
			return 0;
		}
		const Vector<GraphicsInternal::CascadeView>& fit = it->second.cascadeFit;
		const u32 count = Math::Min(static_cast<u32>(fit.size()), static_cast<u32>(out.size()));
		for (u32 i = 0; i < count; ++i) {
			out[i].viewProjection = fit[i].viewProjection;
			out[i].worldTexelSize = fit[i].worldTexelSize;
			out[i].farDepth = fit[i].farDepth;
		}
		return count;
	}

	bool RenderSystem::TakePickAnswer(ViewTargetId target, PickAnswer& out) {
		if (!_state) {
			return false;
		}
		auto it = _state->viewTargets.find(target);
		if (it == _state->viewTargets.end() || it->second.pickAnswers.empty()) {
			return false;
		}
		Vector<PickAnswer>& answers = it->second.pickAnswers;
		out = answers.front();
		answers.erase(answers.begin());
		return true;
	}

	void RenderSystem::SubmitView(ViewTargetId target, const ViewDesc& desc) {
		if (!IsRenderingAvailable()) {
			return;
		}
		RenderState& state = *_state;
		if (state.viewTargets.find(target) == state.viewTargets.end()) {
			return;
		}

		bool namedWorld = false;
		bool namedIsolated = false;
		for (const Handle<Scene>& sceneHandle : desc.scenes) {
			if (const Scene* scene = sceneHandle.Get()) {
				(scene->GetKind() == SceneKind::World ? namedWorld : namedIsolated) = true;
			}
		}
		if (namedWorld && namedIsolated) {
			SLT_LOG_WARN("Graphics", "SubmitView: a view has world scenes AND isolated ones. Dropped");
			return;
		}

		RenderState::PendingView pending;
		pending.kind = namedIsolated ? SceneKind::Isolated : SceneKind::World;
		pending.targetId = target;
		pending.view = desc.view;
		pending.projection = desc.projection;
		pending.cameraPosition = desc.cameraPosition;
		if (const Camera* camera = desc.camera.Get()) {
			pending.cullingMask = camera->GetCullingMask();
		}
		pending.clearColor = desc.clearColor;
		pending.fill = desc.fill;
		pending.wireColor = desc.wireColor;
		pending.debugView = desc.debugView;
		pending.debugOverlay = desc.debugOverlay;
		pending.hasTime = desc.time != nullptr;
		pending.time = desc.time ? *desc.time : 0.0;
		pending.tonemap = desc.tonemap;
		pending.camera = desc.camera;
		pending.retainCascadeFit = desc.retainCascadeFit;
		pending.radianceCapture = desc.radianceCapture;
		if ((desc.shadowFitView == nullptr) != (desc.shadowFitProjection == nullptr)) {
			SLT_LOG_WARN("Graphics", "SubmitView: shadowFitView and shadowFitProjection must be set together. Ignored");
		} else if (desc.shadowFitView) {
			pending.hasShadowFit = true;
			pending.shadowFit.view = *desc.shadowFitView;
			pending.shadowFit.projection = *desc.shadowFitProjection;
		}
		pending.firstLine = static_cast<u32>(state.viewLines.size());
		pending.lineCount = static_cast<u32>(desc.lines.size());
		state.viewLines.insert(state.viewLines.end(), desc.lines.begin(), desc.lines.end());
		pending.firstOverlayLine = static_cast<u32>(state.viewLines.size());
		pending.overlayLineCount = static_cast<u32>(desc.overlayLines.size());
		state.viewLines.insert(state.viewLines.end(), desc.overlayLines.begin(), desc.overlayLines.end());

		pending.firstRow = static_cast<u32>(state.viewPassRows.size());
		for (const Handle<RenderPass>& row : desc.passes) {
			const RenderPass* pass = row.Get();
			if (!pass) {
				continue;
			}
			if (!IsPerViewPassPoint(pass->point)) {
				SLT_LOG_WARN("Graphics", "SubmitView: a row asked for '{}' (not a per-view point). Dropped",
							 GetPassPointName(pass->point));
				continue;
			}
			state.viewPassRows.push_back(row);
		}
		pending.rowCount = static_cast<u32>(state.viewPassRows.size()) - pending.firstRow;

		pending.firstScene = static_cast<u32>(state.viewScenes.size());
		for (const Handle<Scene>& scene : desc.scenes) {
			if (scene.IsValid()) {
				state.viewScenes.push_back(scene.GetInstanceId());
			}
		}
		pending.sceneCount = static_cast<u32>(state.viewScenes.size()) - pending.firstScene;
		pending.firstSkipped = static_cast<u32>(state.viewSkippedTypes.size());
		for (const StringHash type : desc.skippedProceduralTypes) {
			state.viewSkippedTypes.push_back(type.GetValue());
		}
		pending.skippedCount = static_cast<u32>(desc.skippedProceduralTypes.size());

		pending.firstOutline = static_cast<u32>(state.viewOutlines.size());
		pending.outlineWidth = desc.outlineWidth;
		pending.outlineHiddenOpacity = desc.outlineHiddenOpacity;
		bool droppedColor = false;
		for (const ViewOutline& outline : desc.outlines) {
			if (!outline.object.IsValid()) {
				continue;
			}
			u32 value = 0;
			for (u32 index = 0; index < pending.outlineColorCount; ++index) {
				if (pending.outlineColors[index] == outline.color) {
					value = index + 1;
					break;
				}
			}
			if (value == 0) {
				if (pending.outlineColorCount == MAX_OUTLINE_COLORS) {
					droppedColor = true;
					continue;
				}
				pending.outlineColors[pending.outlineColorCount++] = outline.color;
				value = pending.outlineColorCount;
			}
			state.viewOutlines.push_back({outline.object.GetInstanceId(), value});
		}
		std::sort(state.viewOutlines.begin() + pending.firstOutline, state.viewOutlines.end(),
				  [](const RenderState::OutlineEntry& a, const RenderState::OutlineEntry& b) {
					  return a.object != b.object ? a.object < b.object : a.value < b.value;
				  });
		if (droppedColor) {
			SLT_LOG_WARN("Graphics", "SubmitView: outlines in more than {} colors (rest aren't outlined)",
						 MAX_OUTLINE_COLORS);
		}
		if (desc.pick) {
			pending.hasPick = true;
			pending.pick = *desc.pick;
		}
		pending.outlineCount = static_cast<u32>(state.viewOutlines.size()) - pending.firstOutline;

		pending.scopeIndex = static_cast<u32>(state.passScopes.size());
		state.passScopes.push_back({pending.firstScene, pending.sceneCount, pending.kind});

		for (RenderState::PendingView& existing : state.pendingViews) {
			if (existing.targetId == target) {
				existing = pending;
				return;
			}
		}
		state.pendingViews.push_back(pending);
	}

	void RenderSystem::SubmitCameras(ViewTargetId target, std::span<const Handle<Camera>> cameras) {
		if (!IsRenderingAvailable()) {
			return;
		}
		RenderState& state = *_state;
		if (state.viewTargets.find(target) == state.viewTargets.end()) {
			return;
		}
		RenderState::CameraSubmission submission;
		submission.target = target;
		submission.firstCamera = static_cast<u32>(state.submittedCameras.size());
		submission.cameraCount = static_cast<u32>(cameras.size());
		state.submittedCameras.insert(state.submittedCameras.end(), cameras.begin(), cameras.end());
		for (RenderState::CameraSubmission& existing : state.cameraSubmissions) {
			if (existing.target == target) {
				existing = submission;
				return;
			}
		}
		state.cameraSubmissions.push_back(submission);
	}

	void RenderSystem::SetSceneCamerasRenderToWindows(bool enabled) {
		if (_state) {
			_state->sceneCamerasRenderToWindows = enabled;
		}
	}

	bool RenderSystem::GetSceneCamerasRenderToWindows() {
		return _state ? _state->sceneCamerasRenderToWindows : true;
	}

	bool RenderSystem::ReadbackViewTarget(ViewTargetId target, Vector<u8>& outRGBA8, Vec2I& outSize) {
		if (!IsRenderingAvailable()) {
			return false;
		}
		auto it = _state->viewTargets.find(target);
		if (it == _state->viewTargets.end() || !it->second.color.IsValid() || it->second.colorLost) {
			return false;
		}
		outSize = it->second.size;
		outRGBA8.resize(static_cast<size_t>(outSize.x) * outSize.y * 4);
		_state->device->ReadbackTexture(it->second.color, {outRGBA8.data(), outRGBA8.size()});
		return !_state->device->IsLost();
	}

	bool RenderSystem::ReadbackViewTargetRadiance(ViewTargetId target, Vector<u8>& outRGBA16F, Vec2I& outSize) {
		if (!IsRenderingAvailable()) {
			return false;
		}
		auto it = _state->viewTargets.find(target);
		if (it == _state->viewTargets.end() || !it->second.radiance.IsValid() || it->second.radianceLost) {
			return false;
		}
		outSize = _state->device->GetTextureSize(it->second.radiance);
		outRGBA16F.resize(static_cast<size_t>(outSize.x) * outSize.y * 8);
		_state->device->ReadbackTexture(it->second.radiance, {outRGBA16F.data(), outRGBA16F.size()});
		return !_state->device->IsLost();
	}

	namespace {

		SceneKind KindOfOwningScene(const Component& component) {
			const Scene* scene = component.GetScene().Get();
			return scene ? scene->GetKind() : SceneKind::World;
		}

	} // anonymous namespace

	void RenderSystem::RegisterCamera(Handle<Camera> camera) {
		Camera* target = camera.Get();
		if (!_state || !target) {
			return;
		}
		if (KindOfOwningScene(*target) != SceneKind::World) {
			return;
		}
		if (!ContainsHandle(_state->cameras, camera)) {
			_state->cameras.push_back(camera);
		}
	}
	void RenderSystem::UnregisterCamera(Handle<Camera> camera) {
		if (_state) {
			SwapRemoveHandle(_state->cameras, camera);
		}
	}
	std::span<const Handle<Camera>> RenderSystem::GetCameras() {
		if (!_state) {
			return {};
		}
		return {_state->cameras.data(), _state->cameras.size()};
	}
	void RenderSystem::RegisterLight(Handle<Light> light) {
		Light* target = light.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<Light>>& list = KindOfOwningScene(*target) == SceneKind::World
										  ? _state->lights
										  : _state->isolatedLights;
		if (!ContainsHandle(list, light)) {
			list.push_back(light);
		}
	}
	void RenderSystem::UnregisterLight(Handle<Light> light) {
		if (_state) {
			SwapRemoveHandle(_state->lights, light);
			SwapRemoveHandle(_state->isolatedLights, light);
		}
	}
	void RenderSystem::RegisterDecal(Handle<Decal> decal) {
		Decal* target = decal.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<Decal>>& list = KindOfOwningScene(*target) == SceneKind::World ? _state->decals : _state->isolatedDecals;
		auto at = std::lower_bound(list.begin(), list.end(), decal, [](const Handle<Decal>& a, const Handle<Decal>& b) {
			return a.GetInstanceId() < b.GetInstanceId();
		});
		if (at == list.end() || *at != decal) {
			list.insert(at, decal);
		}
	}
	void RenderSystem::UnregisterDecal(Handle<Decal> decal) {
		if (_state) {
			for (Vector<Handle<Decal>>* list : {&_state->decals, &_state->isolatedDecals}) {
				list->erase(std::remove(list->begin(), list->end(), decal), list->end());
			}
		}
	}
	void RenderSystem::RegisterEnvironment(Handle<Environment> environment) {
		Environment* target = environment.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<Environment>>& list = KindOfOwningScene(*target) == SceneKind::World
												? _state->environments
												: _state->isolatedEnvironments;
		if (!ContainsHandle(list, environment)) {
			list.push_back(environment);
		}
	}
	void RenderSystem::UnregisterEnvironment(Handle<Environment> environment) {
		if (_state) {
			for (Vector<Handle<Environment>>* list : {&_state->environments, &_state->isolatedEnvironments}) {
				for (size_t i = 0; i < list->size(); ++i) {
					if ((*list)[i] == environment) {
						list->erase(list->begin() + static_cast<ptrdiff_t>(i));
						break;
					}
				}
			}
			SwapRemoveHandle(_state->warnedDuplicateEnvironments, environment);
			std::erase_if(_state->warnedSkySources, [&](const RenderState::WarnedSkySource& entry) { return entry.environment == environment; });
		}
	}
	void RenderSystem::RegisterReflectionProbe(Handle<ReflectionProbe> probe) {
		ReflectionProbe* target = probe.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<ReflectionProbe>>& list = KindOfOwningScene(*target) == SceneKind::World
													? _state->reflectionProbes
													: _state->isolatedReflectionProbes;
		if (!ContainsHandle(list, probe)) {
			list.push_back(probe);
		}
	}
	void RenderSystem::UnregisterReflectionProbe(Handle<ReflectionProbe> probe) {
		if (_state) {
			for (Vector<Handle<ReflectionProbe>>* list : {&_state->reflectionProbes, &_state->isolatedReflectionProbes}) {
				for (size_t i = 0; i < list->size(); ++i) {
					if ((*list)[i] == probe) {
						list->erase(list->begin() + static_cast<ptrdiff_t>(i));
						break;
					}
				}
			}
		}
	}
	void RenderSystem::RegisterMeshRenderer(Handle<MeshRenderer> renderer) {
		MeshRenderer* target = renderer.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<MeshRenderer>>& list = KindOfOwningScene(*target) == SceneKind::World
												 ? _state->meshRenderers
												 : _state->isolatedMeshRenderers;
		if (!ContainsHandle(list, renderer)) {
			list.push_back(renderer);
		}
		MarkLODRecordDirty(*_state, renderer.GetInstanceId());
	}
	void RenderSystem::UnregisterMeshRenderer(Handle<MeshRenderer> renderer) {
		if (_state) {
			SwapRemoveHandle(_state->meshRenderers, renderer);
			SwapRemoveHandle(_state->isolatedMeshRenderers, renderer);
			MarkLODRecordDirty(*_state, renderer.GetInstanceId());
		}
	}
	f32 GraphicsInternal::GetCameraDepthFit(u64 cameraInstanceId) {
		if (!_state || cameraInstanceId == 0) {
			return 0.0f;
		}
		const auto fit = _state->depthFits.find(cameraInstanceId & ~PENDING_VIEW_LOD_KEY);
		return fit != _state->depthFits.end() ? fit->second.applied : 0.0f;
	}

	void RenderSystem::RegisterLODGroup(Handle<LODGroup> group) {
		if (!_state || !group.Get()) {
			return;
		}
		RenderState& state = *_state;
		u32 free = ~0u;
		for (u32 i = 0; i < state.lodRecords.size(); ++i) {
			if (state.lodRecords[i].group == group) {
				return;
			}
			free = free == ~0u && state.lodRecords[i].group.GetInstanceId() == 0 ? i : free;
		}
		if (free == ~0u) {
			free = static_cast<u32>(state.lodRecords.size());
			state.lodRecords.emplace_back();
		}
		ForgetLODRecord(state, free);
		state.lodRecords[free].group = group;
		state.lodRecords[free].dirty = true;
	}
	void RenderSystem::UnregisterLODGroup(Handle<LODGroup> group) {
		if (!_state) {
			return;
		}
		for (u32 i = 0; i < _state->lodRecords.size(); ++i) {
			if (_state->lodRecords[i].group == group) {
				ForgetLODRecord(*_state, i);
				_state->lodRecords[i].group = {};
				return;
			}
		}
	}
	ColorSpace RenderSystem::GetColorSpace() {
		return _state ? _state->colorSpace : ColorSpace::Linear;
	}

	void RenderSystem::RegisterObjectShadow(Handle<ObjectShadow> shadow) {
		ObjectShadow* target = shadow.Get();
		if (!_state || !target || KindOfOwningScene(*target) != SceneKind::World) {
			return;
		}
		for (const RenderState::ObjectShadowRecord& record : _state->objectShadows) {
			if (record.shadow == shadow) {
				return;
			}
		}
		_state->objectShadows.push_back({shadow, 0.0f});
	}
	void RenderSystem::UnregisterObjectShadow(Handle<ObjectShadow> shadow) {
		if (!_state) {
			return;
		}
		Vector<RenderState::ObjectShadowRecord>& records = _state->objectShadows;
		for (size_t i = 0; i < records.size(); ++i) {
			if (records[i].shadow == shadow) {
				records[i] = records.back();
				records.pop_back();
				return;
			}
		}
	}
	void RenderSystem::RegisterSkinnedMeshRenderer(Handle<SkinnedMeshRenderer> renderer) {
		SkinnedMeshRenderer* target = renderer.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<SkinnedMeshRenderer>>& list = KindOfOwningScene(*target) == SceneKind::World
														? _state->skinnedMeshRenderers
														: _state->isolatedSkinnedMeshRenderers;
		if (!ContainsHandle(list, renderer)) {
			list.push_back(renderer);
		}
		MarkLODRecordDirty(*_state, renderer.GetInstanceId());
	}
	void GraphicsInternal::RegisterProceduralRenderer(Handle<ProceduralRenderer> renderer) {
		ProceduralRenderer* target = renderer.Get();
		if (!_state || !target) {
			return;
		}
		Vector<Handle<ProceduralRenderer>>& list = KindOfOwningScene(*target) == SceneKind::World
													   ? _state->proceduralRenderers
													   : _state->isolatedProceduralRenderers;
		if (!ContainsHandle(list, renderer)) {
			list.push_back(renderer);
		}
	}
	void GraphicsInternal::UnregisterProceduralRenderer(Handle<ProceduralRenderer> renderer) {
		if (_state) {
			SwapRemoveHandle(_state->proceduralRenderers, renderer);
			SwapRemoveHandle(_state->isolatedProceduralRenderers, renderer);
			MarkLODRecordDirty(*_state, renderer.GetInstanceId());
		}
	}

	void RenderSystem::UnregisterSkinnedMeshRenderer(Handle<SkinnedMeshRenderer> renderer) {
		if (_state) {
			SwapRemoveHandle(_state->skinnedMeshRenderers, renderer);
			SwapRemoveHandle(_state->isolatedSkinnedMeshRenderers, renderer);
			MarkLODRecordDirty(*_state, renderer.GetInstanceId());
		}
	}

	bool RenderSystem::ReadbackWindowSceneColor(Handle<Window> window, Vector<u8>& outRGBA16F, Vec2I& outSize) {
		if (!IsRenderingAvailable()) {
			return false;
		}
		RenderState& state = *_state;
		auto it = state.windowContexts.find(window.GetInstanceId());
		if (it == state.windowContexts.end() || !it->second.sceneColor.IsValid()) {
			return false;
		}
		outSize = state.device->GetTextureSize(it->second.sceneColor);
		outRGBA16F.resize(static_cast<size_t>(outSize.x) * outSize.y * 8);
		state.device->ReadbackTexture(it->second.sceneColor, {outRGBA16F.data(), outRGBA16F.size()});
		return !state.device->IsLost();
	}

	void RenderSystem::NotifyShaderFileChanged(const str& physicalPath) {
		if (!_state) {
			return;
		}
		std::lock_guard lock(_state->hotReloadMutex);
		_state->hotReloadPaths.push_back(physicalPath);
	}

	void RenderSystem::RenderAll() {
		if (!IsRenderingAvailable()) {
			return;
		}
		SLT_PROFILE_ZONE("Render.Frame");
		RenderState& state = *_state;
		RHI::Device& device = *state.device;
		state.lastPresents = {};

		if (device.IsLost() && !RecoverDevice(state)) {
			DropFrameSubmissions(state);
			return;
		}

		ReconcileGraphicsConfig(state);

		DrainHotReloads();

		ReconcileReflectionProbeArray(state);

		BuildLODFrame(state);

		using WindowWork = RenderState::WindowWork;
		Vector<WindowWork>& work = state.frameWork;
		u32 workCount = 0;
		for (WindowWork& entry : work) {
			entry.window = {};
			entry.target = INVALID_VIEW_TARGET;
			entry.cameras.clear();
		}
		auto findWork = [&](Handle<Window> window) -> WindowWork* {
			for (u32 i = 0; i < workCount; ++i) {
				if (work[i].window == window) {
					return &work[i];
				}
			}
			return nullptr;
		};
		auto acquireWork = [&](Handle<Window> window) -> WindowWork& {
			if (workCount == work.size()) {
				work.emplace_back();
			}
			WindowWork& entry = work[workCount++];
			entry.window = window;
			return entry;
		};
		auto appendCamera = [&](WindowWork& entry, Camera* camera) {
			RenderState::CameraWork cameraWork;
			cameraWork.camera = camera;
			cameraWork.firstRow = static_cast<u32>(state.viewPassRows.size());
			for (u32 slot = 0; slot < camera->GetPassCount(); ++slot) {
				Handle<RenderPass> row = camera->GetPass(slot);
				const RenderPass* pass = row.Get();
				if (!pass) {
					continue;
				}
				if (!IsPerViewPassPoint(pass->point)) {
					SLT_LOG_WARN("Graphics", "Camera: a row asked for '{}' (not a per-view point). Dropped",
								 GetPassPointName(pass->point));
					continue;
				}
				state.viewPassRows.push_back(row);
			}
			cameraWork.rowCount = static_cast<u32>(state.viewPassRows.size()) - cameraWork.firstRow;
			cameraWork.scopeIndex = static_cast<u32>(state.passScopes.size());
			state.passScopes.push_back({0, 0, SceneKind::World});
			entry.cameras.push_back(cameraWork);
		};

		Handle<Window> primary = WindowSystem::GetPrimaryWindow();
		for (Handle<Camera> cameraHandle : state.cameras) {
			if (!state.sceneCamerasRenderToWindows) {
				break;
			}
			Camera* camera = cameraHandle.Get();
			if (!camera) {
				continue;
			}
			Handle<Window> target = camera->GetTargetWindow().IsValid() ? camera->GetTargetWindow() : primary;
			Window* window = target.Get();
			if (!window || window->IsMinimized()) {
				continue;
			}
			Vec2I clientSize = window->GetClientSize();
			if (clientSize.x <= 0 || clientSize.y <= 0) {
				continue;
			}
			WindowWork* entry = findWork(target);
			if (!entry) {
				entry = &acquireWork(target);
			}
			appendCamera(*entry, camera);
		}

		for (const RenderState::CameraSubmission& submission : state.cameraSubmissions) {
			if (state.viewTargets.find(submission.target) == state.viewTargets.end()) {
				continue;
			}
			WindowWork& entry = acquireWork({});
			entry.target = submission.target;
			for (u32 index = 0; index < submission.cameraCount; ++index) {
				if (Camera* camera = state.submittedCameras[submission.firstCamera + index].Get()) {
					appendCamera(entry, camera);
				}
			}
		}

		if (state.overlay) {
			Vector<Handle<Window>>& overlayWindows = state.frameOverlayWindows;
			overlayWindows.clear();
			state.overlay->CollectOverlayWindows(overlayWindows);
			for (Handle<Window> overlayWindow : overlayWindows) {
				Window* window = overlayWindow.Get();
				if (!window || window->IsMinimized()) {
					continue;
				}
				Vec2I clientSize = window->GetClientSize();
				if (clientSize.x <= 0 || clientSize.y <= 0) {
					continue;
				}
				if (!findWork(overlayWindow)) {
					acquireWork(overlayWindow);
				}
			}
		}

		bool anyColdView = false;
		for (const auto& [id, target] : state.viewTargets) {
			if (!target.colorSampleable && target.color.IsValid()) {
				anyColdView = true;
				break;
			}
		}

		const bool anyOffscreenWork = state.overlay && state.overlay->HasOffscreenWork();

		const bool anyFramePass = GraphicsInternal::GetFramePassCount() > 0;

		if (workCount == 0 && state.pendingViews.empty() && !anyColdView && !anyOffscreenWork && !anyFramePass &&
			state.frameCaptures.empty()) {
			state.lastDrawCalls = 0;
			state.lastTriangles = 0;
			state.lastLightsBinned = 0;
			state.lastLightsDropped = 0;
			state.lastClusterLightsDropped = 0;
			state.lastShadowMapsRendered = 0;
			state.lastShadowLights = 0;
			state.lastShadowLightsDropped = 0;
			state.lastProceduralDrawn = 0;
			state.lastProceduralCulled = 0;
			state.lastDrawCallsSaved = 0;
			state.lastExtractionLanes = 0;
			DropFrameSubmissions(state);
			return;
		}

		ShaderAsset* presentShader = state.presentShader.Get();
		bool anyCameraWork = !state.pendingViews.empty();
		for (u32 i = 0; i < workCount; ++i) {
			if (!work[i].cameras.empty()) {
				anyCameraWork = true;
				break;
			}
		}
		if (anyCameraWork && !presentShader) {
			SLT_LOG_WARN("Graphics", "present shader unavailable. Skipping the frame");
			DropFrameSubmissions(state);
			return;
		}

		RHI::CommandList* cmd = nullptr;
		{
			SLT_PROFILE_ZONE("Render.Begin");
			cmd = device.BeginFrame();
		}
		++state.renderedFrames;
		AnswerFinishedPicks(state, device);
		HarvestDepthRanges(state, device);
		AnswerSkyReadbacks(state, device);
		AnswerFrameCaptures(state, device);

		{
			SLT_PROFILE_ZONE("Render.Build");
			ResolveFrameExposures(state, device, workCount);
			BuildFrame(state, device, workCount, anyColdView, anyOffscreenWork, presentShader);
			GraphicsInternal::ProceduralCulling::Publish(state.procedural);
		}
		{
			SLT_PROFILE_ZONE("Render.Compile");
			state.graph.Compile();
		}
		GraphExecution exec;
		exec.cmd = cmd;
		exec.device = &device;
		state.graph.Execute(exec);
		AgeCameraDepths(state, device);

		SweepRowVolumes(state);

		DropFrameSubmissions(state);

		device.EndFrame();

		state.presentedSwapChains.clear();
		f32 refreshRate = 0.0f;
		for (u32 workIndex = 0; workIndex < workCount; ++workIndex) {
			auto contextIt = state.windowContexts.find(work[workIndex].window.GetInstanceId());
			if (contextIt != state.windowContexts.end() && contextIt->second.presentedThisFrame) {
				contextIt->second.swapChain->Present();
				++state.lastPresents.windows;
				state.presentedSwapChains.push_back(contextIt->second.swapChain);
				if (contextIt->second.swapChain->GetVSync()) {
					++state.lastPresents.vsyncWindows;
				}
				if (const Window* window = work[workIndex].window.Get()) {
					const f32 hz = window->GetMonitor().refreshRate;
					if (hz > 0.0f && (refreshRate <= 0.0f || hz < refreshRate)) {
						refreshRate = hz;
					}
				}
			}
		}
		{
			SLT_PROFILE_ZONE("Render.Sync");
			const f32 hz = refreshRate > 0.0f ? refreshRate : FramePacingInternal::FALLBACK_REFRESH_RATE;
			const RHI::FrameSync sync = RHI::SyncFrame({state.presentedSwapChains.data(), state.presentedSwapChains.size()}, 1.0 / static_cast<f64>(hz),
													   WindowInternal::GetState().pumpTicks);
			state.lastPresents.displayWaitable = sync.waitable;
			state.lastPresents.displaySynced = sync.synced;
		}

		state.lastDrawCalls = exec.drawCalls;
		state.lastTriangles = exec.triangles;
		state.lastLightsBinned = state.frameLightsBinned;
		state.lastLightsDropped = state.frameLightsDropped;
		state.lastClusterLightsDropped = state.frameClusterLightsDropped;
		state.lastShadowMapsRendered = state.frameShadowMapsRendered;
		state.lastShadowLights = state.frameShadowLights;
		state.lastShadowLightsDropped = state.frameShadowLightsDropped;
		state.lastProceduralDrawn = state.frameProceduralDrawn;
		state.lastProceduralCulled = state.frameProceduralCulled;
		state.lastDrawCallsSaved = state.frameDrawCallsSaved;
		state.lastExtractionLanes = state.frameExtractionLanes;
		state.frameLightsBinned = 0;
		state.frameLightsDropped = 0;
		state.frameClusterLightsDropped = 0;
		state.frameShadowMapsRendered = 0;
		state.frameShadowLights = 0;
		state.frameShadowLightsDropped = 0;
		std::erase_if(state.shadowKeeps, [&state](const RenderState::ShadowKeep& keep) {
			return keep.frame + 1 < state.renderedFrames;
		});
		state.frameProceduralDrawn = 0;
		state.frameProceduralCulled = 0;
		state.frameDrawCallsSaved = 0;
		state.frameExtractionLanes = 0;
	}

	FramePassPlan::FramePassPlan(const GraphicsInternal::FramePassPlanInit& init)
		: _graph(init.graph), _passIndex(init.passIndex), _zoneSite(init.zoneSite) {}

	u32 FramePassPlan::AddStep() {
		PassWork work;
		work.index = _passIndex;
		work.subIndex = _stepCount;
		_graph->AddPass("Render.BeforeFrame", _zoneSite, &PassRecordFramePass, work);
		return _stepCount++;
	}

	void FramePassPlan::Read(RHI::BufferHandle buffer, RHI::ResourceState state) {
		SLT_CORE_ASSERT_MSG(_stepCount > 0, "a frame pass declared a read before its 1st step");
		_graph->Read(_graph->ImportBuffer(buffer, true), state);
	}

	void FramePassPlan::Write(RHI::BufferHandle buffer, RHI::ResourceState state) {
		SLT_CORE_ASSERT_MSG(_stepCount > 0, "a frame pass declared a write before its 1st step");
		_graph->Write(_graph->ImportBuffer(buffer, true), state);
	}

	CameraDepth FramePassPlan::ReadCameraDepth(Handle<Camera> camera) {
		CameraDepth out;
		if (_stepCount == 0) {
			SLT_LOG_WARN("Graphics", "a frame pass read a camera's depth before its 1st step. Nothing declared");
			return out;
		}
		const GraphicsInternal::GraphPass& pass = _graph->GetPass(_graph->GetPassCount() - 1);
		if (pass.accessCount >= GraphicsInternal::MAX_PASS_ACCESSES) {
			SLT_LOG_WARN("Graphics", "a frame pass read a camera's depth after its step's {} slot(s) were declared. Read it before the buffers",
						 GraphicsInternal::MAX_PASS_ACCESSES);
			return out;
		}
		RenderState& state = *_state;
		RHI::TextureHandle image = FallbackTexture(state, RHI::TextureDimension::Texture2D, ShaderTextureFallback::Black);
		if (camera.Get()) {
			RenderState::CameraDepthRecord* record = nullptr;
			for (RenderState::CameraDepthRecord& held : state.cameraDepths) {
				record = held.camera == camera ? &held : record;
			}
			if (!record) {
				record = &state.cameraDepths.emplace_back();
				record->camera = camera;
			}
			record->asked = true;
			if (record->drawn && record->image.IsValid()) {
				image = record->image;
				out.view = record->view;
				out.projection = record->projection;
				out.size = record->size;
			}
		}
		if (!image.IsValid()) {
			return out;
		}
		const GraphResourceId read = _graph->Import(image);
		_graph->Read(read, RHI::ResourceState::ShaderResource);
		out.image.id = read;
		return out;
	}

	ComputeContext::ComputeContext(const GraphicsInternal::ComputeContextInit& init)
		: _exec(init.exec), _pass(init.pass), _step(init.step) {}

	namespace {

		bool DeclaredBuffer(const GraphPass& pass, RHI::BufferHandle buffer, RHI::ResourceState& outState) {
			const GraphicsInternal::RenderGraph& graph = _state->graph;
			for (u16 i = 0; i < pass.accessCount; ++i) {
				if (graph.GetAccessBuffer(pass.firstAccess + i) == buffer) {
					outState = graph.GetAccess(pass.firstAccess + i).state;
					return true;
				}
			}
			return false;
		}

		RHI::TextureHandle DeclaredImage(const GraphPass& pass, FrameImage image) {
			const GraphicsInternal::RenderGraph& graph = _state->graph;
			for (u16 i = 0; i < pass.accessCount; ++i) {
				const GraphicsInternal::GraphAccess& access = graph.GetAccess(pass.firstAccess + i);
				if (access.resource == image.id && !access.write) {
					return graph.GetAccessTexture(pass.firstAccess + i);
				}
			}
			return {};
		}

		RHI::DescriptorType KernelBindingType(const ShaderAsset& kernel, u32 binding) {
			for (const RHI::DescriptorBindingDesc& declared : kernel.bindings) {
				if (declared.set == 0 && declared.binding == binding) {
					return declared.type;
				}
			}
			return RHI::DescriptorType::Count;
		}

		void ResolveKernelTexture(const ShaderAsset& kernel, const ComputeTexture& slot, RHI::TextureHandle& outTexture, RHI::SamplerHandle& outSampler) {
			RenderState& state = *_state;
			const ShaderResourceInfo* declared = nullptr;
			for (const ShaderResourceInfo& resource : kernel.resources) {
				declared = resource.set == 0 && resource.binding == slot.binding ? &resource : declared;
			}
			const RHI::TextureDimension dimension = declared ? declared->dimension : RHI::TextureDimension::Texture2D;
			const TextureAsset* texture = slot.texture.Get();
			if (texture && texture->texture.IsValid()) {
				if (texture->GetDimension() == dimension) {
					outTexture = texture->texture;
					outSampler = TextureSampler(state, *texture);
					outSampler = outSampler.IsValid() ? outSampler : state.repeatSampler;
					return;
				}
				const u64 key = (StringHash(std::string_view(kernel.virtualPath)).GetValue() ^ (static_cast<u64>(slot.binding) << 32)) * 0x100000001B3ull ^
								(texture->assetId.low ^ texture->assetId.high);
				if (std::find(state.dimensionMismatchesWarned.begin(), state.dimensionMismatchesWarned.end(), key) == state.dimensionMismatchesWarned.end()) {
					state.dimensionMismatchesWarned.push_back(key);
					SLT_LOG_WARN("Graphics", "kernel '{}': binding {} samples a {} texture and '{}' is {} (sampling its fallback)", kernel.virtualPath,
								 slot.binding, RHI::TextureDimensionName(dimension), texture->virtualPath, RHI::TextureDimensionName(texture->GetDimension()));
				}
			}
			outTexture = FallbackTexture(state, dimension, kernel.GetTextureFallback(declared ? std::string_view(declared->name) : std::string_view()));
			outSampler = state.repeatSampler;
		}

		bool BindKernel(GraphExecution& exec, const GraphPass& pass, const Handle<ShaderAsset>& kernelHandle,
						std::span<const ComputeBinding> buffers, std::span<const ComputeTexture> textures, std::span<const u8> pushConstants) {
			ShaderAsset* kernel = kernelHandle.Get();
			if (!kernel || !kernel->IsKernel()) {
				SLT_LOG_WARN("Graphics", "a frame pass dispatched '{}' (not a kernel)", kernel ? kernel->virtualPath : str("<null>"));
				return false;
			}
			if (pushConstants.size() > kernel->pushConstantSize) {
				SLT_LOG_WARN("Graphics", "a dispatch of '{}' pushed {} B -> a declared {} B. Dropped",
							 kernel->virtualPath, pushConstants.size(), kernel->pushConstantSize);
				return false;
			}
			for (const ComputeBinding& binding : buffers) {
				RHI::ResourceState state = RHI::ResourceState::Undefined;
				if (!DeclaredBuffer(pass, binding.buffer, state)) {
					SLT_LOG_WARN("Graphics", "a dispatch of '{}' bound a buffer at binding {} its step didn't declare. Dropped",
								 kernel->virtualPath, binding.binding);
					return false;
				}
				if (KernelBindingType(*kernel, binding.binding) != RHI::DescriptorType::StorageBuffer) {
					SLT_LOG_WARN("Graphics", "a dispatch of '{}' filled binding {} with a buffer (the kernel doesn't declare one there). Dropped",
								 kernel->virtualPath, binding.binding);
					return false;
				}
			}
			for (const ComputeTexture& texture : textures) {
				if (texture.image.IsValid()) {
					if (texture.texture.IsValid()) {
						SLT_LOG_WARN("Graphics", "a dispatch of '{}' set both an asset and a frame's image for binding {}. Dropped",
									 kernel->virtualPath, texture.binding);
						return false;
					}
					if (!DeclaredImage(pass, texture.image).IsValid()) {
						SLT_LOG_WARN("Graphics", "a dispatch of '{}' bound an image at binding {} its step didn't declare reading. Dropped",
									 kernel->virtualPath, texture.binding);
						return false;
					}
					if (KernelBindingType(*kernel, texture.binding) != RHI::DescriptorType::SampledTexture) {
						SLT_LOG_WARN("Graphics", "a dispatch of '{}' filled binding {} with an image (the kernel doesn't declare a texture there). Dropped",
									 kernel->virtualPath, texture.binding);
						return false;
					}
					continue;
				}
				if (KernelBindingType(*kernel, texture.binding) != RHI::DescriptorType::SampledTexture ||
					KernelBindingType(*kernel, texture.samplerBinding) != RHI::DescriptorType::Sampler) {
					SLT_LOG_WARN("Graphics", "a dispatch of '{}' filled bindings {} and {} with a texture + its sampler (the kernel doesn't declare those there). Dropped",
								 kernel->virtualPath, texture.binding, texture.samplerBinding);
					return false;
				}
			}
			for (const RHI::DescriptorBindingDesc& declared : kernel->bindings) {
				const bool fillable = declared.type == RHI::DescriptorType::StorageBuffer || declared.type == RHI::DescriptorType::SampledTexture ||
									  declared.type == RHI::DescriptorType::Sampler;
				if (declared.set != 0 || !fillable) {
					SLT_LOG_WARN("Graphics", "'{}' declares set {} binding {} as something other than a set-0 storage buffer/texture/sampler (a dispatch can't fill it)",
								 kernel->virtualPath, declared.set, declared.binding);
					return false;
				}
				bool filled = false;
				for (const ComputeBinding& binding : buffers) {
					filled = filled || binding.binding == declared.binding;
				}
				for (const ComputeTexture& texture : textures) {
					filled = filled || texture.binding == declared.binding || (!texture.image.IsValid() && texture.samplerBinding == declared.binding);
				}
				if (!filled) {
					SLT_LOG_WARN("Graphics", "a dispatch of '{}' left binding {} empty. Dropped", kernel->virtualPath, declared.binding);
					return false;
				}
			}

			RHI::PipelineHandle pipeline = GetOrCreatePipeline(kernel, RHI::Format::Unknown, RHI::Format::Unknown, nullptr, 1,
															   SHADER_BASE_VARIANT, ShaderProgramKind::Compute);
			if (!pipeline.IsValid()) {
				return false;
			}
			RHI::CommandList& cmd = *exec.cmd;
			cmd.SetPipeline(pipeline);
			if (!buffers.empty() || !textures.empty()) {
				RHI::TransientSetWriter writer = exec.device->CreateTransientSet(pipeline, 0);
				for (const ComputeBinding& binding : buffers) {
					writer.WriteStorageBuffer(binding.binding, binding.buffer, 0, RHI::WHOLE_BUFFER);
				}
				for (const ComputeTexture& texture : textures) {
					if (texture.image.IsValid()) {
						writer.WriteTexture(texture.binding, DeclaredImage(pass, texture.image));
						continue;
					}
					RHI::TextureHandle image;
					RHI::SamplerHandle sampler;
					ResolveKernelTexture(*kernel, texture, image, sampler);
					writer.WriteTexture(texture.binding, image);
					writer.WriteSampler(texture.samplerBinding, sampler);
				}
				cmd.BindTransientSet(0, writer.Commit());
			}
			if (!pushConstants.empty()) {
				cmd.PushConstants(pushConstants);
			}
			return true;
		}

	} // anonymous namespace

	bool ComputeContext::Dispatch(const Handle<ShaderAsset>& kernel, std::span<const ComputeBinding> buffers, std::span<const ComputeTexture> textures,
								  std::span<const u8> pushConstants, u32 groupsX, u32 groupsY, u32 groupsZ) {
		if (!_exec || !_pass || !BindKernel(*_exec, *_pass, kernel, buffers, textures, pushConstants)) {
			return false;
		}
		_exec->cmd->Dispatch(groupsX, groupsY, groupsZ);
		return true;
	}

	bool ComputeContext::DispatchIndirect(const Handle<ShaderAsset>& kernel, std::span<const ComputeBinding> buffers, std::span<const ComputeTexture> textures,
										  std::span<const u8> pushConstants, RHI::BufferHandle arguments, u64 offset) {
		if (!_exec || !_pass) {
			return false;
		}
		RHI::ResourceState state = RHI::ResourceState::Undefined;
		if (!DeclaredBuffer(*_pass, arguments, state) || state != RHI::ResourceState::IndirectArgument) {
			SLT_LOG_WARN("Graphics", "an indirect dispatch read arguments its step didn't declare as an IndirectArgument read. Dropped");
			return false;
		}
		if (!BindKernel(*_exec, *_pass, kernel, buffers, textures, pushConstants)) {
			return false;
		}
		_exec->cmd->DispatchIndirect(arguments, offset);
		return true;
	}

	bool ComputeContext::CopyBuffer(RHI::BufferHandle source, RHI::BufferHandle destination, u64 sourceOffset, u64 destinationOffset, u64 size) {
		if (!_exec || !_pass) {
			return false;
		}
		RHI::ResourceState read = RHI::ResourceState::Undefined;
		RHI::ResourceState written = RHI::ResourceState::Undefined;
		if (!DeclaredBuffer(*_pass, source, read) || read != RHI::ResourceState::TransferSrc) {
			SLT_LOG_WARN("Graphics", "a frame pass copied from a buffer its step didn't declare as a TransferSrc read. Dropped");
			return false;
		}
		if (!DeclaredBuffer(*_pass, destination, written) || written != RHI::ResourceState::TransferDst) {
			SLT_LOG_WARN("Graphics", "a frame pass copied into a buffer its step didn't declare as a TransferDst write. Dropped");
			return false;
		}
		if (size == 0) {
			return true;
		}
		_exec->cmd->CopyBufferRegion(source, destination, sourceOffset, destinationOffset, size);
		return true;
	}

	namespace ModuleInternal {

		void CaptureRenderSystemEngineState(EngineStateTable& table) {
			table.renderSystem = _state;
		}

		void BindRenderSystemEngineState(const EngineStateTable& table) {
			_state = static_cast<RenderState*>(table.renderSystem);
		}

	} // namespace ModuleInternal

	namespace GraphicsInternal {

		void RequestWindowCapture(Handle<Window> window, CaptureDelivery deliver) {
			if (!RenderSystem::IsRenderingAvailable() || !window.IsValid()) {
				if (deliver) {
					deliver(false, Vector<u8>{GetMemoryResource(MemTag::Graphics)}, {});
				}
				return;
			}
			RenderState::FrameCapture& capture = _state->frameCaptures.emplace_back();
			capture.window = window;
			capture.requestedFrame = _state->renderedFrames;
			capture.deliver = std::move(deliver);
		}

		void RequestCamerasCapture(std::span<const Handle<Camera>> cameras, Vec2I size, CaptureDelivery deliver) {
			const ViewTargetId target = cameras.empty() ? INVALID_VIEW_TARGET : RenderSystem::CreateViewTarget(size);
			if (target == INVALID_VIEW_TARGET) {
				if (deliver) {
					deliver(false, Vector<u8>{GetMemoryResource(MemTag::Graphics)}, {});
				}
				return;
			}
			RenderSystem::SubmitCameras(target, cameras);
			RenderState::FrameCapture& capture = _state->frameCaptures.emplace_back();
			capture.target = target;
			capture.requestedFrame = _state->renderedFrames;
			capture.deliver = std::move(deliver);
		}

		void RequestSceneCapture(Vec2I size, CaptureDelivery deliver) {
			if (!_state) {
				RequestCamerasCapture({}, size, std::move(deliver));
				return;
			}
			RequestCamerasCapture({_state->cameras.data(), _state->cameras.size()}, size, std::move(deliver));
		}

		bool ToRGBA8(std::span<const u8> texels, RHI::Format format, Vec2I size, Vector<u8>& out) {
			const size_t count = size.x > 0 && size.y > 0 ? static_cast<size_t>(size.x) * static_cast<size_t>(size.y) : 0;
			const bool bgra = format == RHI::Format::BGRA8Unorm || format == RHI::Format::BGRA8SRGB;
			const bool rgba = format == RHI::Format::RGBA8Unorm || format == RHI::Format::RGBA8SRGB;
			if (count == 0 || (!bgra && !rgba) || texels.size() < count * 4) {
				return false;
			}
			out.resize(count * 4);
			for (size_t i = 0; i < count; ++i) {
				const u8* texel = texels.data() + i * 4;
				out[i * 4 + 0] = bgra ? texel[2] : texel[0];
				out[i * 4 + 1] = texel[1];
				out[i * 4 + 2] = bgra ? texel[0] : texel[2];
				out[i * 4 + 3] = 255;
			}
			return true;
		}

	} // namespace GraphicsInternal

	namespace ShaderGlobals {

		namespace {

			void Store(std::string_view name, MaterialAsset::Param value) {
				if (!_state) {
					return;
				}
				value.hash = StringHash(name);
				for (MaterialAsset::Param& existing : _state->globalValues) {
					if (existing.hash == value.hash) {
						existing.kind = value.kind;
						std::memcpy(existing.numbers, value.numbers, sizeof(value.numbers));
						existing.integer = value.integer;
						return;
					}
				}
				value.name = str(name);
				_state->globalValues.push_back(std::move(value));
			}

			MaterialAsset::Param Floats(UniformMemberType kind, f32 x, f32 y = 0.0f, f32 z = 0.0f, f32 w = 0.0f) {
				MaterialAsset::Param value = MaterialAsset::MakeParam(kind);
				value.numbers[0] = x;
				value.numbers[1] = y;
				value.numbers[2] = z;
				value.numbers[3] = w;
				return value;
			}

		} // anonymous namespace

		void SetFloat(std::string_view name, f32 value) {
			Store(name, Floats(UniformMemberType::Float, value));
		}

		void SetVector(std::string_view name, const Vec4& value) {
			Store(name, Floats(UniformMemberType::Vec4, value.x, value.y, value.z, value.w));
		}

		void SetColor(std::string_view name, const Color& value) {
			Store(name, Floats(UniformMemberType::Vec4, value.r, value.g, value.b, value.a));
		}

		void SetInt(std::string_view name, i32 value) {
			MaterialAsset::Param param = MaterialAsset::MakeParam(UniformMemberType::Int);
			param.integer = value;
			Store(name, std::move(param));
		}

		void SetTexture(std::string_view name, Handle<TextureAsset> texture) {
			if (!_state) {
				return;
			}
			const StringHash hash(name);
			for (RenderState::GlobalTexture& existing : _state->globalTextures) {
				if (existing.hash == hash) {
					existing.texture = texture;
					return;
				}
			}
			_state->globalTextures.push_back({hash, texture});
		}

		void Clear(std::string_view name) {
			if (!_state) {
				return;
			}
			const StringHash hash(name);
			std::erase_if(_state->globalValues, [hash](const MaterialAsset::Param& value) { return value.hash == hash; });
			std::erase_if(_state->globalTextures, [hash](const RenderState::GlobalTexture& value) { return value.hash == hash; });
		}

		void ClearAll() {
			if (_state) {
				_state->globalValues.clear();
				_state->globalTextures.clear();
			}
		}

	} // namespace ShaderGlobals

} // namespace Shalltear
