using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Jukebox : Behavior {
	public AudioResource? resource;
	public AudioConcurrency? limit;
	public AudioMixerGroup? group;
	public AudioSource? source;
	public List<AudioMixerSnapshot> snapshots = new();
}

static class AudioTests {
	const int RUN_FRAMES = 0;
	const string TONE = "project://Assets/Tone.wav";
	const string MIXER = "project://Assets/Game.mixer";
	const string LIMIT = "project://Assets/Limit.concurrency";

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static void Frames(int count) {
		BindingProbe.Drive(RUN_FRAMES, count, 0);
	}

	static T Stored<T>(string path)
		where T : Asset {
		T? found = ObjectCache.Find<T>(BindingProbe.FindAsset(path));
		if (Gone(found)) {
			throw new ConformanceFailure("no " + typeof(T).Name + " at " + path);
		}
		return found!;
	}

	static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	static void Retire(SceneObject used) {
		used.SetActive(false);
		used.Destroy();
	}

	[ConformanceTest]
	internal static void SourcePropertiesRoundTrip() {
		SceneObject speaker = SceneSystem.CreateObject("Speaker");
		AudioSource source = speaker.AddComponent<AudioSource>();
		Check.Same(source, speaker.GetComponent<AudioSource>());
		Check.True(Gone(source.Resource) && Gone(source.OutputAudioMixerGroup) && Gone(source.Concurrency), "no resource, group or limit");
		Check.True(!source.Mute && source.PlayOnAwake && !source.Loop && !source.IsPlaying && !source.IsVirtual, "source defaults");
		Check.Equal(128, source.Priority);
		Check.Equal(1f, source.Volume);
		Check.Equal(1f, source.Pitch);
		Check.Equal(AudioRolloffMode.Logarithmic, source.RolloffMode);
		Check.Equal(AudioVelocityUpdateMode.Auto, source.VelocityUpdateMode);
		Check.True(!source.Voice.IsValid, "no voice before Play");

		AudioClip tone = Stored<AudioClip>(TONE);
		AudioMixer mixer = Stored<AudioMixer>(MIXER);
		List<AudioMixerGroup> groups = new();
		Check.Equal(1, mixer.FindMatchingGroups("Music", groups));
		AudioConcurrency limit = Stored<AudioConcurrency>(LIMIT);
		source.Resource = tone;
		source.OutputAudioMixerGroup = groups[0];
		source.Concurrency = limit;
		source.Mute = true;
		source.PlayOnAwake = false;
		source.Loop = true;
		source.Priority = 32;
		source.Volume = 0.5f;
		source.Pitch = 1.5f;
		source.StereoPan = -0.25f;
		source.SpatialBlend = 0.75f;
		source.DopplerLevel = 2f;
		source.Spread = 90f;
		source.MinDistance = 2f;
		source.MaxDistance = 40f;
		source.RolloffMode = AudioRolloffMode.Linear;
		source.Occlusion = 0.125f;
		source.Spatialize = true;
		source.SpatializePostEffects = true;
		source.BypassEffects = true;
		source.BypassListenerEffects = true;
		source.BypassReverbZones = true;
		source.ReverbZoneMix = 0.5f;
		source.VelocityUpdateMode = AudioVelocityUpdateMode.Dynamic;
		Check.Same<AudioResource>(tone, source.Resource);
		Check.Same(groups[0], source.OutputAudioMixerGroup);
		Check.Same(limit, source.Concurrency);
		Check.True(source.Mute && !source.PlayOnAwake && source.Loop, "switches");
		Check.Equal(32, source.Priority);
		Check.Equal(0.5f, source.Volume);
		Check.Equal(1.5f, source.Pitch);
		Check.Equal(-0.25f, source.StereoPan);
		Check.Equal(0.75f, source.SpatialBlend);
		Check.Equal(2f, source.DopplerLevel);
		Check.Equal(90f, source.Spread);
		Check.Equal(2f, source.MinDistance);
		Check.Equal(40f, source.MaxDistance);
		Check.Equal(AudioRolloffMode.Linear, source.RolloffMode);
		Check.Equal(0.125f, source.Occlusion);
		Check.True(source.Spatialize && source.SpatializePostEffects && source.BypassEffects && source.BypassListenerEffects && source.BypassReverbZones,
				   "routing switches");
		Check.Equal(0.5f, source.ReverbZoneMix);
		Check.Equal(AudioVelocityUpdateMode.Dynamic, source.VelocityUpdateMode);
		source.Resource = null;
		source.OutputAudioMixerGroup = null;
		source.Concurrency = null;
		Check.True(Gone(source.Resource) && Gone(source.OutputAudioMixerGroup) && Gone(source.Concurrency), "references cleared");
		Retire(speaker);
	}

