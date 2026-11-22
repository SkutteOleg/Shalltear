#include "TestsPCH.h"

#if defined(_WIN32) && defined(__x86_64__)

#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"

#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <chrono>
#include <filesystem>
#include <string>

using namespace Shalltear;

namespace {

	namespace fs = std::filesystem;

	constexpr i32 STATUS_STACK_BUFFER_OVERRUN_CODE = static_cast<i32>(0xC0000409u);

	constexpr const char* MSVC_ABI_PROBE_SOURCE = R"(
using uintptr_t = unsigned long long;
static_assert(sizeof(uintptr_t) == sizeof(void*));

extern "C" uintptr_t __security_cookie;
extern "C" uintptr_t __security_cookie_complement;
extern "C" int __isa_available;
extern "C" void __cdecl __dyn_tls_on_demand_init();

namespace {
	uintptr_t _cookieAtXcu = 0;
	uintptr_t _complementAtXcu = 0;

	struct CaptureCookieInXcu {
		CaptureCookieInXcu() {
			_cookieAtXcu = __security_cookie;
			_complementAtXcu = __security_cookie_complement;
		}
	};
	static CaptureCookieInXcu _captureCookie;

	__declspec(thread) int _xduInitCount = 0;
	void __cdecl OnDynamicThreadInit() {
		++_xduInitCount;
	}

	struct alignas(64) OverAlignedBlock {
		unsigned char payload[128];
	};
}

#pragma section(".CRT$XDU", long, read)
extern "C" __declspec(allocate(".CRT$XDU")) void (__cdecl* const pOnDynamicThreadInit)() = &OnDynamicThreadInit;
#pragma comment(linker, "/include:pOnDynamicThreadInit")

extern "C" int ProbeGsFrameUnwind(int corruptCookie);
extern "C" int ProbeAlignedGsFrameUnwind(int corruptCookie);
extern "C" int ProbeNegativeAlignedGsFrameUnwind(int corruptCookie);
extern "C" int ProbeSetFrameGsFrameUnwind(int corruptCookie);

extern "C" int ProbeVerifyEarlyCookie() {
	if (_cookieAtXcu == 0 || _cookieAtXcu == 0x00002B992DDFA232ULL) {
		return 10;
	}
	if ((_cookieAtXcu >> 48) != 0) {
		return 11;
	}
	if (_complementAtXcu != ~_cookieAtXcu) {
		return 12;
	}
	return 0;
}

extern "C" int ProbeReadIsaAvailable() {
	return __isa_available;
}

extern "C" int ProbeThreadDynamicTLSCount() {
	__dyn_tls_on_demand_init();
	__dyn_tls_on_demand_init();
	return _xduInitCount;
}

extern "C" int ProbeAlignedOperatorNew() {
	auto* single = new OverAlignedBlock();
	auto* array = new OverAlignedBlock[3];
	const bool singleAligned = (reinterpret_cast<uintptr_t>(single) % 64u) == 0u;
	const bool arrayAligned = (reinterpret_cast<uintptr_t>(array) % 64u) == 0u;
	delete single;
	delete[] array;
	return (singleAligned && arrayAligned) ? 0 : 20;
}

extern "C" __declspec(noinline) __declspec(safebuffers) int ProbeRunSehProtected(int corruptCookie) {
	int caught = 0;
	__try {
		ProbeGsFrameUnwind(corruptCookie);
	} __except (1) {
		caught = 1;
	}
	return caught ? 0 : 30;
}

extern "C" __declspec(noinline) __declspec(safebuffers) int ProbeRunAlignedSehProtected(int corruptCookie) {
	int caught = 0;
	__try {
		ProbeAlignedGsFrameUnwind(corruptCookie);
	} __except (1) {
		caught = 1;
	}
	return caught ? 0 : 31;
}

extern "C" __declspec(noinline) __declspec(safebuffers) int ProbeRunNegativeAlignedSehProtected(int corruptCookie) {
	int caught = 0;
	__try {
		ProbeNegativeAlignedGsFrameUnwind(corruptCookie);
	} __except (1) {
		caught = 1;
	}
	return caught ? 0 : 32;
}

extern "C" __declspec(noinline) __declspec(safebuffers) int ProbeRunSetFrameSehProtected(int corruptCookie) {
	int caught = 0;
	__try {
		ProbeSetFrameGsFrameUnwind(corruptCookie);
	} __except (1) {
		caught = 1;
	}
	return caught ? 0 : 33;
}
)";

	constexpr const char* MINGW_DRIVER_SOURCE = R"(
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstring>
#include <thread>

