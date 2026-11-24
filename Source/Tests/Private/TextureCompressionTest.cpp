#include "TestsPCH.h"

#include "CookHarness.h"

#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Math/HalfFloat.h"

#include "Shalltear/Core/Graphics/TextureCodecs.h"
#include "TextureCompression.h"

#include <cmath>
#include <cstring>
#include <functional>

using namespace Shalltear;

namespace {

	Vector<u8> Solid(Vec2I size, u32 slices, std::initializer_list<u8> rgba) {
		Vector<u8> out{GetMemoryResource(MemTag::Asset)};
		out.reserve(static_cast<size_t>(size.x) * size.y * 4 * slices);
		for (i32 i = 0; i < size.x * size.y * static_cast<i32>(slices); ++i) {
			out.insert(out.end(), rgba.begin(), rgba.end());
		}
		return out;
	}

	Vector<u8> Busy(Vec2I size) {
		Vector<u8> out{GetMemoryResource(MemTag::Asset)};
		out.resize(static_cast<size_t>(size.x) * size.y * 4);
		for (i32 y = 0; y < size.y; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				u8* texel = out.data() + (static_cast<size_t>(y) * size.x + x) * 4;
				const u32 hash = (static_cast<u32>(x) * 73856093u) ^ (static_cast<u32>(y) * 19349663u);
				texel[0] = static_cast<u8>(x * 255 / std::max(1, size.x - 1));
				texel[1] = static_cast<u8>(y * 255 / std::max(1, size.y - 1));
				texel[2] = static_cast<u8>(((x / 4 + y / 4) % 2) ? 200 : 40);
				texel[3] = static_cast<u8>(128 + (hash % 64));
			}
		}
		return out;
	}

	f64 Psnr(std::span<const u8> a, std::span<const u8> b, u32 channels) {
		f64 squared = 0.0;
		size_t count = 0;
		for (size_t i = 0; i < a.size(); i += 4) {
			for (u32 c = 0; c < channels; ++c) {
				const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
				squared += d * d;
				++count;
			}
		}
		const f64 mse = squared / static_cast<f64>(count);
		return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
	}

	const TextureCodec& CodecFor(RHI::Format format) {
		const TextureCodec* codec = TextureCodecs::Find(format);
		EXPECT_NE(codec, nullptr);
		static const TextureCodec none{};
		return codec ? *codec : none;
	}

	Vector<u8> Halves(Vec2I size, u32 slices, const std::function<Vec3(i32, i32, u32)>& radiance) {
		Vector<u8> out{GetMemoryResource(MemTag::Asset)};
		out.resize(static_cast<size_t>(size.x) * size.y * slices * 8);
		u16* texels = reinterpret_cast<u16*>(out.data());
		for (u32 slice = 0; slice < slices; ++slice) {
			for (i32 y = 0; y < size.y; ++y) {
				for (i32 x = 0; x < size.x; ++x) {
					const Vec3 value = radiance(x, y, slice);
					u16* texel = texels + ((static_cast<size_t>(slice) * size.y + y) * size.x + x) * 4;
					texel[0] = Math::FloatToHalf(value.x);
					texel[1] = Math::FloatToHalf(value.y);
					texel[2] = Math::FloatToHalf(value.z);
					texel[3] = Math::FloatToHalf(1.0f);
				}
			}
		}
		return out;
	}

	Vec3 BusyRadiance(i32 x, i32 y, u32) {
		const f32 u = static_cast<f32>(x) / 63.0f;
		const f32 v = static_cast<f32>(y) / 63.0f;
		const f32 checker = ((x / 4 + y / 4) % 2) ? 2.0f : 0.25f;
		const f32 dx = u - 0.7f;
		const f32 dy = v - 0.3f;
		const f32 sun = 500.0f * std::exp(-(dx * dx + dy * dy) * 400.0f);
		return Vec3(0.2f + u * checker + sun, 0.4f + v + sun * 0.8f, 1.5f * (1.0f - u) + sun * 0.5f);
	}

	Vec3 HalfTexel(std::span<const u8> texels, size_t index) {
		u16 half[3];
		std::memcpy(half, texels.data() + index * 8, sizeof(half));
		return Vec3(Math::HalfToFloat(half[0]), Math::HalfToFloat(half[1]), Math::HalfToFloat(half[2]));
	}

	f64 LogRmse(std::span<const u8> a, std::span<const u8> b) {
		f64 squared = 0.0;
		size_t count = 0;
		for (size_t texel = 0; texel < a.size() / 8; ++texel) {
			const Vec3 x = HalfTexel(a, texel);
			const Vec3 y = HalfTexel(b, texel);
			for (u32 c = 0; c < 3; ++c) {
				const f64 d = std::log2(1.0 + x[c]) - std::log2(1.0 + y[c]);
				squared += d * d;
				++count;
			}
		}
		return std::sqrt(squared / static_cast<f64>(count));
	}

	Vector<u8> DecodeLevel(RHI::Format format, std::span<const u8> blocks, Vec2I size) {
		Vector<u8> out{GetMemoryResource(MemTag::Asset)};
		out.resize(static_cast<size_t>(RHI::FormatLevelBytes(TextureCodecs::DecodedFormat(format), size)));
		if (!CodecFor(format).decode(format, blocks, size, out)) {
			out.clear();
		}
		return out;
	}

	void SetBits(u8 block[16], u32 position, u32 count, u32 value) {
		for (u32 i = 0; i < count; ++i, ++position) {
			if ((value >> i) & 1) {
				block[position >> 3] |= static_cast<u8>(1u << (position & 7));
			}
		}
	}

} // anonymous namespace

