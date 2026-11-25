#include "TestsPCH.h"

#include "Checkout.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/WebPreviewServer.h"
#include "Shalltear/Network/HTTP.h"
#include "Shalltear/Network/Socket.h"
#include "Targets/WebTarget.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <thread>

using namespace Shalltear;

namespace {

	bool FetchLocal(const str& url, Net::HTTP::Response& outResponse) {
		bool done = false;
		Net::HTTP::Request req;
		req.url = url;
		req.timeoutSeconds = 5.0f;
		Net::HTTP::Send(std::move(req), [&](const Net::HTTP::Response& response) {
			outResponse = response;
			done = true;
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!done && std::chrono::steady_clock::now() < deadline) {
			Net::Dispatch();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return done;
	}

} // anonymous namespace

TEST(WebTargetTest, RegisteredWithExpectedSettingsSchema) {
	const IBuildTarget* target = BuildTargetRegistry::Find("Web");
	ASSERT_NE(target, nullptr);
	EXPECT_EQ(target->GetId(), "Web");

	Vector<BuildSettingDesc> schema{GetMemoryResource(MemTag::Editor)};
	target->DescribeSettings(schema);

	auto findKey = [&](std::string_view key) -> const BuildSettingDesc* {
		for (const BuildSettingDesc& desc : schema) {
			if (desc.key == key) {
				return &desc;
			}
		}
		return nullptr;
	};

	const BuildSettingDesc* threads = findKey("enableThreads");
	ASSERT_NE(threads, nullptr);
	EXPECT_EQ(threads->type, BuildSettingType::Bool);
	EXPECT_EQ(threads->defaultValue, "false");

	const BuildSettingDesc* mem = findKey("initialMemoryMB");
	ASSERT_NE(mem, nullptr);
	EXPECT_EQ(mem->type, BuildSettingType::Int);
	EXPECT_EQ(mem->defaultValue, "128");

	const BuildSettingDesc* growth = findKey("allowMemoryGrowth");
	ASSERT_NE(growth, nullptr);
	EXPECT_EQ(growth->defaultValue, "true");

	const BuildSettingDesc* validation = findKey("webgpuValidation");
	ASSERT_NE(validation, nullptr);
	EXPECT_EQ(validation->defaultValue, "false");

	const BuildSettingDesc* aotCompile = findKey("aotCompile");
	ASSERT_NE(aotCompile, nullptr);
	EXPECT_EQ(aotCompile->type, BuildSettingType::Bool);
	EXPECT_EQ(aotCompile->defaultValue, "true");

	const BuildSettingDesc* htmlTemplate = findKey("htmlTemplate");
	ASSERT_NE(htmlTemplate, nullptr);
	EXPECT_EQ(htmlTemplate->type, BuildSettingType::Path);
	EXPECT_EQ(htmlTemplate->merge, BuildMergePolicy::Replace);
	EXPECT_TRUE(htmlTemplate->defaultValue.empty());
}

TEST(WebTargetTest, RendersWebIndexTemplate) {
	const std::filesystem::path engineAssets = Checkout::Find("EngineAssets");
	ASSERT_FALSE(engineAssets.empty());
	FileSystem::Mount("engine", Path(engineAssets.generic_string()));

	str html;
	ASSERT_TRUE(WebTarget::RenderIndexHTML("Demo <Game>", "Demo.js", "Demo.wasm", true, html));
	EXPECT_NE(html.find("<title>Demo &lt;Game&gt;</title>"), str::npos);
	EXPECT_NE(html.find("const JS_FILE = \"Demo.js\";"), str::npos);
	EXPECT_NE(html.find("const THREADS_ENABLED = true;"), str::npos);
	EXPECT_NE(html.find("const HAS_MANAGED = false;"), str::npos);
	EXPECT_NE(html.find("preinitializedWebGPUDevice"), str::npos);
	EXPECT_NE(html.find("IDBFS"), str::npos);

	str managedHTML;
	ASSERT_TRUE(WebTarget::RenderIndexHTML("Demo <Game>", "Demo.js", "Demo.wasm", false, managedHTML, true));
	EXPECT_NE(managedHTML.find("const HAS_MANAGED = true;"), str::npos);
	EXPECT_NE(managedHTML.find("managed-manifest.json"), str::npos);

	FileSystem::Unmount("engine");
}

TEST(WebTargetTest, RendersCustomHTMLTemplateFile) {
	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / "shalltear_web_custom_template_test";
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot, ec);
	ASSERT_FALSE(ec);

	const str rootStr = tempRoot.generic_string();
	const str templatePath = rootStr + "/custom.html";
	ASSERT_TRUE(FileSystem::WriteTextFile(
		Path(templatePath),
		"<!doctype html><title>{{TITLE}}</title><script src=\"{{JS_FILE}}\"></script>"
		"<!-- {{WASM_FILE}} threads={{THREADS_ENABLED}} managed={{HAS_MANAGED}} -->"));

	str html;
	ASSERT_TRUE(WebTarget::RenderIndexHTML("Demo <Game> & \"Co\"", "App.js", "App.wasm", true, html, false,
										   templatePath));
	EXPECT_EQ(html, "<!doctype html><title>Demo &lt;Game&gt; &amp; &quot;Co&quot;</title>"
					"<script src=\"App.js\"></script>"
					"<!-- App.wasm threads=true managed=false -->");

	const str emptyPath = rootStr + "/empty.html";
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(emptyPath), ""));
	EXPECT_FALSE(WebTarget::RenderIndexHTML("Demo", "App.js", "App.wasm", false, html, false, emptyPath));
	EXPECT_FALSE(
		WebTarget::RenderIndexHTML("Demo", "App.js", "App.wasm", false, html, false, rootStr + "/missing.html"));

	std::filesystem::remove_all(tempRoot, ec);
}

