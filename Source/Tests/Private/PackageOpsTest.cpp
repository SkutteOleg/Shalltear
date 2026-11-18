#include <gtest/gtest.h>

#include "TestPlatform.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/SettingsOps.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Shalltear;

namespace {

	PackageOps::PackageManifest Make(const char* name, PackageOps::PackageKind kind, std::initializer_list<const char*> dependencies = {}) {
		PackageOps::PackageManifest manifest;
		manifest.name = name;
		manifest.version = "1.0.0";
		manifest.kind = kind;
		for (const char* dependency : dependencies) {
			manifest.dependencies.push_back(PackageOps::PackageDependency{str{dependency}, str{"1.0.0"}});
		}
		return manifest;
	}

	PackageOps::PackageManifest MakeEngine(const char* name, std::initializer_list<const char*> dependencies = {}) {
		return Make(name, PackageOps::PackageKind::Engine, dependencies);
	}

	PackageOps::PackageManifest MakeRequiredTool(const char* name, std::initializer_list<const char*> dependencies = {}) {
		PackageOps::PackageManifest manifest = Make(name, PackageOps::PackageKind::Tool, dependencies);
		manifest.required = true;
		return manifest;
	}

	Vector<str> Names(std::span<const PackageOps::ResolvedPackage> resolution) {
		Vector<str> out;
		for (const PackageOps::ResolvedPackage& entry : resolution) {
			out.emplace_back(entry.name);
		}
		return out;
	}

	const PackageOps::ResolvedPackage* Find(std::span<const PackageOps::ResolvedPackage> resolution, std::string_view name) {
		for (const PackageOps::ResolvedPackage& entry : resolution) {
			if (entry.name == name) {
				return &entry;
			}
		}
		return nullptr;
	}

	str Join(std::span<const str> values) {
		str out;
		for (const str& value : values) {
			if (!out.empty()) {
				out += ",";
			}
			out += value;
		}
		return out;
	}

	class PackageDiskFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_packages_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
		}

		void TearDown() override {
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WritePackage(const char* directory, const char* manifestJSON) {
			std::filesystem::create_directories(_root / directory);
			FileSystem::WriteTextFile(Path{(_root / directory / "package.json").generic_string()}, str{manifestJSON});
		}

		Path Root() const { return Path{_root.generic_string()}; }
	};

	class EnabledSetFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		SettingsPaths _paths;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_packageset_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			_paths.project = Path((_root / "ProjectSettings.ini").generic_string());
			_paths.user = Path((_root / "UserSettings.ini").generic_string());
			_paths.userProject = Path((_root / "UserProjectSettings.ini").generic_string());
			Settings::Initialize(_paths);
		}

		void TearDown() override {
			Settings::Terminate();
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}
	};

} // namespace

TEST(PackageManifestTest, MinimalManifestCarries2RequiredFields) {
	PackageOps::PackageManifest manifest;
	str error;
	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"com.acme.physics","version":"1.2.3"})", manifest, error)) << error;
	EXPECT_EQ(manifest.name, "com.acme.physics");
	EXPECT_EQ(manifest.version, "1.2.3");
	EXPECT_EQ(manifest.kind, PackageOps::PackageKind::Package);
	EXPECT_TRUE(manifest.dependencies.empty());

	PackageOps::PackageManifest wrongSchema;
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"id":"com.acme.physics","version":"1.2.3"})", wrongSchema, error));
}

TEST(PackageManifestTest, OptionalFieldsAndDefaultKind) {
	PackageOps::PackageManifest manifest;
	str error;
	ASSERT_TRUE(PackageOps::ParseManifest(
		R"({"name":"com.acme.p","version":"1.0.0","displayName":"Physics","description":"d","kind":"engine"})",
		manifest, error))
		<< error;
	EXPECT_EQ(manifest.displayName, "Physics");
	EXPECT_EQ(manifest.description, "d");
	EXPECT_EQ(manifest.kind, PackageOps::PackageKind::Engine);
}

TEST(PackageManifestTest, StageMustBeCoreStage) {
	PackageOps::PackageManifest manifest;
	str error;

	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0"})", manifest, error)) << error;
	EXPECT_FALSE(manifest.stage.has_value());

	ASSERT_TRUE(PackageOps::ParseManifest(
		R"({"name":"a","version":"1.0.0","stage":"FixedUpdate"})", manifest, error))
		<< error;
	ASSERT_TRUE(manifest.stage.has_value());
	EXPECT_EQ(*manifest.stage, FrameStage::FixedUpdate);

	for (FrameStage stage : FRAME_STAGES) {
		str document = R"({"name":"a","version":"1.0.0","stage":")";
		document += GetFrameStageName(stage);
		document += R"("})";
		PackageOps::PackageManifest each;
		ASSERT_TRUE(PackageOps::ParseManifest(document, each, error)) << GetFrameStageName(stage) << ": " << error;
		ASSERT_TRUE(each.stage.has_value());
		EXPECT_EQ(*each.stage, stage);
	}
}

TEST(PackageManifestTest, UnknownStageRefused) {
	PackageOps::PackageManifest manifest;
	str error;

	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","stage":"PostUpdate"})", manifest, error));
	EXPECT_NE(error.find("Update"), str::npos) << error;
	EXPECT_NE(error.find("Render"), str::npos) << error;

	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","stage":"update"})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","stage":1})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","stage":""})", manifest, error));
}

TEST(PackageManifestTest, DependencyRangeRefused) {
	PackageOps::PackageManifest manifest;
	str error;
	ASSERT_TRUE(PackageOps::ParseManifest(
		R"({"name":"a","version":"1.0.0","dependencies":{"b":"2.0.1"}})", manifest, error))
		<< error;
	ASSERT_EQ(manifest.dependencies.size(), 1u);
	EXPECT_EQ(manifest.dependencies[0].name, "b");
	EXPECT_EQ(manifest.dependencies[0].version, "2.0.1");

	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","dependencies":{"b":"^2.0.1"}})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0"})", manifest, error));
}