TEST(TextureCompressionTest, SRGBChainAveragesLinear) {
	Vector<u8> image{GetMemoryResource(MemTag::Asset)};
	image = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
	Vector<u8> chain{GetMemoryResource(MemTag::Asset)};
	TextureCompressionOps::BuildChain(image, {2, 2}, 2, TexelMeaning::sRGBColor, chain);
	ASSERT_EQ(chain.size(), 16u + 4u);
	EXPECT_NEAR(chain[16], 188, 1);
	EXPECT_EQ(chain[19], 255);
	TextureCompressionOps::BuildChain(image, {2, 2}, 2, TexelMeaning::Linear, chain);
	EXPECT_NEAR(chain[16], 128, 1);
}

TEST(TextureCompressionTest, NormalChainRenormalized) {
	Vector<u8> image{GetMemoryResource(MemTag::Asset)};
	image = {255, 128, 128, 255, 128, 128, 255, 255, 128, 128, 255, 255, 255, 128, 128, 255};
	Vector<u8> chain{GetMemoryResource(MemTag::Asset)};
	TextureCompressionOps::BuildChain(image, {2, 2}, 2, TexelMeaning::Normal, chain);
	ASSERT_EQ(chain.size(), 20u);
	EXPECT_NEAR(chain[16], 218, 1);
	EXPECT_NEAR(chain[17], 128, 1);
	EXPECT_NEAR(chain[18], 218, 1);
	TextureCompressionOps::BuildChain(image, {2, 2}, 2, TexelMeaning::Linear, chain);
	EXPECT_NEAR(chain[16], 192, 1);
}

TEST(TextureCompressionTest, ChainHalvesEachAxisDownToOne) {
	Vector<u8> chain{GetMemoryResource(MemTag::Asset)};
	Vector<u8> wide = Solid({8, 2}, 1, {10, 20, 30, 40});
	TextureCompressionOps::BuildChain(wide, {8, 2}, 4, TexelMeaning::Linear, chain);
	EXPECT_EQ(chain.size(), static_cast<size_t>((16 + 4 + 2 + 1) * 4));
	EXPECT_EQ(chain[chain.size() - 4], 10);
	EXPECT_EQ(chain[chain.size() - 1], 40);

	Vector<u8> odd{GetMemoryResource(MemTag::Asset)};
	for (i32 y = 0; y < 3; ++y) {
		for (i32 x = 0; x < 5; ++x) {
			odd.insert(odd.end(), {static_cast<u8>(50 * x), 0, 0, 255});
		}
	}
	TextureCompressionOps::BuildChain(odd, {5, 3}, 3, TexelMeaning::Linear, chain);
	ASSERT_EQ(chain.size(), static_cast<size_t>((15 + 2 + 1) * 4));
	EXPECT_NEAR(chain[60], 25, 1);
	EXPECT_NEAR(chain[64], 125, 1);
	EXPECT_NEAR(chain[68], 75, 1);
}

