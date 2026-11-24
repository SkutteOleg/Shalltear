#pragma shalltear_blend alpha
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

[[vk::binding(0, 1)]] Texture2D<uint> sltOutlineMask;
[[vk::binding(1, 1)]] SamplerState sltOutlineMaskSampler;
[[vk::binding(2, 1)]] Texture2D<float> sltOutlineDepth;
[[vk::binding(3, 1)]] SamplerState sltOutlineDepthSampler;
[[vk::binding(4, 1)]] Texture2D<float> sltSceneDepth;
[[vk::binding(5, 1)]] SamplerState sltSceneDepthSampler;

// must match GPUOutlinePush in RenderSystem.cpp
struct ShalltearOutlinePush {
	uint4 colors;
	float width;
	float hiddenOpacity;
	uint pad0;
	uint pad1;
};
SLT_PUSH_CONSTANT(ShalltearOutlinePush, gOutline)

float ShalltearOutlineSrgbToLinear(float c) {
	if (gShalltearGammaColorSpace) {
		return c;
	}
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
	float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 positionCS : SV_Position) : SV_Target {
	int2 pixel = int2(positionCS.xy);
	uint width;
	uint height;
	sltOutlineMask.GetDimensions(width, height);
	int2 last = int2(width, height) - 1;

	if (sltOutlineMask.Load(int3(clamp(pixel, 0, last), 0)) != 0) {
		discard;
	}

	int reach = int(ceil(gOutline.width));
	uint found = 0;
	float foundDistance = 1e9;
	int2 foundAt = pixel;
	for (int y = -reach; y <= reach; ++y) {
		for (int x = -reach; x <= reach; ++x) {
			float distance = length(float2(x, y));
			if (distance > gOutline.width) {
				continue;
			}
			int2 at = clamp(pixel + int2(x, y), 0, last);
			uint value = sltOutlineMask.Load(int3(at, 0));
			if (value == 0) {
				continue;
			}
			if (found == 0 || value < found || (value == found && distance < foundDistance)) {
				found = value;
				foundDistance = distance;
				foundAt = at;
			}
		}
	}
	if (found == 0 || found > 4) {
		discard;
	}

	// reverse Z. Covered surface should have the smaller depth
	float objectDepth = sltOutlineDepth.Load(int3(foundAt, 0));
	float sceneDepth = sltSceneDepth.Load(int3(foundAt, 0));
	bool hidden = objectDepth < sceneDepth * 0.9999;

	uint packed = gOutline.colors[found - 1];
	float4 srgb = float4(float(packed & 0xFF), float((packed >> 8) & 0xFF), float((packed >> 16) & 0xFF),
						 float((packed >> 24) & 0xFF)) / 255.0;
	float4 color = float4(ShalltearOutlineSrgbToLinear(srgb.r), ShalltearOutlineSrgbToLinear(srgb.g),
						  ShalltearOutlineSrgbToLinear(srgb.b), srgb.a);
	color.a *= hidden ? gOutline.hiddenOpacity : 1.0;
	return color;
}
