#include "TestsPCH.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"

#include "Shalltear/Editor/EditorConsole.h"

#include <chrono>
#include <thread>

using namespace Shalltear;

namespace {

	class ConsoleSinkTest : public ::testing::Test {
	protected:
		void SetUp() override {
			JobSystem::Initialize();
			LogManager::Initialize({false, false});
			LogManager::SetGlobalLevel(LogLevel::Trace);
			EditorConsole::Initialize();
			SLT_LOG_INFO("Sync", "barrier");
			WaitForBuffered(1);
			EditorConsole::Clear();
		}
		void TearDown() override {
			EditorConsole::Terminate();
			LogManager::Terminate();
			JobSystem::Terminate();
		}

		static bool WaitForBuffered(u32 count, f64 timeoutSeconds = 5.0) {
			auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(timeoutSeconds);
			while (std::chrono::steady_clock::now() < deadline) {
				if (EditorConsole::GetBufferedCount() >= count) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return EditorConsole::GetBufferedCount() >= count;
		}
	};

} // anonymous namespace

TEST_F(ConsoleSinkTest, CapturesLevelCategoryMessageAndSource) {
	SLT_LOG_WARN("ConsoleSinkTest", "hello {}", 42);
	ASSERT_TRUE(WaitForBuffered(1));

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	u64 newest = EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), 1u);
	const ConsoleEntry& entry = entries[0];
	EXPECT_EQ(entry.level, LogLevel::Warn);
	EXPECT_EQ(entry.category, "ConsoleSinkTest");
	EXPECT_EQ(entry.message, "hello 42");
	EXPECT_NE(entry.file.find("ConsoleSinkTest.cpp"), str::npos);
	EXPECT_GT(entry.line, 0u);
	EXPECT_EQ(entry.sequence, newest);
}

TEST_F(ConsoleSinkTest, ReadSinceIsIncrementalAndNonConsuming) {
	SLT_LOG_INFO("Seq", "one");
	SLT_LOG_INFO("Seq", "two");
	SLT_LOG_INFO("Seq", "three");
	ASSERT_TRUE(WaitForBuffered(3));

	Vector<ConsoleEntry> first{GetMemoryResource(MemTag::Editor)};
	u64 cursor = EditorConsole::ReadSince(0, first);
	ASSERT_EQ(first.size(), 3u);
	EXPECT_LT(first[0].sequence, first[1].sequence);
	EXPECT_LT(first[1].sequence, first[2].sequence);

	Vector<ConsoleEntry> second{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, second);
	ASSERT_EQ(second.size(), 3u);
	EXPECT_EQ(second[0].message, first[0].message);

	Vector<ConsoleEntry> none{GetMemoryResource(MemTag::Editor)};
	EXPECT_EQ(EditorConsole::ReadSince(cursor, none), cursor);
	EXPECT_TRUE(none.empty());

	SLT_LOG_INFO("Seq", "four");
	ASSERT_TRUE(WaitForBuffered(4));
	Vector<ConsoleEntry> fresh{GetMemoryResource(MemTag::Editor)};
	u64 advanced = EditorConsole::ReadSince(cursor, fresh);
	ASSERT_EQ(fresh.size(), 1u);
	EXPECT_EQ(fresh[0].message, "four");
	EXPECT_GT(advanced, cursor);
}

TEST_F(ConsoleSinkTest, ConcurrentProducersSerializeWithContiguousSequences) {
	constexpr int THREADS = 4;
	constexpr int PER_THREAD = 50;
	std::vector<std::thread> producers;
	for (int t = 0; t < THREADS; ++t) {
		producers.emplace_back([t]() {
			for (int i = 0; i < PER_THREAD; ++i) {
				SLT_LOG_INFO("Producer", "t{} m{}", t, i);
			}
		});
	}
	for (std::thread& thread : producers) {
		thread.join();
	}
	ASSERT_TRUE(WaitForBuffered(THREADS * PER_THREAD));

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), static_cast<size_t>(THREADS * PER_THREAD));
	for (size_t i = 1; i < entries.size(); ++i) {
		EXPECT_EQ(entries[i].sequence, entries[i - 1].sequence + 1);
	}
	EXPECT_EQ(EditorConsole::GetDroppedCount(), 0u);
}

