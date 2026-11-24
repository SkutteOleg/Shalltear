// the target is UNORM (display encoding is current pass's job)

#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

[[vk::binding(0, 1)]] Texture2D sltSceneColor;
[[vk::binding(1, 1)]] SamplerState sltSceneSampler;

// mirrored by GPUPresentPush in RenderSystem.cpp
struct ShalltearPresentPush {
	uint tonemap;
};

SLT_PUSH_CONSTANT(ShalltearPresentPush, gPresent)

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

float4 PSMain(VSOutput input) : SV_Target {
	float3 scene = sltSceneColor.Sample(sltSceneSampler, input.uv).rgb;
	scene = ShalltearTonemap(gPresent.tonemap, scene);
	return float4(ShalltearSceneToSrgb(scene), 1.0);
}
