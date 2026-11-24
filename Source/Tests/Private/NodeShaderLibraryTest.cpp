#include "TestsPCH.h"

#include "NodeShaderTestUtil.h"

#include "GraphEffect.h"

#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace Shalltear;
using namespace Shalltear::NodeShaderTest;

namespace {

	constexpr u32 CASES = 12;

	struct Random {
		u32 state;
		f32 Next() {
			state = state * 1664525u + 1013904223u;
			return static_cast<f32>(state >> 8) / 16777216.0f;
		}
	};

	RefValue Draw(Random& random, Feed feed, std::array<f32, 2> domain) {
		RefValue value;
		value.n = FeedComponents(feed);
		for (u32 i = 0; i < value.n; ++i) {
			value.c[i] = feed == Feed::B1 ? (random.Next() < 0.5f ? 0.0f : 1.0f) : domain[0] + (domain[1] - domain[0]) * random.Next();
		}
		return value;
	}

	struct Gradient {
		std::array<RefValue, REF_INPUTS> dx{};
		std::array<RefValue, REF_INPUTS> dy{};
	};

	Vector<RefCase> MakeCases(const RefEntry& entry, const RefShape& shape, u32 seed, Vector<Gradient>& gradients) {
		Vector<RefCase> cases{GetMemoryResource(MemTag::Editor)};
		Random random{seed * 2654435761u + 12345u};
		for (u32 k = 0; k < CASES; ++k) {
			RefCase c;
			c.feeds = shape.inputs;
			for (const RefBinding& binding : entry.bindings) {
				c.feeds[binding.input] = binding.feed;
			}
			Gradient gradient;
			for (u32 i = 0; i < REF_INPUTS; ++i) {
				if (c.feeds[i] == Feed::None) {
					continue;
				}
				c.corners[0][i] = Draw(random, c.feeds[i], entry.domains[i]);
				gradient.dx[i].n = gradient.dy[i].n = c.corners[0][i].n;
			}
			if (entry.prepare != nullptr) {
				entry.prepare(c.corners[0], k);
			}
			if (entry.derivatives) {
				for (u32 j = 0; j < c.corners[0][0].n; ++j) {
					f32 sx = random.Next() < 0.5f ? -1.0f : 1.0f;
					f32 sy = random.Next() < 0.5f ? -1.0f : 1.0f;
					gradient.dx[0].c[j] = sx * (0.02f + 0.13f * random.Next());
					gradient.dy[0].c[j] = sy * (0.02f + 0.13f * random.Next());
				}
			}
			c.corners[1] = c.corners[0];
			c.corners[2] = c.corners[0];
			for (u32 i = 0; i < REF_INPUTS; ++i) {
				for (u32 j = 0; j < c.corners[0][i].n; ++j) {
					c.corners[1][i].c[j] = c.corners[0][i].c[j] + gradient.dx[i].c[j];
					c.corners[2][i].c[j] = c.corners[0][i].c[j] + gradient.dy[i].c[j];
				}
			}
			cases.push_back(c);
			gradients.push_back(gradient);
		}
		return cases;
	}

	constexpr u32 NODE_UNDER_TEST = 100;

	std::string_view PropertyTypeOf(Feed feed) {
		switch (feed) {
		case Feed::B1:
			return "Boolean";
		case Feed::F1:
			return "Float";
		case Feed::F2:
			return "Vector2";
		case Feed::F3:
			return "Vector3";
		default:
			return "Vector4";
		}
	}

	bool IsMatrixFeed(Feed feed) { return feed == Feed::M2 || feed == Feed::M3 || feed == Feed::M4; }
	u32 MatrixDimensionOf(Feed feed) { return feed == Feed::M2 ? 2 : (feed == Feed::M3 ? 3 : 4); }

	struct Slot {
		str reference;
		u32 components = 0;
		u32 input = 0;
		i32 row = -1;
		bool binding = false;
		str assemble;
	};

	void ApplySettings(GraphNode& node, std::string_view settings) {
		while (!settings.empty()) {
			size_t end = settings.find(';');
			std::string_view pair = settings.substr(0, end);
			size_t equals = pair.find('=');
			DocumentBuilder::Setting(node, pair.substr(0, equals), pair.substr(equals + 1));
			settings = end == std::string_view::npos ? std::string_view{} : settings.substr(end + 1);
		}
	}

