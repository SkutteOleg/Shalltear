using System;
using System.Collections;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks.Sources;

namespace Shalltear.Interop;

internal abstract class FrameWaiter {
	internal EngineObject? _owner;
	internal double _due;
	internal long _order;

	internal abstract void Resume();

	internal abstract void Drop();
}

internal sealed class FrameSource : FrameWaiter, IValueTaskSource {
	static readonly Action<object?> _completed = static state => {};

	Action<object?>? _continuation;
	object? _state;
	ExecutionContext? _executionContext;
	internal short _version;

	public ValueTaskSourceStatus GetStatus(short token) {
		Validate(token);
		return ReferenceEquals(Volatile.Read(ref _continuation), _completed) ? ValueTaskSourceStatus.Succeeded : ValueTaskSourceStatus.Pending;
	}

	public void OnCompleted(Action<object?> continuation, object? state, short token, ValueTaskSourceOnCompletedFlags flags) {
		Validate(token);
		if ((flags & ValueTaskSourceOnCompletedFlags.FlowExecutionContext) != 0) {
			_executionContext = ExecutionContext.Capture();
		}
		_state = state;
		Action<object?>? previous = Interlocked.CompareExchange(ref _continuation, continuation, null);
		if (previous == null) {
			return;
		}
		_state = null;
		_executionContext = null;
		if (!ReferenceEquals(previous, _completed)) {
			throw new InvalidOperationException("Wait awaited twice. Await it once");
		}
		FrameAwaits.Post(continuation, state);
	}

	public void GetResult(short token) {
		Validate(token);
		FrameAwaits.Return(this);
	}

	internal override void Resume() {
		Action<object?>? continuation = Interlocked.Exchange(ref _continuation, _completed);
		if (continuation == null) {
			return;
		}
		object? state = _state;
		ExecutionContext? flowed = _executionContext;
		_state = null;
		_executionContext = null;
		if (flowed == null) {
			continuation(state);
		} else {
			ExecutionContext.Run(flowed, static boxed => {
				(Action<object?> continuation, object? state) = ((Action<object?>, object?))boxed!;
				continuation(state);
			}, (continuation, state));
		}
	}

	internal override void Drop() {
		FrameAwaits.Return(this);
	}

	internal void Reset() {
		_continuation = null;
		_state = null;
		_executionContext = null;
		_owner = null;
		++_version;
	}

	void Validate(short token) {
		if (token != _version) {
			throw new InvalidOperationException("Wait used after it completed. Await it once");
		}
	}
}

internal sealed class Routine : FrameWaiter {
	IEnumerator?[] _stack = new IEnumerator?[4];
	int _depth;
	bool _stepping;
	internal int _generation;
	internal int _slot = -1;
	internal bool _stopped;

	internal void Begin(EngineObject owner, IEnumerator routine) {
		_owner = owner;
		_stopped = false;
		_depth = 1;
		_stack[0] = routine;
	}

	internal override void Resume() {
		if (_stopped) {
			End();
			return;
		}
		_stepping = true;
		while (_depth > 0 && !_stopped) {
			IEnumerator top = _stack[_depth - 1]!;
			bool moved;
			try {
				moved = top.MoveNext();
			} catch (Exception exception) {
				Bridge.Report(exception);
				_stopped = true;
				break;
			}
			if (!moved) {
				_stack[--_depth] = null;
				continue;
			}
			object? yielded = top.Current;
			if (yielded is IEnumerator nested) {
				if (_depth == _stack.Length) {
					Array.Resize(ref _stack, _depth * 2);
				}
				_stack[_depth++] = nested;
				continue;
			}
			if (FrameAwaits.Wait(this, yielded)) {
				_stepping = false;
				return;
			}
			Bridge.Report(new EngineException("Coroutine yielded " + yielded!.GetType().Name + ". Yield null, a Coroutines wait or an IEnumerator"));
			_stopped = true;
		}
		_stepping = false;
		End();
	}

	internal override void Drop() {
		End();
	}

	internal void Stop() {
		_stopped = true;
		if (!_stepping) {
			Array.Clear(_stack);
			_depth = 0;
		}
	}

	void End() {
		Array.Clear(_stack);
		_depth = 0;
		_owner = null;
		_stopped = true;
		++_generation;
		FrameAwaits.Finish(this);
	}
}

