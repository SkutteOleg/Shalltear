#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"

[[vk::binding(0, 2)]] Texture2D<float> sltSceneDepth;
[[vk::binding(2, 2)]] Texture2D sltSceneNormals;
[[vk::binding(3, 2)]] Texture2D<float> sltPassInput0;
// present only when the row asked for afterOpaque
[[vk::binding(4, 2)]] Texture2D sltSceneColor;

// must match AmbientOcclusionPush in AmbientOcclusionPass.cpp. float for float
struct ShalltearAmbientOcclusionPush {
	float4 viewRows[3];
	// view xy at depth 1 = clip xy * xy + zw
	float4 unproject;
	float4 projHalfAndLinearize;
	float4 radiusFalloffIntensity;
	// xy is this step's extent in pixels
	float4 extentStepSlices;
};

SLT_PUSH_CONSTANT(ShalltearAmbientOcclusionPush, gAO)

#define SHALLTEAR_AO_STEP_ESTIMATE 0
#define SHALLTEAR_AO_STEP_BLUR_X 1
#define SHALLTEAR_AO_STEP_BLUR_Y 2
#define SHALLTEAR_AO_STEP_COMPOSITE 3
#define SHALLTEAR_AO_STEP_UPSAMPLE 4
#define SHALLTEAR_AO_STEP_SMOOTH_X 5
#define SHALLTEAR_AO_STEP_SMOOTH_Y 6

#define SHALLTEAR_AO_STEPS 12
#define SHALLTEAR_AO_NEAR_STEPS 3

#define SHALLTEAR_AO_REFINE 4
#define SHALLTEAR_AO_REFINE_BELOW 0.97

// rads
#define SHALLTEAR_AO_ANGLE_BIAS 0.1

#define SHALLTEAR_AO_THIN_DECAY 0.25

#define SHALLTEAR_AO_EXPONENT 2.0

#define SHALLTEAR_AO_MAX_RADIUS_UV 0.2
#define SHALLTEAR_AO_MIN_RADIUS_PX 2.0

#define SHALLTEAR_AO_DISTANCE_FADE 0.2

#define SHALLTEAR_AO_MIN_FACING 0.05

// pattern period = 4 px, 1 blur slot per phase
#define SHALLTEAR_AO_BLUR_REACH 3

#define SHALLTEAR_AO_BLUR_FAR_WEIGHT 0.25

#define SHALLTEAR_AO_SMOOTH_REACH 3
#define SHALLTEAR_AO_SMOOTH_STRIDE 2

#define SHALLTEAR_AO_PI 3.14159265
#define SHALLTEAR_AO_HALF_PI 1.57079633

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