	DocumentBuilder BuildGraph(const RefEntry& entry, const GraphNodeDefinition& definition, const RefShape& shape, const RefVariant& variant,
							   Vector<Slot>& slots) {
		DocumentBuilder builder;
		u32 nextProperty = 1;
		u32 nextNode = 1;
		GraphNode& node = builder.Node(NODE_UNDER_TEST, definition.id);
		ApplySettings(node, variant.settings);
		if (entry.graph != nullptr) {
			entry.graph(builder, variant.tag);
		}
		for (const RefBinding& binding : entry.bindings) {
			str expression(TestBindingExpression(binding.name));
			if (!IsMatrixFeed(binding.feed)) {
				slots.push_back({expression, FeedComponents(binding.feed), binding.input, -1, true, {}});
				continue;
			}
			for (u32 row = 0; row < 4; ++row) {
				slots.push_back({expression + "_r" + std::to_string(row), 4, binding.input, static_cast<i32>(row), false, expression});
			}
		}
		for (u32 i = 0; i < definition.inputs.size(); ++i) {
			Feed feed = shape.inputs[i];
			if (feed == Feed::None) {
				continue;
			}
			if (!IsMatrixFeed(feed)) {
				str reference = "p" + std::to_string(i);
				u32 propertyNode = builder.Property(nextProperty++, nextNode++, reference, PropertyTypeOf(feed));
				builder.Connect(propertyNode, "Out", NODE_UNDER_TEST, definition.inputs[i].name);
				slots.push_back({reference, FeedComponents(feed), i, -1});
				continue;
			}
			u32 construction = 50 + i;
			builder.Node(construction, "Math/MatrixConstruction");
			for (u32 row = 0; row < 4; ++row) {
				str reference = "p" + std::to_string(i) + "r" + std::to_string(row);
				u32 propertyNode = builder.Property(nextProperty++, nextNode++, reference, "Vector4");
				builder.Connect(propertyNode, "Out", construction, "M" + std::to_string(row));
				slots.push_back({reference, 4, i, static_cast<i32>(row)});
			}
			u32 n = MatrixDimensionOf(feed);
			str port = std::to_string(n) + "x" + std::to_string(n);
			builder.Connect(construction, port, NODE_UNDER_TEST, definition.inputs[i].name);
		}
		return builder;
	}

	std::array<f32, 4> SlotValues(const Slot& slot, const std::array<RefValue, REF_INPUTS>& inputs, Feed feed) {
		std::array<f32, 4> out{};
		const RefValue& value = inputs[slot.input];
		if (slot.row < 0) {
			for (u32 j = 0; j < slot.components; ++j) {
				out[j] = value.c[j];
			}
			return out;
		}
		u32 n = MatrixDimensionOf(feed);
		for (u32 j = 0; j < n && static_cast<u32>(slot.row) < n; ++j) {
			out[j] = value.c[static_cast<u32>(slot.row) * n + j];
		}
		return out;
	}

	str Describe(const RefEntry& entry, const RefShape& shape, const RefVariant& variant) {
		str out(entry.id);
		out += " [";
		for (u32 i = 0; i < REF_INPUTS; ++i) {
			if (shape.inputs[i] == Feed::None) {
				continue;
			}
			constexpr std::string_view Names[] = {"-", "F1", "F2", "F3", "F4", "M2", "M3", "M4", "B1"};
			out += (i == 0 ? "" : ",") + str(Names[static_cast<u32>(shape.inputs[i])]);
		}
		out += "] {" + str(variant.settings) + "}";
		return out;
	}

	bool Within(f32 gpu, f32 reference, const RefEntry& entry) {
		if (std::isnan(reference) || std::isnan(gpu)) {
			return std::isnan(reference) && std::isnan(gpu);
		}
		if (std::isinf(reference) || std::isinf(gpu)) {
			return gpu == reference;
		}
		return std::fabs(gpu - reference) <= entry.absolute + entry.relative * std::fabs(reference);
	}

} // anonymous namespace

