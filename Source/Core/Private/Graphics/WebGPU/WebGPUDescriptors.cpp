#include "Graphics/WebGPU/WebGPUInternal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string>

namespace Shalltear::RHI {

	namespace {

		constexpr const char* BLIT_WGSL = R"(
struct BlitVSOut {
	@builtin(position) pos : vec4<f32>,
	@location(0) uv : vec2<f32>,
};

@group(0) @binding(0) var srcTex : texture_2d<f32>;
@group(0) @binding(1) var srcSampler : sampler;

@vertex fn VSMain(@builtin(vertex_index) vid : u32) -> BlitVSOut {
	let x = f32((vid << 1u) & 2u);
	let y = f32(vid & 2u);
	var out : BlitVSOut;
	out.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
	out.uv = vec2<f32>(x, y);
	return out;
}

@fragment fn PSMain(in : BlitVSOut) -> @location(0) vec4<f32> {
	return textureSampleLevel(srcTex, srcSampler, in.uv, 0.0);
}
)";

		struct WGSLBindingMeta {
			u32 group = 0;
			u32 binding = 0;
			DescriptorType type = DescriptorType::UniformBuffer;
			bool storageReadOnly = true;
			WGPUTextureViewDimension viewDim = WGPUTextureViewDimension_2D;
			WGPUTextureSampleType sampleType = WGPUTextureSampleType_Float;
			bool multisampled = false;
			WGPUSamplerBindingType samplerType = WGPUSamplerBindingType_Filtering;
			str varName;
		};

		bool IsIdentChar(char c) {
			const unsigned char uc = static_cast<unsigned char>(c);
			return std::isalnum(uc) != 0 || c == '_';
		}

