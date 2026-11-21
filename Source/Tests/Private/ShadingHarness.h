#pragma once

#include "TestsPCH.h"

#include "Graphics/EnvironmentIBL.h"
#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/GraphicsProfile.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/PrimitiveMesh.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/ShaderCompileQueue.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Math/HalfFloat.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Window.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ShadingTest {

	using namespace Shalltear;

	inline std::filesystem::path RepoDir(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	inline constexpr f32 VIEW_AMBIENT = 0.03f;

	inline f32 LinearToSRGBChannel(f32 c) {
		c = Math::Clamp(c, 0.0f, 1.0f);
		return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
	}

	inline Vec3 ExpectedTexel(Vec3 linear) {
		return Vec3(LinearToSRGBChannel(linear.x), LinearToSRGBChannel(linear.y), LinearToSRGBChannel(linear.z)) * 255.0f;
	}

	inline Vec3 TonemapNeutral(Vec3 c) {
		constexpr f32 START_COMPRESSION = 0.8f - 0.04f;
		constexpr f32 DESATURATION = 0.15f;
		const f32 x = Math::Min(c.x, Math::Min(c.y, c.z));
		const f32 offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
		c = Vec3(c.x - offset, c.y - offset, c.z - offset);
		const f32 peak = Math::Max(c.x, Math::Max(c.y, c.z));
		if (peak < START_COMPRESSION) {
			return c;
		}
		constexpr f32 D = 1.0f - START_COMPRESSION;
		const f32 newPeak = 1.0f - D * D / (peak + D - START_COMPRESSION);
		c = c * (newPeak / peak);
		const f32 g = 1.0f - 1.0f / (DESATURATION * (peak - newPeak) + 1.0f);
		return c * (1.0f - g) + Vec3(newPeak, newPeak, newPeak) * g;
	}

	inline Vec3 TonemapACES(Vec3 c) {
		auto row = [](f32 a, f32 b, f32 d, Vec3 v) { return a * v.x + b * v.y + d * v.z; };
		Vec3 v(row(0.59719f, 0.35458f, 0.04823f, c), row(0.07600f, 0.90834f, 0.01566f, c),
			   row(0.02840f, 0.13383f, 0.83777f, c));
		auto fit = [](f32 t) {
			return (t * (t + 0.0245786f) - 0.000090537f) / (t * (0.983729f * t + 0.4329510f) + 0.238081f);
		};
		v = Vec3(fit(v.x), fit(v.y), fit(v.z));
		Vec3 out(row(1.60475f, -0.53108f, -0.07367f, v), row(-0.10208f, 1.10813f, -0.00605f, v),
				 row(-0.00327f, -0.07276f, 1.07602f, v));
		return Vec3(Math::Clamp(out.x, 0.0f, 1.0f), Math::Clamp(out.y, 0.0f, 1.0f), Math::Clamp(out.z, 0.0f, 1.0f));
	}

	inline Vec3 TonemapAgx(Vec3 c) {
		constexpr f32 MIN_EV = -12.47393f;
		constexpr f32 MAX_EV = 4.026069f;
		auto row = [](f32 a, f32 b, f32 d, Vec3 v) { return a * v.x + b * v.y + d * v.z; };
		Vec3 v(Math::Max(c.x, 0.0f), Math::Max(c.y, 0.0f), Math::Max(c.z, 0.0f));
		v = Vec3(row(0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f, v),
				 row(0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f, v),
				 row(0.0423756549057051f, 0.0784336f, 0.879142973793104f, v));
		auto encode = [](f32 t) {
			const f32 e = Math::Clamp(std::log2(Math::Max(t, 1e-10f)), MIN_EV, MAX_EV);
			const f32 n = (e - MIN_EV) / (MAX_EV - MIN_EV);
			const f32 n2 = n * n;
			const f32 n4 = n2 * n2;
			return 15.5f * n4 * n2 - 40.14f * n4 * n + 31.96f * n4 - 6.868f * n2 * n + 0.4298f * n2 + 0.1191f * n - 0.00232f;
		};
		v = Vec3(encode(v.x), encode(v.y), encode(v.z));
		v = Vec3(row(1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f, v),
				 row(-0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f, v),
				 row(-0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f, v));
		auto eotf = [](f32 t) { return Math::Clamp(std::pow(Math::Max(t, 0.0f), 2.2f), 0.0f, 1.0f); };
		return Vec3(eotf(v.x), eotf(v.y), eotf(v.z));
	}

	inline Vec3 Tonemap(TonemapCurve curve, Vec3 linear) {
		switch (curve) {
		case TonemapCurve::Neutral:
			return TonemapNeutral(linear);
		case TonemapCurve::ACES:
			return TonemapACES(linear);
		case TonemapCurve::AgX:
			return TonemapAgx(linear);
		default:
			return linear;
		}
	}

	class ShaderDiagnosticSink : public ILogSink {
	public:
		static constexpr const char* SENTINEL = "shader-diagnostic-sentinel";

		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			if (event.message.find(SENTINEL) != str::npos) {
				_armed = true;
				return;
			}
			if (!_armed) {
				return;
			}
			_messages.push_back(str(event.message));
		}
		void Flush() override {}

		bool Armed() const {
			std::lock_guard<std::mutex> lock(_mutex);
			return _armed;
		}

		str Joined() const {
			std::lock_guard<std::mutex> lock(_mutex);
			str all;
			for (const str& message : _messages) {
				all += message;
				all += '\n';
			}
			return all;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
		bool _armed = false;
	};

	inline bool ArmDiagnosticSink(const std::shared_ptr<ShaderDiagnosticSink>& sink) {
		for (int attempt = 0; attempt < 200 && !sink->Armed(); ++attempt) {
			SLT_LOG_INFO("Graphics", "{}", ShaderDiagnosticSink::SENTINEL);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return sink->Armed();
	}

	class MaterialShadingFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		bool _settingsInitialized = false;

		virtual str ProjectSettingsDocument() const { return {}; }

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_shading_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			}
			const str settingsDocument = ProjectSettingsDocument();
			if (!settingsDocument.empty()) {
				SettingsPaths paths;
				paths.project = Path((_root / "ProjectSettings.ini").generic_string());
				FileSystem::WriteTextFile(paths.project, settingsDocument);
				Settings::Initialize(paths);
				_settingsInitialized = true;
			}
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			SceneSystem::Initialize();
		}

		void TearDown() override {
			SceneSystem::Terminate();
			ShaderCompileQueue::Terminate();
			AssetDatabase::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			if (_settingsInitialized) {
				Settings::Terminate();
			}
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WriteProjectFile(const char* name, const str& content) {
			FileSystem::WriteTextFile(Path((_root / name).generic_string()), content);
		}

		bool CompilerAvailable() {
			WriteProjectFile("CompilerProbe.hlsl",
							 "struct VSOutput { float4 positionCS : SV_Position; };\n"
							 "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
							 "\tVSOutput output; output.positionCS = float4(float(vertexId), 0, 0, 1); return output;\n"
							 "}\n"
							 "float4 PSMain(VSOutput input) : SV_Target { return float4(1, 0, 1, 1); }\n");
			ShaderAsset probe;
			return probe.Import(FileSystem::Resolve(Path("project://CompilerProbe.hlsl")));
		}
	};

	inline constexpr f32 LIGHT_YAW_DEG = 210.0f;

	inline constexpr Vec3 Eye{0.0f, 0.0f, 3.0f};

	inline void WriteRadianceHDR(const std::filesystem::path& path, i32 width, i32 height,
								 const std::function<Vec3(f32, f32)>& sample) {
		std::vector<u8> bytes;
		const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) +
								   " +X " + std::to_string(width) + "\n";
		bytes.assign(header.begin(), header.end());
		for (i32 y = 0; y < height; ++y) {
			for (i32 x = 0; x < width; ++x) {
				const Vec3 color = sample((static_cast<f32>(x) + 0.5f) / static_cast<f32>(width),
										  (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height));
				const f32 peak = Math::Max(color.x, Math::Max(color.y, color.z));
				if (peak < 1e-32f) {
					bytes.insert(bytes.end(), {0, 0, 0, 0});
					continue;
				}
				int exponent = 0;
				const f32 mantissa = std::frexp(peak, &exponent);
				const f32 scale = mantissa * 256.0f / peak;
				bytes.push_back(static_cast<u8>(Math::Clamp(color.x * scale, 0.0f, 255.0f)));
				bytes.push_back(static_cast<u8>(Math::Clamp(color.y * scale, 0.0f, 255.0f)));
				bytes.push_back(static_cast<u8>(Math::Clamp(color.z * scale, 0.0f, 255.0f)));
				bytes.push_back(static_cast<u8>(exponent + 128));
			}
		}
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	inline ViewDesc QuadView() {
		ViewDesc desc;
		desc.view = Mat4::LookAt(Eye, Vec3(0.0f, 0.0f, 0.0f));
		desc.projection = Mat4::PerspectiveReverseZ(60.0f, 1.0f, 0.1f, 100.0f);
		desc.cameraPosition = Eye;
		desc.clearColor = Color::Black();
		return desc;
	}

	inline Vec2 EnvBRDFApprox(f32 perceptualRoughness, f32 noV) {
		const Vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
		const Vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
		const Vec4 r = c0 * perceptualRoughness + c1;
		const f32 a004 = Math::Min(r.x * r.x, std::exp2(-9.28f * noV)) * r.x + r.y;
		return Vec2(-1.04f * a004 + r.z, 1.04f * a004 + r.w);
	}

	inline constexpr f32 MIN_PERCEPTUAL_ROUGHNESS = 0.045f;

	inline Vec3 CenterTexel(const Vector<u8>& pixels, Vec2I size) {
		size_t index = (static_cast<size_t>(size.y / 2) * size.x + size.x / 2) * 4;
		return Vec3(static_cast<f32>(pixels[index]), static_cast<f32>(pixels[index + 1]), static_cast<f32>(pixels[index + 2]));
	}

	inline Vec3 RadianceTexel(const Vector<u8>& pixels, Vec2I size, i32 x, i32 y) {
		const u16* texels = reinterpret_cast<const u16*>(pixels.data());
		const size_t index = (static_cast<size_t>(y) * size.x + x) * 4;
		return Vec3(Math::HalfToFloat(texels[index]), Math::HalfToFloat(texels[index + 1]), Math::HalfToFloat(texels[index + 2]));
	}

	inline GraphicsInternal::IrradianceSH CapturedIrradiance(const std::function<Vec3(const Vec3&)>& sky, u32 faceSize = 128) {
		GraphicsInternal::RadianceCube cube;
		cube.faceSize = faceSize;
		cube.mipCount = GraphicsInternal::RadianceCubeMipCount(faceSize);
		cube.pixels.resize(static_cast<size_t>(GraphicsInternal::RadianceCubeBytes(faceSize, cube.mipCount)));
		for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
			u16* texels = reinterpret_cast<u16*>(cube.pixels.data() + GraphicsInternal::RadianceCubeFaceOffset(faceSize, cube.mipCount, 0, face));
			for (u32 y = 0; y < faceSize; ++y) {
				for (u32 x = 0; x < faceSize; ++x) {
					const Vec3 color = sky(GraphicsInternal::CubeTexelDirection(face, (static_cast<f32>(x) + 0.5f) / static_cast<f32>(faceSize),
																				(static_cast<f32>(y) + 0.5f) / static_cast<f32>(faceSize)));
					u16* texel = texels + (static_cast<size_t>(y) * faceSize + x) * 4;
					texel[0] = Math::FloatToHalf(color.x);
					texel[1] = Math::FloatToHalf(color.y);
					texel[2] = Math::FloatToHalf(color.z);
					texel[3] = Math::FloatToHalf(1.0f);
				}
			}
		}
		GraphicsInternal::IrradianceSH sh;
		GraphicsInternal::PrefilterRadianceCube(cube, sh);
		return sh;
	}

	inline Vec3 SkyPixelRay(const ViewDesc& desc, Vec2I size, i32 x, i32 y) {
		const f32 ndcX = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size.x) * 2.0f - 1.0f;
		const f32 ndcY = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size.y) * 2.0f;
		const Mat4 inverse = (desc.projection * desc.view).Inverse();
		const Vec4 farPoint = inverse * Vec4(ndcX, ndcY, 0.0f, 1.0f);
		const Vec3 point(farPoint.x / farPoint.w, farPoint.y / farPoint.w, farPoint.z / farPoint.w);
		return (point - desc.cameraPosition).Normalized();
	}

	inline constexpr const char* MIRROR_BODY = "\t\"floats\": { \"metallic\": 1, \"roughness\": 0 },\n"
											   "\t\"colors\": { \"baseColor\": [1, 1, 1, 1] }";

	inline Vec3 MirrorOf(const Vec3& light) {
		const Vec2 dfg = EnvBRDFApprox(MIN_PERCEPTUAL_ROUGHNESS, 1.0f);
		return light * (dfg.x + dfg.y);
	}

	struct ShadingStage {
		Handle<Scene> scene;
		Handle<SceneObject> quad;
		Handle<Light> sun;
		ViewTargetId target = INVALID_VIEW_TARGET;
		Vec3 lightDirection{};
		Vec3 lightColor{};
	};

	class MaterialReadbackFixture : public MaterialShadingFixture {
	protected:
		MaterialAsset* LoadMaterial(const char* fileName, const Guid& shader, const str& jsonBody) {
			str document = "{\n\t\"shader\": \"" + shader.ToString() + "\"";
			if (!jsonBody.empty()) {
				document += ",\n" + jsonBody;
			}
			document += "\n}\n";
			WriteProjectFile(fileName, document);
			Asset* asset = AssetDatabase::LoadAsset(Path(str("project://") + fileName));
			return static_cast<MaterialAsset*>(asset);
		}

		bool BuildStage(MaterialAsset* material, ShadingStage& out) {
			Asset* quadMesh = AssetDatabase::LoadAsset(Path(GetPrimitiveMeshPath(PrimitiveShape::Quad)));
			if (!quadMesh || !material) {
				return false;
			}
			out.scene = SceneSystem::CreateScene("Shading");
			out.quad = SceneSystem::CreateObject("Quad", out.scene);
			Handle<MeshRenderer> renderer = out.quad->AddComponent<MeshRenderer>();
			renderer->SetMesh(Handle<MeshAsset>(static_cast<MeshAsset*>(quadMesh)));
			renderer->SetMaterial(0, Handle<MaterialAsset>(material));

			Handle<SceneObject> lightObject = SceneSystem::CreateObject("Sun", out.scene);
			lightObject->SetLocalRotation(Quat::AngleAxis(LIGHT_YAW_DEG, Vec3::Up()));
			Handle<Light> light = lightObject->AddComponent<Light>();
			light->SetLightType(LightType::Directional);
			light->SetColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
			light->SetIntensity(1.0f);
			out.sun = light;
			SceneSystem::EndFrame();

			out.lightDirection = -lightObject->TransformDirection(Vec3::Forward()).Normalized();
			out.lightColor = Vec3(1.0f, 1.0f, 1.0f);

			out.target = RenderSystem::CreateViewTarget({64, 64});
			return out.target != INVALID_VIEW_TARGET;
		}

		bool BuildSkyStage(MaterialAsset* sky, ShadingStage& out, Handle<Environment>& environment) {
			if (!sky) {
				return false;
			}
			out.scene = SceneSystem::CreateScene("Sky");
			Handle<SceneObject> lightObject = SceneSystem::CreateObject("Sun", out.scene);
			lightObject->SetLocalRotation(Quat::FromEuler(Vec3(35.0f, 120.0f, 0.0f)));
			Handle<Light> light = lightObject->AddComponent<Light>();
			light->SetLightType(LightType::Directional);
			light->SetColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
			light->SetIntensity(1.0f);
			out.sun = light;

			Handle<SceneObject> environmentObject = SceneSystem::CreateObject("Environment", out.scene);
			environment = environmentObject->AddComponent<Environment>();
			environment->SetSkyMode(SkyMode::Material);
			environment->SetSkyMaterial(Handle<MaterialAsset>(sky));
			SceneSystem::EndFrame();

			out.lightDirection = -lightObject->TransformDirection(Vec3::Forward()).Normalized();
			out.target = RenderSystem::CreateViewTarget({64, 64});
			return out.target != INVALID_VIEW_TARGET;
		}

		bool RenderStage(const ShadingStage& stage, Vector<u8>& pixels, Vec2I& size, Color clear = Color::Black()) {
			ViewDesc desc = QuadView();
			desc.clearColor = clear;
			RenderSystem::SubmitView(stage.target, desc);
			RenderSystem::RenderAll();
			return RenderSystem::ReadbackViewTarget(stage.target, pixels, size);
		}

		bool AddQuad(ShadingStage& stage, MaterialAsset* material, f32 z) {
			Asset* quadMesh = AssetDatabase::LoadAsset(Path(GetPrimitiveMeshPath(PrimitiveShape::Quad)));
			if (!quadMesh || !material || !stage.scene.IsValid()) {
				return false;
			}
			Handle<SceneObject> object = SceneSystem::CreateObject("Quad", stage.scene);
			object->SetLocalPosition(Vec3(0.0f, 0.0f, z));
			Handle<MeshRenderer> renderer = object->AddComponent<MeshRenderer>();
			renderer->SetMesh(Handle<MeshAsset>(static_cast<MeshAsset*>(quadMesh)));
			renderer->SetMaterial(0, Handle<MaterialAsset>(material));
			SceneSystem::EndFrame();
			return true;
		}

		void TearDownStage(ShadingStage& stage) {
			if (stage.target != INVALID_VIEW_TARGET) {
				RenderSystem::DestroyViewTarget(stage.target);
			}
			if (stage.scene.IsValid()) {
				SceneSystem::UnloadScene(stage.scene);
				SceneSystem::EndFrame();
			}
		}
	};

} // namespace ShadingTest
