#ifndef SHALLTEAR_PARTICLE_SYSTEM_HLSLI
#define SHALLTEAR_PARTICLE_SYSTEM_HLSLI

#include "ShalltearBufferAccess.hlsli"

#define SLT_PARTICLE_KERNEL_GROUP 64

[[vk::binding(0, 0)]] RWByteAddressBuffer sltParticleLists;
[[vk::binding(1, 0)]] RWByteAddressBuffer sltParticleAttributes;

struct SltParticleFrame {
	uint capacity;
	uint batchSlots;
	uint slots;
	uint firstSlot;
	uint attributeCount;
	uint outputCount;
	uint updateGroupSize;
	uint batchFresh;
	slt_addr instances;
	float sortViewX;
	float sortViewY;
	float sortViewZ;
	float sortSign;
	float sortForwardX;
	float sortForwardY;
	float sortForwardZ;
	uint sortCount;
	uint stripCount;
	uint particlesPerStrip;
	uint parentCapacity;
	uint events;
};
SLT_PUSH_CONSTANT(SltParticleFrame, sltParticle)

static const uint SLT_PARTICLE_INSTANCE_BYTES = 32;
static const uint SLT_PARTICLE_ADVANCES = 1;
static const uint SLT_PARTICLE_FRESH = 2;
static const uint SLT_PARTICLE_CLAIMED = 4;

struct SltParticleInstance {
	uint slot;
	uint flags;
	uint parity;
	uint spawnCount;
	float deltaTime;
	uint frame;
	uint drawTable;
	slt_addr effect;
	uint lists;
	uint attributes;
	uint arguments;
};
static SltParticleInstance sltInstance;

uint SltParticleSlotListsBytes() {
	uint bytes = 16 + 12 * sltParticle.capacity + 24 * sltParticle.stripCount;
	if (sltParticle.events != 0) {
		bytes += 8 + 4 * sltParticle.capacity;
	}
	return bytes + 48;
}

uint SltParticleSlotAttributesBytes() {
	return sltParticle.attributeCount * sltParticle.capacity * 4;
}

uint SltParticleSlotArgumentsBytes() {
	return 12 + 16 * max(sltParticle.outputCount, 1u) + (sltParticle.events != 0 ? 12u : 0u);
}

uint SltParticleEnter(uint slot) {
	uint4 words = uint4(0u, 0u, 0u, 0u);
	uint4 more = uint4(0u, 0u, 0u, 0u);
	if (slot - sltParticle.firstSlot < sltParticle.slots) {
		slt_addr at = ShalltearAddrAdd(sltParticle.instances, slot * SLT_PARTICLE_INSTANCE_BYTES);
		words = ShalltearLoadU32x4(at);
		more = ShalltearLoadU32x4(ShalltearAddrAdd(at, 16u));
	}
	sltInstance.slot = slot;
	sltInstance.flags = words.x;
	sltInstance.parity = words.y;
	sltInstance.spawnCount = words.z;
	sltInstance.deltaTime = asfloat(words.w);
	sltInstance.frame = more.x;
	sltInstance.drawTable = more.y;
	sltInstance.effect = ShalltearAddrFromWords(more.z, more.w);
	sltInstance.lists = slot * SltParticleSlotListsBytes();
	sltInstance.attributes = slot * SltParticleSlotAttributesBytes();
	sltInstance.arguments = slot * SltParticleSlotArgumentsBytes();
	return sltInstance.flags;
}

bool SltParticleEnterUpdate(uint3 id) {
	return (SltParticleEnter(sltParticle.firstSlot + id.y) & SLT_PARTICLE_ADVANCES) != 0;
}

slt_addr SltParticleEffectAddress() {
	return sltInstance.effect;
}

uint SltParticleFrameIndex() {
	return sltInstance.frame;
}

uint SltParticleParity() {
	return sltInstance.parity;
}

uint SltParticleSpawnCount() {
	return sltInstance.spawnCount;
}

float SltParticleDeltaTime() {
	return sltInstance.deltaTime;
}

#include "ParticleEffect.hlsli"

static const uint SLT_PARTICLE_ALIVE_COUNT = 0;
static const uint SLT_PARTICLE_DEAD_COUNT = 8;
static const uint SLT_PARTICLE_DROPPED_COUNT = 12;
static const uint SLT_PARTICLE_LISTS = 16;

static const uint SLT_PARTICLE_DRAW_ARGUMENTS = 12;

uint SltParticleBatchDispatchOffset() {
	return sltParticle.batchSlots * SltParticleSlotArgumentsBytes();
}

