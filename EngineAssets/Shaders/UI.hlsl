// colors stay SRGB display encoded

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

#pragma shalltear_blend alpha
#pragma shalltear_depth off
#pragma shalltear_cull off

struct UIQuad {
	float4 rect;
	float4 uv;
	float4 color0;	 // SRGB, straight alpha
	float4 color1;
	float4 radii;
	float4 clipRect;
	float4 clipRadii;
	float softness;
	uint flags;
	float rotation; // rads clockwise about center (FLAG_ROTATED)
	float pad0;
};
#define UI_QUAD_STRIDE 128

#define UI_FLAG_GLYPH 1u
#define UI_FLAG_DITHER 2u
#define UI_FLAG_GRADIENT_VERTICAL 4u
#define UI_FLAG_SDF_FROM_UV 8u
#define UI_FLAG_PREMULTIPLIED 16u
#define UI_FLAG_RING 32u
#define UI_FLAG_ROTATED 64u
#define UI_FLAG_RADIAL_FILL 128u
#define UI_FLAG_SUBPIXEL 256u

#define UI_SUBPIXEL_MARGIN_PX 1.0

#define UI_TEXTURE_DECODES 1u

#define UI_PASS_COLOR 0u
#define UI_PASS_COVERAGE 1u
#define UI_PASS_BLUR_X 2u
#define UI_PASS_BLUR_Y 3u

struct UIPush {
	float2 viewportSize;
	float2 originPx;
	slt_addr quadAddress;
	uint textureFlags;
	uint passMode;
};
SLT_PUSH_CONSTANT(UIPush, gPush)

UIQuad ShalltearLoadUIQuad(uint quadIndex) {
#ifdef SHALLTEAR_WEBGPU
	slt_addr base = ShalltearAddrAdd(gPush.quadAddress, quadIndex * UI_QUAD_STRIDE);
	UIQuad q;
	q.rect = ShalltearLoadF32x4(base);
	q.uv = ShalltearLoadF32x4(ShalltearAddrAdd(base, 16u));
	q.color0 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 32u));
	q.color1 = ShalltearLoadF32x4(ShalltearAddrAdd(base, 48u));
	q.radii = ShalltearLoadF32x4(ShalltearAddrAdd(base, 64u));
	q.clipRect = ShalltearLoadF32x4(ShalltearAddrAdd(base, 80u));
	q.clipRadii = ShalltearLoadF32x4(ShalltearAddrAdd(base, 96u));
	uint4 tail = ShalltearLoadU32x4(ShalltearAddrAdd(base, 112u));
	q.softness = asfloat(tail.x);
	q.flags = tail.y;
	q.rotation = asfloat(tail.z);
	q.pad0 = asfloat(tail.w);
	return q;
#else
	return vk::RawBufferLoad<UIQuad>(gPush.quadAddress + uint64_t(quadIndex) * UI_QUAD_STRIDE, 4);
#endif
}

[[vk::binding(1, 1)]] Texture2D sltUITexture;
[[vk::binding(2, 1)]] SamplerState sltUISampler;

struct VSOutput {
	float4 positionCS : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	nointerpolation float4 rect : TEXCOORD1;
	nointerpolation float4 sdfBox : TEXCOORD2;
	nointerpolation float4 radii : TEXCOORD3;
	nointerpolation float4 clipRect : TEXCOORD4;
	nointerpolation float4 clipRadii : TEXCOORD5;
	nointerpolation float softness : TEXCOORD6;
	nointerpolation uint flags : TEXCOORD7;
	nointerpolation float4 innerBox : TEXCOORD8;
	nointerpolation float4 innerRadii : TEXCOORD9;
	nointerpolation float2 cosSin : TEXCOORD10;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	uint quadIndex = vertexId / 6u;
	uint cornerIndex = vertexId - quadIndex * 6u;

	UIQuad quad = ShalltearLoadUIQuad(quadIndex);

