#pragma shalltear_blend additive
#pragma shalltear_depth off
#pragma shalltear_cull off
#pragma shalltear_default flareTexture white

#include "ShalltearBufferAccess.hlsli"

[[vk::binding(0, 1)]] Texture2D flareTexture;
[[vk::binding(1, 1)]] SamplerState flareSampler;

[[vk::binding(0, 2)]] Texture2D<float> sltSceneDepth;
[[vk::binding(1, 2)]] SamplerState sltSceneSampler;

// must match LensFlarePush in LensFlarePass.cpp
struct ShalltearLensFlarePush {
	slt_addr base;
	uint quadOffset;
	float invAspect;
};

SLT_PUSH_CONSTANT(ShalltearLensFlarePush, gFlare)

// must match FlareSource in LensFlarePass.cpp
struct ShalltearFlareSource {
	// xy = NDC, z = depth, w = +1 for reverse Z
	float4 screenDepth;
	float2 radius;
	uint samples;
	uint offScreenVisible;
};

// must match FlareQuad in LensFlarePass.cpp
struct ShalltearFlareQuad {
	float4 centerHalf;
	float4 color;
	float4 shape;
	float rotation;
	uint source;
	uint kindFlags;
	uint gradient;
};

#define SHALLTEAR_FLARE_IMAGE 0
#define SHALLTEAR_FLARE_CIRCLE 1
#define SHALLTEAR_FLARE_POLYGON 2
#define SHALLTEAR_FLARE_RING 3

#define SHALLTEAR_FLARE_INVERSE 0x100u
#define SHALLTEAR_FLARE_ANGULAR 0x200u
#define SHALLTEAR_FLARE_PREMULTIPLIED 0x400u

#define SHALLTEAR_FLARE_GRADIENT_STEPS 16

static const float SHALLTEAR_FLARE_PI = 3.14159265358979323846;
static const float SHALLTEAR_FLARE_GOLDEN_ANGLE = 2.39996322972865332;

static const float2 SHALLTEAR_FLARE_CORNERS[6] = {
	float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
	float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)};

struct VSOutput {
	float4 positionCS : SV_Position;
	float2 local : TEXCOORD0;
	float4 color : TEXCOORD1;
	nointerpolation float4 shape : TEXCOORD2;
	nointerpolation uint kindFlags : TEXCOORD3;
	nointerpolation uint gradient : TEXCOORD4;
};

// must match LensFlareOps::OcclusionSample
float2 ShalltearFlareSample(uint index, uint count) {
	float radius = sqrt((float(index) + 0.5) / float(max(count, 1u)));
	float angle = float(index) * SHALLTEAR_FLARE_GOLDEN_ANGLE;
	return radius * float2(cos(angle), sin(angle));
}

float ShalltearFlareVisibility(ShalltearFlareSource source) {
	if (source.samples == 0) {
		return 1.0;
	}
	uint width, height;
	sltSceneDepth.GetDimensions(width, height);
	float visible = 0.0;
	for (uint i = 0; i < source.samples; ++i) {
		float2 ndc = source.screenDepth.xy + ShalltearFlareSample(i, source.samples) * source.radius;
		float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
		if (any(uv < 0.0) || any(uv >= 1.0)) {
			visible += source.offScreenVisible != 0 ? 1.0 : 0.0;
			continue;
		}
		float depth = sltSceneDepth.Load(int3(int2(uv * float2(width, height)), 0));
		if ((depth - source.screenDepth.z) * source.screenDepth.w <= 0.0) {
			visible += 1.0;
		}
	}
	return visible / float(source.samples);
}

ShalltearFlareQuad ShalltearLoadFlareQuad(uint instanceId) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(gFlare.base, gFlare.quadOffset + instanceId * 64u);
	ShalltearFlareQuad q;
	q.centerHalf = ShalltearLoadF32x4(base);
	q.color = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	q.shape = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	uint4 tail = ShalltearLoadU32x4(ShalltearAddrAdd(base, 48u));
	q.rotation = asfloat(tail.x);
	q.source = tail.y;
	q.kindFlags = tail.z;
	q.gradient = tail.w;
	return q;
#else
	return vk::RawBufferLoad<ShalltearFlareQuad>(gFlare.base + gFlare.quadOffset + uint64_t(instanceId) * 64, 16);
#endif
}

ShalltearFlareSource ShalltearLoadFlareSource(uint sourceIndex) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(gFlare.base, sourceIndex * 32u);
	ShalltearFlareSource s;
	s.screenDepth = ShalltearLoadF32x4(base);
	uint4 tail = ShalltearLoadU32x4(ShalltearAddrAdd(base, 16u));
	s.radius = float2(asfloat(tail.x), asfloat(tail.y));
	s.samples = tail.z;
	s.offScreenVisible = tail.w;
	return s;
