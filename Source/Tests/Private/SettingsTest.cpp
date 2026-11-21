#include "TestsPCH.h"

#include "TestPlatform.h"
#include "Checkout.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Math/MathReflect.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Settings.h"

#include "Graphics/GraphicsSettings.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#undef CreateDirectory
#undef GetFileSize

using namespace Shalltear;

namespace {

	enum class Flavor : u8 {
		Plain = 0,
		Salted,
		Sweet
	};

} // namespace

SLT_REFLECT_ENUM(::Flavor)
SLT_ENUM_VALUE(Plain)
SLT_ENUM_VALUE(Salted)
SLT_ENUM_VALUE(Sweet)
SLT_END_ENUM()

namespace {

	struct SampleSettings {
		SLT_REFLECT_VALUE(SampleSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(flavor)
		SLT_REFLECT_FIELD(portions)
		SLT_REFLECT_FIELD(label)
		SLT_REFLECT_FIELD(enabled)
		SLT_END_REFLECT()

		Flavor flavor = Flavor::Plain;
		i32 portions = 3;
		str label = "default";
		bool enabled = true;
	};

	struct LockedSettings {
		SLT_REFLECT_VALUE(LockedSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(modules)
		SLT_END_REFLECT()

		str modules = "core";
	};

	class ChangeListener : public Object {
	public:
		void OnChanged(const SettingChangedEvent& event) {
			category = str(event.categoryPath);
			field = str(event.fieldName);
			++hits;
		}

		str category;
		str field;
		int hits = 0;
	};

	void* ThisImageBase() {
		return TestPlatform::ImageOf(reinterpret_cast<const void*>(&ThisImageBase));
	}

	struct UnencodableSettings {
		SLT_REFLECT_VALUE(UnencodableSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(where)
		SLT_END_REFLECT()

		Vec3 where{};
	};

	class SettingsFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		SettingsPaths _paths;

		void SetUp() override {
			Settings::Terminate();
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_settings_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);

			_paths.project = Path((_root / "ProjectSettings.ini").generic_string());
			_paths.user = Path((_root / "UserSettings.ini").generic_string());
			_paths.userProject = Path((_root / "UserProjectSettings.ini").generic_string());
		}

		void TearDown() override {
			Settings::Terminate();
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WriteDocument(const Path& path, const char* text) {
			FileSystem::WriteTextFile(path, str(text));
		}

		str ReadDocument(const Path& path) {
			return FileSystem::ReadTextFile(path);
		}
	};

} // namespace

TEST_F(SettingsFixture, AbsentDocumentsKeepDefaults) {
	Settings::Initialize(_paths);
	const SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);

	EXPECT_EQ(settings->flavor, Flavor::Plain);
	EXPECT_EQ(settings->portions, 3);
	EXPECT_EQ(settings->label, "default");
	EXPECT_TRUE(settings->enabled);
}

TEST_F(SettingsFixture, DeclarationIsIdempotentAndFirstOwnerWins) {
	Settings::Initialize(_paths);
	SampleSettings* first = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(first, nullptr);
	first->portions = 11;

	SampleSettings* second = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::User);
	EXPECT_EQ(second, first);
	EXPECT_EQ(second->portions, 11);
}

TEST_F(SettingsFixture, LateProjectReResolves) {
	Settings::Initialize(SettingsPaths{});
	SampleSettings* live = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(live, nullptr);
	EXPECT_EQ(live->portions, 3);

	WriteDocument(_paths.project, "[Sample]\nportions = 9\nlabel = late\n");
	Settings::Initialize(_paths);

	EXPECT_EQ(live->portions, 9);
	EXPECT_EQ(live->label, "late");
	EXPECT_EQ(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), live);
}

TEST_F(SettingsFixture, UnlocatedEditRefused) {
	Settings::Initialize(SettingsPaths{});
	SampleSettings* live = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(live, nullptr);
	live->portions = 7;

	EXPECT_FALSE(Settings::Save());
	EXPECT_FALSE(FileSystem::Exists(_paths.project));
}

