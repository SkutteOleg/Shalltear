#ifndef SHALLTEAR_SURFACE_HLSLI
#define SHALLTEAR_SURFACE_HLSLI

#include "ShalltearCommon.hlsli"

#define SHALLTEAR_MAX_CUSTOM_INTERPOLATORS 16

// contract struct, additions only
struct ShalltearSurfaceInput {
	float3 positionWS;
	float3 normalWS; // geometric, normalized (not the normal map)
	float4 tangentWS;
	float2 uv;
	float4 color;
	float3 viewDirectionWS;

	// pixel center, y down
	float2 positionSS;
	bool isFrontFace;
	float4x4 objectToWorld;
	float4x4 worldToObject;

	float4 interpolators[SHALLTEAR_MAX_CUSTOM_INTERPOLATORS];
};

struct ShalltearVertexInput {
	float3 positionOS;
	float3 normalOS;
	float4 tangentOS;
	float2 uv;
	float4 color;
	uint vertexId;

	float3 positionWS;
	float3 normalWS;
	float4 tangentWS;
	float3 viewDirectionWS;
	float4x4 objectToWorld;
	float4x4 worldToObject;
};

struct ShalltearVertexOutput {
	float3 positionOS;
	float3 normalOS;
	float3 tangentOS;
	float4 interpolators[SHALLTEAR_MAX_CUSTOM_INTERPOLATORS];
};

struct ShalltearSurface {
	float3 baseColor; // linear (not premultiplied)
	float alpha;
	float metallic;
	float roughness; // perceptual (BRDF squares it)
	float reflectance; // dielectric F0, 0.5 = 4%
	float3 normal; // tangent space, +Z = geometric normal
	float occlusion;
	float3 emissive; // linear radiance added after lighting

	// reserved
	float clearCoat;
	float clearCoatRoughness;
	float3 anisotropyDirection; // tangent space
	float anisotropy;
	float3 sheenColor;
	float3 subsurfaceColor;
	float thickness;
};

ShalltearSurface ShalltearDefaultSurface() {
	ShalltearSurface surface;
	surface.baseColor = float3(1.0, 1.0, 1.0);
	surface.alpha = 1.0;
	surface.metallic = 0.0;
	surface.roughness = 1.0;
	surface.reflectance = 0.5;
	surface.normal = float3(0.0, 0.0, 1.0);
	surface.occlusion = 1.0;
	surface.emissive = float3(0.0, 0.0, 0.0);
	surface.clearCoat = 0.0;
	surface.clearCoatRoughness = 0.0;
	surface.anisotropyDirection = float3(1.0, 0.0, 0.0);
	surface.anisotropy = 0.0;
	surface.sheenColor = float3(0.0, 0.0, 0.0);
	surface.subsurfaceColor = float3(0.0, 0.0, 0.0);
	surface.thickness = 0.0;
	return surface;
}

float4x4 ShalltearDrawObjectToWorld(ShalltearPerDraw draw) {
	return transpose(float4x4(draw.world0, draw.world1, draw.world2, draw.world3));
}

float4x4 ShalltearDrawWorldToObject(ShalltearPerDraw draw) {
	float3 translation = draw.world3.xyz;
	return float4x4(float4(draw.normalMatrix0.xyz, -dot(draw.normalMatrix0.xyz, translation)),
					float4(draw.normalMatrix1.xyz, -dot(draw.normalMatrix1.xyz, translation)),
					float4(draw.normalMatrix2.xyz, -dot(draw.normalMatrix2.xyz, translation)), float4(0.0, 0.0, 0.0, 1.0));
}

ShalltearSurfaceInput ShalltearBuildSurfaceInput(float4 positionCS, float3 positionWS, float3 normalWS, float4 tangentWS, float2 uv,
												  float4 color, slt_addr drawAddress) {
	ShalltearSurfaceInput input;
	input.positionWS = positionWS;
	input.normalWS = normalize(normalWS);
	input.tangentWS = tangentWS;
	input.uv = uv;
	input.color = color;
	input.viewDirectionWS = ShalltearViewDirection(positionWS);
	input.positionSS = positionCS.xy;
	input.isFrontFace = true;

	ShalltearPerDraw draw = ShalltearLoadPerDraw(drawAddress);
	input.objectToWorld = ShalltearDrawObjectToWorld(draw);
	input.worldToObject = ShalltearDrawWorldToObject(draw);
	[unroll] for (uint slot = 0; slot < SHALLTEAR_MAX_CUSTOM_INTERPOLATORS; ++slot) {
		input.interpolators[slot] = float4(0.0, 0.0, 0.0, 0.0);
	}
	return input;
}

ShalltearVertexInput ShalltearBuildVertexInput(ShalltearPerDraw draw, ShalltearVertex vertex, uint vertexId) {
	ShalltearVertexInput input;
	input.positionOS = vertex.position;
	input.normalOS = vertex.normal;
	input.tangentOS = vertex.tangent;
	input.uv = float2(vertex.u, vertex.v);
	input.color = ShalltearUnpackColor(vertex.color);
	input.vertexId = vertexId;

	input.positionWS = ShalltearTransformPosition(draw, vertex.position).xyz;
	input.normalWS = ShalltearTransformNormal(draw.normalMatrix0, draw.normalMatrix1, draw.normalMatrix2, vertex.normal);
	input.tangentWS.xyz = ShalltearTransformNormal(draw.normalMatrix0, draw.normalMatrix1, draw.normalMatrix2, vertex.tangent.xyz);
	input.tangentWS.w = vertex.tangent.w * ShalltearDrawHandedness(draw);
	input.viewDirectionWS = ShalltearViewDirection(input.positionWS);
	input.objectToWorld = ShalltearDrawObjectToWorld(draw);
	input.worldToObject = ShalltearDrawWorldToObject(draw);
	return input;
}

ShalltearVertexOutput ShalltearDefaultVertexOutput(ShalltearVertexInput input) {
	ShalltearVertexOutput output;
	output.positionOS = input.positionOS;
	output.normalOS = input.normalOS;
	output.tangentOS = input.tangentOS.xyz;
	[unroll] for (uint slot = 0; slot < SHALLTEAR_MAX_CUSTOM_INTERPOLATORS; ++slot) {
		output.interpolators[slot] = float4(0.0, 0.0, 0.0, 0.0);
	}
	return output;
}

float3 ShalltearUnpackNormalMap(float4 sampled, float scale) {
	float3 normal;
	normal.xy = (sampled.xy * 2.0 - 1.0) * scale;
	normal.z = sqrt(saturate(1.0 - dot(normal.xy, normal.xy)));
	return normal;
}

float3 ShalltearTransformSurfaceNormal(ShalltearSurfaceInput input, float3 tangentSpaceNormal) {
	float3 normal = normalize(input.normalWS);
	float3 tangent = input.tangentWS.xyz - normal * dot(normal, input.tangentWS.xyz);
	float tangentLength = length(tangent);
	if (tangentLength < 1e-6) {
		return normal;
	}
	tangent /= tangentLength;
	float3 bitangent = cross(normal, tangent) * input.tangentWS.w;
	return normalize(tangentSpaceNormal.x * tangent + tangentSpaceNormal.y * bitangent + tangentSpaceNormal.z * normal);
}

#endif