	float2 corner = float2(0.0, 0.0);
	if (cornerIndex == 1u) {
		corner = float2(1.0, 0.0);
	} else if (cornerIndex == 2u || cornerIndex == 4u) {
		corner = float2(1.0, 1.0);
	} else if (cornerIndex == 5u) {
		corner = float2(0.0, 1.0);
	}
	if (cornerIndex == 3u) {
		corner = float2(0.0, 0.0);
	}
	if ((quad.flags & UI_FLAG_SUBPIXEL) != 0u) {
		corner += (corner * 2.0 - 1.0) * (UI_SUBPIXEL_MARGIN_PX / max(quad.rect.zw, 1e-4));
	}

	float2 cosSin = float2(1.0, 0.0);
	float2 positionPx = quad.rect.xy + corner * quad.rect.zw;
	if ((quad.flags & UI_FLAG_ROTATED) != 0u) {
		sincos(quad.rotation, cosSin.y, cosSin.x);
		float2 centre = quad.rect.xy + quad.rect.zw * 0.5;
		float2 offset = (corner - 0.5) * quad.rect.zw;
		positionPx = centre + float2(offset.x * cosSin.x - offset.y * cosSin.y,
									 offset.x * cosSin.y + offset.y * cosSin.x);
	}

	VSOutput output;
	float2 local = positionPx - gPush.originPx;
	output.positionCS = float4(
		2.0 * local.x / gPush.viewportSize.x - 1.0,
		1.0 - 2.0 * local.y / gPush.viewportSize.y,
		0.0, 1.0);
	output.uv = quad.uv.xy + corner * quad.uv.zw;
	float4 rampEnd = ((quad.flags & (UI_FLAG_RING | UI_FLAG_RADIAL_FILL)) != 0u) ? quad.color0 : quad.color1;
	output.color = ((quad.flags & UI_FLAG_GRADIENT_VERTICAL) != 0u)
					   ? lerp(quad.color0, rampEnd, corner.y)
					   : lerp(quad.color0, rampEnd, corner.x);
	output.rect = quad.rect;
	output.sdfBox = ((quad.flags & UI_FLAG_SDF_FROM_UV) != 0u) ? quad.uv : quad.rect;
	output.radii = quad.radii;
	output.clipRect = quad.clipRect;
	output.clipRadii = quad.clipRadii;
	output.softness = quad.softness;
	output.flags = quad.flags;
	output.innerBox = quad.uv;
	output.innerRadii = quad.color1;
	output.cosSin = cosSin;
	return output;
}

float4 ClampRadii(float4 radii, float2 size) {
	float w = max(size.x, 0.0);
	float h = max(size.y, 0.0);
	float top = radii.x + radii.y;
	float right = radii.y + radii.z;
	float bottom = radii.z + radii.w;
	float left = radii.w + radii.x;
	float scale = min(min(top > 0.0 ? w / top : 1.0, bottom > 0.0 ? w / bottom : 1.0),
					  min(right > 0.0 ? h / right : 1.0, left > 0.0 ? h / left : 1.0));
	return radii * min(scale, 1.0);
}

float RoundedBoxSDF(float2 p, float4 box, float4 radii, bool turned, float2 cosSin, out float support) {
	radii = ClampRadii(radii, box.zw);
	float2 center = box.xy + box.zw * 0.5;
	float2 fromCenter = p - center;
	float radius = (fromCenter.x <= 0.0)
					   ? ((fromCenter.y <= 0.0) ? radii.x : radii.w)
					   : ((fromCenter.y <= 0.0) ? radii.y : radii.z);
	float2 q = abs(fromCenter) - box.zw * 0.5 + radius;
	float2 outward = max(q, 0.0);
	float radial = length(outward);
	if (turned) {
		float2 screen = outward * sign(fromCenter);
		screen = float2(screen.x * cosSin.x - screen.y * cosSin.y,
						screen.x * cosSin.y + screen.y * cosSin.x);
		support = (radial > 0.0) ? (abs(screen.x) + abs(screen.y)) / radial : 1.0;
	} else {
		support = (radial > 0.0) ? (outward.x + outward.y) / radial : 1.0;
	}
	return min(max(q.x, q.y), 0.0) + radial - radius;
}

