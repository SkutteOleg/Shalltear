using System;

namespace Shalltear;

/// <summary>Planar samples of an audio callback (channel c, frame i at data[c * stride + i]).</summary>
public partial struct AudioBlock {
	/// <summary>Samples of 1 channel (valid until the engine's call returns).</summary>
	public readonly unsafe Span<float> Channel(int channel) {
		if ((uint)channel >= channels) {
			throw new ArgumentOutOfRangeException(nameof(channel));
		}
		return new Span<float>((float*)data + (long)channel * stride, (int)frames);
	}
}
