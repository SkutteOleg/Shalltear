using Shalltear.Interop;

namespace Shalltear;

public static partial class Time {
	/// <summary>Await to resume on next frame (before OnUpdate calls).</summary>
	public static FrameAwaitable NextFrame() {
		return FrameAwaits.Await(FrameAwaits.WAIT_FRAME, 0f, "Time.NextFrame");
	}

	/// <summary>Await to resume after seconds of game time (scaled by TimeScale).</summary>
	public static FrameAwaitable Delay(float seconds) {
		return FrameAwaits.Await(FrameAwaits.WAIT_DELAY, seconds, "Time.Delay");
	}

	/// <summary>Await to resume after seconds of real time (not scaled).</summary>
	public static FrameAwaitable DelayRealtime(float seconds) {
		return FrameAwaits.Await(FrameAwaits.WAIT_DELAY_REALTIME, seconds, "Time.DelayRealtime");
	}

	/// <summary>Await to resume on next fixed step (before OnFixedUpdate calls).</summary>
	public static FrameAwaitable NextFixedUpdate() {
		return FrameAwaits.Await(FrameAwaits.WAIT_FIXED, 0f, "Time.NextFixedUpdate");
	}
}