TEST(WebTargetTest, ValidatesHTMLTemplateSetting) {
	const IBuildTarget* target = BuildTargetRegistry::Find("Web");
	ASSERT_NE(target, nullptr);

	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / "shalltear_web_validate_template_test";
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot / "Web" / "Template", ec);
	std::filesystem::create_directories(tempRoot / "Web" / "NoIndex", ec);
	std::filesystem::create_directories(tempRoot / "Web" / "EmptyIndex", ec);
	ASSERT_FALSE(ec);

	const str projectRoot = tempRoot.generic_string();
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/custom.html"), "<html>{{JS_FILE}}</html>"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/empty.html"), ""));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/index.html"), "<html>{{TITLE}}</html>"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/EmptyIndex/index.html"), ""));

	auto validateTemplate = [&](const str& value) {
		ResolvedProfile profile;
		profile.name = "Web";
		profile.target = "Web";
		ResolvedSetting setting;
		setting.key = "htmlTemplate";
		setting.type = BuildSettingType::Path;
		setting.value = value;
		profile.settings.push_back(std::move(setting));

		Vector<BuildIssue> allIssues{GetMemoryResource(MemTag::Editor)};
		target->Validate(projectRoot, profile, allIssues);

		Vector<BuildIssue> templateIssues{GetMemoryResource(MemTag::Editor)};
		for (const BuildIssue& issue : allIssues) {
			if (issue.key == "htmlTemplate") {
				templateIssues.push_back(issue);
			}
		}
		return templateIssues;
	};

	EXPECT_TRUE(validateTemplate("").empty());
	EXPECT_TRUE(validateTemplate("Web/custom.html").empty());
	EXPECT_TRUE(validateTemplate("Web/Template").empty());

	{
		const Vector<BuildIssue> issues = validateTemplate("Web/missing.html");
		ASSERT_EQ(issues.size(), 1u);
		EXPECT_EQ(issues[0].severity, BuildIssueSeverity::Error);
	}
	{
		const Vector<BuildIssue> issues = validateTemplate("Web/empty.html");
		ASSERT_EQ(issues.size(), 1u);
		EXPECT_EQ(issues[0].severity, BuildIssueSeverity::Error);
	}
	{
		const Vector<BuildIssue> issues = validateTemplate("Web/NoIndex");
		ASSERT_EQ(issues.size(), 1u);
		EXPECT_EQ(issues[0].severity, BuildIssueSeverity::Error);
	}
	{
		const Vector<BuildIssue> issues = validateTemplate("Web/EmptyIndex");
		ASSERT_EQ(issues.size(), 1u);
		EXPECT_EQ(issues[0].severity, BuildIssueSeverity::Error);
	}
	{
		const Vector<BuildIssue> issues = validateTemplate(projectRoot + "/Web/custom.html");
		ASSERT_EQ(issues.size(), 1u);
		EXPECT_EQ(issues[0].severity, BuildIssueSeverity::Warning);
	}

	std::filesystem::remove_all(tempRoot, ec);
}