TEST(TextureCompressionTest, CodecPerTargetFormat) {
	EXPECT_NE(TextureCodecs::Find(RHI::Format::BC7SRGB), nullptr);
	EXPECT_NE(TextureCodecs::Find(RHI::Format::BC7Unorm), nullptr);
	EXPECT_NE(TextureCodecs::Find(RHI::Format::BC5Unorm), nullptr);
	EXPECT_NE(TextureCodecs::Find(RHI::Format::BC6HUFloat), nullptr);
	EXPECT_EQ(TextureCodecs::Find(RHI::Format::RGBA8SRGB), nullptr);
	EXPECT_EQ(TextureCodecs::Find(RHI::Format::RGBA16Float), nullptr);
}

TEST(TextureCompressionTest, BC7ChainWithinError) {
	const Vec2I size{64, 64};
	const u32 mips = 7;
	Vector<u8> image = Busy(size);
	TextureEncodeInput input;
	input.texels = image;
	input.size = size;
	input.mipLevels = mips;
	input.meaning = TexelMeaning::Linear;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC7Unorm).encode(RHI::Format::BC7Unorm, input, blocks));
	EXPECT_EQ(blocks.size(), static_cast<size_t>((256 + 64 + 16 + 4 + 1 + 1 + 1) * 16));

	Vector<u8> decoded{GetMemoryResource(MemTag::Asset)};
	decoded.resize(image.size());
	ASSERT_TRUE(CodecFor(RHI::Format::BC7Unorm).decode(RHI::Format::BC7Unorm, blocks, size, decoded));
	const f64 psnr = Psnr(image, decoded, 4);
	EXPECT_GT(psnr, 38.0);
	EXPECT_LT(psnr, 99.0);

	Vector<u8> chain{GetMemoryResource(MemTag::Asset)};
	TextureCompressionOps::BuildChain(image, size, mips, TexelMeaning::Linear, chain);
	u8 tail[4] = {};
	ASSERT_TRUE(CodecFor(RHI::Format::BC7Unorm).decode(RHI::Format::BC7Unorm, std::span<const u8>(blocks).last(16), {1, 1}, tail));
	for (u32 c = 0; c < 4; ++c) {
		EXPECT_NEAR(tail[c], chain[chain.size() - 4 + c], 2) << "channel " << c;
	}
}

TEST(TextureCompressionTest, BC5KeepsXY) {
	const Vec2I size{16, 16};
	Vector<u8> image = Solid(size, 1, {204, 89, 222, 255});
	TextureEncodeInput input;
	input.texels = image;
	input.size = size;
	input.mipLevels = 5;
	input.meaning = TexelMeaning::Normal;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC5Unorm).encode(RHI::Format::BC5Unorm, input, blocks));
	Vector<u8> decoded{GetMemoryResource(MemTag::Asset)};
	decoded.resize(image.size());
	ASSERT_TRUE(CodecFor(RHI::Format::BC5Unorm).decode(RHI::Format::BC5Unorm, blocks, size, decoded));
	for (size_t i = 0; i < decoded.size(); i += 4) {
		ASSERT_NEAR(decoded[i], 204, 1) << "texel " << i / 4;
		ASSERT_NEAR(decoded[i + 1], 89, 1) << "texel " << i / 4;
		ASSERT_EQ(decoded[i + 2], 0);
		ASSERT_EQ(decoded[i + 3], 255);
	}
}

