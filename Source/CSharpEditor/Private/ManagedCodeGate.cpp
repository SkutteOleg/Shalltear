#include "ManagedCodeGate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <string_view>

namespace Shalltear::CSharpEditor::ManagedCodeGate {

	namespace {

		namespace fs = std::filesystem;

		constexpr size_t CHUNK_BYTES = size_t{1} << 20;
		constexpr size_t ROOT_BYTES = 20;
		constexpr size_t CLR_DIRECTORY = 14;
		constexpr size_t DIRECTORY_BYTES = 8;
		constexpr size_t IMAGE_HEADERS = 24;

		u16 Read16(const u8* at) {
			u16 value = 0;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}

		u32 Read32(const u8* at) {
			u32 value = 0;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}

		bool IsMetadataRoot(const u8* at) {
			if (std::memcmp(at, "BSJB", 4) != 0 || Read16(at + 4) != 1 || Read32(at + 8) != 0) {
				return false;
			}
			const u32 length = Read32(at + 12);
			if (length == 0 || length > 256 || length % 4 != 0) {
				return false;
			}
			return at[16] == 'v' || std::memcmp(at + 16, "PDB", 3) == 0;
		}

		bool IsManagedImage(std::ifstream& file) {
			std::array<u8, 64> dos{};
			file.read(reinterpret_cast<char*>(dos.data()), static_cast<std::streamsize>(dos.size()));
			if (static_cast<size_t>(file.gcount()) != dos.size() || dos[0] != 'M' || dos[1] != 'Z') {
				return false;
			}
			std::array<u8, IMAGE_HEADERS + 112 + (CLR_DIRECTORY + 1) * DIRECTORY_BYTES> image{};
			file.clear();
			file.seekg(static_cast<std::streamoff>(Read32(dos.data() + 0x3C)));
			file.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
			const size_t got = static_cast<size_t>(file.gcount());
			if (got < IMAGE_HEADERS + 2 || std::memcmp(image.data(), "PE\0\0", 4) != 0) {
				return false;
			}
			const u8* optional = image.data() + IMAGE_HEADERS;
			const u16 magic = Read16(optional);
			const size_t directories = magic == 0x20B ? 112 : magic == 0x10B ? 96
																			 : 0;
			if (directories == 0 || got < IMAGE_HEADERS + directories + (CLR_DIRECTORY + 1) * DIRECTORY_BYTES) {
				return false;
			}
			if (Read32(optional + directories - 4) <= CLR_DIRECTORY) {
				return false;
			}
			const u8* entry = optional + directories + CLR_DIRECTORY * DIRECTORY_BYTES;
			return Read32(entry) != 0 || Read32(entry + 4) != 0;
		}

		str Lower(std::string_view text) {
			str lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lowered;
		}

		std::string_view NameReason(const fs::path& relative, bool release) {
			const str name = Lower(relative.filename().generic_string());
			if (name.ends_with(".runtimeconfig.json") || name.ends_with(".deps.json")) {
				return "belongs to .NET runtime";
			}
			std::string_view stem = name;
			if (stem.starts_with("lib")) {
				stem.remove_prefix(3);
			}
			for (const std::string_view prefix : {"hostfxr", "hostpolicy", "coreclr", "clrjit", "system.private.corelib", "mscorlib"}) {
				if (stem.starts_with(prefix)) {
					return "belongs to .NET runtime";
				}
			}
			if (name.ends_with(".cs")) {
				return "is C# source";
			}
			if (!release) {
				return {};
			}
			if (name == "gamescripts.pdb" || name.ends_with(".dbg")) {
				return "has debug symbols (not for Release)";
			}
			for (const fs::path& part : relative) {
				if (Lower(part.generic_string()).ends_with(".dsym")) {
					return "has debug symbols (not for Release)";
				}
			}
			return {};
		}

	} // anonymous namespace

	void Scanner::Begin(const str& directory, bool release) {
		_files.clear();
		_findings.clear();
		_file = std::ifstream();
		_next = 0;
		_kept = 0;
		_release = release;
		_open = false;
		_root = fs::path(directory);
		_buffer.resize(CHUNK_BYTES + ROOT_BYTES);
		std::error_code ec;
		for (fs::recursive_directory_iterator entry(_root, ec), end; !ec && entry != end; entry.increment(ec)) {
			if (entry->is_regular_file(ec)) {
				_files.push_back(entry->path());
			}
		}
		std::sort(_files.begin(), _files.end());
	}

	void Scanner::Report(std::string_view reason) {
		const str file = _files[_next].lexically_relative(_root).generic_string();
		_findings.push_back({file, std::format("'{}' {}. Remove it from build output", file, reason)});
	}

	void Scanner::Open() {
		const fs::path& path = _files[_next];
		const std::string_view named = NameReason(path.lexically_relative(_root), _release);
		if (!named.empty()) {
			Report(named);
			++_next;
			return;
		}
		_file = std::ifstream(path, std::ios::binary);
		if (!_file.is_open()) {
			const str file = path.lexically_relative(_root).generic_string();
			_findings.push_back({file, std::format("couldn't read '{}'. Check nothing holds it open", file)});
			++_next;
			return;
		}
		if (IsManagedImage(_file)) {
			Report("has managed code");
			_file.close();
			++_next;
			return;
		}
		_file.clear();
		_file.seekg(0);
		_kept = 0;
		_open = true;
	}

	void Scanner::Advance(u64 byteBudget) {
		u64 spent = 0;
		while (_next < _files.size()) {
			if (!_open) {
				Open();
				continue;
			}
			if (spent >= byteBudget) {
				return;
			}
			_file.read(reinterpret_cast<char*>(_buffer.data() + _kept), static_cast<std::streamsize>(CHUNK_BYTES));
			const size_t got = static_cast<size_t>(_file.gcount());
			const size_t have = _kept + got;
			spent += got;
			bool found = false;
			if (have >= ROOT_BYTES) {
				const u8* last = _buffer.data() + have - ROOT_BYTES;
				for (const u8* at = _buffer.data(); at <= last; ++at) {
					at = static_cast<const u8*>(std::memchr(at, 'B', static_cast<size_t>(last - at) + 1));
					if (!at) {
						break;
					}
					if (IsMetadataRoot(at)) {
						found = true;
						break;
					}
				}
			}
			if (found) {
				Report("has .NET metadata");
			}
			if (found || got < CHUNK_BYTES) {
				_file.close();
				_open = false;
				++_next;
				continue;
			}
			_kept = ROOT_BYTES - 1;
			std::memmove(_buffer.data(), _buffer.data() + have - _kept, _kept);
		}
	}

	u32 Scan(const str& directory, bool release, u64 sliceBytes, Vector<Finding>& outFindings) {
		Scanner scanner;
		scanner.Begin(directory, release);
		while (!scanner.IsDone()) {
			scanner.Advance(sliceBytes);
		}
		outFindings = scanner.GetFindings();
		return scanner.GetFileCount();
	}

} // namespace Shalltear::CSharpEditor::ManagedCodeGate
