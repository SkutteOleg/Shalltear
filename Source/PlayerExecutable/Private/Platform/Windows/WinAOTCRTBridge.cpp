#if defined(_WIN32) && defined(__x86_64__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" {
std::uintptr_t _securityCookie asm("__security_cookie") = 0x00002B992DDFA232ULL;
std::uintptr_t _securityCookieComplement asm("__security_cookie_complement") = ~0x00002B992DDFA232ULL;
int _isaAvailable asm("__isa_available") = 0;
unsigned int _isaFavor asm("__favor") = 0;
int _avx2WmemEnabled asm("_Avx2WmemEnabled") = 0;
__thread unsigned char _tlsGuard asm("__tls_guard") = 0;
__attribute__((weak)) int _fltUsed asm("_fltused") = 0;
extern const unsigned char MSVC_STD_NOTHROW asm("?nothrow@std@@3Unothrow_t@1@B");
const unsigned char MSVC_STD_NOTHROW = 0;

using CRTPvfv = void(__cdecl*)();
__attribute__((section(".CRT$XDA"), weak, used)) extern const CRTPvfv XD_BEGIN asm("__xd_a") = nullptr;
__attribute__((section(".CRT$XDZ"), weak, used)) extern const CRTPvfv XD_END asm("__xd_z") = nullptr;

void __cdecl DynTLSOnDemandInit() asm("__dyn_tls_on_demand_init");
void NTAPI DynTLSInit(PVOID, DWORD reason, PVOID) asm("__dyn_tls_init");
void SecurityCheckCookie(std::uintptr_t cookie) asm("__security_check_cookie");
}

namespace {

	constexpr std::uintptr_t DEFAULT_SECURITY_COOKIE = 0x00002B992DDFA232ULL;
	constexpr std::uintptr_t SECURITY_COOKIE_MASK = 0x0000FFFFFFFFFFFFULL;
	constexpr unsigned int FAST_FAIL_STACK_COOKIE_FAILURE = 2u;
	constexpr std::uint8_t UNWIND_CHAIN_INFO_FLAG = 0x04u;
	constexpr int COOKIE_SEED_ATTEMPTS = 8;

	struct GSHandlerData {
		std::int32_t cookieOffset;
		std::int32_t alignedBaseOffset;
		std::int32_t alignment;
	};

	struct Win64RuntimeFunction {
		std::uint32_t beginAddress;
		std::uint32_t endAddress;
		std::uint32_t unwindInfoAddress;
	};

	struct Win64UnwindInfo {
		std::uint8_t versionAndFlags;
		std::uint8_t sizeOfProlog;
		std::uint8_t countOfCodes;
		std::uint8_t frameRegisterAndOffset;
		std::uint16_t unwindCodes[1];
	};

	__attribute__((no_stack_protector)) std::uintptr_t ResolveActiveFrameXorValue(
		std::uintptr_t establisherFrame,
		const DISPATCHER_CONTEXT* dispatcherContext) noexcept {
		if (!dispatcherContext || !dispatcherContext->FunctionEntry ||
			dispatcherContext->ImageBase == 0) {
			return establisherFrame;
		}
		const auto* fnEntry = reinterpret_cast<const Win64RuntimeFunction*>(dispatcherContext->FunctionEntry);
		if (fnEntry->unwindInfoAddress == 0) {
			return establisherFrame;
		}

		std::uint32_t unwindRva = fnEntry->unwindInfoAddress;
		for (int hop = 0; hop < 16 && unwindRva != 0; ++hop) {
			const auto* unwindInfo = reinterpret_cast<const Win64UnwindInfo*>(
				dispatcherContext->ImageBase + static_cast<std::uintptr_t>(unwindRva));
			const std::uint8_t frameReg = static_cast<std::uint8_t>(unwindInfo->frameRegisterAndOffset & 0x0Fu);
			if (frameReg != 0u) {
				// frame-pointer functions xor their cookie with the frame register (establisherFrame + 16 * frameOffset)
				const std::uintptr_t scaledOffset =
					static_cast<std::uintptr_t>((unwindInfo->frameRegisterAndOffset >> 4) & 0x0Fu) * 16u;
				return establisherFrame + scaledOffset;
			}

			const std::uint8_t flags = static_cast<std::uint8_t>(unwindInfo->versionAndFlags >> 3);
			if ((flags & UNWIND_CHAIN_INFO_FLAG) == 0u) {
				break;
			}

			const std::size_t alignedCodes = (static_cast<std::size_t>(unwindInfo->countOfCodes) + 1u) & ~std::size_t(1u);
			const auto* chained = reinterpret_cast<const Win64RuntimeFunction*>(&unwindInfo->unwindCodes[alignedCodes]);
			unwindRva = chained->unwindInfoAddress;
		}

		return establisherFrame;
	}