TEST(TextureCompressionTest, ArraySlicesSideBySide) {
	const Vec2I size{8, 8};
	Vector<u8> image = Solid(size, 1, {200, 30, 30, 255});
	Vector<u8> second = Solid(size, 1, {20, 40, 220, 255});
	image.insert(image.end(), second.begin(), second.end());
	TextureEncodeInput input;
	input.texels = image;
	input.size = size;
	input.slices = 2;
	input.mipLevels = 4;
	input.meaning = TexelMeaning::sRGBColor;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC7SRGB).encode(RHI::Format::BC7SRGB, input, blocks));
	EXPECT_EQ(blocks.size(), static_cast<size_t>((4 + 1 + 1 + 1) * 2 * 16));

	const size_t offset = 4 * 2 * 16 + 1 * 16;
	u8 texels[4 * 4 * 4] = {};
	ASSERT_TRUE(CodecFor(RHI::Format::BC7SRGB).decode(RHI::Format::BC7SRGB, std::span<const u8>(blocks).subspan(offset, 16), {4, 4}, texels));
	EXPECT_NEAR(texels[0], 20, 2);
	EXPECT_NEAR(texels[2], 220, 2);
}

TEST(TextureCompressionTest, EncodingIgnoresWorkerCount) {
	const Vec2I size{256, 256};
	Vector<u8> image = Busy(size);
	Vector<u8> hdr = Halves(size, 1, [](i32 x, i32 y, u32 slice) {
		const i32 tile = x / 64;
		return BusyRadiance(x % 64, y % 64, slice) * static_cast<f32>(1 << tile);
	});
	struct Case {
		RHI::Format format;
		std::span<const u8> texels;
		RHI::Format texelFormat;
		TexelMeaning meaning;
	};
	const Case cases[] = {{RHI::Format::BC7SRGB, image, RHI::Format::RGBA8Unorm, TexelMeaning::sRGBColor},
						  {RHI::Format::BC6HUFloat, hdr, RHI::Format::RGBA16Float, TexelMeaning::Linear}};
	for (const Case& c : cases) {
		TextureEncodeInput input;
		input.texels = c.texels;
		input.texelFormat = c.texelFormat;
		input.size = size;
		input.mipLevels = 1;
		input.meaning = c.meaning;
		Vector<u8> serial{GetMemoryResource(MemTag::Asset)};
		ASSERT_EQ(JobSystem::GetWorkerCount(), 0u);
		ASSERT_TRUE(CodecFor(c.format).encode(c.format, input, serial));

		JobSystem::Initialize();
		const u32 workers = JobSystem::GetWorkerCount();
		Vector<u8> spread{GetMemoryResource(MemTag::Asset)};
		const bool encoded = CodecFor(c.format).encode(c.format, input, spread);
		JobSystem::Terminate();

		ASSERT_GT(workers, 0u);
		ASSERT_TRUE(encoded);
		EXPECT_EQ(spread.size(), static_cast<size_t>(64 * 64 * 16));
		EXPECT_TRUE(spread == serial) << "format " << static_cast<u32>(c.format);
	}
}

TEST(TextureCompressionTest, EncodeRefusesWrongSize) {
	Vector<u8> image = Solid({4, 4}, 1, {1, 2, 3, 4});
	TextureEncodeInput input;
	input.texels = std::span<const u8>(image).first(60);
	input.size = {4, 4};
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	EXPECT_FALSE(CodecFor(RHI::Format::BC7Unorm).encode(RHI::Format::BC7Unorm, input, blocks));
	input.texels = image;
	EXPECT_FALSE(CodecFor(RHI::Format::BC7Unorm).encode(RHI::Format::RGBA8Unorm, input, blocks));
	EXPECT_TRUE(CodecFor(RHI::Format::BC7Unorm).encode(RHI::Format::BC7Unorm, input, blocks));
}