internal static class FrameAwaits {
	// must match SLT_PUMP_* in ScriptBridge.h
	internal const int PHASE_UPDATE = 0;
	internal const int PHASE_FIXED_UPDATE = 1;

	internal const int WAIT_FRAME = 0;
	internal const int WAIT_FIXED = 1;
	internal const int WAIT_DELAY = 2;
	internal const int WAIT_DELAY_REALTIME = 3;

	sealed class WaitList {
		FrameWaiter?[] _items = new FrameWaiter?[16];
		FrameWaiter?[] _batch = new FrameWaiter?[16];
		int _count;
		bool _resuming;

		internal void Add(FrameWaiter waiter) {
			if (_count == _items.Length) {
				Array.Resize(ref _items, _count * 2);
			}
			_items[_count++] = waiter;
		}

		internal void ResumeAll(long before) {
			if (_resuming || _count == 0) {
				return;
			}
			int count = _count;
			(_items, _batch) = (_batch, _items);
			_count = 0;
			_resuming = true;
			for (int index = 0; index < count; ++index) {
				FrameWaiter waiter = _batch[index]!;
				_batch[index] = null;
				if (waiter._order < before) {
					Run(waiter);
				} else {
					Add(waiter);
				}
			}
			_resuming = false;
		}

		internal void Clear() {
			Array.Clear(_items);
			Array.Clear(_batch);
			_count = 0;
		}
	}

	sealed class WaitHeap {
		FrameWaiter?[] _items = new FrameWaiter?[16];
		int _count;

		static bool Before(FrameWaiter left, FrameWaiter right) {
			return left._due < right._due || (left._due == right._due && left._order < right._order);
		}

		internal void Add(FrameWaiter waiter) {
			if (_count == _items.Length) {
				Array.Resize(ref _items, _count * 2);
			}
			int index = _count++;
			while (index > 0) {
				int parent = (index - 1) / 2;
				if (!Before(waiter, _items[parent]!)) {
					break;
				}
				_items[index] = _items[parent];
				index = parent;
			}
			_items[index] = waiter;
		}

		internal void ResumeDue(double now, long before) {
			while (_count > 0) {
				FrameWaiter top = _items[0]!;
				if (top._due > now || top._order >= before) {
					return;
				}
				FrameWaiter last = _items[--_count]!;
				_items[_count] = null;
				int index = 0;
				while (true) {
					int child = index * 2 + 1;
					if (child >= _count) {
						break;
					}
					if (child + 1 < _count && Before(_items[child + 1]!, _items[child]!)) {
						++child;
					}
					if (!Before(_items[child]!, last)) {
						break;
					}
					_items[index] = _items[child];
					index = child;
				}
				if (_count > 0) {
					_items[index] = last;
				}
				Run(top);
			}
		}

		internal void Clear() {
			Array.Clear(_items);
			_count = 0;
		}
	}

	static readonly EngineSynchronizationContext _context = new();
	static readonly WaitList _frames = new();
	static readonly WaitList _fixedSteps = new();
	static readonly WaitHeap _delays = new();
	static readonly WaitHeap _realtimeDelays = new();
	static FrameSource?[] _sources = new FrameSource?[16];
	static int _sourceCount;
	static Routine?[] _idleRoutines = new Routine?[8];
	static int _idleRoutineCount;
	static Routine?[] _routines = new Routine?[8];
	static int _routineCount;
	static long _order;

	static EngineObject? _owner;

	internal static void Install() {
		SynchronizationContext.SetSynchronizationContext(_context);
	}

	internal static void Uninstall() {
		Forget();
		if (ReferenceEquals(SynchronizationContext.Current, _context)) {
			SynchronizationContext.SetSynchronizationContext(null);
		}
	}

	internal static void Forget() {
		_context.Forget();
		_frames.Clear();
		_fixedSteps.Clear();
		_delays.Clear();
		_realtimeDelays.Clear();
		for (int index = 0; index < _routineCount; ++index) {
			Routine routine = _routines[index]!;
			_routines[index] = null;
			routine._slot = -1;
			routine.Stop();
			++routine._generation;
			routine._owner = null;
		}
		_routineCount = 0;
		_owner = null;
	}

