#pragma shalltear_blend opaque
#pragma shalltear_depth test_only
#pragma shalltear_cull off
#pragma shalltear_fill wireframe
#pragma shalltear_depth_bias 1.0 2.0

#include "Color.hlsli"
#include "ShalltearCommon.hlsli"

struct ShalltearWirePush {
	ShalltearDrawHeader draw;
	uint color; // SRGB RGBA8, Color32 layout
};
SLT_PUSH_CONSTANT(ShalltearWirePush, gWire)

struct VSOutput {
	float4 positionCS : SV_Position;
	float4 color : COLOR0;
};

// HLSL 2021 rejects the ternary on vectors
float ShalltearWireSrgbToLinearChannel(float c) {
	if (gShalltearGammaColorSpace) {
		return c;
	}
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

VSOutput VSMain(uint vertexId : SV_VertexID) {
	ShalltearPerDraw draw = ShalltearLoadPerDraw(gWire.draw.drawAddress);
	ShalltearVertex vertex = ShalltearLoadVertex(draw.vertexAddress, vertexId);
	float4 worldPosition = ShalltearTransformPosition(draw, vertex.position);

	float4 srgb = ShalltearUnpackColor(gWire.color);

	VSOutput output;
	output.positionCS = mul(sltViewProjection, worldPosition);
	output.color = float4(ShalltearWireSrgbToLinearChannel(srgb.r),
						  ShalltearWireSrgbToLinearChannel(srgb.g),
						  ShalltearWireSrgbToLinearChannel(srgb.b),
						  srgb.a);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target {
	return input.color;
}