	[ConformanceTest]
	internal static void ClipDescribesItsSamples() {
		AudioClip tone = Stored<AudioClip>(TONE);
		Check.True(tone is AudioResource, "a clip is a resource");
		Check.True(tone.Frequency >= 8000u, "sample rate: " + tone.Frequency);
		Check.Equal(1u, tone.Channels);
		Check.True(MathF.Abs(tone.Length - 0.5f) < 0.02f, "1/2 second: " + tone.Length);
		Check.True(MathF.Abs(tone.Samples / (float)tone.Frequency - tone.Length) < 0.001f, "samples match the length: " + tone.Samples);
		Check.Equal(AudioClipLoadType.DecompressOnLoad, tone.LoadType);
		Check.Equal(AudioCompressionFormat.Opus, tone.CompressionFormat);
		Check.Same<AudioResource>(tone, ObjectCache.Find<AudioResource>(BindingProbe.FindAsset(TONE)));
	}

	[ConformanceTest]
	internal static void SourcePlaysAndStops() {
		SceneObject speaker = SceneSystem.CreateObject("Player");
		AudioSource source = speaker.AddComponent<AudioSource>();
		AudioClip tone = Stored<AudioClip>(TONE);
		ulong eighth = tone.Frequency / 8u;
		source.Resource = tone;
		source.Loop = true;

		source.Play();
		Frames(1);
		AudioVoice voice = source.Voice;
		Check.True(source.IsPlaying && voice.IsValid && Audio.IsPlaying(voice), "playing");
		Check.Equal(voice, source.Voice);
		ulong slot = 7UL;
		ulong generation = 1UL << 32;
		Check.True(!Unsafe.As<ulong, AudioVoice>(ref slot).IsValid && Unsafe.As<ulong, AudioVoice>(ref generation).IsValid, "a voice is valid by its generation");
		source.Pause();
		Frames(1);
		Check.True(!source.IsPlaying && !Audio.IsPlaying(voice), "a paused voice is not playing");
		source.TimeSamples = eighth;
		Frames(1);
		Check.Equal(eighth, source.TimeSamples);
		Check.Equal(eighth, Audio.GetTimeSamples(voice));
		Check.Equal(0.125f, source.Time);
		source.Time = 0.25f;
		Frames(1);
		Check.Equal(eighth * 2UL, source.TimeSamples);
		source.UnPause();
		Frames(1);
		Check.True(source.IsPlaying, "playing again");
		source.Stop();
		Frames(1);
		Check.True(!source.IsPlaying && !Audio.IsPlaying(voice), "stopped");

		source.Play();
		AudioVoice second = source.Voice;
		Check.True(second != voice && !second.Equals(voice), "a new voice per Play");
		Audio.Pause(second);
		Audio.SetTimeSamples(second, eighth);
		Audio.SetVolume(second, 0.25f);
		Audio.SetPitch(second, 2f);
		Audio.SetStereoPan(second, 0.5f);
		Audio.SetLoop(second, false);
		Audio.SetReverbZoneMix(second, 0.5f);
		Audio.SetPosition(second, new Vec3(1f, 2f, 3f), Vec3.Zero);
		Audio.SetPriority(second, 16);
		Audio.SetBypassEffects(second, true);
		Audio.SetBypassListenerEffects(second, true);
		Audio.SetEffectParameter(second, 0, 0, 1f);
		Audio.SetGeneratorParameter(second, 0, 1f);
		Audio.SetLayerVolume(second, 0, 1f);
		Audio.SwitchToClip(second, "None");
		Frames(1);
		Check.Equal(eighth, Audio.GetTimeSamples(second));
		Check.True(!Audio.IsPlaying(second) && !Audio.IsVirtual(second), "paused voice is held");
		Audio.UnPause(second);
		Frames(1);
		Check.True(Audio.IsPlaying(second) && source.IsPlaying, "voice playing again");
		Audio.Stop(second);
		Frames(1);
		Check.True(!Audio.IsPlaying(second) && !source.IsPlaying, "voice stopped");

		AudioTime now = Audio.Time;
		source.PlayScheduled(new AudioTime(now.frames + 48000UL * 600UL));
		AudioVoice scheduled = source.Voice;
		Check.True(scheduled.IsValid && scheduled != second, "a scheduled Play has its voice");
		source.SetScheduledStartTime(new AudioTime(now.frames + 48000UL * 700UL));
		source.SetScheduledEndTime(new AudioTime(now.frames + 48000UL * 800UL));
		Audio.SetScheduledStartTime(scheduled, new AudioTime(now.frames + 48000UL * 900UL));
		Audio.SetScheduledEndTime(scheduled, new AudioTime(now.frames + 48000UL * 1000UL));
		Frames(1);
		Check.Equal(0UL, source.TimeSamples);
		source.Stop();
		source.PlayDelayed(600f);
		Check.True(source.Voice.IsValid && source.Voice != scheduled, "a delayed Play has its voice");
		source.Stop();
		Frames(1);
		Check.True(!source.IsPlaying, "nothing left playing");
		source.PlayOneShot(tone);
		source.PlayOneShot(tone, 0.5f);
		Frames(1);
		Check.True(source.IsPlaying, "one shots count as playing");
		source.Stop();
		Retire(speaker);
		Frames(2);
	}

