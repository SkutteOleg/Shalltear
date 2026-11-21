#include "ScopedSettingsStore.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <iterator>
#include <span>
#include <vector>

using namespace Shalltear;

namespace {

	str GenericUTF8(const std::filesystem::path& path) {
		const std::u8string text = path.generic_u8string();
		return str(reinterpret_cast<const char*>(text.data()), text.size());
	}

	std::filesystem::path FreshTempFolder(const char* prefix) {
		static std::atomic<u64> counter{0};
		const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
		return std::filesystem::temp_directory_path() /
			   (str(prefix) + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
	}

	std::ptrdiff_t CountEntries(const std::filesystem::path& folder) {
		return std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator());
	}

} // anonymous namespace

TEST(FileSystem, PathNormalization) {
	Path p1("C:\\foo\\bar");
	EXPECT_EQ(p1.ToString(), "C:/foo/bar");

	Path p2("some//path\\\\here/");
	EXPECT_EQ(p2.ToString(), "some//path//here");
}

TEST(FileSystem, PathMethods) {
	Path p("C:/engine/assets/texture.png");
	EXPECT_EQ(p.GetDirectory(), "C:/engine/assets");
	EXPECT_EQ(p.GetFilename(), "texture.png");
	EXPECT_EQ(p.GetExtension(), ".png");
	EXPECT_TRUE(p.IsAbsolute());

	Path rel("assets/texture.png");
	EXPECT_FALSE(rel.IsAbsolute());
}

TEST(FileSystem, PathConcatenation) {
	Path p("C:/engine");
	Path combined = p / "assets" / Path("texture.png");
	EXPECT_EQ(combined.ToString(), "C:/engine/assets/texture.png");
}

TEST(FileSystem, MountAndResolve) {
	FileSystem::Mount("engine", Path("C:/actual_engine_dir"));
	Path resolved = FileSystem::Resolve(Path("engine://shaders/basic.glsl"));
	EXPECT_EQ(resolved.ToString(), "C:/actual_engine_dir/shaders/basic.glsl");
	FileSystem::Unmount("engine");
}

TEST(FileSystem, VirtualizeIsTheInverseOfResolve) {
	FileSystem::Mount("engine", Path("C:/actual_engine_dir"));
	Path virtualPath("engine://shaders/basic.glsl");
	EXPECT_EQ(FileSystem::Virtualize(FileSystem::Resolve(virtualPath)).ToString(), virtualPath.ToString());
	EXPECT_EQ(FileSystem::Virtualize(Path("C:\\actual_engine_dir\\shaders\\basic.glsl")).ToString(),
			  virtualPath.ToString());
	EXPECT_EQ(FileSystem::Virtualize(Path("C:/actual_engine_dir")).ToString(), "C:/actual_engine_dir");
	FileSystem::Unmount("engine");
}

TEST(FileSystem, VirtualizeLeavesWhatItCannotName) {
	FileSystem::Mount("engine", Path("C:/actual_engine_dir"));
	EXPECT_EQ(FileSystem::Virtualize(Path("engine://a.glsl")).ToString(), "engine://a.glsl");
	EXPECT_EQ(FileSystem::Virtualize(Path("D:/elsewhere/a.glsl")).ToString(), "D:/elsewhere/a.glsl");
	EXPECT_EQ(FileSystem::Virtualize(Path("C:/actual_engine_dir_backup/a.glsl")).ToString(),
			  "C:/actual_engine_dir_backup/a.glsl");
	FileSystem::Unmount("engine");
}

TEST(FileSystem, VirtualizePrefersTheMostSpecificMount) {
	FileSystem::Mount("project", Path("C:/proj"));
	FileSystem::Mount("content", Path("C:/proj/Assets"));
	EXPECT_EQ(FileSystem::Virtualize(Path("C:/proj/Assets/a.scene")).ToString(), "content://a.scene");
	EXPECT_EQ(FileSystem::Virtualize(Path("C:/proj/Source/a.cpp")).ToString(), "project://Source/a.cpp");
	FileSystem::Unmount("content");
	FileSystem::Unmount("project");
}

