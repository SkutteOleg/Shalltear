#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"

struct ShalltearPrefilterPush {
	uint face;
	float roughness;
	float sourceFaceSize;
	float sourceMipCount;
	float2 targetSize;
};
SLT_PUSH_CONSTANT(ShalltearPrefilterPush, gPrefilter)

[[vk::binding(0, 0)]] TextureCube sltPrefilterSource;
[[vk::binding(1, 0)]] SamplerState sltPrefilterSampler;

static const float PI = 3.14159265358979323846;
static const uint PREFILTER_SAMPLES = 64;

struct VSOutput {
	float4 positionCS : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	VSOutput output;
	output.positionCS = float4(uv * 2.0 - 1.0, 0.0, 1.0);
	return output;
}

float3 CubeTexelDirection(uint face, float u, float v) {
	float s = 2.0 * u - 1.0;
	float t = 2.0 * v - 1.0;
	float3 direction;
	switch (face) {
	case 0: direction = float3(1.0, -t, -s); break;
	case 1: direction = float3(-1.0, -t, s); break;
	case 2: direction = float3(s, 1.0, t); break;
	case 3: direction = float3(s, -1.0, -t); break;
	case 4: direction = float3(s, -t, 1.0); break;
	default: direction = float3(-s, -t, -1.0); break;
	}
	return normalize(direction);
}

float2 Hammersley(uint i, uint count) {
	uint bits = reversebits(i);
	return float2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

float3 ImportanceSampleGGX(float2 xi, float linearRoughness, float3 normal) {
	float a = linearRoughness;
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float3 halfTangent = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
	float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 tangentX = normalize(cross(up, normal));
	float3 tangentY = cross(normal, tangentX);
	return normalize(tangentX * halfTangent.x + tangentY * halfTangent.y + normal * halfTangent.z);
}

float DistributionGGX(float NoH, float linearRoughness) {
	float a2 = linearRoughness * linearRoughness;
	float denominator = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * denominator * denominator, 1e-8);
}

float4 PSMain(VSOutput input) : SV_Target {
	float2 uv = input.positionCS.xy / gPrefilter.targetSize;
	float3 normal = CubeTexelDirection(gPrefilter.face, uv.x, uv.y);
	if (gPrefilter.roughness <= 0.0) {
		return float4(sltPrefilterSource.SampleLevel(sltPrefilterSampler, normal, 0.0).rgb, 1.0);
	}

	float linearRoughness = max(gPrefilter.roughness * gPrefilter.roughness, 1e-3);
	float texelSolidAngle = 4.0 * PI / (6.0 * gPrefilter.sourceFaceSize * gPrefilter.sourceFaceSize);
	float3 sum = float3(0.0, 0.0, 0.0);
	float weight = 0.0;
	for (uint s = 0; s < PREFILTER_SAMPLES; ++s) {
		float3 halfVector = ImportanceSampleGGX(Hammersley(s, PREFILTER_SAMPLES), linearRoughness, normal);
		float VoH = dot(normal, halfVector);
		float3 light = halfVector * (2.0 * VoH) - normal;
		float NoL = dot(normal, light);
		if (NoL <= 0.0) {
			continue;
		}
		float pdf = DistributionGGX(max(VoH, 0.0), linearRoughness) * 0.25;
		float sampleSolidAngle = 1.0 / (float(PREFILTER_SAMPLES) * pdf + 1e-4);
		float sampleMip = 0.5 * log2(sampleSolidAngle / texelSolidAngle);
		// CPU oracle never blends 2 mips
		float readMip = floor(clamp(sampleMip, 0.0, gPrefilter.sourceMipCount - 1.0));
		sum += sltPrefilterSource.SampleLevel(sltPrefilterSampler, light, readMip).rgb * NoL;
		weight += NoL;
	}
	float3 color = weight > 0.0 ? sum / weight : sltPrefilterSource.SampleLevel(sltPrefilterSampler, normal, 0.0).rgb;
	return float4(color, 1.0);
}
