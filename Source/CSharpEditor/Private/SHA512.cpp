#include "SHA512.h"

#include "SystemTools.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Shalltear::CSharpEditor {

	namespace {

		constexpr u64 ROUND_CONSTANTS[80] = {
			0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
			0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
			0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
			0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
			0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
			0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
			0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
			0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
			0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
			0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
			0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
			0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
			0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
			0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
			0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
			0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

		u64 ReadBigEndian(const u8* bytes) {
			u64 value = 0;
			for (u32 index = 0; index < 8; ++index) {
				value = (value << 8) | bytes[index];
			}
			return value;
		}

	} // anonymous namespace

	SHA512::SHA512()
		: _state{0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
				 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull} {}

	void SHA512::Compress(const u8* block) {
		u64 words[80];
		for (u32 index = 0; index < 16; ++index) {
			words[index] = ReadBigEndian(block + index * 8);
		}
		for (u32 index = 16; index < 80; ++index) {
			const u64 low = std::rotr(words[index - 15], 1) ^ std::rotr(words[index - 15], 8) ^ (words[index - 15] >> 7);
			const u64 high = std::rotr(words[index - 2], 19) ^ std::rotr(words[index - 2], 61) ^ (words[index - 2] >> 6);
			words[index] = words[index - 16] + low + words[index - 7] + high;
		}

		std::array<u64, 8> v = _state;
		for (u32 round = 0; round < 80; ++round) {
			const u64 sum1 = std::rotr(v[4], 14) ^ std::rotr(v[4], 18) ^ std::rotr(v[4], 41);
			const u64 choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
			const u64 first = v[7] + sum1 + choice + ROUND_CONSTANTS[round] + words[round];
			const u64 sum0 = std::rotr(v[0], 28) ^ std::rotr(v[0], 34) ^ std::rotr(v[0], 39);
			const u64 majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
			const u64 second = sum0 + majority;
			v[7] = v[6];
			v[6] = v[5];
			v[5] = v[4];
			v[4] = v[3] + first;
			v[3] = v[2];
			v[2] = v[1];
			v[1] = v[0];
			v[0] = first + second;
		}
		for (u32 index = 0; index < 8; ++index) {
			_state[index] += v[index];
		}
	}

	void SHA512::Update(std::span<const u8> bytes) {
		_length += bytes.size();
		size_t at = 0;
		if (_buffered > 0) {
			const size_t take = std::min(bytes.size(), _block.size() - _buffered);
			std::memcpy(_block.data() + _buffered, bytes.data(), take);
			_buffered += take;
			at = take;
			if (_buffered < _block.size()) {
				return;
			}
			Compress(_block.data());
			_buffered = 0;
		}
		for (; at + _block.size() <= bytes.size(); at += _block.size()) {
			Compress(bytes.data() + at);
		}
		_buffered = bytes.size() - at;
		std::memcpy(_block.data(), bytes.data() + at, _buffered);
	}

	str SHA512::Finish() {
		const u64 bits = _length * 8;
		_block[_buffered++] = 0x80;
		if (_buffered > _block.size() - 16) {
			std::memset(_block.data() + _buffered, 0, _block.size() - _buffered);
			Compress(_block.data());
			_buffered = 0;
		}
		std::memset(_block.data() + _buffered, 0, _block.size() - _buffered);
		for (u32 index = 0; index < 8; ++index) {
			_block[_block.size() - 1 - index] = static_cast<u8>(bits >> (index * 8));
		}
		Compress(_block.data());

		static constexpr char HEX_DIGITS[] = "0123456789abcdef";
		str text;
		text.reserve(128);
		for (const u64 word : _state) {
			for (int shift = 60; shift >= 0; shift -= 4) {
				text.push_back(HEX_DIGITS[(word >> shift) & 0xf]);
			}
		}
		return text;
	}

	str HashFile(const str& path) {
		std::ifstream file(MakeLongPath(path), std::ios::binary);
		if (!file) {
			return {};
		}
		SHA512 hash;
		std::vector<char> chunk(1 << 20);
		while (file) {
			file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
			hash.Update({reinterpret_cast<const u8*>(chunk.data()), static_cast<size_t>(file.gcount())});
		}
		return file.bad() ? str() : hash.Finish();
	}

} // namespace Shalltear::CSharpEditor
