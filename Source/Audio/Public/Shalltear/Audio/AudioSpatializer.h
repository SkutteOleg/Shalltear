#pragma once

#include "Shalltear/Audio/AudioAPI.h"

#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear {

	enum class AudioSpeaker : u8 {
		None = 0,
		Mono,
		FrontLeft,
		FrontRight,
		FrontCenter,
		LowFrequency,
		BackLeft,
		BackRight,
		BackCenter,
		SideLeft,
		SideRight,
	};

	inline constexpr u32 AUDIO_MAX_CHANNELS = 8;

	struct AudioSpatializerContext {
		u32 sampleRate = 0;
		u32 blockFrames = 0;
		u32 inputChannels = 0;
		AudioSpeaker inputSpeakers[AUDIO_MAX_CHANNELS]{};
		u32 outputChannels = 0;
		AudioSpeaker outputSpeakers[AUDIO_MAX_CHANNELS]{};
	};

	// planar, channel c frame i = data[c * stride + i]
	struct AudioBlock {
		union {
			f32* data = nullptr;
			u64 dataSlot;
		};
		u32 channels = 0;
		u32 frames = 0;
		u32 stride = 0;
	};

	struct AudioSpatialParams {
		// listener space, x right, y up, z ahead
		Vec3 position;
		// degrees
		f32 spread = 0.0f;
		f32 occlusion = 0.0f;
		f32 distanceAttenuation = 1.0f;
	};

	class IAudioSpatializer {
	public:
		virtual ~IAudioSpatializer() = default;

		virtual bool AppliesDistanceAttenuation() const { return false; }

		virtual void* CreateVoiceState(const AudioSpatializerContext& context) = 0;

		// after the render thread lets go
		virtual void DestroyVoiceState(void* state) = 0;

		virtual void Process(void* state, const AudioSpatialParams& params, const AudioBlock& in, AudioBlock& out) [[clang::nonblocking]] = 0;
	};

	namespace AudioSpatializers {

		SHALLTEAR_AUDIO_API bool Register(std::string_view name, IAudioSpatializer* spatializer, const void* owner);

		SHALLTEAR_AUDIO_API u32 UnregisterOwnedBy(const void* owner);

	} // namespace AudioSpatializers

} // namespace Shalltear