TEST_F(SettingsFixture, TextProjectDocumentReadOnly) {
	SettingsPaths paths;
	paths.userProject = _paths.userProject;
	paths.projectText = "[Sample]\nportions = 9\nenabled = 0\n";
	Settings::Initialize(paths);
	SampleSettings* live =
		Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(live, nullptr);
	EXPECT_EQ(live->portions, 9);
	EXPECT_FALSE(live->enabled);
	str stored;
	ASSERT_TRUE(Settings::TryGetStoredValue("Sample", "portions", stored));
	EXPECT_EQ(stored, "9");

	ASSERT_TRUE(Settings::SetFieldEditScope("Sample", "label", SettingScope::UserProject));
	live->label = "mine";
	EXPECT_TRUE(Settings::Save());
	const str written = ReadDocument(_paths.userProject);
	EXPECT_NE(written.find("label = mine"), str::npos) << written;
	EXPECT_EQ(written.find("portions"), str::npos) << written;
	EXPECT_FALSE(FileSystem::Exists(_paths.project));

	live->portions = 5;
	EXPECT_FALSE(Settings::Save());
	EXPECT_FALSE(Settings::Save());
}

TEST_F(SettingsFixture, FileBeatsText) {
	WriteDocument(_paths.project, "[Sample]\nportions = 9\n");
	SettingsPaths paths = _paths;
	paths.projectText = "[Sample]\nportions = 4\n";
	Settings::Initialize(paths);
	const SampleSettings* live = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(live, nullptr);
	EXPECT_EQ(live->portions, 9);
}

TEST_F(SettingsFixture, UnspellableFieldRefused) {
	Settings::Initialize(_paths);
	EXPECT_EQ(Settings::DeclareCategory<UnencodableSettings>("Bad", SettingScope::Project), nullptr);
}

TEST_F(SettingsFixture, NarrowerScopeWinsForOverridableCategory) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\nlabel = fromProject\n");
	WriteDocument(_paths.user, "[Sample]\nportions = 9\n");

	Settings::Initialize(_paths);
	const SampleSettings* settings =
		Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(settings, nullptr);

	EXPECT_EQ(settings->portions, 9);
	EXPECT_EQ(settings->label, "fromProject");
	EXPECT_TRUE(settings->enabled);
}

TEST_F(SettingsFixture, TheNarrowestStoredScopeWinsOverBoth) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\n");
	WriteDocument(_paths.user, "[Sample]\nportions = 9\n");
	WriteDocument(_paths.userProject, "[Sample]\nportions = 12\n");

	Settings::Initialize(_paths);
	const SampleSettings* settings =
		Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(settings, nullptr);
	EXPECT_EQ(settings->portions, 12);
}

TEST_F(SettingsFixture, NonOverridableCategoryRefusesNarrowerScope) {
	WriteDocument(_paths.project, "[Modules]\nmodules = core;physics\n");
	WriteDocument(_paths.user, "[Modules]\nmodules = core;physics;cheats\n");
	WriteDocument(_paths.userProject, "[Modules]\nmodules = nothing\n");

	Settings::Initialize(_paths);
	const LockedSettings* settings =
		Settings::DeclareCategory<LockedSettings>("Modules", SettingScope::Project, false);
	ASSERT_NE(settings, nullptr);
	EXPECT_EQ(settings->modules, "core;physics");
}

TEST_F(SettingsFixture, ScopeNeverWiderThanDeclared) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\n");
	WriteDocument(_paths.user, "[Sample]\nlabel = mine\n");

	Settings::Initialize(_paths);
	const SampleSettings* settings =
		Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::User, true);
	ASSERT_NE(settings, nullptr);
	EXPECT_EQ(settings->label, "mine");
	EXPECT_EQ(settings->portions, 3);
}

TEST_F(SettingsFixture, OnlyOverridesAreStored) {
	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);
	settings->portions = 42;
	ASSERT_TRUE(Settings::Save());

	Config written;
	ASSERT_TRUE(written.Load(_paths.project));
	EXPECT_TRUE(written.Has("Sample", "portions"));
	EXPECT_FALSE(written.Has("Sample", "flavor"));
	EXPECT_FALSE(written.Has("Sample", "label"));
	EXPECT_FALSE(written.Has("Sample", "enabled"));
}

