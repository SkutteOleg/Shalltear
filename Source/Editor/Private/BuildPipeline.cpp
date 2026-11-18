#include "Shalltear/Editor/BuildPipeline.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "CompilerOutputSink.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <system_error>
#include <vector>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* CONSOLE_CATEGORY = "Build";

		char ToLowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}

		bool PathsEqualIgnoringCase(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
					return false;
				}
			}
			return true;
		}

		bool StartsWithIgnoringCase(std::string_view value, std::string_view prefix) {
			return value.size() >= prefix.size() && PathsEqualIgnoringCase(value.substr(0, prefix.size()), prefix);
		}

		u64 MeasureDirectory(const str& directory) {
			std::error_code ec;
			u64 total = 0;
			for (fs::recursive_directory_iterator it(fs::path{directory}, ec), end; !ec && it != end;
				 it.increment(ec)) {
				std::error_code fileEc;
				if (it->is_regular_file(fileEc) && !fileEc) {
					total += static_cast<u64>(it->file_size(fileEc));
				}
			}
			return total;
		}

		struct StepEntry {
			BuildStepDesc desc;
			bool hasShadow = false;
			BuildPhase shadowPhase = BuildPhase::Compile;
			BuildStepFn shadowFn;
		};

		struct StepState {
			std::vector<StepEntry> steps;
		};

		StepState* _steps = nullptr;

		StepState& Steps() {
			if (!_steps) {
				_steps = New<StepState>(MemTag::Editor);
				BuildStepRegistry::RegisterBuiltIns();
			}
			return *_steps;
		}

	} // anonymous namespace

	void BuildStepRegistry::Shutdown() {
		if (_steps) {
			Delete(_steps);
			_steps = nullptr;
		}
	}

	bool BuildStepRegistry::Register(std::string_view name, BuildPhase phase, BuildStepFn fn, const void* owner) {
		if (name.empty() || !fn) {
			SLT_LOG_WARN("Build", "BuildStepRegistry::Register: ignoring a step with no name or no function");
			return false;
		}
		StepState& state = _steps ? *_steps : Steps();

		for (StepEntry& entry : state.steps) {
			if (entry.desc.name == name) {
				if (owner && !entry.desc.owner && !entry.hasShadow) {
					entry.hasShadow = true;
					entry.shadowPhase = entry.desc.phase;
					entry.shadowFn = entry.desc.fn;
				} else if (!owner) {
					entry.hasShadow = false;
					entry.shadowFn = nullptr;
				}
				entry.desc.phase = phase;
				entry.desc.owner = owner;
				entry.desc.fn = std::move(fn);
				return true;
			}
		}
		state.steps.push_back(StepEntry{BuildStepDesc{str(name), phase, owner, std::move(fn)}});
		return true;
	}

	bool BuildStepRegistry::Unregister(std::string_view name) {
		if (!_steps) {
			return false;
		}
		for (size_t i = 0; i < _steps->steps.size(); ++i) {
			if (_steps->steps[i].desc.name == name) {
				_steps->steps.erase(_steps->steps.begin() + static_cast<ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	u32 BuildStepRegistry::UnregisterOwnedBy(const void* owner) {
		if (!_steps || !owner) {
			return 0;
		}
		u32 removed = 0;
		for (size_t i = _steps->steps.size(); i > 0; --i) {
			StepEntry& entry = _steps->steps[i - 1];
			if (entry.desc.owner != owner) {
				continue;
			}
			++removed;
			if (entry.hasShadow) {
				entry.desc.phase = entry.shadowPhase;
				entry.desc.owner = nullptr;
				entry.desc.fn = std::move(entry.shadowFn);
				entry.hasShadow = false;
				entry.shadowFn = nullptr;
				continue;
			}
			_steps->steps.erase(_steps->steps.begin() + static_cast<ptrdiff_t>(i - 1));
		}
		return removed;
	}

	void BuildStepRegistry::GetSteps(Vector<BuildStepDesc>& out) {
		out.clear();
		StepState& state = Steps();

		std::vector<const BuildStepDesc*> ordered;
		ordered.reserve(state.steps.size());
		for (const StepEntry& entry : state.steps) {
			ordered.push_back(&entry.desc);
		}
		std::stable_sort(ordered.begin(), ordered.end(), [](const BuildStepDesc* a, const BuildStepDesc* b) {
			return static_cast<u8>(a->phase) < static_cast<u8>(b->phase);
		});
		for (const BuildStepDesc* step : ordered) {
			out.push_back(*step);
		}
	}

	class BuildRunner {
	public:
		str projectPath;
		Vector<str> profiles{GetMemoryResource(MemTag::Editor)};
		str outputOverride;
		size_t nextProfile = 0;

		bool profileActive = false;
		ResolvedProfile resolved;
		BuildContext context;
		Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Editor)};
		size_t stepIndex = 0;
		u64 profileStartTicks = 0;
		u32 errorCount = 0;
		u32 warningCount = 0;

		ChildProcess process;
		bool commandStarted = false;
		bool cancelRequested = false;

		CompilerOutputSink sink;

		explicit BuildRunner(Vector<CompilerDiagnostic>& diagnostics)
			: sink(CONSOLE_CATEGORY, diagnostics, errorCount, warningCount) {}

		void ConsumeOutput() { sink.Consume(process); }

		void EndCommand() {
			if (commandStarted) {
				process.Kill();
				ConsumeOutput();
				commandStarted = false;
			}
		}
	};

	void BuildContext::Log(std::string_view message) const {
		SLT_LOG_INFO(CONSOLE_CATEGORY, "{}", message);
	}

	void BuildContext::AddIssue(BuildIssueSeverity severity, std::string_view key, std::string_view message) {
		_issues.push_back(BuildIssue{severity, profile ? profile->name : str{}, str(key), str(message)});

		std::string_view subject = !key.empty() ? key : profile ? std::string_view(profile->name)
																: std::string_view();
		if (severity == BuildIssueSeverity::Error) {
			SLT_LOG_ERROR(CONSOLE_CATEGORY, "{}: {}", subject, message);
			if (_runner) {
				++_runner->errorCount;
			}
		} else {
			SLT_LOG_WARN(CONSOLE_CATEGORY, "{}: {}", subject, message);
			if (_runner) {
				++_runner->warningCount;
			}
		}
	}

	bool BuildContext::IsCancelRequested() const {
		return _runner && _runner->cancelRequested;
	}

	bool BuildContext::StartCommand(const str& commandLine, const str& workingDirectory) {
		if (!_runner || _runner->commandStarted) {
			return false;
		}
		if (!_runner->process.Start(commandLine, workingDirectory)) {
			SLT_LOG_ERROR(CONSOLE_CATEGORY, "couldn't start: {}", commandLine);
			return false;
		}
		_runner->commandStarted = true;
		return true;
	}

	bool BuildContext::HasCommand() const {
		return _runner && _runner->commandStarted;
	}

	bool BuildContext::TryFinishCommand(i32& outExitCode) {
		if (!_runner || !_runner->commandStarted) {
			return false;
		}
		_runner->ConsumeOutput();
		if (!_runner->process.TryFinish(outExitCode)) {
			return false;
		}
		_runner->ConsumeOutput();
		return true;
	}

	str BuildPipeline::GetStagingDirectory(const str& projectPath, const ResolvedProfile& resolved,
										   const str& outputOverride) {
		str base = outputOverride;
		if (base.empty()) {
			const ResolvedSetting* setting = resolved.Find("outputPath");
			base = setting ? setting->value : str{};
		}
		if (base.empty()) {
			base = "Builds";
		}

		str root = Path(base).IsAbsolute() ? base : projectPath + "/" + base;
		return root + "/" + resolved.name;
	}

	str BuildPipeline::GetIntermediateDirectory(const str& projectPath, const str& profileName) {
		return projectPath + "/Library/Player/" + profileName;
	}

	str BuildPipeline::GetCookReportPath(const str& projectPath, const str& profileName) {
		return GetIntermediateDirectory(projectPath, profileName) + "/CookReport.json";
	}

	void BuildPipeline::GetStagedScenes(const ResolvedProfile& resolved, Vector<str>& out) {
		out.clear();
		const ResolvedSetting* scenes = resolved.Find("scenes");
		if (!scenes) {
			return;
		}
		for (const str& scene : scenes->items) {
			if (!scene.empty()) {
				out.push_back(scene);
			}
		}
	}

	bool BuildPipeline::IsEditorOnlyEngineAsset(std::string_view engineRelativePath) {
		static constexpr std::string_view Prefixes[] = {
			"Templates/",
			"Schemas/"};
		static constexpr std::string_view Files[] = {
			"Shaders/EditorGrid.hlsl",
			"Shaders/EditorGrid.hlsl.meta"};

		for (std::string_view prefix : Prefixes) {
			if (StartsWithIgnoringCase(engineRelativePath, prefix)) {
				return true;
			}
		}
		for (std::string_view file : Files) {
			if (PathsEqualIgnoringCase(engineRelativePath, file)) {
				return true;
			}
		}
		return false;
	}

	namespace {
		BuildPipeline* _activeBuildPipeline = nullptr;
	} // namespace

	BuildPipeline* BuildPipeline::GetActive() {
		return _activeBuildPipeline;
	}

	BuildPipeline::BuildPipeline() {
		if (!_activeBuildPipeline) {
			_activeBuildPipeline = this;
		}
	}

	BuildPipeline::~BuildPipeline() {
		if (_activeBuildPipeline == this) {
			_activeBuildPipeline = nullptr;
		}
		if (_runner) {
			_runner->EndCommand();
			Delete(_runner);
			_runner = nullptr;
		}
	}

	bool BuildPipeline::Request(const BuildRequest& request, str& outRefusal) {
		if (_state == BuildState::Running) {
			outRefusal = "a build is already running (" + _currentProfile + "). Wait for it or cancel it";
			return false;
		}
		if (request.projectPath.empty()) {
			outRefusal = "no project path was given";
			return false;
		}
		if (request.profiles.empty()) {
			outRefusal = "no profiles were named";
			return false;
		}

		if (_runner) {
			_runner->EndCommand();
			Delete(_runner);
		}
		_diagnostics.clear();
		_runner = New<BuildRunner>(MemTag::Editor, _diagnostics);
		_runner->projectPath = request.projectPath;
		_runner->profiles = request.profiles;
		_runner->outputOverride = request.outputOverride;

		_result = BuildResult{};
		_state = BuildState::Running;
		_stepIndex = 0;
		_stepCount = 0;
		_stepName.clear();
		_currentProfile.clear();
		outRefusal.clear();
		return true;
	}

	void BuildPipeline::Cancel() {
		if (_state != BuildState::Running || !_runner) {
			return;
		}
		_runner->cancelRequested = true;
		_runner->EndCommand();
		SLT_LOG_WARN(CONSOLE_CATEGORY, "build canceled");
	}

	str BuildPipeline::GetStatusLabel() const {
		if (_state != BuildState::Running || _stepCount == 0) {
			return {};
		}
		return std::format("step {} of {}: {}", _stepIndex, _stepCount, _stepName);
	}

	void BuildPipeline::PumpMainThread() {
		if (_state != BuildState::Running || !_runner) {
			return;
		}
		BuildRunner& runner = *_runner;

		if (runner.cancelRequested) {
			if (runner.profileActive) {
				const BuildStepDesc& step = runner.steps[runner.stepIndex];
				if (step.fn) {
					step.fn(runner.context);
				}
				FinishProfile(BuildOutcome::Cancelled, "canceled");
			}
			FinishRequest();
			return;
		}

		if (!runner.profileActive) {
			if (runner.nextProfile >= runner.profiles.size()) {
				FinishRequest();
				return;
			}
			BeginNextProfile();
			return;
		}

		const BuildStepDesc& step = runner.steps[runner.stepIndex];
		BuildStepStatus status = step.fn ? step.fn(runner.context) : BuildStepStatus::Succeeded;
		if (status == BuildStepStatus::Running) {
			return;
		}

		runner.EndCommand();
		if (status == BuildStepStatus::Failed) {
			FinishProfile(BuildOutcome::Failed, "the '" + step.name + "' step failed");
			return;
		}

		++runner.stepIndex;
		if (runner.stepIndex >= runner.steps.size()) {
			FinishProfile(BuildOutcome::Succeeded, {});
			return;
		}
		PublishStep();
	}

	void BuildPipeline::PublishStep() {
		BuildRunner& runner = *_runner;
		_stepIndex = static_cast<u32>(runner.stepIndex) + 1;
		_stepCount = static_cast<u32>(runner.steps.size());
		_stepName = runner.steps[runner.stepIndex].name;
		runner.context.Log(GetStatusLabel());
	}

	void BuildPipeline::BeginNextProfile() {
		BuildRunner& runner = *_runner;
		str name = runner.profiles[runner.nextProfile++];
		_currentProfile = name;
		runner.errorCount = 0;
		runner.warningCount = 0;

		Vector<BuildSettingDesc> coreSchema{GetMemoryResource(MemTag::Editor)};
		BuildProfileOps::GetCoreSettings(coreSchema);
		ResolvedProfile probe;
		BuildProfileOps::ResolveProfile(runner.projectPath, name, coreSchema, probe);

		str targetId = probe.target.empty() ? BuildTargetRegistry::GetDefaultTargetId() : probe.target;

		Vector<BuildSettingDesc> schema{GetMemoryResource(MemTag::Editor)};
		BuildTargetRegistry::GetSchemaFor(targetId, schema);
		BuildProfileOps::ResolveProfile(runner.projectPath, name, schema, runner.resolved);

		const IBuildTarget* target = BuildTargetRegistry::Find(targetId);
		if (!target) {
			FinishSkipped(name, targetId,
						  "no build target named '" + targetId + "' is registered on this machine");
			return;
		}
		str unavailable;
		if (!target->IsAvailable(unavailable)) {
			FinishSkipped(name, targetId, unavailable);
			return;
		}

		BuildContext& context = runner.context;
		context = BuildContext{};
		context._runner = &runner;
		context.profile = &runner.resolved;
		context.target = target;
		context.projectRoot = runner.projectPath;
		context.stagingDir = GetStagingDirectory(runner.projectPath, runner.resolved, runner.outputOverride);
		context.intermediateDir = GetIntermediateDirectory(runner.projectPath, name);

		const ResolvedSetting* configuration = runner.resolved.Find("configuration");
		context.configuration = configuration && !configuration->value.empty() ? configuration->value : str("Release");

		const ResolvedSetting* executable = runner.resolved.Find("executableName");
		context.executableName = executable && !executable->value.empty()
									 ? executable->value
									 : ScriptProjectOps::GetModuleName(runner.projectPath);

		const ResolvedSetting* version = runner.resolved.Find("version");
		context.version = version ? version->value : str{};

		const ResolvedSetting* defines = runner.resolved.Find("defines");
		if (defines) {
			for (const str& define : defines->items) {
				if (!define.empty()) {
					context.defines.push_back(define);
				}
			}
		}

		target->ConfigurePipeline(context);

		BuildStepRegistry::GetSteps(runner.steps);
		if (runner.steps.empty()) {
			FinishSkipped(name, targetId, "no build steps are registered");
			return;
		}

		runner.stepIndex = 0;
		runner.profileStartTicks = Time::GetTicks();
		runner.profileActive = true;
		SLT_LOG_INFO(CONSOLE_CATEGORY, "building '{}' for {} -> {}", name, target->GetDisplayName(),
					 context.stagingDir);
		PublishStep();
	}

	void BuildPipeline::FinishSkipped(const str& name, const str& targetId, const str& reason) {
		BuildProfileResult entry;
		entry.profile = name;
		entry.targetId = targetId;
		entry.outcome = BuildOutcome::Skipped;
		entry.reason = reason;
		_result.profiles.push_back(std::move(entry));
		SLT_LOG_WARN(CONSOLE_CATEGORY, "'{}' skipped: {}", name, reason);
		WriteLastResult(_result.profiles.back());
	}

	void BuildPipeline::FinishProfile(BuildOutcome outcome, const str& reason) {
		BuildRunner& runner = *_runner;
		runner.EndCommand();

		BuildProfileResult entry;
		entry.profile = runner.resolved.name;
		entry.targetId = runner.context.target ? str(runner.context.target->GetId()) : str{};
		entry.outcome = outcome;
		entry.reason = reason;
		entry.outputPath = runner.context.stagingDir;
		entry.buildNumber = runner.context.buildNumber;
		entry.errorCount = runner.errorCount;
		entry.warningCount = runner.warningCount;
		entry.durationSeconds = static_cast<f64>(Time::GetTicks() - runner.profileStartTicks) /
								static_cast<f64>(Time::GetTicksPerSecond());
		entry.artifactBytes = MeasureDirectory(runner.context.stagingDir);
		if (outcome == BuildOutcome::Succeeded && runner.context.target) {
			entry.launchCommand = runner.context.target->GetLaunchCommand(runner.context);
		}
		for (const BuildIssue& issue : runner.context.GetIssues()) {
			entry.issues.push_back(issue);
		}

		if (outcome == BuildOutcome::Succeeded) {
			SLT_LOG_INFO(CONSOLE_CATEGORY, "'{}' succeeded in {:.1f}s ({} warning(s)) -> {}", entry.profile,
						 entry.durationSeconds, entry.warningCount, entry.outputPath);
		} else {
			SLT_LOG_ERROR(CONSOLE_CATEGORY, "'{}' {} after {:.1f}s ({} error(s)): {}", entry.profile,
						  outcome == BuildOutcome::Cancelled ? "canceled" : "FAILED", entry.durationSeconds,
						  entry.errorCount, entry.reason);
		}

		_result.profiles.push_back(std::move(entry));

		WriteLastResult(_result.profiles.back());

		runner.profileActive = false;
		runner.stepIndex = 0;
		// step functions must die before FreeLibrary
		runner.steps.clear();
		_stepIndex = 0;
		_stepCount = 0;
		_stepName.clear();
	}

	void BuildPipeline::WriteLastResult(const BuildProfileResult& entry) const {
		str projectPath = _runner ? _runner->projectPath : str{};
		if (projectPath.empty() || entry.profile.empty()) {
			return;
		}
		Path resultsPath(projectPath + "/Library/BuildResults.ini");

		Config results;
		results.Load(resultsPath);
		results.SetString(entry.profile, "result",
						  entry.outcome == BuildOutcome::Succeeded	 ? "Succeeded"
						  : entry.outcome == BuildOutcome::Skipped	 ? "Skipped"
						  : entry.outcome == BuildOutcome::Cancelled ? "Cancelled"
																	 : "Failed");
		results.SetString(entry.profile, "target", entry.targetId);
		results.SetString(entry.profile, "outputPath", entry.outputPath);
		results.SetString(entry.profile, "reason", entry.reason);
		results.SetI64(entry.profile, "timeUnix", static_cast<i64>(std::time(nullptr)));
		results.SetF64(entry.profile, "durationSeconds", entry.durationSeconds);
		results.SetI64(entry.profile, "artifactBytes", static_cast<i64>(entry.artifactBytes));
		results.SetI64(entry.profile, "buildNumber", static_cast<i64>(entry.buildNumber));

		std::error_code ec;
		fs::create_directories(fs::path{projectPath} / "Library", ec);
		results.Save(resultsPath);
	}

	void BuildPipeline::FinishRequest() {
		BuildRunner& runner = *_runner;
		bool canceled = runner.cancelRequested;
		runner.EndCommand();
		runner.profileActive = false;

		_state = canceled				  ? BuildState::Cancelled
				 : _result.AllSucceeded() ? BuildState::Succeeded
										  : BuildState::Failed;
		_stepIndex = 0;
		_stepCount = 0;
		_stepName.clear();
		_currentProfile.clear();
	}

} // namespace Shalltear