TEST(TextureCompressionTest, BC6HKeepsLightAbove1) {
	const Vec2I size{64, 64};
	const u32 mips = 7;
	Vector<u8> image = Halves(size, 1, BusyRadiance);
	TextureEncodeInput input;
	input.texels = image;
	input.texelFormat = RHI::Format::RGBA16Float;
	input.size = size;
	input.mipLevels = mips;
	input.meaning = TexelMeaning::Linear;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	EXPECT_EQ(blocks.size(), static_cast<size_t>((256 + 64 + 16 + 4 + 1 + 1 + 1) * 16));

	Vector<u8> top = DecodeLevel(RHI::Format::BC6HUFloat, blocks, size);
	ASSERT_EQ(top.size(), image.size());
	const f64 error = LogRmse(image, top);
	EXPECT_LT(error, 0.03);
	EXPECT_GT(error, 0.0);
	f32 sourcePeak = 0.0f;
	f32 decodedPeak = 0.0f;
	Vec3 mean{};
	for (size_t texel = 0; texel < image.size() / 8; ++texel) {
		const Vec3 value = HalfTexel(image, texel);
		sourcePeak = std::max(sourcePeak, value.x);
		decodedPeak = std::max(decodedPeak, HalfTexel(top, texel).x);
		mean = mean + value;
	}
	mean = mean / static_cast<f32>(image.size() / 8);
	ASSERT_GT(sourcePeak, 300.0f);
	EXPECT_NEAR(decodedPeak, sourcePeak, sourcePeak * 0.1f);

	Vector<u8> tail = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(blocks).last(16), {1, 1});
	ASSERT_EQ(tail.size(), 8u);
	const Vec3 average = HalfTexel(tail, 0);
	for (u32 c = 0; c < 3; ++c) {
		EXPECT_NEAR(average[c], mean[c], mean[c] * 0.02f) << "channel " << c;
	}
	ASSERT_GT(mean.x, 1.0f);
}

TEST(TextureCompressionTest, SuppliedChainEncodedAsIs) {
	const Vec2I size{8, 8};
	const Vec3 levels[4] = {Vec3(1.0f), Vec3(8.0f, 4.0f, 2.0f), Vec3(0.5f), Vec3(3.0f)};
	Vector<u8> chain{GetMemoryResource(MemTag::Asset)};
	for (u32 mip = 0; mip < 4; ++mip) {
		const Vector<u8> level = Halves({8 >> mip, 8 >> mip}, 1, [&](i32, i32, u32) { return levels[mip]; });
		chain.insert(chain.end(), level.begin(), level.end());
	}
	TextureEncodeInput input;
	input.texels = chain;
	input.texelFormat = RHI::Format::RGBA16Float;
	input.size = size;
	input.mipLevels = 4;
	input.suppliedLevels = 4;
	input.meaning = TexelMeaning::Linear;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	ASSERT_EQ(blocks.size(), static_cast<size_t>((4 + 1 + 1 + 1) * 16));

	const Vector<u8> second = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(blocks).subspan(4 * 16, 16), {4, 4});
	ASSERT_FALSE(second.empty());
	for (u32 c = 0; c < 3; ++c) {
		EXPECT_NEAR(HalfTexel(second, 5)[c], levels[1][c], levels[1][c] * 0.01f) << "channel " << c;
	}
	const Vector<u8> last = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(blocks).last(16), {1, 1});
	ASSERT_FALSE(last.empty());
	EXPECT_NEAR(HalfTexel(last, 0).x, 3.0f, 0.03f);

	input.texels = std::span<const u8>(chain).first(static_cast<size_t>(8 * 8 * 8));
	input.suppliedLevels = 1;
	ASSERT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	const Vector<u8> rebuilt = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(blocks).subspan(4 * 16, 16), {4, 4});
	ASSERT_FALSE(rebuilt.empty());
	EXPECT_NEAR(HalfTexel(rebuilt, 5).x, 1.0f, 0.01f);
}

