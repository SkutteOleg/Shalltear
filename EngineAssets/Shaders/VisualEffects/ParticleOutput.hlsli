#ifndef SHALLTEAR_PARTICLE_OUTPUT_HLSLI
#define SHALLTEAR_PARTICLE_OUTPUT_HLSLI

#include "ShalltearBufferAccess.hlsli"
#include "ViewData.hlsli"

struct SltParticleOutputFrame {
	slt_addr lists;
	slt_addr attributes;
	uint capacity;
	uint list;
	uint vertices;
	uint frame;
	slt_addr effect;
	slt_addr order;
	uint pass;
	uint particlesPerStrip;
	uint value;
	uint reserved;
};
SLT_PUSH_CONSTANT(SltParticleOutputFrame, sltParticleOutput)

static const uint SLT_PARTICLE_PASS_SHADOW = 2;
bool SltParticleShadowPass() {
	return sltParticleOutput.pass == SLT_PARTICLE_PASS_SHADOW;
}

slt_addr SltParticleEffectAddress() {
	return sltParticleOutput.effect;
}

uint SltParticleFrameIndex() {
	return sltParticleOutput.frame;
}

#include "ParticleEffect.hlsli"

static const uint SLT_PARTICLE_OUTPUT_LISTS = 16;

uint SltParticleOutputAt(uint slot) {
	if (ShalltearAddrValid(sltParticleOutput.order)) {
		return ShalltearLoadU32(ShalltearAddrAdd(sltParticleOutput.order, slot * 4u));
	}
	uint entry = sltParticleOutput.list * sltParticleOutput.capacity + slot;
	return ShalltearLoadU32(ShalltearAddrAdd(sltParticleOutput.lists, SLT_PARTICLE_OUTPUT_LISTS + entry * 4u));
}

uint SltParticleOutputIndex(uint vertexId) {
	return SltParticleOutputAt(vertexId / max(sltParticleOutput.vertices, 1u));
}

uint SltParticleOutputInstance(uint instanceId) {
	return SltParticleOutputAt(instanceId);
}

static const float4 SLT_PARTICLE_CULLED = float4(2.0, 2.0, 0.5, 1.0);

float3 SltParticleWorldFromClip(float4 positionCS, ShalltearViewData view) {
	float4 world = mul(ShalltearViewDataMatrix(view.inverseViewProjection[0], view.inverseViewProjection[1], view.inverseViewProjection[2],
											   view.inverseViewProjection[3]),
					   positionCS);
	return world.xyz / world.w;
}

uint SltParticleOutputCorner(uint vertexId) {
	return vertexId % max(sltParticleOutput.vertices, 1u);
}

float SltParticleOutputLoad(uint attribute, uint index) {
	uint element = attribute * sltParticleOutput.capacity + index;
	return ShalltearLoadF32(ShalltearAddrAdd(sltParticleOutput.attributes, element * 4u));
}

uint SltParticleOutputStripWord(uint strip, uint field) {
	uint at = SLT_PARTICLE_OUTPUT_LISTS + 3u * sltParticleOutput.capacity * 4u + strip * 24u + field;
	return ShalltearLoadU32(ShalltearAddrAdd(sltParticleOutput.lists, at));
}

uint SltParticleOutputPerStrip() {
	return max(sltParticleOutput.particlesPerStrip, 1u);
}

uint SltParticleOutputStripOf(uint index) {
	return index / SltParticleOutputPerStrip();
}

uint SltParticleOutputStripCount(uint strip) {
	return SltParticleOutputStripWord(strip, 4);
}

uint SltParticleOutputStripPlace(uint index) {
	uint perStrip = SltParticleOutputPerStrip();
	uint strip = index / perStrip;
	return (perStrip + index - strip * perStrip - SltParticleOutputStripWord(strip, 0)) % perStrip;
}

uint SltParticleOutputStripIndex(uint strip, uint place) {
	uint perStrip = SltParticleOutputPerStrip();
	return strip * perStrip + (SltParticleOutputStripWord(strip, 0) + place) % perStrip;
}

