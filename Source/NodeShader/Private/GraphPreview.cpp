#include "GraphPreview.h"

#include "GraphDefinitions.h"
#include "GraphHLSL.h"
#include "GraphNodeLibrary.h"
#include "GraphProperties.h"
#include "GraphSurface.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Editor/GraphDocument.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Shalltear {

	namespace {

		constexpr std::string_view PreviewInputs = R"(struct ShalltearPreviewInput
{
	float2 uv;
	float3 positionWS;
	float3 normalWS;
	float3 tangentWS;
	float3 bitangentWS;
	float3 viewDirectionWS;
	float3 cameraWS;
	float4x4 view;
	float4x4 inverseView;
	float4x4 projection;
	float4x4 inverseProjection;
	float4 projectionParams;
	float4 screenParams;
	bool hit;
};

ShalltearPreviewInput ShalltearPreviewBuild(float2 tileUV, bool threeD)
{
	ShalltearPreviewInput p;
	p.hit = true;
	if (!threeD)
	{
		p.uv = tileUV;
		p.positionWS = float3(tileUV.x - 0.5, 0.5 - tileUV.y, 0.0);
		p.normalWS = float3(0.0, 0.0, -1.0);
		p.tangentWS = float3(1.0, 0.0, 0.0);
		p.bitangentWS = float3(0.0, 1.0, 0.0);
		p.cameraWS = float3(0.0, 0.0, -2.0);
		p.viewDirectionWS = float3(0.0, 0.0, -1.0);
		p.view = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 2.0, 0.0, 0.0, 0.0, 1.0);
		p.inverseView = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -2.0, 0.0, 0.0, 0.0, 1.0);
		p.projection = float4x4(2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, -0.125, 1.25, 0.0, 0.0, 0.0, 1.0);
		p.inverseProjection = float4x4(0.5, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, -8.0, 10.0, 0.0, 0.0, 0.0, 1.0);
		p.projectionParams = float4(2.0, 10.0, 1.0, 0.0);
		p.screenParams = float4(128.0, 128.0, 0.5, 0.5);
		return p;
	}
	// a ray through the pixel from the camera at z = -5, tan(7.5 degrees)
	// across half the tile
	const float tanHalfFov = 0.13165249758739583;
	float3 origin = float3(0.0, 0.0, -5.0);
	float3 direction = normalize(float3((tileUV.x * 2.0 - 1.0) * tanHalfFov, (1.0 - tileUV.y * 2.0) * tanHalfFov, 1.0));
	float b = dot(origin, direction);
	float h = b * b - (dot(origin, origin) - 0.25);
	if (h < 0.0)
	{
		p = (ShalltearPreviewInput)0;
		p.hit = false;
		return p;
	}
	float3 position = origin + direction * (-b - sqrt(h));
	float3 normal = normalize(position);
	p.positionWS = position;
	p.normalWS = normal;
	// longitude and latitude, a UV sphere's: u eastward round the equator
	// from the back, v down from the north pole
	p.uv = float2(atan2(normal.x, -normal.z) * 0.15915494309189535 + 0.5, acos(clamp(normal.y, -1.0, 1.0)) * 0.3183098861837907);
	float3 east = float3(-normal.z, 0.0, normal.x);
	float eastLength = length(east);
	p.tangentWS = eastLength > 1e-6 ? east / eastLength : float3(1.0, 0.0, 0.0);
	p.bitangentWS = cross(p.tangentWS, normal);
	p.cameraWS = origin;
	p.viewDirectionWS = normalize(origin - position);
	p.view = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 5.0, 0.0, 0.0, 0.0, 1.0);
	p.inverseView = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -5.0, 0.0, 0.0, 0.0, 1.0);
	p.projection = float4x4(7.595754112725151, 0.0, 0.0, 0.0, 0.0, 7.595754112725151, 0.0, 0.0, 0.0, 0.0, -0.25, 2.5, 0.0, 0.0, 1.0, 0.0);
	p.inverseProjection = float4x4(0.13165249758739583, 0.0, 0.0, 0.0, 0.0, 0.13165249758739583, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.4, 0.1);
	p.projectionParams = float4(2.0, 10.0, 0.0, 0.0);
	p.screenParams = float4(128.0, 128.0, 0.13165249758739583, 0.13165249758739583);
	return p;
}

)";

		constexpr std::string_view Identity = "float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)";

		constexpr std::string_view PreviewSun = "normalize(float3(-0.45, 0.8, -0.4))";
		// 20 degrees in rads
		constexpr f32 PREVIEW_SUN_DIAMETER = 0.34906585f;

		const GraphBinding previewBindings[] = {
			{"UV0", "pv.uv", GraphValueType::Float2, GraphStageFragment},
			{"VertexColor", "float4(1.0, 1.0, 1.0, 1.0)", GraphValueType::Float4, GraphStageFragment},

			{"WorldSpacePosition", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpacePosition", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpacePosition", "mul(pv.view, float4(pv.positionWS, 1.0)).xyz", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpacePosition", "float3(0.0, 0.0, 0.0)", GraphValueType::Float3, GraphStageFragment},
			{"AbsoluteWorldSpacePosition", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},
			{"WorldSpacePositionPredisplacement", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpacePositionPredisplacement", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpacePositionPredisplacement", "mul(pv.view, float4(pv.positionWS, 1.0)).xyz", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpacePositionPredisplacement", "float3(0.0, 0.0, 0.0)", GraphValueType::Float3, GraphStageFragment},
			{"AbsoluteWorldSpacePositionPredisplacement", "pv.positionWS", GraphValueType::Float3, GraphStageFragment},

			{"LightDirection", str(PreviewSun), GraphValueType::Float3, GraphStageFragment},
			{"LightColor", "float3(1.0, 1.0, 1.0)", GraphValueType::Float3, GraphStageFragment},
			{"LightAttenuation", "1.0", GraphValueType::Float, GraphStageFragment},
			{"LightShadow", "1.0", GraphValueType::Float, GraphStageFragment},
			{"SurfaceBaseColor", "float3(1.0, 1.0, 1.0)", GraphValueType::Float3, GraphStageFragment},
			{"SurfaceAlpha", "1.0", GraphValueType::Float, GraphStageFragment},
			{"SurfaceMetallic", "0.0", GraphValueType::Float, GraphStageFragment},
			{"SurfaceRoughness", "1.0", GraphValueType::Float, GraphStageFragment},
			{"SurfaceReflectance", "0.5", GraphValueType::Float, GraphStageFragment},
			{"SurfaceOcclusion", "1.0", GraphValueType::Float, GraphStageFragment},
			{"SurfaceEmission", "float3(0.0, 0.0, 0.0)", GraphValueType::Float3, GraphStageFragment},
			{"SurfaceNormal", "pv.normalWS", GraphValueType::Float3, GraphStageFragment},
			{"StandardRadiance", "(saturate(dot(pv.normalWS, " + str(PreviewSun) + ")) * 0.3183098861837907).xxx", GraphValueType::Float3,
			 GraphStageFragment},

			{"WorldSpaceNormal", "pv.normalWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpaceNormal", "pv.normalWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpaceNormal", "mul((float3x3)pv.view, pv.normalWS)", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpaceNormal", "float3(0.0, 0.0, 1.0)", GraphValueType::Float3, GraphStageFragment},

			{"WorldSpaceTangent", "pv.tangentWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpaceTangent", "pv.tangentWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpaceTangent", "mul((float3x3)pv.view, pv.tangentWS)", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpaceTangent", "float3(1.0, 0.0, 0.0)", GraphValueType::Float3, GraphStageFragment},

			{"WorldSpaceBiTangent", "pv.bitangentWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpaceBiTangent", "pv.bitangentWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpaceBiTangent", "mul((float3x3)pv.view, pv.bitangentWS)", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpaceBiTangent", "float3(0.0, 1.0, 0.0)", GraphValueType::Float3, GraphStageFragment},

			{"WorldSpaceViewDirection", "pv.viewDirectionWS", GraphValueType::Float3, GraphStageFragment},
			{"ObjectSpaceViewDirection", "pv.viewDirectionWS", GraphValueType::Float3, GraphStageFragment},
			{"ViewSpaceViewDirection", "mul((float3x3)pv.view, pv.viewDirectionWS)", GraphValueType::Float3, GraphStageFragment},
			{"TangentSpaceViewDirection",
			 "normalize(float3(dot(pv.viewDirectionWS, pv.tangentWS), dot(pv.viewDirectionWS, pv.bitangentWS), dot(pv.viewDirectionWS, pv.normalWS)))",
			 GraphValueType::Float3, GraphStageFragment},

			{"ScreenPosition", "float4(pv.uv, 0.0, 1.0)", GraphValueType::Float4, GraphStageFragment},
			{"NDCPosition", "pv.uv", GraphValueType::Float2, GraphStageFragment},
			{"PixelPosition", "(pv.uv * 128.0)", GraphValueType::Float2, GraphStageFragment},
			{"ClipPosition", "(pv.uv * 128.0)", GraphValueType::Float2, GraphStageFragment},

			{"ObjectToWorld", str(Identity), GraphValueType::Float4x4, GraphStageFragment},
			{"WorldToObject", str(Identity), GraphValueType::Float4x4, GraphStageFragment},
			{"View", "pv.view", GraphValueType::Float4x4, GraphStageFragment},
			{"InverseView", "pv.inverseView", GraphValueType::Float4x4, GraphStageFragment},
			{"Projection", "pv.projection", GraphValueType::Float4x4, GraphStageFragment},
			{"InverseProjection", "pv.inverseProjection", GraphValueType::Float4x4, GraphStageFragment},
			{"ViewProjection", "mul(pv.projection, pv.view)", GraphValueType::Float4x4, GraphStageFragment},
			{"InverseViewProjection", "mul(pv.inverseView, pv.inverseProjection)", GraphValueType::Float4x4, GraphStageFragment},

			{"CameraPosition", "pv.cameraWS", GraphValueType::Float3, GraphStageFragment},
			{"ProjectionParams", "pv.projectionParams", GraphValueType::Float4, GraphStageFragment},
			{"ScreenParams", "pv.screenParams", GraphValueType::Float4, GraphStageFragment},
			{"TimeParameters", "sltPreview.time", GraphValueType::Float4, GraphStageFragment},

			{"InstanceID", "0.0", GraphValueType::Float, GraphStageFragment},
			{"EyeIndex", "0.0", GraphValueType::Float, GraphStageFragment},
			{"VertexID", "0.0", GraphValueType::Float, GraphStageFragment},
			{"IsFrontFace", "true", GraphValueType::Bool, GraphStageFragment},
			{"SmoothDeltaTime", "0.0", GraphValueType::Float, GraphStageFragment, "the frame clock has no smoothed delta"},
			{"RendererBoundsMin", "0.0", GraphValueType::Float3, GraphStageFragment, "a draw does not provide its bounds"},
			{"RendererBoundsMax", "0.0", GraphValueType::Float3, GraphStageFragment, "a draw does not provide its bounds"},
			{"SceneColor", "sltSceneColor", GraphValueType::Texture2D, GraphStageFragment, {}, "sltSceneSampler"},
			{"SceneDepth", "sltSceneDepth", GraphValueType::Texture2D, GraphStageFragment, {}, "sltSceneSampler"},
			{"SceneNormals", "sltSceneNormals", GraphValueType::Texture2D, GraphStageFragment, {}, "sltSceneSampler"},
			{"SceneColorScale", "1.0", GraphValueType::Float, GraphStageFragment},
			{"MotionVectors", "", GraphValueType::Texture2D, GraphStageFragment, "nothing renders motion vectors"},

			{"SkyDirection", "pv.normalWS", GraphValueType::Float3, GraphStageFragment},
			{"SkySunDirection", str(PreviewSun), GraphValueType::Float3, GraphStageFragment},
			{"SkySunColor", "float3(1.0, 1.0, 1.0)", GraphValueType::Float3, GraphStageFragment},
			{"SkySunAngularDiameter", GraphHLSL::FloatLiteral(PREVIEW_SUN_DIAMETER), GraphValueType::Float, GraphStageFragment},
			{"SkySunEnabled", "true", GraphValueType::Bool, GraphStageFragment},
			{"SkyCapturing", "false", GraphValueType::Bool, GraphStageFragment},
			{"SkySunDisc",
			 "ShalltearSunDisc(pv.normalWS, " + str(PreviewSun) + ", float3(1.0, 1.0, 1.0), " + GraphHLSL::FloatLiteral(std::cos(PREVIEW_SUN_DIAMETER * 0.5f)) + ")",
			 GraphValueType::Float3, GraphStageFragment},
		};

		struct Upstream {
			HashMap<u32, Vector<u32>> feeders{GetMemoryResource(MemTag::Editor)};
			HashMap<u32, const GraphNode*> nodes{GetMemoryResource(MemTag::Editor)};
		};

		Upstream ReadUpstream(const GraphDocument& document) {
			Upstream graph;
			for (const GraphNode& node : document.nodes) {
				graph.nodes[node.id] = &node;
				Vector<u32>& feeders = graph.feeders[node.id];
				for (const GraphInput& input : node.inputs) {
					if (input.fromNode != 0) {
						feeders.push_back(input.fromNode);
					}
				}
			}
			return graph;
		}

		bool ResolveThreeD(const Upstream& graph, u32 node, HashMap<u32, i8>& memo) {
			auto known = memo.find(node);
			if (known != memo.end()) {
				return known->second > 0;
			}
			memo[node] = 0;
			auto found = graph.nodes.find(node);
			bool threeD = false;
			if (found != graph.nodes.end()) {
				const GraphNodeDefinition* definition = GraphNodeLibrary::Find(found->second->def);
				const GraphPreviewMode mode = definition ? definition->preview : GraphPreviewMode::Inherit;
				if (mode == GraphPreviewMode::Preview3D) {
					threeD = true;
				} else if (mode != GraphPreviewMode::Preview2D) {
					auto feeders = graph.feeders.find(node);
					if (feeders != graph.feeders.end()) {
						for (u32 feeder : feeders->second) {
							threeD = threeD || ResolveThreeD(graph, feeder, memo);
						}
					}
				}
			}
			memo[node] = threeD ? 1 : 0;
			return threeD;
		}

		bool Reads(const Upstream& graph, u32 node, u32 target, HashSet<u32>& seen) {
			if (node == target) {
				return true;
			}
			if (!seen.insert(node).second) {
				return false;
			}
			auto feeders = graph.feeders.find(node);
			if (feeders == graph.feeders.end()) {
				return false;
			}
			for (u32 feeder : feeders->second) {
				if (Reads(graph, feeder, target, seen)) {
					return true;
				}
			}
			return false;
		}

		bool ReadsTime(const GraphProgram& program, GraphValueId value, HashSet<GraphValueId>& seen) {
			if (value == GRAPH_NO_VALUE || !seen.insert(value).second) {
				return false;
			}
			const GraphValue& entry = program.At(value);
			if (entry.op == GraphOp::External && entry.payload < program.externals.size() &&
				program.externals[entry.payload].name == "TimeParameters") {
				return true;
			}
			for (u32 operand = 0; operand < entry.operandCount; ++operand) {
				if (ReadsTime(program, entry.operands[operand], seen)) {
					return true;
				}
			}
			return false;
		}

		str AdaptForPreview(const str& value, GraphValueType type) {
			str color;
			switch (type) {
			case GraphValueType::Bool:
				color = "float4((float)(" + value + "), (float)(" + value + "), (float)(" + value + "), 1.0)";
				return color;
			case GraphValueType::Float:
				color = "float4(" + value + ", " + value + ", " + value + ", 1.0)";
				break;
			case GraphValueType::Float2:
				color = "float4((" + value + ").x, (" + value + ").y, 0.0, 1.0)";
				break;
			case GraphValueType::Float3:
			case GraphValueType::Float4:
				color = "float4((" + value + ").xyz, 1.0)";
				break;
			default:
				return {};
			}
			return "(all(isfinite(" + value + ")) ? " + color + " : float4(1.0, 0.0, 1.0, 1.0))";
		}

	} // anonymous namespace

	namespace GraphPreview {

		std::span<const GraphBinding> Bindings() { return previewBindings; }

		void Previewable(const GraphDocument& document, Vector<Tile>& out, GraphDefinitions* definitions) {
			out.clear();
			GraphDefinitions local;
			GraphDefinitions& resolver = definitions ? *definitions : local;
			const Upstream graph = ReadUpstream(document);
			HashMap<u32, i8> memo{GetMemoryResource(MemTag::Editor)};
			for (const GraphNode& node : document.nodes) {
				const GraphNodeDefinition* definition = resolver.Find(document, node);
				const bool computes = definition && (definition->body || definition->subgraph);
				if (!computes || definition->outputs.empty() || definition->preview == GraphPreviewMode::None) {
					continue;
				}
				Tile tile;
				tile.node = node.id;
				tile.threeD = ResolveThreeD(graph, node.id, memo);
				out.push_back(tile);
			}
			std::sort(out.begin(), out.end(), [](const Tile& a, const Tile& b) { return a.node < b.node; });
		}

		bool Generate(const GraphDocument& document, std::string_view sourceName, Program& out, GraphDefinitions* definitions, const Guid& asset) {
			out.text.clear();
			out.tiles.clear();
			out.diagnostics.clear();
			out.rows = 0;
			if (document.type != GraphSurface::DocumentType && document.type != GraphSubgraphDocumentType) {
				out.diagnostics.push_back({0, "the document is a '" + document.type + "' (not a node shader or a subgraph)"});
				return false;
			}
			GraphDefinitions local;
			GraphDefinitions& resolver = definitions ? *definitions : local;
			Vector<GraphPropertyInfo> properties{GetMemoryResource(MemTag::Editor)};
			if (!GraphProperties::ParseBlackboard(document, properties, out.diagnostics)) {
				return false;
			}
			GraphSubgraphs::CollectKeywords(document, resolver, properties, out.diagnostics);

			Vector<Tile> candidates{GetMemoryResource(MemTag::Editor)};
			Previewable(document, candidates, &resolver);
			const Upstream graph = ReadUpstream(document);

			GraphProgram program;
			for (;;) {
				if (candidates.empty()) {
					return true;
				}
				Vector<GraphCompileRoot> roots{GetMemoryResource(MemTag::Editor)};
				for (const Tile& tile : candidates) {
					const GraphNode* node = graph.nodes.find(tile.node)->second;
					GraphCompileRoot root;
					root.node = tile.node;
					root.port = str(resolver.Find(document, *node)->outputs[0].name);
					root.stage = GraphStageFragment;
					roots.push_back(std::move(root));
				}
				GraphCompileRequest request;
				request.roots = roots;
				request.bindings = previewBindings;
				request.definitions = &resolver;
				request.asset = asset;
				Vector<GraphDiagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
				if (GraphCompiler::Compile(document, request, program, diagnostics)) {
					break;
				}
				HashSet<u32> failing{GetMemoryResource(MemTag::Editor)};
				for (const GraphDiagnostic& diagnostic : diagnostics) {
					if (diagnostic.node == 0) {
						out.diagnostics.push_back(diagnostic);
						return false;
					}
					failing.insert(diagnostic.node);
					out.diagnostics.push_back(diagnostic);
				}
				const size_t before = candidates.size();
				std::erase_if(candidates, [&](const Tile& tile) {
					for (u32 bad : failing) {
						HashSet<u32> seen{GetMemoryResource(MemTag::Editor)};
						if (Reads(graph, tile.node, bad, seen)) {
							return true;
						}
					}
					return false;
				});
				if (candidates.size() == before) {
					return true;
				}
			}

			GraphHLSLText emitted;
			GraphHLSL::Emit(program, GraphStageFragment, emitted);

			Vector<str> colors{GetMemoryResource(MemTag::Editor)};
			for (size_t index = 0; index < candidates.size(); ++index) {
				const GraphValueType type = program.At(program.roots[index].value).type;
				str color = AdaptForPreview(emitted.roots[index], type);
				if (color.empty()) {
					continue;
				}
				Tile tile = candidates[index];
				HashSet<GraphValueId> seen{GetMemoryResource(MemTag::Editor)};
				tile.timed = ReadsTime(program, program.roots[index].value, seen);
				out.tiles.push_back(tile);
				colors.push_back(std::move(color));
			}
			if (out.tiles.empty()) {
				return true;
			}
			const u32 count = static_cast<u32>(out.tiles.size());
			out.rows = (count + ATLAS_COLUMNS - 1) / ATLAS_COLUMNS;

			str text;
			text += "// generated by Node Shader from " + str(sourceName) + "'s node previews. Do not edit manually\n";
			text += "#pragma shalltear_blend opaque\n";
			text += "#pragma shalltear_depth off\n";
			text += "#pragma shalltear_cull off\n";
			text += "#pragma shalltear_skip_optimizations\n";
			for (const GraphPropertyInfo& property : properties) {
				if (property.kind == GraphPropertyKind::Texture2D) {
					text += "#pragma shalltear_default " + property.reference + " " + property.fallback + "\n";
				}
			}
			for (const GraphPropertyInfo& property : properties) {
				if (const str keyword = GraphProperties::DefaultKeyword(property); !keyword.empty()) {
					text += "#define " + keyword + "\n";
				}
			}
			text += GraphProperties::Declarations(properties);
			text += GraphSurface::PassDeclarations(program);
			if (std::any_of(program.externals.begin(), program.externals.end(),
							[](const GraphExternal& external) { return external.expression.find("ShalltearSunDisc(") != str::npos; })) {
				text += "#include \"SunDisc.hlsli\"\n";
			}
			text += "#include \"ShalltearBufferAccess.hlsli\"\n";
			text += "struct ShalltearPreviewPush\n{\n\tfloat4 time;\n};\n";
			text += "SLT_PUSH_CONSTANT(ShalltearPreviewPush, sltPreview)\n\n";
			text += "static const uint SLT_PREVIEW_COLUMNS = " + std::to_string(ATLAS_COLUMNS) + ";\n";
			text += "static const uint SLT_PREVIEW_ROWS = " + std::to_string(out.rows) + ";\n";
			text += "static const uint SLT_PREVIEW_THREE_D[" + std::to_string(count) + "] = {";
			for (u32 index = 0; index < count; ++index) {
				text += index == 0 ? "" : ", ";
				text += out.tiles[index].threeD ? "1" : "0";
			}
			text += "};\n\n";
			text += "struct ShalltearPreviewVaryings\n{\n\tfloat4 positionCS : SV_Position;\n\tfloat2 tileUV : TEXCOORD0;\n"
					"\tnointerpolation uint tile : TEXCOORD1;\n};\n\n";
			// row 0 = clip y +1
			text += "ShalltearPreviewVaryings VSMain(uint vertexId : SV_VertexID, uint instance : SV_InstanceID)\n{\n"
					"\tfloat2 corner = float2((vertexId == 1 || vertexId == 3 || vertexId == 4) ? 1.0 : 0.0,\n"
					"\t\t\t\t\t\t   (vertexId == 2 || vertexId == 4 || vertexId == 5) ? 1.0 : 0.0);\n"
					"\tfloat2 atlas = (float2(instance % SLT_PREVIEW_COLUMNS, instance / SLT_PREVIEW_COLUMNS) + corner) /\n"
					"\t\t\t\t   float2(SLT_PREVIEW_COLUMNS, SLT_PREVIEW_ROWS);\n"
					"\tShalltearPreviewVaryings output;\n"
					"\toutput.positionCS = float4(atlas.x * 2.0 - 1.0, 1.0 - atlas.y * 2.0, 0.0, 1.0);\n"
					"\toutput.tileUV = corner;\n"
					"\toutput.tile = instance;\n"
					"\treturn output;\n}\n\n";
			text += PreviewInputs;
			const size_t functionsAt = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
			text += emitted.functions;
			text += "float4 PSMain(ShalltearPreviewVaryings input) : SV_Target\n{\n";
			text += "\tShalltearPreviewInput pv = ShalltearPreviewBuild(input.tileUV, SLT_PREVIEW_THREE_D[input.tile] != 0);\n";
			text += "\tif (!pv.hit)\n\t{\n\t\tdiscard;\n\t}\n";
			const size_t bodyAt = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
			text += emitted.body;
			text += "\tswitch (input.tile)\n\t{\n";
			for (u32 index = 0; index < count; ++index) {
				text += "\tcase " + std::to_string(index) + ":\n\t\treturn " + colors[index] + ";\n";
			}
			text += "\tdefault:\n\t\treturn float4(0.0, 0.0, 0.0, 0.0);\n\t}\n}\n";
			out.lineNodes.assign(static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1, 0u);
			for (size_t i = 0; i < emitted.functionLineNodes.size(); ++i) {
				out.lineNodes[functionsAt + i] = emitted.functionLineNodes[i];
			}
			for (size_t i = 0; i < emitted.lineNodes.size(); ++i) {
				out.lineNodes[bodyAt + i] = emitted.lineNodes[i];
			}
			out.text = std::move(text);
			return true;
		}

	} // namespace GraphPreview

} // namespace Shalltear
