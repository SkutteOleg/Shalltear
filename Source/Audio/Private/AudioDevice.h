#pragma once

#include "AudioVoiceInternals.h"

#include "Shalltear/Core/Types.h"

#include "AudioChannels.h"

namespace Shalltear {

	namespace AudioImpl {

		class AudioRenderer;

		struct AudioDeviceBackend;

		enum class AudioBackendKind : u8 {
			System = 0,
			Null,
		};

		struct AudioDeviceFormat {
			AudioBackendKind backend = AudioBackendKind::Null;
			u32 sampleRate = 0;
			u32 channels = 0;
			ChannelLayout layout;
			u32 periodFrames = 0;
			u32 periods = 0;
			u32 deviceSampleRate = 0;
			str name;
		};

		struct AudioDeviceEvents {
			u32 started = 0;
			u32 stopped = 0;
			u32 rerouted = 0;
			u32 interrupted = 0;
			u32 requested = 0;

			u32 Restarts() const { return started > requested ? started - requested : 0u; }
		};

		using AudioDevicePreparedFn = void (*)(void* user);

		class AudioDevice {
		public:
			AudioDevice() = default;
			~AudioDevice();
			AudioDevice(const AudioDevice&) = delete;
			AudioDevice& operator=(const AudioDevice&) = delete;

			// renderer is borrowed, must outlive the device
			bool Open(AudioBackendKind backend, u32 periodMilliseconds, AudioRenderer& renderer, AudioDevicePreparedFn prepared = nullptr,
					  void* user = nullptr);

			void Close();

			bool IsOpen() const { return _backend != nullptr; }
			bool IsCallbackActive() const;
			const AudioDeviceFormat& GetFormat() const { return _format; }

			AudioDeviceEvents GetEvents() const;

			void RefreshFormat();
		private:
			AudioDeviceBackend* _backend = nullptr;
			AudioDeviceFormat _format;
		};

	} // namespace AudioImpl

} // namespace Shalltear