bool SltParticleOutputSegment(uint entry, out uint index) {
	if (ShalltearAddrValid(sltParticleOutput.order)) {
		index = ShalltearLoadU32(ShalltearAddrAdd(sltParticleOutput.order, entry * 4u));
		return index < sltParticleOutput.capacity;
	}
	uint perStrip = SltParticleOutputPerStrip();
	uint strip = entry / perStrip;
	uint place = entry - strip * perStrip;
	index = SltParticleOutputStripIndex(strip, place);
	return place + 1 < SltParticleOutputStripCount(strip);
}

// w = 1 where a neighbor exists, 0 past strip end
float4 SltParticleOutputStripNeighbor(uint index, bool next, uint slot) {
	uint strip = SltParticleOutputStripOf(index);
	uint place = SltParticleOutputStripPlace(index);
	if (next ? place + 1 >= SltParticleOutputStripCount(strip) : place == 0) {
		return float4(0.0, 0.0, 0.0, 0.0);
	}
	uint neighbor = SltParticleOutputStripIndex(strip, next ? place + 1 : place - 1);
	return float4(SltParticleOutputLoad(slot, neighbor), SltParticleOutputLoad(slot + 1, neighbor), SltParticleOutputLoad(slot + 2, neighbor), 1.0);
}

float3 SltParticleStripSize(float size, float3 scale) {
	float3 size3 = float3(size, size * scale.y, size);
	return size3 + (step(0.0, size3) * 2.0 - 1.0) * 1e-5;
}

float2 SltParticleQuadCorner(uint corner) {
	float x = (corner == 2 || corner == 3 || corner == 5) ? 1.0 : -1.0;
	float y = (corner == 1 || corner == 4 || corner == 5) ? 1.0 : -1.0;
	return float2(x, y);
}

float2 SltParticleTriangleCorner(uint corner) {
	uint at = corner % 3;
	return at == 0 ? float2(-0.5, -0.288675129413604736328125) : (at == 1 ? float2(0.0, 0.57735025882720947265625) : float2(0.5, -0.288675129413604736328125));
}

float2 SltParticleOctagonCorner(uint corner, float crop) {
	uint at = corner % 3;
	if (at == 0) {
		return float2(0.0, 0.0);
	}
	uint vertex = ((corner / 3) + at - 1) & 7;
	static const float2 OUTLINE[8] = {float2(-0.5, 0.0), float2(-0.5, 0.5), float2(0.0, 0.5), float2(0.5, 0.5),
									  float2(0.5, 0.0),	 float2(0.5, -0.5), float2(0.0, -0.5), float2(-0.5, -0.5)};
	return OUTLINE[vertex] * ((vertex & 1) != 0 ? 1.0 - crop : 1.0);
}

float4 SltParticleClipOnNearPlane(float4 at, float4 other, float nearPlane) {
	if (at.w >= nearPlane || other.w < nearPlane) {
		return at;
	}
	float ratio = (nearPlane - at.w) / (other.w - at.w);
	return at + (other - at) * ratio;
}

float4 SltParticleLineCorner(float4 start, float4 end, uint corner, float4x4 projection, ShalltearViewData view, float thickness, out float pixelOffset) {
	float4 a = SltParticleClipOnNearPlane(start, end, view.projectionParams.x);
	float4 b = SltParticleClipOnNearPlane(end, start, view.projectionParams.x);
	float2 direction = a.xy / a.w - b.xy / b.w;
	float2 normal = normalize(direction.yx * float2(-1.0, abs(projection[1][1]) / projection[0][0]));
	normal *= thickness / view.screenParams.xy;
	static const uint CORNERS[6] = {0, 1, 2, 2, 1, 3};
	uint at = CORNERS[corner % 6];
	float side = (at & 1) != 0 ? -1.0 : 1.0;
	pixelOffset = side;
	float4 endpoint = (at >> 1) != 0 ? b : a;
	return endpoint + float4(normal * endpoint.w * side, 0.0, 0.0);
}