extern "C" int ProbeVerifyEarlyCookie();
extern "C" int ProbeReadIsaAvailable();
extern "C" int ProbeThreadDynamicTLSCount();
extern "C" int ProbeAlignedOperatorNew();
extern "C" int ProbeRunSehProtected(int corruptCookie);
extern "C" int ProbeRunAlignedSehProtected(int corruptCookie);
extern "C" int ProbeRunNegativeAlignedSehProtected(int corruptCookie);
extern "C" int ProbeRunSetFrameSehProtected(int corruptCookie);

extern "C" void TriggerProbeRaise() {
	RaiseException(0xE0000042u, 0, 0, nullptr);
}

__asm__(
	".text\n"
	".globl TriggerProbeRaiseClobberRbp\n"
	".def TriggerProbeRaiseClobberRbp; .scl 2; .type 32; .endef\n"
	".seh_proc TriggerProbeRaiseClobberRbp\n"
	"TriggerProbeRaiseClobberRbp:\n"
	"\tpushq %rbp\n"
	"\t.seh_pushreg %rbp\n"
	"\tsubq $32, %rsp\n"
	"\t.seh_stackalloc 32\n"
	"\tleaq 16(%rsp), %rbp\n"
	"\t.seh_setframe %rbp, 16\n"
	"\t.seh_endprologue\n"
	"\tcallq TriggerProbeRaise\n"
	"\tnop\n"
	"\t.seh_startepilogue\n"
	"\tleaq 16(%rbp), %rsp\n"
	"\tpopq %rbp\n"
	"\t.seh_endepilogue\n"
	"\tretq\n"
	"\t.seh_endproc\n");

