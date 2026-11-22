#pragma once

// shared Win64 /GS stack-cookie and SEH unwind core used by CRTShim.obj and CRTShimStatic.obj

using Size = unsigned long long;

constexpr Size DEFAULT_COOKIE = 0x00002B992DDFA232ull;
constexpr Size COOKIE_MASK = 0x0000FFFFFFFFFFFFull;
constexpr unsigned int FAIL_STACK_COOKIE = 2u;
constexpr unsigned int FAIL_RANGE_CHECK = 8u;
constexpr unsigned long BCRYPT_USE_SYSTEM_PREFERRED_RNG_FLAG = 0x00000002ul;
constexpr int COOKIE_SEED_ATTEMPTS = 8;
constexpr unsigned char UNWIND_CHAIN_INFO_FLAG = 0x04u;

struct GSHandlerData {
	int cookieOffset;
	int alignedBaseOffset;
	int alignment;
};

struct Win64RuntimeFunction {
	unsigned int beginAddress;
	unsigned int endAddress;
	unsigned int unwindInfoAddress;
};

struct Win64UnwindInfo {
	unsigned char versionAndFlags;
	unsigned char sizeOfProlog;
	unsigned char countOfCodes;
	unsigned char frameRegisterAndOffset;
	unsigned short unwindCodes[1];
};

struct Win64DispatcherContext {
	Size controlPc;
	Size imageBase;
	const Win64RuntimeFunction* functionEntry;
	Size establisherFrame;
	Size targetIp;
	void* contextRecord;
	void* languageHandler;
	const void* handlerData;
};

extern "C" {

__declspec(dllimport) long __stdcall BCryptGenRandom(void*, unsigned char*, unsigned long, unsigned long);
// compiler-rt's stack probe (same contract)
void ___chkstk_ms();

Size __security_cookie = DEFAULT_COOKIE;
Size __security_cookie_complement = ~DEFAULT_COOKIE;

[[noreturn]] static inline void FastFail(unsigned int code) noexcept {
	__asm__ volatile("int $0x29" : : "c"(code));
	__builtin_unreachable();
}

// MSVC /GS epilogs call __security_check_cookie after loading the return value into rax
__attribute__((naked)) void __security_check_cookie(Size) {
	__asm__(
		"cmpq __security_cookie(%rip), %rcx\n"
		"jne 1f\n"
		"rolq $16, %rcx\n"
		"testw %cx, %cx\n"
		"jne 1f\n"
		"rorq $16, %rcx\n"
		"retq\n"
		"1:\n"
		"movl $2, %ecx\n"
		"int $0x29\n");
}

__attribute__((noreturn)) void __report_rangecheckfailure() {
	FastFail(FAIL_RANGE_CHECK);
}

static Size ResolveFrameXor(Size establisherFrame, const Win64DispatcherContext* dispatcher) noexcept {
	if (!dispatcher || !dispatcher->functionEntry || dispatcher->imageBase == 0) {
		return establisherFrame;
	}
	unsigned int unwindRva = dispatcher->functionEntry->unwindInfoAddress;
	for (int hop = 0; hop < 16 && unwindRva != 0; ++hop) {
		const auto* unwindInfo = reinterpret_cast<const Win64UnwindInfo*>(dispatcher->imageBase + unwindRva);
		const unsigned char frameReg = static_cast<unsigned char>(unwindInfo->frameRegisterAndOffset & 0x0Fu);
		if (frameReg != 0u) {
			// frame-pointer functions xor their cookie with the frame register (establisherFrame + 16 * frameOffset)
			const Size scaledOffset = static_cast<Size>((unwindInfo->frameRegisterAndOffset >> 4) & 0x0Fu) * 16u;
			return establisherFrame + scaledOffset;
		}
		const unsigned char flags = static_cast<unsigned char>(unwindInfo->versionAndFlags >> 3);
		if ((flags & UNWIND_CHAIN_INFO_FLAG) == 0u) {
			break;
		}
		const Size alignedCodes = (static_cast<Size>(unwindInfo->countOfCodes) + 1u) & ~Size(1u);
		const auto* chained = reinterpret_cast<const Win64RuntimeFunction*>(&unwindInfo->unwindCodes[alignedCodes]);
		unwindRva = chained->unwindInfoAddress;
	}
	return establisherFrame;
}

int __GSHandlerCheck(void*, void* establisherFrame, void*, void* dispatcherContext) {
	const auto* dispatcher = static_cast<const Win64DispatcherContext*>(dispatcherContext);
	if (dispatcher && dispatcher->handlerData) {
		const auto* gsData = static_cast<const GSHandlerData*>(dispatcher->handlerData);
		const Size frame = reinterpret_cast<Size>(establisherFrame);
		Size cookieBase = frame;
		if ((static_cast<unsigned int>(gsData->cookieOffset) & 0x4u) != 0u) {
			const long long aligned = static_cast<long long>(frame) + static_cast<long long>(gsData->alignedBaseOffset);
			const long long mask = -static_cast<long long>(gsData->alignment);
			cookieBase = static_cast<Size>(aligned & mask);
		}
		const int signedOffset = static_cast<int>(static_cast<unsigned int>(gsData->cookieOffset) & ~0x7u);
		const Size cookieAddr = static_cast<Size>(static_cast<long long>(cookieBase) + static_cast<long long>(signedOffset));
		const Size xorFrame = ResolveFrameXor(frame, dispatcher);
		const Size actualCookie = *reinterpret_cast<const Size*>(cookieAddr) ^ xorFrame;
		__security_check_cookie(actualCookie);
	}
	return 1;
}

__attribute__((naked)) void __chkstk() {
	__asm__("jmp ___chkstk_ms");
}

static void SeedSecurityCookie() noexcept {
	for (int attempt = 0; attempt < COOKIE_SEED_ATTEMPTS; ++attempt) {
		Size sample = 0;
		const long status = BCryptGenRandom(nullptr, reinterpret_cast<unsigned char*>(&sample),
											static_cast<unsigned long>(sizeof(sample)), BCRYPT_USE_SYSTEM_PREFERRED_RNG_FLAG);
		if (status < 0) {
			FastFail(FAIL_STACK_COOKIE);
		}
		sample &= COOKIE_MASK;
		if (sample != 0 && sample != DEFAULT_COOKIE) {
			__security_cookie = sample;
			__security_cookie_complement = ~sample;
			return;
		}
	}
	FastFail(FAIL_STACK_COOKIE);
}

} // extern "C"
