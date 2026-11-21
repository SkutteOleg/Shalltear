#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Shalltear::TestHelpers {

	struct LibraryNeeds {
		Vector<str> libraries;
		u32 glibcMajor = 0;
		u32 glibcMinor = 0;
	};

	namespace LibraryReaderDetail {

		template <typename T>
		bool ReadAt(const str& bytes, u64 offset, T& out) {
			if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
				return false;
			}
			std::memcpy(&out, bytes.data() + offset, sizeof(T));
			return true;
		}

		struct Section {
			u32 name;
			u32 type;
			u64 flags;
			u64 address;
			u64 offset;
			u64 size;
			u32 link;
			u32 info;
			u64 alignment;
			u64 entrySize;
		};

		inline str TextAt(const str& bytes, const Section& strings, u64 at) {
			if (at >= strings.size || strings.offset + at >= bytes.size()) {
				return {};
			}
			const size_t start = static_cast<size_t>(strings.offset + at);
			const size_t limit = std::min(bytes.size(), static_cast<size_t>(strings.offset + strings.size));
			size_t end = start;
			while (end < limit && bytes[end] != 0) {
				++end;
			}
			return bytes.substr(start, end - start);
		}

	} // namespace LibraryReaderDetail

	inline bool ReadLibraryNeeds(const std::filesystem::path& file, LibraryNeeds& out) {
		using namespace LibraryReaderDetail;
		std::ifstream stream(file, std::ios::binary);
		const str bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f"
														   "ELF",
											 4) != 0 ||
			bytes[4] != 2 || bytes[5] != 1) {
			return false;
		}
		u64 sectionOffset = 0;
		u16 sectionSize = 0;
		u16 sectionCount = 0;
		if (!ReadAt(bytes, 0x28, sectionOffset) || !ReadAt(bytes, 0x3A, sectionSize) || !ReadAt(bytes, 0x3C, sectionCount) || sectionSize < sizeof(Section)) {
			return false;
		}
		Vector<Section> sections;
		for (u16 index = 0; index < sectionCount; ++index) {
			Section section{};
			if (!ReadAt(bytes, sectionOffset + u64{index} * sectionSize, section)) {
				return false;
			}
			sections.push_back(section);
		}
		constexpr u32 DYNAMIC = 6;
		constexpr u32 VERSION_NEEDS = 0x6FFFFFFE;
		constexpr i64 NEEDED = 1;
		for (const Section& section : sections) {
			if (section.link >= sections.size()) {
				continue;
			}
			const Section& strings = sections[section.link];
			if (section.type == DYNAMIC) {
				for (u64 at = 0; at + 16 <= section.size; at += 16) {
					i64 tag = 0;
					u64 value = 0;
					if (!ReadAt(bytes, section.offset + at, tag) || !ReadAt(bytes, section.offset + at + 8, value)) {
						return false;
					}
					if (tag == NEEDED) {
						out.libraries.push_back(TextAt(bytes, strings, value));
					}
				}
			} else if (section.type == VERSION_NEEDS) {
				u64 need = section.offset;
				for (u32 entry = 0; entry < section.info; ++entry) {
					u16 count = 0;
					u32 auxiliary = 0;
					u32 next = 0;
					if (!ReadAt(bytes, need + 2, count) || !ReadAt(bytes, need + 8, auxiliary) || !ReadAt(bytes, need + 12, next)) {
						return false;
					}
					u64 at = need + auxiliary;
					for (u16 index = 0; index < count; ++index) {
						u32 name = 0;
						u32 nextAuxiliary = 0;
						if (!ReadAt(bytes, at + 8, name) || !ReadAt(bytes, at + 12, nextAuxiliary)) {
							return false;
						}
						const str version = TextAt(bytes, strings, name);
						if (version.starts_with("GLIBC_")) {
							char* rest = nullptr;
							const u32 major = static_cast<u32>(std::strtoul(version.c_str() + 6, &rest, 10));
							const u32 minor = rest && *rest == '.' ? static_cast<u32>(std::strtoul(rest + 1, nullptr, 10)) : 0;
							if (major > out.glibcMajor || (major == out.glibcMajor && minor > out.glibcMinor)) {
								out.glibcMajor = major;
								out.glibcMinor = minor;
							}
						}
						if (nextAuxiliary == 0) {
							break;
						}
						at += nextAuxiliary;
					}
					if (next == 0) {
						break;
					}
					need += next;
				}
			}
		}
		return true;
	}

} // namespace Shalltear::TestHelpers
