#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

[[vk::binding(0, 2)]] Texture2D sltSceneColor;
[[vk::binding(1, 2)]] SamplerState sltSceneSampler;
[[vk::binding(2, 2)]] Texture2D sltPassInput0;
[[vk::binding(3, 2)]] Texture2D sltPassInput1;

// must match PostProcessPush in PostProcessPass.cpp
struct ShalltearPostProcessPush {
	float4 vignetteColorAndIntensity;
	float4 vignetteCenterSmoothnessRounded;
	// xy is step's extent in pixels
	float4 extentAndStep;
	float4 bloomTintAndIntensity;
	float4 bloomParams;
	slt_addr gradeAddress;
};

SLT_PUSH_CONSTANT(ShalltearPostProcessPush, gPost)

// must match PostProcessGrade in PostProcessPass.cpp
struct ShalltearPostGrade {
	float4 colorFilterAndContrast;
	// x is hue shift in rads, z is 2^stops
	float4 hueSaturationExposure;
	float4 whiteBalance0;
	float4 whiteBalance1;
	float4 whiteBalance2;
	float4 shadows;
	float4 midtones;
	float4 highlights;
	float4 limits;
	uint4 flags;
};

#define SHALLTEAR_POST_STEP_PREFILTER 0
#define SHALLTEAR_POST_STEP_DOWNSAMPLE 1
#define SHALLTEAR_POST_STEP_UPSAMPLE 2
#define SHALLTEAR_POST_STEP_COMPOSITE 3

#define SHALLTEAR_POST_CONTRAST_PIVOT 0.18

static const float3 SHALLTEAR_POST_LUMA = float3(0.2126, 0.7152, 0.0722);

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

float3 ShalltearPostHueShift(float3 color, float radians) {
	float c = cos(radians);
	float s = sin(radians);
	float3x3 m = float3x3(
		0.299 + 0.701 * c + 0.168 * s, 0.587 - 0.587 * c + 0.330 * s, 0.114 - 0.114 * c - 0.497 * s,
		0.299 - 0.299 * c - 0.328 * s, 0.587 + 0.413 * c + 0.035 * s, 0.114 - 0.114 * c + 0.292 * s,
		0.299 - 0.300 * c + 1.250 * s, 0.587 - 0.588 * c - 1.050 * s, 0.114 + 0.886 * c - 0.203 * s);
	return mul(m, color);
}

float ShalltearPostRamp(float edge0, float edge1, float x) {
	float t = saturate((x - edge0) / (edge1 - edge0));
	return t * t * (3.0 - 2.0 * t);
}

float3 ShalltearPostGradeColor(float3 color, ShalltearPostGrade grade) {
	if (grade.flags.x != 0u) {
		color *= grade.hueSaturationExposure.z;
	}
	if (grade.flags.y != 0u) {
		color = float3(dot(grade.whiteBalance0.rgb, color), dot(grade.whiteBalance1.rgb, color), dot(grade.whiteBalance2.rgb, color));
	}
	if (grade.flags.x != 0u) {
		color = (color - SHALLTEAR_POST_CONTRAST_PIVOT) * grade.colorFilterAndContrast.w + SHALLTEAR_POST_CONTRAST_PIVOT;
		color *= grade.colorFilterAndContrast.rgb;
	}
	if (grade.flags.z != 0u) {
		float luma = dot(color, SHALLTEAR_POST_LUMA);
		float shadows = 1.0 - ShalltearPostRamp(grade.limits.x, grade.limits.y, luma);
		float highlights = ShalltearPostRamp(grade.limits.z, grade.limits.w, luma);
		color *= grade.shadows.rgb * shadows + grade.midtones.rgb * (1.0 - shadows - highlights) + grade.highlights.rgb * highlights;
	}
	if (grade.flags.x != 0u) {
		if (abs(grade.hueSaturationExposure.x) > 1e-6) {
			color = ShalltearPostHueShift(color, grade.hueSaturationExposure.x);
		}
		float luma = dot(color, SHALLTEAR_POST_LUMA);
		color = luma + (color - luma) * grade.hueSaturationExposure.y;
	}
	return max(color, 0.0);
}

