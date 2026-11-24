#pragma once

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/EnumFlags.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Matrix.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

namespace Shalltear {

	class MaterialAsset;
	class ShaderAsset;
	namespace RHI {
		class CommandList;
	} // namespace RHI
	namespace GraphicsInternal {
		struct GraphExecution;
		struct PassContextInit;
	} // namespace GraphicsInternal

	// declaration order = order, values persist in rows
#define SLT_PASS_POINT_LIST(X) \
	X(BeforeFrame)             \
	X(Skinning)                \
	X(BeforeShadows)           \
	X(Shadows)                 \
	X(AfterShadows)            \
	X(BeforePrepass)           \
	X(DepthPrepass)            \
	X(AfterPrepass)            \
	X(BeforeOpaque)            \
	X(Opaque)                  \
	X(AfterOpaque)             \
	X(Sky)                     \
	X(BeforeTransparent)       \
	X(Transparent)             \
	X(AfterTransparent)        \
	X(BeforePost)              \
	X(Post)                    \
	X(AfterPost)               \
	X(Tonemap)                 \
	X(Overlay)                 \
	X(Present)

	enum class PassPoint : u8 {
#define SLT_DETAIL_PASS_POINT_ENUMERATOR(Name) Name,
		SLT_PASS_POINT_LIST(SLT_DETAIL_PASS_POINT_ENUMERATOR)
#undef SLT_DETAIL_PASS_POINT_ENUMERATOR
	};

	inline constexpr u32 PASS_POINT_COUNT = 21;

	inline constexpr std::string_view GetPassPointName(PassPoint point) {
		switch (point) {
#define SLT_DETAIL_PASS_POINT_NAME(Name) \
	case PassPoint::Name:                \
		return #Name;
			SLT_PASS_POINT_LIST(SLT_DETAIL_PASS_POINT_NAME)
#undef SLT_DETAIL_PASS_POINT_NAME
		}
		return {};
	}

	inline constexpr bool IsPerViewPassPoint(PassPoint point) {
		return point != PassPoint::BeforeFrame && point != PassPoint::Skinning;
	}

	enum class PassRequests : u8 {
		None = 0,
		SceneColor = 1 << 0,
		SceneDepth = 1 << 1,
		SceneNormals = 1 << 2
	};
	SLT_ENUM_CLASS_FLAGS(PassRequests)

	struct PassTarget {
		RHI::Format format = RHI::Format::RGBA16Float;
		u16 divisor = 1;
	};

	struct PassVolume {
		RHI::Format format = RHI::Format::RGBA8Unorm;
		Vec3I size{};
		std::span<const u8> contents;
		u64 revision = 0;
	};

	struct PassStep {
		u8 target = 0xFF;
		u8 reads[2] = {0xFF, 0xFF};
	};

	inline constexpr u8 PASS_SCENE_TARGET = 0xFF;
	inline constexpr u8 PASS_NO_READ = 0xFF;

	enum class PassProduces : u8 {
		None = 0,
		// red channel, 1 = unoccluded
		AmbientOcclusion = 1 << 0
	};
	SLT_ENUM_CLASS_FLAGS(PassProduces)

	class SHALLTEAR_CORE_API PassPlan {
	public:
		static constexpr u32 MAX_TARGETS = 24;
		static constexpr u32 MAX_STEPS = 48;

		PassPlan(Vec2I extent, const Vec3& cameraPosition)
			: _extent(extent), _cameraPosition(cameraPosition) {}

		Vec2I GetExtent() const { return _extent; }

		const Vec3& GetCameraPosition() const { return _cameraPosition; }

		u8 AddTarget(RHI::Format format, u16 divisor);

		bool AddStep(u8 target, u8 readA = PASS_NO_READ, u8 readB = PASS_NO_READ);

		bool Produce(PassProduces kind, u8 target);

		bool DeclareVolume(RHI::Format format, Vec3I size, std::span<const u8> contents, u64 revision);

		PassProduces GetProduced() const { return _produced; }
		u8 GetProducedTarget(PassProduces kind) const;

		bool HasVolume() const { return _hasVolume; }
		const PassVolume& GetVolume() const { return _volume; }