TEST(WebTargetTest, StagesCustomHTMLTemplateFileAndDirectory) {
	const IBuildTarget* target = BuildTargetRegistry::Find("Web");
	ASSERT_NE(target, nullptr);

	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / "shalltear_web_stage_template_test";
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot / "Web" / "Template" / "assets", ec);
	std::filesystem::create_directories(tempRoot / "Web" / "Template" / "Data", ec);
	std::filesystem::create_directories(tempRoot / "Builds" / "Web" / "Data", ec);
	ASSERT_FALSE(ec);

	const str projectRoot = tempRoot.generic_string();
	const str stagingDir = (tempRoot / "Builds" / "Web").generic_string();

	ASSERT_TRUE(FileSystem::WriteTextFile(Path(stagingDir + "/App.js"), "js-output"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(stagingDir + "/App.wasm"), "wasm-output"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(stagingDir + "/Data/Content.slpak"), "pak-output"));

	ASSERT_TRUE(FileSystem::WriteTextFile(
		Path(projectRoot + "/Web/single.html"),
		"<html><title>{{TITLE}}</title><script src=\"{{JS_FILE}}\"></script>{{THREADS_ENABLED}}</html>"));
	ASSERT_TRUE(FileSystem::WriteTextFile(
		Path(projectRoot + "/Web/Template/index.html"),
		"<html><link rel=\"stylesheet\" href=\"styles.css\"><script src=\"{{JS_FILE}}\"></script>{{WASM_FILE}}</html>"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/styles.css"), "body { margin: 0; }"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/assets/icon.png"), "png-bytes"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/assets/icon.png.meta"), "meta-bytes"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/App.wasm"), "template-wasm-clobber"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/Web/Template/Data/Content.slpak"), "template-pak-clobber"));

	auto makeProfile = [](const str& htmlTemplate, const str& threads) {
		ResolvedProfile profile;
		profile.name = "Web";
		profile.target = "Web";
		ResolvedSetting tmpl;
		tmpl.key = "htmlTemplate";
		tmpl.type = BuildSettingType::Path;
		tmpl.value = htmlTemplate;
		profile.settings.push_back(std::move(tmpl));
		ResolvedSetting thr;
		thr.key = "enableThreads";
		thr.type = BuildSettingType::Bool;
		thr.value = threads;
		profile.settings.push_back(std::move(thr));
		return profile;
	};

	// single HTML file template
	{
		const ResolvedProfile profile = makeProfile("Web/single.html", "true");
		BuildContext context;
		context.target = target;
		context.profile = &profile;
		context.projectRoot = projectRoot;
		context.stagingDir = stagingDir;
		context.executableName = "App";

		ASSERT_EQ(WebTarget::StageStep(context), BuildStepStatus::Succeeded);
		const str staged = FileSystem::ReadTextFile(Path(stagingDir + "/index.html"), MemTag::Editor);
		EXPECT_EQ(staged, "<html><title>App</title><script src=\"App.js\"></script>true</html>");
	}

	// template folder with companion assets, .meta filtering, and build-output protection
	{
		const ResolvedProfile profile = makeProfile("Web/Template", "false");
		BuildContext context;
		context.target = target;
		context.profile = &profile;
		context.projectRoot = projectRoot;
		context.stagingDir = stagingDir;
		context.executableName = "App";

		ASSERT_EQ(WebTarget::StageStep(context), BuildStepStatus::Succeeded);
		EXPECT_EQ(FileSystem::ReadTextFile(Path(stagingDir + "/index.html"), MemTag::Editor),
				  "<html><link rel=\"stylesheet\" href=\"styles.css\"><script src=\"App.js\"></script>App.wasm</html>");
		EXPECT_EQ(FileSystem::ReadTextFile(Path(stagingDir + "/styles.css"), MemTag::Editor), "body { margin: 0; }");
		EXPECT_EQ(FileSystem::ReadTextFile(Path(stagingDir + "/assets/icon.png"), MemTag::Editor), "png-bytes");
		EXPECT_FALSE(FileSystem::Exists(Path(stagingDir + "/assets/icon.png.meta")));
		EXPECT_EQ(FileSystem::ReadTextFile(Path(stagingDir + "/App.wasm"), MemTag::Editor), "wasm-output");
		EXPECT_EQ(FileSystem::ReadTextFile(Path(stagingDir + "/Data/Content.slpak"), MemTag::Editor), "pak-output");

		u32 warningCount = 0;
		for (const BuildIssue& issue : context.GetIssues()) {
			if (issue.key == "htmlTemplate" && issue.severity == BuildIssueSeverity::Warning) {
				++warningCount;
			}
		}
		EXPECT_EQ(warningCount, 2u);
	}

	// template folder containing staging output directory must fail safely
	{
		ASSERT_TRUE(FileSystem::WriteTextFile(Path(projectRoot + "/index.html"), "<html>{{JS_FILE}}</html>"));
		const ResolvedProfile profile = makeProfile(".", "false");
		BuildContext context;
		context.target = target;
		context.profile = &profile;
		context.projectRoot = projectRoot;
		context.stagingDir = stagingDir;
		context.executableName = "App";

		EXPECT_EQ(WebTarget::StageStep(context), BuildStepStatus::Failed);
	}

	std::filesystem::remove_all(tempRoot, ec);
}

