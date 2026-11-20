#include "TestsPCH.h"

#include "ScopedSettingsStore.h"
#include "TestPlatform.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ProjectHub.h"
#include "Shalltear/Editor/ProjectOps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>

using namespace Shalltear;
using namespace UITest;

namespace {

	using ProjectOps::ProjectEntry;
	using ProjectOps::ProjectStatus;

	class ProjectHubFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		Path _registry;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_hub_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			_registry = Path((_root / "Projects.ini").generic_string());
		}

		void TearDown() override {
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		str Location() const { return _root.generic_string(); }

		str MakeProject(const str& name) {
			str path;
			EXPECT_TRUE(ProjectOps::CreateProject(Location(), name, path));
			return path;
		}

		str MakePlainDirectory(const str& name) {
			std::filesystem::path directory = _root / name;
			std::filesystem::create_directories(directory);
			return directory.generic_string();
		}

		Vector<ProjectEntry> Load() const {
			Vector<ProjectEntry> entries{GetMemoryResource(MemTag::Editor)};
			ProjectOps::LoadRegistry(_registry, entries);
			return entries;
		}

		static const ProjectEntry* Find(const Vector<ProjectEntry>& entries, const str& path) {
			for (const ProjectEntry& entry : entries) {
				if (entry.path == path) {
					return &entry;
				}
			}
			return nullptr;
		}

		static Vector<PackageOps::PackageManifest> OptInPackages() {
			Vector<PackageOps::PackageManifest> catalogue;
			Vector<str> diagnostics;
			PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
			Vector<PackageOps::PackageManifest> offered;
			PackageOps::ListOptInPackages(catalogue, offered);
			return offered;
		}

		static Vector<str> EnabledIn(const str& projectPath) {
			ScopedSettingsStore store{std::filesystem::path(projectPath)};
			Vector<str> stored;
			PackageOps::ReadEnabledSet(stored);
			return stored;
		}

		static bool Holds(const Vector<str>& names, const str& name) { return std::find(names.begin(), names.end(), name) != names.end(); }
	};

	struct HubBuild {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;

		HubBuild() : context(Desc(), &fonts) {
			root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
		}

		static UI::ContextDesc Desc() {
			UI::ContextDesc desc;
			desc.sizePx = {900, 560};
			return desc;
		}
	};

	bool HasText(UI::Context& context, UI::ElementHandle from, const str& text) {
		if (context.GetTextString(from) == text) {
			return true;
		}
		const UI::Element* element = context.GetElement(from);
		if (!element) {
			return false;
		}
		for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			if (HasText(context, handle, text)) {
				return true;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return false;
	}

	InputEvent MouseDown(Vec2 point) {
		InputEvent event{};
		event.type = InputEventType::MouseDown;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
		return event;
	}

	InputEvent MouseUp(Vec2 point) {
		InputEvent event{};
		event.type = InputEventType::MouseUp;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
		return event;
	}

	void Click(UI::Context& context, Vec2 point) {
		context.InjectInput(MouseDown(point));
		context.InjectInput(MouseUp(point));
		context.Update();
	}

	Vec2 CenterOf(UI::Context& context, UI::ElementHandle handle) {
		const UI::Rect& box = context.GetLayout(handle).borderBox;
		return {box.x + box.w * 0.5f, box.y + box.h * 0.5f};
	}

	UI::ElementHandle RowFor(UI::Context& context, UI::ElementHandle from, const str& cellText) {
		UI::ElementHandle cell = FindByText(context, from, cellText);
		if (!context.IsAlive(cell)) {
			return {};
		}
		const UI::Element* element = context.GetElement(cell);
		return element ? context.MakeHandle(element->parent) : UI::ElementHandle{};
	}

} // namespace

TEST_F(ProjectHubFixture, CreateProjectScaffoldsAndIsRecognized) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "MyGame", path));

	EXPECT_EQ(path, ProjectOps::NormalizePath(Location() + "/MyGame"));
	EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(path) / "Project.ini"));
	EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(path) / "Assets" / "Scenes"));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		std::filesystem::path(path) / "BuildProfiles" / "Default.buildprofile"));
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(path) / "Library"));
	EXPECT_TRUE(ProjectOps::IsProjectDirectory(path));

	const std::filesystem::path settings = std::filesystem::path(path) / "Assets" / "Settings";
	for (const char* tier : {"1 Low.slprofile", "2 Medium.slprofile", "3 High.slprofile", "4 Ultra.slprofile"}) {
		EXPECT_TRUE(std::filesystem::is_regular_file(settings / tier)) << tier;
	}
	Config projectSettings;
	ASSERT_TRUE(projectSettings.Load(Path((std::filesystem::path(path) / "ProjectSettings.ini").generic_string())));
	EXPECT_EQ(projectSettings.GetI64("Graphics", "qualityLevel", -1), 2);

	GraphicsProfile low;
	GraphicsProfile ultra;
	ASSERT_TRUE(low.Import(Path((settings / "1 Low.slprofile").generic_string())));
	ASSERT_TRUE(ultra.Import(Path((settings / "4 Ultra.slprofile").generic_string())));
	EXPECT_EQ(low.msaa, 1);
	EXPECT_EQ(ultra.msaa, 4);
	EXPECT_LT(low.shadowAtlasSize, ultra.shadowAtlasSize);
	EXPECT_LT(low.shadowPcfTaps, ultra.shadowPcfTaps);
	EXPECT_FLOAT_EQ(low.renderScale, 1.0f);
	EXPECT_FLOAT_EQ(ultra.renderScale, 1.0f);

	struct ShadowTier {
		const char* file;
		i32 atlas;
		i32 cascades;
		f32 distance;
		f32 firstCascade;
		i32 tent;
	};
	const ShadowTier tiers[] = {{"1 Low.slprofile", 1024, 2, 40.0f, 10.0f, 3},
								{"2 Medium.slprofile", 2048, 3, 60.0f, 6.0f, 5},
								{"3 High.slprofile", 2048, 4, 60.0f, 4.0f, 5},
								{"4 Ultra.slprofile", 4096, 4, 120.0f, 8.0f, 7}};
	const GraphicsProfile builtIn;
	for (const ShadowTier& tier : tiers) {
		GraphicsProfile profile;
		ASSERT_TRUE(profile.Import(Path((settings / tier.file).generic_string()))) << tier.file;
		EXPECT_EQ(profile.shadowAtlasSize, tier.atlas) << tier.file;
		EXPECT_EQ(profile.shadowCascades, tier.cascades) << tier.file;
		EXPECT_FLOAT_EQ(profile.shadowDistance, tier.distance) << tier.file;
		EXPECT_FLOAT_EQ(profile.shadowFirstCascadeDistance, tier.firstCascade) << tier.file;
		EXPECT_EQ(profile.shadowPcfTaps, tier.tent) << tier.file;
		if (std::string_view(tier.file) == "3 High.slprofile") {
			EXPECT_FLOAT_EQ(profile.shadowDistance, builtIn.shadowDistance);
			EXPECT_FLOAT_EQ(profile.shadowFirstCascadeDistance, builtIn.shadowFirstCascadeDistance);
			EXPECT_EQ(profile.shadowCascades, builtIn.shadowCascades);
			EXPECT_EQ(profile.shadowAtlasSize, builtIn.shadowAtlasSize);
			EXPECT_EQ(profile.shadowPcfTaps, builtIn.shadowPcfTaps);
		}
	}

	str name;
	u64 created = 0;
	ASSERT_TRUE(ProjectOps::ReadProjectFile(path, name, created));
	EXPECT_EQ(name, "MyGame");
	EXPECT_GT(created, 1577836800ull);
}