TEST(PackageManifestTest, CorruptingNameRefused) {
	PackageOps::PackageManifest manifest;
	str error;
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"com.acme;evil","version":"1.0.0"})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"com acme","version":"1.0.0"})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"COM.ACME","version":"1.0.0"})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"","version":"1.0.0"})", manifest, error));

	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","dependencies":{"b;c":"1.0.0"}})", manifest, error));
}

TEST(PackageManifestTest, GarbageAndSelfDependencyRefused) {
	PackageOps::PackageManifest manifest;
	str error;
	EXPECT_FALSE(PackageOps::ParseManifest("not json at all", manifest, error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(PackageOps::ParseManifest(R"([1,2,3])", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","kind":"widget"})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","dependencies":["b"]})", manifest, error));
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"a","version":"1.0.0","dependencies":{"a":"1.0.0"}})", manifest, error));
}

TEST(PackageManifestTest, OnlyToolMayBeRequired) {
	PackageOps::PackageManifest manifest;
	str error;
	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"com.acme.graphs","version":"1.0.0","kind":"tool","required":true})", manifest, error)) << error;
	EXPECT_TRUE(manifest.required);
	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"com.acme.slshader","version":"1.0.0","kind":"tool"})", manifest, error)) << error;
	EXPECT_FALSE(manifest.required);

	error.clear();
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"com.acme.physics","version":"1.0.0","kind":"engine","required":true})", manifest, error));
	EXPECT_NE(error.find("only a 'tool' may be 'required'"), str::npos) << error;
	error.clear();
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"com.acme.graphs","version":"1.0.0","kind":"tool","required":"yes"})", manifest, error));
	EXPECT_NE(error.find("'required' must be true or false"), str::npos) << error;
}

TEST(PackageManifestTest, ScaffoldMustBeTrueOrFalse) {
	PackageOps::PackageManifest manifest;
	str error;
	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"com.acme.physics","version":"1.0.0","kind":"engine"})", manifest, error)) << error;
	EXPECT_TRUE(manifest.scaffold);
	ASSERT_TRUE(PackageOps::ParseManifest(R"({"name":"com.acme.scripting","version":"1.0.0","kind":"engine","scaffold":false})", manifest, error)) << error;
	EXPECT_FALSE(manifest.scaffold);

	error.clear();
	EXPECT_FALSE(PackageOps::ParseManifest(R"({"name":"com.acme.scripting","version":"1.0.0","kind":"engine","scaffold":"no"})", manifest, error));
	EXPECT_NE(error.find("'scaffold' must be true or false"), str::npos) << error;
}

TEST_F(PackageDiskFixture, DiscoveryReadsManifestsOnly) {
	WritePackage("physics", R"({"name":"com.acme.physics","version":"1.0.0","kind":"engine"})");
	WritePackage("animation", R"({"name":"com.acme.animation","version":"1.0.0","kind":"engine"})");
	std::filesystem::create_directories(_root / "not-a-package");

	Path roots[] = {Root()};
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::DiscoverPackages(roots, catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 2u);
	EXPECT_TRUE(PackageOps::FindPackage(catalogue, "com.acme.physics") != nullptr);
	EXPECT_TRUE(PackageOps::FindPackage(catalogue, "com.acme.animation") != nullptr);
	EXPECT_TRUE(diagnostics.empty()) << Join(diagnostics);

	EXPECT_NE(PackageOps::FindPackage(catalogue, "com.acme.physics")->directory.ToString().find("physics"), str::npos);
}

TEST_F(PackageDiskFixture, MalformedManifestIsDiagnostic) {
	WritePackage("good", R"({"name":"com.acme.good","version":"1.0.0"})");
	WritePackage("bad", R"({"name":"com.acme.bad","version":"nope"})");

	Path roots[] = {Root(), Path{(_root / "does-not-exist").generic_string()}};
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::DiscoverPackages(roots, catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].name, "com.acme.good");
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_NE(diagnostics[0].find("bad"), str::npos);
}

TEST_F(PackageDiskFixture, DuplicateNameKeepsFirst) {
	WritePackage("a/physics", R"({"name":"com.acme.physics","version":"1.0.0"})");
	WritePackage("b/physics", R"({"name":"com.acme.physics","version":"2.0.0"})");

	Path roots[] = {Path{(_root / "a").generic_string()}, Path{(_root / "b").generic_string()}};
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::DiscoverPackages(roots, catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].version, "1.0.0");
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_NE(diagnostics[0].find("twice"), str::npos);
}

TEST(PackageCatalogueTest, DependencyOnSomethingAbsentDropsPackage) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("a", {"missing"}));
	catalogue.push_back(MakeEngine("b"));

	Vector<str> diagnostics;
	PackageOps::ValidateCatalogue(catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].name, "b");
	ASSERT_EQ(diagnostics.size(), 1u);
	EXPECT_NE(diagnostics[0].find("missing"), str::npos);
}

TEST(PackageCatalogueTest, DropCascadesToDependents) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("a", {"b"}));
	catalogue.push_back(MakeEngine("b", {"missing"}));
	catalogue.push_back(MakeEngine("c"));

	Vector<str> diagnostics;
	PackageOps::ValidateCatalogue(catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].name, "c");
	EXPECT_EQ(diagnostics.size(), 2u) << Join(diagnostics);
}

TEST(PackageCatalogueTest, CycleRefusedWithMembers) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("a", {"b"}));
	catalogue.push_back(MakeEngine("b", {"a"}));
	catalogue.push_back(MakeEngine("safe"));

	Vector<str> diagnostics;
	PackageOps::ValidateCatalogue(catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].name, "safe");
	EXPECT_EQ(diagnostics.size(), 2u) << Join(diagnostics);
}