		u32 GetTargetCount() const { return _targetCount; }
		const PassTarget& GetTarget(u32 index) const { return _targets[index]; }
		u32 GetStepCount() const { return _stepCount; }
		const PassStep& GetStep(u32 index) const { return _steps[index]; }
	private:
		Vec2I _extent{1, 1};
		Vec3 _cameraPosition{};
		u32 _targetCount = 0;
		u32 _stepCount = 0;
		u32 _written = 0;
		PassProduces _produced = PassProduces::None;
		u8 _producedTargets[8]{PASS_NO_READ, PASS_NO_READ, PASS_NO_READ, PASS_NO_READ,
							   PASS_NO_READ, PASS_NO_READ, PASS_NO_READ, PASS_NO_READ};
		bool _hasVolume = false;
		PassVolume _volume{};
		PassTarget _targets[MAX_TARGETS]{};
		PassStep _steps[MAX_STEPS]{};
	};

} // namespace Shalltear

SLT_REFLECT_ENUM(::Shalltear::PassPoint)
SLT_PASS_POINT_LIST(SLT_ENUM_VALUE)
SLT_END_ENUM()

SLT_REFLECT_ENUM(::Shalltear::FrameStagePosition)
SLT_ENUM_VALUE(Early)
SLT_ENUM_VALUE(Normal)
SLT_ENUM_VALUE(Late)
SLT_END_ENUM()

namespace Shalltear {

	class SHALLTEAR_CORE_API PassContext {
	public:
		explicit PassContext(const GraphicsInternal::PassContextInit& init);

		bool DrawProcedural(const Handle<ShaderAsset>& shader, std::span<const u8> pushConstants, u32 vertexCount = 3, u32 instanceCount = 1);

		bool DrawProcedural(const Handle<MaterialAsset>& material, std::span<const u8> pushConstants, u32 vertexCount = 3, u32 instanceCount = 1);

		bool DrawObjects(u32 renderingLayerMask = ~0u, const Handle<MaterialAsset>& overrideMaterial = {});

		bool DrawObjects(u32 renderingLayerMask, const Handle<MaterialAsset>& overrideMaterial, const Mat4& projection);

		bool DrawFullScreen(const Handle<MaterialAsset>& material);

		bool DrawFullScreen(const Handle<ShaderAsset>& shader, std::span<const u8> pushConstants = {});

		const Mat4& GetView() const { return _view; }
		const Mat4& GetProjection() const { return _projection; }
		const Vec3& GetCameraPosition() const { return _cameraPosition; }

		Vec2I GetExtent() const { return _extent; }

		u32 GetSampleCount() const { return _samples; }

		u32 GetStep() const { return _step; }

		u32 GetStepCount() const { return _stepCount; }
	private:
		GraphicsInternal::GraphExecution* _exec = nullptr;
		RHI::TextureHandle _color;
		RHI::TextureHandle _depth;
		RHI::Format _colorFormat = RHI::Format::Unknown;
		RHI::Format _depthFormat = RHI::Format::Unknown;
		u32 _samples = 1;
		Vec2I _extent{1, 1};
		Mat4 _view = Mat4::Identity();
		Mat4 _projection = Mat4::Identity();
		Vec3 _cameraPosition{};
		u32 _scopeIndex = 0;
		PassRequests _requests = PassRequests::None;
		RHI::TextureHandle _sceneColor;
		RHI::TextureHandle _sceneDepth;
		RHI::TextureHandle _sceneNormals;
		u32 _step = 0;
		u32 _stepCount = 1;
		RHI::TextureHandle _passInput0;
		RHI::TextureHandle _passInput1;
		RHI::TextureHandle _passVolume;
		bool _writesSceneTarget = true;
		bool _colorWritten = false;
	};

	class SHALLTEAR_CORE_API RenderPass : public Asset {
	public:
		SLT_REFLECT_CLASS(RenderPass)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_FIELD(point)
		SLT_REFLECT_FIELD(position)
		SLT_END_REFLECT()

		~RenderPass() override;

		virtual void Record(PassContext& context) = 0;

		virtual PassRequests GetRequests() const { return PassRequests::None; }

		virtual void DeclarePlan(PassPlan& plan) { (void)plan; }

		// single-sample post passes that read resolved SceneColor and do not use scene depth
		virtual bool PrefersResolvedTarget() const { return false; }

		// called after DeclarePlan to check whether this pass will actually overwrite the scene target
		virtual bool WillWriteSceneTarget() const { return true; }

		StringHash GetAssetTypeId() const override { return GetType()->name; }

		bool Import(const Path& sourcePath) override;
		bool Export(const Path& destPath) const override;

		PassPoint point = PassPoint::AfterOpaque;

		FrameStagePosition position = FrameStagePosition::Normal;
	};

	class SHALLTEAR_CORE_API RenderPassImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("RenderPass"); }
		Asset* Import(const Path& sourcePath) override;
	};

} // namespace Shalltear