	std::atomic<int> _bridgeInitState{0};

	__attribute__((no_stack_protector, target("xsave"))) unsigned long long ReadXcr0() noexcept {
		return _xgetbv(0);
	}

	__attribute__((no_stack_protector)) void QueryCpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4]) noexcept {
		asm volatile("cpuid"
					 : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
					 : "a"(leaf), "c"(subleaf));
	}

	__attribute__((no_stack_protector)) void DetectCPUIsa() noexcept {
		unsigned int regs[4] = {0u, 0u, 0u, 0u};
		QueryCpuid(0u, 0u, regs);
		const unsigned int maxLeaf = regs[0];
		int isa = 0;
		int avx2Mem = 0;

		if (maxLeaf >= 1u) {
			QueryCpuid(1u, 0u, regs);
			const unsigned int ecx1 = regs[2];
			const bool hasFma = (ecx1 & (1u << 12)) != 0u;
			const bool hasSse42 = (ecx1 & (1u << 20)) != 0u;
			const bool hasOsxsave = (ecx1 & (1u << 27)) != 0u;
			const bool hasAvx = (ecx1 & (1u << 28)) != 0u;

			if (hasSse42) {
				isa = 1;
			}
			if (hasSse42 && hasOsxsave && hasAvx) {
				const unsigned long long xcr0 = ReadXcr0();
				if ((xcr0 & 0x6ULL) == 0x6ULL) {
					isa = 2;
					if (maxLeaf >= 7u) {
						unsigned int regs7[4] = {0u, 0u, 0u, 0u};
						QueryCpuid(7u, 0u, regs7);
						const unsigned int ebx7 = regs7[1];
						const bool hasBmi1 = (ebx7 & (1u << 3)) != 0u;
						const bool hasAvx2 = (ebx7 & (1u << 5)) != 0u;
						const bool hasBmi2 = (ebx7 & (1u << 8)) != 0u;
						if (hasAvx2 && hasBmi1 && hasBmi2 && hasFma) {
							isa = 5;
							avx2Mem = 1;
							const bool hasAvx512F = (ebx7 & (1u << 16)) != 0u;
							const bool hasAvx512Dq = (ebx7 & (1u << 17)) != 0u;
							const bool hasAvx512Bw = (ebx7 & (1u << 30)) != 0u;
							const bool hasAvx512Vl = (ebx7 & (1u << 31)) != 0u;
							if ((xcr0 & 0xE6ULL) == 0xE6ULL && hasAvx512F && hasAvx512Dq && hasAvx512Bw && hasAvx512Vl) {
								isa = 6;
							}
						}
					}
				}
			}
		}

		_isaAvailable = isa;
		_avx2WmemEnabled = avx2Mem;
		_isaFavor = 2u;
	}

	__attribute__((no_stack_protector)) void InitWinAOTCRTBridge() noexcept {
		if (_bridgeInitState.load(std::memory_order_acquire) == 2) {
			return;
		}
		int expected = 0;
		if (_bridgeInitState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
			std::uintptr_t cookie = 0;
			bool seeded = false;
			for (int attempt = 0; attempt < COOKIE_SEED_ATTEMPTS; ++attempt) {
				std::uintptr_t sample = 0;
				const NTSTATUS status = BCryptGenRandom(
					nullptr,
					reinterpret_cast<PUCHAR>(&sample),
					static_cast<ULONG>(sizeof(sample)),
					BCRYPT_USE_SYSTEM_PREFERRED_RNG);
				if (status < 0) {
					__fastfail(FAST_FAIL_STACK_COOKIE_FAILURE);
				}
				sample &= SECURITY_COOKIE_MASK;
				if (sample != 0 && sample != DEFAULT_SECURITY_COOKIE) {
					cookie = sample;
					seeded = true;
					break;
				}
			}
			if (!seeded) {
				__fastfail(FAST_FAIL_STACK_COOKIE_FAILURE);
			}
			_securityCookie = cookie;
			_securityCookieComplement = ~cookie;
			DetectCPUIsa();
			_bridgeInitState.store(2, std::memory_order_release);
			return;
		}
		while (_bridgeInitState.load(std::memory_order_acquire) != 2) {
			_mm_pause();
		}
	}

	__attribute__((no_stack_protector)) void NTAPI OnWinAOTEarlyTLSCallback(PVOID, DWORD reason, PVOID) {
		if (reason == DLL_PROCESS_ATTACH) {
			InitWinAOTCRTBridge();
		}
	}

	__attribute__((no_stack_protector)) int __cdecl OnWinAOTCRTInit() {
		InitWinAOTCRTBridge();
		DynTLSOnDemandInit();
		return 0;
	}

	using CRTInitFn = int(__cdecl*)();

	__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK EARLY_TLS_CALLBACK = &OnWinAOTEarlyTLSCallback;
	__attribute__((section(".CRT$XIAA"), used)) const CRTInitFn EARLY_CRT_INIT = &OnWinAOTCRTInit;

} // namespace

