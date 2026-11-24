
#ifndef SHALLTEAR_LIGHTS_HLSLI
#define SHALLTEAR_LIGHTS_HLSLI

#include "ShalltearCommon.hlsli"
#include "Brdf.hlsli"
#include "TextureTable.hlsli"

struct ShalltearLight {
	float3 position;
	float invRangeSquared;
	float3 color;
	float spotScale;
	float3 direction;
	float spotOffset;
	uint shadowIndex;
	uint shadowCount;
	float shadowStrength;
	// bits 0-7 layers, 8-15 flags, 16-31 extension slot
	uint info;
};
#define SHALLTEAR_LIGHT_STRIDE 64
#define SHALLTEAR_LIGHT_FLAG_COOKIE 0x100u
#define SHALLTEAR_LIGHT_FLAG_EVERY_LAYER 0x200u

ShalltearLight ShalltearLoadLight(uint index) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(sltLightAddress, index * SHALLTEAR_LIGHT_STRIDE);
	float4 w0 = ShalltearLoadF32x4(base);
	float4 w1 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	float4 w2 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	uint4 w3 = ShalltearLoadU32x4(ShalltearAddrAdd(base, 48u));
	ShalltearLight light;
	light.position = w0.xyz;
	light.invRangeSquared = w0.w;
	light.color = w1.xyz;
	light.spotScale = w1.w;
	light.direction = w2.xyz;
	light.spotOffset = w2.w;
	light.shadowIndex = w3.x;
	light.shadowCount = w3.y;
	light.shadowStrength = asfloat(w3.z);
	light.info = w3.w;
	return light;
#else
	return vk::RawBufferLoad<ShalltearLight>(sltLightAddress + uint64_t(index) * SHALLTEAR_LIGHT_STRIDE, 16);
#endif
}

bool ShalltearLightReaches(ShalltearLight light, uint drawLayers) {
	return (light.info & SHALLTEAR_LIGHT_FLAG_EVERY_LAYER) != 0 || (light.info & drawLayers & 0xFFu) != 0;
}

struct ShalltearLightCookie {
	float4 row0;
	float4 row1;
	float4 row2;
	uint entry;
	uint projection;
	uint pad0;
	uint pad1;
};
#define SHALLTEAR_COOKIE_ORTHOGRAPHIC 0u
#define SHALLTEAR_COOKIE_PERSPECTIVE 1u

float3 ShalltearLightCookieColor(ShalltearLight light, float3 positionWS, float3 positionDdx, float3 positionDdy) {
	if ((light.info & SHALLTEAR_LIGHT_FLAG_COOKIE) == 0) {
		return float3(1.0, 1.0, 1.0);
	}
	ShalltearLightCookie cookie;
#ifdef SHALLTEAR_WEBGPU
	{
		slt_addr base = ShalltearAddrAdd(sltLightAddress, (light.info >> 16) * SHALLTEAR_LIGHT_STRIDE);
		cookie.row0 = ShalltearLoadF32x4(base);
		cookie.row1 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
		cookie.row2 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
		uint4 tail = ShalltearLoadU32x4(ShalltearAddrAdd(base, 48u));
		cookie.entry = tail.x;
		cookie.projection = tail.y;
		cookie.pad0 = tail.z;
		cookie.pad1 = tail.w;
	}
#else
	cookie = vk::RawBufferLoad<ShalltearLightCookie>(sltLightAddress + uint64_t(light.info >> 16) * SHALLTEAR_LIGHT_STRIDE, 16);
#endif
	float4 position = float4(positionWS, 1.0);
	float3 h = float3(dot(cookie.row0, position), dot(cookie.row1, position), dot(cookie.row2, position));
	float3 hDdx = float3(dot(cookie.row0.xyz, positionDdx), dot(cookie.row1.xyz, positionDdx), dot(cookie.row2.xyz, positionDdx));
	float3 hDdy = float3(dot(cookie.row0.xyz, positionDdy), dot(cookie.row1.xyz, positionDdy), dot(cookie.row2.xyz, positionDdy));
	if (cookie.projection == SHALLTEAR_COOKIE_ORTHOGRAPHIC) {
		return ShalltearTableSample2D(cookie.entry, h.xy, hDdx.xy, hDdy.xy).rgb;
	}
	if (cookie.projection == SHALLTEAR_COOKIE_PERSPECTIVE) {
		float2 uv = h.xy / max(h.z, 1e-6);
		float2 uvDdx = (h.xy + hDdx.xy) / max(h.z + hDdx.z, 1e-6) - uv;
		float2 uvDdy = (h.xy + hDdy.xy) / max(h.z + hDdy.z, 1e-6) - uv;
		return ShalltearTableSample2D(cookie.entry, saturate(uv), uvDdx, uvDdy).rgb;
	}
	return ShalltearTableSampleCube(cookie.entry, h, hDdx, hDdy).rgb;
}

ShalltearLightSample ShalltearSampleDirectional(ShalltearLight light) {
	ShalltearLightSample sample;
	sample.direction = -light.direction;
	sample.color = light.color;
	sample.attenuation = 1.0;
	sample.shadow = 1.0; // integrator folds light's maps after
	return sample;
}

// attenuation 0. Not a branch so walk should stay uniform
ShalltearLightSample ShalltearSamplePunctual(ShalltearLight light, float3 positionWS) {
	float3 toSurface = positionWS - light.position;
	float distanceSquared = dot(toSurface, toSurface);
	float3 fromLight = toSurface * rsqrt(max(distanceSquared, 1e-10));

	float ratio = distanceSquared * light.invRangeSquared;
	float window = saturate(1.0 - ratio * ratio);
	float attenuation = window * window / max(distanceSquared, 1e-4);

	float coneFactor = dot(fromLight, light.direction);
	attenuation *= saturate(coneFactor * light.spotScale + light.spotOffset);

	ShalltearLightSample sample;
	sample.direction = -fromLight;
	sample.color = light.color;
	sample.attenuation = attenuation;
	sample.shadow = 1.0;
	return sample;
}

// must agree with LightCulling.cpp (which bins same point on CPU)
uint ShalltearClusterOf(float2 positionSS, float viewZ) {
	uint2 tile = min(uint2(positionSS * sltClusterTileScale), sltClusterDims.xy - 1);
	int slice = int(log2(max(viewZ, 1e-4)) * sltClusterSliceScale + sltClusterSliceBias);
	uint z = uint(clamp(slice, 0, int(sltClusterDims.z) - 1));
	return (z * sltClusterDims.y + tile.y) * sltClusterDims.x + tile.x;
}

// entry.y = light count in bits 0-7, decal count in 8-15
uint2 ShalltearClusterEntry(uint cluster) {
	return ShalltearLoadU32x2(ShalltearAddrAdd(sltClusterAddress, cluster * 8u));
}

// flat list begins after the table
uint ShalltearClusterLight(uint2 entry, uint slot) {
	slt_addr indices = ShalltearAddrAdd(sltClusterAddress, (sltClusterDims.x * sltClusterDims.y * sltClusterDims.z) * 8u);
	return ShalltearLoadU32(ShalltearAddrAdd(indices, (entry.x + slot) * 4u));
}

float ShalltearViewDepth(float3 positionWS) {
	return dot(sltViewForwardRow, float4(positionWS, 1.0));
}

#endif