	internal static void Pump(int phase) {
		long before = _order;
		if (phase == PHASE_FIXED_UPDATE) {
			_fixedSteps.ResumeAll(before);
			return;
		}
		_context.Drain();
		_frames.ResumeAll(before);
		_delays.ResumeDue(Time.ScaledTime, before);
		_realtimeDelays.ResumeDue(Time.UnscaledTime, before);
	}

	internal static FrameAwaitable Await(int kind, float seconds, string member) {
		if (!Bridge.IsMainThread) {
			Bridge.ThrowOffMainThread(member);
		}
		if (float.IsNaN(seconds)) {
			throw new ArgumentOutOfRangeException(nameof(seconds), member + " got NaN seconds. Pass a number");
		}
		FrameSource source;
		if (_sourceCount > 0) {
			source = _sources[--_sourceCount]!;
			_sources[_sourceCount] = null;
		} else {
			source = new FrameSource();
		}
		nint hook = HookState._owner;
		source._owner = hook != 0 ? GCHandle.FromIntPtr(hook).Target as EngineObject : _owner;
		Schedule(source, kind, seconds);
		return new FrameAwaitable(source);
	}

	internal static bool Wait(Routine routine, object? yielded) {
		if (yielded == null) {
			Schedule(routine, WAIT_FRAME, 0f);
			return true;
		}
		if (yielded is not CoroutineWait wait) {
			return false;
		}
		Schedule(routine, wait._kind, wait._seconds);
		return true;
	}

	static void Schedule(FrameWaiter waiter, int kind, float seconds) {
		waiter._order = _order++;
		switch (kind) {
		case WAIT_FRAME:
			_frames.Add(waiter);
			break;
		case WAIT_FIXED:
			_fixedSteps.Add(waiter);
			break;
		case WAIT_DELAY:
			waiter._due = Time.ScaledTime + Math.Max(seconds, 0f);
			_delays.Add(waiter);
			break;
		default:
			waiter._due = Time.UnscaledTime + Math.Max(seconds, 0f);
			_realtimeDelays.Add(waiter);
			break;
		}
	}

	static void Run(FrameWaiter waiter) {
		EngineObject? owner = waiter._owner;
		if (owner is not null && !(owner._id != 0 && ObjectBindings.IsAlive(owner._id))) {
			waiter.Drop();
			return;
		}
		EngineObject? previous = _owner;
		nint previousHook = HookState._owner;
		_owner = owner;
		HookState._owner = 0;
		try {
			waiter.Resume();
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
		_owner = previous;
		HookState._owner = previousHook;
	}

	internal static void Return(FrameSource source) {
		source.Reset();
		if (!Bridge.IsMainThread) {
			return;
		}
		if (_sourceCount == _sources.Length) {
			Array.Resize(ref _sources, _sourceCount * 2);
		}
		_sources[_sourceCount++] = source;
	}

	internal static void Post(Action<object?> continuation, object? state) {
		_context.Post(static boxed => {
			(Action<object?> continuation, object? state) = ((Action<object?>, object?))boxed!;
			continuation(state);
		}, (continuation, state));
	}

	internal static Routine Start(EngineObject owner, IEnumerator routine) {
		Routine started;
		if (_idleRoutineCount > 0) {
			started = _idleRoutines[--_idleRoutineCount]!;
			_idleRoutines[_idleRoutineCount] = null;
		} else {
			started = new Routine();
		}
		started.Begin(owner, routine);
		if (_routineCount == _routines.Length) {
			Array.Resize(ref _routines, _routineCount * 2);
		}
		started._slot = _routineCount;
		_routines[_routineCount++] = started;
		return started;
	}

	internal static void Step(Routine routine) {
		Run(routine);
	}

	internal static void Finish(Routine routine) {
		int slot = routine._slot;
		if (slot < 0) {
			return;
		}
		Routine last = _routines[--_routineCount]!;
		_routines[_routineCount] = null;
		if (!ReferenceEquals(last, routine)) {
			_routines[slot] = last;
			last._slot = slot;
		}
		routine._slot = -1;
		if (_idleRoutineCount == _idleRoutines.Length) {
			Array.Resize(ref _idleRoutines, _idleRoutineCount * 2);
		}
		_idleRoutines[_idleRoutineCount++] = routine;
	}

	internal static void StopAll(EngineObject owner) {
		for (int index = 0; index < _routineCount; ++index) {
			Routine routine = _routines[index]!;
			if (ReferenceEquals(routine._owner, owner)) {
				routine.Stop();
			}
		}
	}
}