#else
	return vk::RawBufferLoad<ShalltearFlareSource>(gFlare.base + uint64_t(sourceIndex) * 32, 16);
#endif
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
	ShalltearFlareQuad quad = ShalltearLoadFlareQuad(instanceId);
	ShalltearFlareSource source = ShalltearLoadFlareSource(quad.source);
	float visibility = ShalltearFlareVisibility(source);

	float2 local = SHALLTEAR_FLARE_CORNERS[vertexId];
	float2 extent = local * quad.centerHalf.zw;
	float c = cos(quad.rotation);
	float s = sin(quad.rotation);
	float2 turned = float2(extent.x * c + extent.y * s, extent.y * c - extent.x * s);
	turned.x *= gFlare.invAspect;

	VSOutput output;
	output.positionCS = visibility > 0.0 ? float4(quad.centerHalf.xy + turned, 0.0, 1.0) : float4(-2.0, -2.0, 0.0, 1.0);
	output.local = local;
	output.color = float4(quad.color.rgb, quad.color.a * visibility);
	output.shape = quad.shape;
	output.kindFlags = quad.kindFlags;
	output.gradient = quad.gradient;
	return output;
}

float4 ShalltearFlareGradient(uint offset, float t) {
	float x = saturate(t) * float(SHALLTEAR_FLARE_GRADIENT_STEPS - 1);
	uint i = min(uint(x), uint(SHALLTEAR_FLARE_GRADIENT_STEPS - 2));
	float4 a = ShalltearLoadF32x4(ShalltearAddrAdd(gFlare.base, offset + i * 16u));
	float4 b = ShalltearLoadF32x4(ShalltearAddrAdd(gFlare.base, offset + (i + 1u) * 16u));
	return lerp(a, b, x - float(i));
}

float ShalltearFlareShape(uint kind, uint flags, float2 p, float4 shape) {
	float sdf;
	float depth;
	if (kind == SHALLTEAR_FLARE_POLYGON) {
		float sector = 2.0 * SHALLTEAR_FLARE_PI / shape.z;
		float b = sector * floor(atan2(p.y, p.x) / sector + 0.5);
		float edge = cos(b) * p.x + sin(b) * p.y;
		float apothem = cos(SHALLTEAR_FLARE_PI / shape.z);
		sdf = lerp(edge - apothem, length(p) - 1.0, shape.w);
		depth = lerp(apothem, 1.0, shape.w);
	} else if (kind == SHALLTEAR_FLARE_RING) {
		float band = shape.z * 0.5;
		sdf = abs(length(p) - (1.0 - band)) - band;
		depth = band;
	} else {
		sdf = length(p) - 1.0;
		depth = 1.0;
	}
	float aa = max(fwidth(sdf), 1.0e-5);
	float coverage = saturate(0.5 - sdf / aa);
	float profile = saturate(-sdf / max(shape.x * depth, aa));
	float value = (flags & SHALLTEAR_FLARE_INVERSE) != 0 ? coverage * (1.0 - profile) : profile;
	return pow(value, shape.y);
}

float4 PSMain(VSOutput input) : SV_Target {
	uint kind = input.kindFlags & 0xFFu;
	float3 rgb = input.color.rgb;
	float alpha = input.color.a;
	float shapeAlpha;
	if (kind == SHALLTEAR_FLARE_IMAGE) {
		float4 texel = flareTexture.Sample(flareSampler, float2(input.local.x * 0.5 + 0.5, 0.5 - input.local.y * 0.5));
		rgb *= texel.rgb;
		shapeAlpha = texel.a;
	} else {
		shapeAlpha = ShalltearFlareShape(kind, input.kindFlags, input.local, input.shape);
	}
	if (input.gradient != 0) {
		float t = (input.kindFlags & SHALLTEAR_FLARE_ANGULAR) != 0 ? atan2(input.local.y, input.local.x) / (2.0 * SHALLTEAR_FLARE_PI) + 0.5
																	: length(input.local);
		float4 tone = ShalltearFlareGradient(input.gradient, t);
		rgb *= tone.rgb;
		alpha *= tone.a;
	}
	if ((input.kindFlags & SHALLTEAR_FLARE_PREMULTIPLIED) != 0) {
		return float4(rgb * alpha * (kind == SHALLTEAR_FLARE_IMAGE ? 1.0 : shapeAlpha), alpha * shapeAlpha);
	}
	return float4(rgb, alpha * shapeAlpha);
}
