#ifndef SHALLTEAR_CASCADE_SELECT_HLSLI
#define SHALLTEAR_CASCADE_SELECT_HLSLI

#include "ShalltearCommon.hlsli"

#define SHALLTEAR_SHADOW_MAP_STRIDE 96
#define SHALLTEAR_SHADOW_MAP_FAR_OFFSET 80

float ShalltearCascadeFar(uint index) {
	return ShalltearLoadF32(
		ShalltearAddrAdd(sltShadowMapAddress, index * SHALLTEAR_SHADOW_MAP_STRIDE + SHALLTEAR_SHADOW_MAP_FAR_OFFSET));
}

uint ShalltearCascadeIndexOf(uint firstMap, uint count, float viewDepth) {
	for (uint i = 0; i < count; ++i) {
		if (viewDepth <= ShalltearCascadeFar(firstMap + i)) {
			return i;
		}
	}
	return count;
}

uint ShalltearCascadeOf(float viewDepth) {
	return ShalltearCascadeIndexOf(0, (uint)sltShadowCascadeCount, viewDepth);
}

#endif
