using System;

namespace Shalltear;

public partial struct AudioMusicClock {
	/// <summary>False for a voice playing nothing with a tempo.</summary>
	public readonly bool HasTempo => beatsPerMinute > 0.0 && sampleRate != 0;

	/// <summary>Beat containing time (beat 0 at origin).</summary>
	public readonly long GetBeat(AudioTime time) {
		AudioMusicClock clock = this;
		return Audio.ClockBeat(new ReadOnlySpan<AudioMusicClock>(in clock), time);
	}

	/// <summary>Bar containing time.</summary>
	public readonly long GetBar(AudioTime time) {
		AudioMusicClock clock = this;
		return Audio.ClockBar(new ReadOnlySpan<AudioMusicClock>(in clock), time);
	}

	/// <summary>When beat starts.</summary>
	public readonly AudioTime GetBeatTime(long beat) {
		AudioMusicClock clock = this;
		return Audio.ClockBeatTime(new ReadOnlySpan<AudioMusicClock>(in clock), beat);
	}

	/// <summary>1st boundary of this kind after a time.</summary>
	public readonly AudioTime NextBoundary(AudioQuantization boundary, AudioTime after) {
		AudioMusicClock clock = this;
		return Audio.ClockNextBoundary(new ReadOnlySpan<AudioMusicClock>(in clock), boundary, after);
	}
}

public static unsafe partial class Audio {
	/// <summary>Tempo of a voice's music (interactive containers only).</summary>
	public static AudioMusicClock GetMusicClock(AudioVoice voice) {
		AudioMusicClock clock = default;
		ReadMusicClock(voice, new Span<AudioMusicClock>(ref clock));
		return clock;
	}
}
