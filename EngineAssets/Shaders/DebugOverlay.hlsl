#pragma shalltear_blend opaque
#pragma shalltear_depth off
#pragma shalltear_cull off

#include "ShalltearBufferAccess.hlsli"
#include "Color.hlsli"
#include "Shadows.hlsli"

// must match DebugOverlay in RenderSystem.h
#define SHALLTEAR_DEBUG_OVERLAY_DIRECTIONAL 1
#define SHALLTEAR_DEBUG_OVERLAY_PUNCTUAL 2

// must match GPUDebugOverlayPush in RenderSystem.cpp
struct ShalltearDebugOverlayPush {
	// clip space corners computed on CPU from rendered extent
	float4 rect;
	uint mode;
	uint tilesPerAxis;
	// pixels across (line width in texels vanishes)
	float sidePx;
	float pad;
};

SLT_PUSH_CONSTANT(ShalltearDebugOverlayPush, gOverlay)

struct VSOutput {
	float4 positionCS : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
	const uint2 CORNERS[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1)};
	uint2 corner = CORNERS[vertexId];
	VSOutput output;
	float2 position = float2(corner.x ? gOverlay.rect.z : gOverlay.rect.x,
							 corner.y ? gOverlay.rect.w : gOverlay.rect.y);
	output.positionCS = float4(position, 0.0, 1.0);
	output.uv = float2(corner);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target {
	// reciprocal is the atlas side in texels
	float side = 1.0 / max(sltShadowAtlasTexel, 1.0e-9);
	int2 texel = int2(saturate(input.uv) * side);

	float stored = gOverlay.mode == SHALLTEAR_DEBUG_OVERLAY_PUNCTUAL
					   ? sltPunctualShadowAtlas.Load(int3(texel, 0)).r
					   : sltShadowAtlas.Load(int3(texel, 0)).r;
	if (gOverlay.mode != SHALLTEAR_DEBUG_OVERLAY_PUNCTUAL && gShalltearShadowMoments) {
		stored = 0.5 - 0.5 * log(max(stored, 1e-30)) / max(sltShadowMoments.x, 1e-4);
	}
	float3 display = stored.rrr;

	if (gOverlay.tilesPerAxis > 1) {
		float2 withinTile = frac(input.uv * gOverlay.tilesPerAxis);
		float2 edge = min(withinTile, 1.0 - withinTile);
		// one pixel is tiles / sidePx in tile local units
		float threshold = float(gOverlay.tilesPerAxis) / max(gOverlay.sidePx, 1.0);
		if (min(edge.x, edge.y) < threshold) {
			display = float3(0.95, 0.55, 0.15);
		}
	}

	float2 fromBorder = min(input.uv, 1.0 - input.uv);
	if (min(fromBorder.x, fromBorder.y) < 1.5 / max(gOverlay.sidePx, 1.0)) {
		display = float3(0.95, 0.55, 0.15);
	}

	return float4(ShalltearSrgbToScene(display), 1.0);
}