TEST(PackageCatalogueTest, ThreeCycleTakesDependents) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("a", {"b"}));
	catalogue.push_back(MakeEngine("b", {"c"}));
	catalogue.push_back(MakeEngine("c", {"a"}));
	catalogue.push_back(MakeEngine("hanger", {"a"}));
	catalogue.push_back(MakeEngine("safe"));

	Vector<str> diagnostics;
	PackageOps::ValidateCatalogue(catalogue, diagnostics);

	ASSERT_EQ(catalogue.size(), 1u);
	EXPECT_EQ(catalogue[0].name, "safe");
}

TEST(PackageCatalogueTest, AValidDiamondSurvivesValidationUntouched) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("top", {"left", "right"}));
	catalogue.push_back(MakeEngine("left", {"base"}));
	catalogue.push_back(MakeEngine("right", {"base"}));
	catalogue.push_back(MakeEngine("base"));

	Vector<str> diagnostics;
	PackageOps::ValidateCatalogue(catalogue, diagnostics);

	EXPECT_EQ(catalogue.size(), 4u);
	EXPECT_TRUE(diagnostics.empty()) << Join(diagnostics);
}

TEST(PackageResolveTest, EnablingADependentAutomaticallyEnablesItsDependencies) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("top", {"left", "right"}));
	catalogue.push_back(MakeEngine("left", {"base"}));
	catalogue.push_back(MakeEngine("right", {"base"}));
	catalogue.push_back(MakeEngine("base"));
	catalogue.push_back(MakeEngine("unrelated"));

	str stored[] = {str{"top"}};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "base,left,right,top");

	EXPECT_TRUE(Find(resolution, "top")->requested);
	EXPECT_FALSE(Find(resolution, "base")->requested);
	EXPECT_EQ(Join(Find(resolution, "base")->requiredBy), "left,right");
	EXPECT_EQ(Join(Find(resolution, "top")->requiredBy), "");
}

TEST(PackageResolveTest, DependentBlocksDisable) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics"));
	catalogue.push_back(MakeEngine("vehicles", {"physics"}));

	Vector<str> blockers;
	{
		str stored[] = {str{"vehicles"}};
		Vector<PackageOps::ResolvedPackage> resolution;
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		EXPECT_FALSE(PackageOps::CanDisable(resolution, "physics", blockers));
		EXPECT_EQ(Join(blockers), "vehicles");
		EXPECT_TRUE(PackageOps::CanDisable(resolution, "vehicles", blockers));
		EXPECT_TRUE(blockers.empty());
	}
	{
		str stored[] = {str{"physics"}};
		Vector<PackageOps::ResolvedPackage> resolution;
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		EXPECT_TRUE(PackageOps::CanDisable(resolution, "physics", blockers));
		EXPECT_TRUE(blockers.empty());
	}
}

TEST(PackageResolveTest, BothDependentsBlock) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics"));
	catalogue.push_back(MakeEngine("vehicles", {"physics"}));
	catalogue.push_back(MakeEngine("ragdoll", {"physics"}));

	Vector<str> blockers;
	{
		str stored[] = {str{"vehicles"}, str{"ragdoll"}};
		Vector<PackageOps::ResolvedPackage> resolution;
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		EXPECT_FALSE(PackageOps::CanDisable(resolution, "physics", blockers));
		EXPECT_EQ(Join(blockers), "ragdoll,vehicles");
	}
	{
		str stored[] = {str{"ragdoll"}};
		Vector<PackageOps::ResolvedPackage> resolution;
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		EXPECT_FALSE(PackageOps::CanDisable(resolution, "physics", blockers));
		EXPECT_EQ(Join(blockers), "ragdoll");
	}
}

TEST(PackageResolveTest, RequiredKindsAlwaysEnabled) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("importers"));
	catalogue.push_back(Make("theproject", PackageOps::PackageKind::Game));
	catalogue.push_back(MakeEngine("physics"));

	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, {}, resolution);

	EXPECT_EQ(Join(Names(resolution)), "importers,theproject");
	EXPECT_TRUE(Find(resolution, "importers")->alwaysOn);
	EXPECT_FALSE(PackageOps::IsEnabled(resolution, "physics"));

	Vector<str> blockers;
	EXPECT_FALSE(PackageOps::CanDisable(resolution, "importers", blockers));
	EXPECT_EQ(Join(blockers), "the engine");
}

TEST(PackageResolveTest, RequiredToolPullsDependencies) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("importers", {"imageio"}));
	catalogue.push_back(MakeEngine("imageio"));

	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, {}, resolution);

	EXPECT_EQ(Join(Names(resolution)), "imageio,importers");
	EXPECT_FALSE(Find(resolution, "imageio")->alwaysOn);
	Vector<str> blockers;
	EXPECT_FALSE(PackageOps::CanDisable(resolution, "imageio", blockers));
	EXPECT_EQ(Join(blockers), "importers");
}

TEST(PackageResolveTest, OptionalToolSwitchesLikeFeature) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("graphs"));
	catalogue.push_back(Make("nodeshader", PackageOps::PackageKind::Tool, {"graphs"}));
	catalogue.push_back(MakeEngine("visualeffects", {"graphs"}));

	Vector<PackageOps::ResolvedPackage> resolution;
	const str effectsOnly[] = {str("visualeffects")};
	PackageOps::ResolveEnabledSet(catalogue, effectsOnly, resolution);
	EXPECT_EQ(Join(Names(resolution)), "graphs,visualeffects");

	const str both[] = {str("nodeshader"), str("visualeffects")};
	PackageOps::ResolveEnabledSet(catalogue, both, resolution);
	EXPECT_EQ(Join(Names(resolution)), "graphs,nodeshader,visualeffects");
	EXPECT_FALSE(Find(resolution, "nodeshader")->alwaysOn);
	EXPECT_TRUE(Find(resolution, "nodeshader")->requested);
	Vector<str> blockers;
	EXPECT_TRUE(PackageOps::CanDisable(resolution, "nodeshader", blockers)) << Join(blockers);
	EXPECT_FALSE(PackageOps::CanDisable(resolution, "graphs", blockers));
	EXPECT_EQ(Join(blockers), "the engine");

	PackageOps::ResolveEnabledSet(catalogue, {}, resolution);
	EXPECT_EQ(Join(Names(resolution)), "graphs");
}

