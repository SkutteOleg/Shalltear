using System;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Threading.Tasks.Sources;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Wait returned by Time (await once).</summary>
public readonly struct FrameAwaitable {
	readonly FrameSource? _source;
	readonly short _token;

	internal FrameAwaitable(FrameSource source) {
		_source = source;
		_token = source._version;
	}

	/// <summary>Called by await.</summary>
	public FrameAwaiter GetAwaiter() {
		return new FrameAwaiter(_source, _token);
	}

	/// <summary>Same wait as a ValueTask (for Task.WhenAll + similar).</summary>
	public ValueTask AsValueTask() {
		return _source == null ? default : new ValueTask(_source, _token);
	}

	/// <summary>Called by await.</summary>
	public readonly struct FrameAwaiter : ICriticalNotifyCompletion {
		static readonly Action<object?> _invoke = static continuation => ((Action)continuation!)();

		readonly FrameSource? _source;
		readonly short _token;

		internal FrameAwaiter(FrameSource? source, short token) {
			_source = source;
			_token = token;
		}

		/// <summary>True once the wait is over.</summary>
		public bool IsCompleted => _source == null || _source.GetStatus(_token) != ValueTaskSourceStatus.Pending;

		/// <summary>Called by await.</summary>
		public void GetResult() {
			_source?.GetResult(_token);
		}

		/// <summary>Called by await.</summary>
		public void OnCompleted(Action continuation) {
			if (_source == null) {
				continuation();
			} else {
				_source.OnCompleted(_invoke, continuation, _token, ValueTaskSourceOnCompletedFlags.FlowExecutionContext);
			}
		}

		/// <summary>Called by await.</summary>
		public void UnsafeOnCompleted(Action continuation) {
			if (_source == null) {
				continuation();
			} else {
				_source.OnCompleted(_invoke, continuation, _token, ValueTaskSourceOnCompletedFlags.None);
			}
		}
	}
}
