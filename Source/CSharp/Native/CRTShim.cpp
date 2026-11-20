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

extern "C" {

void* malloc(Size);
void free(void*);
int __stdcall DllMain(void*, unsigned long, void*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long*);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(Size*);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
// compiler-rt's stack probe (same contract)
void ___chkstk_ms();

Size __security_cookie = DEFAULT_COOKIE;
int _fltused = 0x9875;
unsigned long _tls_index = 0;

void __security_check_cookie(Size cookie) {
	if (cookie != __security_cookie) {
		__fastfail(FAIL_STACK_COOKIE);
	}
}

void __report_rangecheckfailure() {
	__fastfail(FAIL_RANGE_CHECK);
}

int __GSHandlerCheck(void*, void*, void*, void*) {
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
	Size time = 0;
	long long ticks = 0;
	GetSystemTimeAsFileTime(&time);
	QueryPerformanceCounter(&ticks);
	Size seed = reinterpret_cast<Size>(&seed) ^ time ^ GetCurrentProcessId() ^ (static_cast<Size>(GetCurrentThreadId()) << 32);
	seed ^= static_cast<Size>(ticks) * 0x9E3779B97F4A7C15ull;
	seed &= COOKIE_MASK;
	__security_cookie = seed == 0 || seed == DEFAULT_COOKIE ? DEFAULT_COOKIE + 1 : seed;
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
