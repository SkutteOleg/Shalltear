using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

internal sealed class ConformanceFailure : Exception {
	public ConformanceFailure(string message) : base(message) {}
}

internal static unsafe class ConformanceRegistry {
	static readonly List<(string Name, Action Body)> _tests = new();
	static byte* _scratch;
	static int _scratchSize;

	internal static int Count => _tests.Count;

	internal static void Add(string name, Action body) {
		_tests.Add((name, body));
	}

	internal static void Clear() {
		_tests.Clear();
	}

	internal static void GetName(int index, SltString* outName) {
		*outName = Answer(_tests[index].Name);
	}

	internal static int Run(int index, SltString* outMessage) {
		*outMessage = default;
		try {
			_tests[index].Body();
		} catch (ConformanceFailure failure) {
			*outMessage = Answer(failure.Message);
			return 1;
		}
		return Bridge.OK;
	}

	static SltString Answer(string text) {
		int size = Encoding.UTF8.GetByteCount(text);
		if (size > _scratchSize) {
			_scratchSize = Math.Max(256, size * 2);
			_scratch = (byte*)NativeMemory.Realloc(_scratch, (nuint)_scratchSize);
		}
		Encoding.UTF8.GetBytes(text, new Span<byte>(_scratch, size));
		return new SltString { ptr = _scratch, length = size };
	}
}