TEST(TextureCompressionTest, HdrMismatchRefused) {
	const Vector<u8> hdr = Halves({4, 4}, 1, [](i32, i32, u32) { return Vec3(1.0f); });
	const Vector<u8> ldr = Solid({4, 4}, 1, {1, 2, 3, 4});
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	TextureEncodeInput input;
	input.size = {4, 4};
	input.meaning = TexelMeaning::Linear;

	input.texels = hdr;
	input.texelFormat = RHI::Format::RGBA16Float;
	EXPECT_FALSE(CodecFor(RHI::Format::BC7Unorm).encode(RHI::Format::BC7Unorm, input, blocks));
	input.texels = ldr;
	input.texelFormat = RHI::Format::RGBA8Unorm;
	EXPECT_FALSE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));

	input.texels = hdr;
	input.texelFormat = RHI::Format::RGBA16Float;
	input.meaning = TexelMeaning::sRGBColor;
	EXPECT_FALSE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	input.meaning = TexelMeaning::Linear;

	Vector<u8> partial = hdr;
	const Vector<u8> half = Halves({2, 2}, 1, [](i32, i32, u32) { return Vec3(1.0f); });
	partial.insert(partial.end(), half.begin(), half.end());
	input.texels = partial;
	input.mipLevels = 3;
	input.suppliedLevels = 2;
	EXPECT_FALSE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));

	input.texels = hdr;
	input.mipLevels = 1;
	input.suppliedLevels = 1;
	EXPECT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
}

TEST(TextureCompressionTest, BC6HClampsUnrepresentable) {
	const u16 infinity = 0x7C00;
	const u16 negativeTwo = 0xC000;
	const u16 notANumber = 0x7E00;
	Vector<u8> image = Halves({12, 4}, 1, [](i32, i32, u32) { return Vec3(1.0f); });
	u16* texels = reinterpret_cast<u16*>(image.data());
	for (i32 y = 0; y < 4; ++y) {
		for (i32 x = 0; x < 12; ++x) {
			const u16 value = x < 4 ? infinity : (x < 8 ? negativeTwo : notANumber);
			for (u32 c = 0; c < 3; ++c) {
				texels[(static_cast<size_t>(y) * 12 + x) * 4 + c] = value;
			}
		}
	}
	TextureEncodeInput input;
	input.texels = image;
	input.texelFormat = RHI::Format::RGBA16Float;
	input.size = {12, 4};
	input.meaning = TexelMeaning::Linear;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	const Vector<u8> decoded = DecodeLevel(RHI::Format::BC6HUFloat, blocks, {12, 4});
	ASSERT_EQ(decoded.size(), static_cast<size_t>(12 * 4 * 8));
	for (u32 c = 0; c < 3; ++c) {
		EXPECT_NEAR(HalfTexel(decoded, 0)[c], 65504.0f, 65504.0f * 0.01f) << "infinity, channel " << c;
		EXPECT_LT(HalfTexel(decoded, 4)[c], 1e-3f) << c;
		EXPECT_LT(HalfTexel(decoded, 8)[c], 1e-3f) << c;
		EXPECT_FALSE(std::isnan(HalfTexel(decoded, 8)[c]));
	}
}

TEST(TextureCompressionTest, BC6HDecoderMatchesSpec) {
	u8 block[16] = {};
	SetBits(block, 0, 5, 0x03);
	SetBits(block, 35, 10, 1023);
	SetBits(block, 45, 10, 1023);
	SetBits(block, 55, 10, 1023);
	SetBits(block, 64 + 4 * 15, 4, 15);
	SetBits(block, 64 + 4 * 5, 4, 8);
	Vector<u8> texels = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(block, 16), {4, 4});
	ASSERT_EQ(texels.size(), static_cast<size_t>(16 * 8));
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	for (u32 c = 0; c < 3; ++c) {
		EXPECT_EQ(halves[0 * 4 + c], 0u) << "channel " << c;
		EXPECT_EQ(halves[5 * 4 + c], 0x41DFu) << "channel " << c;
		EXPECT_EQ(halves[15 * 4 + c], 0x7BFFu) << "channel " << c;
	}
	EXPECT_EQ(halves[3], 0x3C00u);

	u8 reserved[16];
	std::memset(reserved, 0xFF, sizeof(reserved));
	texels = DecodeLevel(RHI::Format::BC6HUFloat, std::span<const u8>(reserved, 16), {4, 4});
	ASSERT_EQ(texels.size(), static_cast<size_t>(16 * 8));
	halves = reinterpret_cast<const u16*>(texels.data());
	for (u32 texel = 0; texel < 16; ++texel) {
		EXPECT_EQ(halves[texel * 4 + 0] | halves[texel * 4 + 1] | halves[texel * 4 + 2], 0u) << "texel " << texel;
		EXPECT_EQ(halves[texel * 4 + 3], 0x3C00u);
	}
}

