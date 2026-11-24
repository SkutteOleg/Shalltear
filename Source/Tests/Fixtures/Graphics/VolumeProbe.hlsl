// SAMPLES THE VOLUME ITS ROW FILLED, and paints the whole view with what it
// found. The point of the fixture is that it reaches the lattice by NAME --
// `sltPassVolume` in set 2, exactly as `sltSceneColor` and `sltPassInput0`
// are reached -- so a readback of one texel proves declare, create, upload
// and bind at once.
//
// The push block carries the lookup point rather than deriving it from the
// fragment, so what the test asserts is a texel the CPU can name

#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"

[[vk::binding(0, 2)]] Texture3D sltPassVolume;
[[vk::binding(1, 2)]] SamplerState sltVolumeSampler;

struct ProbePush {
	// xyz: where in the volume to look, in normalized [0, 1] coordinates
	float4 coord;
};
SLT_PUSH_CONSTANT(ProbePush, gProbe)

struct VSOutput {
	float4 positionCS : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	VSOutput output;
	float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
	output.positionCS = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target {
	return sltPassVolume.SampleLevel(sltVolumeSampler, gProbe.coord.xyz, 0);
}