TEST_F(ConsoleSinkTest, RingCapEvictsOldestAndCountsDrops) {
	constexpr u32 TOTAL = 4200;
	for (u32 i = 0; i < TOTAL; ++i) {
		SLT_LOG_TRACE("Flood", "{}", i);
	}
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline &&
		   EditorConsole::GetBufferedCount() + EditorConsole::GetDroppedCount() < TOTAL) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	EXPECT_EQ(EditorConsole::GetBufferedCount(), 4096u);
	EXPECT_EQ(EditorConsole::GetDroppedCount(), static_cast<u64>(TOTAL - 4096));

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), 4096u);
	EXPECT_EQ(entries.back().message, std::to_string(TOTAL - 1));
	EXPECT_EQ(entries.back().sequence, entries.front().sequence + 4095);
}

TEST_F(ConsoleSinkTest, ClearEmptiesTheBufferAndCaptureContinues) {
	SLT_LOG_INFO("ClearTest", "before");
	ASSERT_TRUE(WaitForBuffered(1));
	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	u64 cursor = EditorConsole::ReadSince(0, entries);

	EditorConsole::Clear();
	EXPECT_EQ(EditorConsole::GetBufferedCount(), 0u);
	entries.clear();
	EXPECT_EQ(EditorConsole::ReadSince(0, entries), 0u);
	EXPECT_TRUE(entries.empty());

	SLT_LOG_INFO("ClearTest", "after");
	ASSERT_TRUE(WaitForBuffered(1));
	entries.clear();
	u64 advanced = EditorConsole::ReadSince(cursor, entries);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0].message, "after");
	EXPECT_GT(advanced, cursor);
}

TEST_F(ConsoleSinkTest, PushKeepsOriginAndOrder) {
	SLT_LOG_INFO("PushTest", "logged first");
	ASSERT_TRUE(WaitForBuffered(1));

	ConsoleOrigin origin;
	origin.file = "C:/Games/MyGame/Source/CharacterController.cpp";
	origin.line = 41;
	origin.column = 9;
	EditorConsole::Push(LogLevel::Error, "Build", "expected ';' after expression", origin);
	ASSERT_TRUE(WaitForBuffered(2));

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), 2u);

	const ConsoleEntry& logged = entries[0];
	const ConsoleEntry& pushed = entries[1];
	EXPECT_EQ(logged.message, "logged first");
	EXPECT_EQ(pushed.message, "expected ';' after expression");
	EXPECT_LT(logged.sequence, pushed.sequence);

	EXPECT_EQ(pushed.level, LogLevel::Error);
	EXPECT_EQ(pushed.category, "Build");
	EXPECT_EQ(pushed.file, "C:/Games/MyGame/Source/CharacterController.cpp");
	EXPECT_EQ(pushed.line, 41u);
	EXPECT_EQ(pushed.column, 9u);
	EXPECT_TRUE(pushed.clickable);

	EXPECT_FALSE(logged.clickable);
	EXPECT_EQ(logged.column, 0u);
}

TEST_F(ConsoleSinkTest, PushWithoutAFileIsNotClickable) {
	EditorConsole::Push(LogLevel::Error, "Build", "undefined symbol: Foo::Bar", ConsoleOrigin{});
	ASSERT_TRUE(WaitForBuffered(1));

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_FALSE(entries[0].clickable);
	EXPECT_TRUE(entries[0].file.empty());
}

