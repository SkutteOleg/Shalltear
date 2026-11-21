using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Awaiter : Behavior {
	internal static int resumed;

	protected override void OnEnable() {
		Wait();
	}

	async void Wait() {
		await Time.NextFrame();
		++resumed;
		await Time.Delay(0f);
		++resumed;
	}
}

static class AwaitProbe {
	static async void ThrowAfterFrame() {
		await Time.NextFrame();
		throw new InvalidOperationException("thrown after an await");
	}

	static async void RecordAfterFrames() {
		await Time.NextFrame();
		await Time.NextFrame();
		BindingProbe.Record("resumed after the throw");
	}

	static IEnumerator ThrowInRoutine() {
		yield return null;
		throw new InvalidOperationException("thrown by a routine");
	}

	static IEnumerator YieldNumber() {
		yield return 5;
	}

	static async void WaitLong() {
		await Time.Delay(100000f);
		BindingProbe.Record("resumed after a reload");
	}

	static IEnumerator WaitLongRoutine() {
		yield return Coroutines.Delay(100000f);
		BindingProbe.Record("stepped after a reload");
	}

	[ConformanceTest]
	internal static void ThrowsInContinuations() {
		SceneObject owner = BindingProbe.CreateObject("AwaitThrower");
		ThrowAfterFrame();
		RecordAfterFrames();
		Coroutines.Start(owner, ThrowInRoutine());
		Coroutines.Start(owner, YieldNumber());
		BindingProbe.StepFrames(3, 0.5f, 0);
		owner.Destroy();
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void LeavesPendingWork() {
		SceneObject owner = BindingProbe.CreateObject("AwaitPending");
		RecordAfterFrames();
		BindingProbe.StepFrames(3, 0.5f, 0);
		WaitLong();
		Coroutines.Start(owner, WaitLongRoutine());
	}

	[ConformanceTest]
	internal static void StepsFrames() {
		BindingProbe.StepFrames(3, 100000f, 1);
	}
}

static class AwaitTests {
	static int _thread;
	static long _frame;
	static readonly int[] _stamps = new int[3];
	static int _fixedResumes;
	static bool _inFixedStep;
	static int _postedOn;
	static int _worker;
	const int WARM_FRAMES = 20;
	const int MEASURED_FRAMES = 250;

	static int _laps;
	static int _frameLaps;
	static int _measuredLaps;
	static long _measuredBytes;
	static bool _stopLaps;

	static async void ResumeNextFrame() {
		await Time.NextFrame().AsValueTask();
		_thread = Environment.CurrentManagedThreadId;
		_frame = Time.FrameCount;
	}

	static async void StampAfterDelay(bool realtime, float seconds, int slot) {
		long start = Time.FrameCount;
		if (realtime) {
			await Time.DelayRealtime(seconds);
		} else {
			await Time.Delay(seconds);
		}
		_stamps[slot] = (int)(Time.FrameCount - start);
	}

	static async void CountFixedSteps() {
		for (int index = 0; index < 3; ++index) {
			await Time.NextFixedUpdate();
			++_fixedResumes;
			_inFixedStep &= Time.DeltaTime == Time.FixedDeltaTime;
		}
	}

	static async void ComputeOnWorker() {
		_worker = await Task.Run(static () => Environment.CurrentManagedThreadId);
		_postedOn = Environment.CurrentManagedThreadId;
	}

	static async void Lap(int kind) {
		while (!_stopLaps) {
			if (kind == 0) {
				await Time.NextFrame();
				++_frameLaps;
				if (_frameLaps == WARM_FRAMES) {
					_measuredLaps = -_laps;
					_measuredBytes = -GC.GetAllocatedBytesForCurrentThread();
				} else if (_frameLaps == WARM_FRAMES + MEASURED_FRAMES) {
					_measuredBytes += GC.GetAllocatedBytesForCurrentThread();
					_measuredLaps += _laps;
				}
			} else if (kind == 1) {
				await Time.Delay(0f);
			} else if (kind == 2) {
				await Time.DelayRealtime(0f);
			} else {
				await Time.NextFixedUpdate();
			}
			++_laps;
		}
	}

	[ConformanceTest]
	internal static void NextFrameResumesOnMainThread() {
		_thread = 0;
		long start = Time.FrameCount;
		ResumeNextFrame();
		Check.Equal(0, _thread);
		BindingProbe.StepFrames(3, 0.5f, 0);
		Check.Equal(Environment.CurrentManagedThreadId, _thread);
		Check.Equal(start + 1, _frame);
	}

	[ConformanceTest]
	internal static void DelayFollowsTimeScale() {
		Check.Equal(0.5f, Time.TimeScale);
		Array.Clear(_stamps);
		StampAfterDelay(false, 1f, 0);
		StampAfterDelay(true, 1f, 1);
		StampAfterDelay(false, 0f, 2);
		BindingProbe.StepFrames(6, 0.5f, 0);
		Check.Equal(4, _stamps[0]);
		Check.Equal(2, _stamps[1]);
		Check.Equal(1, _stamps[2]);
		try {
			Time.Delay(float.NaN);
			Check.Fail("a delay of NaN was taken");
		} catch (ArgumentOutOfRangeException) {
		}
	}

	[ConformanceTest]
	internal static void NextFixedUpdateResumesInFixedSteps() {
		_fixedResumes = 0;
		_inFixedStep = true;
		CountFixedSteps();
		BindingProbe.StepFrames(2, 0.5f, 0);
		Check.Equal(0, _fixedResumes);
		BindingProbe.StepFrames(1, 0.5f, 2);
		Check.Equal(2, _fixedResumes);
		BindingProbe.StepFrames(1, 0.5f, 3);
		Check.Equal(3, _fixedResumes);
		Check.True(_inFixedStep, "resumed outside a fixed step");
	}

