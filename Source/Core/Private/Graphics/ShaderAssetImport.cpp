#include "Shalltear/Core/Graphics/ShaderAsset.h"

#include "Graphics/ShaderCache.h"
#include "Graphics/ShaderCompiler.h"
#include "Graphics/WGSLTranslator.h"
#include "Graphics/ShaderInterfaces.h"

#include "Shalltear/Core/Graphics/ShaderReflection.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace Shalltear {

	namespace {

		constexpr bool IsSurfaceTemplate(ShaderTemplate value) {
			return value == ShaderTemplate::Lit || value == ShaderTemplate::Unlit;
		}

		RHI::ShaderRenderState SkyRenderState() {
			RHI::ShaderRenderState state;
			state.blend = RHI::BlendMode::Opaque;
			state.depth = RHI::DepthMode::TestOnly;
			state.cull = RHI::CullMode::Off;
			return state;
		}

		bool ParseFloat(std::string_view text, f32& out) {
			const char* begin = text.data();
			const char* end = begin + text.size();
			f32 parsed = 0.0f;
			auto result = std::from_chars(begin, end, parsed);
			if (result.ec != std::errc{} || result.ptr != end) {
				return false;
			}
			out = parsed;
			return true;
		}

		bool ParseFloatPair(std::string_view value, f32& outFirst, f32& outSecond) {
			size_t split = value.find_first_of(" \t");
			if (split == std::string_view::npos) {
				return false;
			}
			std::string_view second = value.substr(split + 1);
			size_t begin = second.find_first_not_of(" \t");
			if (begin == std::string_view::npos) {
				return false;
			}
			second = second.substr(begin);
			if (second.find_first_of(" \t") != std::string_view::npos) {
				return false;
			}
			f32 parsedFirst = 0.0f;
			f32 parsedSecond = 0.0f;
			if (!ParseFloat(value.substr(0, split), parsedFirst) || !ParseFloat(second, parsedSecond)) {
				return false;
			}
			outFirst = parsedFirst;
			outSecond = parsedSecond;
			return true;
		}

		bool ParseMemberDefault(std::string_view value, ShaderMemberDefault& out) {
			size_t split = value.find_first_of(" \t");
			if (split == std::string_view::npos) {
				return false;
			}
			ShaderMemberDefault parsed;
			parsed.name = str(value.substr(0, split));
			std::string_view rest = value.substr(split);
			while (true) {
				size_t begin = rest.find_first_not_of(" \t");
				if (begin == std::string_view::npos) {
					break;
				}
				rest = rest.substr(begin);
				size_t end = rest.find_first_of(" \t");
				std::string_view token = rest.substr(0, end);
				if (parsed.count >= 4 || !ParseFloat(token, parsed.numbers[parsed.count])) {
					return false;
				}
				++parsed.count;
				if (end == std::string_view::npos) {
					break;
				}
				rest = rest.substr(end);
			}
			if (parsed.count == 0) {
				return false;
			}
			out = std::move(parsed);
			return true;
		}

		bool ParseTextureDefault(std::string_view value, ShaderTextureDefault& out) {
			size_t split = value.find_first_of(" \t");
			if (split == std::string_view::npos) {
				return false;
			}
			std::string_view word = value.substr(split + 1);
			size_t begin = word.find_first_not_of(" \t");
			if (begin == std::string_view::npos) {
				return false;
			}
			word = word.substr(begin);
			if (word.find_first_of(" \t") != std::string_view::npos) {
				return false;
			}
			ShaderTextureDefault parsed;
			if (word == "white") {
				parsed.fallback = ShaderTextureFallback::White;
			} else if (word == "black") {
				parsed.fallback = ShaderTextureFallback::Black;
			} else if (word == "normal") {
				parsed.fallback = ShaderTextureFallback::Normal;
			} else if (word == "transparent") {
				parsed.fallback = ShaderTextureFallback::Transparent;
			} else if (word == "gray") {
				parsed.fallback = ShaderTextureFallback::Gray;
			} else {
				return false;
			}
			parsed.name = str(value.substr(0, split));
			out = std::move(parsed);
			return true;
		}

		void SplitWords(std::string_view value, Vector<std::string_view>& out) {
			out.clear();
			size_t cursor = 0;
			while (cursor < value.size()) {
				size_t begin = value.find_first_not_of(" \t", cursor);
				if (begin == std::string_view::npos) {
					break;
				}
				size_t end = value.find_first_of(" \t", begin);
				if (end == std::string_view::npos) {
					end = value.size();
				}
				out.push_back(value.substr(begin, end - begin));
				cursor = end;
			}
		}

		void ParseShalltearPragmas(std::string_view source, const Path& sourcePath, ShaderPragmas& pragmas) {
			RHI::ShaderRenderState& out = pragmas.renderState;
			Vector<std::string_view> words{GetMemoryResource(MemTag::Asset)};
			size_t lineStart = 0;
			while (lineStart < source.size()) {
				size_t lineEnd = source.find('\n', lineStart);
				if (lineEnd == std::string_view::npos) {
					lineEnd = source.size();
				}
				std::string_view line = source.substr(lineStart, lineEnd - lineStart);
				lineStart = lineEnd + 1;

				size_t begin = line.find_first_not_of(" \t\r");
				if (begin == std::string_view::npos || line[begin] != '#') {
					continue;
				}
				line = line.substr(begin);
				constexpr std::string_view Prefix = "#pragma shalltear_";
				if (!line.starts_with(Prefix)) {
					continue;
				}
				line = line.substr(Prefix.size());

				const size_t keyEnd = line.find_first_of(" \t\r");
				const std::string_view bareKey = line.substr(0, keyEnd);
				if (bareKey == "depth_program") {
					const size_t valueBegin = keyEnd == std::string_view::npos ? std::string_view::npos : line.find_first_not_of(" \t\r", keyEnd);
					const std::string_view value = valueBegin == std::string_view::npos ? std::string_view{} : line.substr(valueBegin);
					pragmas.depthProgram = ShaderDepthProgram::Declared;
					std::string_view rest = value;
					while (!rest.empty()) {
						const size_t wordEnd = rest.find_first_of(" \t\r");
						const std::string_view word = rest.substr(0, wordEnd);
						if (word == "clip") {
							pragmas.depthProgram = ShaderDepthProgram::Clipping;
						} else if (word == "value") {
							pragmas.depthValue = true;
						} else if (!word.empty()) {
							SLT_LOG_WARN("Graphics", "'{}': shalltear_depth_program takes 'clip' and 'value', not '{}'", sourcePath.ToString(),
										 str(word));
						}
						const size_t next = wordEnd == std::string_view::npos ? std::string_view::npos : rest.find_first_not_of(" \t\r", wordEnd);
						rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
					}
					continue;
				}
				if (bareKey == "skip_optimizations" || bareKey == "kernel") {
					const bool kernel = bareKey == "kernel";
					(kernel ? pragmas.kernel : pragmas.skipOptimizations) = true;
					if (keyEnd != std::string_view::npos && line.find_first_not_of(" \t\r", keyEnd) != std::string_view::npos) {
						if (kernel) {
							SLT_LOG_WARN("Graphics", "'{}': shalltear_kernel takes no name (a kernel's entry point is CSMain)",
										 sourcePath.ToString());
						} else {
							SLT_LOG_WARN("Graphics", "'{}': shalltear_skip_optimizations takes no value (only 1 graphics API is compiled for)",
										 sourcePath.ToString());
						}
					}
					continue;
				}

				size_t space = line.find_first_of(" \t");
				if (space == std::string_view::npos) {
					continue;
				}
				std::string_view key = line.substr(0, space);
				std::string_view value = line.substr(space + 1);
				size_t valueBegin = value.find_first_not_of(" \t");
				size_t valueEnd = value.find_last_not_of(" \t\r");
				if (valueBegin == std::string_view::npos) {
					continue;
				}
				value = value.substr(valueBegin, valueEnd - valueBegin + 1);

				if (key == "blend") {
					if (!RHI::ParseBlendMode(value, out.blend)) {
						SLT_LOG_WARN("Graphics", "'{}': unknown blend mode '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "depth") {
					if (!RHI::ParseDepthMode(value, out.depth)) {
						SLT_LOG_WARN("Graphics", "'{}': unknown depth mode '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "depth_compare") {
					if (!RHI::ParseDepthCompare(value, out.depthCompare)) {
						SLT_LOG_WARN("Graphics", "'{}': unknown depth compare '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "cull") {
					if (!RHI::ParseCullMode(value, out.cull)) {
						SLT_LOG_WARN("Graphics", "'{}': unknown cull mode '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "topology") {
					if (value == "triangle") {
						out.topology = RHI::PrimitiveTopology::TriangleList;
					} else if (value == "line") {
						out.topology = RHI::PrimitiveTopology::LineList;
					} else if (value == "point") {
						out.topology = RHI::PrimitiveTopology::PointList;
					} else {
						SLT_LOG_WARN("Graphics", "'{}': unknown topology '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "fill") {
					if (value == "solid") {
						out.fill = RHI::FillMode::Solid;
					} else if (value == "wireframe") {
						out.fill = RHI::FillMode::Wireframe;
					} else {
						SLT_LOG_WARN("Graphics", "'{}': unknown fill mode '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "depth_bias") {
					f32 constant = 0.0f;
					f32 slope = 0.0f;
					if (ParseFloatPair(value, constant, slope)) {
						out.depthBiasConstant = constant;
						out.depthBiasSlope = slope;
					} else {
						SLT_LOG_WARN("Graphics", "'{}': depth_bias wants 2 numbers (constant slope), got '{}'",
									 sourcePath.ToString(), str(value));
					}
				} else if (key == "default") {
					ShaderMemberDefault parsed;
					ShaderTextureDefault parsedTexture;
					if (ParseMemberDefault(value, parsed)) {
						pragmas.memberDefaults.push_back(std::move(parsed));
					} else if (ParseTextureDefault(value, parsedTexture)) {
						pragmas.textureDefaults.push_back(std::move(parsedTexture));
					} else {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_default wants a member name and either 1-4 numbers or one of white|black|normal|transparent, got '{}'",
									 sourcePath.ToString(), str(value));
					}
				} else if (key == "template") {
					if (value == "none") {
						pragmas.integrator = ShaderTemplate::None;
					} else if (value == "lit") {
						pragmas.integrator = ShaderTemplate::Lit;
					} else if (value == "unlit") {
						pragmas.integrator = ShaderTemplate::Unlit;
					} else if (value == "sky") {
						pragmas.integrator = ShaderTemplate::Sky;
					} else {
						SLT_LOG_WARN("Graphics", "'{}': unknown shader template '{}'", sourcePath.ToString(), str(value));
					}
				} else if (key == "keyword") {
					SplitWords(value, words);
					i32 entryValue = -1;
					std::string_view entryName;
					bool malformed = words.empty();
					if (words.size() >= 3) {
						const char* end = words[2].data() + words[2].size();
						const auto parsed = std::from_chars(words[2].data(), end, entryValue);
						malformed = parsed.ec != std::errc{} || parsed.ptr != end || entryValue < 0;
						std::string_view rest = value.substr(static_cast<size_t>(end - value.data()));
						const size_t textBegin = rest.find_first_not_of(" \t");
						rest = textBegin == std::string_view::npos ? std::string_view{} : rest.substr(textBegin);
						if (rest.starts_with('"')) {
							const size_t close = rest.find('"', 1);
							malformed = malformed || close == std::string_view::npos;
							entryName = close == std::string_view::npos ? std::string_view{} : rest.substr(1, close - 1);
						} else {
							malformed = malformed || words.size() > 4;
							entryName = words.size() == 4 ? words[3] : std::string_view{};
						}
					}
					if (malformed) {
						SLT_LOG_WARN("Graphics",
									 "'{}': shalltear_keyword takes a name, an optional member, and for an entry its value and \"name\", got '{}'",
									 sourcePath.ToString(), str(value));
						continue;
					}
					bool duplicate = false;
					for (const str& declared : pragmas.keywords) {
						if (declared == words[0]) {
							duplicate = true;
							break;
						}
					}
					if (duplicate) {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_keyword '{}' is declared twice",
									 sourcePath.ToString(), str(words[0]));
						continue;
					}
					pragmas.keywords.push_back(str(words[0]));
					if (words.size() >= 2) {
						ShaderKeywordBinding binding;
						binding.keyword = str(words[0]);
						binding.member = str(words[1]);
						binding.value = entryValue;
						binding.entry = str(entryName);
						pragmas.keywordBindings.push_back(std::move(binding));
					}
				} else if (key == "range") {
					SplitWords(value, words);
					f32 minValue = 0.0f;
					f32 maxValue = 0.0f;
					if (words.size() != 3 || !ParseFloat(words[1], minValue) || !ParseFloat(words[2], maxValue) ||
						!(minValue < maxValue)) {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_range wants a member and 2 numbers with min < max, got '{}'",
									 sourcePath.ToString(), str(value));
						continue;
					}
					ShaderMemberRange range;
					range.name = str(words[0]);
					range.minValue = minValue;
					range.maxValue = maxValue;
					pragmas.memberRanges.push_back(std::move(range));
				} else if (key == "hint") {
					SplitWords(value, words);
					ShaderHint hint = ShaderHint::None;
					if (words.size() == 2 && words[1] == "color") {
						hint = ShaderHint::Color;
					} else if (words.size() == 2 && words[1] == "hdr") {
						hint = ShaderHint::HDR;
					} else if (words.size() == 2 && words[1] == "toggle") {
						hint = ShaderHint::Toggle;
					} else if (words.size() == 2 && words[1] == "hidden") {
						hint = ShaderHint::Hidden;
					} else {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_hint wants a member and one of color|hdr|toggle|hidden, got '{}'",
									 sourcePath.ToString(), str(value));
						continue;
					}
					ShaderMemberHint declared;
					declared.name = str(words[0]);
					declared.hint = hint;
					pragmas.memberHints.push_back(std::move(declared));
				} else if (key == "group") {
					std::string_view rest = value;
					str groupName;
					if (rest.starts_with('"')) {
						size_t close = rest.find('"', 1);
						if (close == std::string_view::npos) {
							SLT_LOG_WARN("Graphics", "'{}': shalltear_group's quoted name is unterminated in '{}'",
										 sourcePath.ToString(), str(value));
							continue;
						}
						groupName = str(rest.substr(1, close - 1));
						rest = rest.substr(close + 1);
					}
					SplitWords(rest, words);
					size_t firstMember = groupName.empty() ? 1 : 0;
					if (words.size() <= firstMember) {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_group wants a name and at least one member, got '{}'",
									 sourcePath.ToString(), str(value));
						continue;
					}
					ShaderMemberGroup group;
					group.name = groupName.empty() ? str(words[0]) : groupName;
					for (size_t i = firstMember; i < words.size(); ++i) {
						group.members.push_back(str(words[i]));
					}
					pragmas.memberGroups.push_back(std::move(group));
				} else if (key == "label") {
					SplitWords(value, words);
					std::string_view text;
					if (!words.empty()) {
						std::string_view rest = value.substr(static_cast<size_t>(words[0].data() + words[0].size() - value.data()));
						size_t textBegin = rest.find_first_not_of(" \t");
						rest = textBegin == std::string_view::npos ? std::string_view{} : rest.substr(textBegin);
						if (rest.starts_with('"')) {
							size_t close = rest.find('"', 1);
							text = close == std::string_view::npos ? std::string_view{} : rest.substr(1, close - 1);
						} else if (words.size() == 2) {
							text = words[1];
						}
					}
					if (text.empty()) {
						SLT_LOG_WARN("Graphics", "'{}': shalltear_label wants a member and its \"text\", got '{}'",
									 sourcePath.ToString(), str(value));
						continue;
					}
					ShaderMemberLabel label;
					label.name = str(words[0]);
					label.label = str(text);
					pragmas.memberLabels.push_back(std::move(label));
				} else {
					SLT_LOG_WARN("Graphics", "'{}': unknown shalltear pragma '{}'", sourcePath.ToString(), str(key));
				}
			}

			if (IsSurfaceTemplate(pragmas.integrator)) {
				bool alreadyDeclared = false;
				for (const str& declared : pragmas.keywords) {
					if (declared == ShaderAlphaClipKeyword) {
						alreadyDeclared = true;
						break;
					}
				}
				if (!alreadyDeclared) {
					pragmas.keywords.push_back(str(ShaderAlphaClipKeyword));
				}
			}
		}

		struct IntegratorTemplate {
			ShaderTemplate value;
			const char* virtualPath;
		};

		constexpr IntegratorTemplate IntegratorTemplates[] = {
			{ShaderTemplate::Lit, "engine://Shaders/Templates/ForwardLit.hlsli"},
			{ShaderTemplate::Unlit, "engine://Shaders/Templates/ForwardUnlit.hlsli"},
			{ShaderTemplate::Sky, "engine://Shaders/Templates/Sky.hlsli"},
		};

		constexpr const char* DEPTH_ONLY_TEMPLATE = "engine://Shaders/Templates/DepthOnly.hlsli";
		constexpr const char* DEPTH_NO_COLOR_DEFINE = "SHALLTEAR_DEPTH_NO_COLOR=1";
		constexpr const char* DEPTH_VALUE_DEFINE = "SHALLTEAR_DEPTH_VALUE=1";

		struct ShaderCompileInput {
			str source;
			Path physicalPath;
			u64 sourceHash = 0;
			Vector<str> defines{GetMemoryResource(MemTag::Asset)};
		};

		bool ResolveCompileInput(const Path& assetPath, str&& assetSource, ShaderTemplate integrator,
								 ShaderProgramKind kind, ShaderCompileInput& out) {
			if (integrator == ShaderTemplate::None) {
				SLT_CORE_ASSERT_MSG(kind == ShaderProgramKind::Forward,
									"a raw shader has no surface function for a 2nd integrator to wrap");
				out.physicalPath = assetPath;
				out.sourceHash = StringHash(std::string_view(assetSource)).GetValue();
				out.source = std::move(assetSource);
				return true;
			}

			const char* virtualPath = nullptr;
			if (kind == ShaderProgramKind::Depth) {
				virtualPath = DEPTH_ONLY_TEMPLATE;
			} else {
				for (const IntegratorTemplate& candidate : IntegratorTemplates) {
					if (candidate.value == integrator) {
						virtualPath = candidate.virtualPath;
						break;
					}
				}
			}
			if (!virtualPath) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' declares a template with no file behind it", assetPath.ToString());
				return false;
			}

			Path templatePath = FileSystem::Resolve(Path(virtualPath));
			if (templatePath.ToString().find("://") != str::npos) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' wants template '{}' but engine:// isn't mounted", assetPath.ToString(), virtualPath);
				return false;
			}
			str templateSource = FileSystem::ReadTextFile(templatePath, MemTag::Asset);
			if (templateSource.empty()) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' wants template '{}' (missing/unreadable)", assetPath.ToString(), templatePath.ToString());
				return false;
			}

			out.defines.push_back("SHALLTEAR_SURFACE_FILE=\"" + assetPath.ToString() + "\"");
			out.physicalPath = std::move(templatePath);
			out.sourceHash = StringHash(std::string_view(templateSource)).GetValue();
			out.source = std::move(templateSource);
			return true;
		}

		str PathKey(std::string_view path) {
			str key(path);
			for (char& c : key) {
				c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return key;
		}

		void MergeIncludes(std::span<const ShaderIncludeDependency> includes, Vector<ShaderAsset::Dependency>& dependencies) {
			for (const ShaderIncludeDependency& include : includes) {
				bool known = false;
				for (const ShaderAsset::Dependency& dependency : dependencies) {
					if (PathKey(dependency.physicalPath) == PathKey(include.physicalPath)) {
						known = true;
						break;
					}
				}
				if (!known) {
					dependencies.push_back({include.physicalPath, include.contentHash});
				}
			}
		}

		bool ObtainStage(const ShaderCompileInput& input, const Path& assetPath,
						 ShaderStageKind stage, std::span<const str> keywordDefines, ShaderCompileMode mode,
						 Vector<ShaderAsset::Dependency>& dependencies, Vector<u8>& outSpirv) {
			Vector<str> defines{GetMemoryResource(MemTag::Asset)};
			defines.assign(input.defines.begin(), input.defines.end());
			for (const str& keyword : keywordDefines) {
				defines.push_back(keyword);
			}

			const bool templated = PathKey(input.physicalPath.ToString()) != PathKey(assetPath.ToString());
			Vector<str> keyDefines{GetMemoryResource(MemTag::Asset)};
			for (const str& define : defines) {
				keyDefines.push_back(define.starts_with("SHALLTEAR_SURFACE_FILE=") ? str("SHALLTEAR_SURFACE_FILE") : define);
			}
			u64 assetHash = 0;
			if (templated) {
				str content = FileSystem::ReadTextFile(assetPath, MemTag::Asset);
				assetHash = StringHash(std::string_view(content)).GetValue();
			}
			const str assetDirectory = PathKey(std::filesystem::path(assetPath.ToString()).parent_path().generic_string());
			const u64 entryKey = ShaderCache::ComputeEntryKey(input.sourceHash, assetHash, assetDirectory,
															  {keyDefines.data(), keyDefines.size()}, stage, mode);

			Vector<ShaderIncludeDependency> cached{GetMemoryResource(MemTag::Asset)};
			if (ShaderCache::TryLoad(entryKey, outSpirv, cached)) {
				MergeIncludes({cached.data(), cached.size()}, dependencies);
				return true;
			}

			if (!ShaderCompiler::IsAvailable()) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' has no cached SPIR-V and {} is unavailable", assetPath.ToString(), ShaderCompiler::GetLibraryName());
				return false;
			}

			ShaderCompileResult compiled = ShaderCompiler::Compile(input.source, input.physicalPath, stage,
																   {defines.data(), defines.size()}, mode);
			if (!compiled.errors.empty()) {
				SLT_LOG_WARN("Graphics", "shader '{}' ({}): {}", assetPath.ToString(),
							 stage == ShaderStageKind::Vertex ? "VS" : "PS", compiled.errors);
			}
			if (!compiled.success) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: compilation failed for '{}'", assetPath.ToString());
				return false;
			}
			MergeIncludes({compiled.includes.data(), compiled.includes.size()}, dependencies);

			Vector<ShaderIncludeDependency> manifest{GetMemoryResource(MemTag::Asset)};
			const str assetKey = PathKey(assetPath.ToString());
			for (const ShaderIncludeDependency& include : compiled.includes) {
				if (PathKey(include.physicalPath) != assetKey) {
					manifest.push_back(include);
				}
			}
			ShaderCache::Store(entryKey, {manifest.data(), manifest.size()}, {compiled.spirv.data(), compiled.spirv.size()});

			outSpirv = std::move(compiled.spirv);
			return true;
		}

		// compiles must share debug info/optimization flags
		ShaderCompileMode CompileMode(bool skipOptimizations, bool forWGSL = false) {
			if (forWGSL) {
#if !defined(SHALLTEAR_CONFIG_RELEASE)
				(void)skipOptimizations;
				return ShaderCompileMode::Unoptimized;
#else
				return skipOptimizations ? ShaderCompileMode::Unoptimized : ShaderCompileMode::Optimized;
#endif
			}
#if !defined(SHALLTEAR_CONFIG_RELEASE)
			(void)skipOptimizations;
			return ShaderCompileMode::Debug;
#else
			return skipOptimizations ? ShaderCompileMode::Unoptimized : ShaderCompileMode::Optimized;
#endif
		}

		bool TranslateVariantToWGSL(ShaderVariant& variant, const Path& sourcePath) {
			auto convert = [&](Vector<u8>& blob, const char* stageName) -> bool {
				if (blob.empty()) {
					return true;
				}
				Vector<u8> wgsl{GetMemoryResource(MemTag::Asset)};
				str error;
				if (!WGSLTranslator::TranslateSpirvToWGSL({blob.data(), blob.size()}, wgsl, error)) {
					SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' ({}) SPIR-V -> WGSL failed: {}",
								  sourcePath.ToString(), stageName, error);
					return false;
				}
				blob = std::move(wgsl);
				return true;
			};
			return convert(variant.vertexSpirv, "VS") &&
				   convert(variant.pixelSpirv, "PS") &&
				   convert(variant.depthVertexSpirv, "DepthVS") &&
				   convert(variant.depthPixelSpirv, "DepthPS") &&
				   convert(variant.depthClipPixelSpirv, "DepthClipPS") &&
				   convert(variant.depthValuePixelSpirv, "DepthValuePS") &&
				   convert(variant.computeSpirv, "CS");
		}

		void KeywordDefines(std::span<const str> keywords, Vector<str>& out) {
			out.clear();
			for (const str& keyword : keywords) {
				out.push_back(keyword + "=1");
			}
		}

		constexpr const char* DEPTH_PROGRAM_DEFINE = "SHALLTEAR_DEPTH_PROGRAM=1";

		void ObtainDepthProgram(const Path& assetPath, ShaderTemplate integrator, ShaderDepthProgram rawDepth, bool rawValue,
								const ShaderCompileInput& forward,
								ShaderCompileMode mode, std::span<const str> keywordDefines, Vector<ShaderAsset::Dependency>& dependencies,
								ShaderVariant& variant) {
			variant.depthVertexSpirv.clear();
			variant.depthPixelSpirv.clear();
			variant.depthClipPixelSpirv.clear();
			variant.depthValuePixelSpirv.clear();
			if (integrator == ShaderTemplate::None) {
				if (rawDepth == ShaderDepthProgram::None) {
					return;
				}
				Vector<str> defines{GetMemoryResource(MemTag::Asset)};
				defines.assign(keywordDefines.begin(), keywordDefines.end());
				defines.push_back(str(DEPTH_PROGRAM_DEFINE));
				bool built = ObtainStage(forward, assetPath, ShaderStageKind::Vertex, {defines.data(), defines.size()}, mode, dependencies,
										 variant.depthVertexSpirv) &&
							 ObtainStage(forward, assetPath, ShaderStageKind::Pixel, {defines.data(), defines.size()}, mode, dependencies,
										 variant.depthPixelSpirv);
				if (built && rawDepth == ShaderDepthProgram::Clipping) {
					defines.push_back(str(DEPTH_NO_COLOR_DEFINE));
					built = ObtainStage(forward, assetPath, ShaderStageKind::Pixel, {defines.data(), defines.size()}, mode, dependencies,
										variant.depthClipPixelSpirv);
				}
				if (!built) {
					SLT_LOG_WARN("Graphics", "shader '{}': its depth program doesn't build (no shadows)",
								 assetPath.ToString());
					variant.depthVertexSpirv.clear();
					variant.depthPixelSpirv.clear();
					variant.depthClipPixelSpirv.clear();
					return;
				}
				if (rawValue) {
					Vector<str> valueDefines{GetMemoryResource(MemTag::Asset)};
					valueDefines.assign(keywordDefines.begin(), keywordDefines.end());
					valueDefines.push_back(str(DEPTH_PROGRAM_DEFINE));
					valueDefines.push_back(str(DEPTH_VALUE_DEFINE));
					if (!ObtainStage(forward, assetPath, ShaderStageKind::Pixel, {valueDefines.data(), valueDefines.size()}, mode, dependencies,
									 variant.depthValuePixelSpirv)) {
						SLT_LOG_WARN("Graphics", "shader '{}': its value stage doesn't build (can't be outlined or picked)",
									 assetPath.ToString());
						variant.depthValuePixelSpirv.clear();
					}
				}
				return;
			}

			if (!IsSurfaceTemplate(integrator)) {
				return;
			}
			ShaderCompileInput input;
			if (!ResolveCompileInput(assetPath, str{}, integrator, ShaderProgramKind::Depth, input)) {
				return;
			}

			bool known = false;
			for (const ShaderAsset::Dependency& dependency : dependencies) {
				if (dependency.physicalPath == input.physicalPath.ToString()) {
					known = true;
					break;
				}
			}
			if (!known) {
				dependencies.push_back({input.physicalPath.ToString(), input.sourceHash});
			}

			bool built = ObtainStage(input, assetPath, ShaderStageKind::Vertex, keywordDefines, mode,
									 dependencies, variant.depthVertexSpirv) &&
						 ObtainStage(input, assetPath, ShaderStageKind::Pixel, keywordDefines, mode,
									 dependencies, variant.depthPixelSpirv);
			if (built) {
				Vector<str> clipDefines{GetMemoryResource(MemTag::Asset)};
				clipDefines.assign(keywordDefines.begin(), keywordDefines.end());
				clipDefines.push_back(str(DEPTH_NO_COLOR_DEFINE));
				built = ObtainStage(input, assetPath, ShaderStageKind::Pixel, {clipDefines.data(), clipDefines.size()},
									mode, dependencies, variant.depthClipPixelSpirv);
			}
			if (!built) {
				SLT_LOG_WARN("Graphics", "shader '{}': the depth-only program doesn't build (no shadows)",
							 assetPath.ToString());
				variant.depthVertexSpirv.clear();
				variant.depthPixelSpirv.clear();
				variant.depthClipPixelSpirv.clear();
				return;
			}

			Vector<str> valueDefines{GetMemoryResource(MemTag::Asset)};
			valueDefines.assign(keywordDefines.begin(), keywordDefines.end());
			valueDefines.push_back(str(DEPTH_VALUE_DEFINE));
			if (!ObtainStage(input, assetPath, ShaderStageKind::Pixel, {valueDefines.data(), valueDefines.size()}, mode, dependencies,
							 variant.depthValuePixelSpirv)) {
				SLT_LOG_WARN("Graphics", "shader '{}': its value stage doesn't build (can't be outlined or picked)",
							 assetPath.ToString());
				variant.depthValuePixelSpirv.clear();
			}
		}

		bool DepthProgramFitsInterface(const ShaderAsset& shader, const ShaderVariant& variant, str& outDifference) {
			SpirvReflection vertexReflection = ReflectSpirv({variant.depthVertexSpirv.data(), variant.depthVertexSpirv.size()});
			auto fits = [&](const Vector<u8>& pixelSpirv) {
				SpirvReflection pixelReflection = ReflectSpirv({pixelSpirv.data(), pixelSpirv.size()});
				Vector<RHI::DescriptorBindingDesc> depthBindings{GetMemoryResource(MemTag::Asset)};
				u32 depthPushConstantSize = 0;
				if (!MergeReflections(vertexReflection, pixelReflection, depthBindings, depthPushConstantSize, outDifference)) {
					return false;
				}
				if (depthPushConstantSize > shader.pushConstantSize) {
					outDifference = "its push constant block is larger than the forward program's";
					return false;
				}
				for (const RHI::DescriptorBindingDesc& declared : depthBindings) {
					bool found = false;
					for (const RHI::DescriptorBindingDesc& known : shader.bindings) {
						if (known.set == declared.set && known.binding == declared.binding && known.type == declared.type) {
							found = true;
							break;
						}
					}
					if (!found) {
						outDifference = "it declares a descriptor the forward program does not";
						return false;
					}
				}
				return true;
			};
			if (!fits(variant.depthPixelSpirv)) {
				return false;
			}
			if (!variant.depthClipPixelSpirv.empty() && !fits(variant.depthClipPixelSpirv)) {
				return false;
			}
			return variant.depthValuePixelSpirv.empty() || fits(variant.depthValuePixelSpirv);
		}

	} // anonymous namespace

	bool ShaderInterfacesMatch(const ShaderAsset& shader, const Vector<RHI::DescriptorBindingDesc>& bindings,
							   u32 pushConstantSize, const Vector<UniformBlockLayout>& blocks,
							   const Vector<ShaderResourceInfo>& resources, str& outDifference) {
		if (pushConstantSize != shader.pushConstantSize) {
			outDifference = "the push constant block changed size";
			return false;
		}
		if (bindings.size() != shader.bindings.size()) {
			outDifference = "the descriptor binding count changed";
			return false;
		}
		for (size_t i = 0; i < bindings.size(); ++i) {
			const RHI::DescriptorBindingDesc& a = bindings[i];
			const RHI::DescriptorBindingDesc& b = shader.bindings[i];
			if (a.set != b.set || a.binding != b.binding || a.type != b.type || a.stages != b.stages) {
				outDifference = "a descriptor binding moved, changed type or changed stages";
				return false;
			}
		}
		if (blocks.size() != shader.uniformBlocks.size()) {
			outDifference = "the uniform block count changed";
			return false;
		}
		for (size_t i = 0; i < blocks.size(); ++i) {
			const UniformBlockLayout& a = blocks[i];
			const UniformBlockLayout& b = shader.uniformBlocks[i];
			if (a.set != b.set || a.binding != b.binding || a.sizeBytes != b.sizeBytes || a.members.size() != b.members.size()) {
				outDifference = "a uniform block changed size or member count";
				return false;
			}
			for (size_t m = 0; m < a.members.size(); ++m) {
				if (a.members[m].name != b.members[m].name || a.members[m].type != b.members[m].type ||
					a.members[m].offset != b.members[m].offset || a.members[m].sizeBytes != b.members[m].sizeBytes) {
					outDifference = "a uniform block member changed name, type or offset";
					return false;
				}
			}
		}
		if (resources.size() != shader.resources.size()) {
			outDifference = "the declared resource count changed";
			return false;
		}
		for (size_t i = 0; i < resources.size(); ++i) {
			if (resources[i].set != shader.resources[i].set || resources[i].binding != shader.resources[i].binding ||
				resources[i].type != shader.resources[i].type || resources[i].name != shader.resources[i].name ||
				resources[i].dimension != shader.resources[i].dimension) {
				outDifference = "a declared resource moved, was renamed or changed dimension";
				return false;
			}
		}
		return true;
	}

	void ShaderAsset::ParsePragmas(std::string_view source, const Path& sourcePath, ShaderPragmas& out) {
		ParseShalltearPragmas(source, sourcePath, out);
	}

	bool ShaderAsset::ImportFromSource(const Path& virtualOrPhysical) {
		struct Recount {
			ShaderAsset& shader;
			~Recount() { shader.RecountMeshReader(); }
		} recount{*this};
		if (!FileSystem::Exists(virtualOrPhysical)) {
			SLT_LOG_ERROR("Graphics", "ShaderAsset: source '{}' doesn't exist", virtualOrPhysical.ToString());
			return false;
		}
		Path sourcePath = FileSystem::Resolve(virtualOrPhysical);
		str source = FileSystem::ReadTextFile(sourcePath, MemTag::Asset);
		if (source.empty()) {
			SLT_LOG_ERROR("Graphics", "ShaderAsset: source '{}' is empty/unreadable", sourcePath.ToString());
			return false;
		}
		u64 sourceHash = StringHash(std::string_view(source)).GetValue();

		ShaderPragmas pragmas;
		pragmas.renderState = renderState;
		ParseShalltearPragmas(source, sourcePath, pragmas);
		renderState = pragmas.renderState;
		if (pragmas.integrator == ShaderTemplate::Sky) {
			const RHI::ShaderRenderState declared = pragmas.renderState;
			const RHI::ShaderRenderState sky = SkyRenderState();
			const RHI::ShaderRenderState plain;
			const bool asked = (declared.blend != plain.blend && declared.blend != sky.blend) ||
							   (declared.depth != plain.depth && declared.depth != sky.depth) ||
							   (declared.cull != plain.cull && declared.cull != sky.cull) ||
							   (declared.depthCompare != plain.depthCompare && declared.depthCompare != sky.depthCompare);
			if (asked) {
				SLT_LOG_WARN("Graphics", "'{}': a sky draws behind everything with the template's state (blend, depth and cull pragmas ignored)",
							 sourcePath.ToString());
			}
			renderState = sky;
		}

		keywords = std::move(pragmas.keywords);
		integrator = pragmas.integrator;
		skipOptimizations = pragmas.skipOptimizations;
		depthProgram = integrator == ShaderTemplate::None && !pragmas.kernel ? pragmas.depthProgram : ShaderDepthProgram::None;
		depthValue = depthProgram != ShaderDepthProgram::None && pragmas.depthValue;
		if (pragmas.kernel && pragmas.depthProgram != ShaderDepthProgram::None) {
			SLT_LOG_WARN("Graphics", "'{}': a kernel has no depth program (shalltear_depth_program ignored)", sourcePath.ToString());
		}

		const RHI::ShaderBytecodeFormat targetFormat = GetDefaultImportBytecodeFormat();
		const bool forWGSL = targetFormat == RHI::ShaderBytecodeFormat::WGSL;

		ShaderCompileInput input;
		if (!ResolveCompileInput(sourcePath, std::move(source), integrator, ShaderProgramKind::Forward, input)) {
			return false;
		}
		if (forWGSL) {
			input.defines.push_back("SHALLTEAR_WEBGPU=1");
		}

		dependencies.clear();
		dependencies.push_back({sourcePath.ToString(), sourceHash});
		if (integrator != ShaderTemplate::None) {
			dependencies.push_back({input.physicalPath.ToString(), input.sourceHash});
		}

		ShaderVariant base;
		base.keywordHash = KeywordSetHash({});
		if (pragmas.kernel) {
			if (integrator != ShaderTemplate::None) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' declares a kernel and a template (a kernel takes no template)", sourcePath.ToString());
				return false;
			}
			if (!keywords.empty()) {
				SLT_LOG_WARN("Graphics", "'{}': a kernel takes no shalltear_keyword ({} keyword(s) dropped)", sourcePath.ToString(), keywords.size());
				keywords.clear();
			}
			if (!ObtainStage(input, sourcePath, ShaderStageKind::Compute, {}, CompileMode(skipOptimizations, forWGSL),
							 dependencies, base.computeSpirv)) {
				return false;
			}
			SpirvReflection kernelReflection = ReflectSpirv({base.computeSpirv.data(), base.computeSpirv.size()});
			if (!kernelReflection.valid) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' reflection rejected: {}", sourcePath.ToString(), kernelReflection.error);
				return false;
			}
			for (const RHI::DescriptorBindingDesc& binding : kernelReflection.bindings) {
				if (binding.set >= RHI::MAX_DESCRIPTOR_SETS) {
					SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' binds set {} (a pipeline has {})", sourcePath.ToString(), binding.set, RHI::MAX_DESCRIPTOR_SETS);
					return false;
				}
			}
			bindings = kernelReflection.bindings;
			pushConstantSize = kernelReflection.pushConstantSize;
			threadGroupSize = kernelReflection.threadGroupSize;
			MergeMaterialInterfaces(kernelReflection, SpirvReflection{}, uniformBlocks, resources);
		} else {
			if (!ObtainStage(input, sourcePath, ShaderStageKind::Vertex, {}, CompileMode(skipOptimizations, forWGSL),
							 dependencies, base.vertexSpirv)) {
				return false;
			}
			if (!ObtainStage(input, sourcePath, ShaderStageKind::Pixel, {}, CompileMode(skipOptimizations, forWGSL),
							 dependencies, base.pixelSpirv)) {
				return false;
			}

			SpirvReflection vertexReflection = ReflectSpirv({base.vertexSpirv.data(), base.vertexSpirv.size()});
			SpirvReflection pixelReflection = ReflectSpirv({base.pixelSpirv.data(), base.pixelSpirv.size()});
			str mergeError;
			if (!MergeReflections(vertexReflection, pixelReflection, bindings, pushConstantSize, mergeError)) {
				SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' reflection rejected: {}", sourcePath.ToString(), mergeError);
				return false;
			}
			threadGroupSize = {};

			MergeMaterialInterfaces(vertexReflection, pixelReflection, uniformBlocks, resources);
		}

		u32 materialTextures = 0;
		u32 materialSamplers = 0;
		u32 materialResources = 0;
		for (const ShaderResourceInfo& resource : resources) {
			if (pragmas.kernel || resource.set != MATERIAL_DESCRIPTOR_SET) {
				continue;
			}
			++materialResources;
			if (resource.type == RHI::DescriptorType::SampledTexture) {
				++materialTextures;
			} else if (resource.type == RHI::DescriptorType::Sampler) {
				++materialSamplers;
			}
		}
		if (materialTextures != materialSamplers) {
			SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' declares {} set-{} texture(s) and {} sampler(s). Each texture needs a sampler, paired in binding order",
						  sourcePath.ToString(), materialTextures, MATERIAL_DESCRIPTOR_SET, materialSamplers);
			return false;
		}
		if (materialResources > MAX_MATERIAL_SET_RESOURCES) {
			SLT_LOG_ERROR("Graphics", "ShaderAsset: '{}' declares {} set-{} resources (transient set writer takes {})",
						  sourcePath.ToString(), materialResources, MATERIAL_DESCRIPTOR_SET, MAX_MATERIAL_SET_RESOURCES);
			return false;
		}

		memberDefaults.clear();
		const UniformBlockLayout* materialLayout = GetMaterialParamLayout();
		for (ShaderMemberDefault& declared : pragmas.memberDefaults) {
			const UniformMember* member = nullptr;
			if (materialLayout) {
				for (const UniformMember& candidate : materialLayout->members) {
					if (candidate.name == declared.name) {
						member = &candidate;
						break;
					}
				}
			}
			if (!member) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_default sets '{}' (the set-{} block doesn't declare it)",
							 sourcePath.ToString(), declared.name, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			u32 expected = UniformMemberFloatCount(member->type);
			if (expected == 0) {
				expected = member->type == UniformMemberType::Unsupported ? 0u : 1u;
			}
			if (expected == 0 || declared.count != expected) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_default for '{}' has {} value(s) (a {} member takes {})",
							 sourcePath.ToString(), declared.name, declared.count,
							 str(UniformMemberTypeName(member->type)), expected);
				continue;
			}
			memberDefaults.push_back(std::move(declared));
		}

		textureDefaults.clear();
		for (ShaderTextureDefault& declared : pragmas.textureDefaults) {
			bool slotExists = false;
			for (const ShaderResourceInfo& resource : resources) {
				if (resource.set == MATERIAL_DESCRIPTOR_SET &&
					resource.type == RHI::DescriptorType::SampledTexture &&
					resource.name == declared.name) {
					slotExists = true;
					break;
				}
			}
			if (!slotExists) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_default sets '{}' (not a set-{} texture slot)",
							 sourcePath.ToString(), declared.name, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			textureDefaults.push_back(std::move(declared));
		}

		auto findMember = [materialLayout](std::string_view name) -> const UniformMember* {
			if (!materialLayout) {
				return nullptr;
			}
			for (const UniformMember& candidate : materialLayout->members) {
				if (candidate.name == name) {
					return &candidate;
				}
			}
			return nullptr;
		};

		memberRanges.clear();
		for (ShaderMemberRange& declared : pragmas.memberRanges) {
			const UniformMember* member = findMember(declared.name);
			if (!member) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_range on '{}' (the set-{} block doesn't declare it)",
							 sourcePath.ToString(), declared.name, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			if (member->type != UniformMemberType::Float && member->type != UniformMemberType::Int) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_range on '{}' is a {} (a slider takes a Float or an Int)",
							 sourcePath.ToString(), declared.name, str(UniformMemberTypeName(member->type)));
				continue;
			}
			memberRanges.push_back(std::move(declared));
		}

		auto isTextureSlot = [this](std::string_view name) {
			for (const ShaderResourceInfo& resource : resources) {
				if (resource.set == MATERIAL_DESCRIPTOR_SET && resource.type == RHI::DescriptorType::SampledTexture &&
					resource.name == name) {
					return true;
				}
			}
			return false;
		};

		memberHints.clear();
		for (ShaderMemberHint& declared : pragmas.memberHints) {
			if (FindMemberHint(declared.name) != ShaderHint::None) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_hint sets '{}' twice (1st one wins)",
							 sourcePath.ToString(), declared.name);
				continue;
			}
			const UniformMember* member = findMember(declared.name);
			if (declared.hint == ShaderHint::Hidden && (member || isTextureSlot(declared.name))) {
				memberHints.push_back(std::move(declared));
				continue;
			}
			if (!member) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_hint on '{}' (the set-{} block doesn't declare it)",
							 sourcePath.ToString(), declared.name, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			bool fits = declared.hint == ShaderHint::Toggle
							? member->type == UniformMemberType::Int
							: member->type == UniformMemberType::Vec3 || member->type == UniformMemberType::Vec4;
			if (!fits) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_hint on '{}' is a {} ({})", sourcePath.ToString(), declared.name,
							 str(UniformMemberTypeName(member->type)),
							 declared.hint == ShaderHint::Toggle ? "a checkbox takes an Int" : "a color picker takes a Vec3 or a Vec4");
				continue;
			}
			memberHints.push_back(std::move(declared));
		}

		memberLabels.clear();
		for (ShaderMemberLabel& declared : pragmas.memberLabels) {
			if (!findMember(declared.name) && !isTextureSlot(declared.name)) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_label on '{}' (neither a set-{} member nor a texture slot)",
							 sourcePath.ToString(), declared.name, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			memberLabels.push_back(std::move(declared));
		}

		memberGroups.clear();
		for (ShaderMemberGroup& declared : pragmas.memberGroups) {
			ShaderMemberGroup kept;
			kept.name = declared.name;
			for (const str& memberName : declared.members) {
				if (!findMember(memberName) && !isTextureSlot(memberName)) {
					SLT_LOG_WARN("Graphics", "'{}': shalltear_group '{}' lists '{}' (neither a set-{} member nor a texture slot)",
								 sourcePath.ToString(), declared.name, memberName, MATERIAL_DESCRIPTOR_SET);
					continue;
				}
				kept.members.push_back(memberName);
			}
			if (kept.members.empty()) {
				continue;
			}
			memberGroups.push_back(std::move(kept));
		}

		keywordBindings.clear();
		for (ShaderKeywordBinding& declared : pragmas.keywordBindings) {
			const UniformMember* member = findMember(declared.member);
			if (!member) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_keyword '{}' binds '{}' (the set-{} block doesn't declare it)",
							 sourcePath.ToString(), declared.keyword, declared.member, MATERIAL_DESCRIPTOR_SET);
				continue;
			}
			if (member->type != UniformMemberType::Float && member->type != UniformMemberType::Int) {
				SLT_LOG_WARN("Graphics", "'{}': shalltear_keyword '{}' binds '{}', a {} (a toggle writes a Float or an Int)",
							 sourcePath.ToString(), declared.keyword, declared.member,
							 str(UniformMemberTypeName(member->type)));
				continue;
			}
			keywordBindings.push_back(std::move(declared));
		}
		Vector<str> brokenGroups{GetMemoryResource(MemTag::Asset)};
		for (const ShaderKeywordBinding& entry : keywordBindings) {
			if (entry.value < 0 || std::find(brokenGroups.begin(), brokenGroups.end(), entry.member) != brokenGroups.end()) {
				continue;
			}
			u32 count = 0;
			u64 seen = 0;
			bool whole = true;
			for (const ShaderKeywordBinding& other : keywordBindings) {
				if (other.member != entry.member) {
					continue;
				}
				whole = whole && other.value >= 0 && other.value < 64 && (seen & (1ull << other.value)) == 0;
				seen |= whole ? 1ull << other.value : 0;
				++count;
			}
			whole = whole && count >= 2 && seen == (count == 64 ? ~0ull : (1ull << count) - 1);
			if (!whole) {
				SLT_LOG_WARN("Graphics", "'{}': the shalltear_keyword entries bound to '{}' aren't 1 value each from 0 up, 2 at least, "
										 "with no toggle beside them (row dropped)",
							 sourcePath.ToString(), entry.member);
				brokenGroups.push_back(entry.member);
			}
		}
		std::erase_if(keywordBindings, [&](const ShaderKeywordBinding& binding) {
			return std::find(brokenGroups.begin(), brokenGroups.end(), binding.member) != brokenGroups.end();
		});

		Vector<str> depthDefines{GetMemoryResource(MemTag::Asset)};
		if (forWGSL) {
			depthDefines.push_back("SHALLTEAR_WEBGPU=1");
		}
		ObtainDepthProgram(sourcePath, integrator, depthProgram, depthValue, input, CompileMode(skipOptimizations, forWGSL),
						   {depthDefines.data(), depthDefines.size()}, dependencies, base);
		if (base.HasProgram(ShaderProgramKind::Depth)) {
			str difference;
			if (!DepthProgramFitsInterface(*this, base, difference)) {
				SLT_LOG_ERROR("Graphics", "shader '{}': its depth only program {} (no shadows)",
							  sourcePath.ToString(), difference);
				base.depthVertexSpirv.clear();
				base.depthPixelSpirv.clear();
				base.depthClipPixelSpirv.clear();
				base.depthValuePixelSpirv.clear();
			}
		}

		if (forWGSL && !TranslateVariantToWGSL(base, sourcePath)) {
			return false;
		}
		bytecodeFormat = targetFormat;

		variants.clear();
		variants.push_back(std::move(base));

		++version;
		return true;
	}

	u32 ShaderAsset::CompileVariant(std::span<const str> keywordSet, u64 hash) {
		Vector<str> wanted{GetMemoryResource(MemTag::Asset)};
		wanted.assign(keywordSet.begin(), keywordSet.end());

		Path sourcePath(dependencies[0].physicalPath);
		str source = FileSystem::ReadTextFile(sourcePath, MemTag::Asset);
		if (source.empty()) {
			SLT_LOG_WARN("Graphics", "shader '{}': source '{}' is gone (drawing the base variant)", virtualPath, sourcePath.ToString());
			return SHADER_BASE_VARIANT;
		}

		const bool forWGSL = bytecodeFormat == RHI::ShaderBytecodeFormat::WGSL;
		ShaderCompileInput input;
		if (!ResolveCompileInput(sourcePath, std::move(source), integrator, ShaderProgramKind::Forward, input)) {
			SLT_LOG_WARN("Graphics", "shader '{}': its integrator template is unavailable (drawing the base variant)", virtualPath);
			return SHADER_BASE_VARIANT;
		}
		if (forWGSL) {
			input.defines.push_back("SHALLTEAR_WEBGPU=1");
		}

		Vector<str> defines{GetMemoryResource(MemTag::Asset)};
		KeywordDefines({wanted.data(), wanted.size()}, defines);
		if (forWGSL) {
			defines.push_back("SHALLTEAR_WEBGPU=1");
		}

		ShaderVariant variant;
		Vector<Dependency> scratchDependencies{GetMemoryResource(MemTag::Asset)};
		scratchDependencies.assign(dependencies.begin(), dependencies.end());
		if (!ObtainStage(input, sourcePath, ShaderStageKind::Vertex, {defines.data(), defines.size()},
						 CompileMode(skipOptimizations, forWGSL), scratchDependencies, variant.vertexSpirv) ||
			!ObtainStage(input, sourcePath, ShaderStageKind::Pixel, {defines.data(), defines.size()},
						 CompileMode(skipOptimizations, forWGSL), scratchDependencies, variant.pixelSpirv)) {
			SLT_LOG_ERROR("Graphics", "shader '{}': variant compile failed (drawing the base variant)", virtualPath);
			return SHADER_BASE_VARIANT;
		}

		SpirvReflection vertexReflection = ReflectSpirv({variant.vertexSpirv.data(), variant.vertexSpirv.size()});
		SpirvReflection pixelReflection = ReflectSpirv({variant.pixelSpirv.data(), variant.pixelSpirv.size()});
		Vector<RHI::DescriptorBindingDesc> variantBindings{GetMemoryResource(MemTag::Asset)};
		Vector<UniformBlockLayout> variantBlocks{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderResourceInfo> variantResources{GetMemoryResource(MemTag::Asset)};
		u32 variantPushConstantSize = 0;
		str error;
		if (!MergeReflections(vertexReflection, pixelReflection, variantBindings, variantPushConstantSize, error)) {
			SLT_LOG_ERROR("Graphics", "shader '{}': variant reflection rejected: {} (drawing the base variant)", virtualPath, error);
			return SHADER_BASE_VARIANT;
		}
		MergeMaterialInterfaces(vertexReflection, pixelReflection, variantBlocks, variantResources);
		if (!ShaderInterfacesMatch(*this, variantBindings, variantPushConstantSize, variantBlocks, variantResources, error)) {
			str names;
			for (const str& keyword : wanted) {
				names += (names.empty() ? "" : " ") + keyword;
			}
			SLT_LOG_ERROR("Graphics", "shader '{}': keyword set '{}' changes the shader's interface ({}). Drawing the base variant",
						  virtualPath, names, error);
			return SHADER_BASE_VARIANT;
		}

		ObtainDepthProgram(sourcePath, integrator, depthProgram, depthValue, input, CompileMode(skipOptimizations, forWGSL),
						   {defines.data(), defines.size()}, scratchDependencies, variant);
		if (variant.HasProgram(ShaderProgramKind::Depth) && !DepthProgramFitsInterface(*this, variant, error)) {
			SLT_LOG_ERROR("Graphics", "shader '{}': this keyword set's depth-only program {} (no shadows)", virtualPath, error);
			variant.depthVertexSpirv.clear();
			variant.depthPixelSpirv.clear();
			variant.depthClipPixelSpirv.clear();
			variant.depthValuePixelSpirv.clear();
		}

		if (forWGSL && !TranslateVariantToWGSL(variant, sourcePath)) {
			return SHADER_BASE_VARIANT;
		}

		variant.keywordHash = hash;
		variant.keywords = std::move(wanted);
		dependencies = std::move(scratchDependencies);
		variants.push_back(std::move(variant));
		return static_cast<u32>(variants.size() - 1);
	}

} // namespace Shalltear
