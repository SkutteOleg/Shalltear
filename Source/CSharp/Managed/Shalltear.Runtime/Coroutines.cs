using System;
using System.Collections;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Wait yielded by a routine (reusable).</summary>
public sealed class CoroutineWait {
	internal readonly int _kind;
	internal readonly float _seconds;

	internal CoroutineWait(int kind, float seconds) {
		_kind = kind;
		_seconds = seconds;
	}
}

/// <summary>Routine started by Coroutines.Start.</summary>
public readonly struct Coroutine : IEquatable<Coroutine> {
	readonly Routine? _routine;
	readonly int _generation;

	internal Coroutine(Routine routine) {
		_routine = routine;
		_generation = routine._generation;
	}

	/// <summary>False once finished/stopped (or owner destroyed).</summary>
	public bool IsRunning => _routine != null && _routine._generation == _generation && !_routine._stopped && _routine._owner;

	/// <summary>Stop before its next step.</summary>
	public void Stop() {
		if (IsRunning) {
			_routine!.Stop();
		}
	}

	/// <inheritdoc/>
	public bool Equals(Coroutine other) {
		return ReferenceEquals(_routine, other._routine) && _generation == other._generation;
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is Coroutine value && Equals(value);
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return _generation;
	}

	/// <summary>Same routine.</summary>
	public static bool operator ==(Coroutine left, Coroutine right) {
		return left.Equals(right);
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(Coroutine left, Coroutine right) {
		return !left.Equals(right);
	}
}

/// <summary>IEnumerator routines stepped per frame. Yield null for next frame, a wait or another IEnumerator.</summary>
public static class Coroutines {
	/// <summary>Yield to resume on next fixed step (before OnFixedUpdate calls).</summary>
	public static CoroutineWait NextFixedUpdate { get; } = new(FrameAwaits.WAIT_FIXED, 0f);

	/// <summary>Run routine to its 1st yield. Stopped when owner is destroyed.</summary>
	public static Coroutine Start(EngineObject owner, IEnumerator routine) {
		ArgumentNullException.ThrowIfNull(owner);
		ArgumentNullException.ThrowIfNull(routine);
		if (!Bridge.IsMainThread) {
			Bridge.ThrowOffMainThread("Coroutines.Start");
		}
		Routine started = FrameAwaits.Start(owner, routine);
		Coroutine handle = new(started);
		FrameAwaits.Step(started);
		return handle;
	}

	/// <summary>Stop routines of owner.</summary>
	public static void StopAll(EngineObject owner) {
		ArgumentNullException.ThrowIfNull(owner);
		if (!Bridge.IsMainThread) {
			Bridge.ThrowOffMainThread("Coroutines.StopAll");
		}
		FrameAwaits.StopAll(owner);
	}

	/// <summary>Yield to resume after seconds of game time (scaled by TimeScale).</summary>
	public static CoroutineWait Delay(float seconds) {
		return MakeWait(FrameAwaits.WAIT_DELAY, seconds, "Coroutines.Delay");
	}

	/// <summary>Yield to resume after seconds of real time (not scaled).</summary>
	public static CoroutineWait DelayRealtime(float seconds) {
		return MakeWait(FrameAwaits.WAIT_DELAY_REALTIME, seconds, "Coroutines.DelayRealtime");
	}

	static CoroutineWait MakeWait(int kind, float seconds, string member) {
		if (float.IsNaN(seconds)) {
			throw new ArgumentOutOfRangeException(nameof(seconds), member + " got NaN seconds. Pass a number");
		}
		return new CoroutineWait(kind, seconds);
	}
}