float2 ShalltearBloomSourceTexel(uint step) {
	float2 texel = 1.0 / max(gPost.extentAndStep.xy, 1.0);
	return step == SHALLTEAR_POST_STEP_UPSAMPLE || step == SHALLTEAR_POST_STEP_COMPOSITE
			   ? texel * 2.0
			   : texel * 0.5;
}

float3 ShalltearBloomEncode(float3 color) {
	return gShalltearGammaColorSpace ? sqrt(color) : color;
}

float3 ShalltearBloomTap(Texture2D source, float2 uv, bool level) {
	float3 stored = source.Sample(sltSceneSampler, uv).rgb;
	return level && gShalltearGammaColorSpace ? stored * stored : stored;
}

float3 ShalltearBloomDownsample(Texture2D source, float2 uv, float2 texel, bool level) {
	float3 a = ShalltearBloomTap(source, uv + texel * float2(-2.0, -2.0), level);
	float3 b = ShalltearBloomTap(source, uv + texel * float2(0.0, -2.0), level);
	float3 c = ShalltearBloomTap(source, uv + texel * float2(2.0, -2.0), level);
	float3 d = ShalltearBloomTap(source, uv + texel * float2(-1.0, -1.0), level);
	float3 e = ShalltearBloomTap(source, uv + texel * float2(1.0, -1.0), level);
	float3 f = ShalltearBloomTap(source, uv + texel * float2(-2.0, 0.0), level);
	float3 g = ShalltearBloomTap(source, uv, level);
	float3 h = ShalltearBloomTap(source, uv + texel * float2(2.0, 0.0), level);
	float3 i = ShalltearBloomTap(source, uv + texel * float2(-1.0, 1.0), level);
	float3 j = ShalltearBloomTap(source, uv + texel * float2(1.0, 1.0), level);
	float3 k = ShalltearBloomTap(source, uv + texel * float2(-2.0, 2.0), level);
	float3 l = ShalltearBloomTap(source, uv + texel * float2(0.0, 2.0), level);
	float3 m = ShalltearBloomTap(source, uv + texel * float2(2.0, 2.0), level);
	float3 result = (d + e + i + j) * 0.125;
	result += (a + b + g + f) * 0.03125;
	result += (b + c + h + g) * 0.03125;
	result += (f + g + l + k) * 0.03125;
	result += (g + h + m + l) * 0.03125;
	return result;
}

// 9 tap tent upsample. levels should show no square edges
float3 ShalltearBloomUpsample(Texture2D source, float2 uv, float2 texel) {
	float3 result = ShalltearBloomTap(source, uv + texel * float2(-1.0, -1.0), true);
	result += ShalltearBloomTap(source, uv + texel * float2(0.0, -1.0), true) * 2.0;
	result += ShalltearBloomTap(source, uv + texel * float2(1.0, -1.0), true);
	result += ShalltearBloomTap(source, uv + texel * float2(-1.0, 0.0), true) * 2.0;
	result += ShalltearBloomTap(source, uv, true) * 4.0;
	result += ShalltearBloomTap(source, uv + texel * float2(1.0, 0.0), true) * 2.0;
	result += ShalltearBloomTap(source, uv + texel * float2(-1.0, 1.0), true);
	result += ShalltearBloomTap(source, uv + texel * float2(0.0, 1.0), true) * 2.0;
	result += ShalltearBloomTap(source, uv + texel * float2(1.0, 1.0), true);
	return result * 0.0625;
}

float3 ShalltearBloomThreshold(float3 color) {
	float threshold = gPost.bloomParams.x;
	float knee = gPost.bloomParams.y;
	// knee should not amplify a firefly
	color = min(color, gPost.bloomParams.w);
	float brightness = max(color.r, max(color.g, color.b));
	float soft = brightness - threshold + knee;
	soft = clamp(soft, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee + 1e-4);
	float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4);
	return color * contribution;
}