uint SltParticleAliveListOffset(uint list) {
	return sltInstance.lists + SLT_PARTICLE_LISTS + list * sltParticle.capacity * 4;
}

uint SltParticleDeadListOffset() {
	return sltInstance.lists + SLT_PARTICLE_LISTS + 2 * sltParticle.capacity * 4;
}

uint SltParticleAliveInCount() {
	return sltParticleLists.Load(sltInstance.lists + SLT_PARTICLE_ALIVE_COUNT + sltInstance.parity * 4);
}

uint SltParticleAliveIn(uint i) {
	return sltParticleLists.Load(SltParticleAliveListOffset(sltInstance.parity) + i * 4);
}

bool SltParticlePopDead(out uint index) {
	uint before;
	sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DEAD_COUNT, 0xFFFFFFFFu, before);
	if (before == 0 || before > sltParticle.capacity) {
		sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DEAD_COUNT, 1u);
		sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DROPPED_COUNT, 1u);
		index = 0;
		return false;
	}
	index = sltParticleLists.Load(SltParticleDeadListOffset() + (before - 1) * 4);
	return true;
}

void SltParticleAppendAlive(uint index) {
	uint list = 1 - sltInstance.parity;
	uint slot;
	sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_ALIVE_COUNT + list * 4, 1u, slot);
	sltParticleLists.Store(SltParticleAliveListOffset(list) + slot * 4, index);
}

void SltParticleKill(uint index) {
	uint slot;
	sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DEAD_COUNT, 1u, slot);
	sltParticleLists.Store(SltParticleDeadListOffset() + slot * 4, index);
}

static const uint SLT_PARTICLE_STRIP_BYTES = 24;
static const uint SLT_PARTICLE_STRIP_FIRST = 0;
static const uint SLT_PARTICLE_STRIP_COUNT = 4;
static const uint SLT_PARTICLE_STRIP_BEGAN = 8;
static const uint SLT_PARTICLE_STRIP_LEAST = 12;
static const uint SLT_PARTICLE_STRIP_MOST = 16;
static const uint SLT_PARTICLE_STRIP_HELD = 20;
static const uint SLT_PARTICLE_STRIP_EMPTY = 0xFFFFFFFFu;

uint SltParticleStripOffsetOf(uint capacity, uint strip) {
	return SLT_PARTICLE_LISTS + 3 * capacity * 4 + strip * SLT_PARTICLE_STRIP_BYTES;
}

uint SltParticleStripOffset(uint strip) {
	return sltInstance.lists + SltParticleStripOffsetOf(sltParticle.capacity, strip);
}

uint SltParticleStripWord(uint strip, uint field) {
	return sltParticleLists.Load(SltParticleStripOffset(strip) + field);
}

uint SltParticlePerStrip() {
	return max(sltParticle.particlesPerStrip, 1u);
}

uint SltParticleStripOf(uint index) {
	return index / SltParticlePerStrip();
}

uint SltParticleStripPlace(uint index) {
	uint perStrip = SltParticlePerStrip();
	uint strip = index / perStrip;
	return (perStrip + index - strip * perStrip - SltParticleStripWord(strip, SLT_PARTICLE_STRIP_FIRST)) % perStrip;
}

bool SltParticleStripSpawn(uint strip, out uint index, out uint place) {
	index = 0;
	place = 0;
	if (strip >= sltParticle.stripCount) {
		sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DROPPED_COUNT, 1u);
		return false;
	}
	uint perStrip = SltParticlePerStrip();
	uint at = SltParticleStripOffset(strip);
	sltParticleLists.InterlockedAdd(at + SLT_PARTICLE_STRIP_COUNT, 1u, place);
	if (place >= perStrip) {
		sltParticleLists.InterlockedAdd(at + SLT_PARTICLE_STRIP_COUNT, 0xFFFFFFFFu);
		sltParticleLists.InterlockedAdd(sltInstance.lists + SLT_PARTICLE_DROPPED_COUNT, 1u);
		return false;
	}
	index = strip * perStrip + (sltParticleLists.Load(at + SLT_PARTICLE_STRIP_FIRST) + place) % perStrip;
	return true;
}

void SltParticleStripLive(uint index) {
	uint at = SltParticleStripOffset(SltParticleStripOf(index));
	uint place = SltParticleStripPlace(index);
	sltParticleLists.InterlockedMin(at + SLT_PARTICLE_STRIP_LEAST, place);
	sltParticleLists.InterlockedMax(at + SLT_PARTICLE_STRIP_MOST, place);
}

