#include "TestsPCH.h"

#include "ScopedImportCostThreshold.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;

namespace {

	class CountingImporter : public IAssetImporter {
	public:
		std::atomic<u32> imports{0};

		str lastClipRange;

		StringHash GetAssetType() const override { return StringHash("KnobAsset"); }
		Asset* Import(const Path& sourcePath) override {
			imports.fetch_add(1);
			TextAsset* asset = New<TextAsset>(MemTag::Asset);
			asset->Import(sourcePath);
			return asset;
		}
		Asset* ImportWithSettings(const Path& sourcePath, const Path& virtualPath, const JSONValue& settings) override {
			(void)virtualPath;
			lastClipRange = str(settings["clipRange"].AsString());
			return Import(sourcePath);
		}
		void SerializeSettings(const Path& sourcePath, JSONValue& out) const override {
			(void)sourcePath;
			out.AddMember("knob", JSONValue::MakeNumber(1.0));
		}
	};

	class LinkedProductImporter : public IAssetImporter {
	public:
		std::atomic<u32> imports{0};

		StringHash GetAssetType() const override { return StringHash("LinkedAsset"); }
		Asset* Import(const Path& sourcePath) override {
			TextAsset* asset = New<TextAsset>(MemTag::Asset);
			asset->Import(sourcePath);
			return asset;
		}
		Asset* ImportWithSettings(const Path& sourcePath, const Path& virtualPath, const JSONValue& settings,
								  AssetImportContext& context) override {
			(void)virtualPath;
			(void)settings;
			imports.fetch_add(1);

			TextAsset* shared = New<TextAsset>(MemTag::Asset);
			const Guid sharedGuid = context.AddProduct("Shared/One", shared);

			TextAsset* dependent = New<TextAsset>(MemTag::Asset);
			context.AddProduct("Dependents/One", dependent);

			if (sharedGuid.IsValid()) {
				AssetDatabase::LoadAssetByGuid(sharedGuid);
			}
			return Import(sourcePath);
		}
	};

	class ReimportFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		CountingImporter _importer;
		LinkedProductImporter _linked;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_reimport_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root / "Assets");

			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			AssetImporterRegistry::Register(StringHash("KnobAsset"), &_importer, {".knob"});
			AssetImporterRegistry::Register(StringHash("LinkedAsset"), &_linked, {".linked"});
		}

		void TearDown() override {
			AssetImporterRegistry::Unregister(&_importer);
			AssetImporterRegistry::Unregister(&_linked);
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WriteSource(const str& virtualPath, const str& content) {
			Path resolved = FileSystem::Resolve(Path(virtualPath));
			std::filesystem::path fsPath(resolved.ToString());
			std::error_code ec;
			std::filesystem::create_directories(fsPath.parent_path(), ec);
			FileSystem::WriteTextFile(resolved, content);
		}

		str ReadMeta(const str& virtualPath) {
			return FileSystem::ReadTextFile(FileSystem::Resolve(Path(virtualPath + ".meta")), MemTag::Core);
		}

		static JSONValue MakeSettings(std::string_view rangeName) {
			JSONValue settings = JSONValue::MakeObject();
			settings.AddMember("clipRange", JSONValue::MakeString(rangeName));
			return settings;
		}
	};

} // namespace

TEST_F(ReimportFixture, SettingsReplaceOnlyBlock) {
	WriteSource("project://Assets/rig.knob", "content");
	Asset* loaded = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(loaded, nullptr);
	const Guid identity = loaded->assetId;
	AssetDatabase::UnloadAsset(loaded);

	{
		str before = ReadMeta("project://Assets/rig.knob");
		LinearAllocator arena(before.size() + 64, MemTag::Core);
		JSONValue root = JSONParse(before, arena);
		ASSERT_TRUE(root.IsObject());
		ASSERT_TRUE(root["importer"]["settings"]["knob"].IsNumber());
	}

	ASSERT_TRUE(AssetDatabase::WriteImporterSettings(Path("project://Assets/rig.knob"), MakeSettings("Walk")));

	str text = ReadMeta("project://Assets/rig.knob");
	LinearAllocator arena(text.size() + 64, MemTag::Core);
	JSONValue root = JSONParse(text, arena);
	ASSERT_TRUE(root.IsObject());

	EXPECT_EQ(root["importer"]["settings"]["clipRange"].AsString(), "Walk");
	EXPECT_FALSE(root["importer"]["settings"]["knob"].IsNumber());

	EXPECT_EQ(Guid::FromString(root["assetId"].AsString()), identity);
	EXPECT_FALSE(root["type"].AsString().empty());
}