TEST_F(ConsoleSinkTest, SummaryReportsNewestEntryWhateverLevel) {
	ConsoleSummary empty = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(empty.newest.sequence, 0u);
	EXPECT_EQ(empty.newestUnseenError.sequence, 0u);

	SLT_LOG_INFO("Summary", "first");
	SLT_LOG_INFO("Summary", "second");
	ASSERT_TRUE(WaitForBuffered(2));

	ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(summary.newest.message, "second");
	EXPECT_EQ(summary.newest.level, LogLevel::Info);
	EXPECT_EQ(summary.unseenErrors, 0u);
	EXPECT_EQ(summary.unseenWarnings, 0u);
}

TEST_F(ConsoleSinkTest, ErrorsAndWarningsCountOutstanding) {
	SLT_LOG_INFO("Summary", "chatter");
	SLT_LOG_WARN("Summary", "a warning");
	SLT_LOG_ERROR("Summary", "a real problem");
	SLT_LOG_WARN("Summary", "another warning");
	ASSERT_TRUE(WaitForBuffered(4));

	ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(summary.unseenErrors, 1u);
	EXPECT_EQ(summary.unseenWarnings, 2u);
	EXPECT_EQ(summary.newest.message, "another warning");
	EXPECT_EQ(summary.newestUnseenError.message, "a real problem");
}

TEST_F(ConsoleSinkTest, HeldErrorIsNEWESTUnseenOne) {
	SLT_LOG_ERROR("Summary", "older");
	SLT_LOG_ERROR("Summary", "newer");
	ASSERT_TRUE(WaitForBuffered(2));

	ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(summary.unseenErrors, 2u);
	EXPECT_EQ(summary.newestUnseenError.message, "newer");
}

TEST_F(ConsoleSinkTest, FatalCountsAsError) {
	SLT_LOG_ERROR("Summary", "an error");
	ASSERT_TRUE(WaitForBuffered(1));
	EditorConsole::Push(LogLevel::Fatal, "Summary", "the worst thing", ConsoleOrigin{});
	ASSERT_TRUE(WaitForBuffered(2));

	ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(summary.unseenErrors, 2u);
	EXPECT_EQ(summary.newestUnseenError.level, LogLevel::Fatal);
}

TEST_F(ConsoleSinkTest, MarkSeenClearsUpToMark) {
	SLT_LOG_ERROR("Summary", "seen by the panel");
	ASSERT_TRUE(WaitForBuffered(1));
	ConsoleSummary before = EditorConsole::GetSummary(LogLevel::Info);
	ASSERT_EQ(before.unseenErrors, 1u);

	EditorConsole::MarkSeenThrough(before.newest.sequence);
	ConsoleSummary seen = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(seen.unseenErrors, 0u);
	EXPECT_EQ(seen.newestUnseenError.sequence, 0u);
	EXPECT_EQ(seen.newest.message, "seen by the panel");

	SLT_LOG_ERROR("Summary", "arrived while hidden");
	ASSERT_TRUE(WaitForBuffered(2));
	ConsoleSummary after = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(after.unseenErrors, 1u);
	EXPECT_EQ(after.newestUnseenError.message, "arrived while hidden");
}

TEST_F(ConsoleSinkTest, TheSeenCursorNeverGoesBackwards) {
	SLT_LOG_ERROR("Summary", "one");
	SLT_LOG_ERROR("Summary", "two");
	ASSERT_TRUE(WaitForBuffered(2));
	u64 newest = EditorConsole::GetSummary(LogLevel::Info).newest.sequence;

	EditorConsole::MarkSeenThrough(newest);
	EditorConsole::MarkSeenThrough(newest - 1);
	EXPECT_EQ(EditorConsole::GetSummary(LogLevel::Info).unseenErrors, 0u);
}

TEST_F(ConsoleSinkTest, ClearLeavesNothingOutstanding) {
	SLT_LOG_ERROR("Summary", "before the clear");
	ASSERT_TRUE(WaitForBuffered(1));
	ASSERT_EQ(EditorConsole::GetSummary(LogLevel::Info).unseenErrors, 1u);

	EditorConsole::Clear();
	ConsoleSummary cleared = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(cleared.newest.sequence, 0u);
	EXPECT_EQ(cleared.unseenErrors, 0u);
	EXPECT_EQ(cleared.unseenWarnings, 0u);

	SLT_LOG_ERROR("Summary", "after the clear");
	ASSERT_TRUE(WaitForBuffered(1));
	EXPECT_EQ(EditorConsole::GetSummary(LogLevel::Info).unseenErrors, 1u);
}