TEST_F(ProjectHubFixture, NewProjectEnablesBuiltIns) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Featured", path));

	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
	ASSERT_FALSE(catalogue.empty());

	Vector<PackageOps::ResolvedPackage> resolution;
	{
		ScopedSettingsStore store{std::filesystem::path(path)};
		Vector<str> stored;
		PackageOps::ReadEnabledSet(stored);
		ASSERT_FALSE(stored.empty());
		PackageOps::ResolveEnabledSet(catalogue, stored, resolution);
		EXPECT_FALSE(PackageOps::UpgradeStoredSet(catalogue));
	}

	for (const PackageOps::PackageManifest& manifest : catalogue) {
		const bool enabled = PackageOps::IsEnabled(resolution, manifest.name);
		const bool dependenciesEnabled = std::all_of(manifest.dependencies.begin(), manifest.dependencies.end(), [&](const PackageOps::PackageDependency& dependency) {
			return PackageOps::IsEnabled(resolution, dependency.name);
		});
		switch (manifest.kind) {
		case PackageOps::PackageKind::Engine:
			EXPECT_EQ(enabled, manifest.bridge ? dependenciesEnabled : manifest.scaffold) << manifest.name;
			break;
		case PackageOps::PackageKind::Package:
			EXPECT_FALSE(enabled);
			break;
		case PackageOps::PackageKind::Tool:
			EXPECT_EQ(enabled, manifest.required || manifest.scaffold) << manifest.name;
			break;
		default:
			break;
		}
	}
}

namespace {

	class ProjectScaffoldSceneFixture : public ProjectHubFixture {
	protected:
		void SetUp() override {
			ProjectHubFixture::SetUp();
			TypeRegistry::InitializePrimitives();
			SceneSystem::Initialize();
		}

		void TearDown() override {
			SceneSystem::Terminate();
			ProjectHubFixture::TearDown();
		}

		void WriteScene(const str& projectPath, const str& relative) {
			std::filesystem::path file = std::filesystem::path(projectPath) / relative;
			std::filesystem::create_directories(file.parent_path());
			ASSERT_TRUE(FileSystem::WriteTextFile(Path(file.generic_string()), "{}\n"));
		}
	};

} // namespace

TEST_F(ProjectScaffoldSceneFixture, NewProjectHasProfileScene) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Seeded", path));

	std::filesystem::path scene = std::filesystem::path(path) / "Assets" / "Scenes" / "Main.scene";
	ASSERT_TRUE(std::filesystem::is_regular_file(scene));

	Handle<Scene> loaded = SceneSystem::LoadScene(Path(scene.generic_string()));
	ASSERT_TRUE(loaded.IsValid());
	bool sawCamera = false;
	bool sawDirectionalLight = false;
	for (u32 i = 0; i < loaded.Get()->GetRootCount(); ++i) {
		Handle<SceneObject> root = loaded.Get()->GetRoot(i);
		if (!root.IsValid()) {
			continue;
		}
		sawCamera |= root.Get()->GetComponent<Camera>().IsValid();
		Handle<Light> light = root.Get()->GetComponent<Light>();
		sawDirectionalLight |= light.IsValid() && light.Get()->GetLightType() == LightType::Directional;
	}
	EXPECT_TRUE(sawCamera);
	EXPECT_TRUE(sawDirectionalLight);
	SceneSystem::UnloadScene(loaded);
	SceneSystem::EndFrame();

	BuildProfile profile;
	ASSERT_TRUE(BuildProfileOps::LoadProfile(BuildProfileOps::GetProfilePath(path, "Default"), profile));
	Vector<str> scenes{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(profile.GetList("scenes", scenes));
	ASSERT_EQ(scenes.size(), 1u);
	EXPECT_EQ(scenes[0], "Assets/Scenes/Main.scene");
}

