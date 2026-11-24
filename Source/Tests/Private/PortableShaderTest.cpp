#include "TestsPCH.h"

#include "Checkout.h"

#ifndef SHALLTEAR_SHARED
#include "Graphics/ShaderCompiler.h"
#include "Graphics/WGSLTranslator.h"
#endif

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/ShaderReflection.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Shalltear;

#ifndef SHALLTEAR_SHARED

namespace {

	struct ForbiddenSpirvFindings {
		bool physicalStorageBuffer = false;
		bool int64 = false;
		bool runtimeDescriptorArray = false;
		bool pushConstant = false;
	};

	ForbiddenSpirvFindings ScanSpirvForWebGPUCompat(std::span<const u8> blob) {
		ForbiddenSpirvFindings findings;
		if (blob.size() < 20 || blob.size() % 4 != 0) {
			return findings;
		}
		constexpr u16 OP_CAPABILITY = 17;
		constexpr u16 OP_VARIABLE = 59;
		constexpr u32 CAP_INT64 = 11;
		constexpr u32 CAP_RUNTIME_DESCRIPTOR_ARRAY = 5302;
		constexpr u32 CAP_PHYSICAL_STORAGE_BUFFER_ADDRESSES = 5347;
		constexpr u32 STORAGE_PUSH_CONSTANT = 9;

		const size_t wordCount = blob.size() / 4;
		auto word = [&](size_t at) {
			u32 value = 0;
			std::memcpy(&value, blob.data() + at * 4, 4);
			return value;
		};
		for (size_t cursor = 5; cursor < wordCount;) {
			const u32 first = word(cursor);
			const u16 op = static_cast<u16>(first & 0xFFFFu);
			const u16 length = static_cast<u16>(first >> 16);
			if (length == 0 || cursor + length > wordCount) {
				break;
			}
			if (op == OP_CAPABILITY && length >= 2) {
				const u32 cap = word(cursor + 1);
				if (cap == CAP_INT64) {
					findings.int64 = true;
				} else if (cap == CAP_RUNTIME_DESCRIPTOR_ARRAY) {
					findings.runtimeDescriptorArray = true;
				} else if (cap == CAP_PHYSICAL_STORAGE_BUFFER_ADDRESSES) {
					findings.physicalStorageBuffer = true;
				}
			} else if (op == OP_VARIABLE && length >= 4 && word(cursor + 3) == STORAGE_PUSH_CONSTANT) {
				findings.pushConstant = true;
			}
			cursor += length;
		}
		return findings;
	}

	const char* TemplateFileFor(ShaderTemplate integrator) {
		switch (integrator) {
		case ShaderTemplate::Lit:
			return "Templates/ForwardLit.hlsli";
		case ShaderTemplate::Unlit:
			return "Templates/ForwardUnlit.hlsli";
		case ShaderTemplate::Sky:
			return "Templates/Sky.hlsli";
		case ShaderTemplate::None:
			break;
		}
		return nullptr;
	}

} // anonymous namespace

