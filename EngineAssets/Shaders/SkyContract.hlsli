#ifndef SHALLTEAR_SKY_CONTRACT_HLSLI
#define SHALLTEAR_SKY_CONTRACT_HLSLI

#include "ShalltearCommon.hlsli"
#include "SunDisc.hlsli"

struct ShalltearSkyPush {
	float4x4 invViewProjection;
	float3 sunDirection;
	float sunCosSize;
	float3 sunColor;
	float skyIntensity;
	float sunAngularDiameter;	// rads
	uint capturing;
	float weight;
};
SLT_PUSH_CONSTANT(ShalltearSkyPush, gSky)

struct ShalltearSkyInput {
	float3 directionWS;
	float3 cameraPositionWS;
	float2 screenUV;
	float4 time;
	// points toward the light
	float3 sunDirectionWS;
	float3 sunColor;
	// rads
	float sunAngularDiameter;
	bool sunEnabled;
	bool capturing;
};

struct ShalltearSkyOutput {
	// linear radiance before Sky Intensity
	float3 color;
};

ShalltearSkyOutput ShalltearDefaultSky() {
	ShalltearSkyOutput sky;
	sky.color = float3(0.0, 0.0, 0.0);
	return sky;
}

struct ShalltearSkyVaryings {
	float4 positionCS : SV_Position;
	float2 ndc : TEXCOORD0;
};

ShalltearSkyVaryings ShalltearSkyVertex(uint vertexId) {
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	ShalltearSkyVaryings output;
	output.ndc = uv * 2.0 - 1.0;
	output.positionCS = float4(output.ndc, 0.0, 1.0);
	return output;
}

float3 ShalltearSkyRay(float2 ndc) {
	float4 farPoint = mul(gSky.invViewProjection, float4(ndc, 0.0, 1.0));
	return normalize(farPoint.xyz / farPoint.w - sltCameraPosition);
}

float3 ShalltearSkySunDisc(float3 directionWS) {
	if (gSky.capturing != 0) {
		return float3(0.0, 0.0, 0.0);
	}
	return ShalltearSunDisc(directionWS, gSky.sunDirection, gSky.sunColor, gSky.sunCosSize);
}

float3 ShalltearSkySunDisc(ShalltearSkyInput input) {
	return ShalltearSkySunDisc(input.directionWS);
}

#endif