TEST(PackageResolveTest, UnknownStoredNameDropped) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics"));

	str stored[] = {str{"physics"}, str{"com.gone.away"}};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "physics");
	EXPECT_FALSE(PackageOps::IsEnabled(resolution, "com.gone.away"));
}

TEST(PackageResolveTest, EditedSetGetsDependencies) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics"));
	catalogue.push_back(MakeEngine("vehicles", {"physics"}));

	str stored[] = {str{"vehicles"}};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "physics,vehicles");
	EXPECT_FALSE(Find(resolution, "physics")->requested);
}

TEST(PackageResolveTest, IndependentPackagesToggleApart) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics2d"));
	catalogue.push_back(MakeEngine("physics3d"));

	str stored[] = {str{"physics2d"}, str{"physics3d"}};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "physics2d,physics3d");
	Vector<str> blockers;
	EXPECT_TRUE(PackageOps::CanDisable(resolution, "physics2d", blockers));
	EXPECT_TRUE(PackageOps::CanDisable(resolution, "physics3d", blockers));
}

TEST(PackageResolveTest, EmptyResolvesEmpty) {
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet({}, {}, resolution);
	EXPECT_TRUE(resolution.empty());

	Vector<str> blockers;
	EXPECT_TRUE(PackageOps::CanDisable(resolution, "anything", blockers));
	EXPECT_TRUE(blockers.empty());
}

TEST(PackagePlayerLinkTest, ToolNeverOnPlayerLink) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("importers"));
	catalogue.push_back(Make("nodeshader", PackageOps::PackageKind::Tool));
	catalogue.push_back(Make("theproject", PackageOps::PackageKind::Game));
	catalogue.push_back(MakeEngine("physics"));

	Vector<PackageOps::ResolvedPackage> resolution;
	const str stored[] = {str("physics"), str("nodeshader")};
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "importers,nodeshader,physics,theproject");

	Vector<str> linked;
	PackageOps::SelectGamePackages(catalogue, resolution, linked);

	EXPECT_EQ(Join(linked), "physics");
}

TEST(PackagePlayerLinkTest, LinkFollowsResolvedSet) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(Make("vehicles", PackageOps::PackageKind::Package, {"physics"}));
	catalogue.push_back(MakeEngine("physics"));
	catalogue.push_back(MakeEngine("navigation"));

	Vector<PackageOps::ResolvedPackage> resolution;
	const str stored[] = {str("vehicles")};
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	Vector<str> linked;
	PackageOps::SelectGamePackages(catalogue, resolution, linked);

	EXPECT_EQ(Join(linked), "physics,vehicles");
	EXPECT_EQ(std::count(linked.begin(), linked.end(), str("navigation")), 0);
}

TEST(PackagePlayerLinkTest, APlayerEnablingNothingLinksNothing) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("physics"));

	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, {}, resolution);

	Vector<str> linked;
	PackageOps::SelectGamePackages(catalogue, resolution, linked);
	EXPECT_TRUE(linked.empty());
}

TEST(PackageCatalogueTest, CatalogueFindsStagedSample) {
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);

	const PackageOps::PackageManifest* sample = PackageOps::FindPackage(catalogue, "dev.shalltear.samplepackage");
	ASSERT_NE(sample, nullptr);

	EXPECT_EQ(sample->kind, PackageOps::PackageKind::Package);
	EXPECT_EQ(sample->version, "1.0.0");

	ASSERT_TRUE(sample->stage.has_value());
	EXPECT_EQ(*sample->stage, FrameStage::Update);

	EXPECT_EQ(PackageOps::GetPackageBinaryName(*sample), "SamplePackage");
}

TEST(PackageCatalogueTest, EffectsResolveWithoutNodeShader) {
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
	const PackageOps::PackageManifest* pipeline = PackageOps::FindPackage(catalogue, "dev.shalltear.importpipeline");
	const PackageOps::PackageManifest* graphs = PackageOps::FindPackage(catalogue, "dev.shalltear.graphs");
	const PackageOps::PackageManifest* nodeShader = PackageOps::FindPackage(catalogue, "dev.shalltear.nodeshader");
	const PackageOps::PackageManifest* effects = PackageOps::FindPackage(catalogue, "dev.shalltear.visualeffects");
	ASSERT_NE(pipeline, nullptr);
	ASSERT_NE(graphs, nullptr);
	ASSERT_NE(nodeShader, nullptr);
	ASSERT_NE(effects, nullptr);
	EXPECT_TRUE(pipeline->required);
	EXPECT_TRUE(graphs->required);
	EXPECT_EQ(nodeShader->kind, PackageOps::PackageKind::Tool);
	EXPECT_FALSE(nodeShader->required);

	const str stored[] = {str("dev.shalltear.visualeffects")};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
	EXPECT_TRUE(PackageOps::IsEnabled(resolution, "dev.shalltear.visualeffects"));
	EXPECT_TRUE(PackageOps::IsEnabled(resolution, "dev.shalltear.graphs"));
	EXPECT_FALSE(PackageOps::IsEnabled(resolution, "dev.shalltear.nodeshader"));
}

