#include "TestsPCH.h"

#include "DeviceLossHarness.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"

#include "Shalltear/Editor/ChildProcess.h"

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <thread>

using namespace Shalltear;

namespace {

	char _firstOwner = 0;
	char _secondOwner = 0;

	class ChildProcessTest : public ::testing::Test {
	protected:
		void TearDown() override {
			ChildEnvironment::UnregisterOwnedBy(&_firstOwner);
			ChildEnvironment::UnregisterOwnedBy(&_secondOwner);
		}

		str Reported(std::initializer_list<const char*> names, std::span<const ChildVariable> variables = {}) {
			str command = QuoteCommandArgument(FileSystem::GetExecutablePath().ToString()) + " " + TestHelpers::PRINT_ENVIRONMENT_FLAG;
			for (const char* name : names) {
				command += " ";
				command += name;
			}
			ChildProcess child;
			if (!child.Start(command, {}, variables)) {
				ADD_FAILURE() << "child didn't start";
				return {};
			}
			i32 exitCode = -1;
			for (u32 attempt = 0; attempt < 4000 && !child.TryFinish(exitCode); ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			EXPECT_EQ(exitCode, 0);
			Vector<str> lines{GetMemoryResource(MemTag::Editor)};
			child.DrainLines(lines);
			str joined;
			for (const str& line : lines) {
				joined += line;
				joined += '\n';
			}
			return joined;
		}
	};

} // anonymous namespace

TEST_F(ChildProcessTest, ContributedVariableReachesAChild) {
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE"}), "SLT_TEST_CHILD_VALUE=\n");
	ChildEnvironment::Set("SLT_TEST_CHILD_VALUE", "alpha", &_firstOwner);
	ChildEnvironment::Set("SLT_TEST_CHILD_OTHER", "with space", &_secondOwner);
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE", "SLT_TEST_CHILD_OTHER"}), "SLT_TEST_CHILD_VALUE=alpha\nSLT_TEST_CHILD_OTHER=with space\n");

	ChildEnvironment::Set("SLT_TEST_CHILD_VALUE", "beta", &_firstOwner);
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE"}), "SLT_TEST_CHILD_VALUE=beta\n");
	EXPECT_EQ(ChildEnvironment::UnregisterOwnedBy(&_firstOwner), 1u);
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE", "SLT_TEST_CHILD_OTHER"}), "SLT_TEST_CHILD_VALUE=\nSLT_TEST_CHILD_OTHER=with space\n");
	EXPECT_EQ(TestPlatform::GetEnvironment("SLT_TEST_CHILD_OTHER"), "");
}

TEST_F(ChildProcessTest, ChildTakesVariablesGivenAtStart) {
	DeviceLossTest::ScopedEnvironment inherited("SLT_TEST_CHILD_INHERITED", "parent");
	ChildEnvironment::Set("SLT_TEST_CHILD_VALUE", "contributed", &_firstOwner);
	const ChildVariable given[] = {{"SLT_TEST_CHILD_VALUE", "given"}, {"SLT_TEST_CHILD_INHERITED", "given too"}, {"SLT_TEST_CHILD_OTHER", "new"}};
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE", "SLT_TEST_CHILD_INHERITED", "SLT_TEST_CHILD_OTHER"}, given),
			  "SLT_TEST_CHILD_VALUE=given\nSLT_TEST_CHILD_INHERITED=given too\nSLT_TEST_CHILD_OTHER=new\n");
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_VALUE", "SLT_TEST_CHILD_INHERITED", "SLT_TEST_CHILD_OTHER"}),
			  "SLT_TEST_CHILD_VALUE=contributed\nSLT_TEST_CHILD_INHERITED=parent\nSLT_TEST_CHILD_OTHER=\n");
}

TEST_F(ChildProcessTest, ContributedValueReplacesAnInheritedOne) {
	DeviceLossTest::ScopedEnvironment inherited("SLT_TEST_CHILD_INHERITED", "parent");
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_INHERITED"}), "SLT_TEST_CHILD_INHERITED=parent\n");
	ChildEnvironment::Set("SLT_TEST_CHILD_INHERITED", "package", &_firstOwner);
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_INHERITED"}), "SLT_TEST_CHILD_INHERITED=package\n");
	EXPECT_EQ(ChildEnvironment::UnregisterOwnedBy(&_firstOwner), 1u);
	EXPECT_EQ(Reported({"SLT_TEST_CHILD_INHERITED"}), "SLT_TEST_CHILD_INHERITED=parent\n");
}

TEST_F(ChildProcessTest, DetachedChildGetsContributedVariables) {
	const std::filesystem::path receipt =
		std::filesystem::temp_directory_path() / ("shalltear_detached_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	str unset;
	const str command = TestPlatform::GetEnvironmentProbeCommand("SLT_TEST_CHILD_DETACHED", receipt.generic_string(), unset);
	ChildEnvironment::Set("SLT_TEST_CHILD_DETACHED", "carried", &_firstOwner);
	ASSERT_TRUE(StartDetachedProcess(command, {}));

	str seen;
	for (u32 attempt = 0; attempt < 400 && seen.find(']') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		seen = FileSystem::ReadTextFile(Path(receipt.generic_string()));
	}
	std::error_code ec;
	std::filesystem::remove(receipt, ec);
	EXPECT_NE(seen.find("[carried]"), str::npos) << seen;
}

TEST_F(ChildProcessTest, DetachedChildTakesVariablesGivenAtStart) {
	const std::filesystem::path receipt =
		std::filesystem::temp_directory_path() / ("shalltear_detached_given_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	str unset;
	const str command = TestPlatform::GetEnvironmentProbeCommand("SLT_TEST_CHILD_DETACHED", receipt.generic_string(), unset);
	ChildEnvironment::Set("SLT_TEST_CHILD_DETACHED", "contributed", &_firstOwner);
	const ChildVariable given[] = {{"SLT_TEST_CHILD_DETACHED", "given"}};
	ASSERT_TRUE(StartDetachedProcess(command, {}, given));

	str seen;
	for (u32 attempt = 0; attempt < 400 && seen.find(']') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		seen = FileSystem::ReadTextFile(Path(receipt.generic_string()));
	}
	std::error_code ec;
	std::filesystem::remove(receipt, ec);
	EXPECT_NE(seen.find("[given]"), str::npos) << seen;
}

TEST_F(ChildProcessTest, SearchPathEntriesLeadPATH) {
	const str inherited = TestPlatform::GetEnvironment("PATH");
	const str separator(1, TestPlatform::GetSearchPathSeparator());
	const str first = TestPlatform::GetAbsoluteRoot() + "slt-first";
	const str second = TestPlatform::GetAbsoluteRoot() + "slt-second";
	EXPECT_EQ(Reported({"PATH"}), "PATH=" + inherited + "\n");

	ChildEnvironment::PrependSearchPath(first, &_firstOwner);
	ChildEnvironment::PrependSearchPath(second, &_secondOwner);
	ChildEnvironment::PrependSearchPath(first, &_firstOwner);
	EXPECT_EQ(Reported({"PATH"}), "PATH=" + first + separator + second + separator + inherited + "\n");

	EXPECT_EQ(ChildEnvironment::UnregisterOwnedBy(&_firstOwner), 1u);
	EXPECT_EQ(Reported({"PATH"}), "PATH=" + second + separator + inherited + "\n");
	EXPECT_EQ(ChildEnvironment::UnregisterOwnedBy(&_secondOwner), 1u);
	EXPECT_EQ(Reported({"PATH"}), "PATH=" + inherited + "\n");
}