TEST_F(ProjectScaffoldSceneFixture, FirstLaunchOpensProjectsOnlyScene) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Fresh", path));

	EXPECT_EQ(EditorSceneOps::ChooseStartupScene(path, {}), "project://Assets/Scenes/Main.scene");
}

TEST_F(ProjectScaffoldSceneFixture, SeveralScenesAndNothingRememberedOpensNothing) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Several", path));
	WriteScene(path, "Assets/Scenes/Level2.scene");

	EXPECT_TRUE(EditorSceneOps::ChooseStartupScene(path, {}).empty());
}

TEST_F(ProjectScaffoldSceneFixture, StartupPrefersRememberedScene) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Remembered", path));
	WriteScene(path, "Assets/Scenes/Level2.scene");
	ASSERT_TRUE(ProjectOps::MountProject(path));

	Vector<str> remembered{GetMemoryResource(MemTag::Editor)};
	remembered.push_back("project://Assets/Scenes/Deleted.scene");
	remembered.push_back("project://Assets/Scenes/Level2.scene");
	remembered.push_back("project://Assets/Scenes/Main.scene");

	EXPECT_EQ(EditorSceneOps::ChooseStartupScene(path, remembered), "project://Assets/Scenes/Level2.scene");

	Vector<str> allGone{GetMemoryResource(MemTag::Editor)};
	allGone.push_back("project://Assets/Scenes/Deleted.scene");
	EXPECT_TRUE(EditorSceneOps::ChooseStartupScene(path, allGone).empty());
}

TEST_F(ProjectScaffoldSceneFixture, ProfileScenesNotStartupScene) {
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Profiled", path));
	WriteScene(path, "Assets/Scenes/Level2.scene");

	BuildProfile profile;
	ASSERT_TRUE(BuildProfileOps::LoadProfile(BuildProfileOps::GetProfilePath(path, "Default"), profile));
	Vector<str> listed{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(profile.GetList("scenes", listed));
	ASSERT_EQ(listed.size(), 1u);

	EXPECT_TRUE(EditorSceneOps::ChooseStartupScene(path, {}).empty());
}

TEST_F(ProjectHubFixture, CreateRefusesBadNamesAndTargets) {
	str path;
	EXPECT_FALSE(ProjectOps::CreateProject(Location(), "", path));
	EXPECT_FALSE(ProjectOps::CreateProject(Location(), "..", path));
	EXPECT_FALSE(ProjectOps::CreateProject(Location(), "Nested/Escape", path));
	EXPECT_FALSE(ProjectOps::CreateProject(Location(), "trailing.", path));
	EXPECT_TRUE(path.empty());

	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "Taken", path));
	str second;
	EXPECT_FALSE(ProjectOps::CreateProject(Location(), "Taken", second));
	EXPECT_TRUE(second.empty());
}

TEST_F(ProjectHubFixture, StatusSeparatesMissingFromInvalid) {
	str project = MakeProject("Alive");
	str plain = MakePlainDirectory("JustAFolder");

	EXPECT_EQ(ProjectOps::GetStatus(project), ProjectStatus::Ok);
	EXPECT_EQ(ProjectOps::GetStatus(plain), ProjectStatus::Invalid);
	EXPECT_EQ(ProjectOps::GetStatus(Location() + "/NeverExisted"), ProjectStatus::Missing);

	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::path(project), ec);
	EXPECT_EQ(ProjectOps::GetStatus(project), ProjectStatus::Missing);
}

TEST_F(ProjectHubFixture, ModifiedTimeFollowsTheNewestProjectFile) {
	str project = MakeProject("Timed");
	EXPECT_GT(ProjectOps::GetModifiedTime(project), 0u);

	std::filesystem::path projectFile = std::filesystem::path(project) / "Project.ini";
	std::filesystem::path assets = std::filesystem::path(project) / "Assets";
	auto now = std::filesystem::file_time_type::clock::now();
	std::error_code ec;

	std::filesystem::last_write_time(projectFile, now - std::chrono::hours{48}, ec);
	ASSERT_FALSE(ec);
	std::filesystem::last_write_time(assets, now - std::chrono::hours{1}, ec);
	ASSERT_FALSE(ec);
	u64 assetsNewer = ProjectOps::GetModifiedTime(project);
	EXPECT_EQ(assetsNewer, FileSystem::GetLastWriteTimeUnix(Path(project + "/Assets")));

	std::filesystem::last_write_time(projectFile, now, ec);
	ASSERT_FALSE(ec);
	u64 projectFileNewer = ProjectOps::GetModifiedTime(project);
	EXPECT_GT(projectFileNewer, assetsNewer);
	EXPECT_EQ(projectFileNewer, FileSystem::GetLastWriteTimeUnix(Path(projectFile.generic_string())));

	EXPECT_EQ(ProjectOps::GetModifiedTime(Location() + "/Gone"), 0u);
}

TEST_F(ProjectHubFixture, PathsCompareWayFileNamesDo) {
	str project = MakeProject("Cased");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	str shouting = project;
	for (char& c : shouting) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 32);
		}
	}
	EXPECT_FALSE(ProjectOps::PathsEqual(project, project + "x"));
	if (!TestPlatform::FileNamesFoldCase()) {
		EXPECT_FALSE(ProjectOps::PathsEqual(project, shouting));
		return;
	}
	EXPECT_TRUE(ProjectOps::PathsEqual(project, shouting));

	ProjectHub hub{_registry};
	hub.Select(shouting);
	EXPECT_EQ(hub.GetSelectedPath(), project);
	hub.Choose(shouting);
	EXPECT_EQ(hub.GetChosenPath(), project);

	ASSERT_TRUE(ProjectOps::AddProject(_registry, shouting));
	EXPECT_EQ(Load().size(), 1u);
}

