#pragma shalltear_kernel
#define SHALLTEAR_VERTEX_HEAP_RW 1
#include "ShalltearCommon.hlsli"

// must match GPUSkinningPush in Skinning.h
struct ShalltearSkinningPush {
	slt_addr source;
	slt_addr output;
	slt_addr influences;
	slt_addr matrices;
	slt_addr morphRanges;
	slt_addr morphDeltas;
	slt_addr frameScales;
	uint vertexCount;
	uint influenceCount;
	uint jointCount;
	uint flags;
};
SLT_PUSH_CONSTANT(ShalltearSkinningPush, gSkin)

#define SHALLTEAR_SKINNING_GROUP_SIZE 64u
#define SHALLTEAR_SKINNING_ROW (65535u * SHALLTEAR_SKINNING_GROUP_SIZE)
#define SHALLTEAR_SKINNING_RENORMALIZE 1u
#define SHALLTEAR_MORPH_DELTA_STRIDE 32
#define SHALLTEAR_INFLUENCE_STRIDE 24

float3 SkinningNormalize(float3 v) {
	float size = sqrt(dot(v, v));
	if (size > 1e-5) {
		return v / size;
	}
	return float3(0.0, 0.0, 0.0);
}

[numthreads(SHALLTEAR_SKINNING_GROUP_SIZE, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
	uint index = id.y * SHALLTEAR_SKINNING_ROW + id.x;
	if (index >= gSkin.vertexCount) {
		return;
	}
	ShalltearVertex vertex = ShalltearLoadVertex(gSkin.source, index);

	if (ShalltearAddrValid(gSkin.frameScales)) {
		uint2 range = ShalltearLoadU32x2(ShalltearAddrAdd(gSkin.morphRanges, index * 8u));
		for (uint entry = range.x; entry < range.x + range.y; ++entry) {
			slt_addr at = ShalltearAddrAdd(gSkin.morphDeltas, entry * SHALLTEAR_MORPH_DELTA_STRIDE);
			float4 position = ShalltearLoadF32x4(at);
			float scale = ShalltearLoadF32(ShalltearAddrAdd(gSkin.frameScales, asuint(position.w) * 4u));
			if (scale != 0.0) {
				vertex.position += position.xyz * scale;
				vertex.normal += ShalltearLoadF32x4(ShalltearAddrAdd(at, 16u)).xyz * scale;
			}
		}
		if ((gSkin.flags & SHALLTEAR_SKINNING_RENORMALIZE) != 0) {
			vertex.normal = SkinningNormalize(vertex.normal);
		}
	}

	if (ShalltearAddrValid(gSkin.matrices) && index < gSkin.influenceCount) {
		slt_addr at = ShalltearAddrAdd(gSkin.influences, index * SHALLTEAR_INFLUENCE_STRIDE);
		uint2 packed = ShalltearLoadU32x2(at);
		float4 weights = ShalltearLoadF32x4(ShalltearAddrAdd(at, 8u));
		uint joints[4] = {packed.x & 0xFFFFu, packed.x >> 16, packed.y & 0xFFFFu, packed.y >> 16};

		float4 column0 = float4(0.0, 0.0, 0.0, 0.0);
		float4 column1 = float4(0.0, 0.0, 0.0, 0.0);
		float4 column2 = float4(0.0, 0.0, 0.0, 0.0);
		float4 column3 = float4(0.0, 0.0, 0.0, 0.0);
		float total = 0.0;
		for (uint k = 0; k < 4; ++k) {
			float weight = weights[k];
			if (weight == 0.0 || joints[k] >= gSkin.jointCount) {
				continue;
			}
			slt_addr matrix = ShalltearAddrAdd(gSkin.matrices, joints[k] * 64u);
			column0 += ShalltearLoadF32x4(matrix) * weight;
			column1 += ShalltearLoadF32x4(ShalltearAddrAdd(matrix, 16u)) * weight;
			column2 += ShalltearLoadF32x4(ShalltearAddrAdd(matrix, 32u)) * weight;
			column3 += ShalltearLoadF32x4(ShalltearAddrAdd(matrix, 48u)) * weight;
			total += weight;
		}

		if (total > 0.0) {
			if (total != 1.0) {
				float rescale = 1.0 / total;
				column0 *= rescale;
				column1 *= rescale;
				column2 *= rescale;
				column3 *= rescale;
			}
			float3 position = vertex.position;
			float3 normal = vertex.normal;
			float3 tangent = vertex.tangent.xyz;
			vertex.position = (column0 * position.x + column1 * position.y + column2 * position.z + column3).xyz;
			vertex.normal = SkinningNormalize(column0.xyz * normal.x + column1.xyz * normal.y + column2.xyz * normal.z);
			vertex.tangent.xyz = column0.xyz * tangent.x + column1.xyz * tangent.y + column2.xyz * tangent.z;
		}
	}

	slt_addr output = ShalltearAddrAdd(gSkin.output, index * SHALLTEAR_VERTEX_STRIDE);
	ShalltearStoreF32x4(output, float4(vertex.position, vertex.u));
	ShalltearStoreF32x4(ShalltearAddrAdd(output, 16u), float4(vertex.normal, vertex.v));
	ShalltearStoreF32x4(ShalltearAddrAdd(output, 32u), vertex.tangent);
	ShalltearStoreU32(ShalltearAddrAdd(output, 48u), vertex.color);
}