	[ConformanceTest]
	internal static void MixerParametersAndSnapshots() {
		AudioMixer mixer = Stored<AudioMixer>(MIXER);
		AudioMixerSnapshot? explore = mixer.FindSnapshot("Explore");
		AudioMixerSnapshot? combat = mixer.FindSnapshot("Combat");
		Check.True(!Gone(explore) && !Gone(combat) && Gone(mixer.FindSnapshot("Missing")), "snapshots by name");
		Check.Same(explore, mixer.FindSnapshot("Explore"));
		Check.Same(mixer, combat!.AudioMixer);
		Check.Equal("Combat", combat.SnapshotId);

		List<AudioMixerGroup> groups = new() { null! };
		Check.Equal(1, mixer.FindMatchingGroups("Music", groups));
		Check.Equal("Music", groups[0].GroupId);
		Check.Same(mixer, groups[0].AudioMixer);
		Check.Equal(4, mixer.FindMatchingGroups("", groups));
		Check.Equal(0, mixer.FindMatchingGroups("Missing", groups));
		Check.Equal(0, groups.Count);

		Check.True(mixer.GetFloat("MusicVolume", out float volume), "exposed parameter read");
		Check.Equal(-6f, volume);
		Check.True(mixer.SetFloat("MusicVolume", -3f) && mixer.GetFloat("MusicVolume", out volume), "exposed parameter set");
		Check.Equal(-3f, volume);
		Check.True(combat.TransitionTo(0f) && mixer.GetFloat("MusicVolume", out volume), "a set parameter stays out of a transition");
		Check.Equal(-3f, volume);
		Check.True(mixer.ClearFloat("MusicVolume") && mixer.GetFloat("MusicVolume", out volume), "exposed parameter cleared");
		Check.Equal(-20f, volume);
		Check.True(!mixer.SetFloat("Missing", 1f) && !mixer.GetFloat("Missing", out volume) && !mixer.ClearFloat("Missing"), "unknown parameter");
		Check.Equal(0f, volume);

		Check.True(mixer.TransitionToSnapshot("Explore", 0f) && mixer.GetFloat("MusicVolume", out volume), "transition by id");
		Check.Equal(-6f, volume);
		Check.True(!mixer.TransitionToSnapshot("Missing", 0f), "unknown snapshot id");
		Check.True(mixer.TransitionToSnapshots([explore!, combat], [0.5f, 0.5f], 0f) && mixer.GetFloat("MusicVolume", out volume), "weighted transition");
		Check.True(volume < -6f && volume > -20f, "a blend lies between its snapshots: " + volume);
		Check.Equal("AudioMixer.TransitionToSnapshots got 2 snapshot(s) + 1 weight(s). Pass 1 weight per snapshot",
					Refusal(() => mixer.TransitionToSnapshots([explore!, combat], [1f], 0f)));
		Check.True(explore!.TransitionTo(0f), "back to the start snapshot");
	}