TEST_F(PackageDiskFixture, ScaffoldingWritesBuiltInFeatures) {
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
	ASSERT_FALSE(catalogue.empty());

	auto switchable = [](const PackageOps::PackageManifest& manifest) {
		return manifest.scaffold &&
			   (manifest.kind == PackageOps::PackageKind::Engine || (manifest.kind == PackageOps::PackageKind::Tool && !manifest.required));
	};
	u32 features = 0;
	u32 optionalTools = 0;
	for (const PackageOps::PackageManifest& manifest : catalogue) {
		features += switchable(manifest) ? 1u : 0u;
		optionalTools += manifest.kind == PackageOps::PackageKind::Tool && !manifest.required ? 1u : 0u;
	}
	ASSERT_GT(features, 0u);
	ASSERT_GT(optionalTools, 0u);

	const Path document{(_root / "ProjectSettings.ini").generic_string()};
	EXPECT_EQ(PackageOps::ScaffoldEnabledSet(document), features);

	const str written = FileSystem::ReadTextFile(document);
	Config reread;
	ASSERT_TRUE(reread.Load(document));
	EXPECT_EQ(reread.GetI64("Packages", "format", 0), 2);
	for (const PackageOps::PackageManifest& manifest : catalogue) {
		const bool named = written.find(manifest.name + ";") != str::npos || written.find(manifest.name + "\n") != str::npos;
		if (switchable(manifest)) {
			EXPECT_TRUE(named);
		} else {
			EXPECT_FALSE(named) << manifest.name << " is a " << PackageOps::KindName(manifest.kind) << ' ' << written;
		}
	}
}

TEST_F(PackageDiskFixture, ScaffoldingKeepsWhatTheDocumentAlreadyHeld) {
	const Path document{(_root / "ProjectSettings.ini").generic_string()};
	Config existing;
	existing.SetI64("Graphics", "qualityLevel", 2);
	ASSERT_TRUE(existing.Save(document));

	ASSERT_GT(PackageOps::ScaffoldEnabledSet(document), 0u);

	Config reread;
	ASSERT_TRUE(reread.Load(document));
	EXPECT_EQ(reread.GetI64("Graphics", "qualityLevel", -1), 2);
}

TEST_F(PackageDiskFixture, ScaffoldSkipsPackageThatOptsOut) {
	WritePackage("physics", R"({"name":"com.acme.physics","version":"1.0.0","kind":"engine"})");
	WritePackage("scripting", R"({"name":"com.acme.scripting","version":"1.0.0","kind":"engine","scaffold":false})");
	WritePackage("shaders", R"({"name":"com.acme.shaders","version":"1.0.0","kind":"tool","scaffold":false})");
	WritePackage("effects", R"({"name":"com.acme.effects","version":"1.0.0","kind":"tool"})");

	Path roots[] = {Root()};
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::DiscoverPackages(roots, catalogue, diagnostics);
	ASSERT_EQ(catalogue.size(), 4u) << Join(diagnostics);

	const Path document{(_root / "ProjectSettings.ini").generic_string()};
	EXPECT_EQ(PackageOps::ScaffoldEnabledSet(document, catalogue), 2u);

	Config reread;
	ASSERT_TRUE(reread.Load(document));
	EXPECT_EQ(reread.GetString("Packages", "enabled"), "com.acme.effects;com.acme.physics");
}

TEST(PackagePlayerLinkTest, BinaryNameIsDirectoryName) {
	PackageOps::PackageManifest manifest;
	manifest.name = "dev.shalltear.samplepackage";
	manifest.directory = Path("C:/build/bin/Packages/SamplePackage");
	EXPECT_EQ(PackageOps::GetPackageBinaryName(manifest), "SamplePackage");

	PackageOps::PackageManifest parsedOnly;
	parsedOnly.name = "dev.shalltear.other";
	EXPECT_TRUE(PackageOps::GetPackageBinaryName(parsedOnly).empty());
}

TEST_F(EnabledSetFixture, EnabledSetRoundTrips) {
	str names[] = {str{"com.acme.physics"}, str{"com.acme.animation"}};
	PackageOps::WriteEnabledSet(names);

	Vector<str> read;
	PackageOps::ReadEnabledSet(read);
	EXPECT_EQ(Join(read), "com.acme.animation,com.acme.physics");

	Settings::Terminate();
	Settings::Initialize(_paths);
	PackageOps::ReadEnabledSet(read);
	EXPECT_EQ(Join(read), "com.acme.animation,com.acme.physics");
}

TEST_F(EnabledSetFixture, UntouchedProjectEnablesNothingAndStoresNothing) {
	Vector<str> read;
	PackageOps::ReadEnabledSet(read);
	EXPECT_TRUE(read.empty());

	PackageOps::DeclareSettings();
	Settings::Save();
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project).find("Packages"), str::npos);

	str names[] = {str{"a"}};
	PackageOps::WriteEnabledSet(names);
	str written = FileSystem::ReadTextFile(_paths.project);
	ASSERT_NE(written.find("Packages"), str::npos) << written;

	PackageOps::WriteEnabledSet({});
	str reverted = FileSystem::ReadTextFile(_paths.project);
	EXPECT_EQ(reverted.find("enabled"), str::npos) << reverted;

	PackageOps::ReadEnabledSet(read);
	EXPECT_TRUE(read.empty());
}

TEST_F(EnabledSetFixture, DuplicatesCollapse) {
	str names[] = {str{"b"}, str{"a"}, str{"b"}};
	PackageOps::WriteEnabledSet(names);

	Vector<str> read;
	PackageOps::ReadEnabledSet(read);
	EXPECT_EQ(Join(read), "a,b");

	const str before = FileSystem::ReadTextFile(_paths.project);
	str reordered[] = {str{"b"}, str{"a"}};
	PackageOps::WriteEnabledSet(reordered);
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project), before);
}

