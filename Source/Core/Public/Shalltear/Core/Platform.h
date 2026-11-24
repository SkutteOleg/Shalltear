#pragma once

#if defined(_WIN32)
#define SLT_PLATFORM_WINDOWS
#elif defined(__EMSCRIPTEN__)
#define SLT_PLATFORM_WEB
#elif defined(__ANDROID__)
#define SLT_PLATFORM_ANDROID
#error "Shalltear: Android support isn't yet implemented"
#elif defined(__linux__)
#define SLT_PLATFORM_LINUX
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define SLT_PLATFORM_IOS
#error "Shalltear: iOS support isn't yet implemented"
#else
#define SLT_PLATFORM_MACOS
#error "Shalltear: macOS support isn't yet implemented"
#endif
#else
#error "Shalltear: unsupported platform"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define SLT_CPU_X64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SLT_CPU_ARM64
#elif defined(__wasm32__)
#define SLT_CPU_WASM32
#else
#error "Shalltear: unsupported CPU architecture (x64, arm64, and wasm32 only)"
#endif

// clang defines _MSC_VER (wtf), test __clang__ first
#if defined(__clang__)
#define SLT_COMPILER_CLANG
#define SLT_DEBUG_BREAK() __builtin_debugtrap()
#else
#error "Shalltear: unsupported compiler (Clang required)"
#endif
