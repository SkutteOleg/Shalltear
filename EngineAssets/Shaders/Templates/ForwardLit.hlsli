#include "Surface.hlsli"
#include "Brdf.hlsli"
#include "Lights.hlsli"
#include "Shadows.hlsli"
#include "Environment.hlsli"
#include "Fog.hlsli"
#include "Decals.hlsli"

// after the library, before entry points
#include SHALLTEAR_SURFACE_FILE

#include "Vertex.hlsli"

// after the surface file (custom lighting in scope)
#include "Shading.hlsli"

float4 PSMain(ShalltearVaryings input, bool isFrontFace : SV_IsFrontFace) : SV_Target {
	ShalltearSurfaceInput surfaceInput = ShalltearBuildPixelSurfaceInput(input, isFrontFace);
	const slt_addr drawAddress = ShalltearVaryingsDrawAddress(input);

	ShalltearSurface surface = ShalltearDefaultSurface();
	ShalltearSurfaceFunction(surfaceInput, surface);

#ifdef SHALLTEAR_ALPHA_CLIP
	clip(surface.alpha - ShalltearLoadAlphaClipThreshold(drawAddress));
#endif
	ShalltearClipLODFade(ShalltearLoadDrawFlags(drawAddress), input.positionCS.xy);

	float3 normalWS = ShalltearTransformSurfaceNormal(surfaceInput, surface.normal);

	uint drawFlags = ShalltearLoadDrawFlags(drawAddress);
	float receivesShadows = (drawFlags & SHALLTEAR_DRAW_RECEIVES_SHADOWS) != 0 ? 1.0 : 0.0;
	uint drawLayers = (drawFlags >> SHALLTEAR_DRAW_LAYERS_SHIFT) & 0xFFu;
	ShalltearApplyDecals(surfaceInput, surface, normalWS, drawLayers, ddx(surfaceInput.positionWS), ddy(surfaceInput.positionWS));

	float4 color = float4(ShalltearShadeSurface(surfaceInput, surface, normalWS, receivesShadows, drawLayers) + surface.emissive, surface.alpha);
	uint blend = ShalltearLoadDrawBlend(drawAddress);
	return ShalltearExpose(ShalltearApplyFog(color, surfaceInput.positionWS, blend), blend);
}
