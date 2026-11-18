using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class RuntimeTests {
	sealed class Finalizable {
		public static int finalized;

		~Finalizable() {
			Interlocked.Increment(ref finalized);
		}
	}

	[ThreadStatic]
	static int _perThread;

	[MethodImpl(MethodImplOptions.NoInlining)]
	static int Thrower(int value) {
		if (value > 0) {
			throw new InvalidOperationException("boom " + value);
		}
		return value;
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	static string? Missing() {
		return Environment.ProcessorCount > 0 ? null : "";
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	static void MakeGarbage() {
		for (int index = 0; index < 1000; ++index) {
			_ = new Finalizable();
		}
	}

	static long Remainders(int modulus) {
		long sum = 0;
		for (int value = 0; value < 1_000_000; ++value) {
			sum += value % modulus;
		}
		return sum;
	}

	[ConformanceTest]
	internal static void ExceptionIsThrownAndCaught() {
		try {
			Thrower(3);
		} catch (InvalidOperationException exception) {
			Check.Equal("boom 3", exception.Message);
			return;
		}
		Check.Fail("nothing thrown");
	}

	[ConformanceTest]
	internal static void NullDereferenceThrows() {
		try {
			_ = Missing()!.Length;
		} catch (NullReferenceException) {
			return;
		}
		Check.Fail("no NullReferenceException");
	}

	[ConformanceTest]
	internal static void DivisionByZeroThrows() {
		int zero = Environment.ProcessorCount - Environment.ProcessorCount;
		try {
			_ = 10 / zero;
		} catch (DivideByZeroException) {
			return;
		}
		Check.Fail("no DivideByZeroException");
	}

	[ConformanceTest]
	internal static void CheckedOverflowThrows() {
		int value = int.MaxValue - Environment.ProcessorCount;
		try {
			checked {
				value += 2 * Environment.ProcessorCount;
			}
		} catch (OverflowException) {
			return;
		}
		Check.Fail("no OverflowException, value " + value);
	}

	[ConformanceTest]
	internal static void ParallelLoopKeepsThreadStatics() {
		long[] sums = new long[8];
		Parallel.For(0, sums.Length, index => {
			_perThread = index + 1;
			long sum = Remainders(index + 2);
			sums[index] = sum + _perThread;
		});
		for (int index = 0; index < sums.Length; ++index) {
			Check.Equal(Remainders(index + 2) + index + 1, sums[index]);
		}
	}

	[ConformanceTest]
	internal static void FinalizersRun() {
		MakeGarbage();
		GC.Collect();
		GC.WaitForPendingFinalizers();
		GC.Collect();
		Check.True(Finalizable.finalized >= 900, "finalized " + Finalizable.finalized + " of 1000");
	}

	[ConformanceTest]
	internal static void LinqGroupsAndFormats() {
		Dictionary<int, int> groups = Enumerable.Range(0, 1000).GroupBy(value => value % 7).ToDictionary(group => group.Key, group => group.Sum());
		Check.Equal(7, groups.Count);
		Check.Equal("71,500|3.50", string.Create(CultureInfo.InvariantCulture, $"{groups[3]:N0}|{3.5:F2}"));
	}
}