__asm__(
	".text\n"
	".globl ProbeGsFrameUnwind\n"
	".def ProbeGsFrameUnwind; .scl 2; .type 32; .endef\n"
	".seh_proc ProbeGsFrameUnwind\n"
	"ProbeGsFrameUnwind:\n"
	"\tsubq $56, %rsp\n"
	"\t.seh_stackalloc 56\n"
	"\t.seh_handler __GSHandlerCheck, @unwind, @except\n"
	"\t.seh_endprologue\n"
	"\tmovq __security_cookie(%rip), %rax\n"
	"\txorq %rsp, %rax\n"
	"\ttestl %ecx, %ecx\n"
	"\tje .Lgs_intact\n"
	"\txorq $0x5A5A5A5A, %rax\n"
	".Lgs_intact:\n"
	"\tmovq %rax, 40(%rsp)\n"
	"\tcallq TriggerProbeRaise\n"
	"\tnop\n"
	"\t.seh_startepilogue\n"
	"\taddq $56, %rsp\n"
	"\t.seh_endepilogue\n"
	"\tretq\n"
	"\t.seh_handlerdata\n"
	"\t.long 40\n"
	"\t.text\n"
	"\t.seh_endproc\n"
	"\t.globl ProbeAlignedGsFrameUnwind\n"
	".def ProbeAlignedGsFrameUnwind; .scl 2; .type 32; .endef\n"
	".seh_proc ProbeAlignedGsFrameUnwind\n"
	"ProbeAlignedGsFrameUnwind:\n"
	"\tpushq %rbp\n"
	"\t.seh_pushreg %rbp\n"
	"\tsubq $272, %rsp\n"
	"\t.seh_stackalloc 272\n"
	"\t.seh_handler __GSHandlerCheck, @unwind, @except\n"
	"\t.seh_endprologue\n"
	"\tleaq 96(%rsp), %rbp\n"
	"\tandq $-64, %rbp\n"
	"\tmovq __security_cookie(%rip), %rax\n"
	"\txorq %rsp, %rax\n"
	"\ttestl %ecx, %ecx\n"
	"\tje .Lgs_aligned_intact\n"
	"\txorq $0x5A5A5A5A, %rax\n"
	".Lgs_aligned_intact:\n"
	"\tmovq %rax, 144(%rbp)\n"
	"\tcallq TriggerProbeRaise\n"
	"\tnop\n"
	"\t.seh_startepilogue\n"
	"\taddq $272, %rsp\n"
	"\tpopq %rbp\n"
	"\t.seh_endepilogue\n"
	"\tretq\n"
	"\t.seh_handlerdata\n"
	"\t.long 0x94\n"
	"\t.long 0x60\n"
	"\t.long 0x40\n"
	"\t.text\n"
	"\t.seh_endproc\n"
	"\t.globl ProbeNegativeAlignedGsFrameUnwind\n"
	".def ProbeNegativeAlignedGsFrameUnwind; .scl 2; .type 32; .endef\n"
	".seh_proc ProbeNegativeAlignedGsFrameUnwind\n"
	"ProbeNegativeAlignedGsFrameUnwind:\n"
	"\tpushq %rbp\n"
	"\t.seh_pushreg %rbp\n"
	"\tsubq $272, %rsp\n"
	"\t.seh_stackalloc 272\n"
	"\t.seh_handler __GSHandlerCheck, @unwind, @except\n"
	"\t.seh_endprologue\n"
	"\tleaq 192(%rsp), %rbp\n"
	"\tandq $-64, %rbp\n"
	"\tmovq __security_cookie(%rip), %rax\n"
	"\txorq %rsp, %rax\n"
	"\ttestl %ecx, %ecx\n"
	"\tje .Lgs_neg_aligned_intact\n"
	"\txorq $0x5A5A5A5A, %rax\n"
	".Lgs_neg_aligned_intact:\n"
	"\tmovq %rax, -32(%rbp)\n"
	"\tcallq TriggerProbeRaise\n"
	"\tnop\n"
	"\t.seh_startepilogue\n"
	"\taddq $272, %rsp\n"
	"\tpopq %rbp\n"
	"\t.seh_endepilogue\n"
	"\tretq\n"
	"\t.seh_handlerdata\n"
	"\t.long 0xFFFFFFE4\n"
	"\t.long 0xC0\n"
	"\t.long 0x40\n"
	"\t.text\n"
	"\t.seh_endproc\n"
	"\t.globl ProbeSetFrameGsFrameUnwind\n"
	".def ProbeSetFrameGsFrameUnwind; .scl 2; .type 32; .endef\n"
	".seh_proc ProbeSetFrameGsFrameUnwind\n"
	"ProbeSetFrameGsFrameUnwind:\n"
	"\tpushq %rbp\n"
	"\t.seh_pushreg %rbp\n"
	"\tsubq $320, %rsp\n"
	"\t.seh_stackalloc 320\n"
	"\tleaq 128(%rsp), %rbp\n"
	"\t.seh_setframe %rbp, 128\n"
	"\t.seh_handler __GSHandlerCheck, @unwind, @except\n"
	"\t.seh_endprologue\n"
	"\tmovq __security_cookie(%rip), %rax\n"
	"\txorq %rbp, %rax\n"
	"\ttestl %ecx, %ecx\n"
	"\tje .Lgs_setframe_intact\n"
	"\txorq $0x5A5A5A5A, %rax\n"
	".Lgs_setframe_intact:\n"
	"\tmovq %rax, 184(%rbp)\n"
	"\tsubq $64, %rsp\n"
	"\tcallq TriggerProbeRaiseClobberRbp\n"
	"\tnop\n"
	"\t.seh_startepilogue\n"
	"\tleaq 192(%rbp), %rsp\n"
	"\tpopq %rbp\n"
	"\t.seh_endepilogue\n"
	"\tretq\n"
	"\t.seh_handlerdata\n"
	"\t.long 312\n"
	"\t.text\n"
	"\t.seh_endproc\n");