TEST_F(SettingsFixture, EnumIsStoredByConstantName) {
	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);
	settings->flavor = Flavor::Sweet;
	ASSERT_TRUE(Settings::Save());

	Config written;
	ASSERT_TRUE(written.Load(_paths.project));
	EXPECT_EQ(written.GetString("Sample", "flavor"), "Sweet");

	Settings::Terminate();
	Settings::Initialize(_paths);
	const SampleSettings* reloaded = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(reloaded, nullptr);
	EXPECT_EQ(reloaded->flavor, Flavor::Sweet);
}

TEST_F(SettingsFixture, AnEnumStoredByNumberStillLoads) {
	WriteDocument(_paths.project, "[Sample]\nflavor = 1\n");
	Settings::Initialize(_paths);
	const SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);
	EXPECT_EQ(settings->flavor, Flavor::Salted);
}

TEST_F(SettingsFixture, OverrideWritesToSourceScope) {
	WriteDocument(_paths.user, "[Sample]\nportions = 9\n");
	Settings::Initialize(_paths);
	SampleSettings* settings =
		Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(settings, nullptr);
	ASSERT_EQ(settings->portions, 9);

	settings->portions = 10;
	ASSERT_TRUE(Settings::Save());

	Config user;
	ASSERT_TRUE(user.Load(_paths.user));
	EXPECT_EQ(user.GetI64("Sample", "portions"), 10);

	Config project;
	if (project.Load(_paths.project)) {
		EXPECT_FALSE(project.Has("Sample", "portions"));
	}
}

TEST_F(SettingsFixture, RevertRestoresDefault) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\n");
	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);
	ASSERT_EQ(settings->portions, 7);

	EXPECT_TRUE(Settings::RevertField("Sample", "portions"));
	EXPECT_EQ(settings->portions, 3);

	ASSERT_TRUE(Settings::Save());
	Config written;
	ASSERT_TRUE(written.Load(_paths.project));
	EXPECT_FALSE(written.Has("Sample", "portions"));
}

TEST_F(SettingsFixture, RevertingUnknownCategoryOrFieldReportsFailure) {
	Settings::Initialize(_paths);
	Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	EXPECT_FALSE(Settings::RevertField("Nope", "portions"));
	EXPECT_FALSE(Settings::RevertField("Sample", "notAField"));
}

TEST_F(SettingsFixture, UndeclaredValueSurvivesSave) {
	const char* document =
		"[Sample]\n"
		"portions = 7\n"
		"\n"
		"[GhostModule]\n"
		"someKey = someValue\n"
		"another = 12\n";
	WriteDocument(_paths.project, document);

	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(settings, nullptr);
	settings->portions = 8;
	ASSERT_TRUE(Settings::Save());

	Config written;
	ASSERT_TRUE(written.Load(_paths.project));
	EXPECT_EQ(written.GetI64("Sample", "portions"), 8);
	EXPECT_EQ(written.GetString("GhostModule", "someKey"), "someValue");
	EXPECT_EQ(written.GetI64("GhostModule", "another"), 12);

	EXPECT_NE(ReadDocument(_paths.project).find("[GhostModule]\nsomeKey = someValue\nanother = 12"), str::npos);
}

TEST_F(SettingsFixture, UndeclaredDocumentRoundTrips) {
	const char* document =
		"[GhostModule]\n"
		"someKey = someValue\n";
	WriteDocument(_paths.project, document);

	Settings::Initialize(_paths);
	ASSERT_TRUE(Settings::Save());
	EXPECT_EQ(ReadDocument(_paths.project), document);
}