TEST_F(ProjectHubFixture, AddProjectDedupesRecentFirst) {
	str first = MakeProject("First");
	str second = MakeProject("Second");

	ASSERT_TRUE(ProjectOps::AddProject(_registry, first));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, second));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, first));

	Vector<ProjectEntry> entries = Load();
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].path, first);
	EXPECT_EQ(entries[1].path, second);
	EXPECT_EQ(entries[0].name, "First");
	EXPECT_EQ(entries[0].status, ProjectStatus::Ok);
}

TEST_F(ProjectHubFixture, AddProjectRefusesNonProjects) {
	EXPECT_FALSE(ProjectOps::AddProject(_registry, MakePlainDirectory("Plain")));
	EXPECT_FALSE(ProjectOps::AddProject(_registry, Location() + "/Nowhere"));
	EXPECT_TRUE(Load().empty());
}

TEST_F(ProjectHubFixture, TouchStampsLastOpenedAndPromotesEntry) {
	str first = MakeProject("First");
	str second = MakeProject("Second");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, first));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, second));

	ProjectOps::TouchProject(_registry, first);

	Vector<ProjectEntry> entries = Load();
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].path, first);
	EXPECT_GT(entries[0].lastOpenedUnix, 0u);
	EXPECT_GE(entries[0].modifiedUnix, entries[0].lastOpenedUnix);
	EXPECT_EQ(entries[1].lastOpenedUnix, 0u);
}

TEST_F(ProjectHubFixture, RemoveForgetsEntryButKeepsFiles) {
	str project = MakeProject("Kept");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	EXPECT_TRUE(ProjectOps::RemoveProject(_registry, project));
	EXPECT_FALSE(ProjectOps::RemoveProject(_registry, project));
	EXPECT_TRUE(Load().empty());
	EXPECT_TRUE(ProjectOps::IsProjectDirectory(project));
}

TEST_F(ProjectHubFixture, MissingProjectsKeepTheirCachedMetadata) {
	str project = MakeProject("Vanishing");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));
	u64 createdBefore = Load()[0].createdUnix;

	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::path(project), ec);

	Vector<ProjectEntry> entries = Load();
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0].status, ProjectStatus::Missing);
	EXPECT_FALSE(entries[0].IsOpenable());
	EXPECT_EQ(entries[0].name, "Vanishing");
	EXPECT_EQ(entries[0].createdUnix, createdBefore);
}

TEST_F(ProjectHubFixture, RegistryRoundTripsThroughDisk) {
	str project = MakeProject("Persisted");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	Config config;
	ASSERT_TRUE(config.Load(_registry));
	EXPECT_EQ(config.GetI64("Hub", "version", 0), 1);
	EXPECT_EQ(config.GetString("Project.0", "path"), project);
	EXPECT_EQ(config.GetString("Project.0", "name"), "Persisted");
}

TEST_F(ProjectHubFixture, UnknownRegistryVersionYieldsAnEmptyList) {
	Config config;
	config.SetI64("Hub", "version", 999);
	config.SetString("Project.0", "path", MakeProject("Future"));
	ASSERT_TRUE(config.Save(_registry));

	EXPECT_TRUE(Load().empty());
}

TEST_F(ProjectHubFixture, FilterMatchesNameAndPathCaseInsensitively) {
	ProjectEntry entry;
	entry.name = "SpaceGame";
	entry.path = "C:/Users/dev/Projects/space-game";

	EXPECT_TRUE(ProjectOps::MatchesFilter(entry, ""));
	EXPECT_TRUE(ProjectOps::MatchesFilter(entry, "spacegame"));
	EXPECT_TRUE(ProjectOps::MatchesFilter(entry, "SPACE"));
	EXPECT_TRUE(ProjectOps::MatchesFilter(entry, "Users/dev"));
	EXPECT_FALSE(ProjectOps::MatchesFilter(entry, "racing"));
}

TEST_F(ProjectHubFixture, DeleteFromDiskRemovesTreeAndEntry) {
	str project = MakeProject("Doomed");
	FileSystem::WriteTextFile(Path(project + "/Assets/thing.txt"), "content");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	EXPECT_TRUE(ProjectOps::DeleteProjectFromDisk(_registry, project));
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(project)));
	EXPECT_TRUE(Load().empty());
}

TEST_F(ProjectHubFixture, DiskDeleteNeedsProject) {
	str plain = MakePlainDirectory("PreciousFolder");
	FileSystem::WriteTextFile(Path(plain + "/irreplaceable.txt"), "content");

	EXPECT_FALSE(ProjectOps::DeleteProjectFromDisk(_registry, plain));
	EXPECT_FALSE(ProjectOps::DeleteProjectFromDisk(_registry, Location() + "/Nowhere"));
	EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(plain) / "irreplaceable.txt"));
}

TEST_F(ProjectHubFixture, DeleteFromDiskRefusesTheOpenProject) {
	str project = MakeProject("Mounted");
	ASSERT_TRUE(ProjectOps::MountProject(project));

	EXPECT_EQ(ProjectOps::GetMountedProjectPath(), project);
	EXPECT_FALSE(ProjectOps::DeleteProjectFromDisk(_registry, project));
	EXPECT_TRUE(ProjectOps::IsProjectDirectory(project));
}

TEST_F(ProjectHubFixture, MountProjectResolvesTheVirtualScheme) {
	str project = MakeProject("Mountable");
	FileSystem::WriteTextFile(Path(project + "/Assets/readme.txt"), "hello");

	ASSERT_TRUE(ProjectOps::MountProject(project));
	EXPECT_EQ(FileSystem::Resolve(Path("project://Assets/readme.txt")).ToString(),
			  project + "/Assets/readme.txt");
	EXPECT_EQ(ProjectOps::GetMountedProjectPath(), project);

	EXPECT_FALSE(ProjectOps::MountProject(Location() + "/Nowhere"));
	EXPECT_EQ(ProjectOps::GetMountedProjectPath(), project);
}

