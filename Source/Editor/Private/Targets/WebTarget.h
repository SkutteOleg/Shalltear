#pragma once

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/EditorAPI.h"

namespace Shalltear {

	class SHALLTEAR_EDITOR_API WebTarget : public IBuildTarget {
	public:
		static constexpr std::string_view Triple = "wasm32-unknown-emscripten";

		std::string_view GetId() const override { return "Web"; }
		std::string_view GetDisplayName() const override { return "Web (WebAssembly + WebGPU)"; }

		bool IsAvailable(str& outReason) const override;
		void DescribeSettings(Vector<BuildSettingDesc>& out) const override;
		void Validate(const str& projectPath, const ResolvedProfile& profile, Vector<BuildIssue>& out) const override;
		void ConfigurePipeline(BuildContext& context) const override;
		str GetLaunchCommand(const BuildContext& context) const override;

		static BuildStepStatus StageStep(BuildContext& context);
		static bool RenderIndexHTML(const str& title, const str& jsFile, const str& wasmFile,
									bool enableThreads, str& outHTML, bool hasManaged = false);
	};

} // namespace Shalltear