TEST(WebTargetTest, PreviewServerServesFilesMimeAndIsolationHeaders) {
	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / "shalltear_web_preview_test";
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot, ec);
	ASSERT_FALSE(ec);

	const str rootStr = tempRoot.generic_string();
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(rootStr + "/index.html"), "<html>ok</html>"));
	ASSERT_TRUE(FileSystem::WriteTextFile(Path(rootStr + "/game.wasm"), "wasm-bytes"));

	const str url = WebPreviewServer::Serve(rootStr, false);
	ASSERT_TRUE(url.starts_with("http://127.0.0.1:"));
	EXPECT_TRUE(WebPreviewServer::IsRunning());

	Net::HTTP::Response indexResp;
	ASSERT_TRUE(FetchLocal(url, indexResp));
	EXPECT_EQ(indexResp.result, Net::HTTP::Result::Ok);
	EXPECT_EQ(indexResp.status, 200u);
	EXPECT_EQ(indexResp.FindHeader("Content-Type"), "text/html; charset=utf-8");
	EXPECT_TRUE(indexResp.FindHeader("Cross-Origin-Opener-Policy").empty());

	Net::HTTP::Response wasmResp;
	ASSERT_TRUE(FetchLocal(url + "game.wasm", wasmResp));
	EXPECT_EQ(wasmResp.status, 200u);
	EXPECT_EQ(wasmResp.FindHeader("Content-Type"), "application/wasm");

	// Switch COOP/COEP isolation headers on (threaded mode)
	const str isolatedUrl = WebPreviewServer::Serve(rootStr, true);
	EXPECT_EQ(isolatedUrl, url);
	Net::HTTP::Response isoResp;
	ASSERT_TRUE(FetchLocal(url, isoResp));
	EXPECT_EQ(isoResp.status, 200u);
	EXPECT_EQ(isoResp.FindHeader("Cross-Origin-Opener-Policy"), "same-origin");
	EXPECT_EQ(isoResp.FindHeader("Cross-Origin-Embedder-Policy"), "require-corp");

	// Missing file -> 404
	Net::HTTP::Response missingResp;
	ASSERT_TRUE(FetchLocal(url + "missing.js", missingResp));
	EXPECT_EQ(missingResp.status, 404u);

	WebPreviewServer::Shutdown();
	EXPECT_FALSE(WebPreviewServer::IsRunning());
	std::filesystem::remove_all(tempRoot, ec);
}
