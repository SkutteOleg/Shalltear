// vertices pulled through a buffer device address

#pragma shalltear_blend alpha
#pragma shalltear_depth test_only
#pragma shalltear_cull off
#pragma shalltear_topology line

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

struct ShalltearDebugLinePush {
	float4x4 viewProjection;
	slt_addr vertexAddress;
};
SLT_PUSH_CONSTANT(ShalltearDebugLinePush, gDebugLines)

#define SHALLTEAR_DEBUG_VERTEX_STRIDE 16

struct VSOutput {
	float4 positionCS : SV_Position;
	float4 color : COLOR0;
};

// HLSL 2021 rejects the ternary on vectors
float ShalltearSrgbToLinearChannel(float c) {
	if (gShalltearGammaColorSpace) {
		return c;
	}
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

VSOutput VSMain(uint vertexId : SV_VertexID) {
	slt_addr address = ShalltearAddrAdd(gDebugLines.vertexAddress, vertexId * SHALLTEAR_DEBUG_VERTEX_STRIDE);
	float3 position = ShalltearLoadF32x3(address);
	uint packed = ShalltearLoadU32(ShalltearAddrAdd(address, 12u));

	float4 srgb = float4(
		float(packed & 0xFF) / 255.0,
		float((packed >> 8) & 0xFF) / 255.0,
		float((packed >> 16) & 0xFF) / 255.0,
		float((packed >> 24) & 0xFF) / 255.0);

	VSOutput output;
	output.positionCS = mul(gDebugLines.viewProjection, float4(position, 1.0));
	output.color = float4(ShalltearSrgbToLinearChannel(srgb.r),
						  ShalltearSrgbToLinearChannel(srgb.g),
						  ShalltearSrgbToLinearChannel(srgb.b),
						  srgb.a);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target {
	return input.color;
}