		bool ParseU32(std::string_view s, u32& out) {
			size_t b = 0;
			while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
				++b;
			}
			size_t e = s.size();
			while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
				--e;
			}
			if (e > b && (s[e - 1] == 'u' || s[e - 1] == 'U' || s[e - 1] == 'i' || s[e - 1] == 'I')) {
				--e;
			}
			const auto res = std::from_chars(s.data() + b, s.data() + e, out);
			return res.ec == std::errc{} && res.ptr == s.data() + e;
		}

		bool ContainsWord(std::string_view haystack, std::string_view word) {
			size_t pos = 0;
			while ((pos = haystack.find(word, pos)) != std::string_view::npos) {
				const bool leftOk = (pos == 0) || !IsIdentChar(haystack[pos - 1]);
				const bool rightOk = (pos + word.size() >= haystack.size()) || !IsIdentChar(haystack[pos + word.size()]);
				if (leftOk && rightOk) {
					return true;
				}
				pos += word.size();
			}
			return false;
		}

		bool IsReferencedBeyondDecl(std::string_view haystack, std::string_view word) {
			if (word.empty()) {
				return true;
			}
			u32 count = 0;
			size_t pos = 0;
			while ((pos = haystack.find(word, pos)) != std::string_view::npos) {
				const bool leftOk = (pos == 0) || !IsIdentChar(haystack[pos - 1]);
				const bool rightOk = (pos + word.size() >= haystack.size()) || !IsIdentChar(haystack[pos + word.size()]);
				if (leftOk && rightOk) {
					if (++count >= 2) {
						return true;
					}
				}
				pos += word.size();
			}
			return false;
		}

		// Returns true when varName (or any helper parameter it flows into) is passed to textureSample / Level / Bias / Grad
		bool UsesFilterableSample(std::string_view wgsl, std::string_view varName) {
			if (varName.empty()) {
				return true;
			}
			constexpr std::string_view SAMPLE_FUNCS[] = {
				"textureSample(",
				"textureSampleLevel(",
				"textureSampleBias(",
				"textureSampleGrad(",
			};
			auto directCall = [&](std::string_view body, std::string_view name) -> bool {
				for (std::string_view fn : SAMPLE_FUNCS) {
					size_t pos = 0;
					while ((pos = body.find(fn, pos)) != std::string_view::npos) {
						size_t argStart = pos + fn.size();
						while (argStart < body.size() && std::isspace(static_cast<unsigned char>(body[argStart]))) {
							++argStart;
						}
						if (body.substr(argStart).starts_with(name)) {
							const size_t after = argStart + name.size();
							if (after < body.size() && !IsIdentChar(body[after])) {
								return true;
							}
						}
						pos = argStart;
					}
				}
				return false;
			};

			if (directCall(wgsl, varName)) {
				return true;
			}

			// Check if passed into a helper function that calls textureSample* on that parameter
			struct FuncInfo {
				std::string_view name;
				Vector<std::string_view> params{GetMemoryResource(MemTag::Graphics)};
				std::string_view body;
			};
			Vector<FuncInfo> funcs{GetMemoryResource(MemTag::Graphics)};
			size_t cursor = 0;
			while ((cursor = wgsl.find("fn ", cursor)) != std::string_view::npos) {
				if (cursor > 0 && IsIdentChar(wgsl[cursor - 1])) {
					cursor += 3;
					continue;
				}
				size_t nameStart = cursor + 3;
				while (nameStart < wgsl.size() && std::isspace(static_cast<unsigned char>(wgsl[nameStart]))) {
					++nameStart;
				}
				size_t nameEnd = nameStart;
				while (nameEnd < wgsl.size() && IsIdentChar(wgsl[nameEnd])) {
					++nameEnd;
				}
				const size_t lparen = wgsl.find('(', nameEnd);
				const size_t rparen = lparen == std::string_view::npos ? std::string_view::npos : wgsl.find(')', lparen);
				const size_t lbrace = rparen == std::string_view::npos ? std::string_view::npos : wgsl.find('{', rparen);
				if (lbrace == std::string_view::npos) {
					break;
				}
				int depth = 1;
				size_t p = lbrace + 1;
				while (p < wgsl.size() && depth > 0) {
					if (wgsl[p] == '{') {
						++depth;
					} else if (wgsl[p] == '}') {
						--depth;
					}
					++p;
				}
				FuncInfo info;
				info.name = wgsl.substr(nameStart, nameEnd - nameStart);
				info.body = wgsl.substr(lbrace, p - lbrace);
				std::string_view paramList = wgsl.substr(lparen + 1, rparen - lparen - 1);
				size_t itemStart = 0;
				while (itemStart < paramList.size()) {
					size_t comma = paramList.find(',', itemStart);
					if (comma == std::string_view::npos) {
						comma = paramList.size();
					}
					std::string_view item = paramList.substr(itemStart, comma - itemStart);
					size_t colon = item.find(':');
					if (colon != std::string_view::npos) {
						std::string_view lhs = item.substr(0, colon);
						size_t e = lhs.size();
						while (e > 0 && std::isspace(static_cast<unsigned char>(lhs[e - 1]))) {
							--e;
						}
						size_t b = e;
						while (b > 0 && IsIdentChar(lhs[b - 1])) {
							--b;
						}
						info.params.push_back(lhs.substr(b, e - b));
					} else {
						info.params.push_back({});
					}
					itemStart = comma + 1;
				}
				funcs.push_back(std::move(info));
				cursor = p;
			}

			// Fixed-point: which (funcIdx, paramIdx) leads to textureSample*
			Vector<Vector<bool>> paramSamples{GetMemoryResource(MemTag::Graphics)};
			paramSamples.resize(funcs.size());
			for (size_t i = 0; i < funcs.size(); ++i) {
				paramSamples[i].assign(funcs[i].params.size(), false);
				for (size_t k = 0; k < funcs[i].params.size(); ++k) {
					if (!funcs[i].params[k].empty() && directCall(funcs[i].body, funcs[i].params[k])) {
						paramSamples[i][k] = true;
					}
				}
			}

			auto argPassedAt = [&](std::string_view body, std::string_view callee, std::string_view ident, size_t argIndex) -> bool {
				size_t pos = 0;
				while ((pos = body.find(callee, pos)) != std::string_view::npos) {
					if (pos > 0 && IsIdentChar(body[pos - 1])) {
						pos += callee.size();
						continue;
					}
					size_t after = pos + callee.size();
					while (after < body.size() && std::isspace(static_cast<unsigned char>(body[after]))) {
						++after;
					}
					if (after >= body.size() || body[after] != '(') {
						pos += callee.size();
						continue;
					}
					size_t cur = after + 1;
					size_t curArg = 0;
					int parenDepth = 0;
					size_t argBegin = cur;
					while (cur < body.size()) {
						const char ch = body[cur];
						if (ch == '(') {
							++parenDepth;
						} else if (ch == ')') {
							if (parenDepth == 0) {
								if (curArg == argIndex && ContainsWord(body.substr(argBegin, cur - argBegin), ident)) {
									return true;
								}
								break;
							}
							--parenDepth;
						} else if (ch == ',' && parenDepth == 0) {
							if (curArg == argIndex && ContainsWord(body.substr(argBegin, cur - argBegin), ident)) {
								return true;
							}
							++curArg;
							argBegin = cur + 1;
						}
						++cur;
					}
					pos = after + 1;
				}
				return false;
			};

			bool changed = true;
			while (changed) {
				changed = false;
				for (size_t i = 0; i < funcs.size(); ++i) {
					for (size_t k = 0; k < funcs[i].params.size(); ++k) {
						if (paramSamples[i][k] || funcs[i].params[k].empty()) {
							continue;
						}
						for (size_t j = 0; j < funcs.size(); ++j) {
							for (size_t m = 0; m < funcs[j].params.size(); ++m) {
								if (paramSamples[j][m] && argPassedAt(funcs[i].body, funcs[j].name, funcs[i].params[k], m)) {
									paramSamples[i][k] = true;
									changed = true;
									break;
								}
							}
							if (paramSamples[i][k]) {
								break;
							}
						}
					}
				}
			}

			for (size_t j = 0; j < funcs.size(); ++j) {
				for (size_t m = 0; m < funcs[j].params.size(); ++m) {
					if (paramSamples[j][m] && argPassedAt(wgsl, funcs[j].name, varName, m)) {
						return true;
					}
				}
			}
			return false;
		}

		// Returns true when sampler varName is passed as an argument to funcName (e.g. "textureSample(", "textureGather(")
		bool SamplerUsedInCall(std::string_view wgsl, std::string_view varName, std::span<const std::string_view> funcs) {
			if (varName.empty()) {
				return false;
			}
			for (std::string_view fn : funcs) {
				size_t pos = 0;
				while ((pos = wgsl.find(fn, pos)) != std::string_view::npos) {
					size_t cur = pos + fn.size();
					int parenDepth = 0;
					const size_t callStart = cur;
					while (cur < wgsl.size()) {
						const char ch = wgsl[cur];
						if (ch == '(') {
							++parenDepth;
						} else if (ch == ')') {
							if (parenDepth == 0) {
								if (ContainsWord(wgsl.substr(callStart, cur - callStart), varName)) {
									return true;
								}
								break;
							}
							--parenDepth;
						}
						++cur;
					}
					pos += fn.size();
				}
			}
			return false;
		}

		WGPUSamplerBindingType DetectSamplerBindingType(std::string_view wgsl, std::string_view varName) {
			constexpr std::string_view FILTERING_FUNCS[] = {
				"textureSample(",
				"textureSampleLevel(",
				"textureSampleBias(",
				"textureSampleGrad(",
			};
			if (SamplerUsedInCall(wgsl, varName, FILTERING_FUNCS)) {
				return WGPUSamplerBindingType_Filtering;
			}
			constexpr std::string_view GATHER_FUNCS[] = {
				"textureGather(",
			};
			if (SamplerUsedInCall(wgsl, varName, GATHER_FUNCS)) {
				return WGPUSamplerBindingType_NonFiltering;
			}
			// Unused samplers stay Filtering so both linear and nearest samplers remain bindable
			return WGPUSamplerBindingType_Filtering;
		}

		void ParseWGSLBindings(std::string_view wgsl, Vector<WGSLBindingMeta>& out) {
			size_t pos = 0;
			while (pos < wgsl.size()) {
				const size_t groupPos = wgsl.find("@group(", pos);
				if (groupPos == std::string_view::npos) {
					break;
				}
				const size_t semi = wgsl.find(';', groupPos);
				if (semi == std::string_view::npos) {
					break;
				}
				// Find start of this declaration (in case @binding came before @group)
				size_t declStart = groupPos;
				while (declStart > 0 && wgsl[declStart - 1] != ';' && wgsl[declStart - 1] != '}') {
					--declStart;
				}
				const std::string_view decl = wgsl.substr(declStart, semi - declStart);
				pos = semi + 1;

				const size_t gOpen = decl.find("@group(");
				const size_t bOpen = decl.find("@binding(");
				if (gOpen == std::string_view::npos || bOpen == std::string_view::npos) {
					continue;
				}
				const size_t gClose = decl.find(')', gOpen + 7);
				const size_t bClose = decl.find(')', bOpen + 9);
				if (gClose == std::string_view::npos || bClose == std::string_view::npos) {
					continue;
				}
				u32 group = 0;
				u32 binding = 0;
				if (!ParseU32(decl.substr(gOpen + 7, gClose - (gOpen + 7)), group) ||
					!ParseU32(decl.substr(bOpen + 9, bClose - (bOpen + 9)), binding)) {
					continue;
				}

				const size_t varPos = decl.find("var");
				const size_t colonPos = varPos == std::string_view::npos ? std::string_view::npos : decl.find(':', varPos);
				if (colonPos == std::string_view::npos) {
					continue;
				}
				std::string_view varHeader = decl.substr(varPos, colonPos - varPos);
				std::string_view typeText = decl.substr(colonPos + 1);
				while (!typeText.empty() && std::isspace(static_cast<unsigned char>(typeText.front()))) {
					typeText.remove_prefix(1);
				}
				while (!typeText.empty() && std::isspace(static_cast<unsigned char>(typeText.back()))) {
					typeText.remove_suffix(1);
				}

				// Extract identifier immediately before ':'
				size_t nameEnd = varHeader.size();
				while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(varHeader[nameEnd - 1]))) {
					--nameEnd;
				}
				size_t nameBegin = nameEnd;
				while (nameBegin > 0 && IsIdentChar(varHeader[nameBegin - 1])) {
					--nameBegin;
				}

				WGSLBindingMeta meta;
				meta.group = group;
				meta.binding = binding;
				meta.varName = str(varHeader.substr(nameBegin, nameEnd - nameBegin));
				if (group != TABLE_SET && !IsReferencedBeyondDecl(wgsl, meta.varName)) {
					continue;
				}

				if (varHeader.find("<uniform>") != std::string_view::npos) {
					meta.type = DescriptorType::UniformBuffer;
				} else if (varHeader.find("<storage") != std::string_view::npos) {
					meta.type = DescriptorType::StorageBuffer;
					meta.storageReadOnly = varHeader.find("read_write") == std::string_view::npos;
				} else if (typeText.starts_with("sampler_comparison")) {
					meta.type = DescriptorType::Sampler;
					meta.samplerType = WGPUSamplerBindingType_Comparison;
				} else if (typeText.starts_with("sampler")) {
					meta.type = DescriptorType::Sampler;
					meta.samplerType = DetectSamplerBindingType(wgsl, meta.varName);
				} else if (typeText.starts_with("texture_")) {
					meta.type = DescriptorType::SampledTexture;
					if (typeText.starts_with("texture_multisampled_2d")) {
						meta.viewDim = WGPUTextureViewDimension_2D;
						meta.multisampled = true;
					} else if (typeText.starts_with("texture_depth_2d_array")) {
						meta.viewDim = WGPUTextureViewDimension_2DArray;
						meta.sampleType = WGPUTextureSampleType_Depth;
					} else if (typeText.starts_with("texture_depth_2d")) {
						meta.viewDim = WGPUTextureViewDimension_2D;
						meta.sampleType = WGPUTextureSampleType_Depth;
					} else if (typeText.starts_with("texture_depth_cube_array")) {
						meta.viewDim = WGPUTextureViewDimension_CubeArray;
						meta.sampleType = WGPUTextureSampleType_Depth;
					} else if (typeText.starts_with("texture_depth_cube")) {
						meta.viewDim = WGPUTextureViewDimension_Cube;
						meta.sampleType = WGPUTextureSampleType_Depth;
					} else if (typeText.starts_with("texture_2d_array")) {
						meta.viewDim = WGPUTextureViewDimension_2DArray;
					} else if (typeText.starts_with("texture_cube_array")) {
						meta.viewDim = WGPUTextureViewDimension_CubeArray;
					} else if (typeText.starts_with("texture_cube")) {
						meta.viewDim = WGPUTextureViewDimension_Cube;
					} else if (typeText.starts_with("texture_3d")) {
						meta.viewDim = WGPUTextureViewDimension_3D;
					} else {
						meta.viewDim = WGPUTextureViewDimension_2D;
					}

					if (meta.sampleType != WGPUTextureSampleType_Depth) {
						if (typeText.find("<u32>") != std::string_view::npos) {
							meta.sampleType = WGPUTextureSampleType_Uint;
						} else if (typeText.find("<i32>") != std::string_view::npos) {
							meta.sampleType = WGPUTextureSampleType_Sint;
						} else if (meta.multisampled || !UsesFilterableSample(wgsl, meta.varName)) {
							meta.sampleType = WGPUTextureSampleType_UnfilterableFloat;
						} else {
							meta.sampleType = WGPUTextureSampleType_Float;
						}
					}
				} else {
					continue;
				}
				out.push_back(std::move(meta));
			}
		}

		WGPUShaderStage ToWGPUStages(ShaderStage stages) {
			WGPUShaderStage flags = WGPUShaderStage_None;
			if (HasFlag(stages, ShaderStage::Vertex)) {
				flags |= WGPUShaderStage_Vertex;
			}
			if (HasFlag(stages, ShaderStage::Pixel)) {
				flags |= WGPUShaderStage_Fragment;
			}
			if (HasFlag(stages, ShaderStage::Compute)) {
				flags |= WGPUShaderStage_Compute;
			}
			return flags ? flags : (WGPUShaderStage_Vertex | WGPUShaderStage_Fragment);
		}

		WGPUCompareFunction ToWGPUCompare(DepthCompare compare) {
			switch (compare) {
			case DepthCompare::Nearer:
				return WGPUCompareFunction_Greater;
			case DepthCompare::Farther:
				return WGPUCompareFunction_Less;
			case DepthCompare::FartherOrEqual:
				return WGPUCompareFunction_LessEqual;
			case DepthCompare::Equal:
				return WGPUCompareFunction_Equal;
			case DepthCompare::NotEqual:
				return WGPUCompareFunction_NotEqual;
			case DepthCompare::Always:
				return WGPUCompareFunction_Always;
			case DepthCompare::NearerOrEqual:
			case DepthCompare::Count:
			default:
				return WGPUCompareFunction_GreaterEqual;
			}
		}

		WGPUCullMode ToWGPUCull(CullMode cull) {
			switch (cull) {
			case CullMode::Front:
				return WGPUCullMode_Front;
			case CullMode::Off:
				return WGPUCullMode_None;
			case CullMode::Back:
			case CullMode::Count:
			default:
				return WGPUCullMode_Back;
			}
		}

		WGPUPrimitiveTopology ToWGPUTopology(PrimitiveTopology topology) {
			switch (topology) {
			case PrimitiveTopology::LineList:
				return WGPUPrimitiveTopology_LineList;
			case PrimitiveTopology::PointList:
				return WGPUPrimitiveTopology_PointList;
			case PrimitiveTopology::TriangleList:
			default:
				return WGPUPrimitiveTopology_TriangleList;
			}
		}

		WGPUBlendState ToWGPUBlend(BlendMode mode, bool& outEnabled) {
			WGPUBlendState state{};
			outEnabled = true;
			switch (mode) {
			case BlendMode::Alpha:
				state.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
				state.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
				break;
			case BlendMode::Additive:
				state.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_One};
				state.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
				break;
			case BlendMode::Premultiplied:
				state.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
				state.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
				break;
			case BlendMode::Multiply:
				state.color = {WGPUBlendOperation_Add, WGPUBlendFactor_Dst, WGPUBlendFactor_Zero};
				state.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_DstAlpha, WGPUBlendFactor_Zero};
				break;
			case BlendMode::Opaque:
			case BlendMode::Count:
			default:
				outEnabled = false;
				break;
			}
			return state;
		}

		bool SameContent(const ResolvedWrite* a, const ResolvedWrite* b, u32 count) {
			for (u32 i = 0; i < count; ++i) {
				if (a[i].object != b[i].object || a[i].offset != b[i].offset || a[i].range != b[i].range ||
					a[i].binding != b[i].binding || a[i].type != b[i].type) {
					return false;
				}
			}
			return true;
		}

		bool HasOverrideId(std::string_view wgsl, u32 id) {
			const str num = std::to_string(id);
			return wgsl.find("@id(" + num + ")") != std::string_view::npos ||
				   wgsl.find("@id(" + num + "u)") != std::string_view::npos;
		}

	} // anonymous namespace

	WGPUBindGroupLayout DeviceImpl::GetOrCreateSetLayout(std::span<const WGPUBindGroupLayoutEntry> entries) {
		u64 hash = 1469598103934665603ull;
		auto mix = [&hash](u64 value) {
			hash ^= value;
			hash *= 1099511628211ull;
		};
		for (const WGPUBindGroupLayoutEntry& e : entries) {
			mix(e.binding);
			mix(static_cast<u64>(e.visibility));
			mix(static_cast<u64>(e.buffer.type) | (static_cast<u64>(e.buffer.hasDynamicOffset) << 8));
			mix(static_cast<u64>(e.sampler.type) << 16);
			mix((static_cast<u64>(e.texture.sampleType) << 24) | (static_cast<u64>(e.texture.viewDimension) << 32) |
				(static_cast<u64>(e.texture.multisampled) << 40));
		}
		mix(entries.size());

		if (auto it = setLayoutCache.find(hash); it != setLayoutCache.end()) {
			return it->second;
		}

		WGPUBindGroupLayoutDescriptor desc{};
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		WGPUBindGroupLayout layout = wgpu.wgpuDeviceCreateBindGroupLayout(device, &desc);
		if (layout) {
			setLayoutCache.emplace(hash, layout);
		}
		return layout;
	}

	bool DeviceImpl::CreateTable() {
		WGPUBufferDescriptor metaDesc{};
		metaDesc.label = ToWGPUString("ShalltearAtlasMeta");
		metaDesc.size = TextureAtlasTable::ATLAS_2D_MAX_SLOTS * 16u;
		metaDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		table.atlasMetaBuffer = wgpu.wgpuDeviceCreateBuffer(device, &metaDesc);
		if (!table.atlasMetaBuffer) {
			return false;
		}

		WGPUTextureDescriptor a2dDesc{};
		a2dDesc.label = ToWGPUString("ShalltearAtlas2D");
		a2dDesc.dimension = WGPUTextureDimension_2D;
		a2dDesc.size = {TextureAtlasTable::ATLAS_2D_SIZE, TextureAtlasTable::ATLAS_2D_SIZE, 1};
		a2dDesc.format = WGPUTextureFormat_RGBA8Unorm;
		a2dDesc.mipLevelCount = 1;
		a2dDesc.sampleCount = 1;
		a2dDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
		table.atlas2D = wgpu.wgpuDeviceCreateTexture(device, &a2dDesc);
		if (!table.atlas2D) {
			return false;
		}
		WGPUTextureViewDescriptor a2dView{};
		a2dView.format = WGPUTextureFormat_RGBA8Unorm;
		a2dView.dimension = WGPUTextureViewDimension_2D;
		a2dView.mipLevelCount = 1;
		a2dView.arrayLayerCount = 1;
		a2dView.aspect = WGPUTextureAspect_All;
		table.atlas2DView = wgpu.wgpuTextureCreateView(table.atlas2D, &a2dView);

		WGPUTextureDescriptor cubeDesc{};
		cubeDesc.label = ToWGPUString("ShalltearAtlasCube");
		cubeDesc.dimension = WGPUTextureDimension_2D;
		cubeDesc.size = {TextureAtlasTable::ATLAS_CUBE_SIZE, TextureAtlasTable::ATLAS_CUBE_SIZE, TextureAtlasTable::ATLAS_CUBE_SLOTS * 6u};
		cubeDesc.format = WGPUTextureFormat_RGBA16Float;
		cubeDesc.mipLevelCount = 1;
		cubeDesc.sampleCount = 1;
		cubeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
		table.atlasCube = wgpu.wgpuDeviceCreateTexture(device, &cubeDesc);
		if (!table.atlasCube) {
			return false;
		}
		WGPUTextureViewDescriptor cubeView{};
		cubeView.format = WGPUTextureFormat_RGBA16Float;
		cubeView.dimension = WGPUTextureViewDimension_CubeArray;
		cubeView.mipLevelCount = 1;
		cubeView.arrayLayerCount = TextureAtlasTable::ATLAS_CUBE_SLOTS * 6u;
		cubeView.aspect = WGPUTextureAspect_All;
		table.atlasCubeView = wgpu.wgpuTextureCreateView(table.atlasCube, &cubeView);

		table.atlasCubeFaceViews.clear();
		table.atlasCubeFaceViews.reserve(TextureAtlasTable::ATLAS_CUBE_SLOTS * 6u);
		for (u32 layer = 0; layer < TextureAtlasTable::ATLAS_CUBE_SLOTS * 6u; ++layer) {
			WGPUTextureViewDescriptor fv{};
			fv.format = WGPUTextureFormat_RGBA16Float;
			fv.dimension = WGPUTextureViewDimension_2D;
			fv.mipLevelCount = 1;
			fv.baseArrayLayer = layer;
			fv.arrayLayerCount = 1;
			fv.aspect = WGPUTextureAspect_All;
			table.atlasCubeFaceViews.push_back(wgpu.wgpuTextureCreateView(table.atlasCube, &fv));
		}

		WGPUSamplerDescriptor sampDesc{};
		sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
		sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
		sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
		sampDesc.magFilter = WGPUFilterMode_Linear;
		sampDesc.minFilter = WGPUFilterMode_Linear;
		sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
		sampDesc.lodMaxClamp = 32.0f;
		sampDesc.maxAnisotropy = 1;
		table.atlasSampler = wgpu.wgpuDeviceCreateSampler(device, &sampDesc);
		table.blitSampler = wgpu.wgpuDeviceCreateSampler(device, &sampDesc);

		// Portable Set 3 layouts: graphics (read-only vertex heap) and compute-RW (read-write vertex heap)
		auto buildSet3Layout = [&](bool rwCompute) -> WGPUBindGroupLayout {
			const WGPUShaderStage allStages = rwCompute ? WGPUShaderStage_Compute
														: (WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute);
			WGPUBindGroupLayoutEntry entries[7]{};

			entries[0].binding = TABLE_PUSH_BINDING;
			entries[0].visibility = allStages;
			entries[0].buffer.type = WGPUBufferBindingType_Uniform;
			entries[0].buffer.hasDynamicOffset = 1;

			entries[1].binding = TABLE_TRANSIENT_RING_BINDING;
			entries[1].visibility = allStages;
			entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

			entries[2].binding = TABLE_VERTEX_HEAP_BINDING;
			entries[2].visibility = rwCompute ? WGPUShaderStage_Compute : allStages;
			entries[2].buffer.type = rwCompute ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;

			entries[3].binding = TABLE_ATLAS_META_BINDING;
			entries[3].visibility = allStages;
			entries[3].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

			entries[4].binding = TABLE_ATLAS_2D_BINDING;
			entries[4].visibility = allStages;
			entries[4].texture.sampleType = WGPUTextureSampleType_Float;
			entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;

			entries[5].binding = TABLE_ATLAS_CUBE_BINDING;
			entries[5].visibility = allStages;
			entries[5].texture.sampleType = WGPUTextureSampleType_Float;
			entries[5].texture.viewDimension = WGPUTextureViewDimension_CubeArray;

			entries[6].binding = TABLE_ATLAS_SAMPLER_BINDING;
			entries[6].visibility = allStages;
			entries[6].sampler.type = WGPUSamplerBindingType_Filtering;

			WGPUBindGroupLayoutDescriptor desc{};
			desc.entryCount = 7;
			desc.entries = entries;
			return wgpu.wgpuDeviceCreateBindGroupLayout(device, &desc);
		};

		table.graphicsLayout = buildSet3Layout(false);
		table.computeReadWriteLayout = buildSet3Layout(true);

		WGPUBindGroupLayout emptyLayout = GetOrCreateSetLayout({});
		if (emptyLayout) {
			WGPUBindGroupDescriptor emptyBgDesc{};
			emptyBgDesc.layout = emptyLayout;
			emptyBgDesc.entryCount = 0;
			emptyBgDesc.entries = nullptr;
			table.emptyBindGroup = wgpu.wgpuDeviceCreateBindGroup(device, &emptyBgDesc);
		}

		// Blit helper shader & layout for GenerateMips + atlas population
		WGPUShaderSourceWGSL wgslSource{};
		wgslSource.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgslSource.code = ToWGPUString(BLIT_WGSL);
		WGPUShaderModuleDescriptor modDesc{};
		modDesc.nextInChain = &wgslSource.chain;
		table.blitShader = wgpu.wgpuDeviceCreateShaderModule(device, &modDesc);

		WGPUBindGroupLayoutEntry blitEntries[2]{};
		blitEntries[0].binding = 0;
		blitEntries[0].visibility = WGPUShaderStage_Fragment;
		blitEntries[0].texture.sampleType = WGPUTextureSampleType_Float;
		blitEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		blitEntries[1].binding = 1;
		blitEntries[1].visibility = WGPUShaderStage_Fragment;
		blitEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		WGPUBindGroupLayoutDescriptor blitLayoutDesc{};
		blitLayoutDesc.entryCount = 2;
		blitLayoutDesc.entries = blitEntries;
		table.blitLayout = wgpu.wgpuDeviceCreateBindGroupLayout(device, &blitLayoutDesc);

		WGPUPipelineLayoutDescriptor blitPipeLayoutDesc{};
		blitPipeLayoutDesc.bindGroupLayoutCount = 1;
		blitPipeLayoutDesc.bindGroupLayouts = &table.blitLayout;
		table.blitPipelineLayout = wgpu.wgpuDeviceCreatePipelineLayout(device, &blitPipeLayoutDesc);

		return table.graphicsLayout && table.computeReadWriteLayout && table.blitShader && table.blitPipelineLayout;
	}

	void DeviceImpl::DestroyTable() {
		for (auto& [fmt, pipe] : table.blitPipelines) {
			if (pipe) {
				wgpu.wgpuRenderPipelineRelease(pipe);
			}
		}
		table.blitPipelines.clear();
		if (table.blitPipelineLayout) {
			wgpu.wgpuPipelineLayoutRelease(table.blitPipelineLayout);
			table.blitPipelineLayout = nullptr;
		}
		if (table.blitLayout) {
			wgpu.wgpuBindGroupLayoutRelease(table.blitLayout);
			table.blitLayout = nullptr;
		}
		if (table.blitShader) {
			wgpu.wgpuShaderModuleRelease(table.blitShader);
			table.blitShader = nullptr;
		}
		if (table.blitSampler) {
			wgpu.wgpuSamplerRelease(table.blitSampler);
			table.blitSampler = nullptr;
		}
		if (table.emptyBindGroup) {
			wgpu.wgpuBindGroupRelease(table.emptyBindGroup);
			table.emptyBindGroup = nullptr;
		}
		if (table.graphicsLayout) {
			wgpu.wgpuBindGroupLayoutRelease(table.graphicsLayout);
			table.graphicsLayout = nullptr;
		}
		if (table.computeReadWriteLayout) {
			wgpu.wgpuBindGroupLayoutRelease(table.computeReadWriteLayout);
			table.computeReadWriteLayout = nullptr;
		}
		if (table.atlasSampler) {
			wgpu.wgpuSamplerRelease(table.atlasSampler);
			table.atlasSampler = nullptr;
		}
		for (WGPUTextureView fv : table.atlasCubeFaceViews) {
			if (fv) {
				wgpu.wgpuTextureViewRelease(fv);
			}
		}
		table.atlasCubeFaceViews.clear();
		if (table.atlasCubeView) {
			wgpu.wgpuTextureViewRelease(table.atlasCubeView);
			table.atlasCubeView = nullptr;
		}
		if (table.atlasCube) {
			wgpu.wgpuTextureDestroy(table.atlasCube);
			wgpu.wgpuTextureRelease(table.atlasCube);
			table.atlasCube = nullptr;
		}
		if (table.atlas2DView) {
			wgpu.wgpuTextureViewRelease(table.atlas2DView);
			table.atlas2DView = nullptr;
		}
		if (table.atlas2D) {
			wgpu.wgpuTextureDestroy(table.atlas2D);
			wgpu.wgpuTextureRelease(table.atlas2D);
			table.atlas2D = nullptr;
		}
		if (table.atlasMetaBuffer) {
			wgpu.wgpuBufferDestroy(table.atlasMetaBuffer);
			wgpu.wgpuBufferRelease(table.atlasMetaBuffer);
			table.atlasMetaBuffer = nullptr;
		}
		for (TextureAtlasSlot& slot : table.slots2D) {
			slot = {};
		}
		for (bool& used : table.cubeUsed) {
			used = false;
		}
		table.shelfX = 0;
		table.shelfY = 0;
		table.shelfHeight = 0;
	}

	void DeviceImpl::EnsureFrameSet3BindGroups(FrameContext& frame) {
		if (frame.set3HeapVersion == vertexHeap.version && frame.set3GraphicsGroup && frame.set3ComputeReadWriteGroup) {
			return;
		}
		if (frame.set3GraphicsGroup) {
			frame.deletionQueue.bindGroups.push_back(frame.set3GraphicsGroup);
			frame.set3GraphicsGroup = nullptr;
		}
		if (frame.set3ComputeReadWriteGroup) {
			frame.deletionQueue.bindGroups.push_back(frame.set3ComputeReadWriteGroup);
			frame.set3ComputeReadWriteGroup = nullptr;
		}

		auto makeGroup = [&](WGPUBindGroupLayout layout) -> WGPUBindGroup {
			WGPUBindGroupEntry entries[7]{};
			entries[0].binding = TABLE_PUSH_BINDING;
			entries[0].buffer = frame.pushBuffer;
			entries[0].offset = 0;
			entries[0].size = WEBGPU_PUSH_STRIDE;

			entries[1].binding = TABLE_TRANSIENT_RING_BINDING;
			entries[1].buffer = frame.transientBuffer;
			entries[1].offset = 0;
			entries[1].size = frame.transientCapacity;

			entries[2].binding = TABLE_VERTEX_HEAP_BINDING;
			entries[2].buffer = vertexHeap.buffer;
			entries[2].offset = 0;
			entries[2].size = vertexHeap.capacity;

			entries[3].binding = TABLE_ATLAS_META_BINDING;
			entries[3].buffer = table.atlasMetaBuffer;
			entries[3].offset = 0;
			entries[3].size = TextureAtlasTable::ATLAS_2D_MAX_SLOTS * 16u;

			entries[4].binding = TABLE_ATLAS_2D_BINDING;
			entries[4].textureView = table.atlas2DView;

			entries[5].binding = TABLE_ATLAS_CUBE_BINDING;
			entries[5].textureView = table.atlasCubeView;

			entries[6].binding = TABLE_ATLAS_SAMPLER_BINDING;
			entries[6].sampler = table.atlasSampler;

			WGPUBindGroupDescriptor desc{};
			desc.layout = layout;
			desc.entryCount = 7;
			desc.entries = entries;
			return wgpu.wgpuDeviceCreateBindGroup(device, &desc);
		};

		frame.set3GraphicsGroup = makeGroup(table.graphicsLayout);
		frame.set3ComputeReadWriteGroup = makeGroup(table.computeReadWriteLayout);
		frame.set3HeapVersion = vertexHeap.version;
	}

	WGPURenderPipeline DeviceImpl::GetOrCreateBlitPipeline(WGPUTextureFormat targetFormat) {
		const u32 key = static_cast<u32>(targetFormat);
		if (auto it = table.blitPipelines.find(key); it != table.blitPipelines.end()) {
			return it->second;
		}
		WGPUColorTargetState target{};
		target.format = targetFormat;
		target.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState frag{};
		frag.module = table.blitShader;
		frag.entryPoint = ToWGPUString("PSMain");
		frag.targetCount = 1;
		frag.targets = &target;

		WGPURenderPipelineDescriptor desc{};
		desc.layout = table.blitPipelineLayout;
		desc.vertex.module = table.blitShader;
		desc.vertex.entryPoint = ToWGPUString("VSMain");
		desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		desc.primitive.frontFace = WGPUFrontFace_CW;
		desc.primitive.cullMode = WGPUCullMode_None;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.fragment = &frag;

		WGPURenderPipeline pipe = wgpu.wgpuDeviceCreateRenderPipeline(device, &desc);
		if (pipe) {
			table.blitPipelines.emplace(key, pipe);
		}
		return pipe;
	}

	void DeviceImpl::BlitTextureToAtlas2D(TextureResource& src, u32 slot) {
		if (lost || slot >= TextureAtlasTable::ATLAS_2D_MAX_SLOTS || !src.view) {
			return;
		}
		const TextureAtlasSlot& rect = table.slots2D[slot];
		WGPURenderPipeline blitPipe = GetOrCreateBlitPipeline(WGPUTextureFormat_RGBA8Unorm);
		if (!blitPipe) {
			return;
		}

		WGPUBindGroupEntry entries[2]{};
		entries[0].binding = 0;
		entries[0].textureView = src.view;
		entries[1].binding = 1;
		entries[1].sampler = table.blitSampler;
		WGPUBindGroupDescriptor bgDesc{};
		bgDesc.layout = table.blitLayout;
		bgDesc.entryCount = 2;
		bgDesc.entries = entries;
		WGPUBindGroup bg = wgpu.wgpuDeviceCreateBindGroup(device, &bgDesc);

		WGPUCommandEncoderDescriptor encDesc{};
		WGPUCommandEncoder enc = wgpu.wgpuDeviceCreateCommandEncoder(device, &encDesc);
		WGPURenderPassColorAttachment color{};
		color.view = table.atlas2DView;
		color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		color.loadOp = WGPULoadOp_Load;
		color.storeOp = WGPUStoreOp_Store;
		WGPURenderPassDescriptor passDesc{};
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &color;

		WGPURenderPassEncoder pass = wgpu.wgpuCommandEncoderBeginRenderPass(enc, &passDesc);
		wgpu.wgpuRenderPassEncoderSetViewport(pass, static_cast<f32>(rect.x), static_cast<f32>(rect.y),
											  static_cast<f32>(rect.width), static_cast<f32>(rect.height), 0.0f, 1.0f);
		wgpu.wgpuRenderPassEncoderSetScissorRect(pass, rect.x, rect.y, rect.width, rect.height);
		wgpu.wgpuRenderPassEncoderSetPipeline(pass, blitPipe);
		wgpu.wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
		wgpu.wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
		wgpu.wgpuRenderPassEncoderEnd(pass);
		wgpu.wgpuRenderPassEncoderRelease(pass);

		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
		wgpu.wgpuQueueSubmit(queue, 1, &cb);
		wgpu.wgpuCommandBufferRelease(cb);
		wgpu.wgpuCommandEncoderRelease(enc);
		wgpu.wgpuBindGroupRelease(bg);
	}

	void DeviceImpl::BlitCubeToAtlasCube(TextureResource& src, u32 slot) {
		if (lost || slot >= TextureAtlasTable::ATLAS_CUBE_SLOTS || src.arrayLayers < 6) {
			return;
		}
		WGPURenderPipeline blitPipe = GetOrCreateBlitPipeline(WGPUTextureFormat_RGBA16Float);
		if (!blitPipe) {
			return;
		}

		WGPUCommandEncoderDescriptor encDesc{};
		WGPUCommandEncoder enc = wgpu.wgpuDeviceCreateCommandEncoder(device, &encDesc);
		for (u32 face = 0; face < 6; ++face) {
			WGPUTextureView srcFace = src.AttachmentView(face, 0);
			WGPUTextureView dstFace = table.atlasCubeFaceViews[slot * 6u + face];
			if (!srcFace || !dstFace) {
				continue;
			}
			WGPUBindGroupEntry entries[2]{};
			entries[0].binding = 0;
			entries[0].textureView = srcFace;
			entries[1].binding = 1;
			entries[1].sampler = table.blitSampler;
			WGPUBindGroupDescriptor bgDesc{};
			bgDesc.layout = table.blitLayout;
			bgDesc.entryCount = 2;
			bgDesc.entries = entries;
			WGPUBindGroup bg = wgpu.wgpuDeviceCreateBindGroup(device, &bgDesc);

			WGPURenderPassColorAttachment color{};
			color.view = dstFace;
			color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			color.loadOp = WGPULoadOp_Clear;
			color.storeOp = WGPUStoreOp_Store;
			WGPURenderPassDescriptor passDesc{};
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments = &color;

			WGPURenderPassEncoder pass = wgpu.wgpuCommandEncoderBeginRenderPass(enc, &passDesc);
			wgpu.wgpuRenderPassEncoderSetPipeline(pass, blitPipe);
			wgpu.wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
			wgpu.wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
			wgpu.wgpuRenderPassEncoderEnd(pass);
			wgpu.wgpuRenderPassEncoderRelease(pass);
			wgpu.wgpuBindGroupRelease(bg);
		}
		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
		wgpu.wgpuQueueSubmit(queue, 1, &cb);
		wgpu.wgpuCommandBufferRelease(cb);
		wgpu.wgpuCommandEncoderRelease(enc);
	}

	u32 Device::GetTableIndex(TextureHandle texture) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(texture);
		if (!resource || !HasFlag(resource->usage, TextureUsage::Sampled) || resource->samples > 1 || IsDepthFormat(resource->format)) {
			return TABLE_NONE;
		}
		if (resource->tableIndex != TABLE_NONE) {
			return resource->tableIndex;
		}

		if (resource->dimension == TextureDimension::Texture2D) {
			const u32 tileW = std::clamp<u32>(static_cast<u32>(resource->size.x), 1u, TextureAtlasTable::ATLAS_2D_MAX_TILE);
			const u32 tileH = std::clamp<u32>(static_cast<u32>(resource->size.y), 1u, TextureAtlasTable::ATLAS_2D_MAX_TILE);

			u32 freeSlot = TABLE_NONE;
			for (u32 i = 0; i < TextureAtlasTable::ATLAS_2D_MAX_SLOTS; ++i) {
				if (!impl->table.slots2D[i].used && impl->table.slots2D[i].width >= tileW && impl->table.slots2D[i].height >= tileH) {
					freeSlot = i;
					break;
				}
			}
			if (freeSlot == TABLE_NONE) {
				for (u32 i = 0; i < TextureAtlasTable::ATLAS_2D_MAX_SLOTS; ++i) {
					if (!impl->table.slots2D[i].used && impl->table.slots2D[i].width == 0) {
						if (impl->table.shelfX + tileW > TextureAtlasTable::ATLAS_2D_SIZE) {
							impl->table.shelfX = 0;
							impl->table.shelfY += impl->table.shelfHeight;
							impl->table.shelfHeight = 0;
						}
						if (impl->table.shelfY + tileH > TextureAtlasTable::ATLAS_2D_SIZE) {
							break;
						}
						impl->table.slots2D[i] = {true, impl->table.shelfX, impl->table.shelfY, tileW, tileH};
						impl->table.shelfX += tileW;
						impl->table.shelfHeight = std::max(impl->table.shelfHeight, tileH);
						freeSlot = i;
						break;
					}
				}
			} else {
				impl->table.slots2D[freeSlot].used = true;
			}
			if (freeSlot == TABLE_NONE) {
				return TABLE_NONE;
			}
			const TextureAtlasSlot& rect = impl->table.slots2D[freeSlot];
			const f32 inv = 1.0f / static_cast<f32>(TextureAtlasTable::ATLAS_2D_SIZE);
			// Half-texel inset avoids shelf bleeding under linear filtering
			const f32 uvRect[4] = {
				(static_cast<f32>(rect.x) + 0.5f) * inv,
				(static_cast<f32>(rect.y) + 0.5f) * inv,
				std::max(static_cast<f32>(rect.width) - 1.0f, 0.0f) * inv,
				std::max(static_cast<f32>(rect.height) - 1.0f, 0.0f) * inv,
			};
			impl->wgpu.wgpuQueueWriteBuffer(impl->queue, impl->table.atlasMetaBuffer, static_cast<u64>(freeSlot) * 16u, uvRect, sizeof(uvRect));
			impl->BlitTextureToAtlas2D(*resource, freeSlot);
			resource->tableIndex = freeSlot;
			return freeSlot;
		}

		if (resource->dimension == TextureDimension::Cube) {
			for (u32 i = 0; i < TextureAtlasTable::ATLAS_CUBE_SLOTS; ++i) {
				if (!impl->table.cubeUsed[i]) {
					impl->table.cubeUsed[i] = true;
					impl->BlitCubeToAtlasCube(*resource, i);
					resource->tableIndex = i;
					return i;
				}
			}
		}
		return TABLE_NONE;
	}

	u32 Device::GetTableIndex(SamplerHandle sampler) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SamplerResource* resource = impl->samplers.Resolve(sampler);
		if (!resource) {
			return TABLE_NONE;
		}
		resource->tableIndex = 0;
		return 0;
	}

	TransientSetWriter Device::CreateTransientSet(PipelineHandle pipeline, u32 setIndex) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(impl->inFrame, "CreateTransientSet outside a frame");
		SLT_CORE_ASSERT(setIndex < MAX_DESCRIPTOR_SETS && setIndex != TABLE_SET);

		TransientSetWriter writer;
		PipelineResource* resource = impl->pipelines.Resolve(pipeline);
		if (!resource || impl->lost) {
			return writer;
		}
		if (!resource->layout) {
			return writer;
		}
		writer._device = impl;
		writer._pipelineIndex = pipeline.index;
		writer._setIndex = setIndex;
		return writer;
	}

	TransientSetWriter& TransientSetWriter::WriteUniformBuffer(u32 binding, BufferHandle buffer, u64 offset, u64 range) {
		if (_device && _writeCount < MAX_WRITES) {
			_writes[_writeCount++] = {binding, DescriptorType::UniformBuffer, offset, range, buffer.index, buffer.generation};
		}
		return *this;
	}

	TransientSetWriter& TransientSetWriter::WriteStorageBuffer(u32 binding, BufferHandle buffer, u64 offset, u64 range) {
		if (_device && _writeCount < MAX_WRITES) {
			_writes[_writeCount++] = {binding, DescriptorType::StorageBuffer, offset, range, buffer.index, buffer.generation};
		}
		return *this;
	}

	TransientSetWriter& TransientSetWriter::WriteTexture(u32 binding, TextureHandle texture) {
		if (_device && _writeCount < MAX_WRITES) {
			_writes[_writeCount++] = {binding, DescriptorType::SampledTexture, 0, 0, texture.index, texture.generation};
		}
		return *this;
	}

	TransientSetWriter& TransientSetWriter::WriteSampler(u32 binding, SamplerHandle sampler) {
		if (_device && _writeCount < MAX_WRITES) {
			_writes[_writeCount++] = {binding, DescriptorType::Sampler, 0, 0, sampler.index, sampler.generation};
		}
		return *this;
	}

	TransientSet TransientSetWriter::Commit() {
		if (!_device || _device->lost) {
			return {};
		}
		DeviceImpl& impl = *_device;
		PipelineResource& pipeline = impl.pipelines.slots[_pipelineIndex].resource;
		if (_setIndex >= pipeline.setCount) {
			SLT_LOG_WARN("Graphics", "TransientSetWriter: set {} not present on the pipeline", _setIndex);
			return {};
		}
		WGPUBindGroupLayout layout = pipeline.setLayouts[_setIndex];
		if (!layout) {
			return {};
		}

		ResolvedWrite resolved[MAX_WRITES];
		WGPUBindGroupEntry entries[MAX_WRITES]{};
		u32 count = 0;
		u64 hash = 1469598103934665603ull;
		auto mix = [&hash](u64 value) {
			hash ^= value;
			hash *= 1099511628211ull;
		};
		mix(reinterpret_cast<uintptr_t>(layout));

		for (u32 i = 0; i < _writeCount; ++i) {
			const StagedWrite& staged = _writes[i];
			if (staged.binding >= 64u || (pipeline.setBindingMask[_setIndex] & (1ull << staged.binding)) == 0) {
				continue;
			}
			ResolvedWrite& out = resolved[count];
			out = {};
			out.binding = staged.binding;
			out.type = staged.type;
			WGPUBindGroupEntry& entry = entries[count];
			entry = {};
			entry.binding = staged.binding;

			switch (staged.type) {
			case DescriptorType::UniformBuffer:
			case DescriptorType::StorageBuffer: {
				BufferResource* buffer = impl.buffers.Resolve(BufferHandle{staged.resourceIndex, staged.resourceGeneration});
				if (!buffer) {
					SLT_LOG_WARN("Graphics", "TransientSetWriter: stale buffer at binding {}", staged.binding);
					continue;
				}
				if (staged.type == DescriptorType::StorageBuffer &&
					!HasFlag(buffer->usage, BufferUsage::Storage) && !HasFlag(buffer->usage, BufferUsage::Vertex)) {
					SLT_LOG_ERROR("Graphics", "TransientSetWriter: the buffer at binding {} wasn't created with Storage usage", staged.binding);
					continue;
				}
				if (staged.a % BINDING_OFFSET_ALIGNMENT != 0) {
					SLT_LOG_ERROR("Graphics", "TransientSetWriter: buffer offset {} at binding {} isn't a multiple of {} (allocate with default alignment)",
								  staged.a, staged.binding, BINDING_OFFSET_ALIGNMENT);
					continue;
				}
				WGPUBuffer gpuBuf = buffer->inVertexHeap ? impl.vertexHeap.buffer : buffer->buffer;
				const u64 baseOffset = buffer->inVertexHeap ? buffer->heapOffset : 0;
				const u64 offset = baseOffset + staged.a;
				const u64 range = staged.b == WHOLE_BUFFER ? (buffer->gpuSize - staged.a) : AlignUp(staged.b, 4);
				entry.buffer = gpuBuf;
				entry.offset = offset;
				entry.size = range;
				out.object = reinterpret_cast<uintptr_t>(gpuBuf);
				out.offset = offset;
				out.range = range;
				break;
			}
			case DescriptorType::SampledTexture: {
				TextureResource* texture = impl.textures.Resolve(TextureHandle{staged.resourceIndex, staged.resourceGeneration});
				if (!texture || !texture->view) {
					SLT_LOG_WARN("Graphics", "TransientSetWriter: stale texture at binding {}", staged.binding);
					continue;
				}
				if (texture->state != ResourceState::ShaderResource) {
					SLT_LOG_ERROR("Graphics", "TransientSetWriter: texture at binding {} is in state {}, not ShaderResource (missing TransitionTexture)",
								  staged.binding, static_cast<u32>(texture->state));
				}
				entry.textureView = texture->view;
				out.object = reinterpret_cast<uintptr_t>(texture->view);
				break;
			}
			case DescriptorType::Sampler: {
				SamplerResource* sampler = impl.samplers.Resolve(SamplerHandle{staged.resourceIndex, staged.resourceGeneration});
				if (!sampler || !sampler->sampler) {
					SLT_LOG_WARN("Graphics", "TransientSetWriter: stale sampler at binding {}", staged.binding);
					continue;
				}
				entry.sampler = sampler->sampler;
				out.object = reinterpret_cast<uintptr_t>(sampler->sampler);
				break;
			}
			default:
				continue;
			}
			mix(out.object);
			mix(out.offset ^ (out.range << 1));
			mix(static_cast<u64>(out.binding) | (static_cast<u64>(out.type) << 32));
			++count;
		}
		mix(count);

		FrameBindGroupCache& cache = impl.frames[impl.frameIndex].bindGroups;
		u32 slot = static_cast<u32>(hash) & (FrameBindGroupCache::CACHE_SLOTS - 1);
		for (u32 probe = 0; probe < FrameBindGroupCache::CACHE_MAX_PROBES; ++probe) {
			const u32 idx = cache.slots[slot];
			if (idx == FrameBindGroupCache::CACHE_EMPTY) {
				break;
			}
			const FrameBindGroupCache::Entry& existing = cache.entries[idx];
			if (existing.hash == hash && existing.layout == layout && existing.writeCount == count &&
				SameContent(cache.writes.data() + existing.firstWrite, resolved, count)) {
				TransientSet result;
				result.opaque = existing.opaque;
				return result;
			}
			slot = (slot + 1) & (FrameBindGroupCache::CACHE_SLOTS - 1);
		}

		u64 opaque = 0;
		if (count == pipeline.setBindingCount[_setIndex]) {
			WGPUBindGroupDescriptor desc{};
			desc.layout = layout;
			desc.entryCount = count;
			desc.entries = entries;
			WGPUBindGroup group = impl.wgpu.wgpuDeviceCreateBindGroup(impl.device, &desc);
			if (!group) {
				return {};
			}
			cache.ownedGroups.push_back(group);
			opaque = static_cast<u64>(reinterpret_cast<uintptr_t>(group));
		} else {
			// Partial set (e.g. skipped misaligned write): keep deterministic dedup handle without submitting invalid WebGPU bind group
			opaque = (1ull << 63) | static_cast<u64>(cache.entries.size() + 1u);
		}

		slot = static_cast<u32>(hash) & (FrameBindGroupCache::CACHE_SLOTS - 1);
		for (u32 probe = 0; probe < FrameBindGroupCache::CACHE_MAX_PROBES; ++probe) {
			if (cache.slots[slot] == FrameBindGroupCache::CACHE_EMPTY) {
				FrameBindGroupCache::Entry entry;
				entry.hash = hash;
				entry.layout = layout;
				entry.opaque = opaque;
				entry.firstWrite = static_cast<u32>(cache.writes.size());
				entry.writeCount = count;
				for (u32 i = 0; i < count; ++i) {
					cache.writes.push_back(resolved[i]);
				}
				cache.slots[slot] = static_cast<u32>(cache.entries.size());
				cache.entries.push_back(entry);
				break;
			}
			slot = (slot + 1) & (FrameBindGroupCache::CACHE_SLOTS - 1);
		}

		TransientSet result;
		result.opaque = opaque;
		return result;
	}

	namespace {

		bool BuildWebGPUPipelineLayout(DeviceImpl& impl, std::span<const DescriptorBindingDesc> bindings,
									   std::span<const WGSLBindingMeta> wgslMeta, u32 pushConstantSize, bool isCompute,
									   WGPUBindGroupLayout (&setLayouts)[MAX_DESCRIPTOR_SETS],
									   bool (&setHasBindings)[MAX_DESCRIPTOR_SETS],
									   u64 (&setBindingMask)[MAX_DESCRIPTOR_SETS],
									   u32 (&setBindingCount)[MAX_DESCRIPTOR_SETS], u32& setCount,
									   bool& outTableWritable, WGPUPipelineLayout& outLayout) {
			setCount = 0;
			outTableWritable = false;
			for (u32 s = 0; s < MAX_DESCRIPTOR_SETS; ++s) {
				setHasBindings[s] = false;
				setBindingMask[s] = 0;
				setBindingCount[s] = 0;
			}
			for (const DescriptorBindingDesc& b : bindings) {
				setCount = std::max(setCount, b.set + 1);
				if (b.set == TABLE_SET && b.binding == TABLE_VERTEX_HEAP_BINDING && !b.readOnly) {
					outTableWritable = true;
				}
			}
			for (const WGSLBindingMeta& m : wgslMeta) {
				setCount = std::max(setCount, m.group + 1);
				if (m.group == TABLE_SET && m.binding == TABLE_VERTEX_HEAP_BINDING && !m.storageReadOnly) {
					outTableWritable = true;
				}
			}
			if (pushConstantSize > 0) {
				setCount = std::max(setCount, TABLE_SET + 1);
			}
			if (setCount > MAX_DESCRIPTOR_SETS) {
				return false;
			}

			Vector<WGPUBindGroupLayoutEntry> entries{GetMemoryResource(MemTag::Graphics)};
			for (u32 set = 0; set < setCount; ++set) {
				if (set == TABLE_SET) {
					setLayouts[set] = (isCompute && outTableWritable) ? impl.table.computeReadWriteLayout : impl.table.graphicsLayout;
					continue;
				}
				entries.clear();
				for (const DescriptorBindingDesc& b : bindings) {
					if (b.set != set) {
						continue;
					}
					bool hasMatched = false;
					WGSLBindingMeta merged{};
					bool sawNonFiltering = false;
					bool sawFiltering = false;
					for (const WGSLBindingMeta& m : wgslMeta) {
						if (m.group == set && m.binding == b.binding) {
							if (!hasMatched) {
								merged = m;
								hasMatched = true;
							} else {
								merged.storageReadOnly = merged.storageReadOnly && m.storageReadOnly;
								if (m.sampleType == WGPUTextureSampleType_Float &&
									merged.sampleType == WGPUTextureSampleType_UnfilterableFloat) {
									merged.sampleType = WGPUTextureSampleType_Float;
								}
							}
							if (m.type == DescriptorType::Sampler) {
								if (m.samplerType == WGPUSamplerBindingType_NonFiltering) {
									sawNonFiltering = true;
								} else if (m.samplerType == WGPUSamplerBindingType_Filtering) {
									sawFiltering = true;
								}
							}
						}
					}
					// Skip bindings from a shared shader layout that aren't declared in this stage's WGSL
					if (!hasMatched) {
						continue;
					}
					if (merged.type == DescriptorType::Sampler && sawNonFiltering) {
						merged.samplerType = WGPUSamplerBindingType_NonFiltering;
					} else if (merged.type == DescriptorType::Sampler && sawFiltering && merged.samplerType != WGPUSamplerBindingType_Comparison) {
						merged.samplerType = WGPUSamplerBindingType_Filtering;
					}

					WGPUBindGroupLayoutEntry e{};
					e.binding = b.binding;
					e.visibility = ToWGPUStages(b.stages);
					switch (b.type) {
					case DescriptorType::UniformBuffer:
						e.buffer.type = WGPUBufferBindingType_Uniform;
						break;
					case DescriptorType::StorageBuffer: {
						const bool ro = merged.storageReadOnly;
						e.buffer.type = ro ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
						if (!ro && (e.visibility & WGPUShaderStage_Vertex)) {
							e.visibility &= ~WGPUShaderStage_Vertex;
						}
						break;
					}
					case DescriptorType::SampledTexture:
						e.texture.sampleType = merged.sampleType;
						e.texture.viewDimension = merged.viewDim;
						e.texture.multisampled = merged.multisampled ? 1 : 0;
						break;
					case DescriptorType::Sampler:
						e.sampler.type = merged.samplerType;
						break;
					default:
						continue;
					}
					if (b.binding < 64u) {
						setBindingMask[set] |= (1ull << b.binding);
					}
					entries.push_back(e);
				}
				std::sort(entries.begin(), entries.end(),
						  [](const WGPUBindGroupLayoutEntry& a, const WGPUBindGroupLayoutEntry& b) { return a.binding < b.binding; });
				setHasBindings[set] = !entries.empty();
				setBindingCount[set] = static_cast<u32>(entries.size());
				setLayouts[set] = impl.GetOrCreateSetLayout({entries.data(), entries.size()});
			}

			WGPUPipelineLayoutDescriptor desc{};
			desc.bindGroupLayoutCount = setCount;
			desc.bindGroupLayouts = setLayouts;
			outLayout = impl.wgpu.wgpuDeviceCreatePipelineLayout(impl.device, &desc);
			return outLayout != nullptr;
		}

		WGPUShaderModule CreateWGSLShaderModule(DeviceImpl& impl, std::span<const u8> wgslBytes, const char* debugName) {
			if (wgslBytes.empty()) {
				return nullptr;
			}
			WGPUShaderSourceWGSL source{};
			source.chain.sType = WGPUSType_ShaderSourceWGSL;
			source.code = {reinterpret_cast<const char*>(wgslBytes.data()), wgslBytes.size()};
			WGPUShaderModuleDescriptor desc{};
			desc.nextInChain = &source.chain;
			desc.label = ToWGPUString(debugName);
			return impl.wgpu.wgpuDeviceCreateShaderModule(impl.device, &desc);
		}

	} // anonymous namespace

	WGPURenderPipeline DeviceImpl::GetOrCreateGraphicsVariant(PipelineResource& pipeline, FrontFace frontFace, i32 depthBias, f32 depthBiasSlope) {
		for (const GraphicsPipelineVariant& v : pipeline.variants) {
			if (v.frontFace == frontFace && v.depthBias == depthBias && v.depthBiasSlope == depthBiasSlope) {
				return v.pipeline;
			}
		}

		Vector<str> constantKeys{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUConstantEntry> constantEntries{GetMemoryResource(MemTag::Graphics)};
		constantKeys.reserve(pipeline.pixelConstants.size());
		constantEntries.reserve(pipeline.pixelConstants.size());
		for (const SpecializationConstant& sc : pipeline.pixelConstants) {
			constantKeys.push_back(std::to_string(sc.id));
			WGPUConstantEntry ce{};
			ce.key = ToWGPUString(std::string_view(constantKeys.back()));
			ce.value = static_cast<f64>(sc.value);
			constantEntries.push_back(ce);
		}

		WGPUColorTargetState colorTargets[MAX_COLOR_ATTACHMENTS]{};
		WGPUBlendState blendStates[MAX_COLOR_ATTACHMENTS]{};
		for (u32 i = 0; i < pipeline.colorFormatCount; ++i) {
			colorTargets[i].format = ToWGPUFormat(pipeline.colorFormats[i]);
			colorTargets[i].writeMask = WGPUColorWriteMask_All;
			bool blendEnabled = false;
			blendStates[i] = ToWGPUBlend(pipeline.blend, blendEnabled);
			if (blendEnabled) {
				colorTargets[i].blend = &blendStates[i];
			}
		}

		WGPUFragmentState fragment{};
		if (pipeline.pixelModule) {
			fragment.module = pipeline.pixelModule;
			fragment.entryPoint = ToWGPUString("PSMain");
			fragment.constantCount = constantEntries.size();
			fragment.constants = constantEntries.data();
			fragment.targetCount = pipeline.colorFormatCount;
			fragment.targets = colorTargets;
		}

		WGPUDepthStencilState depthStencil{};
		const bool hasDepth = pipeline.depthFormat != Format::Unknown;
		if (hasDepth) {
			depthStencil.format = ToWGPUFormat(pipeline.depthFormat);
			depthStencil.depthWriteEnabled = pipeline.depth == DepthMode::TestWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
			depthStencil.depthCompare = pipeline.depth == DepthMode::Off ? WGPUCompareFunction_Always : ToWGPUCompare(pipeline.depthCompare);
			depthStencil.stencilFront = {WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep};
			depthStencil.stencilBack = depthStencil.stencilFront;
			depthStencil.stencilReadMask = 0xFFFFFFFFu;
			depthStencil.stencilWriteMask = 0xFFFFFFFFu;
			depthStencil.depthBias = depthBias;
			depthStencil.depthBiasSlopeScale = depthBiasSlope;
			depthStencil.depthBiasClamp = 0.0f;
		}

		WGPURenderPipelineDescriptor desc{};
		desc.layout = pipeline.layout;
		desc.vertex.module = pipeline.vertexModule;
		desc.vertex.entryPoint = ToWGPUString("VSMain");
		desc.primitive.topology = ToWGPUTopology(pipeline.topology);
		desc.primitive.frontFace = frontFace == FrontFace::CounterClockwise ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
		desc.primitive.cullMode = ToWGPUCull(pipeline.cull);
		desc.depthStencil = hasDepth ? &depthStencil : nullptr;
		desc.multisample.count = pipeline.samples;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.fragment = pipeline.pixelModule ? &fragment : nullptr;

		WGPURenderPipeline created = wgpu.wgpuDeviceCreateRenderPipeline(device, &desc);
		if (!created) {
			return nullptr;
		}
		pipeline.variants.push_back({frontFace, depthBias, depthBiasSlope, created});
		return created;
	}

	PipelineHandle Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (desc.vertexSpirv.empty() || desc.bytecodeFormat != ShaderBytecodeFormat::WGSL || impl->lost) {
			if (desc.bytecodeFormat != ShaderBytecodeFormat::WGSL) {
				SLT_LOG_ERROR("Graphics", "WebGPU CreateGraphicsPipeline requires WGSL bytecode ('{}')", desc.debugName ? desc.debugName : "<unnamed>");
			}
			return {};
		}
		const bool depthOnly = desc.pixelSpirv.empty();
		if (depthOnly && desc.colorFormatCount > 0) {
			return {};
		}

		const std::string_view vsText(reinterpret_cast<const char*>(desc.vertexSpirv.data()), desc.vertexSpirv.size());
		const std::string_view psText = depthOnly ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(desc.pixelSpirv.data()), desc.pixelSpirv.size());

		Vector<WGSLBindingMeta> meta{GetMemoryResource(MemTag::Graphics)};
		ParseWGSLBindings(vsText, meta);
		if (!depthOnly) {
			ParseWGSLBindings(psText, meta);
		}

		WGPUShaderModule vsMod = CreateWGSLShaderModule(*impl, desc.vertexSpirv, desc.debugName);
		WGPUShaderModule psMod = depthOnly ? nullptr : CreateWGSLShaderModule(*impl, desc.pixelSpirv, desc.debugName);
		if (!vsMod || (!depthOnly && !psMod)) {
			if (vsMod) {
				impl->wgpu.wgpuShaderModuleRelease(vsMod);
			}
			if (psMod) {
				impl->wgpu.wgpuShaderModuleRelease(psMod);
			}
			return {};
		}

		WGPUBindGroupLayout setLayouts[MAX_DESCRIPTOR_SETS]{};
		bool setHasBindings[MAX_DESCRIPTOR_SETS]{};
		u64 setBindingMask[MAX_DESCRIPTOR_SETS]{};
		u32 setBindingCount[MAX_DESCRIPTOR_SETS]{};
		u32 setCount = 0;
		bool tableWritable = false;
		WGPUPipelineLayout layout = nullptr;
		if (!BuildWebGPUPipelineLayout(*impl, desc.bindings, {meta.data(), meta.size()}, desc.pushConstantSize, false,
									   setLayouts, setHasBindings, setBindingMask, setBindingCount, setCount, tableWritable, layout)) {
			impl->wgpu.wgpuShaderModuleRelease(vsMod);
			if (psMod) {
				impl->wgpu.wgpuShaderModuleRelease(psMod);
			}
			return {};
		}

		auto [index, generation] = impl->pipelines.Acquire();
		PipelineResource& resource = impl->pipelines.slots[index].resource;
		resource.isCompute = false;
		resource.layout = layout;
		for (u32 s = 0; s < setCount; ++s) {
			resource.setLayouts[s] = setLayouts[s];
			resource.setHasBindings[s] = setHasBindings[s];
			resource.setBindingMask[s] = setBindingMask[s];
			resource.setBindingCount[s] = setBindingCount[s];
		}
		resource.setCount = setCount;
		resource.bindsTable = setCount > TABLE_SET;
		resource.tableWritable = tableWritable;
		resource.bindings.assign(desc.bindings.begin(), desc.bindings.end());
		resource.pushConstantSize = desc.pushConstantSize;
		resource.vertexModule = vsMod;
		resource.pixelModule = psMod;
		for (const SpecializationConstant& sc : desc.pixelConstants) {
			if (!depthOnly && HasOverrideId(psText, sc.id)) {
				resource.pixelConstants.push_back(sc);
			}
		}
		resource.blend = desc.blend;
		resource.depth = desc.depth;
		resource.cull = desc.cull;
		resource.topology = desc.topology;
		resource.depthBiasConstant = desc.depthBiasConstant;
		resource.depthBiasSlope = desc.depthBiasSlope;
		resource.dynamicDepthBias = desc.dynamicDepthBias;
		for (u32 i = 0; i < desc.colorFormatCount; ++i) {
			resource.colorFormats[i] = desc.colorFormats[i];
		}
		resource.colorFormatCount = desc.colorFormatCount;
		resource.depthFormat = desc.depthFormat;
		resource.samples = std::max(1u, desc.samples);
		resource.depthCompare = desc.depthCompare;

		const i32 baseBias = desc.dynamicDepthBias ? 0 : static_cast<i32>(desc.depthBiasConstant);
		const f32 baseSlope = desc.dynamicDepthBias ? 0.0f : desc.depthBiasSlope;
		if (!impl->GetOrCreateGraphicsVariant(resource, FrontFace::Clockwise, baseBias, baseSlope)) {
			DestroyResource(PipelineHandle{index, generation});
			return {};
		}
		return PipelineHandle{index, generation};
	}

	PipelineHandle Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (desc.spirv.empty() || desc.bytecodeFormat != ShaderBytecodeFormat::WGSL || impl->lost) {
			if (desc.bytecodeFormat != ShaderBytecodeFormat::WGSL) {
				SLT_LOG_ERROR("Graphics", "WebGPU CreateComputePipeline requires WGSL bytecode ('{}')", desc.debugName ? desc.debugName : "<unnamed>");
			}
			return {};
		}

		const std::string_view csText(reinterpret_cast<const char*>(desc.spirv.data()), desc.spirv.size());
		Vector<WGSLBindingMeta> meta{GetMemoryResource(MemTag::Graphics)};
		ParseWGSLBindings(csText, meta);

		WGPUShaderModule csMod = CreateWGSLShaderModule(*impl, desc.spirv, desc.debugName);
		if (!csMod) {
			return {};
		}

		WGPUBindGroupLayout setLayouts[MAX_DESCRIPTOR_SETS]{};
		bool setHasBindings[MAX_DESCRIPTOR_SETS]{};
		u64 setBindingMask[MAX_DESCRIPTOR_SETS]{};
		u32 setBindingCount[MAX_DESCRIPTOR_SETS]{};
		u32 setCount = 0;
		bool tableWritable = false;
		WGPUPipelineLayout layout = nullptr;
		if (!BuildWebGPUPipelineLayout(*impl, desc.bindings, {meta.data(), meta.size()}, desc.pushConstantSize, true,
									   setLayouts, setHasBindings, setBindingMask, setBindingCount, setCount, tableWritable, layout)) {
			impl->wgpu.wgpuShaderModuleRelease(csMod);
			return {};
		}

		WGPUComputePipelineDescriptor pipeDesc{};
		pipeDesc.label = ToWGPUString(desc.debugName);
		pipeDesc.layout = layout;
		pipeDesc.compute.module = csMod;
		pipeDesc.compute.entryPoint = ToWGPUString("CSMain");
		WGPUComputePipeline computePipe = impl->wgpu.wgpuDeviceCreateComputePipeline(impl->device, &pipeDesc);
		impl->wgpu.wgpuShaderModuleRelease(csMod);
		if (!computePipe) {
			impl->wgpu.wgpuPipelineLayoutRelease(layout);
			return {};
		}

		auto [index, generation] = impl->pipelines.Acquire();
		PipelineResource& resource = impl->pipelines.slots[index].resource;
		resource.isCompute = true;
		resource.computePipeline = computePipe;
		resource.layout = layout;
		for (u32 s = 0; s < setCount; ++s) {
			resource.setLayouts[s] = setLayouts[s];
			resource.setHasBindings[s] = setHasBindings[s];
			resource.setBindingMask[s] = setBindingMask[s];
			resource.setBindingCount[s] = setBindingCount[s];
		}
		resource.setCount = setCount;
		resource.bindsTable = setCount > TABLE_SET;
		resource.tableWritable = tableWritable;
		resource.bindings.assign(desc.bindings.begin(), desc.bindings.end());
		resource.pushConstantSize = desc.pushConstantSize;
		return PipelineHandle{index, generation};
	}

} // namespace Shalltear::RHI