float RoundedBoxSDF(float2 p, float4 box, float4 radii) {
	float ignored;
	return RoundedBoxSDF(p, box, radii, false, float2(1.0, 0.0), ignored);
}

float BoxCoverage(float dist, float support) {
	float half = 0.5 * support;
	if (dist <= -half) {
		return 1.0;
	}
	if (dist >= half) {
		return 0.0;
	}
	float spread = sqrt(max(2.0 - support * support, 0.0));
	float hi = 0.5 * (support + spread);
	float lo = support - hi;
	if (abs(dist) <= 0.5 * spread) {
		return 0.5 - dist / hi;
	}
	float tail = half - abs(dist);
	float area = tail * tail / max(2.0 * hi * lo, 1e-6);
	return dist > 0.0 ? area : 1.0 - area;
}

float Coverage(float dist, float support, float softness) {
	if (softness > 0.0) {
		float halfWidth = 0.5 * (1.0 + softness);
		return 1.0 - smoothstep(-halfWidth, halfWidth, dist);
	}
	return BoxCoverage(dist, max(support, 1.0));
}

float ClippedCoverage(float dist, float support, float clipDist, float clipSupport,
					  float clipMask, float softness) {
	if (softness > 0.0) {
		return Coverage(dist, support, softness) * clipMask;
	}
	bool shapeWins = dist >= clipDist;
	return Coverage(shapeWins ? dist : clipDist, shapeWins ? support : clipSupport, 0.0);
}

// wedge = center xy, start rads, signed sweep rads (clockwise positive, +y down)
float RadialFillCoverage(float2 p, float4 wedge) {
	const float tau = 6.28318530718;
	float sweep = abs(wedge.w);
	if (sweep >= tau) {
		return 1.0;
	}
	float2 v = p - wedge.xy;
	float rel = (wedge.w >= 0.0 ? atan2(v.y, v.x) - wedge.z : wedge.z - atan2(v.y, v.x));
	rel -= tau * floor(rel / tau);
	float angular = rel <= sweep ? min(rel, sweep - rel) : -min(rel - sweep, tau - rel);
	float px = length(v) * sin(clamp(angular, -1.5707963, 1.5707963));
	return saturate(px + 0.5);
}

float BoxOverlap(float2 p, float4 box, float4 clip) {
	float2 lo = max(max(p - 0.5, box.xy), clip.xy);
	float2 hi = min(min(p + 0.5, box.xy + box.zw), clip.xy + clip.zw);
	float2 span = saturate(hi - lo);
	return span.x * span.y;
}

float2 ClampToUVRect(float2 uv, float4 uvRect) {
	float width;
	float height;
	sltUITexture.GetDimensions(width, height);
	float2 lo = min(uvRect.xy, uvRect.xy + uvRect.zw);
	float2 hi = max(uvRect.xy, uvRect.xy + uvRect.zw);
	float2 inset = min(0.5 / float2(width, height), (hi - lo) * 0.5);
	return clamp(uv, lo + inset, hi - inset);
}

float DitherNoise(float2 p) {
	return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453123);
}

float ShadowBlur(VSOutput input, float2 axis) {
	const float halfWidth = 0.5 * (1.0 + input.softness);
	const int reach = (int)ceil(halfWidth);
	const float2 low = input.rect.xy + 0.5;
	const float2 high = input.rect.xy + input.rect.zw - 0.5;
	const float2 centre = floor(input.positionCS.xy) + 0.5;
	float sum = 0.0;
	for (int tap = -reach; tap <= reach; ++tap) {
		const float weight = smoothstep(-halfWidth, halfWidth, tap + 0.5) - smoothstep(-halfWidth, halfWidth, tap - 0.5);
		const float2 at = clamp(centre + axis * tap, low, high);
		sum += weight * sltUITexture.SampleLevel(sltUISampler, at / gPush.viewportSize, 0).r;
	}
	return sum;
}

