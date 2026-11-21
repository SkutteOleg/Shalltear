using System;
using System.Runtime.ExceptionServices;
using System.Threading;

namespace Shalltear.Interop;

internal sealed class EngineSynchronizationContext : SynchronizationContext {
	struct Posted {
		public SendOrPostCallback callback;
		public object? state;
	}

	sealed class Sent {
		public SendOrPostCallback callback = null!;
		public object? state;
		public Exception? failure;
		public readonly ManualResetEventSlim done = new(false);
	}

	readonly object _gate = new();
	Posted[] _queue = new Posted[16];
	Posted[] _batch = new Posted[16];
	int _count;
	bool _draining;

	public override void Post(SendOrPostCallback callback, object? state) {
		lock (_gate) {
			if (_count == _queue.Length) {
				Array.Resize(ref _queue, _count * 2);
			}
			_queue[_count++] = new Posted { callback = callback, state = state };
		}
	}

	public override void Send(SendOrPostCallback callback, object? state) {
		if (Bridge.IsMainThread) {
			callback(state);
			return;
		}
		Sent sent = new() { callback = callback, state = state };
		Post(static boxed => {
			Sent sent = (Sent)boxed!;
			try {
				sent.callback(sent.state);
			} catch (Exception exception) {
				sent.failure = exception;
			}
			sent.done.Set();
		}, sent);
		sent.done.Wait();
		sent.done.Dispose();
		if (sent.failure != null) {
			ExceptionDispatchInfo.Throw(sent.failure);
		}
	}

	public override SynchronizationContext CreateCopy() {
		return this;
	}

	internal void Drain() {
		if (_draining) {
			return;
		}
		int count;
		lock (_gate) {
			count = _count;
			if (count == 0) {
				return;
			}
			(_queue, _batch) = (_batch, _queue);
			_count = 0;
		}
		_draining = true;
		for (int index = 0; index < count; ++index) {
			Posted posted = _batch[index];
			_batch[index] = default;
			try {
				posted.callback(posted.state);
			} catch (Exception exception) {
				Bridge.Report(exception);
			}
		}
		_draining = false;
	}

	internal void Forget() {
		lock (_gate) {
			Array.Clear(_queue);
			_count = 0;
		}
		if (!_draining) {
			Array.Clear(_batch);
		}
	}
}