// inverse of the vertex stage's flip
float2 ShalltearAOClipFromUV(float2 uv) {
	return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 ShalltearAODepthExtent() {
	uint width, height;
	sltSceneDepth.GetDimensions(width, height);
	return float2(width, height);
}

// 0.25 to step off a texel edge (1/2 extent centers)
int2 ShalltearAODepthTexel(int2 pixel, float2 depthExtent) {
	float2 uv = (float2(pixel) + 0.5) / gAO.extentStepSlices.xy;
	return min(int2(floor(uv * depthExtent + 0.25)), int2(depthExtent) - 1);
}

float ShalltearAOViewDepth(float rawDepth) {
	return gAO.projHalfAndLinearize.z / max(rawDepth + gAO.projHalfAndLinearize.w, 1.0e-6);
}

float3 ShalltearAOViewPosition(int2 texel, float2 depthExtent, float rawDepth) {
	float viewDepth = ShalltearAOViewDepth(rawDepth);
	float2 clip = ShalltearAOClipFromUV((float2(texel) + 0.5) / depthExtent);
	return float3((clip * gAO.unproject.xy + gAO.unproject.zw) * viewDepth, viewDepth);
}

// x = slice turn, y = step shift, 16 pairs per 4x4 block
float2 ShalltearAOPattern(int2 pixel) {
	uint2 cell = uint2(pixel) & 3u;
	uint column = ((cell.x & 1u) << 1) | (cell.x >> 1);
	uint index = (((column + cell.y) & 3u) << 2) | cell.y;
	return (float2(index, (index * 5u) & 15u) + 0.5) / 16.0;
}

float ShalltearAOEstimate(int2 pixel) {
	float2 depthExtent = ShalltearAODepthExtent();
	int2 texel = ShalltearAODepthTexel(pixel, depthExtent);
	float rawDepth = sltSceneDepth.Load(int3(texel, 0));
	if (rawDepth <= 0.0) {
		return 1.0;
	}
	float3 position = ShalltearAOViewPosition(texel, depthExtent, rawDepth);
	float falloffDistance = gAO.radiusFalloffIntensity.y;
	float fade = saturate((falloffDistance - position.z) / (falloffDistance * SHALLTEAR_AO_DISTANCE_FADE));
	float radius = gAO.radiusFalloffIntensity.x;
	float radiusPx = min(radius * gAO.projHalfAndLinearize.y / position.z, SHALLTEAR_AO_MAX_RADIUS_UV) * depthExtent.y;
	fade *= saturate((radiusPx - SHALLTEAR_AO_MIN_RADIUS_PX) / (2.0 * SHALLTEAR_AO_MIN_RADIUS_PX));
	float intensity = gAO.radiusFalloffIntensity.z;
	if (fade <= 0.0 || intensity <= 0.0) {
		return 1.0;
	}

	float3 normalWS = sltSceneNormals.Load(int3(texel, 0)).xyz * 2.0 - 1.0;
	float3 normal = normalize(float3(dot(gAO.viewRows[0].xyz, normalWS), dot(gAO.viewRows[1].xyz, normalWS), dot(gAO.viewRows[2].xyz, normalWS)));
	float3 toEye = normalize(-position);
	normal = normalize(normal + toEye * max(SHALLTEAR_AO_MIN_FACING - dot(normal, toEye), 0.0));
	float normalToEye = dot(normal, toEye);

	float2 pattern = ShalltearAOPattern(pixel);
	float slices = gAO.extentStepSlices.w;
	float farStart = float(SHALLTEAR_AO_NEAR_STEPS + 1);
	float inverseRadiusSq = 1.0 / (radius * radius);
	float2 baseUV = (float2(texel) + 0.5) / depthExtent;

	float visible = 0.0;
	float open = 0.0;
	float sliceCount = slices;
	for (float slice = 0.0; slice < sliceCount; slice += 1.0) {
		float round = floor(slice / slices);
		float turn = (slice - round * slices + pattern.x + round / float(SHALLTEAR_AO_REFINE)) * (SHALLTEAR_AO_PI / slices);
		float2 direction = float2(cos(turn), sin(turn));
		float3 directionVS = float3(direction.x, -direction.y, 0.0);
		float3 across = directionVS - toEye * dot(directionVS, toEye);
		float3 axis = normalize(cross(directionVS, toEye));
		float3 planeNormal = normal - axis * dot(normal, axis);
		float planeLength = length(planeNormal);
		float cosTilt = saturate(dot(planeNormal, toEye) / max(planeLength, 1.0e-5));
		float tilt = (dot(across, planeNormal) < 0.0 ? -1.0 : 1.0) * acos(cosTilt);
		float sinTilt = sin(tilt);

		// cosine of horizon's rise off surface. x = against direction, y = along
		float2 rise = 1.0;
		float shift = frac(pattern.y + slice / slices + round * 0.381966);
		for (int s = 0; s < SHALLTEAR_AO_STEPS; ++s) {
			float reachPx = float(s + 1);
			if (s >= SHALLTEAR_AO_NEAR_STEPS) {
				float along = (float(s - SHALLTEAR_AO_NEAR_STEPS) + shift) / float(SHALLTEAR_AO_STEPS - SHALLTEAR_AO_NEAR_STEPS);
				reachPx = lerp(farStart, max(radiusPx, farStart), along * along);
			}
			if (reachPx > radiusPx) {
				break;
			}
			float2 offset = direction * reachPx / depthExtent;
			for (int side = 0; side < 2; ++side) {
				int2 sampleTexel = int2(floor((baseUV + (side == 0 ? -offset : offset)) * depthExtent));
				if (any(sampleTexel < 0) || any(sampleTexel >= int2(depthExtent))) {
					continue;
				}
				float sampleRaw = sltSceneDepth.Load(int3(sampleTexel, 0));
				float cosine = 1.0;
				float falloff = 0.0;
				if (sampleRaw > 0.0) {
					float3 toSample = ShalltearAOViewPosition(sampleTexel, depthExtent, sampleRaw) - position;
					float distanceSq = dot(toSample, toSample);
					if (distanceSq < 1.0e-10) {
						continue;
					}
					float3 toward = toSample * rsqrt(distanceSq);
					float lift = dot(toward, normal);
					if (lift > 0.0) {
						// angle in tap's own slice (texel centers sit off this one)
						float facing = dot(toward, toEye);
						float run = normalToEye - facing * lift;
						cosine = run * rsqrt(max(run * run + lift * lift * (1.0 - facing * facing), 1.0e-12));
						falloff = saturate(1.0 - distanceSq * inverseRadiusSq);
					}
				}
				float weighed = lerp(1.0, cosine, falloff);
				// lower surface past horizon -> occluder was thin
				rise[side] = weighed < rise[side] ? weighed : lerp(rise[side], max(cosine, rise[side]), SHALLTEAR_AO_THIN_DECAY);
			}
		}

		float2 bend = acos(clamp(rise, -1.0, 1.0));
		bend *= saturate(bend / SHALLTEAR_AO_ANGLE_BIAS - 1.0);
		bend = min(bend, float2(SHALLTEAR_AO_HALF_PI - tilt, SHALLTEAR_AO_HALF_PI + tilt));
		float2 angle = float2(tilt - SHALLTEAR_AO_HALF_PI + bend.x, tilt + SHALLTEAR_AO_HALF_PI - bend.y);
		float2 arc = cosTilt + 2.0 * angle * sinTilt - cos(2.0 * angle - tilt);
		visible += planeLength * 0.25 * (arc.x + arc.y);
		open += planeLength * (cosTilt + tilt * sinTilt);
		if (slice == slices - 1.0 && visible < open * SHALLTEAR_AO_REFINE_BELOW) {
			sliceCount = slices * float(SHALLTEAR_AO_REFINE);
		}
	}

	float visibility = pow(saturate(visible / max(open, 1.0e-4)), SHALLTEAR_AO_EXPONENT * intensity);
	return lerp(1.0, visibility, fade);
}

float ShalltearAOEdge(float rawDepth, float planeDepth, float centerRaw, float pixels) {
	float pixelSlope = gAO.projHalfAndLinearize.y * gAO.extentStepSlices.y;
	float slope = (rawDepth - planeDepth) / (centerRaw + gAO.projHalfAndLinearize.w) * pixelSlope / pixels;
	return exp(-slope * slope);
}

float ShalltearAOBlur(int2 pixel, int2 axis) {
	float2 depthExtent = ShalltearAODepthExtent();
	float center = sltPassInput0.Load(int3(pixel, 0));
	float centerRaw = sltSceneDepth.Load(int3(ShalltearAODepthTexel(pixel, depthExtent), 0));
	if (centerRaw <= 0.0) {
		return center;
	}

	int2 extent = int2(gAO.extentStepSlices.xy);
	float raws[2 * SHALLTEAR_AO_BLUR_REACH + 1];
	for (int i = -SHALLTEAR_AO_BLUR_REACH; i <= SHALLTEAR_AO_BLUR_REACH; ++i) {
		int2 tap = pixel + axis * i;
		bool inside = all(tap >= 0) && all(tap < extent);
		raws[i + SHALLTEAR_AO_BLUR_REACH] = inside ? sltSceneDepth.Load(int3(ShalltearAODepthTexel(tap, depthExtent), 0)) : -1.0;
	}
	float ahead = raws[SHALLTEAR_AO_BLUR_REACH + 1] - centerRaw;
	float behind = centerRaw - raws[SHALLTEAR_AO_BLUR_REACH - 1];
	float gradient = abs(ahead) < abs(behind) ? ahead : behind;

	float4 total = float4(center, 0.0, 0.0, 0.0);
	float4 weight = float4(1.0, 0.0, 0.0, 0.0);
	float4 seen = float4(1.0, 0.0, 0.0, 0.0);
	for (int k = -SHALLTEAR_AO_BLUR_REACH; k <= SHALLTEAR_AO_BLUR_REACH; ++k) {
		float raw = raws[k + SHALLTEAR_AO_BLUR_REACH];
		if (k == 0 || raw < 0.0) {
			continue;
		}
		float edge = ShalltearAOEdge(raw, centerRaw + gradient * float(k), centerRaw, abs(float(k)));
		float tapWeight = edge * (abs(k) == SHALLTEAR_AO_BLUR_REACH ? SHALLTEAR_AO_BLUR_FAR_WEIGHT : 1.0);
		int phase = (k + 4) & 3;
		total[phase] += sltPassInput0.Load(int3(pixel + axis * k, 0)) * tapWeight;
		weight[phase] += tapWeight;
		seen[phase] += edge;
	}
	seen = saturate(seen);
	return dot(total / max(weight, 1.0e-5), seen) / dot(seen, 1.0);
}

float ShalltearAOSmooth(int2 pixel, int2 axis) {
	float2 depthExtent = ShalltearAODepthExtent();
	float center = sltPassInput0.Load(int3(pixel, 0));
	float centerRaw = sltSceneDepth.Load(int3(ShalltearAODepthTexel(pixel, depthExtent), 0));
	if (centerRaw <= 0.0) {
		return center;
	}

	int2 extent = int2(gAO.extentStepSlices.xy);
	int2 last = extent - 1;
	float ahead = sltSceneDepth.Load(int3(ShalltearAODepthTexel(min(pixel + axis, last), depthExtent), 0)) - centerRaw;
	float behind = centerRaw - sltSceneDepth.Load(int3(ShalltearAODepthTexel(max(pixel - axis, 0), depthExtent), 0));
	float gradient = abs(ahead) < abs(behind) ? ahead : behind;

	float total = center * float(SHALLTEAR_AO_SMOOTH_REACH + 1);
	float weightSum = float(SHALLTEAR_AO_SMOOTH_REACH + 1);
	for (int k = -SHALLTEAR_AO_SMOOTH_REACH; k <= SHALLTEAR_AO_SMOOTH_REACH; ++k) {
		int2 tap = pixel + axis * (k * SHALLTEAR_AO_SMOOTH_STRIDE);
		if (k == 0 || any(tap < 0) || any(tap >= extent)) {
			continue;
		}
		float pixels = float(k * SHALLTEAR_AO_SMOOTH_STRIDE);
		float raw = sltSceneDepth.Load(int3(ShalltearAODepthTexel(tap, depthExtent), 0));
		float weight = ShalltearAOEdge(raw, centerRaw + gradient * pixels, centerRaw, abs(pixels)) * float(SHALLTEAR_AO_SMOOTH_REACH + 1 - abs(k));
		total += sltPassInput0.Load(int3(tap, 0)) * weight;
		weightSum += weight;
	}
	return total / weightSum;
}

float ShalltearAOUpsample(int2 pixel) {
	uint sourceWidth, sourceHeight;
	sltPassInput0.GetDimensions(sourceWidth, sourceHeight);
	float2 sourceExtent = float2(sourceWidth, sourceHeight);
	float2 extent = gAO.extentStepSlices.xy;

	float centerRaw = sltSceneDepth.Load(int3(pixel, 0));
	if (centerRaw <= 0.0) {
		return 1.0;
	}
	int2 last = int2(extent) - 1;
	float right = sltSceneDepth.Load(int3(min(pixel + int2(1, 0), last), 0)) - centerRaw;
	float left = centerRaw - sltSceneDepth.Load(int3(max(pixel - int2(1, 0), 0), 0));
	float down = sltSceneDepth.Load(int3(min(pixel + int2(0, 1), last), 0)) - centerRaw;
	float up = centerRaw - sltSceneDepth.Load(int3(max(pixel - int2(0, 1), 0), 0));
	float2 gradient = float2(abs(right) < abs(left) ? right : left, abs(down) < abs(up) ? down : up);

	float2 spacing = extent / sourceExtent;
	int2 base = int2(floor((float2(pixel) + 0.5) / spacing - 0.5));
	float total = 0.0;
	float weightSum = 0.0;
	for (int j = 0; j <= 1; ++j) {
		for (int i = 0; i <= 1; ++i) {
			int2 source = clamp(base + int2(i, j), 0, int2(sourceExtent) - 1);
			// must match ShalltearAODepthTexel at source's extent
			int2 texel = min(int2(floor((float2(source) + 0.5) * spacing + 0.25)), last);
			float2 offset = float2(texel - pixel);
			float2 tent = saturate(1.0 - abs(offset) / spacing);
			float raw = sltSceneDepth.Load(int3(texel, 0));
			float edge = ShalltearAOEdge(raw, centerRaw + dot(gradient, offset), centerRaw, max(length(offset), 1.0));
			float weight = tent.x * tent.y * edge + 1.0e-4;
			total += sltPassInput0.Load(int3(source, 0)) * weight;
			weightSum += weight;
		}
	}
	return total / weightSum;
}

float4 PSMain(VSOutput input) : SV_Target {
	uint step = (uint)gAO.extentStepSlices.z;
	int2 pixel = int2(input.positionCS.xy);
	if (step == SHALLTEAR_AO_STEP_COMPOSITE) {
		float occluded = sltPassInput0.Load(int3(pixel, 0));
		float3 scene = sltSceneColor.Load(int3(pixel, 0)).rgb;
		return float4(scene * occluded, 1.0);
	}

	float occlusion;
	if (step == SHALLTEAR_AO_STEP_ESTIMATE) {
		occlusion = ShalltearAOEstimate(pixel);
	} else if (step == SHALLTEAR_AO_STEP_BLUR_X) {
		occlusion = ShalltearAOBlur(pixel, int2(1, 0));
	} else if (step == SHALLTEAR_AO_STEP_BLUR_Y) {
		occlusion = ShalltearAOBlur(pixel, int2(0, 1));
	} else if (step == SHALLTEAR_AO_STEP_SMOOTH_X) {
		occlusion = ShalltearAOSmooth(pixel, int2(1, 0));
	} else if (step == SHALLTEAR_AO_STEP_SMOOTH_Y) {
		occlusion = ShalltearAOSmooth(pixel, int2(0, 1));
	} else {
		occlusion = ShalltearAOUpsample(pixel);
	}
	return float4(occlusion, occlusion, occlusion, 1.0);
}