bool SltParticleStripBegins(uint index) {
	return SltParticleStripPlace(index) + 1 < SltParticleStripWord(SltParticleStripOf(index), SLT_PARTICLE_STRIP_COUNT);
}

static const uint SLT_PARTICLE_EVENT_COUNT = 0;
static const uint SLT_PARTICLE_EVENT_ID_BASE = 4;
static const uint SLT_PARTICLE_EVENT_SOURCES = 8;

uint SltParticleEventListOffset(uint capacity, uint strips) {
	return SltParticleStripOffsetOf(capacity, strips);
}

uint SltParticleEventDispatchOffset(uint outputs) {
	return SLT_PARTICLE_DRAW_ARGUMENTS + outputs * 16;
}

uint SltParticleEventCount() {
	return min(sltParticleLists.Load(sltInstance.lists + SltParticleEventListOffset(sltParticle.capacity, sltParticle.stripCount) + SLT_PARTICLE_EVENT_COUNT),
			   sltParticle.capacity);
}

uint SltParticleEventSource(uint i) {
	return sltParticleLists.Load(sltInstance.lists + SltParticleEventListOffset(sltParticle.capacity, sltParticle.stripCount) + SLT_PARTICLE_EVENT_SOURCES + i * 4);
}

uint SltParticleEventIdBase() {
	return sltParticleLists.Load(sltInstance.lists + SltParticleEventListOffset(sltParticle.capacity, sltParticle.stripCount) + SLT_PARTICLE_EVENT_ID_BASE);
}

void SltParticleAppendEvents(RWByteAddressBuffer lists, RWByteAddressBuffer arguments, uint capacity, uint strips, uint outputs, uint index, uint count) {
	count = min(count, capacity);
	if (count == 0) {
		return;
	}
	uint at = SltParticleEventListOffset(capacity, strips);
	uint first;
	lists.InterlockedAdd(at + SLT_PARTICLE_EVENT_COUNT, count, first);
	uint end = min(first + count, capacity);
	for (uint slot = first; slot < end; ++slot) {
		lists.Store(at + SLT_PARTICLE_EVENT_SOURCES + slot * 4, index);
	}
	uint group = SLT_PARTICLE_KERNEL_GROUP;
	if (end > first) {
		uint groups = (end + group - 1) / group - (first + group - 1) / group;
		if (groups > 0) {
			arguments.InterlockedAdd(SltParticleEventDispatchOffset(outputs), groups);
		}
	}
	if (first + count > max(first, capacity)) {
		lists.InterlockedAdd(SLT_PARTICLE_DROPPED_COUNT, first + count - max(first, capacity));
	}
}

void SltParticleHoldForStrip(RWByteAddressBuffer lists, uint capacity, uint index) {
	lists.Store(SltParticleStripOffsetOf(capacity, index) + SLT_PARTICLE_STRIP_HELD, 1u);
}

static const uint SLT_PARTICLE_BOUNDS_BYTES = 24;
static const uint SLT_PARTICLE_BOUNDS_EMPTY_LOW = 0xFFFFFFFFu;

uint SltParticleBoundsOffset(uint which) {
	uint at = sltInstance.lists + SltParticleEventListOffset(sltParticle.capacity, sltParticle.stripCount);
	if (sltParticle.events != 0) {
		at += SLT_PARTICLE_EVENT_SOURCES + sltParticle.capacity * 4;
	}
	return at + which * SLT_PARTICLE_BOUNDS_BYTES;
}

uint SltParticleFinishedBoundsOffset() {
	return sltParticle.batchSlots * SltParticleSlotListsBytes() + sltInstance.slot * SLT_PARTICLE_BOUNDS_BYTES;
}

void SltParticleEmptyBoundsAt(uint at) {
	sltParticleLists.Store3(at, uint3(SLT_PARTICLE_BOUNDS_EMPTY_LOW, SLT_PARTICLE_BOUNDS_EMPTY_LOW, SLT_PARTICLE_BOUNDS_EMPTY_LOW));
	sltParticleLists.Store3(at + 12, uint3(0u, 0u, 0u));
}

void SltParticleEmptyBounds(uint which) {
	SltParticleEmptyBoundsAt(SltParticleBoundsOffset(which));
}

float SltParticleLoad(uint attribute, uint index) {
	return asfloat(sltParticleAttributes.Load(sltInstance.attributes + (attribute * sltParticle.capacity + index) * 4));
}

void SltParticleStore(uint attribute, uint index, float value) {
	sltParticleAttributes.Store(sltInstance.attributes + (attribute * sltParticle.capacity + index) * 4, asuint(value));
}

#endif
