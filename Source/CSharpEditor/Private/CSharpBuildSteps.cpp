#include "CSharpBuildSteps.h"

#include "CSharpAuthoring.h"
#include "CSharpDiagnostics.h"
#include "CSharpProjectFiles.h"
#include "CSharpReload.h"
#include "CSharpSettings.h"
#include "CSharpSetup.h"
#include "DotNetToolchain.h"
#include "ManagedCodeGate.h"
#include "NativeAOTHost.h"
#include "ScriptLog.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <filesystem>
#include <format>

namespace Shalltear::CSharpEditor::CSharpBuildSteps {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view ShellProject = "GameScripts.csproj";
		constexpr std::string_view ScriptsFolder = "scripts";
		constexpr std::string_view PublishFolder = "publish";
		constexpr std::string_view SymbolsFolder = "symbols";
		constexpr std::string_view RenameFolder = "obj/rename";
		constexpr std::string_view NameMap = "GameScripts.map";
		constexpr std::string_view NoticesFolder = "notices";
		constexpr std::string_view RuntimeNotices = "THIRD-PARTY-NOTICES-DotNet.txt";
		constexpr std::string_view CommonArguments =
			" -nologo -v:q -tl:off -clp:NoSummary -nodeReuse:false -p:UseSharedCompilation=false -p:GenerateFullPaths=true";
		constexpr size_t UNPARSED_LINE_CAP = 30;
		constexpr u64 GATE_BYTES_PER_PUMP = u64{32} << 20;

		enum class Stage : u8 {
			Idle = 0,
			Scripts,
			Shell
		};

		enum class Loading : u8 {
			Idle = 0,
			Setup,
			Compile
		};

		struct State {
			ChildProcess child;
			ManagedCodeGate::Scanner gate;
			Stage stage = Stage::Idle;
			Loading loading = Loading::Idle;
			bool gating = false;
			const void* owner = nullptr;
			str sdkRoot;
			str root;
			u32 errorCount = 0;
			u32 warningCount = 0;
			u64 startTicks = 0;
			Vector<str> lines{GetMemoryResource(MemTag::Editor)};
			Vector<str> unparsed{GetMemoryResource(MemTag::Editor)};
			Vector<CSharpDiagnostics::Diagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
			HashSet<str> seen{GetMemoryResource(MemTag::Editor)};
		};

		State* _state = nullptr;

		State& GetState() {
			if (!_state) {
				_state = New<State>(MemTag::Editor);
			}
			return *_state;
		}

		str GetPlayerRoot(const str& projectRoot, const str& profileName) {
			return projectRoot + "/Library/CSharp/Player/" + profileName;
		}

		std::string_view RuntimeIdentifierOf(std::string_view targetId) {
			return targetId == "Windows" || targetId == "WindowsServer" ? std::string_view("win-x64") : std::string_view();
		}

		bool WriteIfDifferent(const fs::path& file, const str& contents) {
			std::error_code ec;
			if (fs::is_regular_file(file, ec) && FileSystem::ReadTextFile(Path(file.generic_string()), MemTag::Editor) == contents) {
				return true;
			}
			fs::create_directories(file.parent_path(), ec);
			return FileSystem::WriteTextFile(Path(file.generic_string()), contents);
		}

		bool RenderShell(const State& state, const Vector<str>& assemblies) {
			Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
			str references;
			str aliases;
			str calls;
			for (size_t index = 0; index < assemblies.size(); ++index) {
				references += std::format("\t\t<Reference Include=\"{0}\" HintPath=\"{1}/bin/{0}.dll\" Aliases=\"Scripts{2}\" />\n"
										  "\t\t<ShalltearRenameAssembly Include=\"{0}\" />\n",
										  assemblies[index], ScriptsFolder, index);
				aliases += std::format("extern alias Scripts{};\n", index);
				calls += std::format("\t\tScripts{}::Shalltear.Generated.TypeTable.Register();\n", index);
			}
			tokens.push_back({"SCRIPT_REFERENCES", references});
			tokens.push_back({"SCRIPT_ALIASES", aliases});
			tokens.push_back({"SCRIPT_CALLS", calls});
			str project;
			str exports;
			str tables;
			if (!ScriptProjectOps::RenderTemplate("CSharpPlayer.csproj", tokens, project) ||
				!ScriptProjectOps::RenderTemplate("CSharpPlayerExports.cs", tokens, exports) ||
				!ScriptProjectOps::RenderTemplate("CSharpPlayerTypeTables.cs", tokens, tables)) {
				return false;
			}
			const fs::path root(state.root);
			return WriteIfDifferent(root / ShellProject, project) && WriteIfDifferent(root / "Exports.g.cs", exports) &&
				   WriteIfDifferent(root / "TypeTables.g.cs", tables);
		}

