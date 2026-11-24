// only reader of the shadow map array

#ifndef SHALLTEAR_SHADOWS_HLSLI
#define SHALLTEAR_SHADOWS_HLSLI

#include "ShalltearCommon.hlsli"
#include "CascadeSelect.hlsli"

// must match ShaderReflection.h's SHADER_SHADOW_MOMENTS_CONSTANT_ID
#define SHALLTEAR_SHADOW_MOMENTS_CONSTANT 101
[[vk::constant_id(SHALLTEAR_SHADOW_MOMENTS_CONSTANT)]] const bool gShalltearShadowMoments = false;

// only declared by a shader that shadows
// moments under gShalltearShadowMoments, depth otherwise
[[vk::binding(1, 0)]] Texture2D<float4> sltShadowAtlas;
[[vk::binding(2, 0)]] SamplerState sltShadowSampler;
[[vk::binding(3, 0)]] Texture2D<float4> sltPunctualShadowAtlas;

struct ShalltearShadowMap {
	float4 column0;
	float4 column1;
	float4 column2;
	float4 column3;
	float4 uvBounds;
	float farDepth;
	float worldTexelSize;
	float depthBias;
	float normalBias;
};

ShalltearShadowMap ShalltearLoadShadowMap(uint index) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(sltShadowMapAddress, index * SHALLTEAR_SHADOW_MAP_STRIDE);
	ShalltearShadowMap map;
	map.column0 = ShalltearLoadF32x4(base);
	map.column1 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	map.column2 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	map.column3 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 48u));
	map.uvBounds = ShalltearLoadF32x4(ShalltearAddrAdd(base, 64u));
	float4 tail = ShalltearLoadF32x4(ShalltearAddrAdd(base, 80u));
	map.farDepth = tail.x;
	map.worldTexelSize = tail.y;
	map.depthBias = tail.z;
	map.normalBias = tail.w;
	return map;
#else
	return vk::RawBufferLoad<ShalltearShadowMap>(
		sltShadowMapAddress + uint64_t(index) * SHALLTEAR_SHADOW_MAP_STRIDE, 16);
#endif
}

// steepest receiver slope as a tangent ~84 degrees
#define SHALLTEAR_SHADOW_MAX_SLOPE 10.0

// both offsets in texels of this map -> applied before projection
float3 ShalltearShadowBiasedPosition(ShalltearShadowMap map, float texelSize, float3 positionWS,
									 float3 normalWS, float3 lightDirection) {
	float NoL = saturate(dot(normalWS, -lightDirection));
	float sinTheta = sqrt(saturate(1.0 - NoL * NoL));

	float3 offset = normalWS * (texelSize * map.normalBias * sinTheta);
	offset -= lightDirection * (texelSize * map.depthBias);
	return positionWS + offset;
}

float3 ShalltearShadowProject(ShalltearShadowMap map, float3 positionWS) {
	float4 projected = map.column0 * positionWS.x + map.column1 * positionWS.y +
					   map.column2 * positionWS.z + map.column3;
	return projected.xyz / max(projected.w, 1e-6);
}

// should stop far fetches on slanted receiver reading itself as occluder
float2 ShalltearShadowReceiverSlope(ShalltearShadowMap map, float texelSize, float3 biased, float3 projected,
									float3 positionDdx, float3 positionDdy) {
	float3 acrossX = ShalltearShadowProject(map, biased + positionDdx * (texelSize / max(length(positionDdx), 1e-20))) - projected;
	float3 acrossY = ShalltearShadowProject(map, biased + positionDdy * (texelSize / max(length(positionDdy), 1e-20))) - projected;
	float3 plane = cross(acrossX, acrossY);
	return -plane.xy * ((plane.z < 0.0 ? -1.0 : 1.0) / max(abs(plane.z), 1e-30));
}

// tent's area from its left foot to x
float ShalltearShadowTentArea(float x, float halfWidth) {
	float clamped = clamp(x, -halfWidth, halfWidth);
	return 0.5 * halfWidth * halfWidth + halfWidth * clamped - 0.5 * clamped * abs(clamped);
}

// x = where fetch lands in texels, y = weight
float2 ShalltearShadowTentFetch(uint pair, uint pairs, float offset, float halfWidth) {
	float left = float(2 * pair) - float(pairs) - offset;
	float leftArea = ShalltearShadowTentArea(left, halfWidth);
	float middleArea = ShalltearShadowTentArea(left + 1.0, halfWidth);
	float rightArea = ShalltearShadowTentArea(left + 2.0, halfWidth);
	float pairArea = max(rightArea - leftArea, 1e-6);
	return float2(float(2 * pair) - float(pairs) + 0.5 + (rightArea - middleArea) / pairArea,
				  pairArea / (halfWidth * halfWidth));
}

