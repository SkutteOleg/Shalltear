// startup and CRT symbols expected by NativeAOT's Windows x64 objects:
// - default (x86_64-pc-windows-msvc, freestanding): standalone /NODEFAULTLIB GameScripts.dll shim
// - SLT_AOT_STATIC_BRIDGE (x86_64-w64-mingw32): static-player bridge into MinGW's CRT and libc++

#include "Win64GS.inl"

using InitFn = int (*)();
using VoidFn = void (*)();
using TLSCallbackFn = void(__stdcall*)(void*, unsigned long, void*);

constexpr unsigned long PROCESS_ATTACH = 1;
constexpr unsigned long THREAD_ATTACH = 2;

#if defined(SLT_AOT_STATIC_BRIDGE)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" {

int __isa_available = 0;
unsigned int __favor = 0;
int _Avx2WmemEnabled = 0;
__thread unsigned char __tls_guard = 0;
__attribute__((weak)) int _fltused = 0;
extern const unsigned char _msvcStdNothrow asm("?nothrow@std@@3Unothrow_t@1@B");
const unsigned char _msvcStdNothrow = 0;

__attribute__((section(".CRT$XDA"), weak, used)) extern const VoidFn __xd_a = nullptr;
__attribute__((section(".CRT$XDZ"), weak, used)) extern const VoidFn __xd_z = nullptr;

void __dyn_tls_on_demand_init();
void __stdcall __dyn_tls_init(void*, unsigned long reason, void*);

} // extern "C"

namespace {

	std::atomic<int> _bridgeInitState{0};

	__attribute__((no_stack_protector, target("xsave"))) unsigned long long ReadXcr0() noexcept {
		unsigned int lo = 0;
		unsigned int hi = 0;
		__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
		return (static_cast<unsigned long long>(hi) << 32) | static_cast<unsigned long long>(lo);
	}

	__attribute__((no_stack_protector)) void QueryCpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4]) noexcept {
		__asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(leaf), "c"(subleaf));
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
				if ((xcr0 & 0x6ull) == 0x6ull) {
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
							if ((xcr0 & 0xE6ull) == 0xE6ull && hasAvx512F && hasAvx512Dq && hasAvx512Bw && hasAvx512Vl) {
								isa = 6;
							}
						}
					}
				}
			}
		}

		__isa_available = isa;
		_Avx2WmemEnabled = avx2Mem;
		__favor = 2u;
	}

	__attribute__((no_stack_protector)) void InitStaticBridge() noexcept {
		if (_bridgeInitState.load(std::memory_order_acquire) == 2) {
			return;
		}
		int expected = 0;
		if (_bridgeInitState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
			SeedSecurityCookie();
			DetectCPUIsa();
			_bridgeInitState.store(2, std::memory_order_release);
			return;
		}
		while (_bridgeInitState.load(std::memory_order_acquire) != 2) {
			__asm__ volatile("pause");
		}
	}

	__attribute__((no_stack_protector)) void __stdcall OnEarlyTLSCallback(void*, unsigned long reason, void*) {
		if (reason == PROCESS_ATTACH) {
			InitStaticBridge();
		}
	}

	__attribute__((no_stack_protector)) int OnEarlyCRTInit() {
		InitStaticBridge();
		__dyn_tls_on_demand_init();
		return 0;
	}

	__attribute__((section(".CRT$XLB"), used)) const TLSCallbackFn _earlyTLSCallback = &OnEarlyTLSCallback;
	__attribute__((section(".CRT$XIAA"), used)) const InitFn _earlyCRTInit = &OnEarlyCRTInit;

} // anonymous namespace

extern "C" {

void __dyn_tls_on_demand_init() {
	if (__tls_guard != 0) {
		return;
	}
	__tls_guard = 1;
	std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(&__xd_a) + sizeof(VoidFn);
	const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(&__xd_z);
	__asm__ volatile("" : "+r"(cur));
	for (; cur < end; cur += sizeof(VoidFn)) {
		const VoidFn fn = *reinterpret_cast<const VoidFn*>(cur);
		if (fn != nullptr) {
			fn();
		}
	}
}

void __stdcall __dyn_tls_init(void*, unsigned long reason, void*) {
	if (reason == THREAD_ATTACH) {
		InitStaticBridge();
		__dyn_tls_on_demand_init();
	}
}

__attribute__((section(".CRT$XLC"), used)) extern const TLSCallbackFn __dyn_tls_init_callback = &__dyn_tls_init;

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

#else

constexpr unsigned long PROCESS_DETACH = 0;
constexpr int EXIT_CAPACITY = 512;

extern "C" {

void* malloc(Size);
void free(void*);
int __stdcall DllMain(void*, unsigned long, void*);

int _fltused = 0x9875;
unsigned long _tls_index = 0;

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

int __stdcall _DllMainCRTStartup(void* instance, unsigned long reason, void* reserved) {
	if (reason == PROCESS_ATTACH) {
		SeedSecurityCookie();
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

#endif
