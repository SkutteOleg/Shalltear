#ifndef SHALLTEAR_PARTICLE_SORT_PASS_HLSLI
#define SHALLTEAR_PARTICLE_SORT_PASS_HLSLI

#include "ShalltearBufferAccess.hlsli"
#include "VisualEffects/ParticleOrder.hlsli"

[[vk::binding(0, 0)]] RWByteAddressBuffer sltSortKeys;
[[vk::binding(1, 0)]] RWByteAddressBuffer sltSortOrder;

struct SltParticleSortFrame {
	uint count;
	uint size;
	uint source;
	uint last;
	uint order;
};
SLT_PUSH_CONSTANT(SltParticleSortFrame, sltSort)

#define SLT_PARTICLE_SORT_LOCAL 1024
#define SLT_PARTICLE_SORT_LOCAL_THREADS 256
#define SLT_PARTICLE_SORT_MERGE_THREADS 256
#define SLT_PARTICLE_SORT_MERGE_GROUPS 32768

uint2 SltParticleSortLoad(uint side, uint at) {
	return sltSortKeys.Load2((side * sltSort.count + at) * 8);
}

void SltParticleSortWrite(uint side, uint at, uint2 entry) {
	if (sltSort.last != 0) {
		sltSortOrder.Store((sltSort.order + at) * 4, entry.y);
	} else {
		sltSortKeys.Store2((side * sltSort.count + at) * 8, entry);
	}
}

#endif