TEST_F(ProjectHubFixture, MountingDoesNotIndex) {
	str project = MakeProject("Unindexed");
	FileSystem::WriteTextFile(Path(project + "/Assets/readme.txt"), "hello");
	Path vpath("project://Assets/readme.txt");

	ASSERT_TRUE(ProjectOps::MountProject(project));
	EXPECT_FALSE(AssetDatabase::LookupGuid(vpath).IsValid());
	EXPECT_FALSE(FileSystem::Exists(FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath))));

	AssetDatabase::ScanProjectDirectory();
	EXPECT_TRUE(AssetDatabase::LookupGuid(vpath).IsValid());
	EXPECT_TRUE(FileSystem::Exists(FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath))));
}

TEST_F(ProjectHubFixture, ViewListsEveryColumnOfEveryProject) {
	str project = MakeProject("Listed");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	ASSERT_EQ(hub.GetEntries().size(), 1u);
	EXPECT_TRUE(HasText(build.context, build.root, "Listed"));
	EXPECT_TRUE(HasText(build.context, build.root, project));
	EXPECT_TRUE(HasText(build.context, build.root, "OK"));
	EXPECT_FALSE(HasText(build.context, build.root, std::to_string(hub.GetEntries()[0].createdUnix)));
}

TEST_F(ProjectHubFixture, ViewFiltersByNameAndByPath) {
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Alpha")));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Beta")));

	ProjectHub hub{_registry};
	EXPECT_EQ(hub.GetVisibleCount(), 2u);

	hub.SetFilter("alph");
	EXPECT_EQ(hub.GetVisibleCount(), 1u);

	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();
	EXPECT_TRUE(HasText(build.context, build.root, "Alpha"));
	EXPECT_FALSE(HasText(build.context, build.root, "Beta"));

	hub.SetFilter("shalltear_hub_");
	EXPECT_EQ(hub.GetVisibleCount(), 2u);

	hub.SetFilter("no-such-project");
	EXPECT_EQ(hub.GetVisibleCount(), 0u);
	HubBuild empty;
	hub.BuildContent(empty.context, empty.root);
	EXPECT_TRUE(HasText(empty.context, empty.root, "No projects match the search"));
}

TEST_F(ProjectHubFixture, MissingProjectsUnclickable) {
	str alive = MakeProject("Alive");
	str gone = MakeProject("Gone");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, alive));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, gone));
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::path(gone), ec);

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	EXPECT_TRUE(HasText(build.context, build.root, "Missing"));

	UI::ElementHandle missingRow = RowFor(build.context, build.root, gone);
	ASSERT_TRUE(build.context.IsAlive(missingRow));
	EXPECT_FALSE(static_cast<bool>(build.context.Events(missingRow).onClick));
	EXPECT_FALSE(static_cast<bool>(build.context.Events(missingRow).onDoubleClick));
	EXPECT_TRUE(static_cast<bool>(build.context.Events(missingRow).onRightClick));

	UI::ElementHandle aliveRow = RowFor(build.context, build.root, alive);
	ASSERT_TRUE(build.context.IsAlive(aliveRow));
	EXPECT_TRUE(static_cast<bool>(build.context.Events(aliveRow).onClick));
	EXPECT_TRUE(static_cast<bool>(build.context.Events(aliveRow).onDoubleClick));

	hub.Select(gone);
	EXPECT_TRUE(hub.GetSelectedPath().empty());
	hub.Choose(gone);
	EXPECT_TRUE(hub.GetChosenPath().empty());
}

TEST_F(ProjectHubFixture, ClickSelectsDoubleClickOpens) {
	str project = MakeProject("Clicked");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle row = RowFor(build.context, build.root, project);
	ASSERT_TRUE(build.context.IsAlive(row));
	Vec2 center = CenterOf(build.context, row);
	EXPECT_EQ(build.context.HitTest(center), row);

	Click(build.context, center);
	EXPECT_EQ(hub.GetSelectedPath(), project);

	Click(build.context, center);
	EXPECT_EQ(hub.GetChosenPath(), project);
}

TEST_F(ProjectHubFixture, ClickingOpenChoosesTheSelection) {
	str project = MakeProject("Opened");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	ProjectHub hub{_registry};
	hub.Select(project);
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle openLabel = FindByText(build.context, build.root, "Open");
	ASSERT_TRUE(build.context.IsAlive(openLabel));
	Click(build.context, CenterOf(build.context, openLabel));
	EXPECT_EQ(hub.GetChosenPath(), project);
}

TEST_F(ProjectHubFixture, SelectingThenOpeningYieldsTheChosenPath) {
	str project = MakeProject("Chosen");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));

	ProjectHub hub{_registry};
	hub.Select(project);
	EXPECT_EQ(hub.GetSelectedPath(), project);
	hub.Choose(project);
	EXPECT_EQ(hub.GetChosenPath(), project);
}

TEST_F(ProjectHubFixture, HubManagesProjectsThroughTheSameOps) {
	ProjectHub hub{_registry};
	ASSERT_TRUE(hub.CreateAndAdd(Location(), "MadeByTheHub"));
	ASSERT_EQ(hub.GetEntries().size(), 1u);
	str created = hub.GetEntries()[0].path;
	EXPECT_TRUE(ProjectOps::IsProjectDirectory(created));
	EXPECT_EQ(hub.GetSelectedPath(), created);

	str existing = MakeProject("FoundOnDisk");
	ASSERT_TRUE(hub.AddExisting(existing));
	EXPECT_EQ(hub.GetEntries().size(), 2u);
	EXPECT_FALSE(hub.AddExisting(MakePlainDirectory("NotAProject")));
	EXPECT_EQ(hub.GetEntries().size(), 2u);

	ASSERT_TRUE(hub.Remove(existing));
	EXPECT_EQ(hub.GetEntries().size(), 1u);
	EXPECT_TRUE(ProjectOps::IsProjectDirectory(existing));

	ASSERT_TRUE(hub.DeleteFromDisk(created));
	EXPECT_TRUE(hub.GetEntries().empty());
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(created)));
	EXPECT_TRUE(hub.GetSelectedPath().empty());
}