TEST_F(EnabledSetFixture, EditedDocumentStillReads) {
	FileSystem::WriteTextFile(_paths.project, str{"[Packages]\nenabled = a; b ;;c\n"});
	Settings::Terminate();
	Settings::Initialize(_paths);

	Vector<str> read;
	PackageOps::ReadEnabledSet(read);
	EXPECT_EQ(Join(read), "a,b,c");
}

TEST_F(EnabledSetFixture, EnabledSetRefusesMachineLocalOverride) {
	str names[] = {str{"a"}};
	PackageOps::WriteEnabledSet(names);

	FileSystem::WriteTextFile(_paths.user, str{"[Packages]\nenabled = a;sneaky\n"});
	Settings::Terminate();
	Settings::Initialize(_paths);

	Vector<str> read;
	PackageOps::ReadEnabledSet(read);
	EXPECT_EQ(Join(read), "a");
}

TEST_F(PackageDiskFixture, DiskCatalogueMatchesBuilt) {
	WritePackage("vehicles", R"({"name":"com.acme.vehicles","version":"1.0.0","kind":"engine","dependencies":{"com.acme.physics":"1.0.0"}})");
	WritePackage("physics", R"({"name":"com.acme.physics","version":"1.0.0","kind":"engine"})");
	WritePackage("importers", R"({"name":"com.acme.importers","version":"1.0.0","kind":"tool","required":true})");

	Path roots[] = {Root()};
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::DiscoverPackages(roots, catalogue, diagnostics);
	ASSERT_EQ(catalogue.size(), 3u) << Join(diagnostics);

	str stored[] = {str{"com.acme.vehicles"}};
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);

	EXPECT_EQ(Join(Names(resolution)), "com.acme.importers,com.acme.physics,com.acme.vehicles");

	Vector<str> blockers;
	EXPECT_FALSE(PackageOps::CanDisable(resolution, "com.acme.physics", blockers));
	EXPECT_EQ(Join(blockers), "com.acme.vehicles");
}

namespace {

	Vector<PackageOps::PackageRow> RowsFor(std::span<const PackageOps::PackageManifest> catalogue, std::span<const str> stored) {
		Vector<PackageOps::ResolvedPackage> resolution;
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		Vector<PackageOps::PackageRow> rows;
		PackageOps::BuildPackageRows(catalogue, resolution, rows);
		return rows;
	}

	const PackageOps::PackageRow* FindRow(std::span<const PackageOps::PackageRow> rows, std::string_view name) {
		for (const PackageOps::PackageRow& row : rows) {
			if (row.name == name) {
				return &row;
			}
		}
		return nullptr;
	}

	str RowNames(std::span<const PackageOps::PackageRow> rows) {
		Vector<str> names;
		for (const PackageOps::PackageRow& row : rows) {
			names.emplace_back(row.name);
		}
		return Join(names);
	}

} // namespace

TEST(PackageRowsTest, RequiredToolHasDeadToggle) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("com.acme.importers"));
	catalogue.push_back(Make("com.acme.theproject", PackageOps::PackageKind::Game));
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<PackageOps::PackageRow> rows = RowsFor(catalogue, {});

	EXPECT_EQ(rows.size(), 2u) << RowNames(rows);
	const PackageOps::PackageRow* tool = FindRow(rows, "com.acme.importers");
	ASSERT_NE(tool, nullptr);
	EXPECT_TRUE(tool->enabled);
	EXPECT_TRUE(tool->required);
	EXPECT_FALSE(tool->canToggle);
	EXPECT_TRUE(tool->blockedBy.empty());

	EXPECT_EQ(FindRow(rows, "com.acme.theproject"), nullptr);
}

TEST(PackageRowsTest, BuiltInsSortFirst) {
	Vector<PackageOps::PackageManifest> catalogue;
	PackageOps::PackageManifest zebra = Make("com.acme.zebra", PackageOps::PackageKind::Package);
	zebra.displayName = "Zebra";
	PackageOps::PackageManifest apple = Make("com.acme.apple", PackageOps::PackageKind::Package);
	apple.displayName = "Apple";
	PackageOps::PackageManifest tool = Make("com.acme.tool", PackageOps::PackageKind::Tool);
	tool.displayName = "Import Pipeline";
	PackageOps::PackageManifest feature = MakeEngine("com.acme.feature");
	feature.displayName = "Physics";
	catalogue.push_back(zebra);
	catalogue.push_back(apple);
	catalogue.push_back(tool);
	catalogue.push_back(feature);

	Vector<PackageOps::PackageRow> rows = RowsFor(catalogue, {});

	EXPECT_EQ(RowNames(rows), "com.acme.feature,com.acme.tool,com.acme.apple,com.acme.zebra");
}

TEST(PackageRowsTest, NamelessManifestShowsId) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<PackageOps::PackageRow> rows = RowsFor(catalogue, {});
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows[0].displayName, "com.acme.physics");
}

TEST(PackageRowsTest, DependencyShowsUnrequested) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.vehicles", {"com.acme.physics"}));
	catalogue.push_back(MakeEngine("com.acme.physics"));

	str stored[] = {str{"com.acme.vehicles"}};
	Vector<PackageOps::PackageRow> rows = RowsFor(catalogue, stored);

	const PackageOps::PackageRow* physics = FindRow(rows, "com.acme.physics");
	ASSERT_NE(physics, nullptr);
	EXPECT_TRUE(physics->enabled);
	EXPECT_FALSE(physics->requested);
	EXPECT_FALSE(physics->canToggle);
	EXPECT_EQ(Join(physics->blockedBy), "com.acme.vehicles");

	const PackageOps::PackageRow* vehicles = FindRow(rows, "com.acme.vehicles");
	ASSERT_NE(vehicles, nullptr);
	EXPECT_TRUE(vehicles->requested);
	EXPECT_TRUE(vehicles->canToggle);
	EXPECT_TRUE(vehicles->blockedBy.empty());
}