	[ConformanceTest]
	internal static void DestroyedOwnersAwaitNeverResumes() {
		Awaiter.resumed = 0;
		SceneObject kept = BindingProbe.CreateObject("AwaitKept");
		SceneObject gone = BindingProbe.CreateObject("AwaitGone");
		kept.AddComponent<Awaiter>();
		gone.AddComponent<Awaiter>();
		gone.Destroy();
		BindingProbe.EndFrame();
		BindingProbe.StepFrames(1, 0.5f, 0);
		Check.Equal(1, Awaiter.resumed);

		kept.Destroy();
		BindingProbe.EndFrame();
		BindingProbe.StepFrames(2, 0.5f, 0);
		Check.Equal(1, Awaiter.resumed);
	}

	[ConformanceTest]
	internal static void WorkerResultsResumeOnMainThread() {
		_postedOn = 0;
		Check.True(SynchronizationContext.Current != null, "no synchronization context on main thread");
		ComputeOnWorker();
		for (int attempt = 0; attempt < 5000 && _postedOn == 0; ++attempt) {
			Thread.Sleep(1);
			BindingProbe.StepFrames(1, 0.5f, 0);
		}
		Check.Equal(Environment.CurrentManagedThreadId, _postedOn);
		Check.True(_worker != 0 && _worker != _postedOn, "work ran on main thread");
	}

	[ConformanceTest]
	internal static void RepeatedAwaitsAllocateNothing() {
		_stopLaps = false;
		_laps = 0;
		_frameLaps = 0;
		for (int kind = 0; kind < 4; ++kind) {
			Lap(kind);
		}
		BindingProbe.StepFrames(WARM_FRAMES + MEASURED_FRAMES, 0.5f, 1);
		_stopLaps = true;
		BindingProbe.StepFrames(WARM_FRAMES + MEASURED_FRAMES, 0.5f, 1);
		Check.Equal(4 * MEASURED_FRAMES, _measuredLaps);
		Check.Equal(0L, _measuredBytes);
	}
}

static class CoroutineTests {
	static readonly List<string> _trace = new();

	static IEnumerator Inner() {
		_trace.Add("inner");
		yield return null;
		_trace.Add("inner end");
	}

	static IEnumerator Steps() {
		_trace.Add("start");
		yield return null;
		_trace.Add("frame");
		yield return Inner();
		_trace.Add("after inner");
		yield return Coroutines.NextFixedUpdate;
		_trace.Add("fixed");
		yield return Coroutines.Delay(0.5f);
		_trace.Add("delay");
		yield return Coroutines.DelayRealtime(1f);
		_trace.Add("realtime");
	}

	static IEnumerator Count(int[] steps) {
		while (true) {
			++steps[0];
			yield return null;
		}
	}

	static void Traced(params string[] expected) {
		Check.Equal(string.Join(", ", expected), string.Join(", ", _trace));
		_trace.Clear();
	}

	[ConformanceTest]
	internal static void RoutineStepsThroughItsWaits() {
		_trace.Clear();
		SceneObject owner = BindingProbe.CreateObject("RoutineOwner");
		Coroutine routine = Coroutines.Start(owner, Steps());
		Traced("start");
		Check.True(routine.IsRunning, "routine ended at its 1st yield");
		BindingProbe.StepFrames(1, 0.5f, 0);
		Traced("frame", "inner");
		BindingProbe.StepFrames(1, 0.5f, 0);
		Traced("inner end", "after inner");
		BindingProbe.StepFrames(2, 0.5f, 0);
		Traced();
		BindingProbe.StepFrames(1, 0.5f, 1);
		Traced("fixed");
		BindingProbe.StepFrames(1, 0.5f, 0);
		Traced();
		BindingProbe.StepFrames(8, 0.5f, 0);
		Traced("delay", "realtime");
		Check.True(!routine.IsRunning, "finished routine still runs");
		Coroutine copy = routine;
		Check.True(copy == routine && routine != default, "handle equality");
		owner.Destroy();
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void StoppedRoutinesEnd() {
		SceneObject owner = BindingProbe.CreateObject("RoutineStopper");
		SceneObject other = BindingProbe.CreateObject("RoutineOther");
		int[] first = new int[1];
		int[] second = new int[1];
		int[] third = new int[1];
		Coroutine stopped = Coroutines.Start(owner, Count(first));
		Coroutines.Start(owner, Count(second));
		Coroutine orphan = Coroutines.Start(other, Count(third));
		BindingProbe.StepFrames(1, 0.5f, 0);
		Check.Equal(6, first[0] + second[0] + third[0]);

		stopped.Stop();
		Check.True(!stopped.IsRunning, "stopped routine still runs");
		BindingProbe.StepFrames(1, 0.5f, 0);
		Check.Equal(2, first[0]);
		Check.Equal(3, second[0]);

		Coroutines.StopAll(owner);
		BindingProbe.StepFrames(1, 0.5f, 0);
		Check.Equal(3, second[0]);
		Check.Equal(4, third[0]);

		other.Destroy();
		BindingProbe.EndFrame();
		Check.True(!orphan.IsRunning, "routine of a destroyed owner still runs");
		BindingProbe.StepFrames(2, 0.5f, 0);
		Check.Equal(4, third[0]);
		owner.Destroy();
		BindingProbe.EndFrame();
	}
}
