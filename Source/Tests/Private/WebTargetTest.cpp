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

	str managedHtml;
	ASSERT_TRUE(WebTarget::RenderIndexHTML("Demo <Game>", "Demo.js", "Demo.wasm", false, managedHtml, true));
	EXPECT_NE(managedHtml.find("const HAS_MANAGED = true;"), str::npos);
	EXPECT_NE(managedHtml.find("managed-manifest.json"), str::npos);

	FileSystem::Unmount("engine");
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