	[ConformanceTest]
	internal static void ConcurrencyRoundTrips() {
		AudioConcurrency limit = Stored<AudioConcurrency>(LIMIT);
		Check.Equal(16, limit.MaxCount);
		Check.True(!limit.LimitToOwner, "limit over the scene");
		Check.Equal(AudioConcurrencyRule.StopFarthestThenOldest, limit.ResolutionRule);
		limit.MaxCount = 2;
		limit.LimitToOwner = true;
		limit.ResolutionRule = AudioConcurrencyRule.StopQuietest;
		limit.RetriggerTime = 0.25f;
		limit.VoiceStealReleaseTime = 0.125f;
		Check.Equal(2, limit.MaxCount);
		Check.True(limit.LimitToOwner, "limit per owner");
		Check.Equal(AudioConcurrencyRule.StopQuietest, limit.ResolutionRule);
		Check.Equal(0.25f, limit.RetriggerTime);
		Check.Equal(0.125f, limit.VoiceStealReleaseTime);
		limit.MaxCount = 16;
		limit.LimitToOwner = false;
		limit.ResolutionRule = AudioConcurrencyRule.StopFarthestThenOldest;
		limit.RetriggerTime = 0f;
		limit.VoiceStealReleaseTime = 0f;
	}

	[ConformanceTest]
	internal static void ListenerFiltersAndZonesRoundTrip() {
		SceneObject ears = SceneSystem.CreateObject("Ears");
		AudioListener listener = ears.AddComponent<AudioListener>();
		AudioMixer mixer = Stored<AudioMixer>(MIXER);
		List<AudioMixerGroup> groups = new();
		mixer.FindMatchingGroups("SFX", groups);
		listener.OutputAudioMixerGroup = groups[0];
		listener.VelocityUpdateMode = AudioVelocityUpdateMode.Fixed;
		Check.Same(groups[0], listener.OutputAudioMixerGroup);
		Check.Equal(AudioVelocityUpdateMode.Fixed, listener.VelocityUpdateMode);
		listener.OutputAudioMixerGroup = null;

		AudioLowPassFilter low = ears.AddComponent<AudioLowPassFilter>();
		AudioHighPassFilter high = ears.AddComponent<AudioHighPassFilter>();
		AudioEchoFilter echo = ears.AddComponent<AudioEchoFilter>();
		AudioDistortionFilter distortion = ears.AddComponent<AudioDistortionFilter>();
		AudioReverbFilter reverb = ears.AddComponent<AudioReverbFilter>();
		AudioChorusFilter chorus = ears.AddComponent<AudioChorusFilter>();
		Check.Same<AudioFilter>(low, ears.GetComponent<AudioFilter>());
		List<AudioFilter> filters = new();
		ears.GetComponents(filters);
		Check.Equal(6, filters.Count);
		Check.True(filters[1] is AudioHighPassFilter && filters[2] is AudioEchoFilter && filters[3] is AudioDistortionFilter && filters[4] is AudioReverbFilter &&
					   filters[5] is AudioChorusFilter,
				   "most derived filter class");
		Check.Equal("SceneObject.AddComponent can't add AudioFilter (abstract)", Refusal(() => ears.AddComponent<AudioFilter>()));

		low.CutoffFrequency = 800f;
		low.LowpassResonanceQ = 2f;
		high.CutoffFrequency = 300f;
		high.HighpassResonanceQ = 3f;
		echo.Delay = 250f;
		echo.DecayRatio = 0.25f;
		echo.WetMix = 0.75f;
		echo.DryMix = 0.5f;
		distortion.DistortionLevel = 0.625f;
		chorus.DryMix = 0.25f;
		chorus.WetMix1 = 0.125f;
		chorus.WetMix2 = 0.375f;
		chorus.WetMix3 = 0.625f;
		chorus.Delay = 30f;
		chorus.Rate = 1.5f;
		chorus.Depth = 0.0625f;
		chorus.Feedback = 0.5f;
		Check.Equal(800f, low.CutoffFrequency);
		Check.Equal(2f, low.LowpassResonanceQ);
		Check.Equal(300f, high.CutoffFrequency);
		Check.Equal(3f, high.HighpassResonanceQ);
		Check.Equal(250f, echo.Delay);
		Check.Equal(0.25f, echo.DecayRatio);
		Check.Equal(0.75f, echo.WetMix);
		Check.Equal(0.5f, echo.DryMix);
		Check.Equal(0.625f, distortion.DistortionLevel);
		Check.Equal(0.25f, chorus.DryMix);
		Check.Equal(0.125f, chorus.WetMix1);
		Check.Equal(0.375f, chorus.WetMix2);
		Check.Equal(0.625f, chorus.WetMix3);
		Check.Equal(30f, chorus.Delay);
		Check.Equal(1.5f, chorus.Rate);
		Check.Equal(0.0625f, chorus.Depth);
		Check.Equal(0.5f, chorus.Feedback);

		reverb.ReverbPreset = AudioReverbPreset.User;
		reverb.DryLevel = -100f;
		reverb.Room = -200f;
		reverb.RoomHF = -300f;
		reverb.RoomLF = -400f;
		reverb.DecayTime = 2.5f;
		reverb.DecayHFRatio = 0.75f;
		reverb.ReflectionsLevel = -500f;
		reverb.ReflectionsDelay = 0.0625f;
		reverb.ReverbLevel = -600f;
		reverb.ReverbDelay = 0.03125f;
		reverb.HFReference = 4000f;
		reverb.LFReference = 200f;
		reverb.Diffusion = 80f;
		reverb.Density = 60f;
		Check.Equal(AudioReverbPreset.User, reverb.ReverbPreset);
		Check.Equal(-100f, reverb.DryLevel);
		Check.Equal(-200f, reverb.Room);
		Check.Equal(-300f, reverb.RoomHF);
		Check.Equal(-400f, reverb.RoomLF);
		Check.Equal(2.5f, reverb.DecayTime);
		Check.Equal(0.75f, reverb.DecayHFRatio);
		Check.Equal(-500f, reverb.ReflectionsLevel);
		Check.Equal(0.0625f, reverb.ReflectionsDelay);
		Check.Equal(-600f, reverb.ReverbLevel);
		Check.Equal(0.03125f, reverb.ReverbDelay);
		Check.Equal(4000f, reverb.HFReference);
		Check.Equal(200f, reverb.LFReference);
		Check.Equal(80f, reverb.Diffusion);
		Check.Equal(60f, reverb.Density);

		AudioReverbZone zone = ears.AddComponent<AudioReverbZone>();
		zone.MinDistance = 3f;
		zone.MaxDistance = 12f;
		zone.ReverbPreset = AudioReverbPreset.Cave;
		zone.Room = -150f;
		zone.RoomHF = -250f;
		zone.RoomLF = -350f;
		zone.DecayTime = 3.5f;
		zone.DecayHFRatio = 0.5f;
		zone.Reflections = -450f;
		zone.ReflectionsDelay = 0.125f;
		zone.Reverb = -550f;
		zone.ReverbDelay = 0.0625f;
		zone.HFReference = 5000f;
		zone.LFReference = 250f;
		zone.Diffusion = 70f;
		zone.Density = 50f;
		Check.Equal(3f, zone.MinDistance);
		Check.Equal(12f, zone.MaxDistance);
		Check.Equal(AudioReverbPreset.Cave, zone.ReverbPreset);
		Check.Equal(-150f, zone.Room);
		Check.Equal(-250f, zone.RoomHF);
		Check.Equal(-350f, zone.RoomLF);
		Check.Equal(3.5f, zone.DecayTime);
		Check.Equal(0.5f, zone.DecayHFRatio);
		Check.Equal(-450f, zone.Reflections);
		Check.Equal(0.125f, zone.ReflectionsDelay);
		Check.Equal(-550f, zone.Reverb);
		Check.Equal(0.0625f, zone.ReverbDelay);
		Check.Equal(5000f, zone.HFReference);
		Check.Equal(250f, zone.LFReference);
		Check.Equal(70f, zone.Diffusion);
		Check.Equal(50f, zone.Density);
		Retire(ears);
		Frames(1);
	}