float4 PSMain(VSOutput input) : SV_Target {
	if (gPush.passMode == UI_PASS_BLUR_X || gPush.passMode == UI_PASS_BLUR_Y) {
		const float blurred = ShadowBlur(input, gPush.passMode == UI_PASS_BLUR_X ? float2(1.0, 0.0) : float2(0.0, 1.0));
		if (blurred <= 0.0) {
			discard;
		}
		return float4(1.0, 1.0, 1.0, blurred);
	}

	float2 contextPos = input.positionCS.xy + gPush.originPx;

	float clipSupport;
	float clipDist = RoundedBoxSDF(contextPos, input.clipRect, input.clipRadii, false, float2(1.0, 0.0), clipSupport);
	float clipMask = Coverage(clipDist, clipSupport, 0.0);
	if (clipMask <= 0.0) {
		discard;
	}

	const bool rotated = (input.flags & UI_FLAG_ROTATED) != 0u;
	float2 shapePos = contextPos;
	if (rotated) {
		float2 centre = input.rect.xy + input.rect.zw * 0.5;
		float2 d = contextPos - centre;
		shapePos = centre + float2(d.x * input.cosSin.x + d.y * input.cosSin.y,
								   -d.x * input.cosSin.y + d.y * input.cosSin.x);
	}

	const bool subPixel = (input.flags & UI_FLAG_SUBPIXEL) != 0u;
	const bool ring = (input.flags & UI_FLAG_RING) != 0u && input.innerBox.z > 0.0 && input.innerBox.w > 0.0;
	const bool square = subPixel && input.softness <= 0.0 && all(input.radii == 0.0) && all(input.clipRadii == 0.0) &&
						(!ring || all(input.innerRadii == 0.0));
	float2 uv = input.uv;
	if (subPixel && (input.flags & UI_FLAG_RING) == 0u) {
		uv = ClampToUVRect(uv, input.innerBox);
	}

	float4 color = input.color;
	float4 texel = sltUITexture.Sample(sltUISampler, uv);
	if ((gPush.textureFlags & UI_TEXTURE_DECODES) != 0u) {
		texel.rgb = ShalltearLinearToSrgb(texel.rgb);
	}

	if ((input.flags & UI_FLAG_GLYPH) != 0u) {
		float edge = clipMask;
		if (square) {
			edge = BoxOverlap(contextPos, input.sdfBox, input.clipRect);
		} else if (subPixel) {
			float support;
			float dist = RoundedBoxSDF(shapePos, input.sdfBox, input.radii, false, input.cosSin, support);
			edge = ClippedCoverage(dist, support, clipDist, clipSupport, clipMask, input.softness);
		}
		color.a *= texel.r * edge;
	} else {
		if ((input.flags & UI_FLAG_PREMULTIPLIED) != 0u && texel.a > 0.0) {
			texel.rgb /= texel.a;
		}
		color *= texel;
		float coverage;
		if (square) {
			coverage = BoxOverlap(contextPos, input.sdfBox, input.clipRect);
			if (ring) {
				coverage = max(coverage - BoxOverlap(contextPos, input.innerBox, input.clipRect), 0.0);
			}
		} else {
			float support;
			float dist = RoundedBoxSDF(shapePos, input.sdfBox, input.radii, rotated, input.cosSin, support);
			coverage = ClippedCoverage(dist, support, clipDist, clipSupport, clipMask, input.softness);
			if (ring) {
				float innerSupport;
				float innerDist = RoundedBoxSDF(shapePos, input.innerBox, input.innerRadii, rotated, input.cosSin, innerSupport);
				coverage = max(coverage - ClippedCoverage(innerDist, innerSupport, clipDist, clipSupport, clipMask, input.softness), 0.0);
			}
		}
		if ((input.flags & UI_FLAG_RADIAL_FILL) != 0u) {
			coverage *= RadialFillCoverage(contextPos, input.innerRadii);
		}
		color.a *= coverage;
	}

	if ((input.flags & UI_FLAG_DITHER) != 0u) {
		color.rgb += (DitherNoise(contextPos) - 0.5) * (1.0 / 255.0);
	}

	if (color.a <= 0.0) {
		discard;
	}
	if (gPush.passMode == UI_PASS_COVERAGE) {
		return float4(1.0, 1.0, 1.0, color.a);
	}
	return color;
}