extern "C" {

void NTAPI DynTLSInit(PVOID, DWORD reason, PVOID) {
	if (reason == DLL_THREAD_ATTACH) {
		InitWinAOTCRTBridge();
		DynTLSOnDemandInit();
	}
}

__attribute__((section(".CRT$XLC"), used)) extern const PIMAGE_TLS_CALLBACK DYN_TLS_INIT_CALLBACK asm("__dyn_tls_init_callback") = &DynTLSInit;

__attribute__((no_stack_protector)) EXCEPTION_DISPOSITION GSHandlerCheck(
	PEXCEPTION_RECORD,
	PVOID establisherFrame,
	PCONTEXT,
	PDISPATCHER_CONTEXT dispatcherContext) asm("__GSHandlerCheck");

__attribute__((no_stack_protector)) EXCEPTION_DISPOSITION GSHandlerCheck(
	PEXCEPTION_RECORD,
	PVOID establisherFrame,
	PCONTEXT,
	PDISPATCHER_CONTEXT dispatcherContext) {
	const auto* gsData = static_cast<const GSHandlerData*>(dispatcherContext->HandlerData);
	const auto frame = reinterpret_cast<std::uintptr_t>(establisherFrame);
	std::uintptr_t cookieBase = frame;
	if ((static_cast<std::uint32_t>(gsData->cookieOffset) & 0x4u) != 0u) {
		const auto aligned = static_cast<std::intptr_t>(frame) + static_cast<std::intptr_t>(gsData->alignedBaseOffset);
		const auto mask = -static_cast<std::intptr_t>(gsData->alignment);
		cookieBase = static_cast<std::uintptr_t>(aligned & mask);
	}
	const auto signedCookieOffset =
		static_cast<std::int32_t>(static_cast<std::uint32_t>(gsData->cookieOffset) & ~0x7u);
	const auto cookieAddr = static_cast<std::uintptr_t>(
		static_cast<std::intptr_t>(cookieBase) + static_cast<std::intptr_t>(signedCookieOffset));
	const std::uintptr_t xorFrame = ResolveActiveFrameXorValue(frame, dispatcherContext);
	const std::uintptr_t actualCookie = *reinterpret_cast<const std::uintptr_t*>(cookieAddr) ^ xorFrame;
	if (actualCookie != _securityCookie || (actualCookie >> 48) != 0u) {
		__fastfail(FAST_FAIL_STACK_COOKIE_FAILURE);
	}
	SecurityCheckCookie(actualCookie);
	return ExceptionContinueSearch;
}

__attribute__((noreturn, no_stack_protector)) void ReportRangeCheckFailure() asm("__report_rangecheckfailure");
__attribute__((noreturn, no_stack_protector)) void ReportRangeCheckFailure() {
	__fastfail(8u);
	__builtin_unreachable();
}

void __cdecl DynTLSOnDemandInit() asm("__dyn_tls_on_demand_init");
void __cdecl DynTLSOnDemandInit() {
	if (_tlsGuard != 0) {
		return;
	}
	_tlsGuard = 1;
	std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(&XD_BEGIN) + sizeof(CRTPvfv);
	const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(&XD_END);
	asm volatile("" : "+r"(cur));
	for (; cur < end; cur += sizeof(CRTPvfv)) {
		const CRTPvfv fn = *reinterpret_cast<CRTPvfv const*>(cur);
		if (fn != nullptr) {
			fn();
		}
	}
}

void* MsvcOperatorNew(std::size_t size) asm("??2@YAPEAX_K@Z");
void* MsvcOperatorNew(std::size_t size) {
	return ::operator new(size);
}

void* MsvcOperatorNewArray(std::size_t size) asm("??_U@YAPEAX_K@Z");
void* MsvcOperatorNewArray(std::size_t size) {
	return ::operator new[](size);
}

void* MsvcOperatorNewNothrow(std::size_t size, const void*) noexcept asm("??2@YAPEAX_KAEBUnothrow_t@std@@@Z");
void* MsvcOperatorNewNothrow(std::size_t size, const void*) noexcept {
	return ::operator new(size, std::nothrow);
}

void* MsvcOperatorNewArrayNothrow(std::size_t size, const void*) noexcept asm("??_U@YAPEAX_KAEBUnothrow_t@std@@@Z");
void* MsvcOperatorNewArrayNothrow(std::size_t size, const void*) noexcept {
	return ::operator new[](size, std::nothrow);
}

void MsvcOperatorDelete(void* ptr) noexcept asm("??3@YAXPEAX@Z");
void MsvcOperatorDelete(void* ptr) noexcept {
	::operator delete(ptr);
}

void MsvcOperatorDeleteArray(void* ptr) noexcept asm("??_V@YAXPEAX@Z");
void MsvcOperatorDeleteArray(void* ptr) noexcept {
	::operator delete[](ptr);
}

void MsvcOperatorDeleteSized(void* ptr, std::size_t) noexcept asm("??3@YAXPEAX_K@Z");
void MsvcOperatorDeleteSized(void* ptr, std::size_t) noexcept {
	::operator delete(ptr);
}

void MsvcOperatorDeleteArraySized(void* ptr, std::size_t) noexcept asm("??_V@YAXPEAX_K@Z");
void MsvcOperatorDeleteArraySized(void* ptr, std::size_t) noexcept {
	::operator delete[](ptr);
}

void MsvcOperatorDeleteNothrow(void* ptr, const void*) noexcept asm("??3@YAXPEAXAEBUnothrow_t@std@@@Z");
void MsvcOperatorDeleteNothrow(void* ptr, const void*) noexcept {
	::operator delete(ptr, std::nothrow);
}

void MsvcOperatorDeleteArrayNothrow(void* ptr, const void*) noexcept asm("??_V@YAXPEAXAEBUnothrow_t@std@@@Z");
void MsvcOperatorDeleteArrayNothrow(void* ptr, const void*) noexcept {
	::operator delete[](ptr, std::nothrow);
}

void* MsvcOperatorNewAligned(std::size_t size, std::align_val_t align) asm("??2@YAPEAX_KW4align_val_t@std@@@Z");
void* MsvcOperatorNewAligned(std::size_t size, std::align_val_t align) {
	return ::operator new(size, align);
}

void* MsvcOperatorNewArrayAligned(std::size_t size, std::align_val_t align) asm("??_U@YAPEAX_KW4align_val_t@std@@@Z");
void* MsvcOperatorNewArrayAligned(std::size_t size, std::align_val_t align) {
	return ::operator new[](size, align);
}

void* MsvcOperatorNewAlignedNothrow(std::size_t size, std::align_val_t align, const void*) noexcept asm("??2@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z");
void* MsvcOperatorNewAlignedNothrow(std::size_t size, std::align_val_t align, const void*) noexcept {
	return ::operator new(size, align, std::nothrow);
}

void* MsvcOperatorNewArrayAlignedNothrow(std::size_t size, std::align_val_t align, const void*) noexcept asm("??_U@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z");
void* MsvcOperatorNewArrayAlignedNothrow(std::size_t size, std::align_val_t align, const void*) noexcept {
	return ::operator new[](size, align, std::nothrow);
}

void MsvcOperatorDeleteAligned(void* ptr, std::align_val_t align) noexcept asm("??3@YAXPEAXW4align_val_t@std@@@Z");
void MsvcOperatorDeleteAligned(void* ptr, std::align_val_t align) noexcept {
	::operator delete(ptr, align);
}

void MsvcOperatorDeleteArrayAligned(void* ptr, std::align_val_t align) noexcept asm("??_V@YAXPEAXW4align_val_t@std@@@Z");
void MsvcOperatorDeleteArrayAligned(void* ptr, std::align_val_t align) noexcept {
	::operator delete[](ptr, align);
}

void MsvcOperatorDeleteSizedAligned(void* ptr, std::size_t, std::align_val_t align) noexcept asm("??3@YAXPEAX_KW4align_val_t@std@@@Z");
void MsvcOperatorDeleteSizedAligned(void* ptr, std::size_t, std::align_val_t align) noexcept {
	::operator delete(ptr, align);
}

void MsvcOperatorDeleteArraySizedAligned(void* ptr, std::size_t, std::align_val_t align) noexcept asm("??_V@YAXPEAX_KW4align_val_t@std@@@Z");
void MsvcOperatorDeleteArraySizedAligned(void* ptr, std::size_t, std::align_val_t align) noexcept {
	::operator delete[](ptr, align);
}

void MsvcOperatorDeleteAlignedNothrow(void* ptr, std::align_val_t align, const void*) noexcept asm("??3@YAXPEAXW4align_val_t@std@@AEBUnothrow_t@1@@Z");
void MsvcOperatorDeleteAlignedNothrow(void* ptr, std::align_val_t align, const void*) noexcept {
	::operator delete(ptr, align, std::nothrow);
}

void MsvcOperatorDeleteArrayAlignedNothrow(void* ptr, std::align_val_t align, const void*) noexcept asm("??_V@YAXPEAXW4align_val_t@std@@AEBUnothrow_t@1@@Z");
void MsvcOperatorDeleteArrayAlignedNothrow(void* ptr, std::align_val_t align, const void*) noexcept {
	::operator delete[](ptr, align, std::nothrow);
}

} // extern "C"

__asm__(
	".text\n"
	".globl __chkstk\n"
	".def __chkstk; .scl 2; .type 32; .endef\n"
	"__chkstk:\n"
	"\tjmp ___chkstk_ms\n"
	".globl __security_check_cookie\n"
	".def __security_check_cookie; .scl 2; .type 32; .endef\n"
	"__security_check_cookie:\n"
	"\tcmpq __security_cookie(%rip), %rcx\n"
	"\tjne .Lsec_cookie_fail\n"
	"\trolq $16, %rcx\n"
	"\ttestw %cx, %cx\n"
	"\tjne .Lsec_cookie_fail\n"
	"\trorq $16, %rcx\n"
	"\tretq\n"
	".Lsec_cookie_fail:\n"
	"\tmovl $2, %ecx\n"
	"\tint $0x29\n");

#endif