	[ConformanceTest]
	internal static void ClockAndVolumeAreRead() {
		Check.True(Audio.OutputSampleRate >= 8000u, "output sample rate: " + Audio.OutputSampleRate);
		AudioTime first = Audio.Time;
		Frames(1);
		Check.True(Audio.Time.frames >= first.frames, "the clock never runs back");
		object boxed = new AudioTime(first.frames);
		Check.True(first == new AudioTime(first.frames) && first.Equals(boxed) && first != new AudioTime(first.frames + 1UL), "times compare by frames");
		Check.Equal(first.frames.GetHashCode(), first.GetHashCode());

		float volume = Audio.GlobalVolume;
		Check.Equal(1f, volume);
		Check.True(Audio.SetGlobalVolume(0.5f), "global volume set");
		Check.Equal(0.5f, Audio.GlobalVolume);
		Check.True(Audio.SetGlobalVolume(volume), "global volume put back");
	}

	[ConformanceTest]
	internal static void MusicClockCountsBeats() {
		AudioMusicClock clock = new() { origin = new AudioTime(48000UL), beatsPerMinute = 120.0, beatsPerBar = 4, sampleRate = 48000 };
		Check.True(clock.HasTempo && !default(AudioMusicClock).HasTempo, "tempo");
		Check.Equal(0L, clock.GetBeat(new AudioTime(48000UL)));
		Check.Equal(2L, clock.GetBeat(new AudioTime(96000UL)));
		Check.Equal(1L, clock.GetBar(new AudioTime(48000UL * 3UL + 1UL)));
		Check.Equal(new AudioTime(48000UL + 24000UL * 5UL), clock.GetBeatTime(5L));
		Check.Equal(new AudioTime(72000UL), clock.NextBoundary(AudioQuantization.NextBeat, new AudioTime(48001UL)));
		Check.Equal(new AudioTime(48000UL * 3UL), clock.NextBoundary(AudioQuantization.NextBar, new AudioTime(48001UL)));
		Check.Equal(new AudioTime(50000UL), clock.NextBoundary(AudioQuantization.Immediate, new AudioTime(50000UL)));
		Check.True(!Audio.GetMusicClock(default).HasTempo, "no clock without a voice");
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldAudioTypes() {
		SceneObject box = SceneSystem.CreateObject("Jukebox");
		AudioSource source = box.AddComponent<AudioSource>();
		Jukebox jukebox = box.AddComponent<Jukebox>();
		AudioMixer mixer = Stored<AudioMixer>(MIXER);
		AudioConcurrency limit = Stored<AudioConcurrency>(LIMIT);
		List<AudioMixerGroup> groups = new();
		mixer.FindMatchingGroups("Music", groups);
		jukebox.resource = Stored<AudioClip>(TONE);
		jukebox.limit = limit;
		jukebox.group = groups[0];
		jukebox.source = source;
		jukebox.snapshots.Add(mixer.FindSnapshot("Combat")!);

		SceneObject copy = SceneSystem.Instantiate(box);
		Jukebox copied = copy.GetComponent<Jukebox>()!;
		Check.Same(limit, copied.limit);
		Check.Same(groups[0], copied.group);
		Check.Same(copy.GetComponent<AudioSource>(), copied.source);
		Check.Equal(1, copied.snapshots.Count);
		Check.Same(mixer.FindSnapshot("Combat"), copied.snapshots[0]);
		Check.True(jukebox.resource is AudioClip, "a resource field takes a clip");
		Retire(box);
		Retire(copy);
		Frames(1);
	}

	[ConformanceTest]
	internal static void RepeatedAudioCallsAllocateNothing() {
		SceneObject speaker = SceneSystem.CreateObject("Loop");
		AudioSource source = speaker.AddComponent<AudioSource>();
		source.Resource = Stored<AudioClip>(TONE);
		source.Loop = true;
		source.Play();
		Frames(1);
		AudioMixer mixer = Stored<AudioMixer>(MIXER);
		AudioVoice voice = source.Voice;
		mixer.SetFloat("MusicVolume", -9f);
		mixer.GetFloat("MusicVolume", out float warm);
		Check.Equal(-9f, warm);

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			hits += mixer.SetFloat("MusicVolume", -9f) && mixer.GetFloat("MusicVolume", out float read) && read == -9f ? 1 : 0;
			source.Volume = 0.5f;
			source.Pitch = 1f;
			hits += source.Volume == 0.5f && source.Pitch == 1f && source.Loop ? 1 : 0;
			hits += source.IsPlaying && source.Voice == voice && Audio.IsPlaying(voice) ? 1 : 0;
			Audio.SetVolume(voice, 0.5f);
			Audio.SetPosition(voice, new Vec3(0f, 0f, 1f), Vec3.Zero);
			hits += Audio.Time.frames > 0UL || Audio.OutputSampleRate > 0u ? 1 : 0;
			hits += ReferenceEquals(source.Resource, speaker.GetComponent<AudioSource>()!.Resource) ? 1 : 0;
		}
		Frames(4);
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(5000, hits);
		Check.Equal(0L, allocated);
		mixer.ClearFloat("MusicVolume");
		source.Stop();
		Retire(speaker);
		Frames(1);
	}

