#pragma once

#include "Generated/AudioBindings.generated.h"

#include "Shalltear/Core/Types.h"

#include "Shalltear/Audio.h"

#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::AudioBindings {

	// returned count may exceed ids.size()
	i32 FindGroups(AudioMixer& mixer, std::string_view subPath, std::span<u64> ids);
	bool TransitionToWeights(AudioMixer& mixer, std::span<const u64> snapshots, std::span<const f32> weights, f32 timeToReach);
	bool ReadFloat(const AudioMixer& mixer, std::string_view name, std::span<f32> value);

	void ReadMusicClock(AudioVoice voice, std::span<SltAudioMusicClock> clock);
	i64 ClockBeat(std::span<const SltAudioMusicClock> clock, AudioTime time);
	i64 ClockBar(std::span<const SltAudioMusicClock> clock, AudioTime time);
	AudioTime ClockBeatTime(std::span<const SltAudioMusicClock> clock, i64 beat);
	AudioTime ClockNextBoundary(std::span<const SltAudioMusicClock> clock, AudioQuantization boundary, AudioTime after);

	void Release();

} // namespace Shalltear::CSharpImpl::AudioBindings