// texels compared with plane at their centers (not at tap)
float ShalltearShadowCompare(Texture2D<float4> atlas, float2 tap, float2 uv, float reference, float2 slope) {
	float2 texel = tap / sltShadowAtlasTexel - 0.5;
	float2 lower = floor(texel);
	float2 weight = texel - lower;
	float2 corner = (lower + 1.0) * sltShadowAtlasTexel;
	float4 stored = atlas.GatherRed(sltShadowSampler, corner);
	float2 rise = 0.5 * sltShadowAtlasTexel * slope;
	float4 plane = reference + dot(corner - uv, slope) + float4(rise.y - rise.x, rise.x + rise.y, rise.x - rise.y, -rise.x - rise.y);
	// plane only where nearer light, reference elsewhere
	float4 lit = step(stored, max(plane, reference));
	return lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
}

// widths 1-7 cost 1-16 bilinear fetches
float ShalltearShadowTent(Texture2D<float4> atlas, float2 uv, float reference, float2 slope, float2 reach, float4 uvBounds) {
	uint width = (uint)max(sltShadowPcfTaps, 1.0);
	if (width <= 1) {
		return ShalltearShadowCompare(atlas, clamp(uv, uvBounds.xy, uvBounds.zw), uv, reference, slope);
	}
	float2 texel = uv / sltShadowAtlasTexel;
	float2 corner = floor(texel + 0.5);
	float2 offset = texel - corner;
	uint pairs = (width + 1) / 2;
	float halfWidth = 0.5 * float(width);
	float sum = 0.0;
	for (uint y = 0; y < pairs; ++y) {
		float2 row = ShalltearShadowTentFetch(y, pairs, offset.y, halfWidth);
		for (uint x = 0; x < pairs; ++x) {
			float2 column = ShalltearShadowTentFetch(x, pairs, offset.x, halfWidth);
			float2 tap = clamp(uv + ((corner + float2(column.x, row.x)) * sltShadowAtlasTexel - uv) * reach, uvBounds.xy, uvBounds.zw);
			sum += column.y * row.y * ShalltearShadowCompare(atlas, tap, uv, reference, slope);
		}
	}
	return sum;
}

float ShalltearSampleShadowMap(Texture2D<float4> atlas, uint index, float texelSize, float3 positionWS,
							   float3 positionDdx, float3 positionDdy, float3 normalWS, float3 lightDirection, bool tileOnly = false) {
	ShalltearShadowMap map = ShalltearLoadShadowMap(index);
	float3 biased = ShalltearShadowBiasedPosition(map, texelSize, positionWS, normalWS, lightDirection);
	float3 projected = ShalltearShadowProject(map, biased);
	if (projected.z <= 0.0 || projected.z >= 1.0) {
		return 1.0;
	}
	if (tileOnly && (any(projected.xy < map.uvBounds.xy) || any(projected.xy > map.uvBounds.zw))) {
		return 1.0;
	}
	float2 slope = ShalltearShadowReceiverSlope(map, texelSize, biased, projected, positionDdx, positionDdy);
	float towardLight = abs(ShalltearShadowProject(map, biased - lightDirection * texelSize).z - projected.z);
	float2 reach = min(1.0, SHALLTEAR_SHADOW_MAX_SLOPE * towardLight / (sltShadowAtlasTexel * max(abs(slope), 1e-30)));
	return ShalltearShadowTent(atlas, projected.xy, projected.z, slope, reach, map.uvBounds);
}

float ShalltearMomentBound(float2 moments, float warped, float minimumVariance) {
	float variance = max(moments.y - moments.x * moments.x, minimumVariance);
	float gap = warped - moments.x;
	float bound = variance / (variance + gap * gap);
	float bleed = sltShadowMoments.z;
	return warped <= moments.x ? 1.0 : saturate((bound - bleed) / max(1.0 - bleed, 1e-4));
}

float4 ShalltearLoadMoments(float2 uv) {
	float2 texel = uv / sltShadowAtlasTexel - 0.5;
	int2 corner = int2(floor(texel));
	float2 weight = texel - float2(corner);
	int last = int(1.0 / sltShadowAtlasTexel + 0.5) - 1;
	int2 next = min(corner + 1, int2(last, last));
	float4 top = lerp(sltShadowAtlas.Load(int3(corner, 0)), sltShadowAtlas.Load(int3(next.x, corner.y, 0)), weight.x);
	float4 bottom = lerp(sltShadowAtlas.Load(int3(corner.x, next.y, 0)), sltShadowAtlas.Load(int3(next, 0)), weight.x);
	return lerp(top, bottom, weight.y);
}

