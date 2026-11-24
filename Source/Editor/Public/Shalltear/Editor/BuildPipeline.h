#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorAPI.h"

#include <functional>
#include <string_view>

namespace Shalltear {

	enum class BuildPhase : u8 {
		Validate,
		PreBuild,
		Compile,
		Stage,
		PostBuild
	};

	enum class BuildStepStatus : u8 {
		Running,
		Succeeded,
		Failed
	};

	class BuildContext;

	using BuildStepFn = std::function<BuildStepStatus(BuildContext& context)>;

	struct BuildStepDesc {
		str name;
		BuildPhase phase = BuildPhase::Compile;
		const void* owner = nullptr;
		BuildStepFn fn;
	};

	class SHALLTEAR_EDITOR_API BuildStepRegistry {
	public:
		static void RegisterBuiltIns();
		static void Shutdown();

		static bool Register(std::string_view name, BuildPhase phase, BuildStepFn fn, const void* owner = nullptr);

		static bool Unregister(std::string_view name);

		static u32 UnregisterOwnedBy(const void* owner);

		static void GetSteps(Vector<BuildStepDesc>& out);
	};

	class SHALLTEAR_EDITOR_API BuildContext {
	public:
		const ResolvedProfile* profile = nullptr;
		const IBuildTarget* target = nullptr;

		str projectRoot; // physical, normalized
		str stagingDir;
		str intermediateDir;

		str sdkRoot;

		str entrySource;

		bool windowedSubsystem = true;

		str executableName;
		str executableSuffix = ".exe";
		RHI::ShaderBytecodeFormat shaderFormat = RHI::ShaderBytecodeFormat::Spirv;

		Vector<str> defines;
		Vector<str> extraConfigureArgs;

		str configuration;

		u32 buildNumber = 0;
		i64 buildTimeUnix = 0;
		str buildTimeIso;
		str version;

		str bootManifest;

		// staged beside executable, found beside SDK/editor
		Vector<str> runtimeFiles;

		// appended to embedded third-party notices in single-file builds
		Vector<str> extraThirdPartyNotices;

		// WebAssembly publish folder whose managed assemblies are packed into Content.slpak
		str managedAssembliesDir;

		Vector<u32> executableIconSizes;
		Vector<u32> windowIconSizes;
		str executableIconFile;
		str windowIconDocument;

		void Log(std::string_view message) const;

		void AddIssue(BuildIssueSeverity severity, std::string_view key, std::string_view message);
		const Vector<BuildIssue>& GetIssues() const { return _issues; }

		bool IsCancelRequested() const;

		bool StartCommand(const str& commandLine, const str& workingDirectory);
		bool HasCommand() const;

		bool TryFinishCommand(i32& outExitCode);
	private:
		friend class BuildPipeline;

		class BuildRunner* _runner = nullptr;
		Vector<BuildIssue> _issues;
	};

	struct BuildRequest {
		str projectPath;
		Vector<str> profiles;

		str outputOverride;
	};

	enum class BuildOutcome : u8 {
		Succeeded,
		Failed,
		Skipped,
		Cancelled
	};

	struct BuildProfileResult {
		str profile;
		str targetId;
		BuildOutcome outcome = BuildOutcome::Failed;

		str reason;

		str outputPath;

		str launchCommand;

		u32 buildNumber = 0;
		u32 errorCount = 0;
		u32 warningCount = 0;
		f64 durationSeconds = 0.0;
		u64 artifactBytes = 0;

		Vector<BuildIssue> issues;
	};

	struct BuildResult {
		Vector<BuildProfileResult> profiles;

		bool AllSucceeded() const {
			for (const BuildProfileResult& entry : profiles) {
				if (entry.outcome == BuildOutcome::Failed || entry.outcome == BuildOutcome::Cancelled) {
					return false;
				}
			}
			return true;
		}

		u32 FailureCount() const {
			u32 count = 0;
			for (const BuildProfileResult& entry : profiles) {
				if (entry.outcome == BuildOutcome::Failed || entry.outcome == BuildOutcome::Cancelled) {
					++count;
				}
			}
			return count;
		}
	};

	enum class BuildState : u8 {
		Idle = 0,
		Running,
		Succeeded,
		Failed,
		Cancelled
	};

	class SHALLTEAR_EDITOR_API BuildPipeline {
	public:
		BuildPipeline();
		~BuildPipeline();

		BuildPipeline(const BuildPipeline&) = delete;
		BuildPipeline& operator=(const BuildPipeline&) = delete;

		static BuildPipeline* GetActive();

		bool Request(const BuildRequest& request, str& outRefusal);

		void Cancel();

		void PumpMainThread();

		BuildState GetState() const { return _state; }
		bool IsBusy() const { return _state == BuildState::Running; }

		str GetStatusLabel() const;
		u32 GetStepIndex() const { return _stepIndex; }
		u32 GetStepCount() const { return _stepCount; }
		const str& GetStepName() const { return _stepName; }
		const str& GetCurrentProfileName() const { return _currentProfile; }

		const BuildResult& GetResult() const { return _result; }

		const Vector<CompilerDiagnostic>& GetDiagnostics() const { return _diagnostics; }

		static str GetStagingDirectory(const str& projectPath, const ResolvedProfile& resolved,
									   const str& outputOverride);

		static str GetIntermediateDirectory(const str& projectPath, const str& profileName);

		static str GetCookReportPath(const str& projectPath, const str& profileName);

		static void GetStagedScenes(const ResolvedProfile& resolved, Vector<str>& out);

		static bool IsEditorOnlyEngineAsset(std::string_view engineRelativePath);
	private:
		void BeginNextProfile();
		void PublishStep();
		void FinishSkipped(const str& name, const str& targetId, const str& reason);
		void FinishProfile(BuildOutcome outcome, const str& reason);
		void FinishRequest();
		void WriteLastResult(const BuildProfileResult& entry) const;

		class BuildRunner* _runner = nullptr;

		BuildState _state = BuildState::Idle;
		u32 _stepIndex = 0;
		u32 _stepCount = 0;
		str _stepName;
		str _currentProfile;

		BuildResult _result;
		Vector<CompilerDiagnostic> _diagnostics{GetMemoryResource(MemTag::Editor)};
	};

} // namespace Shalltear
