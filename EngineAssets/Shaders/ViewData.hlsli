#ifndef SHALLTEAR_VIEW_DATA_HLSLI
#define SHALLTEAR_VIEW_DATA_HLSLI

#include "ShalltearBufferAccess.hlsli"

struct ShalltearViewData {
	float4 view[4];
	float4 inverseView[4];
	float4 projection[4];
	float4 inverseProjection[4];
	float4 inverseViewProjection[4];
	// game time in seconds, sin/cos taken in double
	float4 time;
	float4 projectionParams;
	// xy = target size in pixels, zw = projection half extents at unit depth
	float4 screenParams;
	slt_addr exposureAddress;
	float2 exposurePad;
};

#define SHALLTEAR_VIEW_DATA_TIME 320
#define SHALLTEAR_VIEW_DATA_PROJECTION_PARAMS 336
#define SHALLTEAR_VIEW_DATA_SCREEN_PARAMS 352
#define SHALLTEAR_VIEW_DATA_EXPOSURE 368

float4x4 ShalltearViewDataMatrix(float4 c0, float4 c1, float4 c2, float4 c3) { return transpose(float4x4(c0, c1, c2, c3)); }

float ShalltearLoadExposure(slt_addr exposureAddress) {
	return ShalltearAddrValid(exposureAddress) ? ShalltearLoadF32(exposureAddress) : 1.0;
}

float ShalltearGetExposure(ShalltearViewData view) { return ShalltearLoadExposure(view.exposureAddress); }

#endif