		bool Start(State& state, const BuildContext& context, const str& arguments) {
			const ChildVariable variables[] = {
				{"DOTNET_ROOT", state.sdkRoot},
				{"DOTNET_CLI_UI_LANGUAGE", "en-us"},
				{"DOTNET_CLI_TELEMETRY_OPTOUT", "1"},
				{"DOTNET_NOLOGO", "1"},
				{"DOTNET_CLI_USE_MSBUILD_SERVER", "0"},
			};
			state.unparsed.clear();
			const str command = QuoteCommandArgument(DotNetToolchain::GetMuxerPath(state.sdkRoot)) + " " + arguments + str(CommonArguments);
			context.Log(command);
			return state.child.Start(command, context.projectRoot, variables);
		}

		void Consume(State& state) {
			state.lines.clear();
			state.child.DrainLines(state.lines);
			for (const str& line : state.lines) {
				CSharpDiagnostics::Diagnostic diagnostic;
				if (!CSharpDiagnostics::ParseLine(line, diagnostic)) {
					if (!line.empty()) {
						if (state.unparsed.size() == UNPARSED_LINE_CAP) {
							state.unparsed.erase(state.unparsed.begin());
						}
						state.unparsed.push_back(line);
					}
					continue;
				}
				if (!CSharpDiagnostics::AddUnique(state.diagnostics, state.seen, diagnostic)) {
					continue;
				}
				const bool failed = diagnostic.severity == CompilerDiagnosticSeverity::Error;
				++(failed ? state.errorCount : state.warningCount);
				LogManager::Dispatch(failed ? LogLevel::Error : LogLevel::Warn, "C#", CSharpImpl::InternLogOrigin(diagnostic.file), diagnostic.line, "", "{}",
									 CSharpDiagnostics::Describe(diagnostic));
			}
		}

		BuildStepStatus Fail(BuildContext& context, State& state, std::string_view what, i32 exitCode) {
			str said = state.errorCount > 0 ? std::format("{} failed ({} error(s) in log above)", what, state.errorCount)
											: std::format("{} failed with exit code {}", what, exitCode);
			if (state.errorCount == 0) {
				for (const str& line : state.unparsed) {
					said += "\n" + line;
				}
			}
			for (const CSharpDiagnostics::Diagnostic& diagnostic : state.diagnostics) {
				if (diagnostic.code == "IL2104" || diagnostic.code == "IL3053") {
					said += ". A NuGet package isn't safe to compile ahead of time. Replace it (or add its warning code to WarningsNotAsErrors in CSharp.props)";
					break;
				}
			}
			context.AddIssue(BuildIssueSeverity::Error, "", said);
			state.stage = Stage::Idle;
			return BuildStepStatus::Failed;
		}

		BuildStepStatus StartScripts(BuildContext& context, State& state) {
			state.errorCount = 0;
			state.warningCount = 0;
			state.diagnostics.clear();
			state.seen.clear();
			state.startTicks = Time::GetTicks();
			if (state.sdkRoot.empty()) {
				state.sdkRoot = DotNetToolchain::FindSDKRoot();
			}

			const CSharpProjectFiles::Options files = CSharpProjectFiles::GetDefaultOptions(context.projectRoot, state.sdkRoot);
			const str project = CSharpProjectFiles::GetProjectFile(files);
			const std::string_view targetId = context.target->GetId();
			Vector<str> assemblies{GetMemoryResource(MemTag::Editor)};
			for (const CSharpAssemblies::Definition& definition : files.assemblies.definitions) {
				if (!CSharpAssemblies::BuildsFor(definition, targetId)) {
					continue;
				}
				for (const str& reference : definition.references) {
					const CSharpAssemblies::Definition* used = CSharpAssemblies::Find(files.assemblies, reference);
					if (used && !CSharpAssemblies::BuildsFor(*used, targetId)) {
						context.AddIssue(BuildIssueSeverity::Error, "",
										 std::format("'{}' references '{}' (not built for {}). Add the target to its Platforms (or remove the reference)",
													 definition.file, reference, context.target->GetDisplayName()));
						return BuildStepStatus::Failed;
					}
				}
				assemblies.push_back(definition.name);
			}
			assemblies.push_back(fs::path(project).replace_extension().generic_string());
			state.root = GetPlayerRoot(context.projectRoot, context.profile->name);
			if (!RenderShell(state, assemblies)) {
				context.AddIssue(BuildIssueSeverity::Error, "", std::format("couldn't write C# project files under '{}'. Check folder is writable", state.root));
				return BuildStepStatus::Failed;
			}

			const str arguments = std::format("msbuild {} -restore -t:Build -p:Configuration={} -p:ShalltearPlayerScripts=true -p:ShalltearTargetPlatform={} {}",
											  QuoteCommandArgument(project), context.configuration, targetId,
											  QuoteCommandArgument(std::format("-p:ShalltearBuildRoot={}/{}/", state.root, ScriptsFolder)));
			if (!Start(state, context, arguments)) {
				context.AddIssue(BuildIssueSeverity::Error, "", "C# script compile couldn't be started");
				return BuildStepStatus::Failed;
			}
			state.stage = Stage::Scripts;
			return BuildStepStatus::Running;
		}

