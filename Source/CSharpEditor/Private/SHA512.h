#pragma once

#include "Shalltear/Core/Types.h"

#include <array>
#include <span>

namespace Shalltear::CSharpEditor {

	class SHA512 {
	public:
		SHA512();

		void Update(std::span<const u8> bytes);

		// lowercase hex, Update must not follow
		str Finish();
	private:
		void Compress(const u8* block);

		std::array<u64, 8> _state;
		std::array<u8, 128> _block{};
		u64 _length = 0;
		size_t _buffered = 0;
	};

	str HashFile(const str& path);

} // namespace Shalltear::CSharpEditor
