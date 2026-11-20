#include "TestsPCH.h"

#ifdef SLT_TEST_CSHARP_AOT

#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace Shalltear;

namespace {

	constexpr u64 SHIM_DEFAULT_COOKIE = 0x00002B992DDFA232ull;
	constexpr i32 FAST_FAIL_EXIT = static_cast<i32>(0xC0000409u);

	std::filesystem::path AOTFile(const char* extension) {
		return std::filesystem::path(FileSystem::GetExecutablePath().ToString()).parent_path() / "CSharp" / "aot" /
			   (str("Shalltear.ConformanceAOT") + extension);
	}

	u64 FindMapValue(const str& map, std::string_view name) {
		size_t at = 0;
		while ((at = map.find(name, at)) != str::npos) {
			const size_t end = at + name.size();
			const bool whole = at > 0 && map[at - 1] == ' ' && end < map.size() && map[end] == ' ';
			if (whole) {
				return std::strtoull(map.c_str() + end, nullptr, 16);
			}
			at = end;
		}
		return 0;
	}

	u64 FindSymbolOffset(const str& map, std::string_view name) {
		const u64 preferred = FindMapValue(map, "address is");
		const u64 address = FindMapValue(map, name);
		return preferred != 0 && address > preferred ? address - preferred : 0;
	}

	str ProbeCommand(const char* mode) {
		return "\"" + FileSystem::GetExecutablePath().ToString() + "\" " + TestHelpers::CSHARP_SHIM_FLAG + " \"" + AOTFile(".dll").generic_string() +
			   "\" \"" + AOTFile(".map").generic_string() + "\" " + mode;
	}

	bool ReadCookie(u64& outCookie) {
		int exitCode = -1;
		const str output = TestPlatform::RunCommand(ProbeCommand("cookie"), exitCode);
		unsigned long long cookie = 0;
		const size_t at = output.find("COOKIE ");
		if (exitCode != 0 || at == str::npos || std::sscanf(output.c_str() + at, "COOKIE %llx", &cookie) != 1) {
			ADD_FAILURE() << "exit " << exitCode << ": " << output;
			return false;
		}
		outCookie = cookie;
		return true;
	}

} // anonymous namespace

int Shalltear::TestHelpers::RunCSharpShimProbe(const char* library, const char* map, const char* mode) {
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	const str symbols = FileSystem::ReadTextFile(Path(str(map)));
	const u64 cookieOffset = FindSymbolOffset(symbols, "__security_cookie");
	const u64 checkOffset = FindSymbolOffset(symbols, "__security_check_cookie");
	const u64 rangeOffset = FindSymbolOffset(symbols, "__report_rangecheckfailure");
	if (cookieOffset == 0 || checkOffset == 0 || rangeOffset == 0) {
		std::printf("NO SYMBOLS %s\n", map);
		return 3;
	}
	const HMODULE module = ::LoadLibraryA(library);
	if (!module) {
		std::printf("NO LIBRARY %s\n", library);
		return 4;
	}
	u8* base = reinterpret_cast<u8*>(module);
	const u64 cookie = *reinterpret_cast<const u64*>(base + cookieOffset);
	const str chosen = mode;
	if (chosen == "cookie") {
		std::printf("COOKIE %llx\n", static_cast<unsigned long long>(cookie));
		std::fflush(stdout);
		std::_Exit(0);
	}
	if (chosen == "match") {
		reinterpret_cast<void (*)(u64)>(base + checkOffset)(cookie);
		std::_Exit(0);
	}
	if (chosen == "mismatch") {
		reinterpret_cast<void (*)(u64)>(base + checkOffset)(cookie ^ 1);
	} else if (chosen == "range") {
		reinterpret_cast<void (*)()>(base + rangeOffset)();
	}
	std::_Exit(5);
}

TEST(CSharpWindowsLinkTest, StackCookieSeededPerProcess) {
	u64 first = 0;
	u64 second = 0;
	ASSERT_TRUE(ReadCookie(first));
	ASSERT_TRUE(ReadCookie(second));
	EXPECT_NE(first, SHIM_DEFAULT_COOKIE);
	EXPECT_NE(first, 0u);
	EXPECT_NE(first, second);
	EXPECT_EQ(first >> 48, 0u);
	EXPECT_EQ(second >> 48, 0u);
}

TEST(CSharpWindowsLinkTest, CookieMismatchFailsFast) {
	const TestPlatform::ProcessResult match = TestPlatform::RunProcess(ProbeCommand("match"), 60000);
	ASSERT_TRUE(match.exited);
	EXPECT_EQ(match.exitCode, 0);
	const TestPlatform::ProcessResult mismatch = TestPlatform::RunProcess(ProbeCommand("mismatch"), 60000);
	ASSERT_TRUE(mismatch.exited);
	EXPECT_EQ(mismatch.exitCode, FAST_FAIL_EXIT);
	const TestPlatform::ProcessResult range = TestPlatform::RunProcess(ProbeCommand("range"), 60000);
	ASSERT_TRUE(range.exited);
	EXPECT_EQ(range.exitCode, FAST_FAIL_EXIT);
}

#endif
