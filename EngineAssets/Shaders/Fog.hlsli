// each layer's fog evaluated in its mode -> composited

#ifndef SHALLTEAR_FOG_HLSLI
#define SHALLTEAR_FOG_HLSLI

#include "ShalltearCommon.hlsli"

#define SHALLTEAR_FOG_OFFSET 176
#define SHALLTEAR_FOG_STRIDE 32
#define SHALLTEAR_FOG_LINEAR 0.0
#define SHALLTEAR_FOG_EXPONENTIAL 1.0

struct ShalltearFog {
	float3 color;
	float amount;
};

float ShalltearFogDistance(float3 positionWS) {
	return max(dot(sltCameraPosition - positionWS, ShalltearViewDirection(positionWS)), 0.0);
}

float ShalltearFogVisibility(float4 params, float distance) {
	if (params.x == SHALLTEAR_FOG_LINEAR) {
		return saturate(distance * params.y + params.z);
	}
	float depth = params.y * distance;
	return params.x == SHALLTEAR_FOG_EXPONENTIAL ? exp(-depth) : exp(-depth * depth);
}

ShalltearFog ShalltearEvaluateFog(float3 positionWS) {
	ShalltearFog fog;
	fog.color = float3(0.0, 0.0, 0.0);
	fog.amount = 0.0;
	uint count = uint(sltFogLayerCount);
	if (count == 0 || !ShalltearAddrValid(sltEnvironmentAddress)) {
		return fog;
	}
	float distance = ShalltearFogDistance(positionWS);
	for (uint index = 0; index < count; ++index) {
		slt_addr layer = ShalltearAddrAdd(sltEnvironmentAddress, SHALLTEAR_FOG_OFFSET + index * SHALLTEAR_FOG_STRIDE);
		float4 colorShare = ShalltearLoadF32x4(layer);
		float4 params = ShalltearLoadF32x4(ShalltearAddrAdd(layer, 16u));
		float taken = colorShare.a * (1.0 - ShalltearFogVisibility(params, distance));
		fog.color += colorShare.rgb * taken;
		fog.amount += taken;
	}
	return fog;
}

// only surface's own contribution should take the fog in front
float4 ShalltearApplyFog(float4 color, float3 positionWS, uint blend) {
	ShalltearFog fog = ShalltearEvaluateFog(positionWS);
	float3 kept = color.rgb * (1.0 - fog.amount);
	if (blend == SHALLTEAR_BLEND_ADDITIVE) {
		color.rgb = kept;
	} else if (blend == SHALLTEAR_BLEND_PREMULTIPLIED) {
		color.rgb = kept + fog.color * color.a;
	} else if (blend == SHALLTEAR_BLEND_MULTIPLY) {
		color.rgb = kept + fog.amount;
	} else {
		color.rgb = kept + fog.color;
	}
	return color;
}

float4 ShalltearApplyFog(float4 color, float3 positionWS) {
	return ShalltearApplyFog(color, positionWS, SHALLTEAR_BLEND_OPAQUE);
}

#endif