	static AudioEffectDefinitionProxy? _registeredEffect;
	static AudioSpatializerProxy? _registeredSpatializer;

	static AudioEffectDefinitionProxy Halving(Action? released) {
		AudioEffectDefinitionProxy definition = new();
		definition.SetCreateEffect(context => {
			AudioEffectProxy effect = new();
			effect.SetProcess((block, sidechain) => {
				for (int channel = 0; channel < block.channels; ++channel) {
					Span<float> samples = block.Channel(channel);
					for (int index = 0; index < samples.Length; ++index) {
						samples[index] *= 0.5f;
					}
				}
			});
			if (released != null) {
				effect.SetRelease(released);
			}
			return effect.Base;
		});
		return definition;
	}

	[ConformanceTest]
	internal static void EffectRunsOnAudioThread() {
		int main = Environment.CurrentManagedThreadId;
		int calls = 0;
		int offMain = 0;
		int released = 0;
		float side = -1f;
		long first = 0;
		long last = 0;
		using AudioEffectDefinitionProxy definition = new();
		definition.SetCreateEffect(context => {
			Check.Equal(64u, context.BlockFrames);
			AudioEffectProxy effect = new();
			effect.SetProcess((block, sidechain) => {
				if (calls++ == 1) {
					first = GC.GetAllocatedBytesForCurrentThread();
				}
				offMain += Environment.CurrentManagedThreadId != main ? 1 : 0;
				for (int channel = 0; channel < block.channels; ++channel) {
					Span<float> samples = block.Channel(channel);
					for (int index = 0; index < samples.Length; ++index) {
						samples[index] *= 0.5f;
					}
				}
				side = sidechain.channels == 0 ? 0f : sidechain.Channel(1)[63];
				last = GC.GetAllocatedBytesForCurrentThread();
			});
			effect.SetRelease(() => ++released);
			return effect.Base;
		});
		int kept = SlotCallbacks.Count;
		Check.Equal(0.5f, AudioProbe.RunEffect(definition.Base, 2, 64, 200, true));
		Check.Equal(200, calls);
		Check.Equal(200, offMain);
		Check.Equal(0.25f, side);
		Check.Equal(0L, last - first);
		Check.Equal(1, released);
		Check.Equal(0.5f, AudioProbe.RunEffect(definition.Base, 1, 64, 1, false));
		Check.Equal(0f, side);
		Check.Equal(2, released);
		Check.Equal(kept, SlotCallbacks.Count);
	}