bool CoveredElsewhere(const GraphNodeDefinition& definition) {
	if (definition.id == "Input/Property" || definition.id == GraphDocumentOps::RedirectDef) {
		return true;
	}
	if (definition.category == "Context" || definition.category == "Block") {
		return true;
	}
	if (GraphEffectRules::IsEffectOnly(definition.id)) {
		return true;
	}
	constexpr std::string_view ThroughTheTarget[] = {"Input/UV",
													 "Input/VertexColor",
													 "Input/SampleTexture2D",
													 "Input/SampleTexture2DLOD",
													 "Input/TextureSize",
													 "Input/CalculateLevelOfDetailTexture2D",
													 "UV/ParallaxMapping",
													 "UV/ParallaxOcclusionMapping",
													 "UV/Triplanar",
													 "Artistic/NormalFromTexture",
													 "Input/SampleTexture2DArray",
													 "Input/SampleTexture3D",
													 "Input/SampleCubemap",
													 "Input/SampleReflectedCubemap"};
	for (std::string_view id : ThroughTheTarget) {
		if (definition.id == id) {
			return true;
		}
	}
	if (definition.id == "Input/Gradient") {
		return true;
	}
	if (definition.id == "Utility/BranchOnInputConnection") {
		return true;
	}
	if (definition.id == "Input/SceneColor" || definition.id == "Input/SceneDepth" || definition.id == "Input/SampleBuffer") {
		return true;
	}
	if (definition.id == "Input/Sky" || definition.id == "Input/SunDisc") {
		return true;
	}
	return definition.id == "Input/VertexID" || definition.id == "CustomInterpolators/Instance" || definition.id == "Input/Light" ||
		   definition.id == "Input/Surface" || definition.id == "Input/StandardLighting" || definition.id == "Utility/Keyword";
}

TEST(NodeShaderLibraryTest, DefinitionsMatchReferences) {
	for (const GraphNodeDefinition* definition : GraphNodeLibrary::All()) {
		if (CoveredElsewhere(*definition)) {
			continue;
		}
		bool found = false;
		for (const RefEntry& entry : ReferenceTable()) {
			found = found || entry.id == definition->id;
		}
		EXPECT_TRUE(found) << definition->id;
	}
	for (const RefEntry& entry : ReferenceTable()) {
		EXPECT_NE(GraphNodeLibrary::Find(entry.id), nullptr) << entry.id;
	}
}

