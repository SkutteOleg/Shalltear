#include "TestsPCH.h"

#include "CSharpConformance.h"
#include "CSharpRuntimeFixture.h"
#include "CookHarness.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/StringHash.h"

#include <algorithm>
#include <filesystem>
#include <memory>

using namespace Shalltear;

TEST(CSharpBindingsTest, GeneratedFilesMatchTheirInputs) {
	const std::filesystem::path schemas = CookHarness::FindRepoDir("Source/CSharp/Bindings");
	const std::filesystem::path script = CookHarness::FindRepoDir("Scripts/generate_csharp_bindings.py");
	ASSERT_FALSE(schemas.empty());
	ASSERT_FALSE(script.empty());

	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(schemas)) {
		if (entry.path().extension() == ".json") {
			files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.filename() < b.filename(); });
	ASSERT_GE(files.size(), 2u);

	str inputs;
	for (const std::filesystem::path& file : files) {
		inputs += file.filename().generic_string();
		inputs += '\0';
		inputs += FileSystem::ReadTextFile(Path(file.generic_string()));
		inputs += '\0';
	}
	inputs += FileSystem::ReadTextFile(Path(script.generic_string()));
	EXPECT_EQ(StringHash(std::string_view(inputs)).GetValue(), SLT_BINDINGS_INPUT_DIGEST);
}

#ifdef SHALLTEAR_SHARED

using namespace Shalltear::CSharpTests;

TEST_F(CSharpRuntimeFixture, EditorRuntimeChecksThreads) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("FeatureProbe.ReportsThreadChecks", message), CASE_FAILED);
	EXPECT_EQ(message, "thread checks on");
}

TEST_F(CSharpRuntimeFixture, LogReachesEngineLog) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const auto rows = std::make_shared<CSharpConformance::RowSink>();
	LogManager::AddSink(rows);

	str message;
	EXPECT_EQ(Run("LogProbe.WritesEachLevel", message), SLT_OK) << message;
	_log.Delivered();
	LogManager::RemoveSink(rows);

#ifdef NDEBUG
	const u32 debugRows = 0;
#else
	const u32 debugRows = 1;
#endif
	EXPECT_EQ(rows->Count(LogLevel::Trace, "Conformance", "trace from C#"), debugRows);
	EXPECT_EQ(rows->Count(LogLevel::Debug, "Conformance", "debug from C#"), debugRows);
	EXPECT_EQ(rows->Count(LogLevel::Info, "Conformance", "info from C#"), 1u);
	EXPECT_EQ(rows->Count(LogLevel::Warn, "Conformance", "warn from C#"), 1u);
	EXPECT_EQ(rows->Count(LogLevel::Error, "Conformance", "error from C#"), 1u);
	EXPECT_EQ(rows->CountFrom("Conformance"), 3u + 2u * debugRows);
}

TEST_F(CSharpRuntimeFixture, LogFollowsGlobalLevel) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const auto rows = std::make_shared<CSharpConformance::RowSink>();
	LogManager::AddSink(rows);
	LogManager::SetGlobalLevel(LogLevel::Warn);

	str message;
	EXPECT_EQ(Run("LogProbe.WritesEachLevel", message), SLT_OK) << message;
	LogManager::SetGlobalLevel(LogLevel::Trace);
	_log.Delivered();
	LogManager::RemoveSink(rows);

	EXPECT_EQ(rows->Count(LogLevel::Warn, "Conformance", "warn from C#"), 1u);
	EXPECT_EQ(rows->Count(LogLevel::Error, "Conformance", "error from C#"), 1u);
	EXPECT_EQ(rows->CountFrom("Conformance"), 2u);
}

TEST_F(CSharpRuntimeFixture, LogTakesOddText) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const auto rows = std::make_shared<CSharpConformance::RowSink>();
	LogManager::AddSink(rows);

	str message;
	EXPECT_EQ(Run("LogProbe.WritesOddText", message), SLT_OK) << message;
	_log.Delivered();
	LogManager::RemoveSink(rows);

	EXPECT_EQ(rows->Count(LogLevel::Info, "", ""), 2u);
	EXPECT_EQ(rows->Count(LogLevel::Info, "Conformance", str(5000, 'x') + " caf\xC3\xA9 end"), 1u);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, LogWritesFromStartedThread) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const auto rows = std::make_shared<CSharpConformance::RowSink>();
	LogManager::AddSink(rows);

	str message;
	EXPECT_EQ(Run("LogProbe.WritesFromStartedThread", message), SLT_OK) << message;
	_log.Delivered();
	LogManager::RemoveSink(rows);

	EXPECT_EQ(rows->Count(LogLevel::Info, "Conformance", "info from a started thread"), 1u);
}

#endif
