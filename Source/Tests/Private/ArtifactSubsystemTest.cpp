#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <filesystem>

using namespace Shalltear;

TEST(ArtifactSubsystemTest, TheEditorExecutableIsAWindowedApplication) {
	std::filesystem::path here(ScriptProjectOps::GetEditorExecutablePath());
	if (here.empty() || !here.has_parent_path()) {
		GTEST_SKIP();
	}
	std::filesystem::path editor = here.parent_path() / "Shalltear" SLT_TEST_EXE_SUFFIX;
	if (!std::filesystem::exists(editor)) {
		GTEST_SKIP() << "no editor exe (" << editor.string() << ")";
	}

	str why;
	EXPECT_TRUE(TestPlatform::IsArtifactKind(here.generic_string(), false, why)) << why;
	EXPECT_TRUE(TestPlatform::IsArtifactKind(editor.generic_string(), true, why)) << why;
}

TEST(ArtifactCodePageTest, EngineExecutablesReadTextAsUTF8) {
	std::filesystem::path here(ScriptProjectOps::GetEditorExecutablePath());
	if (here.empty() || !here.has_parent_path()) {
		GTEST_SKIP();
	}
	std::filesystem::path editor = here.parent_path() / "Shalltear" SLT_TEST_EXE_SUFFIX;
	if (!std::filesystem::exists(editor)) {
		GTEST_SKIP() << "no editor exe (" << editor.string() << ")";
	}

	str why;
	EXPECT_TRUE(TestPlatform::ReadsTextAsUTF8(here.generic_string(), why)) << why;
	EXPECT_TRUE(TestPlatform::ReadsTextAsUTF8(editor.generic_string(), why)) << why;
}