	[ConformanceTest]
	internal static void GeneratorRunsOnAudioThread() {
		int main = Environment.CurrentManagedThreadId;
		int offMain = 0;
		ulong lastFrom = ulong.MaxValue;
		using AudioGeneratorInstanceProxy instance = new();
		instance.SetProcess((block, fromFrame) => {
			offMain += Environment.CurrentManagedThreadId != main ? 1 : 0;
			lastFrom = fromFrame;
			block.Channel(0).Fill(0.75f);
			return block.frames;
		});
		Check.Equal(0.75f, AudioProbe.RunGenerator(instance.Base, 48, 10));
		Check.Equal(10, offMain);
		Check.Equal(432UL, lastFrom);
	}

	[ConformanceTest]
	internal static void SpatializerRunsOnAudioThread() {
		int main = Environment.CurrentManagedThreadId;
		int offMain = 0;
		int destroyed = 0;
		using AudioSpatializerProxy spatializer = new();
		spatializer.SetCreateVoiceState(context => context.OutputChannels == 2 ? 7UL : 0UL);
		spatializer.SetDestroyVoiceState(state => destroyed += state == 7UL ? 1 : 0);
		spatializer.SetProcess((state, parameters, input, output) => {
			offMain += Environment.CurrentManagedThreadId != main && state == 7UL ? 1 : 0;
			Span<float> source = input.Channel(0);
			for (int channel = 0; channel < output.channels; ++channel) {
				Span<float> target = output.Channel(channel);
				for (int index = 0; index < target.Length; ++index) {
					target[index] = source[index] * parameters.distanceAttenuation;
				}
			}
		});
		Check.Equal(0.25f, AudioProbe.RunSpatializer(spatializer.Base, 0.25f, 32, 5));
		Check.Equal(5, offMain);
		Check.Equal(1, destroyed);
	}

