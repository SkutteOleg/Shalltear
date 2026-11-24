// runs inside the chain -> overlays stay on screen

#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"
#include "CascadeSelect.hlsli"

// must match DebugView in RenderSystem.h
#define SHALLTEAR_DEBUG_VIEW_NORMALS 1
#define SHALLTEAR_DEBUG_VIEW_DEPTH 2
#define SHALLTEAR_DEBUG_VIEW_CASCADES 3

[[vk::binding(0, 1)]] Texture2D sltSource;
[[vk::binding(1, 1)]] SamplerState sltSourceSampler;

// must match GPUDebugViewPush in RenderSystem.cpp
struct ShalltearDebugViewPush {
	uint mode;
	uint orthographic;
	float2 depthLinearize;
	// Core should avoid holding appearance values
	float4 cascadeTint[4];
};

SLT_PUSH_CONSTANT(ShalltearDebugViewPush, gDebug)

struct VSOutput {
	float4 positionCS : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	VSOutput output;
	float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
	output.positionCS = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	output.uv = float2(corner.x, 1.0 - corner.y);
	return output;
}

float ShalltearDebugViewDepth(float storedZ) {
	if (gDebug.orthographic != 0) {
		return storedZ * gDebug.depthLinearize.x + gDebug.depthLinearize.y;
	}
	return gDebug.depthLinearize.x / max(storedZ + gDebug.depthLinearize.y, 1.0e-6);
}

float4 PSMain(VSOutput input) : SV_Target {
	float4 source = sltSource.Sample(sltSourceSampler, input.uv);
	float3 display = float3(0.0, 0.0, 0.0);

	if (gDebug.mode == SHALLTEAR_DEBUG_VIEW_DEPTH) {
		display = source.rrr;
	} else if (gDebug.mode == SHALLTEAR_DEBUG_VIEW_CASCADES) {
		uint count = (uint)sltShadowCascadeCount;
		if (count == 0) {
			display = float3(0.0, 0.0, 0.0);
		} else {
			uint cascade = ShalltearCascadeOf(ShalltearDebugViewDepth(source.r));
			display = cascade < count ? gDebug.cascadeTint[cascade].rgb : float3(0.12, 0.12, 0.12);
		}
	} else {
		display = source.rgb;
	}

	return float4(ShalltearSrgbToScene(display), 1.0);
}
