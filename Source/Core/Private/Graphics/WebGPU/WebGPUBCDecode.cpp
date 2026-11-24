#include "Graphics/WebGPU/WebGPUBCDecode.h"

#include "Shalltear/Core/Graphics/TextureCodecs.h"

#include <algorithm>
#include <cstring>

namespace Shalltear::RHI::WebGPU {

	namespace {

		struct BitReader {
			const u8* data = nullptr;
			u32 pos = 0;

			u32 Read(u32 count) {
				u32 value = 0;
				for (u32 i = 0; i < count; ++i, ++pos) {
					value |= static_cast<u32>((data[pos >> 3] >> (pos & 7)) & 1u) << i;
				}
				return value;
			}
		};

		void DecodeBC4Channel(const u8 block[8], u8* outRGBA, u32 channelOffset) {
			const u32 a0 = block[0];
			const u32 a1 = block[1];
			u8 palette[8];
			palette[0] = static_cast<u8>(a0);
			palette[1] = static_cast<u8>(a1);
			if (a0 > a1) {
				palette[2] = static_cast<u8>((6u * a0 + 1u * a1 + 3u) / 7u);
				palette[3] = static_cast<u8>((5u * a0 + 2u * a1 + 3u) / 7u);
				palette[4] = static_cast<u8>((4u * a0 + 3u * a1 + 3u) / 7u);
				palette[5] = static_cast<u8>((3u * a0 + 4u * a1 + 3u) / 7u);
				palette[6] = static_cast<u8>((2u * a0 + 5u * a1 + 3u) / 7u);
				palette[7] = static_cast<u8>((1u * a0 + 6u * a1 + 3u) / 7u);
			} else {
				palette[2] = static_cast<u8>((4u * a0 + 1u * a1 + 2u) / 5u);
				palette[3] = static_cast<u8>((3u * a0 + 2u * a1 + 2u) / 5u);
				palette[4] = static_cast<u8>((2u * a0 + 3u * a1 + 2u) / 5u);
				palette[5] = static_cast<u8>((1u * a0 + 4u * a1 + 2u) / 5u);
				palette[6] = 0;
				palette[7] = 255;
			}
			u64 indices = 0;
			for (u32 i = 0; i < 6; ++i) {
				indices |= static_cast<u64>(block[2 + i]) << (i * 8u);
			}
			for (u32 t = 0; t < 16; ++t) {
				outRGBA[t * 4 + channelOffset] = palette[(indices >> (t * 3u)) & 7u];
			}
		}

		void DecodeBC5Block(const u8 block[16], u8 outRGBA[64]) {
			for (u32 t = 0; t < 16; ++t) {
				outRGBA[t * 4 + 2] = 0;
				outRGBA[t * 4 + 3] = 255;
			}
			DecodeBC4Channel(block, outRGBA, 0);
			DecodeBC4Channel(block + 8, outRGBA, 1);
		}

		constexpr u8 BC6H_PARTITION_TARGET = 12;

		struct BC6HField {
			u8 target;
			u8 to;
			u8 from;
		};

		struct BC6HMode {
			u8 number;
			bool transformed;
			bool twoSubsets;
			u8 endpointBits;
			u8 deltaBits[3];
			u8 fieldCount;
			BC6HField fields[24];
		};

