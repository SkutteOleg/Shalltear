#ifndef SHALLTEAR_BUFFER_ACCESS_HLSLI
#define SHALLTEAR_BUFFER_ACCESS_HLSLI

#ifdef SHALLTEAR_WEBGPU

typedef uint2 slt_addr;
#define SLT_ADDR_ZERO uint2(0u, 0u)

bool ShalltearAddrValid(slt_addr a) {
	return (a.x | a.y) != 0u;
}

slt_addr ShalltearAddrAdd(slt_addr a, uint byteOffset) {
	return uint2(a.x + byteOffset, a.y);
}

slt_addr ShalltearAddrFromWords(uint lowWord, uint highWord) {
	return uint2(lowWord, highWord);
}

#define SLT_PUSH_CONSTANT(Type, Name) [[vk::binding(0, 3)]] cbuffer SltPushBlock { Type Name; };

[[vk::binding(1, 3)]] ByteAddressBuffer sltTransientRing;
#ifdef SHALLTEAR_VERTEX_HEAP_RW
[[vk::binding(2, 3)]] RWByteAddressBuffer sltVertexHeap;
#else
[[vk::binding(2, 3)]] ByteAddressBuffer sltVertexHeap;
#endif

uint ShalltearLoadU32(slt_addr a) {
	return a.y != 0u ? sltVertexHeap.Load(a.x) : sltTransientRing.Load(a.x);
}

uint2 ShalltearLoadU32x2(slt_addr a) {
	return a.y != 0u ? sltVertexHeap.Load2(a.x) : sltTransientRing.Load2(a.x);
}

uint3 ShalltearLoadU32x3(slt_addr a) {
	return a.y != 0u ? sltVertexHeap.Load3(a.x) : sltTransientRing.Load3(a.x);
}

uint4 ShalltearLoadU32x4(slt_addr a) {
	return a.y != 0u ? sltVertexHeap.Load4(a.x) : sltTransientRing.Load4(a.x);
}

float ShalltearLoadF32(slt_addr a) {
	return asfloat(ShalltearLoadU32(a));
}

float2 ShalltearLoadF32x2(slt_addr a) {
	return asfloat(ShalltearLoadU32x2(a));
}

float3 ShalltearLoadF32x3(slt_addr a) {
	return asfloat(ShalltearLoadU32x3(a));
}

float4 ShalltearLoadF32x4(slt_addr a) {
	return asfloat(ShalltearLoadU32x4(a));
}

slt_addr ShalltearLoadAddr(slt_addr a) {
	return ShalltearLoadU32x2(a);
}

#ifdef SHALLTEAR_VERTEX_HEAP_RW
void ShalltearStoreU32(slt_addr a, uint value) {
	sltVertexHeap.Store(a.x, value);
}

void ShalltearStoreF32x4(slt_addr a, float4 value) {
	sltVertexHeap.Store4(a.x, asuint(value));
}
#endif

#else

typedef uint64_t slt_addr;
#define SLT_ADDR_ZERO 0ull

bool ShalltearAddrValid(slt_addr a) {
	return a != 0ull;
}

slt_addr ShalltearAddrAdd(slt_addr a, uint byteOffset) {
	return a + uint64_t(byteOffset);
}

slt_addr ShalltearAddrFromWords(uint lowWord, uint highWord) {
	return uint64_t(lowWord) | (uint64_t(highWord) << 32);
}

#define SLT_PUSH_CONSTANT(Type, Name) [[vk::push_constant]] Type Name;

uint ShalltearLoadU32(slt_addr a) {
	return vk::RawBufferLoad<uint>(a, 4);
}

uint2 ShalltearLoadU32x2(slt_addr a) {
	return vk::RawBufferLoad<uint2>(a, 4);
}

uint3 ShalltearLoadU32x3(slt_addr a) {
	return vk::RawBufferLoad<uint3>(a, 4);
}

uint4 ShalltearLoadU32x4(slt_addr a) {
	return vk::RawBufferLoad<uint4>(a, 4);
}

float ShalltearLoadF32(slt_addr a) {
	return vk::RawBufferLoad<float>(a, 4);
}

float2 ShalltearLoadF32x2(slt_addr a) {
	return vk::RawBufferLoad<float2>(a, 4);
}

float3 ShalltearLoadF32x3(slt_addr a) {
	return vk::RawBufferLoad<float3>(a, 4);
}

float4 ShalltearLoadF32x4(slt_addr a) {
	return vk::RawBufferLoad<float4>(a, 4);
}

slt_addr ShalltearLoadAddr(slt_addr a) {
	return vk::RawBufferLoad<uint64_t>(a, 8);
}

void ShalltearStoreU32(slt_addr a, uint value) {
	vk::RawBufferStore<uint>(a, value, 4);
}

void ShalltearStoreF32x4(slt_addr a, float4 value) {
	vk::RawBufferStore<float4>(a, value, 4);
}

#endif

#endif
