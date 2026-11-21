// startup symbols expected by NativeAOT's objects (x86_64-pc-windows-msvc, freestanding)

using Size = unsigned long long;
using InitFn = int (*)();
using VoidFn = void (*)();
using TLSCallbackFn = void(__stdcall*)(void*, unsigned long, void*);

constexpr unsigned long PROCESS_DETACH = 0;
constexpr unsigned long PROCESS_ATTACH = 1;
constexpr unsigned long THREAD_ATTACH = 2;
constexpr int EXIT_CAPACITY = 512;
constexpr Size DEFAULT_COOKIE = 0x00002B992DDFA232ull;
constexpr Size COOKIE_MASK = 0x0000FFFFFFFFFFFFull;
constexpr unsigned int FAIL_STACK_COOKIE = 2;
constexpr unsigned int FAIL_RANGE_CHECK = 8;
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

void* malloc(Size);
void free(void*);
int __stdcall DllMain(void*, unsigned long, void*);
__declspec(dllimport) long __stdcall BCryptGenRandom(void*, unsigned char*, unsigned long, unsigned long);
// compiler-rt's stack probe (same contract)
void ___chkstk_ms();

Size __security_cookie = DEFAULT_COOKIE;
int _fltused = 0x9875;
unsigned long _tls_index = 0;

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

void __report_rangecheckfailure() {
	__fastfail(FAIL_RANGE_CHECK);
}

static Size ResolveFrameXor(Size establisherFrame, const Win64DispatcherContext* dispatcher) {
	if (!dispatcher || !dispatcher->functionEntry || dispatcher->imageBase == 0) {
		return establisherFrame;
	}
	unsigned int unwindRva = dispatcher->functionEntry->unwindInfoAddress;
	for (int hop = 0; hop < 16 && unwindRva != 0; ++hop) {
		const auto* unwindInfo = reinterpret_cast<const Win64UnwindInfo*>(dispatcher->imageBase + unwindRva);
		const unsigned char frameReg = static_cast<unsigned char>(unwindInfo->frameRegisterAndOffset & 0x0Fu);
		if (frameReg != 0u) {
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

static VoidFn _exitHandlers[EXIT_CAPACITY];
static int _exitHandlerCount;

int atexit(VoidFn handler) {
	if (_exitHandlerCount >= EXIT_CAPACITY) {
		return -1;
	}
	_exitHandlers[_exitHandlerCount++] = handler;
	return 0;
}

__attribute__((naked)) void __guard_dispatch_icall_nop() {
	__asm__("jmp *%rax");
}
void* __guard_dispatch_icall_fptr = reinterpret_cast<void*>(&__guard_dispatch_icall_nop);

__attribute__((naked)) void __chkstk() {
	__asm__("jmp ___chkstk_ms");
}

} // extern "C"

#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
#pragma section(".CRT$XDA", long, read)
#pragma section(".CRT$XDZ", long, read)
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLC", long, read)
#pragma section(".CRT$XLZ", long, read)

extern "C" {

__declspec(allocate(".CRT$XIA")) InitFn __xi_a = nullptr;
__declspec(allocate(".CRT$XIZ")) InitFn __xi_z = nullptr;
__declspec(allocate(".CRT$XCA")) VoidFn __xc_a = nullptr;
__declspec(allocate(".CRT$XCZ")) VoidFn __xc_z = nullptr;
__declspec(allocate(".CRT$XDA")) VoidFn __xd_a = nullptr;
__declspec(allocate(".CRT$XDZ")) VoidFn __xd_z = nullptr;
__declspec(allocate(".CRT$XLA")) TLSCallbackFn __xl_a = nullptr;
__declspec(allocate(".CRT$XLZ")) TLSCallbackFn __xl_z = nullptr;

#pragma data_seg(".tls")
char _tls_start = 0;
#pragma data_seg(".tls$ZZZ")
char _tls_end = 0;
#pragma data_seg()

// IMAGE_TLS_DIRECTORY64, found by linker as _tls_used
struct TLSDirectory {
	Size start;
	Size end;
	Size index;
	Size callbacks;
	unsigned int zeroFill;
	unsigned int characteristics;
};
extern const TLSDirectory _tls_used = {
	reinterpret_cast<Size>(&_tls_start), reinterpret_cast<Size>(&_tls_end), reinterpret_cast<Size>(&_tls_index),
	reinterpret_cast<Size>(&__xl_a + 1), 0, 0};

thread_local bool __tls_guard = false;

static void RunThreadInitializers() {
	if (__tls_guard) {
		return;
	}
	__tls_guard = true;
	for (VoidFn* initializer = &__xd_a + 1; initializer < &__xd_z; ++initializer) {
		if (*initializer) {
			(*initializer)();
		}
	}
}

void __dyn_tls_on_demand_init() {
	RunThreadInitializers();
}

void __stdcall __dyn_tls_init(void*, unsigned long reason, void*) {
	if (reason == THREAD_ATTACH) {
		RunThreadInitializers();
	}
}
__declspec(allocate(".CRT$XLC")) TLSCallbackFn __dyn_tls_init_callback = __dyn_tls_init;

static void SeedCookie() {
	for (int attempt = 0; attempt < COOKIE_SEED_ATTEMPTS; ++attempt) {
		Size sample = 0;
		const long status = BCryptGenRandom(nullptr, reinterpret_cast<unsigned char*>(&sample),
											static_cast<unsigned long>(sizeof(sample)), BCRYPT_USE_SYSTEM_PREFERRED_RNG_FLAG);
		if (status < 0) {
			__fastfail(FAIL_STACK_COOKIE);
		}
		sample &= COOKIE_MASK;
		if (sample != 0 && sample != DEFAULT_COOKIE) {
			__security_cookie = sample;
			return;
		}
	}
	__fastfail(FAIL_STACK_COOKIE);
}

int __stdcall _DllMainCRTStartup(void* instance, unsigned long reason, void* reserved) {
	if (reason == PROCESS_ATTACH) {
		SeedCookie();
		for (InitFn* initializer = &__xi_a + 1; initializer < &__xi_z; ++initializer) {
			if (*initializer && (*initializer)() != 0) {
				return 0;
			}
		}
		for (VoidFn* constructor = &__xc_a + 1; constructor < &__xc_z; ++constructor) {
			if (*constructor) {
				(*constructor)();
			}
		}
		RunThreadInitializers();
	}
	const int result = DllMain(instance, reason, reserved);
	if (reason == PROCESS_DETACH) {
		while (_exitHandlerCount > 0) {
			_exitHandlers[--_exitHandlerCount]();
		}
	}
	return result;
}

} // extern "C"

namespace std {
	struct nothrow_t {
		explicit nothrow_t() = default;
	};
	extern const nothrow_t nothrow;
	const nothrow_t nothrow{};
} // namespace std

void* operator new(Size size, const std::nothrow_t&) noexcept {
	return malloc(size ? size : 1);
}

void* operator new[](Size size, const std::nothrow_t&) noexcept {
	return malloc(size ? size : 1);
}

void operator delete(void* block, Size) noexcept {
	free(block);
}

void operator delete[](void* block) noexcept {
	free(block);
}
