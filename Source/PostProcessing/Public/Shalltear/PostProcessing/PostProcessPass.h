#pragma once

#include "Shalltear/Core/Graphics/RenderPass.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/PostProcessing/PostProcessingAPI.h"

namespace Shalltear {

	class SHALLTEAR_POSTPROCESSING_API PostProcessPass : public RenderPass {
	public:
		SLT_REFLECT_CLASS(PostProcessPass)
		SLT_REFLECT_PARENT(RenderPass)
		SLT_REFLECT_CREATABLE()
		SLT_END_REFLECT()

		PostProcessPass();
		~PostProcessPass() override;

		PassRequests GetRequests() const override { return PassRequests::SceneColor; }

		bool PrefersResolvedTarget() const override { return true; }

		bool WillWriteSceneTarget() const override;

		void DeclarePlan(PassPlan& plan) override;

		void Record(PassContext& context) override;

		bool Cook(CookWriter& descriptor, CookWriter& payload) const override;
	private:
		ShaderAsset* EnsureShader();

		Handle<ShaderAsset> _shader;
		bool _shaderResolved = false;

		bool _warnedNoShader = false;

		struct Resolved;
		const Resolved& ResolveFor(const Vec3& cameraPosition, bool refresh);
		UniquePtr<Resolved> _resolved;
	};

} // namespace Shalltear