TEST_F(ReimportFixture, SettingsNeedAsset) {
	EXPECT_FALSE(AssetDatabase::WriteImporterSettings(Path("project://Assets/absent.knob"), MakeSettings("Walk")));
	EXPECT_TRUE(ReadMeta("project://Assets/absent.knob").empty());
}

TEST_F(ReimportFixture, SettingReachesAssetOnReimport) {
	WriteSource("project://Assets/rig.knob", "content");
	Asset* first = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(first, nullptr);
	const u32 afterFirstLoad = _importer.imports.load();
	ASSERT_EQ(afterFirstLoad, 1u);

	AssetDatabase::UnloadAsset(first);

	ASSERT_TRUE(AssetDatabase::WriteImporterSettings(Path("project://Assets/rig.knob"), MakeSettings("Walk")));
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.knob")));

	EXPECT_EQ(_importer.imports.load(), afterFirstLoad + 1);
	EXPECT_EQ(_importer.lastClipRange, "Walk");
}

TEST_F(ReimportFixture, ReimportReplacesInstanceDatabaseAnswersWith) {
	WriteSource("project://Assets/rig.knob", "content");
	Asset* first = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(first, nullptr);
	const u64 firstInstance = first->GetInstanceId();
	const Guid identity = first->assetId;
	AssetDatabase::UnloadAsset(first);

	ASSERT_TRUE(AssetDatabase::WriteImporterSettings(Path("project://Assets/rig.knob"), MakeSettings("Walk")));
	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.knob")));

	Asset* second = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(second, nullptr);
	EXPECT_NE(second->GetInstanceId(), firstInstance);
	EXPECT_EQ(second->assetId, identity);
	AssetDatabase::UnloadAsset(second);
}

TEST_F(ReimportFixture, ReimportLeavesNoParkedInstance) {
	WriteSource("project://Assets/rig.knob", "content");
	Asset* first = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(first, nullptr);
	const Guid identity = first->assetId;
	AssetDatabase::UnloadAsset(first);

	ASSERT_TRUE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.knob")));

	Asset* fresh = AssetDatabase::LoadAssetByGuid(identity);
	ASSERT_NE(fresh, nullptr);
	const u64 freshInstance = fresh->GetInstanceId();

	AssetDatabase::CollectGarbage();

	const u32 importsBefore = _importer.imports.load();
	Asset* after = AssetDatabase::LoadAssetByGuid(identity);
	ASSERT_NE(after, nullptr);
	EXPECT_EQ(after->GetInstanceId(), freshInstance);
	EXPECT_EQ(_importer.imports.load(), importsBefore);
	AssetDatabase::UnloadAsset(after);
	AssetDatabase::UnloadAsset(fresh);
}

TEST_F(ReimportFixture, HeldAssetReimportRefused) {
	WriteSource("project://Assets/rig.knob", "content");
	Asset* held = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(held, nullptr);
	const u64 heldInstance = held->GetInstanceId();
	const u32 importsBefore = _importer.imports.load();

	ASSERT_TRUE(AssetDatabase::WriteImporterSettings(Path("project://Assets/rig.knob"), MakeSettings("Walk")));
	EXPECT_FALSE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.knob")));

	EXPECT_EQ(_importer.imports.load(), importsBefore);

	Asset* again = AssetDatabase::LoadAsset(Path("project://Assets/rig.knob"));
	ASSERT_NE(again, nullptr);
	EXPECT_EQ(again->GetInstanceId(), heldInstance);
	AssetDatabase::UnloadAsset(again);
	AssetDatabase::UnloadAsset(held);

	AssetDatabase::CollectGarbage();
	EXPECT_TRUE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.knob")));
	EXPECT_EQ(_importer.imports.load(), importsBefore + 1);
}

TEST_F(ReimportFixture, ReimportNeedsDatabaseAsset) {
	EXPECT_FALSE(AssetDatabase::ReimportAsset(Path("project://Assets/absent.knob")));
	EXPECT_EQ(_importer.imports.load(), 0u);
}

TEST_F(ReimportFixture, SiblingHeldProductAllowsReimport) {
	ScopedImportCostThreshold noCache{ScopedImportCostThreshold::CACHE_NOTHING};
	WriteSource("project://Assets/rig.linked", "content");
	Asset* source = AssetDatabase::LoadAsset(Path("project://Assets/rig.linked"));
	ASSERT_NE(source, nullptr);
	ASSERT_EQ(_linked.imports.load(), 1u);
	AssetDatabase::UnloadAsset(source);

	Vector<AssetProduct> products{GetMemoryResource(MemTag::Asset)};
	AssetDatabase::GetProducts(source->assetId, products);
	ASSERT_EQ(products.size(), 2u);

	EXPECT_TRUE(AssetDatabase::ReimportAsset(Path("project://Assets/rig.linked")));
	EXPECT_EQ(_linked.imports.load(), 2u);
}
