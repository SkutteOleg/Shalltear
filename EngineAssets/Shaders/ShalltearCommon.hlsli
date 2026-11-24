// shaders should never declare vertex input attributes because everything is pulled

#ifndef SHALLTEAR_COMMON_HLSLI
#define SHALLTEAR_COMMON_HLSLI

#include "ShalltearBufferAccess.hlsli"

struct ShalltearVertex {
	float3 position;
	float u;
	float3 normal;
	float v;
	float4 tangent;
	uint color;
};
#define SHALLTEAR_VERTEX_STRIDE 52

ShalltearVertex ShalltearLoadVertex(slt_addr vertexAddress, uint index) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(vertexAddress, index * SHALLTEAR_VERTEX_STRIDE);
	float4 a = ShalltearLoadF32x4(base);
	float4 b = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	float4 c = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	uint col = ShalltearLoadU32(ShalltearAddrAdd(base, 48u));
	ShalltearVertex vertex;
	vertex.position = a.xyz;
	vertex.u = a.w;
	vertex.normal = b.xyz;
	vertex.v = b.w;
	vertex.tangent = c;
	vertex.color = col;
	return vertex;
#else
	return vk::RawBufferLoad<ShalltearVertex>(vertexAddress + uint64_t(index) * SHALLTEAR_VERTEX_STRIDE, 4);
#endif
}

float4 ShalltearUnpackColor(uint packed) {
	return float4(
		float(packed & 0xFF) / 255.0,
		float((packed >> 8) & 0xFF) / 255.0,
		float((packed >> 16) & 0xFF) / 255.0,
		float((packed >> 24) & 0xFF) / 255.0);
}

// push block has only address of first instance's element
struct ShalltearDrawHeader {
	slt_addr drawAddress;
	uint value;
	uint reserved;
};
#define SHALLTEAR_PER_DRAW_STRIDE 128

struct ShalltearPerDraw {
	float4 world0;
	float4 world1;
	float4 world2;
	float4 world3;
	float4 normalMatrix0;
	float4 normalMatrix1;
	float4 normalMatrix2;
	slt_addr vertexAddress;
	float alphaClipThreshold;
	// bit 0 receives shadows, bits 1-3 carry blend, bits 8-15 rendering layers
	uint flags;
};
#define SHALLTEAR_DRAW_RECEIVES_SHADOWS 1u
#define SHALLTEAR_DRAW_LAYERS_SHIFT 8u
// values are RHI::BlendMode's which RenderSystem.cpp asserts
#define SHALLTEAR_DRAW_BLEND_SHIFT 1u
#define SHALLTEAR_DRAW_BLEND_MASK 7u
#define SHALLTEAR_BLEND_OPAQUE 0u
#define SHALLTEAR_BLEND_ALPHA 1u
#define SHALLTEAR_BLEND_ADDITIVE 2u
#define SHALLTEAR_BLEND_PREMULTIPLIED 3u
#define SHALLTEAR_BLEND_MULTIPLY 4u
// bits 16-23 fade amount, bit 24 set for complementary pixels
#define SHALLTEAR_DRAW_FADE_SHIFT 16u
#define SHALLTEAR_DRAW_FADE_ABOVE 0x1000000u
// must match ShaderReflection.h's SHADER_LOD_FADE_CONSTANT_ID
#define SHALLTEAR_LOD_FADE_CONSTANT 100
[[vk::constant_id(SHALLTEAR_LOD_FADE_CONSTANT)]] const bool gShalltearLODFade = false;

