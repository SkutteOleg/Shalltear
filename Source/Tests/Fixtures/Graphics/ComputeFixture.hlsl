#pragma shalltear_kernel

#ifdef SHALLTEAR_WEBGPU
#define SLT_FIXTURE_PUSH(Type, Name) [[vk::binding(0, 3)]] cbuffer SltPushBlock { Type Name; };
#else
#define SLT_FIXTURE_PUSH(Type, Name) [[vk::push_constant]] Type Name;
#endif

// THE RHI TIER'S ONE KERNEL (VisualEffects.md §3.1). What it does is chosen by
// `mode`, so every compute case compiles one file:
//   0  values[i] = base + i, for i < count, after `spin` rounds of work that
//      hold the write back - which is what lets a missing barrier be SEEN
//   1  values[i] = values[i] * 2, for i < count
//   2  values[0..2] = (count, 1, 1): a dispatch's group counts, from a kernel
//   3  values[0..3] = (count, 1, 0, 0): a draw's vertex and instance counts
//      and its first vertex and instance
[[vk::binding(0, 0)]] RWByteAddressBuffer values;

struct ComputePush {
	uint mode;
	uint count;
	uint base;
	uint spin;
	// zero at run time and unknown to the compiler, so the spin's result
	// reaches the store and the loop cannot be removed
	uint mask;
};
SLT_FIXTURE_PUSH(ComputePush, push)

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
	uint i = id.x;
	if (push.mode == 2) {
		if (i == 0) {
			values.Store3(0, uint3(push.count, 1, 1));
		}
		return;
	}
	if (push.mode == 3) {
		if (i == 0) {
			values.Store4(0, uint4(push.count, 1, 0, 0));
		}
		return;
	}
	if (i >= push.count) {
		return;
	}
	if (push.mode == 0) {
		uint h = i;
		for (uint s = 0; s < push.spin; ++s) {
			h = h * 1664525u + 1013904223u;
		}
		values.Store(i * 4, push.base + i + (h & push.mask));
	} else {
		values.Store(i * 4, values.Load(i * 4) * 2u);
	}
}