static LONG CALLBACK CatchProbeVectoredFilter(PEXCEPTION_POINTERS) {
	return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	if (argc < 2) {
		return 2;
	}
	const char* mode = argv[1];
	if (std::strcmp(mode, "cookie") == 0) {
		return ProbeVerifyEarlyCookie();
	}
	if (std::strcmp(mode, "isa") == 0) {
		return ProbeReadIsaAvailable();
	}
	if (std::strcmp(mode, "tls") == 0) {
		if (ProbeThreadDynamicTLSCount() != 1) {
			return 40;
		}
		std::atomic<int> failures{0};
		std::thread workers[6];
		for (auto& worker : workers) {
			worker = std::thread([&failures]() {
				if (ProbeThreadDynamicTLSCount() != 1) {
					failures.fetch_add(1);
				}
			});
		}
		for (auto& worker : workers) {
			worker.join();
		}
		return failures.load() == 0 ? 0 : 41;
	}
	if (std::strcmp(mode, "aligned-new") == 0) {
		return ProbeAlignedOperatorNew();
	}
	if (std::strcmp(mode, "gs-clean") == 0) {
		PVOID handle = AddVectoredExceptionHandler(1, &CatchProbeVectoredFilter);
		const int rc = ProbeRunSehProtected(0);
		if (handle) {
			RemoveVectoredExceptionHandler(handle);
		}
		return rc;
	}
	if (std::strcmp(mode, "gs-corrupt") == 0) {
		return ProbeRunSehProtected(1);
	}
	if (std::strcmp(mode, "gs-aligned-clean") == 0) {
		PVOID handle = AddVectoredExceptionHandler(1, &CatchProbeVectoredFilter);
		const int rc = ProbeRunAlignedSehProtected(0);
		if (handle) {
			RemoveVectoredExceptionHandler(handle);
		}
		return rc;
	}
	if (std::strcmp(mode, "gs-aligned-corrupt") == 0) {
		return ProbeRunAlignedSehProtected(1);
	}
	if (std::strcmp(mode, "gs-neg-aligned-clean") == 0) {
		PVOID handle = AddVectoredExceptionHandler(1, &CatchProbeVectoredFilter);
		const int rc = ProbeRunNegativeAlignedSehProtected(0);
		if (handle) {
			RemoveVectoredExceptionHandler(handle);
		}
		return rc;
	}
	if (std::strcmp(mode, "gs-neg-aligned-corrupt") == 0) {
		return ProbeRunNegativeAlignedSehProtected(1);
	}
	if (std::strcmp(mode, "gs-setframe-clean") == 0) {
		PVOID handle = AddVectoredExceptionHandler(1, &CatchProbeVectoredFilter);
		const int rc = ProbeRunSetFrameSehProtected(0);
		if (handle) {
			RemoveVectoredExceptionHandler(handle);
		}
		return rc;
	}
	if (std::strcmp(mode, "gs-setframe-corrupt") == 0) {
		return ProbeRunSetFrameSehProtected(1);
	}
	return 3;
}
)";

	bool HostSupportsAvx2() {
		int regs[4] = {0, 0, 0, 0};
		__cpuid(regs, 0);
		if (regs[0] < 7) {
			return false;
		}
		__cpuid(regs, 1);
		const unsigned int ecx1 = static_cast<unsigned int>(regs[2]);
		if ((ecx1 & (1u << 27)) == 0u || (ecx1 & (1u << 28)) == 0u || (ecx1 & (1u << 12)) == 0u) {
			return false;
		}
		int regs7[4] = {0, 0, 0, 0};
		__cpuidex(regs7, 7, 0);
		const unsigned int ebx7 = static_cast<unsigned int>(regs7[1]);
		return (ebx7 & (1u << 5)) != 0u && (ebx7 & (1u << 3)) != 0u && (ebx7 & (1u << 8)) != 0u;
	}

	class WinAOTCRTBridgeTest : public ::testing::Test {
	protected:
		static fs::path _suiteDir;
		static str _probeExePath;
		static str _skipReason;

		static void SetUpTestSuite() {
			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = ScriptProjectOps::GetSDKRoot();
			options.probeVersions = false;
			const CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);
			if (!toolchain.IsComplete()) {
				_skipReason = "C++ toolchain unavailable";
				return;
			}

			std::error_code ec;
			const fs::path staticShim = fs::path(FileSystem::GetExecutablePath().ToString()).parent_path() / "CSharp" / "native" / "CRTShimStatic.obj";
			if (!fs::is_regular_file(staticShim, ec)) {
				_skipReason = "CSharp/native/CRTShimStatic.obj not found";
				return;
			}

			const fs::path clangDriver = fs::path(toolchain.compiler.path).parent_path() / "clang++.exe";
			const str msvcCompiler = fs::is_regular_file(clangDriver, ec) ? clangDriver.generic_string() : toolchain.compiler.path;

			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_suiteDir = fs::temp_directory_path() / ("slt_winaot_crt_bridge_" + std::to_string(stamp));
			fs::create_directories(_suiteDir, ec);

			const fs::path msvcSrc = _suiteDir / "probe_msvc.cpp";
			const fs::path msvcObj = _suiteDir / "probe_msvc.obj";
			const fs::path driverSrc = _suiteDir / "probe_driver.cpp";
			const fs::path probeExe = _suiteDir / "bridge_probe.exe";

			ASSERT_TRUE(FileSystem::WriteTextFile(Path(msvcSrc.generic_string()), str(MSVC_ABI_PROBE_SOURCE)));
			ASSERT_TRUE(FileSystem::WriteTextFile(Path(driverSrc.generic_string()), str(MINGW_DRIVER_SOURCE)));

			int exitCode = 0;
			const str compileMsvcCmd =
				"\"" + msvcCompiler +
				"\" --target=x86_64-pc-windows-msvc -std=c++20 -O2 -fms-extensions -faligned-allocation -fstack-protector-strong -c \"" +
				msvcSrc.generic_string() + "\" -o \"" + msvcObj.generic_string() + "\"";
			const str compileLog = TestPlatform::RunCommand(compileMsvcCmd, exitCode);
			ASSERT_EQ(exitCode, 0) << "MSVC-target object compile failed: " << compileLog;

			const str linkCmd = "\"" + toolchain.compiler.path + "\" -std=c++20 -O2 -Wall -Wextra -Werror -Wl,-u,__dyn_tls_init \"" +
								driverSrc.generic_string() + "\" \"" + msvcObj.generic_string() + "\" \"" +
								staticShim.generic_string() + "\" -lbcrypt -o \"" + probeExe.generic_string() + "\"";
			const str linkLog = TestPlatform::RunCommand(linkCmd, exitCode);
			ASSERT_EQ(exitCode, 0) << "Bridge probe link failed: " << linkLog;
			_probeExePath = probeExe.generic_string();
		}

		static void TearDownTestSuite() {
			if (!_suiteDir.empty()) {
				std::error_code ec;
				fs::remove_all(_suiteDir, ec);
			}
			_suiteDir.clear();
			_probeExePath.clear();
		}

		void SetUp() override {
			if (!_skipReason.empty()) {
				GTEST_SKIP() << _skipReason;
			}
			ASSERT_FALSE(_probeExePath.empty());
		}

		TestPlatform::ProcessResult RunProbe(const char* mode) const {
			return TestPlatform::RunProcess("\"" + _probeExePath + "\" " + mode, 15000);
		}
	};

	fs::path WinAOTCRTBridgeTest::_suiteDir;
	str WinAOTCRTBridgeTest::_probeExePath;
	str WinAOTCRTBridgeTest::_skipReason;

} // anonymous namespace

