#include "Surface.hlsli"
#include "Brdf.hlsli"
#include "Fog.hlsli"

#include SHALLTEAR_SURFACE_FILE

#include "Vertex.hlsli"

float4 PSMain(ShalltearVaryings input, bool isFrontFace : SV_IsFrontFace) : SV_Target {
	ShalltearSurfaceInput surfaceInput = ShalltearBuildPixelSurfaceInput(input, isFrontFace);
	const slt_addr drawAddress = ShalltearVaryingsDrawAddress(input);

	ShalltearSurface surface = ShalltearDefaultSurface();
	ShalltearSurfaceFunction(surfaceInput, surface);

#ifdef SHALLTEAR_ALPHA_CLIP
	clip(surface.alpha - ShalltearLoadAlphaClipThreshold(drawAddress));
#endif
	ShalltearClipLODFade(ShalltearLoadDrawFlags(drawAddress), input.positionCS.xy);

	uint blend = ShalltearLoadDrawBlend(drawAddress);
	return ShalltearExpose(ShalltearApplyFog(float4(surface.baseColor + surface.emissive, surface.alpha), surfaceInput.positionWS, blend),
						   blend);
}
