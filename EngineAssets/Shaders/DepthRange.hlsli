#ifndef SHALLTEAR_DEPTH_RANGE_HLSLI
#define SHALLTEAR_DEPTH_RANGE_HLSLI

#include "ShalltearBufferAccess.hlsli"

// must match GPUDepthRangePush in RenderSystem.cpp
struct ShalltearDepthRangePush {
	uint resultOffset;
	float a;
	float b;
	uint width;
	uint height;
	uint orthographic;
};
SLT_PUSH_CONSTANT(ShalltearDepthRangePush, gRange)

#ifdef SHALLTEAR_DEPTH_RANGE_MULTISAMPLED
[[vk::binding(0, 0)]] Texture2DMS<float> gDepth;
#else
[[vk::binding(0, 0)]] Texture2D<float> gDepth;
#endif
[[vk::binding(1, 0)]] RWByteAddressBuffer gResult;

#define SHALLTEAR_DEPTH_RANGE_SIDE 16u
#define SHALLTEAR_DEPTH_RANGE_THREADS (SHALLTEAR_DEPTH_RANGE_SIDE * SHALLTEAR_DEPTH_RANGE_SIDE)

groupshared float gNearest[SHALLTEAR_DEPTH_RANGE_THREADS];
groupshared float gFarthest[SHALLTEAR_DEPTH_RANGE_THREADS];

float ShalltearRangeLoad(uint2 pixel) {
#ifdef SHALLTEAR_DEPTH_RANGE_MULTISAMPLED
	return gDepth.Load(int2(pixel), 0);
#else
	return gDepth.Load(int3(pixel, 0));
#endif
}

float ShalltearRangeViewDepth(float device) {
	return gRange.orthographic != 0u ? (device - gRange.b) / gRange.a : gRange.b / (device - gRange.a);
}

[numthreads(SHALLTEAR_DEPTH_RANGE_SIDE, SHALLTEAR_DEPTH_RANGE_SIDE, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
	float nearest = 3.0e38;
	float farthest = 0.0;
	for (uint y = 0u; y < 4u; y += 2u) {
		for (uint x = 0u; x < 4u; x += 2u) {
			const uint2 pixel = id.xy * 4u + uint2(x, y);
			if (pixel.x < gRange.width && pixel.y < gRange.height) {
				const float device = ShalltearRangeLoad(pixel);
				if (device > 0.0) {
					const float depth = ShalltearRangeViewDepth(device);
					nearest = min(nearest, depth);
					farthest = max(farthest, depth);
				}
			}
		}
	}
	gNearest[index] = nearest;
	gFarthest[index] = farthest;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = SHALLTEAR_DEPTH_RANGE_THREADS / 2u; stride > 0u; stride >>= 1) {
		if (index < stride) {
			gNearest[index] = min(gNearest[index], gNearest[index + stride]);
			gFarthest[index] = max(gFarthest[index], gFarthest[index + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0u && gFarthest[0] > 0.0) {
		uint ignored;
		gResult.InterlockedMin(gRange.resultOffset, asuint(gNearest[0]), ignored);
		gResult.InterlockedMax(gRange.resultOffset + 4u, asuint(gFarthest[0]), ignored);
	}
}

#endif