namespace {

	class BC6HSampleFixture : public ::testing::Test {
	protected:
		RHI::Device* _device = nullptr;

		void SetUp() override {
			RHI::DeviceDesc desc;
			desc.headless = true;
			_device = RHI::Device::Create(desc);
			if (!_device) {
				GTEST_SKIP() << "no Vulkan ICD available";
			}
		}

		void TearDown() override {
			if (_device) {
				_device->WaitIdle();
				RHI::Device::Destroy(_device);
			}
		}
	};

} // anonymous namespace

TEST_F(BC6HSampleFixture, CodecDecodeMatchesGpu) {
	constexpr i32 WIDTH = 64;
	constexpr i32 ENCODED_HEIGHT = 64;
	constexpr u32 RANDOM_BLOCKS = 1024;
	constexpr i32 HEIGHT = ENCODED_HEIGHT + static_cast<i32>(RANDOM_BLOCKS / (WIDTH / 4)) * 4;
	const std::filesystem::path source = CookHarness::FindRepoDir("EngineAssets/Shaders/Blit.hlsl");
	ASSERT_FALSE(source.empty());
	ShaderAsset blit;
	if (!blit.Import(Path(source.generic_string()))) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}

	const Vector<u8> image = Halves({WIDTH, ENCODED_HEIGHT}, 1, [](i32 x, i32 y, u32 slice) {
		return BusyRadiance(x, y, slice) * static_cast<f32>(1 << ((x / 32) + 2 * (y / 32)));
	});
	TextureEncodeInput input;
	input.texels = image;
	input.texelFormat = RHI::Format::RGBA16Float;
	input.size = {WIDTH, ENCODED_HEIGHT};
	input.meaning = TexelMeaning::Linear;
	Vector<u8> blocks{GetMemoryResource(MemTag::Asset)};
	ASSERT_TRUE(CodecFor(RHI::Format::BC6HUFloat).encode(RHI::Format::BC6HUFloat, input, blocks));
	u64 state = 0x9E3779B97F4A7C15ull;
	for (u32 block = 0; block < RANDOM_BLOCKS; ++block) {
		u8 bits[16];
		for (u8& byte : bits) {
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			byte = static_cast<u8>(state >> 56);
		}
		bits[0] = static_cast<u8>((bits[0] & ~0x1Fu) | (block % 32));
		blocks.insert(blocks.end(), bits, bits + 16);
	}
	const Vector<u8> decoded = DecodeLevel(RHI::Format::BC6HUFloat, blocks, {WIDTH, HEIGHT});
	ASSERT_EQ(decoded.size(), static_cast<size_t>(WIDTH * HEIGHT * 8));

	RHI::TextureDesc textureDesc;
	textureDesc.size = {WIDTH, HEIGHT};
	textureDesc.format = RHI::Format::BC6HUFloat;
	textureDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
	textureDesc.debugName = "BC6HSource";
	RHI::TextureHandle texture = _device->CreateTextureResource(textureDesc);
	ASSERT_TRUE(texture.IsValid());
	_device->UploadTexture(texture, {blocks.data(), blocks.size()});
	RHI::TextureDesc targetDesc;
	targetDesc.size = {WIDTH, HEIGHT};
	targetDesc.format = RHI::Format::RGBA16Float;
	targetDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
	targetDesc.debugName = "BC6HSampled";
	RHI::TextureHandle target = _device->CreateTextureResource(targetDesc);
	RHI::SamplerDesc samplerDesc;
	samplerDesc.filter = RHI::FilterMode::Nearest;
	samplerDesc.mipFilter = RHI::FilterMode::Nearest;
	samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
	RHI::SamplerHandle sampler = _device->CreateSamplerResource(samplerDesc);
	const ShaderVariant& code = blit.variants[SHADER_BASE_VARIANT];
	RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexSpirv = {code.vertexSpirv.data(), code.vertexSpirv.size()};
	pipelineDesc.pixelSpirv = {code.pixelSpirv.data(), code.pixelSpirv.size()};
	pipelineDesc.bytecodeFormat = blit.bytecodeFormat;
	pipelineDesc.bindings = {blit.bindings.data(), blit.bindings.size()};
	pipelineDesc.pushConstantSize = blit.pushConstantSize;
	pipelineDesc.blend = blit.renderState.blend;
	pipelineDesc.depth = blit.renderState.depth;
	pipelineDesc.cull = blit.renderState.cull;
	pipelineDesc.colorFormats[0] = RHI::Format::RGBA16Float;
	pipelineDesc.colorFormatCount = 1;
	pipelineDesc.debugName = "BC6HSample";
	RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(pipelineDesc);
	ASSERT_TRUE(target.IsValid());
	ASSERT_TRUE(sampler.IsValid());
	ASSERT_TRUE(pipeline.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(target, RHI::ResourceState::RenderTarget);
	RHI::RenderPassDesc::Attachment attachment;
	attachment.texture = target;
	attachment.load = RHI::LoadOp::DontCare;
	RHI::RenderPassDesc pass;
	pass.colorAttachments = {&attachment, 1};
	cmd->BeginRenderPass(pass);
	cmd->SetPipeline(pipeline);
	RHI::TransientSet set = _device->CreateTransientSet(pipeline, 1).WriteTexture(0, texture).WriteSampler(1, sampler).Commit();
	cmd->BindTransientSet(1, set);
	cmd->Draw(3);
	cmd->EndRenderPass();
	_device->EndFrame();
	_device->WaitIdle();
	Vector<u8> sampled{GetMemoryResource(MemTag::Graphics)};
	sampled.assign(static_cast<size_t>(WIDTH * HEIGHT * 8), 0xAB);
	_device->ReadbackTexture(target, {sampled.data(), sampled.size()});

	u32 differingEncoded = 0;
	u32 differingRandom = 0;
	f32 brightest = 0.0f;
	for (size_t texel = 0; texel < static_cast<size_t>(WIDTH * HEIGHT); ++texel) {
		const bool encoded = texel < static_cast<size_t>(WIDTH * ENCODED_HEIGHT);
		for (u32 c = 0; c < 3; ++c) {
			u16 gpu = 0;
			u16 cpu = 0;
			std::memcpy(&gpu, sampled.data() + texel * 8 + c * 2, 2);
			std::memcpy(&cpu, decoded.data() + texel * 8 + c * 2, 2);
			(encoded ? differingEncoded : differingRandom) += gpu != cpu ? 1u : 0u;
		}
		if (encoded) {
			brightest = std::max(brightest, HalfTexel(sampled, texel).x);
		}
	}
	EXPECT_EQ(differingEncoded, 0u) << "of " << WIDTH * ENCODED_HEIGHT * 3;
	EXPECT_EQ(differingRandom, 0u) << "of " << RANDOM_BLOCKS * 16 * 3;
	EXPECT_GT(brightest, 100.0f);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(sampler);
	_device->DestroyResource(target);
	_device->DestroyResource(texture);
	for (int i = 0; i < 2; ++i) {
		_device->BeginFrame();
		_device->EndFrame();
	}
}