TEST_F(ConsoleSinkTest, MessageFloorSkipsLowLevels) {
	SLT_LOG_INFO("Floor", "something worth saying");
	ASSERT_TRUE(WaitForBuffered(1));
	SLT_LOG_WARN("Floor", "and a warning");
	ASSERT_TRUE(WaitForBuffered(2));
	SLT_LOG_DEBUG("Floor", "a handle value");
	SLT_LOG_TRACE("Floor", "served from artifact");
	ASSERT_TRUE(WaitForBuffered(4));

	ConsoleSummary bar = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(bar.newest.message, "and a warning");
	EXPECT_EQ(bar.unseenWarnings, 1u);

	ConsoleSummary all = EditorConsole::GetSummary(LogLevel::Trace);
	EXPECT_EQ(all.newest.message, "served from artifact");
	EXPECT_EQ(all.unseenWarnings, 1u);
}

TEST_F(ConsoleSinkTest, SummaryDoesNotWalkPastEvictedTail) {
	constexpr u32 TOTAL = 4200;
	SLT_LOG_ERROR("Evicted", "the first error");
	for (u32 i = 0; i < TOTAL; ++i) {
		SLT_LOG_INFO("Flood", "{}", i);
	}
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline &&
		   EditorConsole::GetBufferedCount() + EditorConsole::GetDroppedCount() < TOTAL + 1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_GT(EditorConsole::GetDroppedCount(), 0u);

	ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
	EXPECT_EQ(summary.unseenErrors, 0u);
	EXPECT_EQ(summary.newestUnseenError.sequence, 0u);
	EXPECT_EQ(summary.newest.message, std::to_string(TOTAL - 1));
}

TEST_F(ConsoleSinkTest, LogRowFromProjectFileIsClickable) {
	FileSystem::Mount("project", Path("C:/Games/MyGame"));
	LogManager::Dispatch(LogLevel::Info, "Game", "C:\\Games\\MyGame\\Assets\\Scripts\\Spinner.cs", 12, "", "{}", "inside");
	LogManager::Dispatch(LogLevel::Info, "Game", "C:/Games/MyGameTwo/Assets/Spinner.cs", 3, "", "{}", "sibling");
	LogManager::Dispatch(LogLevel::Info, "Game", "Source/Game/Spinner.cpp", 7, "", "{}", "relative");
	LogManager::Dispatch(LogLevel::Info, "Game", "", 0, "", "{}", "nowhere");
	FileSystem::Mount("sdk", Path("C:/Sdk"));
	LogManager::Dispatch(LogLevel::Info, "Game", "C:/Sdk/Templates/Spinner.cs", 2, "", "{}", "another mount");
	const bool buffered = WaitForBuffered(5);
	FileSystem::Unmount("sdk");
	FileSystem::Unmount("project");
	ASSERT_TRUE(buffered);

	Vector<ConsoleEntry> entries{GetMemoryResource(MemTag::Editor)};
	EditorConsole::ReadSince(0, entries);
	ASSERT_EQ(entries.size(), 5u);
	EXPECT_TRUE(entries[0].clickable);
	EXPECT_EQ(entries[0].file, "C:\\Games\\MyGame\\Assets\\Scripts\\Spinner.cs");
	EXPECT_EQ(entries[0].line, 12u);
	EXPECT_EQ(entries[0].column, 0u);
	EXPECT_FALSE(entries[1].clickable);
	EXPECT_FALSE(entries[2].clickable);
	EXPECT_FALSE(entries[3].clickable);
	EXPECT_FALSE(entries[4].clickable);
}
