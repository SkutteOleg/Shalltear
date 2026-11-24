#pragma shalltear_kernel

#ifdef SHALLTEAR_WEBGPU
#define SLT_FIXTURE_PUSH(Type, Name) [[vk::binding(0, 3)]] cbuffer SltPushBlock { Type Name; };
#else
#define SLT_FIXTURE_PUSH(Type, Name) [[vk::push_constant]] Type Name;
#endif

// A KERNEL SAMPLING A TEXTURE ASSET (VisualEffects.md §9.13): the volume at
// binding 1, its sampler at binding 2, read at `uvw` on mip 0 and stored as
// four floats at the front of `values` - what FramePassTest reads back to see
// which texture a dispatch bound
[[vk::binding(0, 0)]] RWByteAddressBuffer values;
[[vk::binding(1, 0)]] Texture3D volume;
[[vk::binding(2, 0)]] SamplerState volumeSampler;

struct TexturePush {
	float3 uvw;
	float level;
};
SLT_FIXTURE_PUSH(TexturePush, push)

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
	values.Store4(0, asuint(volume.SampleLevel(volumeSampler, push.uvw, push.level)));
}