TEST(FileSystem, MountGenerationTracksMounts) {
	u64 start = FileSystem::GetMountGeneration();

	FileSystem::Mount("probe", Path("C:/probe"));
	u64 afterMount = FileSystem::GetMountGeneration();
	EXPECT_NE(afterMount, start);

	EXPECT_EQ(FileSystem::Resolve(Path("probe://thing.txt")).ToString(), "C:/probe/thing.txt");
	EXPECT_EQ(FileSystem::Virtualize(Path("C:/probe/thing.txt")).ToString(), "probe://thing.txt");
	EXPECT_EQ(FileSystem::GetMountGeneration(), afterMount);

	FileSystem::Mount("probe", Path("C:/probe"));
	u64 afterRemount = FileSystem::GetMountGeneration();
	EXPECT_NE(afterRemount, afterMount);

	FileSystem::Unmount("probe");
	EXPECT_NE(FileSystem::GetMountGeneration(), afterRemount);
}

TEST(FileSystem, ReadWriteTextFile) {
	Path testPath("test_file.txt");
	str content = "Hello, Shalltear FileSystem!";

	EXPECT_TRUE(FileSystem::WriteTextFile(testPath, content));
	EXPECT_TRUE(FileSystem::Exists(testPath));

	str readContent = FileSystem::ReadTextFile(testPath);
	EXPECT_EQ(content, readContent);

	EXPECT_TRUE(FileSystem::GetFileSize(testPath) > 0);
	EXPECT_TRUE(FileSystem::GetLastWriteTime(testPath) > 0);

	std::filesystem::remove(testPath.ToString());
}

TEST(FileSystem, TextRoundTripIsByteExact) {
	Path testPath("test_newlines.txt");
	str content = "line one\nline two\n\nline four\n";

	ASSERT_TRUE(FileSystem::WriteTextFile(testPath, content));
	EXPECT_EQ(FileSystem::ReadTextFile(testPath), content);
	EXPECT_EQ(FileSystem::GetFileSize(testPath), content.size());

	str readBack = FileSystem::ReadTextFile(testPath);
	ASSERT_TRUE(FileSystem::WriteTextFile(testPath, readBack));
	EXPECT_EQ(FileSystem::ReadTextFile(testPath), content);

	std::filesystem::remove(testPath.ToString());
}

TEST(FileSystem, UnixTimestampsAreWallClock) {
	Path testPath("test_timestamps.txt");
	ASSERT_TRUE(FileSystem::WriteTextFile(testPath, "stamped"));

	u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
								   std::chrono::system_clock::now().time_since_epoch())
								   .count());
	u64 written = FileSystem::GetLastWriteTimeUnix(testPath);
	u64 created = FileSystem::GetCreationTimeUnix(testPath);

	constexpr u64 SANE_FLOOR = 1577836800ull;
	EXPECT_GT(written, SANE_FLOOR);
	EXPECT_LT(written, now + 60);
	EXPECT_GT(created, SANE_FLOOR);
	EXPECT_LT(created, now + 60);

	EXPECT_EQ(FileSystem::GetLastWriteTimeUnix(Path("no_such_file.txt")), 0u);
	EXPECT_EQ(FileSystem::GetCreationTimeUnix(Path("no_such_file.txt")), 0u);

	std::filesystem::remove(testPath.ToString());
}

TEST(FileSystem, UserDataDirectoryIsWritable) {
	Path userData = FileSystem::GetUserDataDirectory();
	Path home = FileSystem::GetUserHomeDirectory();
	ASSERT_FALSE(userData.ToString().empty());
	EXPECT_TRUE(FileSystem::IsDirectory(userData));
	EXPECT_EQ(userData.ToString().find('\\'), str::npos);

	ASSERT_FALSE(home.ToString().empty());
	EXPECT_TRUE(FileSystem::IsDirectory(home));
	EXPECT_EQ(home.ToString().find('\\'), str::npos);
}