float ShalltearSampleCascadeMoments(uint index, float3 positionWS, float3 normalWS, float3 lightDirection) {
	ShalltearShadowMap map = ShalltearLoadShadowMap(index);
	float3 biased = ShalltearShadowBiasedPosition(map, map.worldTexelSize, positionWS, normalWS, lightDirection);
	float3 projected = ShalltearShadowProject(map, biased);
	if (projected.z <= 0.0 || projected.z >= 1.0) {
		return 1.0;
	}
	float4 moments = ShalltearLoadMoments(clamp(projected.xy, map.uvBounds.xy, map.uvBounds.zw));
	// must match MomentResolve.hlsl
	float depth = 1.0 - 2.0 * projected.z;
	float2 warped = float2(exp(sltShadowMoments.x * depth), -exp(-sltShadowMoments.y * depth));
	float2 scale = 1e-4 * sltShadowMoments.xy * warped;
	return min(ShalltearMomentBound(moments.xy, warped.x, scale.x * scale.x), ShalltearMomentBound(moments.zw, warped.y, scale.y * scale.y));
}

float ShalltearSampleCascade(uint index, float3 positionWS, float3 positionDdx, float3 positionDdy,
							 float3 normalWS, float3 lightDirection) {
	if (gShalltearShadowMoments) {
		return ShalltearSampleCascadeMoments(index, positionWS, normalWS, lightDirection);
	}
	float texel = ShalltearLoadShadowMap(index).worldTexelSize;
	return ShalltearSampleShadowMap(sltShadowAtlas, index, texel, positionWS, positionDdx, positionDdy, normalWS, lightDirection);
}

#define SHALLTEAR_CASCADE_BLEND_FRACTION 0.1

float ShalltearObjectShadow(uint index, float3 positionWS, float3 positionDdx, float3 positionDdy,
							float3 normalWS, float3 lightDirection) {
	float texel = ShalltearLoadShadowMap(index).worldTexelSize;
	return ShalltearSampleShadowMap(sltPunctualShadowAtlas, index, texel, positionWS, positionDdx, positionDdy, normalWS,
									lightDirection, true);
}

// must match shadowCount packing in RenderSystem.cpp
float ShalltearDirectionalShadow(uint firstMap, uint packed, float3 positionWS, float3 positionDdx, float3 positionDdy,
								 float3 normalWS, float3 lightDirection, float viewDepth) {
	uint count = packed & 0xFFu;
	if (!ShalltearAddrValid(sltShadowMapAddress) || count == 0) {
		return 1.0;
	}
	uint cascade = ShalltearCascadeIndexOf(firstMap, count, viewDepth);
	if (cascade >= count) {
		return 1.0;
	}

	float shadow = ShalltearSampleCascade(firstMap + cascade, positionWS, positionDdx, positionDdy, normalWS, lightDirection);

	float cascadeFar = ShalltearCascadeFar(firstMap + cascade);
	float cascadeNear = cascade == 0 ? 0.0 : ShalltearCascadeFar(firstMap + cascade - 1);
	float band = (cascadeFar - cascadeNear) * SHALLTEAR_CASCADE_BLEND_FRACTION;
	if (cascade + 1 < count && band > 0.0 && viewDepth > cascadeFar - band) {
		float blend = saturate((viewDepth - (cascadeFar - band)) / band);
		shadow = lerp(shadow, ShalltearSampleCascade(firstMap + cascade + 1, positionWS, positionDdx, positionDdy, normalWS, lightDirection), blend);
	}

	uint objectFirst = packed >> 16;
	uint objectCount = (packed >> 8) & 0xFFu;
	[loop] for (uint i = 0; i < objectCount; ++i) {
		shadow *= ShalltearObjectShadow(objectFirst + i, positionWS, positionDdx, positionDdy, normalWS, lightDirection);
	}

	float fadeRange = max(sltShadowFadeEnd - sltShadowFadeStart, 1e-4);
	float fade = saturate((sltShadowFadeEnd - viewDepth) / fadeRange);
	return lerp(1.0, shadow, fade);
}

uint ShalltearPointFaceOf(float3 fromLight) {
	float3 a = abs(fromLight);
	if (a.x >= a.y && a.x >= a.z) {
		return fromLight.x >= 0.0 ? 0 : 1;
	}
	if (a.y >= a.z) {
		return fromLight.y >= 0.0 ? 2 : 3;
	}
	return fromLight.z >= 0.0 ? 4 : 5;
}

float ShalltearPunctualShadow(uint firstMap, uint count, float3 lightPosition, float3 positionWS,
							  float3 positionDdx, float3 positionDdy, float3 normalWS) {
	if (!ShalltearAddrValid(sltShadowMapAddress) || count == 0) {
		return 1.0;
	}
	float3 toSurface = positionWS - lightPosition;
	float distanceToLight = max(length(toSurface), 1e-4);
	float3 fromLight = toSurface / distanceToLight;
	uint face = count > 1 ? ShalltearPointFaceOf(fromLight) : 0;

	// scales with receiver distance
	uint index = firstMap + min(face, count - 1);
	float texel = ShalltearLoadShadowMap(index).worldTexelSize * distanceToLight;
	return ShalltearSampleShadowMap(sltPunctualShadowAtlas, index, texel, positionWS, positionDdx, positionDdy, normalWS, fromLight);
}

#endif
