using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

internal struct HookCost {
	public uint calls;
	public long ticks;
	public long bytes;
}

internal static unsafe class ScriptProfile {
	internal const int HOOK_COUNT = 8;

	static readonly int[] _collections = new int[3];
	static long _pauseTicks;

	internal static void Run(Behavior script, int hook) {
		long bytes = GC.GetAllocatedBytesForCurrentThread();
		long start = Stopwatch.GetTimestamp();
		try {
			script.RunHook(hook);
		} finally {
			long ticks = Stopwatch.GetTimestamp() - start;
			long allocated = GC.GetAllocatedBytesForCurrentThread() - bytes;
			ScriptType type = script._scriptType!;
			type._costs ??= new HookCost[HOOK_COUNT];
			ref HookCost cost = ref type._costs[hook];
			++cost.calls;
			cost.ticks += ticks;
			cost.bytes += allocated;
		}
	}

	[UnmanagedCallersOnly]
	internal static void SetProfiling(int enabled) {
		HookState._profiling = enabled != 0;
		foreach (ScriptType type in ScriptTypes.All) {
			type._costs = null;
		}
		for (int generation = 0; generation < _collections.Length; ++generation) {
			_collections[generation] = GC.CollectionCount(generation);
		}
		_pauseTicks = GC.GetTotalPauseDuration().Ticks;
	}

	[UnmanagedCallersOnly]
	internal static int ReadProfile(SltProfileRow* rows, int capacity, SltProfileFrame* frame) {
		try {
			int count = 0;
			foreach (ScriptType type in ScriptTypes.All) {
				for (int hook = 0; type._costs != null && hook < HOOK_COUNT; ++hook) {
					count += type._costs[hook].calls != 0 ? 1 : 0;
				}
			}
			if (count > capacity) {
				return count;
			}
			int written = 0;
			for (int id = 0; id < ScriptTypes.All.Count; ++id) {
				HookCost[]? costs = ScriptTypes.All[id]._costs;
				for (int hook = 0; costs != null && hook < HOOK_COUNT; ++hook) {
					if (costs[hook].calls == 0) {
						continue;
					}
					rows[written].managedTypeId = id;
					rows[written].hook = hook;
					rows[written].calls = costs[hook].calls;
					rows[written].nanoseconds = (long)(costs[hook].ticks * (1e9 / Stopwatch.Frequency));
					rows[written].bytes = costs[hook].bytes;
					costs[hook] = default;
					++written;
				}
			}
			for (int generation = 0; generation < _collections.Length; ++generation) {
				int collected = GC.CollectionCount(generation);
				frame->collections[generation] = collected - _collections[generation];
				_collections[generation] = collected;
			}
			long pauseTicks = GC.GetTotalPauseDuration().Ticks;
			frame->pauseNanoseconds = (pauseTicks - _pauseTicks) * 100;
			_pauseTicks = pauseTicks;
			frame->heapBytes = GC.GetTotalMemory(false);
			return written;
		} catch (Exception exception) {
			Bridge.Report(exception);
			return 0;
		}
	}
}
