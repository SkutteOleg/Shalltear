#include "Targets/WindowsTarget.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "BuildIcon.h"
#include "Shalltear/Editor/ChildProcess.h"
#include "Platform/EditorPlatform.h"

namespace Shalltear {

	namespace {

		void AddIssue(Vector<BuildIssue>& out, BuildIssueSeverity severity, const str& profile, const char* key,
					  str message) {
			out.push_back(BuildIssue{severity, profile, str(key), std::move(message)});
		}

	} // anonymous namespace

	bool WindowsTarget::IsAvailable(str& outReason) const {
		if (EditorPlatform::GetHostPlatformName() != "Windows") {
			outReason = "a Windows player can only be built on Windows";
			return false;
		}
		outReason.clear();
		return true;
	}

	void WindowsTarget::DescribeSettings(Vector<BuildSettingDesc>& out) const {
		BuildSettingDesc symbols;
		symbols.key = "copyDebugSymbols";
		symbols.type = BuildSettingType::Bool;
		symbols.merge = BuildMergePolicy::Replace;
		symbols.label = "Copy Debug Symbols";
		symbols.help = "emit a .pdb beside the executable. A Debug configuration produces one either way";
		symbols.defaultValue = "false";
		out.push_back(std::move(symbols));

		BuildSettingDesc singleFile;
		singleFile.key = "singleFile";
		singleFile.type = BuildSettingType::Bool;
		singleFile.merge = BuildMergePolicy::Replace;
		singleFile.label = "Single-File Executable";
		singleFile.help = "link runtime libraries statically and embed Data/Content.slpak into the executable";
		singleFile.defaultValue = "false";
		out.push_back(std::move(singleFile));

		BuildIcon::DescribeSetting(out);
	}

	void WindowsTarget::Validate(const str& projectPath, const ResolvedProfile& profile, Vector<BuildIssue>& out) const {
		BuildProfileOps::StaticSDKProbe probe;
		BuildProfileOps::FindStaticSDKRoot(probe, Triple);
		if (probe.root.empty()) {
			AddIssue(out, BuildIssueSeverity::Error, profile.name, "",
					 "a Shalltear player links the game statically + no STATIC SDK was found. " + probe.fix);
		}
		BuildIcon::Validate(projectPath, profile, out);
	}

	void WindowsTarget::ConfigurePipeline(BuildContext& context) const {
		BuildProfileOps::StaticSDKProbe probe;
		BuildProfileOps::FindStaticSDKRoot(probe, Triple);
		context.sdkRoot = probe.root;
		if (!probe.root.empty()) {
			context.Log("static SDK: " + probe.root + " (" + probe.source + ")");
		}

		context.entrySource = "PlayerMain.cpp";
		context.windowedSubsystem = true;
		context.executableSuffix = ".exe";

		context.defines.push_back("SLT_PLAYER");

		context.runtimeFiles.push_back("libc++.dll");
		context.runtimeFiles.push_back("libunwind.dll");

		context.executableIconSizes = {16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256};
	}

	str WindowsTarget::GetLaunchCommand(const BuildContext& context) const {
		if (context.stagingDir.empty() || context.executableName.empty()) {
			return {};
		}
		return QuoteCommandArgument(context.stagingDir + "/" + context.executableName + ".exe");
	}

} // namespace Shalltear