		BuildStepStatus StartShell(BuildContext& context, State& state) {
			str arguments = std::format("msbuild {} -restore -t:Publish -p:Configuration={} -p:RuntimeIdentifier={}",
										QuoteCommandArgument(state.root + "/" + str(ShellProject)), context.configuration,
										RuntimeIdentifierOf(context.target->GetId()));
			const str native = fs::path(DotNetToolchain::GetRuntimeAssemblyPath()).parent_path().generic_string() + "/native/";
			arguments += " " + QuoteCommandArgument("-p:ShalltearNativeDir=" + native);
			std::error_code ec;
			fs::remove_all(fs::path(state.root) / RenameFolder, ec);
			if (context.configuration == "Release" && CSharpSettings::GetProject().renameScriptNames) {
				arguments += " -p:ShalltearRenameScripts=true";
			}
			if (CSharpSettings::GetProject().windowsLinker == WindowsLinker::VisualStudio) {
				arguments += " -p:ShalltearWindowsLinker=VisualStudio";
			} else {
				CppToolchain::DiscoveryOptions options;
				options.sdkRoot = context.sdkRoot;
				options.probeVersions = false;
				const str compiler = CppToolchain::Discover(options).compiler.path;
				arguments += " " + QuoteCommandArgument("-p:ShalltearLinkerDir=" + fs::path(compiler).parent_path().generic_string() + "/");
			}
			if (!Start(state, context, arguments)) {
				context.AddIssue(BuildIssueSeverity::Error, "", "C# native compile couldn't be started");
				state.stage = Stage::Idle;
				return BuildStepStatus::Failed;
			}
			state.stage = Stage::Shell;
			return BuildStepStatus::Running;
		}

		BuildStepStatus EndLoad(State& state, BuildStepStatus status) {
			state.loading = Loading::Idle;
			CSharpReload::SetBuildStarting(false);
			return status;
		}

		BuildStepStatus LoadStep(BuildContext& context) {
			State& state = GetState();
			if (context.IsCancelRequested()) {
				return EndLoad(state, BuildStepStatus::Failed);
			}
			if (state.loading == Loading::Idle) {
				CSharpReload::SetBuildStarting(true);
				if (CSharpSetup::GetState() == CSharpSetup::State::Idle && !CSharpAuthoring::IsActive()) {
					CSharpSetup::Options options = CSharpSetup::GetDefaultOptions();
					options.buildServers = false;
					CSharpSetup::Begin(std::move(options), state.owner);
				}
				state.loading = Loading::Setup;
			}
			CSharpSetup::Pump();
			CSharpAuthoring::Pump();
			if (state.loading == Loading::Setup) {
				const CSharpSetup::State setup = CSharpSetup::GetState();
				if (setup == CSharpSetup::State::Acquiring) {
					return BuildStepStatus::Running;
				}
				if (!CSharpAuthoring::IsActive()) {
					context.AddIssue(BuildIssueSeverity::Error, "", "C# scripts couldn't start (reason in log above)");
					return EndLoad(state, BuildStepStatus::Failed);
				}
				if (CSharpReload::IsPlaying()) {
					return EndLoad(state, BuildStepStatus::Succeeded);
				}
				if (!CSharpAuthoring::GetCompile().IsBusy()) {
					CSharpAuthoring::RequestCompile();
				}
				state.loading = Loading::Compile;
				return BuildStepStatus::Running;
			}
			const CSharpCompile& compile = CSharpAuthoring::GetCompile();
			if (compile.IsBusy()) {
				return BuildStepStatus::Running;
			}
			if (compile.GetState() == CompileState::Failed) {
				context.AddIssue(BuildIssueSeverity::Error, "", "C# scripts don't compile (errors in log above)");
				return EndLoad(state, BuildStepStatus::Failed);
			}
			if (CSharpReload::IsPending()) {
				return BuildStepStatus::Running;
			}
			if (!CSharpReload::IsLoaded() || CSharpReload::IsBehind()) {
				context.AddIssue(BuildIssueSeverity::Error, "", "C# scripts couldn't be loaded (errors in log above)");
				return EndLoad(state, BuildStepStatus::Failed);
			}
			return EndLoad(state, BuildStepStatus::Succeeded);
		}