TEST(NodeShaderLibraryTest, NodeOfConstantsFoldsToReference) {
	u32 folded = 0;
	for (const RefEntry& entry : ReferenceTable()) {
		const GraphNodeDefinition* definition = GraphNodeLibrary::Find(entry.id);
		if (definition == nullptr || entry.derivatives || entry.check != RefCheck::Value) {
			continue;
		}
		for (const RefShape& shape : entry.shapes) {
			bool matrices = false;
			for (Feed feed : shape.inputs) {
				matrices = matrices || IsMatrixFeed(feed);
			}
			if (matrices) {
				continue;
			}
			for (const RefVariant& variant : entry.variants) {
				Vector<Gradient> gradients{GetMemoryResource(MemTag::Editor)};
				Vector<RefCase> cases = MakeCases(entry, shape, 7, gradients);
				const RefCase* finite = nullptr;
				for (const RefCase& candidate : cases) {
					bool all = true;
					for (u32 i = 0; i < REF_INPUTS; ++i) {
						for (u32 j = 0; j < candidate.In(i).n; ++j) {
							all = all && std::isfinite(candidate.In(i).c[j]);
						}
					}
					if (all) {
						finite = &candidate;
						break;
					}
				}
				ASSERT_NE(finite, nullptr) << Describe(entry, shape, variant);
				const RefCase& c = *finite;
				DocumentBuilder builder;
				builder.Node(NODE_UNDER_TEST, definition->id);
				ApplySettings(builder.document.nodes.back(), variant.settings);
				if (entry.graph != nullptr) {
					entry.graph(builder, variant.tag);
				}
				auto number = [](f32 value) {
					str literal = GraphHLSL::FloatLiteral(value);
					literal.erase(std::remove(literal.begin(), literal.end(), '('), literal.end());
					literal.erase(std::remove(literal.begin(), literal.end(), ')'), literal.end());
					return "[" + literal + "]";
				};
				for (u32 i = 0; i < definition->inputs.size(); ++i) {
					if (shape.inputs[i] == Feed::None) {
						continue;
					}
					GraphPortType port = definition->inputs[i].type;
					bool dynamic = port == GraphPortType::DynamicVector || port == GraphPortType::Dynamic;
					if (dynamic && c.In(i).n > 1) {
						u32 combine = 10 + i;
						GraphNode& source = builder.Node(combine, "Channel/Combine");
						constexpr std::string_view Channels[] = {"R", "G", "B", "A"};
						for (u32 j = 0; j < c.In(i).n; ++j) {
							DocumentBuilder::Constant(source, Channels[j], number(c.In(i).c[j]));
						}
						constexpr std::string_view Outputs[] = {"", "", "RG", "RGB", "RGBA"};
						builder.Connect(combine, Outputs[c.In(i).n], NODE_UNDER_TEST, definition->inputs[i].name);
						continue;
					}
					str json = "[";
					for (u32 j = 0; j < c.In(i).n; ++j) {
						str literal = number(c.In(i).c[j]);
						json += (j == 0 ? "" : ",") + literal.substr(1, literal.size() - 2);
					}
					DocumentBuilder::Constant(*GraphDocumentOps::FindNode(builder.document, NODE_UNDER_TEST), definition->inputs[i].name, json + "]");
				}
				Vector<GraphCompileRoot> roots{GetMemoryResource(MemTag::Editor)};
				Vector<u32> rooted{GetMemoryResource(MemTag::Editor)};
				for (u32 o = 0; o < definition->outputs.size(); ++o) {
					if ((entry.unrooted & (1u << o)) == 0) {
						roots.push_back({NODE_UNDER_TEST, str(definition->outputs[o].name), GraphStageFragment});
						rooted.push_back(o);
					}
				}
				GraphCompileRequest request{roots, TestBindings()};
				GraphProgram program;
				Vector<GraphDiagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
				ASSERT_TRUE(GraphCompiler::Compile(builder.document, request, program, diagnostics)) << Describe(entry, shape, variant);

				std::array<RefValue, REF_INPUTS> expected{};
				entry.function(c, variant.tag, expected);
				bool allConstant = true;
				for (size_t r = 0; r < program.roots.size(); ++r) {
					const u32 o = rooted[r];
					std::span<const f32> constant = program.ConstantOf(program.roots[r].value);
					if (constant.empty()) {
						allConstant = false;
						continue;
					}
					for (u32 j = 0; j < constant.size(); ++j) {
						f32 want = expected[o].c[j];
						EXPECT_TRUE(std::fabs(constant[j] - want) <= 1e-6f + 1e-6f * std::fabs(want))
							<< Describe(entry, shape, variant) << " output " << o << "[" << j << "] folded to " << constant[j] << ", reference " << want;
					}
				}
				folded += allConstant ? 1 : 0;
			}
		}
	}
	EXPECT_GE(folded, 50u) << "only " << folded;
}

namespace {

	class NodeShaderGPUFixture : public ::testing::Test {
	protected:
		RHI::Device* _device = nullptr;
		std::filesystem::path _root;

		void SetUp() override {
			RHI::DeviceDesc desc;
			desc.headless = true;
			_device = RHI::Device::Create(desc);
			if (_device == nullptr) {
				GTEST_SKIP() << "no Vulkan ICD available";
			}
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() / ("shalltear_shadergraph_" + std::to_string(stamp));
			std::filesystem::create_directories(_root);
		}

