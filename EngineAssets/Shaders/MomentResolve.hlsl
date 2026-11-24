#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"

// must match GPUMomentResolvePush in RenderSystem.cpp
struct ShalltearMomentResolvePush {
	int2 tileOrigin;
	int2 tileSize;
	float positiveExponent;
	float negativeExponent;
	int radius;
	uint samples;
};
SLT_PUSH_CONSTANT(ShalltearMomentResolvePush, gResolve)

[[vk::binding(0, 1)]] Texture2DMS<float> sltMomentDepth;
[[vk::binding(1, 1)]] SamplerState sltMomentSampler;

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
	float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 positionCS : SV_Position) : SV_Target {
	int2 texel = int2(positionCS.xy) - gResolve.tileOrigin;
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	for (int y = -gResolve.radius; y <= gResolve.radius; ++y) {
		for (int x = -gResolve.radius; x <= gResolve.radius; ++x) {
			int2 at = clamp(texel + int2(x, y), int2(0, 0), gResolve.tileSize - 1);
			for (uint s = 0; s < gResolve.samples; ++s) {
				// reverse Z (1 near) mapped to -1 near, +1 far
				float depth = 1.0 - 2.0 * sltMomentDepth.Load(at, s);
				float positive = exp(gResolve.positiveExponent * depth);
				float negative = -exp(-gResolve.negativeExponent * depth);
				sum += float4(positive, positive * positive, negative, negative * negative);
			}
		}
	}
	int side = 2 * gResolve.radius + 1;
	return sum / float(side * side * max(gResolve.samples, 1u));
}