TEST_F(WinAOTCRTBridgeTest, SecurityCookieInitializedBeforeCppStaticConstructors) {
	const TestPlatform::ProcessResult result = RunProbe("cookie");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, IsaAvailableReflectsHostAvx2Support) {
	const TestPlatform::ProcessResult result = RunProbe("isa");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	if (HostSupportsAvx2()) {
		EXPECT_GE(result.exitCode, 5);
	} else {
		EXPECT_GE(result.exitCode, 1);
	}
}

TEST_F(WinAOTCRTBridgeTest, DynamicTLSCallbacksExecuteExactlyOncePerThread) {
	const TestPlatform::ProcessResult result = RunProbe("tls");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, MsvcOverAlignedOperatorNewHonorsSixtyFourByteAlignment) {
	const TestPlatform::ProcessResult result = RunProbe("aligned-new");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, IntactGsFrameUnwindsCleanlyThroughSehHandler) {
	const TestPlatform::ProcessResult result = RunProbe("gs-clean");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, CorruptedGsFrameCookieTerminatesWithStackBufferOverrun) {
	const TestPlatform::ProcessResult result = RunProbe("gs-corrupt");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, STATUS_STACK_BUFFER_OVERRUN_CODE);
}

TEST_F(WinAOTCRTBridgeTest, IntactAlignedGsFrameUnwindsCleanlyThroughSehHandler) {
	const TestPlatform::ProcessResult result = RunProbe("gs-aligned-clean");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, CorruptedAlignedGsFrameCookieTerminatesWithStackBufferOverrun) {
	const TestPlatform::ProcessResult result = RunProbe("gs-aligned-corrupt");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, STATUS_STACK_BUFFER_OVERRUN_CODE);
}

TEST_F(WinAOTCRTBridgeTest, IntactNegativeOffsetAlignedGsFrameUnwindsCleanlyThroughSehHandler) {
	const TestPlatform::ProcessResult result = RunProbe("gs-neg-aligned-clean");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, CorruptedNegativeOffsetAlignedGsFrameCookieTerminatesWithStackBufferOverrun) {
	const TestPlatform::ProcessResult result = RunProbe("gs-neg-aligned-corrupt");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, STATUS_STACK_BUFFER_OVERRUN_CODE);
}

TEST_F(WinAOTCRTBridgeTest, IntactSetFrameGsFrameUnwindsCleanlyAcrossInnerRbpClobber) {
	const TestPlatform::ProcessResult result = RunProbe("gs-setframe-clean");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, 0);
}

TEST_F(WinAOTCRTBridgeTest, CorruptedSetFrameGsFrameCookieTerminatesWithStackBufferOverrun) {
	const TestPlatform::ProcessResult result = RunProbe("gs-setframe-corrupt");
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.exited);
	EXPECT_EQ(result.exitCode, STATUS_STACK_BUFFER_OVERRUN_CODE);
}

TEST(WindowsUTF8ManifestTest, TestsExecutableRunsWithUTF8ActiveCodePage) {
	EXPECT_EQ(GetACP(), static_cast<UINT>(CP_UTF8));
}

#endif