// rads, applied Z -> X -> Y
float3x3 SltParticleEulerMatrix(float3 angles) {
	float3 s, c;
	sincos(angles, s, c);
	return float3x3(c.y * c.z + s.x * s.y * s.z, c.z * s.x * s.y - c.y * s.z, c.x * s.y,
					c.x * s.z, c.x * c.z, -s.x,
					-c.z * s.y + c.y * s.x * s.z, c.y * c.z * s.x + s.y * s.z, c.x * c.y);
}

// degrees, quad corners at +/- 1/2
float3x3 SltParticleElementMatrix(float3 axisX, float3 axisY, float3 axisZ, float3 angles, float3 size) {
	float3x3 rotAndScale = mul(SltParticleEulerMatrix(radians(angles)), float3x3(size.x, 0.0, 0.0, 0.0, size.y, 0.0, 0.0, 0.0, size.z));
	return mul(transpose(float3x3(axisX, axisY, axisZ)), rotAndScale);
}

float3 SltParticleElementPosition(float3 axisX, float3 axisY, float3 axisZ, float3 angles, float3 pivot, float3 size, float3 position, float3 offset) {
	return position + mul(SltParticleElementMatrix(axisX, axisY, axisZ, angles, size), offset - pivot);
}

void SltParticleBendNormal(float2 bent, inout float3 normalWS, inout float3 tangentWS, float3 bitangentWS) {
	float3 factors = float3(bent, sqrt(max(0.0, 1.0 - dot(bent, bent))));
	normalWS = normalize(tangentWS * factors.x + bitangentWS * factors.y + normalWS * factors.z);
	tangentWS = normalize(cross(normalWS, bitangentWS));
}

float3 SltParticleTransformNormal(float3x3 m, float3 normal) {
	float3x3 columns = transpose(m);
	float3 turned = cross(columns[1], columns[2]) * normal.x + cross(columns[2], columns[0]) * normal.y + cross(columns[0], columns[1]) * normal.z;
	return normalize(turned) * (dot(columns[0], cross(columns[1], columns[2])) < 0.0 ? -1.0 : 1.0);
}

float2 SltParticleTextureUV(float2 bottomUpUV) {
	return float2(bottomUpUV.x, 1.0 - bottomUpUV.y);
}

float2 SltParticleSubUV(float index, float2 uv, float2 size) {
	float2 inverse = 1.0 / size;
	float2 tile = float2(fmod(index, size.x), size.y - 1.0 - floor(index * inverse.x));
	return SltParticleTextureUV((tile + uv) * inverse);
}

float4 SltParticleFlipbookUVs(float texIndex, float texIndexBlend, float2 size, float2 uv, out float blend) {
	blend = frac(texIndex);
	float frame = texIndex - blend;
	return float4(SltParticleSubUV(frame, uv, size), SltParticleSubUV(frame + texIndexBlend, uv, size));
}

float SltParticleViewDepth(float depth, ShalltearViewData view) {
	float nearPlane = view.projectionParams.x;
	float farPlane = view.projectionParams.y;
	if (view.projectionParams.z > 0.5) {
		return farPlane - depth * (farPlane - nearPlane);
	}
	return nearPlane * farPlane / (depth * (farPlane - nearPlane) + nearPlane);
}

float SltParticleSoftFade(float sceneDepth, float viewDepth, ShalltearViewData view, float distance) {
	float fade = saturate((SltParticleViewDepth(sceneDepth, view) - viewDepth) / max(distance, 1e-5));
	return fade * fade * (3.0 - 2.0 * fade);
}

float4 SltParticleClipWRow(ShalltearViewData view) {
	float4x4 world = ShalltearViewDataMatrix(view.view[0], view.view[1], view.view[2], view.view[3]);
	float4x4 projection = ShalltearViewDataMatrix(view.projection[0], view.projection[1], view.projection[2], view.projection[3]);
	return mul(projection, world)[3];
}

#endif
