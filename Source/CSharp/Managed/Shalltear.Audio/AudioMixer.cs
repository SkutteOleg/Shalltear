using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear;

public sealed partial class AudioMixer {
	const int STACK_IDS = 32;

	/// <summary>Fill results with each group at a path ending in subPath. Returns their count.</summary>
	[SkipLocalsInit]
	public int FindMatchingGroups(string subPath, List<AudioMixerGroup> results) {
		const string MEMBER = "AudioMixer.FindMatchingGroups";
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<ulong> ids = stackalloc ulong[STACK_IDS];
		int count = FindGroups(subPath, ids, MEMBER);
		ulong[]? rented = null;
		if (count > ids.Length) {
			rented = ArrayPool<ulong>.Shared.Rent(count);
			ids = rented;
			count = Math.Min(FindGroups(subPath, ids, MEMBER), ids.Length);
		}
		foreach (ulong id in ids.Slice(0, count)) {
			if (ObjectCache.Find(id) is AudioMixerGroup group) {
				results.Add(group);
			}
		}
		if (rented != null) {
			ArrayPool<ulong>.Shared.Return(rented);
		}
		return results.Count;
	}

	/// <summary>Move to a blend of snapshots (1 weight each) over timeToReach seconds.</summary>
	[SkipLocalsInit]
	public bool TransitionToSnapshots(ReadOnlySpan<AudioMixerSnapshot> snapshots, ReadOnlySpan<float> weights, float timeToReach) {
		Span<ulong> ids = snapshots.Length <= STACK_IDS ? stackalloc ulong[STACK_IDS] : new ulong[snapshots.Length];
		for (int index = 0; index < snapshots.Length; ++index) {
			ArgumentNullException.ThrowIfNull(snapshots[index]);
			ids[index] = snapshots[index]._id;
		}
		return TransitionToWeights(ids.Slice(0, snapshots.Length), weights, timeToReach, "AudioMixer.TransitionToSnapshots");
	}

	/// <summary>Read an exposed parameter. False when there is no such parameter.</summary>
	public bool GetFloat(string name, out float value) {
		value = 0f;
		return ReadFloat(name, new Span<float>(ref value), "AudioMixer.GetFloat");
	}
}