		constexpr BC6HMode BC6H_MODES[] = {
			{0, true, true, 10, {5, 5, 5}, 20, {{7, 4, 4}, {8, 4, 4}, {11, 4, 4}, {0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}, {12, 4, 0}}},
			{1, true, true, 7, {6, 6, 6}, 24, {{7, 5, 5}, {10, 4, 4}, {10, 5, 5}, {0, 6, 0}, {11, 0, 0}, {11, 1, 1}, {8, 4, 4}, {1, 6, 0}, {8, 5, 5}, {11, 2, 2}, {7, 4, 4}, {2, 6, 0}, {11, 3, 3}, {11, 5, 5}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}, {12, 4, 0}}},
			{2, true, true, 11, {5, 4, 4}, 19, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 4, 0}, {0, 10, 10}, {7, 3, 0}, {4, 3, 0}, {1, 10, 10}, {11, 0, 0}, {10, 3, 0}, {5, 3, 0}, {2, 10, 10}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}, {12, 4, 0}}},
			{6, true, true, 11, {4, 5, 4}, 21, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 10}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {1, 10, 10}, {10, 3, 0}, {5, 3, 0}, {2, 10, 10}, {11, 1, 1}, {8, 3, 0}, {6, 3, 0}, {11, 0, 0}, {11, 2, 2}, {9, 3, 0}, {7, 4, 4}, {11, 3, 3}, {12, 4, 0}}},
			{10, true, true, 11, {4, 4, 5}, 21, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 10}, {8, 4, 4}, {7, 3, 0}, {4, 3, 0}, {1, 10, 10}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {2, 10, 10}, {8, 3, 0}, {6, 3, 0}, {11, 1, 1}, {11, 2, 2}, {9, 3, 0}, {11, 4, 4}, {11, 3, 3}, {12, 4, 0}}},
			{14, true, true, 9, {5, 5, 5}, 20, {{0, 8, 0}, {8, 4, 4}, {1, 8, 0}, {7, 4, 4}, {2, 8, 0}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}, {12, 4, 0}}},
			{18, true, true, 8, {6, 5, 5}, 20, {{0, 7, 0}, {10, 4, 4}, {8, 4, 4}, {1, 7, 0}, {11, 2, 2}, {7, 4, 4}, {2, 7, 0}, {11, 3, 3}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}, {12, 4, 0}}},
			{22, true, true, 8, {5, 6, 5}, 22, {{0, 7, 0}, {11, 0, 0}, {8, 4, 4}, {1, 7, 0}, {7, 5, 5}, {7, 4, 4}, {2, 7, 0}, {10, 5, 5}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 4, 0}, {11, 1, 1}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}, {12, 4, 0}}},
			{26, true, true, 8, {5, 5, 6}, 22, {{0, 7, 0}, {11, 1, 1}, {8, 4, 4}, {1, 7, 0}, {8, 5, 5}, {7, 4, 4}, {2, 7, 0}, {11, 5, 5}, {11, 4, 4}, {3, 4, 0}, {10, 4, 4}, {7, 3, 0}, {4, 4, 0}, {11, 0, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 4, 0}, {11, 2, 2}, {9, 4, 0}, {11, 3, 3}, {12, 4, 0}}},
			{30, false, true, 6, {0, 0, 0}, 24, {{0, 5, 0}, {10, 4, 4}, {11, 0, 0}, {11, 1, 1}, {8, 4, 4}, {1, 5, 0}, {7, 5, 5}, {8, 5, 5}, {11, 2, 2}, {7, 4, 4}, {2, 5, 0}, {10, 5, 5}, {11, 3, 3}, {11, 5, 5}, {11, 4, 4}, {3, 5, 0}, {7, 3, 0}, {4, 5, 0}, {10, 3, 0}, {5, 5, 0}, {8, 3, 0}, {6, 5, 0}, {9, 5, 0}, {12, 4, 0}}},
			{3, false, false, 10, {0, 0, 0}, 6, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 9, 0}, {4, 9, 0}, {5, 9, 0}}},
			{7, true, false, 11, {9, 9, 9}, 9, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 8, 0}, {0, 10, 10}, {4, 8, 0}, {1, 10, 10}, {5, 8, 0}, {2, 10, 10}}},
			{11, true, false, 12, {8, 8, 8}, 9, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 7, 0}, {0, 10, 11}, {4, 7, 0}, {1, 10, 11}, {5, 7, 0}, {2, 10, 11}}},
			{15, true, false, 16, {4, 4, 4}, 9, {{0, 9, 0}, {1, 9, 0}, {2, 9, 0}, {3, 3, 0}, {0, 10, 15}, {4, 3, 0}, {1, 10, 15}, {5, 3, 0}, {2, 10, 15}}},
		};

		constexpr u16 PARTITIONS2[64] = {
			0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
			0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
			0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
			0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
			0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
			0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
			0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
			0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
		};

		constexpr u32 PARTITIONS3[64] = {
			0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
			0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
			0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
			0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
			0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
			0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
			0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
			0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u,
		};

		constexpr u8 ANCHORS2[64] = {
			15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
			15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
			15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
			6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
		};

		constexpr u8 ANCHORS3_1[64] = {
			3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
			3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
			8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
			3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
		};

		constexpr u8 ANCHORS3_2[64] = {
			15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
			15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
			15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
			15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
		};

		constexpr u8 WEIGHTS2[4] = {0, 21, 43, 64};
		constexpr u8 WEIGHTS3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
		constexpr u8 WEIGHTS4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
		constexpr u16 HALF_ONE = 0x3C00;

		i32 SignExtend(i32 value, u32 bits) {
			const u32 shift = 32 - bits;
			return static_cast<i32>(static_cast<u32>(value) << shift) >> shift;
		}

		i32 UnquantizeBC6H(i32 value, u32 bits) {
			if (bits >= 15 || value == 0) {
				return value;
			}
			if (value == (1 << bits) - 1) {
				return 0xFFFF;
			}
			return ((value << 16) + 0x8000) >> bits;
		}

		void DecodeBC6HBlock(const u8 block[16], u16 rgba[64]) {
			BitReader bits{block};
			u32 number = bits.Read(2);
			if (number >= 2) {
				number |= bits.Read(3) << 2;
			}
			const BC6HMode* mode = nullptr;
			for (const BC6HMode& candidate : BC6H_MODES) {
				if (candidate.number == number) {
					mode = &candidate;
					break;
				}
			}
			if (!mode) {
				for (u32 t = 0; t < 16; ++t) {
					rgba[t * 4 + 0] = 0;
					rgba[t * 4 + 1] = 0;
					rgba[t * 4 + 2] = 0;
					rgba[t * 4 + 3] = HALF_ONE;
				}
				return;
			}

			i32 values[BC6H_PARTITION_TARGET + 1] = {};
			for (u32 f = 0; f < mode->fieldCount; ++f) {
				const BC6HField& field = mode->fields[f];
				const i32 step = field.to >= field.from ? 1 : -1;
				const u32 count = static_cast<u32>((static_cast<i32>(field.to) - static_cast<i32>(field.from)) * step) + 1;
				i32 bit = field.from;
				for (u32 i = 0; i < count; ++i, bit += step) {
					values[field.target] |= static_cast<i32>(bits.Read(1)) << bit;
				}
			}

			const u32 endpoints = mode->twoSubsets ? 4u : 2u;
			const i32 mask = (1 << mode->endpointBits) - 1;
			i32 unquantized[4][3] = {};
			for (u32 channel = 0; channel < 3; ++channel) {
				for (u32 e = 0; e < endpoints; ++e) {
					i32 value = values[e * 3 + channel];
					if (mode->transformed && e > 0) {
						value = (values[channel] + SignExtend(value, mode->deltaBits[channel])) & mask;
					}
					unquantized[e][channel] = UnquantizeBC6H(value, mode->endpointBits);
				}
			}

			const u32 partition = static_cast<u32>(values[BC6H_PARTITION_TARGET]);
			bits.pos = mode->twoSubsets ? 82u : 65u;
			const u32 indexBits = mode->twoSubsets ? 3u : 4u;
			const u8* weights = mode->twoSubsets ? WEIGHTS3 : WEIGHTS4;
			for (u32 t = 0; t < 16; ++t) {
				const bool anchor = t == 0 || (mode->twoSubsets && t == ANCHORS2[partition]);
				const i32 weight = weights[bits.Read(indexBits - (anchor ? 1u : 0u))];
				const u32 subset = mode->twoSubsets ? ((PARTITIONS2[partition] >> t) & 1u) : 0u;
				for (u32 channel = 0; channel < 3; ++channel) {
					const i32 a = unquantized[subset * 2][channel];
					const i32 b = unquantized[subset * 2 + 1][channel];
					const i32 interpolated = (a * (64 - weight) + b * weight + 32) >> 6;
					rgba[t * 4 + channel] = static_cast<u16>((interpolated * 31) >> 6);
				}
				rgba[t * 4 + 3] = HALF_ONE;
			}
		}

		struct BC7ModeInfo {
			u8 subsets;
			u8 partitionBits;
			u8 rotationBits;
			u8 indexSelectionBits;
			u8 colorBits;
			u8 alphaBits;
			u8 endpointPBits;
			u8 sharedPBits;
			u8 indexBits;
			u8 indexBits2;
		};

		constexpr BC7ModeInfo BC7_MODES[8] = {
			{3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
			{2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
			{3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
			{2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
			{1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
			{1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
			{1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
			{2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
		};

		u8 ExpandBits(u32 v, u32 bits) {
			return static_cast<u8>((v << (8u - bits)) | (v >> (2u * bits - 8u)));
		}

		const u8* WeightsFor(u32 bits) {
			return bits == 2 ? WEIGHTS2 : (bits == 3 ? WEIGHTS3 : WEIGHTS4);
		}

		void DecodeBC7Block(const u8 block[16], u8 outRGBA[64]) {
			u32 mode = 0;
			while (mode < 8 && ((block[0] >> mode) & 1u) == 0) {
				++mode;
			}
			if (mode >= 8) {
				std::memset(outRGBA, 0, 64);
				return;
			}
			const BC7ModeInfo& info = BC7_MODES[mode];
			BitReader bits{block, mode + 1u};
			const u32 partition = info.partitionBits ? bits.Read(info.partitionBits) : 0u;
			const u32 rotation = info.rotationBits ? bits.Read(info.rotationBits) : 0u;
			const u32 indexSelect = info.indexSelectionBits ? bits.Read(info.indexSelectionBits) : 0u;

			const u32 endpointCount = info.subsets * 2u;
			u8 ep[6][4] = {};
			for (u32 c = 0; c < 3; ++c) {
				for (u32 e = 0; e < endpointCount; ++e) {
					ep[e][c] = static_cast<u8>(bits.Read(info.colorBits));
				}
			}
			if (info.alphaBits > 0) {
				for (u32 e = 0; e < endpointCount; ++e) {
					ep[e][3] = static_cast<u8>(bits.Read(info.alphaBits));
				}
			} else {
				for (u32 e = 0; e < endpointCount; ++e) {
					ep[e][3] = 255;
				}
			}

			u32 colorPrecision = info.colorBits;
			u32 alphaPrecision = info.alphaBits;
			if (info.endpointPBits > 0) {
				for (u32 e = 0; e < endpointCount; ++e) {
					const u32 p = bits.Read(1);
					for (u32 c = 0; c < 3; ++c) {
						ep[e][c] = static_cast<u8>((ep[e][c] << 1u) | p);
					}
					if (info.alphaBits > 0) {
						ep[e][3] = static_cast<u8>((ep[e][3] << 1u) | p);
					}
				}
				++colorPrecision;
				if (info.alphaBits > 0) {
					++alphaPrecision;
				}
			} else if (info.sharedPBits > 0) {
				for (u32 s = 0; s < info.subsets; ++s) {
					const u32 p = bits.Read(1);
					for (u32 k = 0; k < 2; ++k) {
						for (u32 c = 0; c < 3; ++c) {
							ep[s * 2 + k][c] = static_cast<u8>((ep[s * 2 + k][c] << 1u) | p);
						}
					}
				}
				++colorPrecision;
			}

			for (u32 e = 0; e < endpointCount; ++e) {
				for (u32 c = 0; c < 3; ++c) {
					ep[e][c] = ExpandBits(ep[e][c], colorPrecision);
				}
				if (info.alphaBits > 0) {
					ep[e][3] = ExpandBits(ep[e][3], alphaPrecision);
				}
			}

			u8 idx1[16] = {};
			u8 idx2[16] = {};
			for (u32 t = 0; t < 16; ++t) {
				bool anchor = (t == 0);
				if (info.subsets == 2 && t == ANCHORS2[partition]) {
					anchor = true;
				} else if (info.subsets == 3 && (t == ANCHORS3_1[partition] || t == ANCHORS3_2[partition])) {
					anchor = true;
				}
				idx1[t] = static_cast<u8>(bits.Read(info.indexBits - (anchor ? 1u : 0u)));
			}
			if (info.indexBits2 > 0) {
				for (u32 t = 0; t < 16; ++t) {
					idx2[t] = static_cast<u8>(bits.Read(info.indexBits2 - (t == 0 ? 1u : 0u)));
				}
			}

			const u8* w1 = WeightsFor(info.indexBits);
			const u8* w2 = info.indexBits2 > 0 ? WeightsFor(info.indexBits2) : w1;
			for (u32 t = 0; t < 16; ++t) {
				u32 subset = 0;
				if (info.subsets == 2) {
					subset = (PARTITIONS2[partition] >> t) & 1u;
				} else if (info.subsets == 3) {
					subset = (PARTITIONS3[partition] >> (t * 2u)) & 3u;
				}
				const u8 colorWeight = (info.indexBits2 > 0 && indexSelect != 0) ? w2[idx2[t]] : w1[idx1[t]];
				const u8 alphaWeight = info.indexBits2 > 0 ? (indexSelect != 0 ? w1[idx1[t]] : w2[idx2[t]]) : colorWeight;

				u8 rgba[4];
				for (u32 c = 0; c < 3; ++c) {
					rgba[c] = static_cast<u8>(((64u - colorWeight) * ep[subset * 2][c] + colorWeight * ep[subset * 2 + 1][c] + 32u) >> 6u);
				}
				rgba[3] = info.alphaBits > 0
							  ? static_cast<u8>(((64u - alphaWeight) * ep[subset * 2][3] + alphaWeight * ep[subset * 2 + 1][3] + 32u) >> 6u)
							  : 255u;

				if (rotation == 1) {
					std::swap(rgba[0], rgba[3]);
				} else if (rotation == 2) {
					std::swap(rgba[1], rgba[3]);
				} else if (rotation == 3) {
					std::swap(rgba[2], rgba[3]);
				}
				std::memcpy(outRGBA + t * 4, rgba, 4);
			}
		}

	} // anonymous namespace

	bool DecodeBlockCompressedLevel(Format format, std::span<const u8> blocks, Vec2I size, std::span<u8> outTexels) {
		if (const TextureCodec* registered = TextureCodecs::Find(format); registered && registered->decode) {
			return registered->decode(format, blocks, size, outTexels);
		}
		const Format decoded = TextureCodecs::DecodedFormat(format);
		if (decoded == Format::Unknown || size.x <= 0 || size.y <= 0 ||
			blocks.size() < FormatLevelBytes(format, size) || outTexels.size() < FormatLevelBytes(decoded, size)) {
			return false;
		}
		const size_t texelBytes = decoded == Format::RGBA16Float ? 8u : 4u;
		const u32 blocksX = (static_cast<u32>(size.x) + 3u) / 4u;
		const u32 blocksY = (static_cast<u32>(size.y) + 3u) / 4u;
		u16 decodedHalf[16 * 4];
		u8* blockBytes = reinterpret_cast<u8*>(decodedHalf);

		for (u32 by = 0; by < blocksY; ++by) {
			for (u32 bx = 0; bx < blocksX; ++bx) {
				const u8* block = blocks.data() + (static_cast<size_t>(by) * blocksX + bx) * 16u;
				if (format == Format::BC6HUFloat) {
					DecodeBC6HBlock(block, decodedHalf);
				} else if (format == Format::BC5Unorm) {
					DecodeBC5Block(block, blockBytes);
				} else {
					DecodeBC7Block(block, blockBytes);
				}
				for (u32 y = 0; y < 4 && by * 4 + y < static_cast<u32>(size.y); ++y) {
					const u32 width = std::min(4u, static_cast<u32>(size.x) - bx * 4u);
					std::memcpy(outTexels.data() + ((static_cast<size_t>(by) * 4 + y) * size.x + bx * 4) * texelBytes,
								blockBytes + y * 4 * texelBytes, width * texelBytes);
				}
			}
		}
		return true;
	}

} // namespace Shalltear::RHI::WebGPU
