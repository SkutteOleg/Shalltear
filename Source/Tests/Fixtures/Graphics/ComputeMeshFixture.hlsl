#pragma shalltear_kernel

#include "ShalltearBufferAccess.hlsli"

// A KERNEL READING A MESH BY ADDRESS (VisualEffects.md §9.15): for each of the
// first `count` indices, the index itself and the x of the position it names -
// the first float of a 52-byte MeshVertex - stored as two words per index:
// what FramePassTest reads back to see that a mesh's indices reach a kernel
// through MeshAsset::indexBufferAddress
[[vk::binding(0, 0)]] RWByteAddressBuffer values;

struct MeshPush {
	slt_addr vertices;
	slt_addr indices;
	uint count;
	uint padding;
};
SLT_PUSH_CONSTANT(MeshPush, push)

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= push.count) {
		return;
	}
	const uint index = ShalltearLoadU32(ShalltearAddrAdd(push.indices, id.x * 4u));
	const float x = ShalltearLoadF32(ShalltearAddrAdd(push.vertices, index * 52u));
	values.Store2(id.x * 8, uint2(index, asuint(x)));
}
