#include "TestsPCH.h"

#ifdef SLT_TEST_CSHARP_AOT

#include "LinuxLibraryReader.h"

#include "Shalltear/Core/FileSystem.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace Shalltear;

TEST(CSharpLinuxLinkTest, LinksWithThePack) {
	const std::filesystem::path library =
		std::filesystem::path(FileSystem::GetExecutablePath().ToString()).parent_path() / "CSharp" / "aot" / "Shalltear.ConformanceAOT.so";
	TestHelpers::LibraryNeeds needs;
	ASSERT_TRUE(TestHelpers::ReadLibraryNeeds(library, needs)) << library.generic_string();
	ASSERT_FALSE(needs.libraries.empty());
	for (const str& name : needs.libraries) {
		const bool system = name == "libdl.so.2" || name == "libm.so.6" || name == "libpthread.so.0" || name == "libc.so.6" || name == "librt.so.1" ||
							name == "ld-linux-x86-64.so.2";
		EXPECT_TRUE(system) << name;
	}

	std::ifstream stream(std::filesystem::path(SLT_TEST_LINUX_PACK) / "sysroot" / "usr" / "include" / "features.h");
	const str features((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	u32 packMinor = 0;
	for (size_t at = features.find("__GLIBC_MINOR__"); at != str::npos && packMinor == 0; at = features.find("__GLIBC_MINOR__", at + 1)) {
		packMinor = static_cast<u32>(std::strtoul(features.c_str() + at + 15, nullptr, 10));
	}
	ASSERT_GT(packMinor, 0u);
	EXPECT_EQ(needs.glibcMajor, 2u);
	EXPECT_GT(needs.glibcMinor, 0u);
	EXPECT_LE(needs.glibcMinor, packMinor);
}

#endif
