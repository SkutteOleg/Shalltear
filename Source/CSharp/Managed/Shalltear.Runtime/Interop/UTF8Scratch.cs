using System;
using System.Buffers;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Unicode;

namespace Shalltear.Interop;

// stack must be stackalloc memory (pointer taken without pinning)
internal unsafe ref struct UTF8Scratch {
	internal const int STACK_BYTES = 256;

	byte* _heap;
	SltString _text;

	internal UTF8Scratch(string? text, Span<byte> stack) {
		_heap = null;
		_text = default;
		if (string.IsNullOrEmpty(text)) {
			return;
		}
		Span<byte> buffer = stack;
		if (Utf8.FromUtf16(text, buffer, out _, out int written) != OperationStatus.Done) {
			int exact = Encoding.UTF8.GetByteCount(text);
			_heap = (byte*)NativeMemory.Alloc((nuint)exact);
			buffer = new Span<byte>(_heap, exact);
			Utf8.FromUtf16(text, buffer, out _, out written);
		}
		_text.length = written;
		_text.ptr = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(buffer));
	}

	internal readonly SltString Text => _text;

	public void Dispose() {
		if (_heap != null) {
			NativeMemory.Free(_heap);
			_heap = null;
		}
	}
}