TEST_F(ProjectHubFixture, StatusMessageYieldsBackToTheCount) {
	str kept = MakeProject("Kept");
	str dropped = MakeProject("Dropped");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, kept));
	ASSERT_TRUE(ProjectOps::AddProject(_registry, dropped));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	EXPECT_TRUE(HasText(build.context, build.root, "2 of 2 projects"));

	ASSERT_TRUE(hub.Remove(dropped));
	hub.BuildContent(build.context, build.root);
	EXPECT_TRUE(HasText(build.context, build.root, "removed from the list (files untouched)"));

	hub.SetFilter("Kept");
	hub.BuildContent(build.context, build.root);
	EXPECT_TRUE(HasText(build.context, build.root, "1 of 1 projects"));
}

TEST_F(ProjectHubFixture, DefaultLocationIsUsableOnFirstRun) {
	ProjectHub fresh{_registry};
	ASSERT_TRUE(fresh.GetEntries().empty());
	str firstRun = fresh.GetDefaultLocation();
	ASSERT_FALSE(firstRun.empty());
	EXPECT_TRUE(FileSystem::IsDirectory(Path(firstRun)));

	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Neighbour")));
	ProjectHub listed{_registry};
	EXPECT_EQ(listed.GetDefaultLocation(), ProjectOps::NormalizePath(Location()));
}

TEST_F(ProjectHubFixture, ViewPaintsRowsAndChrome) {
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Painted")));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	UI::RenderList list;
	build.context.BuildRenderList(list);

	u32 textured = 0;
	for (const UI::UICommand& command : list.commands) {
		textured += command.texture.IsValid() ? 1 : 0;
	}
	EXPECT_GT(list.quads.size(), 20u);
	EXPECT_GT(textured, 0u);
}

TEST_F(ProjectHubFixture, RebuildingTheViewIsIdempotent) {
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Twice")));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();
	u32 afterFirst = build.context.GetLiveCount();

	hub.BuildContent(build.context, build.root);
	build.context.Update();
	EXPECT_EQ(build.context.GetLiveCount(), afterFirst);
	EXPECT_TRUE(HasText(build.context, build.root, "Twice"));
}

TEST_F(ProjectHubFixture, DialogsOpenCenteredOverADimmedHub) {
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Behind")));

	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle row = RowFor(build.context, build.root, "Behind");
	ASSERT_TRUE(build.context.IsAlive(row));
	Vec2 rowCenter = CenterOf(build.context, row);

	Click(build.context, CenterOf(build.context, FindByText(build.context, build.root, "Add Existing...")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	build.context.Update();

	const UI::Rect& card = build.context.GetLayout(build.context.GetOpenPopup(0)).borderBox;
	f32 contextWidth = static_cast<f32>(build.context.GetSize().x);
	f32 contextHeight = static_cast<f32>(build.context.GetSize().y);
	ASSERT_GT(card.w, 0.0f);
	ASSERT_LT(card.w, contextWidth);
	EXPECT_NEAR(card.x + card.w * 0.5f, contextWidth * 0.5f, 1.0f);
	EXPECT_NEAR(card.y + card.h * 0.5f, contextHeight * 0.5f, 1.0f);

	ASSERT_FALSE(build.context.GetLayout(row).borderBox.Contains(rowCenter) &&
				 card.Contains(rowCenter));
	EXPECT_NE(build.context.HitTest(rowCenter), row);
}

TEST_F(ProjectHubFixture, SearchFieldStandsOut) {
	ASSERT_TRUE(ProjectOps::AddProject(_registry, MakeProject("Visible")));

	ProjectHub hub{_registry};
	HubBuild build;
	EditorTheme::ApplyToContext(build.context);
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	UI::ElementHandle field = FindByClass(build.context, build.root, StringHash("slt-textfield"), 0);
	ASSERT_TRUE(build.context.IsAlive(field));
	ASSERT_EQ(build.context.GetComputedStyle(field).background.type, UI::BrushType::Solid);

	auto parentOf = [&build](UI::ElementHandle handle) -> UI::ElementHandle {
		const UI::Element* element = build.context.GetElement(handle);
		return (element && element->parent != UI::INVALID_UI_INDEX) ? build.context.MakeHandle(element->parent)
																	: UI::ElementHandle{};
	};
	UI::ElementHandle ground = parentOf(field);
	while (build.context.IsAlive(ground) &&
		   build.context.GetComputedStyle(ground).background.type != UI::BrushType::Solid) {
		ground = parentOf(ground);
	}
	ASSERT_TRUE(build.context.IsAlive(ground));

	Color fill = build.context.GetComputedStyle(field).background.solid;
	Color behind = build.context.GetComputedStyle(ground).background.solid;

	auto step = [](f32 a, f32 b) { return std::abs(a - b) * 255.0f; };
	f32 separation = std::max({step(fill.r, behind.r), step(fill.g, behind.g), step(fill.b, behind.b)});
	EXPECT_GE(separation, 8.0f);
}

TEST(ProjectRelaunchTest, CommandLineCarriesSessionAndRewritesProject) {
	Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
	arguments.push_back("--frame-cost");
	arguments.push_back("--project");
	arguments.push_back("C:/Old Project");
	arguments.push_back("--zones");

	str line = ProjectOps::BuildRelaunchCommandLine("C:/bin/Shalltear.exe",
													{arguments.data(), arguments.size()},
													"C:/New Project");
	EXPECT_EQ(line,
			  "\"C:/bin/Shalltear.exe\" \"--frame-cost\" \"--zones\" --project \"C:/New Project\"")
		<< line;
}

TEST(ProjectRelaunchTest, HubLaunchGainsProjectItPicked) {
	Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
	str line = ProjectOps::BuildRelaunchCommandLine("C:/bin/Editor.exe",
													{arguments.data(), arguments.size()},
													"C:/Games/MyProject");
	EXPECT_EQ(line, "\"C:/bin/Editor.exe\" --project \"C:/Games/MyProject\"") << line;
}

TEST(ProjectRelaunchTest, NoProjectRelaunchesIntoTheHub) {
	Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
	arguments.push_back("--project");
	arguments.push_back("C:/Typo");
	str line = ProjectOps::BuildRelaunchCommandLine("C:/bin/Editor.exe",
													{arguments.data(), arguments.size()}, "");
	EXPECT_EQ(line, "\"C:/bin/Editor.exe\"") << line;
}

TEST(ProjectRelaunchTest, EmptyExecutablePathRefused) {
	Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
	EXPECT_TRUE(ProjectOps::BuildRelaunchCommandLine("", {arguments.data(), arguments.size()},
													 "C:/Games/MyProject")
					.empty());
}

TEST_F(ProjectHubFixture, CreateWithOptInPackageEnablesIt) {
	const Vector<PackageOps::PackageManifest> offered = OptInPackages();
	if (offered.empty()) {
		GTEST_SKIP() << "no built-in package is opt-in in this tree";
	}
	ProjectOps::CreateOptions options;
	options.includeCppModule = false;
	options.optInPackages.push_back(offered.front().name);
	options.optInPackages.push_back("com.acme.absent");
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "OptedIn", options, path));

	const Vector<str> enabled = EnabledIn(path);
	EXPECT_TRUE(Holds(enabled, offered.front().name));
	EXPECT_FALSE(Holds(enabled, "com.acme.absent"));
	const Vector<str> plain = EnabledIn(MakeProject("Plain"));
	EXPECT_EQ(enabled.size(), plain.size() + 1);
	for (const str& name : plain) {
		EXPECT_TRUE(Holds(enabled, name)) << name;
	}
}