TEST_F(SettingsFixture, VSyncDefaultsToWindowHint) {
	Settings::Initialize(_paths);
	const GraphicsSettings* graphics =
		Settings::DeclareCategory<GraphicsSettings>("Graphics", SettingScope::Project, true);
	ASSERT_NE(graphics, nullptr);
	EXPECT_EQ(graphics->vsync, VSyncMode::WindowHint);
	EXPECT_EQ(graphics->adapter, -1);
}

TEST_F(SettingsFixture, VSyncResolvesThreeStates) {
	WriteDocument(_paths.project, "[Graphics]\nvsync = Off\n");
	Settings::Initialize(_paths);
	const GraphicsSettings* graphics =
		Settings::DeclareCategory<GraphicsSettings>("Graphics", SettingScope::Project, true);
	ASSERT_NE(graphics, nullptr);
	EXPECT_EQ(graphics->vsync, VSyncMode::Off);
}

TEST_F(SettingsFixture, ValidationDefaultTracksTheBuildConfiguration) {
	Settings::Initialize(_paths);
	const GraphicsSettings* graphics =
		Settings::DeclareCategory<GraphicsSettings>("Graphics", SettingScope::Project, true);
	ASSERT_NE(graphics, nullptr);
#if defined(NDEBUG) || defined(SHALLTEAR_CONFIG_RELEASE)
	EXPECT_FALSE(graphics->validation);
#else
	EXPECT_TRUE(graphics->validation);
#endif
}

TEST_F(SettingsFixture, CategoriesListScopeAndType) {
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true), nullptr);
	ASSERT_NE(Settings::DeclareCategory<LockedSettings>("Locked", SettingScope::Project), nullptr);

	Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Editor)};
	Settings::GetCategories(categories);
	ASSERT_EQ(categories.size(), 2u);
	EXPECT_EQ(categories[0].path, "Sample");
	EXPECT_TRUE(categories[0].overridable);
	EXPECT_EQ(categories[0].type, SampleSettings::GetReflectType());
	EXPECT_NE(categories[0].live, nullptr);
	EXPECT_FALSE(categories[1].overridable);
}

TEST_F(SettingsFixture, FieldStateShowsDefault) {
	WriteDocument(_paths.user, "[Sample]\nportions = 9\n");
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true), nullptr);

	SettingFieldState stored;
	ASSERT_TRUE(Settings::GetFieldState("Sample", "portions", stored));
	EXPECT_TRUE(stored.isOverride);
	EXPECT_EQ(stored.resolvedFrom, SettingScope::User);
	EXPECT_EQ(stored.editsLandIn, SettingScope::User);

	SettingFieldState untouched;
	ASSERT_TRUE(Settings::GetFieldState("Sample", "label", untouched));
	EXPECT_FALSE(untouched.isOverride);
	EXPECT_EQ(untouched.editsLandIn, SettingScope::Project);
}

TEST_F(SettingsFixture, EditScopeMustBeReadable) {
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::User, true), nullptr);
	ASSERT_NE(Settings::DeclareCategory<LockedSettings>("Locked", SettingScope::Project), nullptr);

	EXPECT_TRUE(Settings::SetFieldEditScope("Sample", "portions", SettingScope::UserProject));
	EXPECT_FALSE(Settings::SetFieldEditScope("Sample", "portions", SettingScope::Project));
	EXPECT_FALSE(Settings::SetFieldEditScope("Locked", "modules", SettingScope::User));
	EXPECT_TRUE(Settings::SetFieldEditScope("Locked", "modules", SettingScope::Project));
	EXPECT_FALSE(Settings::SetFieldEditScope("Sample", "nosuchfield", SettingScope::User));
}

TEST_F(SettingsFixture, UserEditKeepsCommitted) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\n");
	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(settings, nullptr);
	ASSERT_EQ(settings->portions, 7);

	ASSERT_TRUE(Settings::SetFieldEditScope("Sample", "portions", SettingScope::User));
	settings->portions = 12;
	ASSERT_TRUE(Settings::Save());

	EXPECT_NE(ReadDocument(_paths.user).find("portions = 12"), str::npos);
	EXPECT_NE(ReadDocument(_paths.project).find("portions = 7"), str::npos);

	SettingFieldState state;
	ASSERT_TRUE(Settings::GetFieldState("Sample", "portions", state));
	EXPECT_EQ(state.resolvedFrom, SettingScope::User);
}

