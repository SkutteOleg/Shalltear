#ifndef SHALLTEAR_TEMPLATE_VERTEX_HLSLI
#define SHALLTEAR_TEMPLATE_VERTEX_HLSLI

#ifndef SHALLTEAR_INTERPOLATORS_LINEAR
#define SHALLTEAR_INTERPOLATORS_LINEAR 0
#endif
#ifndef SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE
#define SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE 0
#endif
#ifndef SHALLTEAR_INTERPOLATORS_FLAT
#define SHALLTEAR_INTERPOLATORS_FLAT 0
#endif

#define SHALLTEAR_INTERPOLATORS_FIRST_NO_PERSPECTIVE SHALLTEAR_INTERPOLATORS_LINEAR
#define SHALLTEAR_INTERPOLATORS_FIRST_FLAT (SHALLTEAR_INTERPOLATORS_LINEAR + SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE)
#define SHALLTEAR_INTERPOLATORS_TOTAL (SHALLTEAR_INTERPOLATORS_FIRST_FLAT + SHALLTEAR_INTERPOLATORS_FLAT)

#if SHALLTEAR_INTERPOLATORS_TOTAL > SHALLTEAR_MAX_CUSTOM_INTERPOLATORS
#error "a surface shader declares more custom interpolators than SHALLTEAR_MAX_CUSTOM_INTERPOLATORS"
#endif
#if SHALLTEAR_INTERPOLATORS_TOTAL > 0 && !defined(SHALLTEAR_VERTEX_FUNCTION)
#error "a surface shader declares custom interpolators and no SHALLTEAR_VERTEX_FUNCTION to write them"
#endif

SLT_PUSH_CONSTANT(ShalltearDrawHeader, gDraw)

struct ShalltearVaryings {
	float4 positionCS : SV_Position;
	float3 positionWS : TEXCOORD0;
	float3 normalWS : TEXCOORD1;
	float4 tangentWS : TEXCOORD2;
	centroid float2 uv : TEXCOORD3;
	centroid float4 color : COLOR0;
#if SHALLTEAR_INTERPOLATORS_LINEAR > 0
	float4 interpolatorsLinear[SHALLTEAR_INTERPOLATORS_LINEAR] : SHALLTEAR_LINEAR;
#endif
#if SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE > 0
	noperspective float4 interpolatorsNoPerspective[SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE] : SHALLTEAR_NO_PERSPECTIVE;
#endif
#if SHALLTEAR_INTERPOLATORS_FLAT > 0
	nointerpolation float4 interpolatorsFlat[SHALLTEAR_INTERPOLATORS_FLAT] : SHALLTEAR_FLAT;
#endif
	nointerpolation uint instance : SHALLTEAR_INSTANCE;
};

slt_addr ShalltearVaryingsDrawAddress(ShalltearVaryings input) {
	return ShalltearInstanceDrawAddress(gDraw.drawAddress, input.instance);
}

ShalltearVaryings VSMain(uint vertexId : SV_VertexID, uint instance : SV_InstanceID) {
	ShalltearPerDraw draw = ShalltearLoadPerDraw(ShalltearInstanceDrawAddress(gDraw.drawAddress, instance));
	ShalltearVertex vertex = ShalltearLoadVertex(draw.vertexAddress, vertexId);

	ShalltearVaryings output;
	output.instance = instance;
#ifdef SHALLTEAR_VERTEX_FUNCTION
	ShalltearVertexInput vertexInput = ShalltearBuildVertexInput(draw, vertex, vertexId);
	ShalltearVertexOutput displaced = ShalltearDefaultVertexOutput(vertexInput);
	ShalltearVertexFunction(vertexInput, displaced);
	vertex.position = displaced.positionOS;
	vertex.normal = displaced.normalOS;
	vertex.tangent.xyz = displaced.tangentOS;
#if SHALLTEAR_INTERPOLATORS_LINEAR > 0
	[unroll] for (uint linearSlot = 0; linearSlot < SHALLTEAR_INTERPOLATORS_LINEAR; ++linearSlot) {
		output.interpolatorsLinear[linearSlot] = displaced.interpolators[linearSlot];
	}
#endif
#if SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE > 0
	[unroll] for (uint noPerspectiveSlot = 0; noPerspectiveSlot < SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE; ++noPerspectiveSlot) {
		output.interpolatorsNoPerspective[noPerspectiveSlot] = displaced.interpolators[SHALLTEAR_INTERPOLATORS_FIRST_NO_PERSPECTIVE + noPerspectiveSlot];
	}
#endif
#if SHALLTEAR_INTERPOLATORS_FLAT > 0
	[unroll] for (uint flatSlot = 0; flatSlot < SHALLTEAR_INTERPOLATORS_FLAT; ++flatSlot) {
		output.interpolatorsFlat[flatSlot] = displaced.interpolators[SHALLTEAR_INTERPOLATORS_FIRST_FLAT + flatSlot];
	}
#endif
#endif

	float4 positionWS = ShalltearTransformPosition(draw, vertex.position);
	output.positionWS = positionWS.xyz;
	output.positionCS = mul(sltViewProjection, positionWS);
	output.normalWS = ShalltearTransformNormal(draw.normalMatrix0, draw.normalMatrix1, draw.normalMatrix2, vertex.normal);
	// w = sign, flipped when the draw mirrors
	output.tangentWS.xyz = ShalltearTransformNormal(draw.normalMatrix0, draw.normalMatrix1, draw.normalMatrix2, vertex.tangent.xyz);
	output.tangentWS.w = vertex.tangent.w * ShalltearDrawHandedness(draw);
	output.uv = float2(vertex.u, vertex.v);
	output.color = ShalltearUnpackColor(vertex.color);
	return output;
}

ShalltearSurfaceInput ShalltearBuildPixelSurfaceInput(ShalltearVaryings input, bool isFrontFace) {
	ShalltearSurfaceInput surfaceInput = ShalltearBuildSurfaceInput(input.positionCS, input.positionWS, input.normalWS, input.tangentWS,
																	input.uv, input.color, ShalltearVaryingsDrawAddress(input));
	surfaceInput.isFrontFace = isFrontFace;
#if SHALLTEAR_INTERPOLATORS_LINEAR > 0
	[unroll] for (uint linearSlot = 0; linearSlot < SHALLTEAR_INTERPOLATORS_LINEAR; ++linearSlot) {
		surfaceInput.interpolators[linearSlot] = input.interpolatorsLinear[linearSlot];
	}
#endif
#if SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE > 0
	[unroll] for (uint noPerspectiveSlot = 0; noPerspectiveSlot < SHALLTEAR_INTERPOLATORS_NO_PERSPECTIVE; ++noPerspectiveSlot) {
		surfaceInput.interpolators[SHALLTEAR_INTERPOLATORS_FIRST_NO_PERSPECTIVE + noPerspectiveSlot] = input.interpolatorsNoPerspective[noPerspectiveSlot];
	}
#endif
#if SHALLTEAR_INTERPOLATORS_FLAT > 0
	[unroll] for (uint flatSlot = 0; flatSlot < SHALLTEAR_INTERPOLATORS_FLAT; ++flatSlot) {
		surfaceInput.interpolators[SHALLTEAR_INTERPOLATORS_FIRST_FLAT + flatSlot] = input.interpolatorsFlat[flatSlot];
	}
#endif
	return surfaceInput;
}

#endif