TEST_F(ProjectHubFixture, CreateWithoutItDoesNot) {
	const Vector<PackageOps::PackageManifest> offered = OptInPackages();
	if (offered.empty()) {
		GTEST_SKIP() << "no built-in package is opt-in in this tree";
	}
	ProjectOps::CreateOptions options;
	options.includeCppModule = false;
	str path;
	ASSERT_TRUE(ProjectOps::CreateProject(Location(), "NotOptedIn", options, path));
	const Vector<str> enabled = EnabledIn(path);
	ASSERT_FALSE(enabled.empty());
	for (const PackageOps::PackageManifest& manifest : offered) {
		EXPECT_FALSE(Holds(enabled, manifest.name)) << manifest.name;
	}

	ProjectHub hub{_registry};
	ASSERT_TRUE(hub.CreateAndAdd(Location(), "HubDefault", false));
	const Vector<str> fromHub = EnabledIn(hub.GetChosenPath());
	for (const PackageOps::PackageManifest& manifest : offered) {
		EXPECT_FALSE(Holds(fromHub, manifest.name)) << manifest.name;
	}
}

TEST_F(ProjectHubFixture, OptInChoiceSurvivesRegistryWrites) {
	Vector<str> choice{GetMemoryResource(MemTag::Editor)};
	ProjectOps::LoadOptInChoice(_registry, choice);
	EXPECT_TRUE(choice.empty());

	const str names[] = {str{"com.acme.scripting"}, str{"com.acme.shaders"}};
	ASSERT_TRUE(ProjectOps::SaveOptInChoice(_registry, names));
	ProjectOps::LoadOptInChoice(_registry, choice);
	ASSERT_EQ(choice.size(), 2u);
	EXPECT_EQ(choice[0], "com.acme.scripting");
	EXPECT_EQ(choice[1], "com.acme.shaders");

	const str project = MakeProject("Listed");
	ASSERT_TRUE(ProjectOps::AddProject(_registry, project));
	ProjectOps::TouchProject(_registry, project);
	ProjectOps::LoadOptInChoice(_registry, choice);
	EXPECT_EQ(choice.size(), 2u);
	ASSERT_EQ(Load().size(), 1u);

	ASSERT_TRUE(ProjectOps::SaveOptInChoice(_registry, {}));
	ProjectOps::LoadOptInChoice(_registry, choice);
	EXPECT_TRUE(choice.empty());
	EXPECT_EQ(Load().size(), 1u);
}