TEST(PortableShaderTest, AllEngineShadersCompileForWebGPUWithoutVulkanOnlyFeatures) {
	if (!ShaderCompiler::IsAvailable()) {
		GTEST_SKIP() << "DXC unavailable";
	}
	const std::filesystem::path engineAssets = Checkout::Find("EngineAssets");
	ASSERT_FALSE(engineAssets.empty());
	const std::filesystem::path shadersDir = engineAssets / "Shaders";
	ASSERT_TRUE(std::filesystem::is_directory(shadersDir));

	FileSystem::Mount("engine", Path(engineAssets.generic_string()));
	struct EngineUnmount {
		~EngineUnmount() { FileSystem::Unmount("engine"); }
	} unmount;

	u32 compiledShaders = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(shadersDir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".hlsl") {
			continue;
		}
		const str assetPath = entry.path().generic_string();
		const str assetSource = FileSystem::ReadTextFile(Path(assetPath), MemTag::Asset);
		ASSERT_FALSE(assetSource.empty()) << assetPath;

		ShaderPragmas pragmas;
		ShaderAsset::ParsePragmas(assetSource, Path(assetPath), pragmas);

		// ParticleArguments/ParticleReset/ParticleStrips/ParticleSort* include engine-generated wrapper stubs in VisualEffects
		if (assetPath.find("/VisualEffects/") != str::npos) {
			continue;
		}

		str compileSource = assetSource;
		Path compilePath(assetPath);
		Vector<str> baseDefines{GetMemoryResource(MemTag::Asset)};
		baseDefines.push_back("SHALLTEAR_WEBGPU=1");

		if (const char* templateRel = TemplateFileFor(pragmas.integrator)) {
			const std::filesystem::path templatePath = shadersDir / templateRel;
			compileSource = FileSystem::ReadTextFile(Path(templatePath.generic_string()), MemTag::Asset);
			ASSERT_FALSE(compileSource.empty()) << templatePath.generic_string();
			compilePath = Path(templatePath.generic_string());
			baseDefines.push_back("SHALLTEAR_SURFACE_FILE=\"" + assetPath + "\"");
		}

		auto checkStage = [&](ShaderStageKind stage) {
			ShaderCompileResult result = ShaderCompiler::Compile(
				compileSource, compilePath, stage, {baseDefines.data(), baseDefines.size()}, ShaderCompileMode::Unoptimized);
			ASSERT_TRUE(result.success) << assetPath << " stage " << static_cast<int>(stage) << ": " << result.errors;

			const ForbiddenSpirvFindings findings = ScanSpirvForWebGPUCompat({result.spirv.data(), result.spirv.size()});
			EXPECT_FALSE(findings.physicalStorageBuffer) << assetPath << " uses PhysicalStorageBufferAddresses";
			EXPECT_FALSE(findings.int64) << assetPath << " uses Int64";
			EXPECT_FALSE(findings.runtimeDescriptorArray) << assetPath << " uses RuntimeDescriptorArray";
			EXPECT_FALSE(findings.pushConstant) << assetPath << " uses PushConstant";

			SpirvReflection reflection = ReflectSpirv({result.spirv.data(), result.spirv.size()});
			EXPECT_TRUE(reflection.valid) << assetPath << " reflect failed: " << reflection.error;

			if (WGSLTranslator::IsAvailable()) {
				Vector<u8> wgsl{GetMemoryResource(MemTag::Asset)};
				str wgslError;
				EXPECT_TRUE(WGSLTranslator::TranslateSpirvToWGSL({result.spirv.data(), result.spirv.size()}, wgsl, wgslError))
					<< assetPath << " stage " << static_cast<int>(stage) << " Tint failed: " << wgslError;
				EXPECT_FALSE(wgsl.empty()) << assetPath;
			}
		};

		if (pragmas.kernel) {
			checkStage(ShaderStageKind::Compute);
		} else {
			checkStage(ShaderStageKind::Vertex);
			checkStage(ShaderStageKind::Pixel);
		}
		++compiledShaders;
	}

	EXPECT_GE(compiledShaders, 15u);
}

#endif

TEST(PortableShaderTest, ShaderAssetCookRoundTripsBytecodeFormatAndReadOnlyBindings) {
	ShaderAsset original;
	original.bytecodeFormat = RHI::ShaderBytecodeFormat::WGSL;
	original.pushConstantSize = 32;
	original.bindings.push_back({0, 0, RHI::DescriptorType::UniformBuffer, RHI::ShaderStage::Vertex | RHI::ShaderStage::Pixel, false});
	original.bindings.push_back({3, 1, RHI::DescriptorType::StorageBuffer, RHI::ShaderStage::Vertex | RHI::ShaderStage::Pixel, true});
	original.bindings.push_back({3, 2, RHI::DescriptorType::StorageBuffer, RHI::ShaderStage::Compute, false});
	ShaderVariant variant;
	const char wgsl[] = "@vertex fn VSMain() -> @builtin(position) vec4<f32> { return vec4<f32>(0.0); }";
	variant.vertexSpirv.assign(reinterpret_cast<const u8*>(wgsl), reinterpret_cast<const u8*>(wgsl) + sizeof(wgsl) - 1);
	original.variants.push_back(std::move(variant));

	PakStringPool strings;
	Vector<u8> descBytes{GetMemoryResource(MemTag::Asset)};
	Vector<u8> payloadBytes{GetMemoryResource(MemTag::Asset)};
	CookWriter descWriter(descBytes, strings);
	CookWriter payloadWriter(payloadBytes, strings);
	ASSERT_TRUE(original.Cook(descWriter, payloadWriter));

	PakStringTable table;
	Vector<u8> stringBlob{GetMemoryResource(MemTag::Asset)};
	PakWriteStringBlob(stringBlob, strings.Texts());
	ASSERT_TRUE(PakReadStringBlob(stringBlob.data(), stringBlob.size(), table));

	CookReader descReader(descBytes.data(), descBytes.size(), table);
	CookReader payloadReader(payloadBytes.data(), payloadBytes.size());
	ShaderAsset restored;
	ASSERT_TRUE(restored.LoadCooked(descReader, payloadReader));

	EXPECT_EQ(restored.bytecodeFormat, RHI::ShaderBytecodeFormat::WGSL);
	ASSERT_EQ(restored.bindings.size(), 3u);
	EXPECT_FALSE(restored.bindings[0].readOnly);
	EXPECT_TRUE(restored.bindings[1].readOnly);
	EXPECT_FALSE(restored.bindings[2].readOnly);
	EXPECT_EQ(restored.bindings[1].stages, RHI::ShaderStage::Vertex | RHI::ShaderStage::Pixel);
}
