#pragma once

#include "AudioVoiceInternals.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Platform.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <thread>

struct OpusMSDecoder;

namespace Shalltear {

	namespace AudioImpl {

		namespace Realtime {

			inline u64 GetTicks() [[clang::nonblocking]] {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
				return Time::GetTicks();
#pragma clang diagnostic pop
			}

			inline std::thread::id GetThreadId() [[clang::nonblocking]] {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
				return std::this_thread::get_id();
#pragma clang diagnostic pop
			}

			struct DenormalsFlushed {
#if defined(SLT_CPU_X64)
				DenormalsFlushed() [[clang::nonblocking]] : _saved(__builtin_ia32_stmxcsr()) {
					__builtin_ia32_ldmxcsr(_saved | FLUSH_TO_ZERO | DENORMALS_ARE_ZERO);
				}
				~DenormalsFlushed() [[clang::nonblocking]] { __builtin_ia32_ldmxcsr(_saved); }
#else
				DenormalsFlushed() [[clang::nonblocking]] = default;
				~DenormalsFlushed() [[clang::nonblocking]] = default;
#endif
				DenormalsFlushed(const DenormalsFlushed&) = delete;
				DenormalsFlushed& operator=(const DenormalsFlushed&) = delete;

				static constexpr u32 FLUSH_TO_ZERO = 0x8000u;
				static constexpr u32 DENORMALS_ARE_ZERO = 0x0040u;
			private:
				u32 _saved = 0;
			};

			inline bool ReadAt(const FileReader& file, u64 offset, std::span<u8> out) [[clang::nonallocating]] {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
				return file.ReadAt(offset, out);
#pragma clang diagnostic pop
			}

			i32 OpusDecode(OpusMSDecoder* decoder, const u8* packet, u32 size, f32* out, u32 frames) [[clang::nonallocating]];

			bool OpusInit(OpusMSDecoder* decoder, u32 channels, u8 streams, u8 coupled, const u8* mapping) [[clang::nonallocating]];

		} // namespace Realtime

	} // namespace AudioImpl

} // namespace Shalltear
