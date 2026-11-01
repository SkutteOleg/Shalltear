#include "Shalltear/Editor/BuildPipeline.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/CookOps.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/CSharpTranspiler.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/SettingsOps.h"

#include "ChildProcess.h"

#include <ctime>
#include <filesystem>
#include <format>
#include <map>
#include <span>
#include <system_error>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* PLAYER_PROJECT_TEMPLATE = "PlayerProject.cmake";
		constexpr const char* PLAYER_STUB_TEMPLATE = "PlayerProjectStub.cpp";

		char ToLowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}

		bool PathsEqualIgnoringCase(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				char lhs = a[i] == '\\' ? '/' : ToLowerAscii(a[i]);
				char rhs = b[i] == '\\' ? '/' : ToLowerAscii(b[i]);
				if (lhs != rhs) {
					return false;
				}
			}
			return true;
		}

		bool EndsWithIgnoringCase(std::string_view value, std::string_view suffix) {
			return value.size() >= suffix.size() &&
				   PathsEqualIgnoringCase(value.substr(value.size() - suffix.size()), suffix);
		}

		str Quote(const str& value) {
			return QuoteCommandArgument(value);
		}

		str ResolvedString(const BuildContext& context, std::string_view key) {
			const ResolvedSetting* setting = context.profile ? context.profile->Find(key) : nullptr;
			return setting ? setting->value : str{};
		}

		bool ResolvedBool(const BuildContext& context, std::string_view key) {
			str value = StringUtil::ToLower(ResolvedString(context, key));
			return value == "true" || value == "1" || value == "yes" || value == "on";
		}

		CookOps::CookCompression ResolveCompression(const str& value) {
			str lowered = StringUtil::ToLower(value);
			if (lowered == "lz4") {
				return CookOps::CookCompression::LZ4;
			}
			if (lowered == "lz4hc") {
				return CookOps::CookCompression::LZ4HC;
			}
			return CookOps::CookCompression::None;
		}

		str ResolveEngineAssetsRoot() {
			constexpr const char* PROBE = "engine://.mountprobe";
			str resolved = FileSystem::Resolve(Path(PROBE)).ToString();
			if (resolved == PROBE) {
				return {};
			}
			return Path(resolved).GetDirectory();
		}

		BuildStepStatus StartCommand(BuildContext& context, const str& what, const str& commandLine,
									 const str& workingDirectory) {
			context.Log(what + ": " + commandLine);
			if (!context.StartCommand(commandLine, workingDirectory)) {
				context.AddIssue(BuildIssueSeverity::Error, "", what + " couldn't be started");
				return BuildStepStatus::Failed;
			}
			return BuildStepStatus::Running;
		}

		BuildStepStatus PollCommand(BuildContext& context, const str& what) {
			i32 exitCode = 0;
			if (!context.TryFinishCommand(exitCode)) {
				return BuildStepStatus::Running;
			}
			if (exitCode != 0) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 what + " failed with exit code " + std::format("{}", exitCode));
				return BuildStepStatus::Failed;
			}
			return BuildStepStatus::Succeeded;
		}

		BuildStepStatus RunProfileCommand(BuildContext& context, std::string_view key, const char* what) {
			str command = ResolvedString(context, key);
			if (command.empty()) {
				return BuildStepStatus::Succeeded;
			}
			if (context.HasCommand()) {
				return PollCommand(context, str(what));
			}
			return StartCommand(context, str(what), command, context.projectRoot);
		}

		BuildStepStatus ValidateStep(BuildContext& context) {
			Vector<BuildIssue> issues{GetMemoryResource(MemTag::Editor)};

			Vector<BuildSettingDesc> schema{GetMemoryResource(MemTag::Editor)};
			BuildTargetRegistry::GetSchemaFor(context.target->GetId(), schema);
			BuildProfileOps::ValidateProfile(context.projectRoot, *context.profile, schema, issues);
			context.target->Validate(context.projectRoot, *context.profile, issues);

			if (context.target->RequiresMonotonicBuildNumber()) {
				const ResolvedSetting* number = context.profile->Find("buildNumber");
				if (number && number->rawValue.find("${AUTO}") != str::npos) {
					issues.push_back(BuildIssue{BuildIssueSeverity::Warning, context.profile->name, "buildNumber",
												"this target requires a monotonic build number + ${AUTO} counts per "
												"machine. Use ${ENV:...} from a build server"});
				}
			}

			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = context.sdkRoot;
			options.probeVersions = false;
			CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);
			if (!toolchain.IsComplete()) {
				issues.push_back(BuildIssue{BuildIssueSeverity::Error, context.profile->name, "",
											"the C++ toolchain is incomplete:\n" + CppToolchain::Describe(toolchain)});
			}

			u32 errors = 0;
			for (const BuildIssue& issue : issues) {
				context.AddIssue(issue.severity, issue.key, issue.message);
				if (issue.severity == BuildIssueSeverity::Error) {
					++errors;
				}
			}

			Vector<str> staged{GetMemoryResource(MemTag::Editor)};
			BuildPipeline::GetStagedScenes(*context.profile, staged);
			Vector<str> inProject{GetMemoryResource(MemTag::Editor)};
			BuildProfileOps::GetProjectScenes(context.projectRoot, inProject);
			context.Log(std::format("{} scene(s) in this project, {} in this build", inProject.size(), staged.size()));

			return errors == 0 ? BuildStepStatus::Succeeded : BuildStepStatus::Failed;
		}

		BuildStepStatus BuildNumberStep(BuildContext& context) {
			std::time_t now = std::time(nullptr);
			context.buildTimeUnix = static_cast<i64>(now);
			std::tm utc{};
			if (::gmtime_s(&utc, &now) == 0) {
				char buffer[32] = {};
				std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
				context.buildTimeIso = buffer;
			}

			str raw = ResolvedString(context, "buildNumber");
			if (raw.find("${AUTO}") != str::npos) {
				u32 next = BuildProfileOps::NextBuildNumber(context.projectRoot, context.target->GetId());
				raw = StringUtil::Replace(raw, "${AUTO}", std::format("{}", next));
			}

			if (!raw.empty()) {
				u32 parsed = 0;
				bool numeric = true;
				for (char c : raw) {
					if (c < '0' || c > '9') {
						numeric = false;
						break;
					}
					parsed = parsed * 10 + static_cast<u32>(c - '0');
				}
				if (numeric) {
					context.buildNumber = parsed;
				} else {
					context.AddIssue(BuildIssueSeverity::Warning, "buildNumber",
									 "'" + raw + "' didn't expand to a whole number (build stamped 0)");
				}
			}

			std::error_code ec;
			fs::create_directories(fs::path{context.stagingDir}, ec);
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, "outputPath",
								 "could not create '" + context.stagingDir + "' (" + ec.message() + ")");
				return BuildStepStatus::Failed;
			}
			context.Log(std::format("build {} at {}", context.buildNumber, context.buildTimeIso));
			return BuildStepStatus::Succeeded;
		}

		BuildStepStatus BootManifestStep(BuildContext& context) {
			str productName;
			u64 created = 0;
			ProjectOps::ReadProjectFile(context.projectRoot, productName, created);
			if (productName.empty()) {
				productName = context.executableName;
			}

			Vector<str> scenes{GetMemoryResource(MemTag::Editor)};
			BuildPipeline::GetStagedScenes(*context.profile, scenes);
			if (scenes.empty()) {
				context.AddIssue(BuildIssueSeverity::Warning, "scenes",
								 "this build stages no scenes (the player will start empty)");
			}

			str json = "{\n";
			json += std::format("\t\"formatVersion\": {},\n", 2);
			json += "\t\"productName\": " + JSONSerialize(JSONValue::MakeString(productName), false) + ",\n";
			json += "\t\"scenes\": [";
			for (size_t i = 0; i < scenes.size(); ++i) {
				Guid guid = AssetDatabase::LookupGuid(Path("project://" + scenes[i]));
				if (!guid.IsValid()) {
					context.AddIssue(BuildIssueSeverity::Error, "scenes",
									 "'" + scenes[i] +
										 "' has no Guid. Open the project in the "
										 "editor once to index it");
					return BuildStepStatus::Failed;
				}
				json += "\n\t\t{ \"path\": " + JSONSerialize(JSONValue::MakeString(scenes[i]), false);
				json += ", \"guid\": \"" + guid.ToString() + "\" }";
				json += i + 1 < scenes.size() ? "," : "\n\t";
			}
			json += "]\n}\n";

			context.bootManifest = std::move(json);
			return BuildStepStatus::Succeeded;
		}

		bool RenderGeneratedProject(BuildContext& context, str& outSourceDirectory) {
			outSourceDirectory = context.intermediateDir;

			Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
			tokens.push_back({"MODULE_NAME", ScriptProjectOps::GetModuleName(context.projectRoot)});

			str project;
			str stub;
			if (!ScriptProjectOps::RenderTemplate(PLAYER_PROJECT_TEMPLATE, tokens, project) ||
				!ScriptProjectOps::RenderTemplate(PLAYER_STUB_TEMPLATE, tokens, stub)) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 "this project has no CMakeLists.txt and the generated one could not be rendered "
								 "(is engine:// mounted?)");
				return false;
			}

			std::error_code ec;
			fs::create_directories(fs::path{outSourceDirectory}, ec);
			return FileSystem::WriteTextFile(Path(outSourceDirectory + "/CMakeLists.txt"), project) &&
				   FileSystem::WriteTextFile(Path(outSourceDirectory + "/GameStub.cpp"), stub);
		}

		BuildStepStatus ConfigureStep(BuildContext& context) {
			if (!ResolvedString(context, "buildCommand").empty()) {
				context.Log("configure skipped: this profile has a buildCommand");
				return BuildStepStatus::Succeeded;
			}
			if (context.HasCommand()) {
				return PollCommand(context, "configure");
			}

			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = context.sdkRoot;
			options.probeVersions = false;
			CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);

			std::error_code csGenEc;
			if (CSharpTranspiler::HasCSharpSources(context.projectRoot) ||
				fs::is_directory(fs::path(CSharpTranspiler::GetGeneratedDirectory(context.projectRoot)), csGenEc)) {
				CSharpTranspiler::TranspileProjectResult transpileResult;
				if (!CSharpTranspiler::TranspileProject(context.projectRoot, transpileResult)) {
					for (const CompilerDiagnostic& diag : transpileResult.diagnostics) {
						BuildIssueSeverity sev = diag.severity == CompilerDiagnosticSeverity::Error
													 ? BuildIssueSeverity::Error
													 : BuildIssueSeverity::Warning;
						context.AddIssue(sev, "csharp",
										 std::format("{}:{}:{}: {}", diag.file, diag.line, diag.column, diag.message));
					}
					return BuildStepStatus::Failed;
				}
			}

			str sourceDirectory = context.projectRoot;
			if (!ScriptProjectOps::HasScriptProject(context.projectRoot) &&
				!RenderGeneratedProject(context, sourceDirectory)) {
				return BuildStepStatus::Failed;
			}

			str defines;
			for (const str& define : context.defines) {
				if (!defines.empty()) {
					defines += ";";
				}
				defines += define;
			}

			str commandLine = std::format("{} -S {} -B {} -G Ninja -DCMAKE_BUILD_TYPE={}", Quote(toolchain.cmake.path),
										  Quote(sourceDirectory), Quote(context.intermediateDir),
										  context.configuration);
			commandLine += " -DCMAKE_CXX_COMPILER=" + Quote(toolchain.compiler.path);
			commandLine += " -DCMAKE_MAKE_PROGRAM=" + Quote(toolchain.ninja.path);
			commandLine += " -DShalltear_ROOT=" + Quote(context.sdkRoot);
			commandLine += " -DSHALLTEAR_PROJECT_ROOT=" + Quote(context.projectRoot);

			commandLine += " -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=" + Quote(CppToolchain::GetEngineIdentity());

			commandLine += " -DSHALLTEAR_PLAYER_TARGET=" + Quote(str(context.target->GetId()));
			commandLine += " -DSHALLTEAR_PLAYER_ENTRY=" + Quote(context.entrySource);
			commandLine += str(" -DSHALLTEAR_PLAYER_WINDOWED=") + (context.windowedSubsystem ? "ON" : "OFF");
			commandLine += " -DSHALLTEAR_PLAYER_OUTPUT_DIRECTORY=" + Quote(context.stagingDir);
			commandLine += " -DSHALLTEAR_PLAYER_OUTPUT_NAME=" + Quote(context.executableName);
			commandLine += " -DSHALLTEAR_PLAYER_DEFINES=" + Quote(defines);
			commandLine += str(" -DSHALLTEAR_PLAYER_DEBUG_SYMBOLS=") +
						   (ResolvedBool(context, "copyDebugSymbols") ? "ON" : "OFF");

			Vector<PackageOps::PackageManifest> catalogue;
			Vector<str> packageDiagnostics;
			PackageOps::LoadBuiltInCatalogue(catalogue, packageDiagnostics);
			for (const str& diagnostic : packageDiagnostics) {
				SLT_LOG_WARN("Editor", "package catalogue: {}", diagnostic);
			}

			Vector<str> stored;
			PackageOps::ReadEnabledSet(stored);
			Vector<PackageOps::ResolvedPackage> resolution;
			PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
			Vector<str> playerPackages;
			PackageOps::SelectGamePackages(catalogue, resolution, playerPackages);

			str packageList;
			for (const str& packageId : playerPackages) {
				if (!packageList.empty()) {
					packageList += ";";
				}
				packageList += packageId;
			}
			commandLine += " -DSHALLTEAR_ENABLED_PACKAGES=" + Quote(packageList);

			commandLine += " -DSHALLTEAR_STAMP_NUMBER=" + std::format("{}", context.buildNumber);
			commandLine += " -DSHALLTEAR_STAMP_TIME_UNIX=" + std::format("{}", context.buildTimeUnix);
			commandLine += " -DSHALLTEAR_STAMP_TIME_ISO=" + Quote(context.buildTimeIso);
			commandLine += " -DSHALLTEAR_STAMP_VERSION=" + Quote(context.version);
			commandLine += " -DSHALLTEAR_STAMP_PROFILE=" + Quote(context.profile->name);

			return StartCommand(context, "configure", commandLine, context.projectRoot);
		}

		BuildStepStatus CompileStep(BuildContext& context) {
			str custom = ResolvedString(context, "buildCommand");
			if (context.HasCommand()) {
				return PollCommand(context, custom.empty() ? str("compile") : str("buildCommand"));
			}
			if (!custom.empty()) {
				return StartCommand(context, "buildCommand", custom, context.projectRoot);
			}

			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = context.sdkRoot;
			options.probeVersions = false;
			CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);

			str commandLine = std::format("{} --build {} --parallel", Quote(toolchain.cmake.path),
										  Quote(context.intermediateDir));
			return StartCommand(context, "compile", commandLine, context.projectRoot);
		}

		void ClearStagedSubtree(const str& directory) {
			std::error_code ec;
			fs::remove_all(fs::path{directory}, ec);
		}

		bool CopyFileInto(BuildContext& context, const str& source, const str& destination) {
			std::error_code ec;
			fs::create_directories(fs::path{destination}.parent_path(), ec);
			ec.clear();
			fs::copy_file(fs::path{source}, fs::path{destination}, fs::copy_options::overwrite_existing, ec);
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, "", "couldn't stage '" + source + "' (" + ec.message() + ")");
				return false;
			}
			return true;
		}

		struct RuntimeDependency {
			std::string_view source;
			std::string_view destination;
		};

		constexpr RuntimeDependency RuntimeDependencies[] = {
			{"THIRD-PARTY-NOTICES-Runtime.txt", "THIRD-PARTY-NOTICES.txt"},
#ifdef __MINGW32__
			{"libc++.dll", "libc++.dll"},
			{"libunwind.dll", "libunwind.dll"},
#endif
		};

		bool FindRuntimeDependency(const BuildContext& context, std::string_view fileName, str& out) {
			Vector<str> directories{GetMemoryResource(MemTag::Editor)};
			if (!context.sdkRoot.empty()) {
				directories.push_back(context.sdkRoot);
				directories.push_back(context.sdkRoot + "/bin");
				directories.push_back(context.sdkRoot + "/Bin");
			}
			str editor = ScriptProjectOps::GetEditorExecutablePath();
			if (!editor.empty()) {
				directories.push_back(Path(editor).GetDirectory());
			}

			std::error_code ec;
			for (const str& directory : directories) {
				str candidate = directory + "/" + str(fileName);
				if (fs::is_regular_file(fs::path{candidate}, ec)) {
					out = std::move(candidate);
					return true;
				}
				ec.clear();
			}
			return false;
		}

		BuildStepStatus StageRuntimeStep(BuildContext& context) {
			for (const RuntimeDependency& dependency : RuntimeDependencies) {
				str source;
				if (!FindRuntimeDependency(context, dependency.source, source)) {
					context.AddIssue(BuildIssueSeverity::Error, "",
									 "'" + str(dependency.source) +
										 "' was not found beside the SDK or the editor (should be installed beside the "
										 "engine binaries)");
					return BuildStepStatus::Failed;
				}
				if (!CopyFileInto(context, source, context.stagingDir + "/" + str(dependency.destination))) {
					return BuildStepStatus::Failed;
				}
			}
			return BuildStepStatus::Succeeded;
		}

		str ReadProjectSettingsDocument() {
			Path document = Settings::GetDefaultPaths().project;
			if (document.ToString().empty() || !FileSystem::Exists(document)) {
				return {};
			}
			return FileSystem::ReadTextFile(document, MemTag::Editor);
		}

		bool EnumerateCandidates(BuildContext& context, u32& outCandidates) {
			std::map<str, str> guidToPath;
			outCandidates = 0;

			auto walk = [&](const str& root, const str& label) {
				std::error_code ec;
				if (!fs::is_directory(fs::path{root}, ec)) {
					return true;
				}
				for (fs::recursive_directory_iterator it(fs::path{root}, ec), end; !ec && it != end;
					 it.increment(ec)) {
					std::error_code fileEc;
					if (!it->is_regular_file(fileEc) || fileEc) {
						continue;
					}
					str path = it->path().generic_string();
					if (!EndsWithIgnoringCase(path, ".meta")) {
						continue;
					}
					str text = FileSystem::ReadTextFile(Path(path), MemTag::Editor);
					if (text.empty()) {
						continue;
					}
					LinearAllocator arena(text.size() + 64, MemTag::Editor);
					JSONValue root2 = JSONParse(text, arena);
					str guid = str(root2["assetId"].AsString());
					if (guid.empty()) {
						continue;
					}
					++outCandidates;

					str subject = path.substr(0, path.size() - 5);
					auto existing = guidToPath.find(guid);
					if (existing != guidToPath.end()) {
						context.AddIssue(BuildIssueSeverity::Error, "",
										 "two assets have Guid " + guid + ": '" + existing->second + "' and '" +
											 subject + "'. Delete one .meta and let it be re-minted");
						return false;
					}
					guidToPath.emplace(std::move(guid), std::move(subject));
				}
				(void)label;
				return true;
			};

			for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
				if (!walk(context.projectRoot + "/" + str(contentRoot), str(contentRoot))) {
					return false;
				}
			}
			str engineRoot = ResolveEngineAssetsRoot();
			return engineRoot.empty() || walk(engineRoot, str("engine://"));
		}

		BuildStepStatus CookContentStep(BuildContext& context) {
			if (ResolveEngineAssetsRoot().empty()) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 "engine:// isn't mounted (built in assets can't be cooked)");
				return BuildStepStatus::Failed;
			}

			u32 candidates = 0;
			if (!EnumerateCandidates(context, candidates)) {
				return BuildStepStatus::Failed;
			}

			Vector<str> listed{GetMemoryResource(MemTag::Editor)};
			BuildPipeline::GetStagedScenes(*context.profile, listed);
			Vector<Guid> roots{GetMemoryResource(MemTag::Editor)};
			for (const str& relative : listed) {
				Guid guid = AssetDatabase::LookupGuid(Path("project://" + relative));
				if (!guid.IsValid()) {
					context.AddIssue(BuildIssueSeverity::Error, "scenes",
									 "'" + relative +
										 "' has no Guid (can't be cooked). Open the project in the editor once "
										 "to index it");
					return BuildStepStatus::Failed;
				}
				roots.push_back(guid);
			}

			const u32 fromSettings = SettingsOps::CollectSettingsAssetRoots(roots);
			if (fromSettings > 0) {
				context.Log("seeded " + std::to_string(fromSettings) + " asset(s) listed in the project's settings");
			}

			AssetDatabase::CollectGarbage();

			CookOps::CookClosure closure;
			str error;
			if (!CookOps::ComputeClosure(roots, closure, error)) {
				context.AddIssue(BuildIssueSeverity::Error, "", error);
				return BuildStepStatus::Failed;
			}

			CookOps::PakWriteDesc desc;
			desc.shipDebugNames = !ResolvedBool(context, "stripDebugNames");
			desc.compression = ResolveCompression(ResolvedString(context, "compressionMethod"));

			if (context.bootManifest.empty()) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 "no boot manifest reached the cook (the game wouldn't start). The "
								 "'boot-manifest' step writes it");
				return BuildStepStatus::Failed;
			}
			desc.documents.push_back({PakDocumentKind::BootManifest, context.bootManifest});
			if (str settings = ReadProjectSettingsDocument(); !settings.empty()) {
				desc.documents.push_back({PakDocumentKind::ProjectSettings, std::move(settings)});
			}
			Vector<u8> package{GetMemoryResource(MemTag::Editor)};
			CookOps::CookReport report;
			if (!CookOps::CookPackage(closure, desc, package, error, &report)) {
				context.AddIssue(BuildIssueSeverity::Error, "", "the cook failed: " + error);
				return BuildStepStatus::Failed;
			}

			str destination = context.stagingDir + "/Data";
			ClearStagedSubtree(destination);
			std::error_code ec;
			fs::create_directories(fs::path{destination}, ec);

			Buffer bytes(package.size(), MemTag::Editor);
			std::memcpy(bytes.data, package.data(), package.size());
			str packagePath = destination + "/Content.slpak";
			if (!FileSystem::WriteFile(Path(packagePath), bytes)) {
				context.AddIssue(BuildIssueSeverity::Error, "", "couldn't write '" + packagePath + "'");
				return BuildStepStatus::Failed;
			}

			if (!AssetDatabase::ValidatePackage(Path(packagePath), {closure.assets.data(), closure.assets.size()},
												error)) {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 "the cook produced a package this build cannot read: " + error);
				return BuildStepStatus::Failed;
			}

			ClearStagedSubtree(context.stagingDir + "/EngineAssets");
			for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
				ClearStagedSubtree(context.stagingDir + "/" + str(contentRoot));
			}
			std::filesystem::remove(fs::path{context.stagingDir + "/.assets.db"}, ec);

			std::filesystem::remove(fs::path{context.stagingDir + "/Boot.json"}, ec);
			if (str settingsName = Settings::GetDefaultPaths().project.GetFilename(); !settingsName.empty()) {
				std::filesystem::remove(fs::path{context.stagingDir + "/" + settingsName}, ec);
			}

			report.profileName = context.profile ? context.profile->name : str();
			report.packagePath = packagePath;
			report.candidateCount = candidates;
			str reportPath = BuildPipeline::GetCookReportPath(context.projectRoot, report.profileName);
			fs::create_directories(fs::path{reportPath}.parent_path(), ec);
			str reportJSON = CookOps::WriteReportJSON(report);
			Buffer reportBytes(reportJSON.size(), MemTag::Editor);
			std::memcpy(reportBytes.data, reportJSON.data(), reportJSON.size());
			if (!FileSystem::WriteFile(Path(reportPath), reportBytes)) {
				context.AddIssue(BuildIssueSeverity::Warning, "",
								 "the package was written but its build report was not: '" + reportPath + "'");
			}

			context.Log(std::format("cooked {} asset(s) of {} into Data/Content.slpak ({} byte(s), {} not reached)",
									closure.assets.size(), candidates, package.size(),
									candidates > closure.assets.size() ? candidates - closure.assets.size() : 0));
			return BuildStepStatus::Succeeded;
		}

	} // anonymous namespace

	void BuildStepRegistry::RegisterBuiltIns() {
		Register("validate", BuildPhase::Validate, &ValidateStep, nullptr);
		Register("build-number", BuildPhase::PreBuild, &BuildNumberStep, nullptr);
		Register("pre-build-command", BuildPhase::PreBuild, [](BuildContext& context) { return RunProfileCommand(context, "preBuildCommand", "preBuildCommand"); }, nullptr);
		Register("boot-manifest", BuildPhase::PreBuild, &BootManifestStep, nullptr);
		Register("configure", BuildPhase::Compile, &ConfigureStep, nullptr);
		Register("compile", BuildPhase::Compile, &CompileStep, nullptr);
		Register("cook-content", BuildPhase::Stage, &CookContentStep, nullptr);
		Register("stage-runtime", BuildPhase::Stage, &StageRuntimeStep, nullptr);
		Register("post-build-command", BuildPhase::PostBuild, [](BuildContext& context) { return RunProfileCommand(context, "postBuildCommand", "postBuildCommand"); }, nullptr);
	}

} // namespace Shalltear