		void TearDown() override {
			if (_device != nullptr) {
				_device->WaitIdle();
				for (int i = 0; i < 2; ++i) {
					_device->BeginFrame();
					_device->EndFrame();
				}
				_device->WaitIdle();
				RHI::Device::Destroy(_device);
			}
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		bool Render(const str& source, std::string_view name, u32 width, u32 height, Vector<u32>& pixels, bool& skipped) {
			std::filesystem::path file = _root / (str(name) + ".hlsl");
			{
				std::ofstream stream(file, std::ios::binary);
				stream << source;
			}
			ShaderAsset shader;
			if (!shader.Import(Path(file.generic_string()))) {
				skipped = true;
				return false;
			}
			const ShaderVariant& base = shader.variants[SHADER_BASE_VARIANT];
			RHI::GraphicsPipelineDesc desc;
			desc.vertexSpirv = {base.vertexSpirv.data(), base.vertexSpirv.size()};
			desc.pixelSpirv = {base.pixelSpirv.data(), base.pixelSpirv.size()};
			desc.bytecodeFormat = shader.bytecodeFormat;
			desc.bindings = {shader.bindings.data(), shader.bindings.size()};
			desc.pushConstantSize = shader.pushConstantSize;
			desc.blend = shader.renderState.blend;
			desc.depth = shader.renderState.depth;
			desc.cull = shader.renderState.cull;
			desc.colorFormats[0] = RHI::Format::R32Uint;
			desc.colorFormatCount = 1;
			desc.depthFormat = RHI::Format::Unknown;
			desc.samples = 1;
			desc.debugName = "NodeShaderTable";
			RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(desc);
			if (!pipeline.IsValid()) {
				return false;
			}

			RHI::TextureDesc target;
			target.size = {static_cast<i32>(width), static_cast<i32>(height)};
			target.format = RHI::Format::R32Uint;
			target.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
			target.debugName = "NodeShaderTableTarget";
			RHI::TextureHandle texture = _device->CreateTextureResource(target);

			RHI::CommandList* cmd = _device->BeginFrame();
			cmd->TransitionTexture(texture, RHI::ResourceState::RenderTarget);
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = texture;
			attachment.load = RHI::LoadOp::Clear;
			attachment.clearColor = Color{0.0f, 0.0f, 0.0f, 0.0f};
			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&attachment, 1};
			cmd->BeginRenderPass(pass);
			cmd->SetPipeline(pipeline);
			cmd->Draw(3);
			cmd->EndRenderPass();
			_device->EndFrame();
			_device->WaitIdle();

			pixels.assign(static_cast<size_t>(width) * height, 0);
			_device->ReadbackTexture(texture, {reinterpret_cast<u8*>(pixels.data()), pixels.size() * sizeof(u32)});
			_device->DestroyResource(texture);
			_device->DestroyResource(pipeline);
			return true;
		}
	};

	void FlattenRoots(const GraphProgram& program, const GraphHLSLText& text, Vector<str>& out) {
		for (size_t r = 0; r < program.roots.size(); ++r) {
			GraphValueType type = program.At(program.roots[r].value).type;
			const str& value = text.roots[r];
			str read = "(" + value + ")";
			u32 dimension = GraphTypes::MatrixDimension(type);
			if (dimension != 0) {
				for (u32 row = 0; row < dimension; ++row) {
					for (u32 column = 0; column < dimension; ++column) {
						out.push_back(read + "[" + std::to_string(row) + "][" + std::to_string(column) + "]");
					}
				}
				continue;
			}
			u32 width = GraphTypes::VectorWidth(type);
			if (width <= 1) {
				out.push_back(type == GraphValueType::Bool ? "(float)" + read : read);
				continue;
			}
			for (u32 j = 0; j < width; ++j) {
				out.push_back(read + "." + str(1, "xyzw"[j]));
			}
		}
	}

	str Hex(f32 value) {
		char text[16];
		std::snprintf(text, sizeof(text), "0x%08Xu", std::bit_cast<u32>(value));
		return text;
	}

} // anonymous namespace

