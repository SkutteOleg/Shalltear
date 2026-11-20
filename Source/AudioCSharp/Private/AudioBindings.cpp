#include "AudioBindings.h"

#include "BindingSupport.h"

#include "Shalltear/Core/Containers.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::AudioBindings {

	namespace {

		Vector<Handle<AudioMixerGroup>> _groups;
		Vector<Handle<AudioMixerSnapshot>> _snapshots;

		AudioMusicClock ToEngine(std::span<const SltAudioMusicClock> clock) {
			AudioMusicClock engine;
			if (!clock.empty()) {
				engine.origin = AudioTime{clock[0].origin};
				engine.beatsPerMinute = clock[0].beatsPerMinute;
				engine.beatsPerBar = clock[0].beatsPerBar;
				engine.sampleRate = clock[0].sampleRate;
			}
			return engine;
		}

	} // anonymous namespace

	i32 FindGroups(AudioMixer& mixer, std::string_view subPath, std::span<u64> ids) {
		_groups.clear();
		mixer.FindMatchingGroups(subPath, _groups);
		for (size_t index = 0; index < _groups.size() && index < ids.size(); ++index) {
			ids[index] = IdOf(_groups[index]);
		}
		return static_cast<i32>(_groups.size());
	}

	bool TransitionToWeights(AudioMixer& mixer, std::span<const u64> snapshots, std::span<const f32> weights, f32 timeToReach) {
		if (snapshots.size() != weights.size()) {
			throw std::runtime_error(
				std::format("AudioMixer.TransitionToSnapshots got {} snapshot(s) + {} weight(s). Pass 1 weight per snapshot", snapshots.size(), weights.size()));
		}
		_snapshots.clear();
		for (const u64 id : snapshots) {
			_snapshots.push_back(Handle<AudioMixerSnapshot>(ResolveObject<AudioMixerSnapshot>(id)));
		}
		return mixer.TransitionToSnapshots(std::span<const Handle<AudioMixerSnapshot>>(_snapshots), weights, timeToReach);
	}

	bool ReadFloat(const AudioMixer& mixer, std::string_view name, std::span<f32> value) {
		f32 read = 0.0f;
		const bool found = mixer.GetFloat(name, read);
		if (found && !value.empty()) {
			value[0] = read;
		}
		return found;
	}

	void ReadMusicClock(AudioVoice voice, std::span<SltAudioMusicClock> clock) {
		if (!clock.empty()) {
			const AudioMusicClock engine = Audio::GetMusicClock(voice);
			clock[0] = SltAudioMusicClock{engine.origin.frames, engine.beatsPerMinute, engine.beatsPerBar, engine.sampleRate};
		}
	}

	i64 ClockBeat(std::span<const SltAudioMusicClock> clock, AudioTime time) {
		return ToEngine(clock).GetBeat(time);
	}

	i64 ClockBar(std::span<const SltAudioMusicClock> clock, AudioTime time) {
		return ToEngine(clock).GetBar(time);
	}

	AudioTime ClockBeatTime(std::span<const SltAudioMusicClock> clock, i64 beat) {
		return ToEngine(clock).GetBeatTime(beat);
	}

	AudioTime ClockNextBoundary(std::span<const SltAudioMusicClock> clock, AudioQuantization boundary, AudioTime after) {
		return ToEngine(clock).NextBoundary(boundary, after);
	}

	void Release() {
		_groups = {};
		_snapshots = {};
	}

} // namespace Shalltear::CSharpImpl::AudioBindings