TEST_F(ProjectHubFixture, NewProjectRemembersOptInPackageChoice) {
	const Vector<PackageOps::PackageManifest> offered = OptInPackages();
	if (offered.empty()) {
		GTEST_SKIP() << "no built-in package is opt-in in this tree";
	}
	const PackageOps::PackageManifest& package = offered.front();
	const str label = "Include " + (package.displayName.empty() ? package.name : package.displayName);
	const auto open = [&](ProjectHub& hub, HubBuild& build) {
		hub.BuildContent(build.context, build.root);
		build.context.Update();
		Click(build.context, CenterOf(build.context, FindByText(build.context, build.root, "New Project...")));
		build.context.Update();
		return build.context.GetOpenPopupCount() == 1 ? build.context.GetOpenPopup(0) : UI::ElementHandle{};
	};
	const auto boxOf = [&](HubBuild& build, UI::ElementHandle card) {
		const UI::Element* text = build.context.GetElement(FindByText(build.context, card, label));
		return text ? FindByClass(build.context, build.context.MakeHandle(text->parent), StringHash("slt-checkbox"), 0) : UI::ElementHandle{};
	};
	const auto ticked = [&](HubBuild& build, UI::ElementHandle box) {
		const UI::Element* mark = build.context.GetElement(FindByClass(build.context, box, StringHash("slt-check-mark"), 0));
		return mark && HasFlag(mark->flags, UI::ElementFlags::Visible);
	};

	{
		ProjectHub hub{_registry};
		HubBuild build;
		const UI::ElementHandle card = open(hub, build);
		ASSERT_TRUE(build.context.IsAlive(card));
		const UI::ElementHandle box = boxOf(build, card);
		ASSERT_TRUE(build.context.IsAlive(box));
		EXPECT_FALSE(ticked(build, box));

		Click(build.context, CenterOf(build.context, FindByText(build.context, card, "Cancel")));
		build.context.Update();
		Vector<str> choice{GetMemoryResource(MemTag::Editor)};
		ProjectOps::LoadOptInChoice(_registry, choice);
		EXPECT_TRUE(choice.empty());
	}

	str created;
	{
		ProjectHub hub{_registry};
		HubBuild build;
		const UI::ElementHandle card = open(hub, build);
		ASSERT_TRUE(build.context.IsAlive(card));
		const UI::ElementHandle locationField = FindByClass(build.context, card, StringHash("slt-textfield"), 1);
		ASSERT_TRUE(build.context.IsAlive(locationField));
		build.context.SetTextString(locationField, Location());
		ClickElement(build.context, boxOf(build, card));
		EXPECT_TRUE(ticked(build, boxOf(build, card)));
		Click(build.context, CenterOf(build.context, FindByText(build.context, card, "Create")));
		build.context.Update();
		created = hub.GetChosenPath();
		ASSERT_FALSE(created.empty());
	}
	EXPECT_TRUE(Holds(EnabledIn(created), package.name));

	{
		ProjectHub hub{_registry};
		HubBuild build;
		const UI::ElementHandle card = open(hub, build);
		ASSERT_TRUE(build.context.IsAlive(card));
		EXPECT_TRUE(ticked(build, boxOf(build, card)));
		build.context.SetTextString(FindByClass(build.context, card, StringHash("slt-textfield"), 0), "Kept");
		Click(build.context, CenterOf(build.context, FindByText(build.context, card, "Create")));
		build.context.Update();
		ASSERT_NE(hub.GetChosenPath(), created);
		EXPECT_TRUE(Holds(EnabledIn(hub.GetChosenPath()), package.name));
	}
	{
		ProjectHub hub{_registry};
		HubBuild build;
		const UI::ElementHandle card = open(hub, build);
		ASSERT_TRUE(build.context.IsAlive(card));
		EXPECT_TRUE(ticked(build, boxOf(build, card)));
		const UI::ElementHandle nameField = FindByClass(build.context, card, StringHash("slt-textfield"), 0);
		build.context.SetTextString(nameField, "Unticked");
		ClickElement(build.context, boxOf(build, card));
		EXPECT_FALSE(ticked(build, boxOf(build, card)));
		Click(build.context, CenterOf(build.context, FindByText(build.context, card, "Create")));
		build.context.Update();
		ASSERT_NE(hub.GetChosenPath(), created);
		EXPECT_FALSE(Holds(EnabledIn(hub.GetChosenPath()), package.name));
	}
	{
		ProjectHub hub{_registry};
		HubBuild build;
		const UI::ElementHandle card = open(hub, build);
		ASSERT_TRUE(build.context.IsAlive(card));
		EXPECT_FALSE(ticked(build, boxOf(build, card)));
	}
}

TEST_F(ProjectHubFixture, CreatedProjectOpens) {
	ProjectHub hub{_registry};
	ASSERT_TRUE(hub.CreateAndAdd(Location(), "OpenedOnCreate"));

	ASSERT_EQ(hub.GetEntries().size(), 1u);
	str created = hub.GetEntries()[0].path;
	EXPECT_EQ(hub.GetChosenPath(), created);
}

TEST_F(ProjectHubFixture, AFailedCreateLeavesTheHubStanding) {
	ProjectHub hub{_registry};
	EXPECT_FALSE(hub.CreateAndAdd(Location(), "Nested/Escape"));
	EXPECT_TRUE(hub.GetChosenPath().empty());
}

TEST_F(ProjectHubFixture, NewProjectFocusesName) {
	ProjectHub hub{_registry};
	HubBuild build;
	hub.BuildContent(build.context, build.root);
	build.context.Update();

	Click(build.context, CenterOf(build.context, FindByText(build.context, build.root, "New Project...")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	build.context.Update();

	UI::ElementHandle card = build.context.GetOpenPopup(0);
	UI::ElementHandle nameField = FindByClass(build.context, card, StringHash("slt-textfield"), 0);
	ASSERT_TRUE(build.context.IsAlive(nameField));
	ASSERT_EQ(build.context.GetTextString(nameField), "MyProject");
	EXPECT_EQ(build.context.GetFocused(), nameField);

	InputEvent typed{};
	typed.type = InputEventType::Text;
	typed.codepoint = U'V';
	build.context.InjectInput(typed);
	build.context.Update();

	EXPECT_EQ(build.context.GetTextString(nameField), "V");
}

TEST(ProjectRelaunchTest, RelaunchKeepsThrottleOptOut) {
	Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
	arguments.push_back("--no-throttle");
	arguments.push_back("--project");
	arguments.push_back("C:/Old");

	str line = ProjectOps::BuildRelaunchCommandLine("C:/bin/Shalltear.exe",
													{arguments.data(), arguments.size()},
													"C:/New");
	EXPECT_EQ(line, "\"C:/bin/Shalltear.exe\" \"--no-throttle\" --project \"C:/New\"") << line;
}