	[ConformanceTest]
	internal static void EffectRunsWhileMainThreadCollects() {
		int released = 0;
		using AudioEffectDefinitionProxy definition = Halving(() => ++released);
		Check.True(AudioProbe.StartEffectLoop(definition.Base, 64), "loop started");
		for (int round = 0; round < 10; ++round) {
			byte[][] garbage = new byte[64][];
			for (int index = 0; index < garbage.Length; ++index) {
				garbage[index] = new byte[16384];
			}
			GC.Collect(2, GCCollectionMode.Forced, true);
		}
		ulong[] stats = new ulong[3];
		AudioProbe.StopEffectLoop(stats);
		Check.True(stats[0] > 0UL, "effect called while collecting");
		Check.Equal(1, released);
	}

	[ConformanceTest]
	internal static void RegistrationsHoldUntilUnload() {
		_registeredEffect = Halving(null);
		_registeredSpatializer = new AudioSpatializerProxy();
		Check.True(AudioEffects.Register("ScriptHalf", _registeredEffect.Base), "effect name free");
		Check.True(AudioSpatializers.Register("ScriptPan", _registeredSpatializer.Base), "spatializer name free");
		Check.True(!AudioEffects.Register("ScriptHalf", _registeredEffect.Base), "effect name taken");
	}
}
