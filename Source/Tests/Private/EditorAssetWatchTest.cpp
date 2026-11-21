#include "TestsPCH.h"

#include "Checkout.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/UI/StyleSheetAsset.h"

#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Editor/EditorAssetWatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace Shalltear;

namespace {

	class AssetWatchTest : public ::testing::Test {
	protected:
		void SetUp() override { EditorAssetWatch::Initialize(); }
		void TearDown() override { EditorAssetWatch::Terminate(); }

		static u32 CountOf(const char* path) {
			u32 count = 0;
			for (const Path& changed : EditorAssetWatch::GetChangedThisFrame()) {
				if (changed.ToString() == path) {
					++count;
				}
			}
			return count;
		}
	};

	TEST_F(AssetWatchTest, ChangeHiddenUntilPolled) {
		EditorAssetWatch::NotifyChanged(Path("project://Textures/Rock.png"));
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());

		EditorAssetWatch::PollChanges();
		EXPECT_EQ(CountOf("project://Textures/Rock.png"), 1u);
	}

	TEST_F(AssetWatchTest, ChangeIsReportedForOneFrame) {
		EditorAssetWatch::NotifyChanged(Path("project://Materials/Rock.mat"));
		EditorAssetWatch::PollChanges();
		ASSERT_EQ(CountOf("project://Materials/Rock.mat"), 1u);

		EditorAssetWatch::PollChanges();
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());
	}

	TEST_F(AssetWatchTest, OneFileTouchedRepeatedlyIsOneChange) {
		for (int i = 0; i < 5; ++i) {
			EditorAssetWatch::NotifyChanged(Path("project://Textures/Atlas.png"));
		}
		EditorAssetWatch::NotifyChanged(Path("project://Textures/Other.png"));
		EditorAssetWatch::PollChanges();

		EXPECT_EQ(CountOf("project://Textures/Atlas.png"), 1u);
		EXPECT_EQ(CountOf("project://Textures/Other.png"), 1u);
		EXPECT_EQ(EditorAssetWatch::GetChangedThisFrame().size(), 2u);
	}

	TEST_F(AssetWatchTest, EditorsSidecarWritesAreNotChanges) {
		EditorAssetWatch::NotifyChanged(Path("project://Textures/Rock.png.meta"));
		EditorAssetWatch::NotifyChanged(Path("project://Library/index.assets.db"));
		EditorAssetWatch::NotifyChanged(Path("project://Textures/Rock.png"));
		EditorAssetWatch::PollChanges();

		EXPECT_EQ(EditorAssetWatch::GetChangedThisFrame().size(), 1u);
		EXPECT_EQ(CountOf("project://Textures/Rock.png"), 1u);
	}

	TEST_F(AssetWatchTest, BulkChangeSaysEverythingNotNamingSome) {
		for (int i = 0; i < 4000; ++i) {
			EditorAssetWatch::NotifyChanged(Path(std::format("project://Bulk/{}.png", i)));
		}
		EditorAssetWatch::PollChanges();

		EXPECT_TRUE(EditorAssetWatch::DidOverflowThisFrame());
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());

		EditorAssetWatch::PollChanges();
		EXPECT_FALSE(EditorAssetWatch::DidOverflowThisFrame());
	}

	TEST_F(AssetWatchTest, WatcherReportsLookupPath) {
		std::filesystem::path root = std::filesystem::temp_directory_path() /
									 ("shalltear_watch_" +
									  std::to_string(static_cast<u64>(
										  std::chrono::steady_clock::now().time_since_epoch().count())));
		std::filesystem::create_directories(root / "Assets" / "Textures");
		FileSystem::Mount("project", Path(root.generic_string()));
		EditorAssetWatch::PollChanges();

		str physical = (root / "Assets" / "Textures" / "Rock.png").string();
		EditorAssetWatch::NotifyChanged(Path(physical));
		EditorAssetWatch::PollChanges();

		ASSERT_EQ(EditorAssetWatch::GetChangedThisFrame().size(), 1u) << physical;
		EXPECT_EQ(EditorAssetWatch::GetChangedThisFrame()[0].ToString(), "project://Assets/Textures/Rock.png");

		EditorAssetWatch::NotifyChanged(Path("C:/Elsewhere/Rock.png"));
		EditorAssetWatch::PollChanges();
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());

		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}

	TEST_F(AssetWatchTest, WatcherFollowsProjectMount) {
		std::filesystem::path base = std::filesystem::temp_directory_path() /
									 ("shalltear_watch_gen_" +
									  std::to_string(static_cast<u64>(
										  std::chrono::steady_clock::now().time_since_epoch().count())));
		std::filesystem::path first = base / "First";
		std::filesystem::path second = base / "Second";
		std::filesystem::create_directories(first / "Assets");
		std::filesystem::create_directories(second / "Assets");

		FileSystem::Mount("project", Path(first.generic_string()));
		EditorAssetWatch::PollChanges();

		for (u32 i = 0; i < 200; ++i) {
			EditorAssetWatch::PollChanges();
		}
		EditorAssetWatch::NotifyChanged(Path((first / "Assets" / "Rock.png").string()));
		EditorAssetWatch::PollChanges();
		EXPECT_EQ(CountOf("project://Assets/Rock.png"), 1u);

		FileSystem::Mount("project", Path(second.generic_string()));
		EditorAssetWatch::PollChanges();

		EditorAssetWatch::NotifyChanged(Path((second / "Assets" / "Rock.png").string()));
		EditorAssetWatch::PollChanges();
		EXPECT_EQ(CountOf("project://Assets/Rock.png"), 1u);

		EditorAssetWatch::NotifyChanged(Path((first / "Assets" / "Rock.png").string()));
		EditorAssetWatch::PollChanges();
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());

		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(base, ec);
	}

	TEST_F(AssetWatchTest, ItAnswersSafelyWhileNotInitialized) {
		EditorAssetWatch::Terminate();

		EditorAssetWatch::NotifyChanged(Path("project://Textures/Rock.png"));
		EditorAssetWatch::PollChanges();
		EXPECT_TRUE(EditorAssetWatch::GetChangedThisFrame().empty());
		EXPECT_FALSE(EditorAssetWatch::DidOverflowThisFrame());

		EditorAssetWatch::Initialize();
	}

	class AssetReloadTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_reload_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root / "Assets");
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			EditorAssetOps::Initialize();
		}

		void TearDown() override {
			EditorAssetOps::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void Write(const char* text) {
			ASSERT_TRUE(FileSystem::WriteTextFile(FileSystem::Resolve(Path(SHEET)), text));
		}

		StyleSheetAsset* Hold() {
			Asset* asset = AssetDatabase::LoadAsset(Path(SHEET));
			EXPECT_NE(asset, nullptr);
			return static_cast<StyleSheetAsset*>(asset);
		}

		static u32 Reload(std::function<void()>* conflict = nullptr, u32* asked = nullptr) {
			const Path changed[] = {Path(SHEET)};
			return EditorAssetOps::ReloadChanged(changed, [conflict, asked](const Path&, std::function<void()> reload) {
				if (asked) {
					++*asked;
				}
				if (conflict) {
					*conflict = std::move(reload);
				}
			});
		}

		static constexpr const char* SHEET = "project://Assets/Hud.slss";
	};

	TEST_F(AssetReloadTest, ExternalChangeRefillsHeldAsset) {
		Write(".a { width: 1px; }");
		StyleSheetAsset* sheet = Hold();
		ASSERT_NE(sheet, nullptr);

		Write(".a { width: 2px; }");
		EditorAssetWatch::Initialize();
		EditorAssetWatch::NotifyChanged(Path(SHEET));
		EditorAssetWatch::PollChanges();
		EXPECT_EQ(EditorAssetOps::ReloadChanged(EditorAssetWatch::GetChangedThisFrame(), {}), 1u);
		EditorAssetWatch::Terminate();

		EXPECT_EQ(sheet->source, ".a { width: 2px; }");
		EXPECT_EQ(sheet->version, 1u);
		AssetDatabase::UnloadAsset(sheet);
	}

	TEST_F(AssetReloadTest, UnheldFileWaitsForLoad) {
		Write(".a { width: 1px; }");
		StyleSheetAsset* sheet = Hold();
		ASSERT_NE(sheet, nullptr);
		AssetDatabase::UnloadAsset(sheet);
		AssetDatabase::CollectGarbage();

		Write(".a { width: 2px; }");
		EXPECT_EQ(Reload(), 0u);
		StyleSheetAsset* again = Hold();
		ASSERT_NE(again, nullptr);
		EXPECT_EQ(again->source, ".a { width: 2px; }");
		AssetDatabase::UnloadAsset(again);
	}

	TEST_F(AssetReloadTest, UnsavedEditsAskBeforeReplace) {
		Write(".a { width: 1px; }");
		StyleSheetAsset* sheet = Hold();
		ASSERT_NE(sheet, nullptr);
		sheet->source = ".a { width: 9px; }";
		EditorAssetOps::MarkDirty(Handle<Asset>(sheet));

		Write(".a { width: 2px; }");
		std::function<void()> reload;
		u32 asked = 0;
		EXPECT_EQ(Reload(&reload, &asked), 0u);
		ASSERT_EQ(asked, 1u);
		EXPECT_EQ(sheet->source, ".a { width: 9px; }");

		ASSERT_TRUE(static_cast<bool>(reload));
		reload();
		EXPECT_EQ(sheet->source, ".a { width: 2px; }");
		EXPECT_FALSE(EditorAssetOps::IsDirty(Handle<Asset>(sheet)));
		AssetDatabase::UnloadAsset(sheet);
	}

	TEST_F(AssetReloadTest, EditorsSaveIsNotChangeFromOutside) {
		Write(".a { width: 1px; }");
		StyleSheetAsset* sheet = Hold();
		ASSERT_NE(sheet, nullptr);
		sheet->source = ".a { width: 3px; }";
		EditorAssetOps::MarkDirty(Handle<Asset>(sheet));
		EditorAssetOps::SaveAsset(Handle<Asset>(sheet));
		sheet->source = ".a { width: 4px; }";
		EditorAssetOps::MarkDirty(Handle<Asset>(sheet));

		u32 asked = 0;
		EXPECT_EQ(Reload(nullptr, &asked), 0u);
		EXPECT_EQ(asked, 0u);
		EXPECT_EQ(sheet->source, ".a { width: 4px; }");

		Write(".a { width: 5px; }");
		EXPECT_EQ(Reload(nullptr, &asked), 0u);
		EXPECT_EQ(asked, 1u);
		AssetDatabase::UnloadAsset(sheet);
	}

} // namespace

TEST(EditorReimportTest, OnlyEditorAssetOpsReimports) {
	const std::filesystem::path source = Checkout::Find("Source");
	ASSERT_FALSE(source.empty());
	u32 scanned = 0;
	std::vector<std::string> callers;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
		const std::string extension = entry.path().extension().string();
		if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h")) {
			continue;
		}
		const std::filesystem::path relative = entry.path().lexically_relative(source);
		const std::string top = relative.begin()->string();
		if (top == "Core" || top == "Tests") {
			continue;
		}
		++scanned;
		std::ifstream file(entry.path(), std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (text.find("AssetDatabase::ReimportAsset(") != std::string::npos) {
			callers.push_back(relative.generic_string());
		}
	}
	EXPECT_GT(scanned, 500u);
	const std::vector<std::string> expected{"CSharp/Private/Generated/AssetDatabaseSlots.generated.cpp", "Editor/Private/EditorAssetOps.cpp"};
	std::sort(callers.begin(), callers.end());
	EXPECT_EQ(callers, expected);
}
