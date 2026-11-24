// grid never ends/needs no geometry

#pragma shalltear_blend alpha
#pragma shalltear_depth test_write
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"

// 108/128 push bytes
struct ShalltearGridPush {
	float4x4 invGridProjection;	// relative to gridCameraPosition
	float3 gridCameraPosition;
	float cellPixelBudget;		// smallest a cell may get in pixels
	float fadeDistance;
	float minorLineWidthPx;		// in pixels (not in cells)
	float majorLineWidthPx;
	uint minorColor;			// RGBA8 SRGB like a DebugLine's
	uint majorColor;
	uint axisAlongXColor;
	uint axisAlongZColor;
};
SLT_PUSH_CONSTANT(ShalltearGridPush, gGrid)

// grid loses every depth tie (floors sit at y = 0)
static const float SHALLTEAR_GRID_DEPTH_BIAS = 1.0 / 1024.0;

// depth tests run per sample, constant bias settles 1 point
static const float SHALLTEAR_GRID_SAMPLE_RADIUS_PX = 0.5;

struct VSOutput {
	float4 positionCS : SV_Position;
	float2 ndc : TEXCOORD0;
};

struct PSOutput {
	float4 color : SV_Target;
	float depth : SV_Depth;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	VSOutput output;
	output.ndc = uv * 2.0 - 1.0;
	output.positionCS = float4(output.ndc, 0.0, 1.0);
	return output;
}

float ShalltearSrgbToLinearChannel(float c) {
	if (gShalltearGammaColorSpace) {
		return c;
	}
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 ShalltearUnpackColor(uint packed) {
	float4 srgb = float4(float(packed & 0xFF), float((packed >> 8) & 0xFF),
						 float((packed >> 16) & 0xFF), float((packed >> 24) & 0xFF)) /
				  255.0;
	return float4(ShalltearSrgbToLinearChannel(srgb.r), ShalltearSrgbToLinearChannel(srgb.g),
				  ShalltearSrgbToLinearChannel(srgb.b), srgb.a);
}

// line width is a screen fact decided here only
float2 ShalltearLineCoverage(float2 distancePx, float widthPx) {
	float halfWidth = widthPx * 0.5;
	return 1.0 - smoothstep(halfWidth - 0.5, halfWidth + 0.5, distancePx);
}

// per axis. grazing lattices should antialias along their compression
float2 ShalltearLatticeDistancePx(float2 camera, float2 offset, float2 deriv, float cell) {
	// one sum would round offset away far from origin
	float2 phase = frac(camera / cell) + offset / cell;
	float2 distanceCells = abs(frac(phase + 0.5) - 0.5);
	return distanceCells * (cell / max(deriv, 1.0e-20));
}

float ShalltearLattice(float2 camera, float2 offset, float2 deriv, float cell, float widthPx) {
	float2 coverage = ShalltearLineCoverage(ShalltearLatticeDistancePx(camera, offset, deriv, cell), widthPx);
	return max(coverage.x, coverage.y);
}

float ShalltearAxis(float coord, float deriv, float widthPx) {
	return ShalltearLineCoverage(abs(coord) / max(deriv, 1.0e-20), widthPx).x;
}

PSOutput PSMain(VSOutput input) {
	float4 nearH = mul(gGrid.invGridProjection, float4(input.ndc, 1.0, 1.0));
	float4 farH = mul(gGrid.invGridProjection, float4(input.ndc, 0.0, 1.0));

	float cameraHeight = gGrid.gridCameraPosition.y;
	float nearHeight = nearH.y + cameraHeight * nearH.w;
	float farHeight = farH.y + cameraHeight * farH.w;
	float planeZ = farHeight / (farHeight - nearHeight);
	if (!(planeZ >= 0.0 && planeZ <= 1.0)) {
		discard;
	}
	float4 hit = lerp(farH, nearH, planeZ);
	if (hit.w <= 1.0e-20) {
		discard;
	}
	float2 offset = hit.xz / hit.w;

	float slopeBias = SHALLTEAR_GRID_SAMPLE_RADIUS_PX * (abs(ddx(planeZ)) + abs(ddy(planeZ)));
	float ndcZ = planeZ - planeZ * SHALLTEAR_GRID_DEPTH_BIAS - slopeBias;
	if (ndcZ <= 0.0) {
		discard;
	}

	float2 camera = gGrid.gridCameraPosition.xz;
	float2 deriv = float2(length(float2(ddx(offset.x), ddy(offset.x))),
						  length(float2(ddx(offset.y), ddy(offset.y))));
	float footprint = max(max(deriv.x, deriv.y), 1.0e-8);

	float level = log10(footprint * max(gGrid.cellPixelBudget, 1.0));
	float levelBase = floor(level);
	float levelBlend = level - levelBase;
	float fineCell = pow(10.0, levelBase);
	float midCell = fineCell * 10.0;
	float coarseCell = fineCell * 100.0;

	float fineWeight = 1.0 - levelBlend;
	float coarseWeight = levelBlend;

	float4 minorRgba = ShalltearUnpackColor(gGrid.minorColor);
	float4 majorRgba = ShalltearUnpackColor(gGrid.majorColor);
	float4 midRgba = lerp(majorRgba, minorRgba, levelBlend);
	float midWidthPx = lerp(gGrid.majorLineWidthPx, gGrid.minorLineWidthPx, levelBlend);

	float fine = ShalltearLattice(camera, offset, deriv, fineCell, gGrid.minorLineWidthPx) * fineWeight;
	float mid = ShalltearLattice(camera, offset, deriv, midCell, midWidthPx);
	float coarse = ShalltearLattice(camera, offset, deriv, coarseCell, gGrid.majorLineWidthPx) * coarseWeight;

	float2 coord = camera + offset;
	float2 axis = float2(ShalltearAxis(coord.x, deriv.x, gGrid.majorLineWidthPx),
						 ShalltearAxis(coord.y, deriv.y, gGrid.majorLineWidthPx));

	float4 axisAlongXRgba = ShalltearUnpackColor(gGrid.axisAlongXColor);
	float4 axisAlongZRgba = ShalltearUnpackColor(gGrid.axisAlongZColor);

	float4 color = float4(0.0, 0.0, 0.0, 0.0);
	color = lerp(color, minorRgba, fine * minorRgba.a);
	color = lerp(color, midRgba, mid * midRgba.a);
	color = lerp(color, majorRgba, coarse * majorRgba.a);
	color = lerp(color, axisAlongZRgba, axis.x * axisAlongZRgba.a);
	color = lerp(color, axisAlongXRgba, axis.y * axisAlongXRgba.a);

	float ground = length(offset);
	float fade = 1.0 - saturate(ground / max(gGrid.fadeDistance, 1.0));
	color.a *= fade * fade;
	if (color.a < 0.002) {
		discard;
	}

	PSOutput output;
	output.color = color;
	output.depth = ndcZ;
	return output;
}