TEST(PackageRowsTest, DisabledRowsEnable) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<PackageOps::PackageRow> rows = RowsFor(catalogue, {});
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_FALSE(rows[0].enabled);
	EXPECT_TRUE(rows[0].canToggle);
}

TEST_F(EnabledSetFixture, DependentStoresOnlyItsName) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.vehicles", {"com.acme.physics"}));
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.vehicles", true, blockers)) << Join(blockers);

	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.vehicles");

	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
	EXPECT_EQ(Join(Names(resolution)), "com.acme.physics,com.acme.vehicles");
}

TEST_F(EnabledSetFixture, RefusedDisableNamesDependent) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.vehicles", {"com.acme.physics"}));
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.vehicles", true, blockers));
	const str before = FileSystem::ReadTextFile(_paths.project);

	EXPECT_FALSE(PackageOps::SetPackageEnabled(catalogue, "com.acme.physics", false, blockers));
	EXPECT_EQ(Join(blockers), "com.acme.vehicles");
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project), before);

	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.vehicles", false, blockers)) << Join(blockers);
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_TRUE(stored.empty()) << Join(stored);
}

TEST_F(EnabledSetFixture, RequiredToolDisableRefused) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("com.acme.importers"));

	Vector<str> blockers;
	EXPECT_FALSE(PackageOps::SetPackageEnabled(catalogue, "com.acme.importers", false, blockers));
	EXPECT_EQ(Join(blockers), "the engine");

	EXPECT_FALSE(PackageOps::SetPackageEnabled(catalogue, "com.acme.importers", true, blockers));
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_TRUE(stored.empty()) << Join(stored);
}

TEST_F(EnabledSetFixture, OptionalToolToggles) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeRequiredTool("com.acme.graphs"));
	catalogue.push_back(Make("com.acme.slshader", PackageOps::PackageKind::Tool, {"com.acme.graphs"}));

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.slshader", true, blockers)) << Join(blockers);
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.slshader");

	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.slshader", false, blockers)) << Join(blockers);
	PackageOps::ReadEnabledSet(stored);
	EXPECT_TRUE(stored.empty()) << Join(stored);
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
	EXPECT_EQ(Join(Names(resolution)), "com.acme.graphs");
}

namespace {

	void WriteSetFromBeforeOptionalTools(const SettingsPaths& paths, std::string_view enabled) {
		Settings::Terminate();
		Config document;
		document.SetString("Packages", "enabled", enabled);
		ASSERT_TRUE(document.Save(paths.project));
		Settings::Initialize(paths);
	}

	Vector<PackageOps::PackageManifest> ThreeKinds() {
		Vector<PackageOps::PackageManifest> catalogue;
		catalogue.push_back(MakeEngine("com.acme.physics"));
		catalogue.push_back(MakeRequiredTool("com.acme.graphs"));
		catalogue.push_back(Make("com.acme.slshader", PackageOps::PackageKind::Tool, {"com.acme.graphs"}));
		return catalogue;
	}

} // namespace

TEST_F(EnabledSetFixture, OldSetEnablesOptionalTools) {
	WriteSetFromBeforeOptionalTools(_paths, "com.acme.physics");
	const Vector<PackageOps::PackageManifest> catalogue = ThreeKinds();

	ASSERT_TRUE(PackageOps::UpgradeStoredSet(catalogue));
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.physics,com.acme.slshader");
	Config reread;
	ASSERT_TRUE(reread.Load(_paths.project));
	EXPECT_EQ(reread.GetI64("Packages", "format", 0), 2);
	const str once = FileSystem::ReadTextFile(_paths.project);
	EXPECT_FALSE(PackageOps::UpgradeStoredSet(catalogue));
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project), once);

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.slshader", false, blockers)) << Join(blockers);
	EXPECT_FALSE(PackageOps::UpgradeStoredSet(catalogue));
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.physics");
}

TEST_F(EnabledSetFixture, OldSetMigratesIds) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("dev.shalltear.physics3d"));
	catalogue.push_back(MakeEngine("com.acme.weather"));
	catalogue.push_back(Make("dev.shalltear.nodeshader", PackageOps::PackageKind::Tool, {}));

	Settings::Terminate();
	Config document;
	document.SetString("Packages", "enabled", "com.acme.weather;com.shalltear.physics3d");
	document.SetI64("Packages", "format", 1);
	ASSERT_TRUE(document.Save(_paths.project));
	Settings::Initialize(_paths);

	ASSERT_TRUE(PackageOps::UpgradeStoredSet(catalogue));
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.weather,dev.shalltear.physics3d");
	Vector<PackageOps::ResolvedPackage> resolution;
	PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
	EXPECT_TRUE(PackageOps::IsEnabled(resolution, "dev.shalltear.physics3d"));
	const str once = FileSystem::ReadTextFile(_paths.project);
	EXPECT_FALSE(PackageOps::UpgradeStoredSet(catalogue));
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project), once);

	WriteSetFromBeforeOptionalTools(_paths, "com.shalltear.physics3d");
	ASSERT_TRUE(PackageOps::UpgradeStoredSet(catalogue));
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "dev.shalltear.nodeshader,dev.shalltear.physics3d");
}

TEST_F(EnabledSetFixture, OldSetToggleKeepsTools) {
	WriteSetFromBeforeOptionalTools(_paths, "com.acme.physics");
	const Vector<PackageOps::PackageManifest> catalogue = ThreeKinds();

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.physics", false, blockers)) << Join(blockers);
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_EQ(Join(stored), "com.acme.slshader");
}