ShalltearPostGrade ShalltearLoadPostGrade(slt_addr address) {
#ifdef SHALLTEAR_WEBGPU
	ShalltearPostGrade g;
	g.colorFilterAndContrast = ShalltearLoadF32x4(address);
	g.hueSaturationExposure = ShalltearLoadF32x4(ShalltearAddrAdd(address, 16u));
	g.whiteBalance0 = ShalltearLoadF32x4(ShalltearAddrAdd(address, 32u));
	g.whiteBalance1 = ShalltearLoadF32x4(ShalltearAddrAdd(address, 48u));
	g.whiteBalance2 = ShalltearLoadF32x4(ShalltearAddrAdd(address, 64u));
	g.shadows = ShalltearLoadF32x4(ShalltearAddrAdd(address, 80u));
	g.midtones = ShalltearLoadF32x4(ShalltearAddrAdd(address, 96u));
	g.highlights = ShalltearLoadF32x4(ShalltearAddrAdd(address, 112u));
	g.limits = ShalltearLoadF32x4(ShalltearAddrAdd(address, 128u));
	g.flags = ShalltearLoadU32x4(ShalltearAddrAdd(address, 144u));
	return g;
#else
	return vk::RawBufferLoad<ShalltearPostGrade>(address, 16);
#endif
}

float4 PSMain(VSOutput input) : SV_Target {
	uint step = (uint)(gPost.extentAndStep.z + 0.5);
	float2 sourceTexel = ShalltearBloomSourceTexel(step);

	// steps shall return before grading runs
	if (step == SHALLTEAR_POST_STEP_PREFILTER) {
		float3 bright = ShalltearBloomThreshold(ShalltearBloomDownsample(sltSceneColor, input.uv, sourceTexel, false));
		return float4(ShalltearBloomEncode(bright), 1.0);
	}
	if (step == SHALLTEAR_POST_STEP_DOWNSAMPLE) {
		return float4(ShalltearBloomEncode(ShalltearBloomDownsample(sltPassInput0, input.uv, sourceTexel, true)), 1.0);
	}
	if (step == SHALLTEAR_POST_STEP_UPSAMPLE) {
		float3 lower = ShalltearBloomUpsample(sltPassInput0, input.uv, sourceTexel);
		float3 here = ShalltearBloomTap(sltPassInput1, input.uv, true);
		return float4(ShalltearBloomEncode(lerp(here, lower, gPost.bloomParams.z)), 1.0);
	}

	float3 color = sltSceneColor.Sample(sltSceneSampler, input.uv).rgb;

	bool decoded = gShalltearGammaColorSpace &&
				   (gPost.bloomTintAndIntensity.w > 0.0 || gPost.extentAndStep.w > 0.5 || ShalltearAddrValid(gPost.gradeAddress));
	if (decoded) {
		color = ShalltearSrgbToLinearUnclamped(color);
	}

	// bloom added before grade (filters should tint it too)
	if (gPost.bloomTintAndIntensity.w > 0.0) {
		float3 bloom = ShalltearBloomUpsample(sltPassInput0, input.uv, sourceTexel);
		color += bloom * gPost.bloomTintAndIntensity.rgb * gPost.bloomTintAndIntensity.w;
	}

	if (gPost.extentAndStep.w > 0.5) {
		float2 d = abs(input.uv - gPost.vignetteCenterSmoothnessRounded.xy) * gPost.vignetteColorAndIntensity.w;
		float aspect = gPost.extentAndStep.y > 0.0 ? gPost.extentAndStep.x / gPost.extentAndStep.y : 1.0;
		d.x *= lerp(1.0, aspect, gPost.vignetteCenterSmoothnessRounded.w);
		float factor = pow(saturate(1.0 - dot(d, d)), gPost.vignetteCenterSmoothnessRounded.z);
		color *= lerp(gPost.vignetteColorAndIntensity.rgb, 1.0, factor);
	}

	if (ShalltearAddrValid(gPost.gradeAddress)) {
		// after bloom, before curve
		color = ShalltearPostGradeColor(color, ShalltearLoadPostGrade(gPost.gradeAddress));
	}

	return float4(decoded ? ShalltearLinearToSrgbUnclamped(color) : color, 1.0);
}