TEST_F(SettingsFixture, WriteClearsNarrowerScopes) {
	WriteDocument(_paths.userProject, "[Sample]\nportions = 1\n");
	Settings::Initialize(_paths);
	SampleSettings* settings = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(settings, nullptr);
	ASSERT_EQ(settings->portions, 1);

	ASSERT_TRUE(Settings::SetFieldEditScope("Sample", "portions", SettingScope::User));
	settings->portions = 5;
	ASSERT_TRUE(Settings::Save());

	EXPECT_EQ(ReadDocument(_paths.userProject).find("portions"), str::npos);
	EXPECT_NE(ReadDocument(_paths.user).find("portions = 5"), str::npos);
}

TEST_F(SettingsFixture, NotifyChangedPublishesIdentityAndNothingElse) {
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), nullptr);

	auto subscriber = MakeShared<ChangeListener>();
	Handle<ChangeListener> handle(subscriber.get());
	SettingsEvents::Changed.Subscribe(EventBus::GetGlobal(), handle, &ChangeListener::OnChanged);

	Settings::NotifyChanged("Sample", "portions");

	EXPECT_EQ(subscriber->hits, 1);
	EXPECT_EQ(subscriber->category, "Sample");
	EXPECT_EQ(subscriber->field, "portions");

	SettingsEvents::Changed.Unsubscribe(EventBus::GetGlobal(), handle.GetInstanceId());
}

TEST_F(SettingsFixture, SweepKeepsDocument) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\n");
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), nullptr);

	EXPECT_EQ(Settings::UnregisterOwnedBy(ThisImageBase()), 1u);

	Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Core)};
	Settings::GetCategories(categories);
	EXPECT_TRUE(categories.empty());

	ASSERT_TRUE(Settings::Save());
	EXPECT_EQ(ReadDocument(_paths.project), "[Sample]\nportions = 7\n");
}

TEST_F(SettingsFixture, ASweepOfAnotherImageTakesNothing) {
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), nullptr);

	void* other = TestPlatform::GetSystemImage();
	ASSERT_NE(other, nullptr);
	EXPECT_EQ(Settings::UnregisterOwnedBy(other), 0u);

	Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Core)};
	Settings::GetCategories(categories);
	EXPECT_EQ(categories.size(), 1u);
}

TEST_F(SettingsFixture, ANullOwnerSweepsNothing) {
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), nullptr);

	EXPECT_EQ(Settings::UnregisterOwnedBy(nullptr), 0u);

	Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Core)};
	Settings::GetCategories(categories);
	EXPECT_EQ(categories.size(), 1u);
}

TEST_F(SettingsFixture, RedeclareRestoresValue) {
	WriteDocument(_paths.project, "[Sample]\nportions = 7\nlabel = kept\n");
	Settings::Initialize(_paths);
	ASSERT_NE(Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project), nullptr);
	ASSERT_EQ(Settings::UnregisterOwnedBy(ThisImageBase()), 1u);

	const SampleSettings* second = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second->portions, 7);
	EXPECT_EQ(second->label, "kept");
}

namespace {

	struct ConfigConsumer {
		const char* path;
		const char* why;
	};

	const ConfigConsumer configAllowlist[] = {
		{"Source/Core/Private/Config.cpp", "Config itself"},
		{"Source/Core/Private/Settings.cpp", "scope documents are INI files"},
		{"Source/Core/Public/Shalltear.h", "SDK umbrella"},
		{"Source/CSharp/Private/Generated/Config.generated.cpp", "C# binding of Config"},
		{"Source/Editor/Private/SettingsOps.cpp", "legacy file migration"},
		{"Source/Editor/Private/EditorConfig.h", "Layout.ini is state"},
		{"Source/Editor/Private/EditorLayoutOps.cpp", "layout document (state)"},
		{"Source/Editor/Private/BuildProfile.cpp", "per target overrides (not per scope)"},
		{"Source/Editor/Private/BuildPipeline.cpp", "build results ledger (a report)"},
		{"Source/Editor/Private/Panels/BuildProfilesPanel.cpp", "panel's last result column"},
		{"Source/Editor/Private/PackageOps.cpp", "ScaffoldEnabledSet writes an unmounted document"},
		{"Source/Editor/Private/ProjectOps.cpp", "hub registry + project identity"},
		{"Source/Editor/Private/ScriptProjectOps.cpp", "[Script] module + [Project] name"},
	};