TEST_F(NodeShaderGPUFixture, EveryNodeMatchesReferenceOnGPU) {
	u32 checked = 0;
	u32 undecidable = 0;
	str measured;
	u32 entryIndex = 0;
	for (const RefEntry& entry : ReferenceTable()) {
		++entryIndex;
		const GraphNodeDefinition* definition = GraphNodeLibrary::Find(entry.id);
		ASSERT_NE(definition, nullptr) << entry.id;
		f32 worst = 0.0f;
		u32 shapeIndex = 0;
		for (const RefShape& shape : entry.shapes) {
			++shapeIndex;
			for (const RefVariant& variant : entry.variants) {
				str label = Describe(entry, shape, variant);
				Vector<Slot> slots{GetMemoryResource(MemTag::Editor)};
				DocumentBuilder builder = BuildGraph(entry, *definition, shape, variant, slots);
				Vector<GraphCompileRoot> roots{GetMemoryResource(MemTag::Editor)};
				Vector<u32> rooted{GetMemoryResource(MemTag::Editor)};
				for (u32 o = 0; o < definition->outputs.size(); ++o) {
					if ((entry.unrooted & (1u << o)) == 0) {
						roots.push_back({NODE_UNDER_TEST, str(definition->outputs[o].name), GraphStageFragment});
						rooted.push_back(o);
					}
				}
				GraphCompileRequest request{roots, TestBindings()};
				GraphProgram program;
				Vector<GraphDiagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
				ASSERT_TRUE(GraphCompiler::Compile(builder.document, request, program, diagnostics)) << label;
				GraphHLSLText text;
				GraphHLSL::Emit(program, GraphStageFragment, text);

				Vector<Gradient> gradients{GetMemoryResource(MemTag::Editor)};
				Vector<RefCase> cases = MakeCases(entry, shape, entryIndex * 31 + shapeIndex, gradients);
				cases.back() = cases.front();
				gradients.back() = gradients.front();

				str table;
				u32 stride = static_cast<u32>(slots.size()) * 12;
				for (size_t k = 0; k < cases.size(); ++k) {
					for (const Slot& slot : slots) {
						Feed feed = cases[k].feeds[slot.input];
						std::array<f32, 4> base = SlotValues(slot, cases[k].corners[0], feed);
						std::array<f32, 4> dx = SlotValues(slot, gradients[k].dx, feed);
						std::array<f32, 4> dy = SlotValues(slot, gradients[k].dy, feed);
						for (const std::array<f32, 4>& part : {base, dx, dy}) {
							for (f32 component : part) {
								table += Hex(component) + ",";
							}
						}
					}
				}

				Vector<str> components{GetMemoryResource(MemTag::Editor)};
				FlattenRoots(program, text, components);
				u32 width = static_cast<u32>(components.size()) * 2;
				u32 height = static_cast<u32>(cases.size()) * 2;

				str source = "#pragma shalltear_blend opaque\n#pragma shalltear_depth off\n#pragma shalltear_cull off\n\n";
				source += "static const uint TABLE[] = {" + table + "0u};\n\n";
				for (const Slot& slot : slots) {
					if (!slot.binding) {
						source += "static " + str(GraphTypes::HLSLName(GraphTypes::Vector(slot.components))) + " " + slot.reference + ";\n";
					}
				}
				for (const GraphBinding& binding : TestBindings()) {
					if (binding.unavailable.empty()) {
						source += "static " + str(GraphTypes::HLSLName(binding.type)) + " " + binding.expression + ";\n";
					}
				}
				source += "\n";
				source += text.functions;
				source += "struct VSOutput {\n\tfloat4 positionCS : SV_Position;\n};\n\n";
				source += "VSOutput VSMain(uint vertexId : SV_VertexID) {\n\tfloat2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), "
						  "float2(-1.0, 3.0)};\n\tVSOutput output;\n\toutput.positionCS = float4(corners[vertexId], 0.0, 1.0);\n\treturn output;\n}\n\n";
				source += "float4 Read(uint at) {\n\treturn float4(asfloat(TABLE[at]), asfloat(TABLE[at + 1]), asfloat(TABLE[at + 2]), "
						  "asfloat(TABLE[at + 3]));\n}\n\n";
				source += "uint PSMain(VSOutput input) : SV_Target {\n\tuint2 pixel = (uint2)input.positionCS.xy;\n";
				source += "\tuint caseIndex = pixel.y / 2;\n\tuint column = pixel.x / 2;\n\tfloat2 corner = float2(pixel & 1u);\n";
				source += "\tuint at = caseIndex * " + std::to_string(stride) + "u;\n";
				for (size_t s = 0; s < slots.size(); ++s) {
					str offset = "at + " + std::to_string(s * 12) + "u";
					str swizzle = str(".xyzw").substr(0, 1 + slots[s].components);
					source += "\t" + slots[s].reference + " = (Read(" + offset + ") + Read(" + offset + " + 4u) * corner.x + Read(" + offset +
							  " + 8u) * corner.y)" + swizzle + ";\n";
				}
				for (size_t s = 0; s < slots.size(); ++s) {
					if (!slots[s].assemble.empty() && slots[s].row == 0) {
						const str& m = slots[s].assemble;
						source += "\t" + m + " = float4x4(" + m + "_r0, " + m + "_r1, " + m + "_r2, " + m + "_r3);\n";
					}
				}
				source += text.body;
				source += "\tfloat values[" + std::to_string(components.size()) + "] = {";
				for (size_t j = 0; j < components.size(); ++j) {
					source += (j == 0 ? "" : ", ") + components[j];
				}
				source += "};\n\treturn asuint(values[column]);\n}\n";

				Vector<u32> pixels{GetMemoryResource(MemTag::Editor)};
				bool skipped = false;
				str name = str(entry.id).substr(str(entry.id).find('/') + 1) + "_" + std::to_string(shapeIndex) + "_" + std::to_string(variant.tag);
				if (!Render(source, name, width, height, pixels, skipped)) {
					if (skipped) {
						GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
					}
					FAIL() << label << ": harness didn't build\n"
						   << source;
				}

				for (size_t k = 0; k < cases.size(); ++k) {
					std::array<RefValue, REF_INPUTS> expected{};
					entry.function(cases[k], variant.tag, expected);
					std::array<std::array<f32, 2>, REF_INPUTS> bounds{};
					if (entry.range != nullptr) {
						entry.range(cases[k], bounds);
					}
					bool decidable = entry.decidable == nullptr || entry.decidable(cases[k], variant.tag);
					undecidable += decidable ? 0 : 1;
					u32 column = 0;
					for (size_t r = 0; r < program.roots.size(); ++r) {
						const u32 o = rooted[r];
						u32 count = GraphTypes::Components(program.At(program.roots[r].value).type);
						for (u32 j = 0; j < count; ++j, ++column) {
							f32 gpu = std::bit_cast<f32>(pixels[(k * 2) * width + column * 2]);
							f32 first = std::bit_cast<f32>(pixels[column * 2]);
							if (k + 1 == cases.size()) {
								EXPECT_EQ(std::bit_cast<u32>(gpu), std::bit_cast<u32>(first)) << label << " output " << o << "[" << j;
							}
							if (entry.check == RefCheck::Range) {
								EXPECT_TRUE(gpu >= bounds[o][0] && gpu <= bounds[o][1])
									<< label << " case " << k << " output " << o << "[" << j << "] = " << gpu << ", outside [" << bounds[o][0] << ", "
									<< bounds[o][1] << "]";
								continue;
							}
							if (!decidable) {
								continue;
							}
							f32 want = expected[o][j];
							EXPECT_TRUE(Within(gpu, want, entry))
								<< label << " case " << k << " output " << o << "[" << j << "]: GPU " << gpu << ", reference " << want
								<< "\ninputs:" << [&] {
									   str in;
									   for (u32 i = 0; i < REF_INPUTS; ++i) {
										   if (cases[k].feeds[i] == Feed::None) {
											   continue;
										   }
										   in += " (";
										   for (u32 q = 0; q < cases[k].In(i).n; ++q) {
											   in += (q == 0 ? "" : ", ") + std::to_string(cases[k].In(i).c[q]);
										   }
										   in += ")";
									   }
									   return in;
								   }();
							if (std::isfinite(want) && std::isfinite(gpu)) {
								worst = std::max(worst, std::fabs(gpu - want));
							}
							++checked;
						}
					}
				}
			}
		}
		char line[160];
		std::snprintf(line, sizeof(line), "  %-32s worst |gpu - reference| %.3g\n", str(entry.id).c_str(), static_cast<double>(worst));
		measured += line;
	}
	std::printf("[nodeshader] per-node worst error on this device:\n%s", measured.c_str());
	std::printf("[nodeshader] %u components checked, %u Voronoi cases too close to call\n", checked, undecidable);
	EXPECT_GT(checked, 2000u);
}
