#ifndef SHALLTEAR_ENVIRONMENT_HLSLI
#define SHALLTEAR_ENVIRONMENT_HLSLI

#include "ShalltearCommon.hlsli"
#include "Brdf.hlsli"

[[vk::binding(4, 0)]] TextureCube sltEnvironmentCube;
[[vk::binding(5, 0)]] SamplerState sltEnvironmentSampler;
// a single environment weights this 0 (it is never sampled)
[[vk::binding(9, 0)]] TextureCube sltEnvironmentCube2;

#define SHALLTEAR_ENVIRONMENT_SH_OFFSET 0
#define SHALLTEAR_ENVIRONMENT_PARAMS_OFFSET 144
#define SHALLTEAR_ENVIRONMENT_FLAT_OFFSET 160

float3 ShalltearLoadSH(slt_addr address, uint index) {
	return ShalltearLoadF32x4(ShalltearAddrAdd(address, SHALLTEAR_ENVIRONMENT_SH_OFFSET + index * 16u)).rgb;
}

// in order EvaluateBasis in EnvironmentIBL.cpp writes
// unrolled because a looped form returns 0 after first term
float3 ShalltearEvaluateSH(slt_addr address, float3 direction) {
	float3 result = ShalltearLoadSH(address, 0) * 0.282095;
	result += ShalltearLoadSH(address, 1) * (0.488603 * direction.y);
	result += ShalltearLoadSH(address, 2) * (0.488603 * direction.z);
	result += ShalltearLoadSH(address, 3) * (0.488603 * direction.x);
	result += ShalltearLoadSH(address, 4) * (1.092548 * direction.x * direction.y);
	result += ShalltearLoadSH(address, 5) * (1.092548 * direction.y * direction.z);
	result += ShalltearLoadSH(address, 6) * (0.315392 * (3.0 * direction.z * direction.z - 1.0));
	result += ShalltearLoadSH(address, 7) * (1.092548 * direction.x * direction.z);
	result += ShalltearLoadSH(address, 8) * (0.546274 * (direction.x * direction.x - direction.y * direction.y));
	return max(result, float3(0.0, 0.0, 0.0));
}

float4 ShalltearLoadEnvironmentParams(slt_addr address) {
	return ShalltearLoadF32x4(ShalltearAddrAdd(address, SHALLTEAR_ENVIRONMENT_PARAMS_OFFSET));
}

float4 ShalltearLoadEnvironmentFlat(slt_addr address) {
	return ShalltearLoadF32x4(ShalltearAddrAdd(address, SHALLTEAR_ENVIRONMENT_FLAT_OFFSET));
}

[[vk::binding(8, 0)]] TextureCubeArray sltReflectionProbes;

#define SHALLTEAR_PROBE_STRIDE 48

#define SHALLTEAR_PROBE_BLEND_FRACTION 0.15

float4 ShalltearLoadProbeField(slt_addr address, uint index, uint field) {
	return ShalltearLoadF32x4(ShalltearAddrAdd(address, index * SHALLTEAR_PROBE_STRIDE + field * 16u));
}

float3 ShalltearBoxProject(float3 reflectionWS, float3 positionWS, float3 boxMin, float3 boxMax, float3 captureWS) {
	float3 inverseDirection = 1.0 / reflectionWS;
	float3 planeMax = (boxMax - positionWS) * inverseDirection;
	float3 planeMin = (boxMin - positionWS) * inverseDirection;
	float3 forward = select(reflectionWS > 0.0, planeMax, planeMin);
	float distance = min(min(forward.x, forward.y), forward.z);
	return (positionWS + reflectionWS * distance) - captureWS;
}

float3 ShalltearSampleProbe(uint slot, float3 reflectionWS, float3 positionWS, float perceptualRoughness) {
	float4 boxMin = ShalltearLoadProbeField(sltProbeAddress, slot, 0);
	float4 boxMax = ShalltearLoadProbeField(sltProbeAddress, slot, 1);
	float4 capture = ShalltearLoadProbeField(sltProbeAddress, slot, 2);
	float3 direction = boxMin.w > 0.0
						   ? ShalltearBoxProject(reflectionWS, positionWS, boxMin.xyz, boxMax.xyz, capture.xyz)
						   : reflectionWS;
	float mip = perceptualRoughness * max(capture.w - 1.0, 0.0);
	return sltReflectionProbes.SampleLevel(sltEnvironmentSampler, float4(direction, float(slot)), mip).rgb;
}