	bool IncludesConfigHeader(const std::filesystem::path& file) {
		std::ifstream stream(file);
		str line;
		while (std::getline(stream, line)) {
			size_t start = line.find_first_not_of(" \t");
			if (start == str::npos || line.compare(start, 8, "#include") != 0) {
				continue;
			}
			if (line.find("Shalltear/Core/Config.h") != str::npos) {
				return true;
			}
		}
		return false;
	}

} // namespace

TEST(SettingsSourceGate, OnlyTheAllowlistedConsumersReachRawConfig) {
	std::filesystem::path root = Checkout::Root();
	ASSERT_FALSE(root.empty()) << std::filesystem::current_path().string();
	std::filesystem::path sourceDir = root / "Source";
	ASSERT_TRUE(std::filesystem::exists(sourceDir)) << root.string();

	Vector<str> found{GetMemoryResource(MemTag::Core)};
	u32 scanned = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDir, ec)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		str extension = entry.path().extension().string();
		if (extension != ".cpp" && extension != ".h") {
			continue;
		}
		str relative = std::filesystem::relative(entry.path(), root, ec).generic_string();
		if (relative.rfind("Source/Tests/", 0) == 0 || relative.rfind("Source/Benchmarks/", 0) == 0) {
			continue;
		}
		++scanned;
		if (IncludesConfigHeader(entry.path())) {
			found.push_back(relative);
		}
	}
	EXPECT_GT(scanned, 100u);

	for (const str& path : found) {
		bool allowed = false;
		for (const ConfigConsumer& consumer : configAllowlist) {
			allowed = allowed || path == consumer.path;
		}
		EXPECT_TRUE(allowed);
	}

	for (const ConfigConsumer& consumer : configAllowlist) {
		bool present = false;
		for (const str& path : found) {
			present = present || path == consumer.path;
		}
		EXPECT_TRUE(present);
	}
}

TEST(SettingsSourceGate, HeaderHidesRawConfig) {
	std::filesystem::path root = Checkout::Root();
	ASSERT_FALSE(root.empty());
	std::filesystem::path header = root / "Source/Core/Public/Shalltear/Core/Settings.h";
	ASSERT_TRUE(std::filesystem::exists(header)) << header.string();

	EXPECT_FALSE(IncludesConfigHeader(header));
	EXPECT_TRUE(IncludesConfigHeader(root / "Source/Core/Private/Settings.cpp"));
}

TEST_F(SettingsFixture, SavingCreatesDocumentsDirectoryNotFailing) {
	const std::filesystem::path nested = _root / "Library" / "Editor";
	ASSERT_FALSE(std::filesystem::exists(nested));
	_paths.userProject = Path((nested / "UserProjectSettings.ini").generic_string());

	Settings::Initialize(_paths);
	SampleSettings* live = Settings::DeclareCategory<SampleSettings>("Sample", SettingScope::Project, true);
	ASSERT_NE(live, nullptr);

	ASSERT_TRUE(Settings::SetFieldEditScope("Sample", "portions", SettingScope::UserProject));
	live->portions = 41;
	Settings::NotifyChanged("Sample", "portions");

	EXPECT_TRUE(Settings::Save());
	EXPECT_TRUE(std::filesystem::is_regular_file(nested / "UserProjectSettings.ini"));

	const str text = FileSystem::ReadTextFile(Path((nested / "UserProjectSettings.ini").generic_string()));
	EXPECT_NE(text.find("41"), str::npos) << text;
}
