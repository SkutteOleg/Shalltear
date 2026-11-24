#ifndef SHALLTEAR_DECALS_HLSLI
#define SHALLTEAR_DECALS_HLSLI

#include "Surface.hlsli"
#include "Lights.hlsli"
#include "TextureTable.hlsli"

struct ShalltearDecal {
	float4 row0;
	float4 row1;
	float4 row2;
	float4 uvScaleBias;
	float4 baseColor;
	float3 emissive;
	float normalScale;
	uint baseColorEntry;
	uint normalEntry;
	uint maskEntry;
	uint emissiveEntry;
	float angleFadeScale;
	float angleFadeBias;
	// bits 0-7 layers, bit 8 all layers
	uint layers;
	uint pad0;
};
#define SHALLTEAR_DECAL_STRIDE 128
#define SHALLTEAR_DECAL_EVERY_LAYER 0x100u

ShalltearDecal ShalltearLoadDecal(uint index) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(sltDecalAddress, index * SHALLTEAR_DECAL_STRIDE);
	ShalltearDecal decal;
	decal.row0 = ShalltearLoadF32x4(base);
	decal.row1 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	decal.row2 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	decal.uvScaleBias = ShalltearLoadF32x4(ShalltearAddrAdd(base, 48u));
	decal.baseColor = ShalltearLoadF32x4(ShalltearAddrAdd(base, 64u));
	float4 w5 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 80u));
	decal.emissive = w5.xyz;
	decal.normalScale = w5.w;
	uint4 w6 = ShalltearLoadU32x4(ShalltearAddrAdd(base, 96u));
	decal.baseColorEntry = w6.x;
	decal.normalEntry = w6.y;
	decal.maskEntry = w6.z;
	decal.emissiveEntry = w6.w;
	uint4 w7 = ShalltearLoadU32x4(ShalltearAddrAdd(base, 112u));
	decal.angleFadeScale = asfloat(w7.x);
	decal.angleFadeBias = asfloat(w7.y);
	decal.layers = w7.z;
	decal.pad0 = w7.w;
	return decal;
#else
	return vk::RawBufferLoad<ShalltearDecal>(sltDecalAddress + uint64_t(index) * SHALLTEAR_DECAL_STRIDE, 16);
#endif
}

void ShalltearApplyDecals(ShalltearSurfaceInput input, inout ShalltearSurface surface, inout float3 normalWS, uint drawLayers,
						  float3 positionDdx, float3 positionDdy) {
	if (!ShalltearAddrValid(sltDecalAddress)) {
		return;
	}
	uint2 entry = ShalltearClusterEntry(ShalltearClusterOf(input.positionSS, ShalltearViewDepth(input.positionWS)));
	uint lightCount = entry.y & 0xFFu;
	uint decalCount = (entry.y >> 8) & 0xFFu;
	float4 position = float4(input.positionWS, 1.0);
	for (uint slot = 0; slot < decalCount; ++slot) {
		uint index = ShalltearClusterLight(entry, lightCount + slot);
		ShalltearDecal decal = ShalltearLoadDecal(index);
		if ((decal.layers & SHALLTEAR_DECAL_EVERY_LAYER) == 0 && (decal.layers & drawLayers & 0xFFu) == 0) {
			continue;
		}
		float3 local = float3(dot(decal.row0, position), dot(decal.row1, position), dot(decal.row2, position));
		if (any(abs(local) > 0.5)) {
			continue;
		}
		float3 right = normalize(decal.row0.xyz);
		float3 up = normalize(decal.row1.xyz);
		float3 back = -normalize(decal.row2.xyz);
		float angle = acos(clamp(dot(input.normalWS, back), -1.0, 1.0));
		float coverage = saturate(angle * decal.angleFadeScale + decal.angleFadeBias);

		float2 uv = float2(local.x + 0.5, 0.5 - local.y) * decal.uvScaleBias.xy + decal.uvScaleBias.zw;
		float2 uvDdx = float2(dot(decal.row0.xyz, positionDdx), -dot(decal.row1.xyz, positionDdx)) * decal.uvScaleBias.xy;
		float2 uvDdy = float2(dot(decal.row0.xyz, positionDdy), -dot(decal.row1.xyz, positionDdy)) * decal.uvScaleBias.xy;

		float4 base = decal.baseColor;
		if (decal.baseColorEntry != SHALLTEAR_TABLE_NONE) {
			base *= ShalltearTableSample2D(decal.baseColorEntry, uv, uvDdx, uvDdy);
		}
		float alpha = saturate(base.a * coverage);
		surface.baseColor = lerp(surface.baseColor, base.rgb, alpha);
		if (decal.maskEntry != SHALLTEAR_TABLE_NONE) {
			float3 mask = ShalltearTableSample2D(decal.maskEntry, uv, uvDdx, uvDdy).rgb;
			surface.occlusion = lerp(surface.occlusion, mask.r, alpha);
			surface.roughness = lerp(surface.roughness, mask.g, alpha);
			surface.metallic = lerp(surface.metallic, mask.b, alpha);
		}
		if (decal.normalEntry != SHALLTEAR_TABLE_NONE) {
			float3 tangentNormal = ShalltearUnpackNormalMap(ShalltearTableSample2D(decal.normalEntry, uv, uvDdx, uvDdy), decal.normalScale);
			float3 decalNormal = normalize(tangentNormal.x * right + tangentNormal.y * up + tangentNormal.z * back);
			normalWS = normalize(lerp(normalWS, decalNormal, alpha));
		}
		float3 emission = decal.emissive;
		if (decal.emissiveEntry != SHALLTEAR_TABLE_NONE) {
			emission *= ShalltearTableSample2D(decal.emissiveEntry, uv, uvDdx, uvDdy).rgb;
		}
		surface.emissive += emission * alpha;
	}
}

#endif