TEST(FileSystem, NamesOutsideAsciiRoundTrip) {
	const std::filesystem::path root = FreshTempFolder("shalltear_names_");
	const std::filesystem::path folder = root / u8"é Олег 太郎";
	const str folderText = GenericUTF8(folder);
	const Path note(folderText + "/note.txt");

	ASSERT_TRUE(FileSystem::CreateDirectory(Path(folderText)));
	ASSERT_TRUE(FileSystem::WriteTextFile(note, "text"));
	EXPECT_TRUE(FileSystem::IsDirectory(Path(folderText)));
	EXPECT_TRUE(FileSystem::Exists(note));
	EXPECT_EQ(FileSystem::ReadTextFile(note), "text");
	EXPECT_EQ(FileSystem::GetFileSize(note), 4u);

	EXPECT_TRUE(std::filesystem::exists(folder / u8"note.txt"));
	EXPECT_EQ(CountEntries(root), 1);
	EXPECT_EQ(std::filesystem::path(folderText), folder);
	EXPECT_EQ(folder.generic_string(), folderText);

	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, UserDataOutsideAsciiReceivesFiles) {
	const std::filesystem::path root = FreshTempFolder("shalltear_user_data_");
	const std::filesystem::path appData = root / u8"appdata é Олег 太郎";
	{
		ScopedUserDataDirectory scope(appData);
		const Path userData = FileSystem::GetUserDataDirectory();
		EXPECT_EQ(userData.ToString(), GenericUTF8(appData));
		ASSERT_TRUE(FileSystem::CreateDirectory(userData / "Shalltear"));
		EXPECT_TRUE(FileSystem::WriteTextFile(userData / "Shalltear" / "UserSettings.ini", "[Game]\nvolume = 1\n"));
	}

	EXPECT_TRUE(std::filesystem::exists(appData / u8"Shalltear" / u8"UserSettings.ini"));
	EXPECT_EQ(CountEntries(root), 1);

	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

TEST(FileSystem, ReadWriteBinaryFile) {
	Path testPath("test_binary.dat");

	Buffer writeBuffer(5, MemTag::Unknown);
	writeBuffer.data[0] = 0xDE;
	writeBuffer.data[1] = 0xAD;
	writeBuffer.data[2] = 0xBE;
	writeBuffer.data[3] = 0xEF;
	writeBuffer.data[4] = 0x00;

	EXPECT_TRUE(FileSystem::WriteFile(testPath, writeBuffer));

	Buffer readBuffer = FileSystem::ReadFile(testPath, MemTag::Unknown);
	EXPECT_EQ(readBuffer.size, 5);
	EXPECT_EQ(readBuffer.data[0], 0xDE);
	EXPECT_EQ(readBuffer.data[1], 0xAD);
	EXPECT_EQ(readBuffer.data[2], 0xBE);
	EXPECT_EQ(readBuffer.data[3], 0xEF);
	EXPECT_EQ(readBuffer.data[4], 0x00);

	std::filesystem::remove(testPath.ToString());
}

TEST(FileSystem, FileWatcherBasic) {
	Path watchDir("test_watch_dir");
	FileSystem::CreateDirectory(watchDir);

	std::atomic<int> callbackFiredCount{0};
	FileAction lastAction = FileAction::Added;
	Path lastPath;

	{
		FileWatcher watcher(watchDir, [&](const Path& p, FileAction action) {
			lastAction = action;
			lastPath = p;
			callbackFiredCount++;
		});

		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		Path testFile = watchDir / "new_file.txt";
		FileSystem::WriteTextFile(testFile, "test");

		for (int i = 0; i < 20; ++i) {
			if (callbackFiredCount > 0)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	EXPECT_GT(callbackFiredCount.load(), 0);
	EXPECT_EQ(lastPath.GetFilename(), "new_file.txt");

	std::filesystem::remove_all(watchDir.ToString());
}

namespace {

	u8 PatternByte(u64 index) {
		return static_cast<u8>((index * 2654435761ull) >> 13);
	}

	std::filesystem::path WritePatternFile(const char* name, u64 size) {
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		Buffer bytes(static_cast<size_t>(size), MemTag::Unknown);
		for (u64 i = 0; i < size; ++i) {
			bytes.data[i] = PatternByte(i);
		}
		EXPECT_TRUE(FileSystem::WriteFile(Path(path.generic_string()), bytes));
		return path;
	}

	bool MatchesPattern(const std::vector<u8>& bytes, u64 offset) {
		for (size_t i = 0; i < bytes.size(); ++i) {
			if (bytes[i] != PatternByte(offset + i)) {
				return false;
			}
		}
		return true;
	}

} // anonymous namespace

TEST(FileReader, ReadsBytesAtOffsetAsked) {
	const u64 size = 3 * 1024 * 1024 + 17;
	const std::filesystem::path path = WritePatternFile("shalltear_filereader_offsets.bin", size);
	{
		FileReader reader;
		ASSERT_TRUE(reader.Open(Path(path.generic_string())));
		EXPECT_TRUE(reader.IsOpen());
		EXPECT_EQ(reader.GetSize(), size);

		const u64 offsets[] = {0, 4095, 1024 * 1024 + 3, size - 1, 0};
		const u64 lengths[] = {4096, 70000, 12345, 1, size};
		for (u32 i = 0; i < 5; ++i) {
			std::vector<u8> bytes(static_cast<size_t>(lengths[i]), 0);
			ASSERT_TRUE(reader.ReadAt(offsets[i], std::span<u8>(bytes))) << "read " << i;
			EXPECT_TRUE(MatchesPattern(bytes, offsets[i])) << "read " << i;
		}
	}
	std::filesystem::remove(path);
}

TEST(FileReader, MissingFileOrRangeRefused) {
	FileReader missing;
	EXPECT_FALSE(missing.Open(Path((std::filesystem::temp_directory_path() / "shalltear_filereader_nothing_here.bin").generic_string())));
	EXPECT_FALSE(missing.IsOpen());
	u8 one = 0;
	EXPECT_FALSE(missing.ReadAt(0, std::span<u8>(&one, 1)));

	const std::filesystem::path path = WritePatternFile("shalltear_filereader_bounds.bin", 1000);
	{
		FileReader reader;
		ASSERT_TRUE(reader.Open(Path(path.generic_string())));
		std::vector<u8> bytes(11, 0);
		EXPECT_FALSE(reader.ReadAt(990, std::span<u8>(bytes)));
		EXPECT_FALSE(reader.ReadAt(~0ull - 4, std::span<u8>(bytes)));
		EXPECT_TRUE(reader.ReadAt(1000, std::span<u8>()));
		EXPECT_FALSE(reader.ReadAt(1001, std::span<u8>()));
		bytes.resize(10);
		EXPECT_TRUE(reader.ReadAt(990, std::span<u8>(bytes)));
		EXPECT_TRUE(MatchesPattern(bytes, 990));
	}
	std::filesystem::remove(path);
}

TEST(FileReader, ConcurrentReadsGetOwnBytes) {
	const u64 size = 1024 * 1024;
	const std::filesystem::path path = WritePatternFile("shalltear_filereader_threads.bin", size);
	{
		FileReader reader;
		ASSERT_TRUE(reader.Open(Path(path.generic_string())));
		std::atomic<u32> wrong{0};
		auto hammer = [&](u64 seed) {
			std::vector<u8> bytes;
			u64 state = seed;
			for (u32 i = 0; i < 2000; ++i) {
				state = state * 6364136223846793005ull + 1442695040888963407ull;
				const u64 length = 1 + (state >> 33) % 4096;
				const u64 offset = (state >> 7) % (size - length);
				bytes.assign(static_cast<size_t>(length), 0);
				if (!reader.ReadAt(offset, std::span<u8>(bytes)) || !MatchesPattern(bytes, offset)) {
					wrong.fetch_add(1);
				}
			}
		};
		std::thread first(hammer, 1);
		std::thread second(hammer, 2);
		first.join();
		second.join();
		EXPECT_EQ(wrong.load(), 0u);
	}
	std::filesystem::remove(path);
}

TEST(FileReader, OpenFileRefusesWriterAndDelete) {
	const std::filesystem::path path = WritePatternFile("shalltear_filereader_share.bin", 4096);
	const Path file(path.generic_string());
	Buffer replacement(16, MemTag::Unknown);
	std::memset(replacement.data, 0xAB, replacement.size);
	{
		FileReader reader;
		ASSERT_TRUE(reader.Open(file));

		EXPECT_FALSE(FileSystem::WriteFile(file, replacement));
		std::error_code ec;
		if (TestPlatform::OpenFileRefusesDelete()) {
			EXPECT_FALSE(std::filesystem::remove(path, ec));
		}

		FileReader second;
		EXPECT_TRUE(second.Open(file));
		EXPECT_EQ(FileSystem::ReadFile(file, MemTag::Unknown).size, 4096u);

		FileReader moved(std::move(reader));
		EXPECT_FALSE(reader.IsOpen());
		std::vector<u8> bytes(64, 0);
		EXPECT_TRUE(moved.ReadAt(100, std::span<u8>(bytes)));
		EXPECT_TRUE(MatchesPattern(bytes, 100));
		EXPECT_FALSE(FileSystem::WriteFile(file, replacement));
	}
	EXPECT_TRUE(FileSystem::WriteFile(file, replacement));
	std::filesystem::remove(path);
}

TEST(FileReader, ReadDeleteAllowsRename) {
	const std::filesystem::path path = WritePatternFile("shalltear_filereader_rename.bin", 4096);
	const std::filesystem::path aside = std::filesystem::temp_directory_path() / "shalltear_filereader_rename.old";
	std::error_code ec;
	std::filesystem::remove(aside, ec);
	Buffer replacement(16, MemTag::Unknown);
	std::memset(replacement.data, 0xAB, replacement.size);
	{
		FileReader reader;
		ASSERT_TRUE(reader.Open(Path(path.generic_string()), FileSharing::ReadDelete));
		std::filesystem::rename(path, aside, ec);
		ASSERT_FALSE(ec);
		EXPECT_TRUE(FileSystem::WriteFile(Path(path.generic_string()), replacement));
		EXPECT_FALSE(FileSystem::WriteFile(Path(aside.generic_string()), replacement));

		FileReader reopened;
		ASSERT_TRUE(reopened.Open(reader));
		EXPECT_EQ(reopened.GetSize(), 4096u);
		EXPECT_TRUE(std::filesystem::remove(aside, ec));
		FileReader afterDelete;
		ASSERT_TRUE(afterDelete.Open(reopened));

		std::vector<u8> bytes;
		for (const FileReader* each : {&reader, &reopened, &afterDelete}) {
			bytes.assign(64, 0);
			EXPECT_TRUE(each->ReadAt(100, std::span<u8>(bytes)));
			EXPECT_TRUE(MatchesPattern(bytes, 100));
		}
	}
	EXPECT_FALSE(std::filesystem::exists(aside, ec));
	EXPECT_EQ(FileSystem::GetFileSize(Path(path.generic_string())), 16u);
	std::filesystem::remove(path, ec);
}

TEST(FileReader, ReopenInheritsSharing) {
	const std::filesystem::path path = WritePatternFile("shalltear_filereader_reopen.bin", 4096);
	std::error_code ec;
	{
		FileReader reopened;
		{
			FileReader reader;
			ASSERT_TRUE(reader.Open(Path(path.generic_string())));
			ASSERT_TRUE(reopened.Open(reader));
		}
		Buffer replacement(16, MemTag::Unknown);
		EXPECT_FALSE(FileSystem::WriteFile(Path(path.generic_string()), replacement));
		if (TestPlatform::OpenFileRefusesDelete()) {
			EXPECT_FALSE(std::filesystem::remove(path, ec));
		}
		std::vector<u8> bytes(96, 0);
		EXPECT_TRUE(reopened.ReadAt(4000, std::span<u8>(bytes)));
		EXPECT_TRUE(MatchesPattern(bytes, 4000));

		FileReader closed;
		EXPECT_FALSE(reopened.Open(closed));
		EXPECT_FALSE(reopened.IsOpen());
	}
	EXPECT_TRUE(std::filesystem::remove(path, ec));
}