float ShalltearProbeWeight(float3 positionWS, float3 boxMin, float3 boxMax) {
	float3 halfExtent = (boxMax - boxMin) * 0.5;
	float3 toEdge = halfExtent - abs(positionWS - (boxMin + boxMax) * 0.5);
	if (any(toEdge < 0.0)) {
		return 0.0;
	}
	float3 region = max(halfExtent * SHALLTEAR_PROBE_BLEND_FRACTION, 1e-4);
	float3 perAxis = saturate(toEdge / region);
	return min(min(perAxis.x, perAxis.y), perAxis.z);
}

float2 ShalltearEnvBRDFApprox(float perceptualRoughness, float NoV) {
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = perceptualRoughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	return float2(-1.04, 1.04) * a004 + r.zw;
}

// occlusion applies only here
float3 ShalltearIndirectDiffuse(ShalltearSurface surface, float3 normalWS, float3 positionWS) {
	float3 irradiance = ShalltearAddrValid(sltEnvironmentAddress)
							? ShalltearEvaluateSH(sltEnvironmentAddress, normalWS)
							: sltAmbientColor;
	return ShalltearDiffuseColor(surface) * irradiance * surface.occlusion;
}

float3 ShalltearIndirectSpecular(ShalltearSurface surface, float3 reflectionWS, float NoV, float3 positionWS) {
	float perceptualRoughness = max(surface.roughness, SHALLTEAR_MIN_PERCEPTUAL_ROUGHNESS);
	float3 prefiltered = sltAmbientColor;
	float intensity = 1.0;
	if (ShalltearAddrValid(sltEnvironmentAddress)) {
		float4 params = ShalltearLoadEnvironmentParams(sltEnvironmentAddress);
		float4 flat = ShalltearLoadEnvironmentFlat(sltEnvironmentAddress);
		prefiltered = flat.rgb;
		if (params.x > 0.0 && params.y > 0.0) {
			float mip = perceptualRoughness * max(params.x - 1.0, 0.0);
			prefiltered += params.y * sltEnvironmentCube.SampleLevel(sltEnvironmentSampler, reflectionWS, mip).rgb;
		}
		if (params.z > 0.0 && params.w > 0.0) {
			float mip = perceptualRoughness * max(params.z - 1.0, 0.0);
			prefiltered += params.w * sltEnvironmentCube2.SampleLevel(sltEnvironmentSampler, reflectionWS, mip).rgb;
		}
		intensity = flat.w;
	}

	uint probeCount = uint(sltProbeCount);
	if (probeCount > 0 && ShalltearAddrValid(sltProbeAddress)) {
		uint bestSlot = 0;
		uint secondSlot = 0;
		float bestImportance = -1.0;
		float secondImportance = -1.0;
		float bestWeight = 0.0;
		float secondWeight = 0.0;
		for (uint index = 0; index < probeCount; ++index) {
			float4 boxMin = ShalltearLoadProbeField(sltProbeAddress, index, 0);
			float4 boxMax = ShalltearLoadProbeField(sltProbeAddress, index, 1);
			float weight = ShalltearProbeWeight(positionWS, boxMin.xyz, boxMax.xyz);
			if (weight <= 0.0) {
				continue;
			}
			float importance = boxMax.w;
			if (importance > bestImportance) {
				secondSlot = bestSlot;
				secondImportance = bestImportance;
				secondWeight = bestWeight;
				bestSlot = index;
				bestImportance = importance;
				bestWeight = weight;
			} else if (importance > secondImportance) {
				secondSlot = index;
				secondImportance = importance;
				secondWeight = weight;
			}
		}
		if (bestImportance >= 0.0) {
			float3 probeRadiance = ShalltearSampleProbe(bestSlot, reflectionWS, positionWS, perceptualRoughness);
			if (secondImportance >= 0.0) {
				float3 other = ShalltearSampleProbe(secondSlot, reflectionWS, positionWS, perceptualRoughness);
				// 2nd probe should show only where the 1st fades
				float mix = secondImportance >= bestImportance
								? secondWeight / max(bestWeight + secondWeight, 1e-4)
								: 1.0 - bestWeight;
				probeRadiance = lerp(probeRadiance, other, mix);
			}
			prefiltered = lerp(prefiltered, probeRadiance * intensity, saturate(bestWeight + secondWeight));
		}
	}

	float2 dfg = ShalltearEnvBRDFApprox(perceptualRoughness, NoV);
	return prefiltered * (ShalltearF0(surface) * dfg.x + dfg.y) * surface.occlusion;
}

#endif