// passing block by value miscompiles in DXC
ShalltearPerDraw ShalltearLoadPerDraw(slt_addr drawAddress) {
#ifdef SHALLTEAR_WEBGPU
	ShalltearPerDraw draw;
	draw.world0 = ShalltearLoadF32x4(drawAddress);
	draw.world1 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 16u));
	draw.world2 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 32u));
	draw.world3 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 48u));
	draw.normalMatrix0 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 64u));
	draw.normalMatrix1 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 80u));
	draw.normalMatrix2 = ShalltearLoadF32x4(ShalltearAddrAdd(drawAddress, 96u));
	uint4 tail = ShalltearLoadU32x4(ShalltearAddrAdd(drawAddress, 112u));
	draw.vertexAddress = ShalltearAddrFromWords(tail.x, tail.y);
	draw.alphaClipThreshold = asfloat(tail.z);
	draw.flags = tail.w;
	return draw;
#else
	return vk::RawBufferLoad<ShalltearPerDraw>(drawAddress, 16);
#endif
}

slt_addr ShalltearInstanceDrawAddress(slt_addr drawAddress, uint instance) {
	return ShalltearAddrAdd(drawAddress, instance * SHALLTEAR_PER_DRAW_STRIDE);
}

#define SHALLTEAR_ALPHA_CLIP_OFFSET 120
float ShalltearLoadAlphaClipThreshold(slt_addr drawAddress) {
	return ShalltearLoadF32(ShalltearAddrAdd(drawAddress, SHALLTEAR_ALPHA_CLIP_OFFSET));
}

#define SHALLTEAR_DRAW_FLAGS_OFFSET 124
uint ShalltearLoadDrawFlags(slt_addr drawAddress) {
	return ShalltearLoadU32(ShalltearAddrAdd(drawAddress, SHALLTEAR_DRAW_FLAGS_OFFSET));
}

void ShalltearClipLODFade(uint flags, float2 pixel) {
	uint fade = (flags >> SHALLTEAR_DRAW_FADE_SHIFT) & 0xFFu;
	if (!gShalltearLODFade || fade == 0u) {
		return;
	}
	static const float BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	uint2 cell = uint2(pixel) & 3u;
	float threshold = (BAYER[cell.y * 4u + cell.x] + 0.5) / 16.0;
	float keep = float(255u - fade) / 255.0;
	clip((flags & SHALLTEAR_DRAW_FADE_ABOVE) != 0u ? threshold - (1.0 - keep) : keep - threshold);
}

uint ShalltearLoadDrawBlend(slt_addr drawAddress) {
	return (ShalltearLoadDrawFlags(drawAddress) >> SHALLTEAR_DRAW_BLEND_SHIFT) & SHALLTEAR_DRAW_BLEND_MASK;
}

float4 ShalltearTransformPosition(ShalltearPerDraw draw, float3 position) {
	return draw.world0 * position.x + draw.world1 * position.y + draw.world2 * position.z + draw.world3;
}

// sign of the world matrix determinant applied before bitangent
float ShalltearDrawHandedness(ShalltearPerDraw draw) {
	return dot(cross(draw.world0.xyz, draw.world1.xyz), draw.world2.xyz) < 0.0 ? -1.0 : 1.0;
}

float3 ShalltearTransformNormal(float4 column0, float4 column1, float4 column2, float3 normal) {
	float3x3 normalMatrix = float3x3(column0.xyz, column1.xyz, column2.xyz);
	float3 transformed = mul(normal, normalMatrix);
	// normalize gives NaN for 0 length tangents
	float lengthSquared = dot(transformed, transformed);
	return lengthSquared > 1e-20 ? transformed * rsqrt(lengthSquared) : transformed;
}

#include "ShalltearGlobals.hlsli"