		BuildStepStatus ValidateStep(BuildContext& context) {
			State& state = GetState();
			state.child.Kill();
			state.stage = Stage::Idle;
			state.gating = false;
			EndLoad(state, BuildStepStatus::Succeeded);
			if (RuntimeIdentifierOf(context.target->GetId()).empty()) {
				context.AddIssue(BuildIssueSeverity::Error, "target",
								 std::format("C# scripts don't build for {} yet. Build this profile for Windows (or turn off C# Scripting package)",
											 context.target->GetDisplayName()));
				return BuildStepStatus::Failed;
			}
			CSharpSettings::Declare();
			state.sdkRoot = DotNetToolchain::FindSDKRoot();
			if (state.sdkRoot.empty()) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 std::format(".NET SDK {} wasn't found. Open project in editor once (downloads it)",
											 DotNetToolchain::GetPin().sdkVersion));
				return BuildStepStatus::Failed;
			}
			return BuildStepStatus::Succeeded;
		}

		BuildStepStatus CompileStep(BuildContext& context) {
			State& state = GetState();
			if (context.IsCancelRequested()) {
				state.child.Kill();
				state.stage = Stage::Idle;
				return BuildStepStatus::Failed;
			}
			if (state.stage == Stage::Idle) {
				return StartScripts(context, state);
			}
			Consume(state);
			i32 exitCode = 0;
			if (!state.child.TryFinish(exitCode)) {
				return BuildStepStatus::Running;
			}
			Consume(state);
			const bool scripts = state.stage == Stage::Scripts;
			if (exitCode != 0 || state.errorCount > 0) {
				return Fail(context, state, scripts ? "C# script compile" : "C# native compile", exitCode);
			}
			if (scripts) {
				return StartShell(context, state);
			}
			state.stage = Stage::Idle;
			if (state.warningCount > 0) {
				context.AddIssue(BuildIssueSeverity::Warning, "", std::format("C# scripts compiled with {} warning(s) (in log above)", state.warningCount));
			}
			context.Log(std::format("C# scripts compiled in {:.1f} s",
									static_cast<f64>(Time::GetTicks() - state.startTicks) / static_cast<f64>(Time::GetTicksPerSecond())));
			std::error_code ec;
			const fs::path report = fs::path(state.root) / RenameFolder / "report.txt";
			if (fs::is_regular_file(report, ec)) {
				for (const str& line : StringUtil::Split(FileSystem::ReadTextFile(Path(report.generic_string()), MemTag::Editor), "\n")) {
					const str trimmed = StringUtil::Trim(line);
					if (!trimmed.empty()) {
						context.Log(trimmed);
					}
				}
			}
			return BuildStepStatus::Succeeded;
		}

		BuildStepStatus StageStep(BuildContext& context) {
			const fs::path publish = fs::path(GetPlayerRoot(context.projectRoot, context.profile->name)) / PublishFolder;
			const fs::path library = publish / CSharpImpl::NativeAOTHost::GetPlayerLibraryName();
			std::error_code ec;
			fs::create_directories(fs::path(context.stagingDir), ec);
			ec.clear();
			fs::copy_file(library, fs::path(context.stagingDir) / CSharpImpl::NativeAOTHost::GetPlayerLibraryName(), fs::copy_options::overwrite_existing, ec);
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, "", std::format("couldn't stage '{}' ({})", library.generic_string(), ec.message()));
				return BuildStepStatus::Failed;
			}
			const fs::path libraryExtension = library.extension();
			for (fs::directory_iterator it(publish, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code copied;
				if (!it->is_regular_file(copied) || it->path().extension() != libraryExtension || it->path().filename() == library.filename()) {
					continue;
				}
				fs::copy_file(it->path(), fs::path(context.stagingDir) / it->path().filename(), fs::copy_options::overwrite_existing, copied);
				if (copied) {
					context.AddIssue(BuildIssueSeverity::Error, "", std::format("couldn't stage '{}' ({})", it->path().generic_string(), copied.message()));
					return BuildStepStatus::Failed;
				}
			}
			str notices = std::format(".NET {} runtime/libraries, compiled into {} by NativeAOT\n\n", DotNetToolchain::GetPin().runtimeVersion,
									  CSharpImpl::NativeAOTHost::GetPlayerLibraryName());
			for (const std::string_view name : {"LICENSE.TXT", "THIRD-PARTY-NOTICES.TXT"}) {
				const fs::path part = publish / NoticesFolder / name;
				if (!fs::is_regular_file(part, ec)) {
					context.AddIssue(BuildIssueSeverity::Error, "", std::format("'{}' is missing. Build player again", part.generic_string()));
					return BuildStepStatus::Failed;
				}
				notices += FileSystem::ReadTextFile(Path(part.generic_string()), MemTag::Editor) + "\n";
			}
			const fs::path staged = fs::path(context.stagingDir) / RuntimeNotices;
			if (!FileSystem::WriteTextFile(Path(staged.generic_string()), notices)) {
				context.AddIssue(BuildIssueSeverity::Error, "", std::format("couldn't write '{}'. Check staging folder is writable", staged.generic_string()));
				return BuildStepStatus::Failed;
			}
			const fs::path kept = fs::path(context.intermediateDir) / SymbolsFolder;
			const fs::path map = fs::path(GetPlayerRoot(context.projectRoot, context.profile->name)) / RenameFolder / NameMap;
			fs::remove(kept / NameMap, ec);
			for (const fs::path& symbols : {fs::path(library).replace_extension(".pdb"), map}) {
				if (!fs::is_regular_file(symbols, ec)) {
					continue;
				}
				fs::create_directories(kept, ec);
				ec.clear();
				fs::copy_file(symbols, kept / symbols.filename(), fs::copy_options::overwrite_existing, ec);
				if (ec) {
					context.AddIssue(BuildIssueSeverity::Warning, "", std::format("couldn't keep '{}' ({})", symbols.generic_string(), ec.message()));
				}
			}
			return BuildStepStatus::Succeeded;
		}

		BuildStepStatus GateStep(BuildContext& context) {
			State& state = GetState();
			if (context.IsCancelRequested()) {
				state.gating = false;
				return BuildStepStatus::Failed;
			}
			if (!state.gating) {
				state.gate.Begin(context.stagingDir, context.configuration == "Release");
				state.gating = true;
			}
			state.gate.Advance(GATE_BYTES_PER_PUMP);
			if (!state.gate.IsDone()) {
				return BuildStepStatus::Running;
			}
			state.gating = false;
			for (const ManagedCodeGate::Finding& finding : state.gate.GetFindings()) {
				context.AddIssue(BuildIssueSeverity::Error, "", finding.message);
			}
			if (!state.gate.GetFindings().empty()) {
				return BuildStepStatus::Failed;
			}
			context.Log(std::format("{} staged file(s) have no managed code", state.gate.GetFileCount()));
			return BuildStepStatus::Succeeded;
		}

	} // anonymous namespace

	void Register(const void* owner) {
		GetState().owner = owner;
		BuildStepRegistry::Register("csharp-validate", BuildPhase::Validate, &ValidateStep, owner);
		BuildStepRegistry::Register("csharp-load", BuildPhase::PreBuild, &LoadStep, owner);
		BuildStepRegistry::Register("csharp-compile", BuildPhase::Compile, &CompileStep, owner);
		BuildStepRegistry::Register("csharp-stage", BuildPhase::Stage, &StageStep, owner);
		BuildStepRegistry::Register("csharp-no-il", BuildPhase::PostBuild, &GateStep, owner);
	}

	void Shutdown() {
		if (_state) {
			BuildStepRegistry::UnregisterOwnedBy(_state->owner);
			CSharpReload::SetBuildStarting(false);
			_state->child.Kill();
			Delete(_state);
			_state = nullptr;
		}
	}

} // namespace Shalltear::CSharpEditor::CSharpBuildSteps
