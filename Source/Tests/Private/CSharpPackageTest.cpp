#include "TestsPCH.h"

#include "Checkout.h"

#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;

#ifndef SHALLTEAR_SHARED
SLT_DECLARE_LINKED_PACKAGE(CSharp)
#endif

namespace {

	constexpr const char* PACKAGE_ID = "dev.shalltear.csharp";
	constexpr const char* PACKAGE_TARGET = "CSharp";

	std::filesystem::path ManagedDirectory() {
		return std::filesystem::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
	}

	str ReadText(const std::filesystem::path& file) {
		return FileSystem::ReadTextFile(Path(file.generic_string()));
	}

	class CSharpPackageFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_csharp_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
		}

		void TearDown() override {
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}
	};

} // namespace

TEST(CSharpManifestTest, BuiltInAndOffUntilEnabled) {
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);

	const PackageOps::PackageManifest* csharp = PackageOps::FindPackage(catalogue, PACKAGE_ID);
	ASSERT_NE(csharp, nullptr);
	EXPECT_EQ(csharp->kind, PackageOps::PackageKind::Engine);
	EXPECT_EQ(csharp->displayName, "C# Scripting");
	EXPECT_FALSE(csharp->scaffold);
	EXPECT_EQ(PackageOps::GetPackageBinaryName(*csharp), PACKAGE_TARGET);

	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, {}, resolution);
	EXPECT_FALSE(PackageOps::IsEnabled(resolution, PACKAGE_ID));

	const str stored[] = {str(PACKAGE_ID)};
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
	EXPECT_TRUE(PackageOps::IsEnabled(resolution, PACKAGE_ID));

	Vector<str> playerPackages;
	PackageOps::SelectGamePackages(catalogue, resolution, playerPackages);
	EXPECT_NE(std::find(playerPackages.begin(), playerPackages.end(), str(PACKAGE_ID)), playerPackages.end());
}

TEST_F(CSharpPackageFixture, NewProjectLeavesItOut) {
	const Path document{(_root / "ProjectSettings.ini").generic_string()};
	ASSERT_GT(PackageOps::ScaffoldEnabledSet(document), 0u);

	const str written = FileSystem::ReadTextFile(document);
	EXPECT_EQ(written.find(PACKAGE_ID), str::npos) << written;
	EXPECT_NE(written.find("dev.shalltear.physics3d"), str::npos) << written;
}

TEST(CSharpPackageTest, LoadsAndUnloads) {
	ModuleSystem::Initialize();
#ifdef SHALLTEAR_SHARED
	EXPECT_EQ(AssetImporterRegistry::Find(StringHash("CSharpScriptAsset")), nullptr);
	ModuleHandle package = ModuleSystem::Load(Path(str(PACKAGE_TARGET) + SLT_TEST_DLL_SUFFIX));
	EXPECT_TRUE(ModuleSystem::IsLoaded(package));
	EXPECT_NE(AssetImporterRegistry::Find(StringHash("CSharpScriptAsset")), nullptr);
	EXPECT_EQ(AssetImporterRegistry::FindByExtension(".cs"), AssetImporterRegistry::Find(StringHash("CSharpScriptAsset")));
	ModuleSystem::Unload(package);
	EXPECT_FALSE(ModuleSystem::IsLoaded(package));
	EXPECT_EQ(AssetImporterRegistry::Find(StringHash("CSharpScriptAsset")), nullptr);
#else
	char token = 0;
	SLT_CALL_LINKED_PACKAGE(CSharp, &token);
	EXPECT_EQ(FrameStages::UnregisterOwnedBy(&token), 1u);
#endif
	ModuleSystem::Terminate();
}

TEST(CSharpPackageTest, ManagedOutputsExist) {
	const std::filesystem::path managed = ManagedDirectory();
	for (const char* name : {"Shalltear.Runtime.dll", "Shalltear.Runtime.xml", "Shalltear.Generators.dll", "Shalltear.Conformance.dll"}) {
		std::error_code ec;
		EXPECT_GT(std::filesystem::file_size(managed / name, ec), 0u) << name;
	}

	const std::filesystem::path pinFile = Checkout::Find("Source/CSharp/DotNetPin.json");
	ASSERT_FALSE(pinFile.empty());
	const str pinText = ReadText(pinFile);
	const str configText = ReadText(managed / "Shalltear.Runtime.runtimeconfig.json");
	ASSERT_FALSE(configText.empty());

	LinearAllocator arena(2 * (pinText.size() + configText.size()) + 1024, MemTag::Editor);
	const JSONValue pin = JSONParse(pinText, arena);
	const JSONValue config = JSONParse(configText, arena);
	const JSONValue& options = config["runtimeOptions"];
	ASSERT_FALSE(pin["runtimeVersion"].AsString().empty());
	EXPECT_EQ(options["framework"]["name"].AsString(), "Microsoft.NETCore.App");
	EXPECT_EQ(options["framework"]["version"].AsString(), pin["runtimeVersion"].AsString());
	EXPECT_EQ(options["tfm"].AsString(), pin["targetFramework"].AsString());
	EXPECT_EQ(options["rollForward"].AsString(), "Disable");
}