// per view/bound once per view from upload ring
[[vk::binding(0, 0)]] cbuffer ShalltearView {
	float4x4 sltViewProjection;
	float3 sltCameraPosition;
	float sltDirectionalLightCount;
	float3 sltAmbientColor;
	float sltPunctualLightCount;
	slt_addr sltLightAddress;
	slt_addr sltClusterAddress;
	float4 sltViewForwardRow;
	uint3 sltClusterDims;
	uint sltClusterPad;
	float2 sltClusterTileScale; // pixels2tile
	float sltClusterSliceScale;
	float sltClusterSliceBias;
	slt_addr sltShadowMapAddress;
	float sltShadowCascadeCount;
	float sltShadowAtlasTexel;
	float sltShadowPcfTaps;
	float sltShadowFadeStart;
	float sltShadowFadeEnd;
	float sltTransparentReceivesShadows;
	slt_addr sltEnvironmentAddress;
	// SV_Position = pixel center. No 1/2 texel offset should be here
	float2 sltRenderExtentInverse;
	slt_addr sltProbeAddress;
	float sltProbeCount;
	float sltFogLayerCount;
	slt_addr sltViewDataAddress;
	slt_addr sltDecalAddress;
	// positive exponent, negative exponent, bleed reduction
	float4 sltShadowMoments;
};

#include "ViewData.hlsli"

// 0 address reads 0, not losing the device
float4 ShalltearLoadViewFloat4(uint offset) {
	if (!ShalltearAddrValid(sltViewDataAddress)) {
		return float4(0.0, 0.0, 0.0, 0.0);
	}
	return ShalltearLoadF32x4(ShalltearAddrAdd(sltViewDataAddress, offset));
}

float4x4 ShalltearLoadViewMatrix(uint offset) {
	return ShalltearViewDataMatrix(ShalltearLoadViewFloat4(offset), ShalltearLoadViewFloat4(offset + 16), ShalltearLoadViewFloat4(offset + 32),
								   ShalltearLoadViewFloat4(offset + 48));
}

float4x4 ShalltearViewMatrix() { return ShalltearLoadViewMatrix(0); }
float4x4 ShalltearInverseViewMatrix() { return ShalltearLoadViewMatrix(64); }
float4x4 ShalltearProjectionMatrix() { return ShalltearLoadViewMatrix(128); }
float4x4 ShalltearInverseProjectionMatrix() { return ShalltearLoadViewMatrix(192); }
float4x4 ShalltearInverseViewProjectionMatrix() { return ShalltearLoadViewMatrix(256); }
float4 ShalltearTimeParameters() { return ShalltearLoadViewFloat4(SHALLTEAR_VIEW_DATA_TIME); }
float4 ShalltearProjectionParameters() { return ShalltearLoadViewFloat4(SHALLTEAR_VIEW_DATA_PROJECTION_PARAMS); }
float4 ShalltearScreenParameters() { return ShalltearLoadViewFloat4(SHALLTEAR_VIEW_DATA_SCREEN_PARAMS); }

float ShalltearGetExposure() {
	if (!ShalltearAddrValid(sltViewDataAddress)) {
		return 1.0;
	}
	return ShalltearLoadExposure(ShalltearLoadAddr(ShalltearAddrAdd(sltViewDataAddress, SHALLTEAR_VIEW_DATA_EXPOSURE)));
}

#define SHALLTEAR_HALF_MAX 65504.0

// writers clamp to finite 1/2 range after fog
float3 ShalltearExpose(float3 radiance) {
	return min(radiance * ShalltearGetExposure(), SHALLTEAR_HALF_MAX);
}

float4 ShalltearExpose(float4 color, uint blend) {
	if (blend == SHALLTEAR_BLEND_MULTIPLY) {
		return color;
	}
	return float4(ShalltearExpose(color.rgb), color.a);
}

// orthographic rays are parallel. This is camera's backward axis
float3 ShalltearViewDirection(float3 positionWS) {
	if (ShalltearProjectionParameters().z != 0.0) {
		return -normalize(ShalltearViewMatrix()[2].xyz);
	}
	return normalize(sltCameraPosition - positionWS);
}

[[vk::binding(6, 0)]] Texture2D<float> sltAmbientOcclusion;
[[vk::binding(7, 0)]] SamplerState sltAmbientOcclusionSampler;

float ShalltearScreenSpaceOcclusion(float2 positionSS) {
	return sltAmbientOcclusion.SampleLevel(sltAmbientOcclusionSampler, positionSS * sltRenderExtentInverse, 0);
}

#endif