TEST_F(EnabledSetFixture, SettingsWindowDrawsSetAndNotFormat) {
	PackageOps::DeclareSettings();
	Vector<SettingsCategoryInfo> categories;
	Settings::GetCategories(categories);
	const SettingsCategoryInfo* packages = nullptr;
	for (const SettingsCategoryInfo& category : categories) {
		packages = category.path == PackageOps::SettingsCategoryPath ? &category : packages;
	}
	ASSERT_NE(packages, nullptr);
	ASSERT_NE(packages->type, nullptr);
	u32 shown = 0;
	bool sawFormat = false;
	for (u32 index = 0; index < packages->type->fieldCount; ++index) {
		const FieldInfo& field = packages->type->fields[index];
		const bool format = field.displayName && std::string_view(field.displayName) == "format";
		sawFormat = sawFormat || format;
		if (SettingsOps::IsFieldShown(*packages, field)) {
			EXPECT_FALSE(format);
			++shown;
		}
	}
	EXPECT_TRUE(sawFormat);
	EXPECT_EQ(shown, 1u);
}

TEST_F(EnabledSetFixture, UncarriedNameRefused) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<str> blockers;
	EXPECT_FALSE(PackageOps::SetPackageEnabled(catalogue, "com.acme.ghost", true, blockers));
	Vector<str> stored;
	PackageOps::ReadEnabledSet(stored);
	EXPECT_TRUE(stored.empty()) << Join(stored);
}

TEST_F(EnabledSetFixture, DoubleEnableWritesOnce) {
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeEngine("com.acme.physics"));

	Vector<str> blockers;
	ASSERT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.physics", true, blockers));
	const str once = FileSystem::ReadTextFile(_paths.project);
	EXPECT_TRUE(PackageOps::SetPackageEnabled(catalogue, "com.acme.physics", true, blockers));
	EXPECT_EQ(FileSystem::ReadTextFile(_paths.project), once);
}

namespace {

	PackageOps::PackageManifest MakeStagedAs(const char* name, const char* binaryName) {
		PackageOps::PackageManifest manifest = MakeEngine(name);
		manifest.directory = Path(str("C:/staged/Packages/") + binaryName);
		return manifest;
	}

	constexpr const char* CORE_ONLY_MODULE = "ShalltearSampleModule" SLT_TEST_DLL_SUFFIX;

} // anonymous namespace

TEST(PackageImportTest, UnloadedImportNamed) {
	const char* common = TestPlatform::GetCommonImportName();
	if (!*common) {
		GTEST_SKIP() << "modules import no package-like library in this tree";
	}
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeStagedAs("com.test.kernel", common));
	catalogue.push_back(MakeStagedAs("com.test.absent", "ImportedByNothing"));

	Vector<str> unloaded;
	ASSERT_TRUE(PackageOps::FindUnloadedPackageImports(Path(CORE_ONLY_MODULE), catalogue, {}, unloaded));
	EXPECT_EQ(Join(unloaded), "com.test.kernel");
}

TEST(PackageImportTest, ImportedPackageSessionLoadedIsNotNamed) {
	const char* common = TestPlatform::GetCommonImportName();
	if (!*common) {
		GTEST_SKIP() << "modules import no package-like library in this tree";
	}
	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeStagedAs("com.test.kernel", common));

	const str loaded[] = {str("com.test.kernel")};
	Vector<str> unloaded;
	ASSERT_TRUE(PackageOps::FindUnloadedPackageImports(Path(CORE_ONLY_MODULE), catalogue, loaded, unloaded));
	EXPECT_TRUE(unloaded.empty()) << Join(unloaded);
}

TEST(PackageImportTest, IncompleteImageHasNoAnswer) {
	const std::filesystem::path directory = std::filesystem::temp_directory_path() /
											("shalltear_imports_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const std::filesystem::path text = directory / "NotAnImage" SLT_TEST_DLL_SUFFIX;
	const std::filesystem::path truncated = directory / "Truncated" SLT_TEST_DLL_SUFFIX;
	std::ofstream(text, std::ios::binary) << "this is not a PE image";

	char head[256];
	std::ifstream source(CORE_ONLY_MODULE, std::ios::binary);
	ASSERT_TRUE(source.read(head, sizeof(head)));
	std::ofstream(truncated, std::ios::binary).write(head, sizeof(head));

	Vector<PackageOps::PackageManifest> catalogue;
	catalogue.push_back(MakeStagedAs("com.test.kernel", TestPlatform::GetCommonImportName()));
	for (const std::filesystem::path& file : {text, truncated, directory / "Missing" SLT_TEST_DLL_SUFFIX}) {
		Vector<str> unloaded;
		unloaded.push_back("left over from a previous call");
		EXPECT_FALSE(PackageOps::FindUnloadedPackageImports(Path(file.generic_string()), catalogue, {}, unloaded))
			<< file.generic_string();
		EXPECT_TRUE(unloaded.empty()) << file.generic_string();
	}

	std::error_code ec;
	std::filesystem::remove_all(directory, ec);
}

#ifdef SHALLTEAR_SHARED

TEST(PackageImportTest, UnloadedPhysicsLinkNamed) {
	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
	ASSERT_NE(PackageOps::FindPackage(catalogue, "dev.shalltear.physics3d"), nullptr) << Join(diagnostics);

	Vector<str> unloaded;
	ASSERT_TRUE(PackageOps::FindUnloadedPackageImports(Path("ShalltearPhysicsScriptModule" SLT_TEST_DLL_SUFFIX), catalogue, {}, unloaded));
	EXPECT_EQ(Join(unloaded), "dev.shalltear.physics3d");

	const str loaded[] = {str("dev.shalltear.physics3d")};
	ASSERT_TRUE(PackageOps::FindUnloadedPackageImports(Path("ShalltearPhysicsScriptModule" SLT_TEST_DLL_SUFFIX), catalogue, loaded, unloaded));
	EXPECT_TRUE(unloaded.empty()) << Join(unloaded);

	ASSERT_TRUE(PackageOps::FindUnloadedPackageImports(Path(CORE_ONLY_MODULE), catalogue, {}, unloaded));
	EXPECT_TRUE(unloaded.empty()) << Join(unloaded);
}

#endif
