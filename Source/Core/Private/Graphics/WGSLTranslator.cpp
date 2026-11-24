#include "Graphics/WGSLTranslator.h"

#include "Platform/Process.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace Shalltear {

	namespace {

		std::atomic<u64> _tempCounter{0};

		std::filesystem::path FromUTF8(std::string_view s) {
			return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
		}

		str FindTintExecutable() {
			if (const char* envPath = std::getenv("SHALLTEAR_TINT_PATH"); envPath && *envPath) {
				std::error_code ec;
				if (std::filesystem::is_regular_file(FromUTF8(envPath), ec)) {
					return str(envPath);
				}
			}

			const str exePath = Platform::GetExecutablePath();
			const std::filesystem::path exeDir = exePath.empty() ? std::filesystem::current_path()
																 : FromUTF8(exePath).parent_path();

			const char* names[] = {"tint.exe", "tint"};
			auto probeDir = [&](const std::filesystem::path& dir) -> str {
				std::error_code ec;
				for (const char* name : names) {
					const std::filesystem::path candidate = dir / name;
					if (std::filesystem::is_regular_file(candidate, ec)) {
						const std::u8string u8 = candidate.generic_u8string();
						return str(reinterpret_cast<const char*>(u8.data()), u8.size());
					}
				}
				return {};
			};

			if (str beside = probeDir(exeDir); !beside.empty()) {
				return beside;
			}
			if (str sdkDawn = probeDir(exeDir / "Toolchain" / "dawn" / "bin"); !sdkDawn.empty()) {
				return sdkDawn;
			}

			std::error_code ec;
			std::filesystem::path walk = exeDir;
			for (int depth = 0; depth < 6 && !walk.empty(); ++depth) {
				if (str found = probeDir(walk / "External" / "Prebuilt" / "dawn" / "bin"); !found.empty()) {
					return found;
				}
				if (str found = probeDir(walk / "Toolchain" / "dawn" / "bin"); !found.empty()) {
					return found;
				}
				const std::filesystem::path parent = walk.parent_path();
				if (parent == walk) {
					break;
				}
				walk = parent;
			}

			walk = std::filesystem::current_path(ec);
			for (int depth = 0; depth < 6 && !walk.empty(); ++depth) {
				if (str found = probeDir(walk / "External" / "Prebuilt" / "dawn" / "bin"); !found.empty()) {
					return found;
				}
				const std::filesystem::path parent = walk.parent_path();
				if (parent == walk) {
					break;
				}
				walk = parent;
			}
			return {};
		}

		struct TranslatorState {
			str path;
			u64 stamp = 0;
			bool checked = false;
			std::mutex mutex;
		};

		TranslatorState& State() {
			static TranslatorState state;
			return state;
		}

		void EnsureChecked() {
			TranslatorState& s = State();
			std::lock_guard lock(s.mutex);
			if (s.checked) {
				return;
			}
			s.checked = true;
			s.path = FindTintExecutable();
			if (s.path.empty()) {
				return;
			}
			std::error_code ec;
			const std::filesystem::path file = FromUTF8(s.path);
			const u64 size = static_cast<u64>(std::filesystem::file_size(file, ec));
			const u64 mtime = static_cast<u64>(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
			s.stamp = (size * 1099511628211ull) ^ mtime ^ 0x5747534C54494E54ull;
		}

	} // anonymous namespace

	bool WGSLTranslator::IsAvailable() {
		EnsureChecked();
		return !State().path.empty();
	}

	str WGSLTranslator::GetToolPath() {
		EnsureChecked();
		return State().path;
	}

	u64 WGSLTranslator::GetTranslatorStamp() {
		EnsureChecked();
		return State().stamp;
	}

	bool WGSLTranslator::TranslateSpirvToWGSL(std::span<const u8> spirv, Vector<u8>& outWGSL, str& outError) {
		outWGSL.clear();
		outError.clear();
		if (spirv.empty()) {
			return true;
		}
		EnsureChecked();
		const str tool = State().path;
		if (tool.empty()) {
			outError = "tint executable not found (run: python Scripts/fetch_prebuilt.py --web)";
			return false;
		}

		std::error_code ec;
		const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
		if (ec) {
			outError = "failed to resolve temporary directory for WGSL translation";
			return false;
		}

		const u64 id = _tempCounter.fetch_add(1, std::memory_order_relaxed);
		const str stem = "slt_tint_" + std::to_string(reinterpret_cast<uintptr_t>(&State())) + "_" + std::to_string(id);
		const std::filesystem::path spvPath = tempDir / (stem + ".spv");
		const std::filesystem::path wgslPath = tempDir / (stem + ".wgsl");

		struct Cleanup {
			std::filesystem::path a;
			std::filesystem::path b;
			~Cleanup() {
				std::error_code ignored;
				std::filesystem::remove(a, ignored);
				std::filesystem::remove(b, ignored);
			}
		} cleanup{spvPath, wgslPath};

		{
			std::ofstream out(spvPath, std::ios::binary | std::ios::trunc);
			if (!out) {
				outError = "failed to write temporary SPIR-V input for tint";
				return false;
			}
			out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
			if (!out) {
				outError = "failed to flush temporary SPIR-V input for tint";
				return false;
			}
		}

		const std::u8string spvU8 = spvPath.generic_u8string();
		const std::u8string wgslU8 = wgslPath.generic_u8string();
		const str spvStr(reinterpret_cast<const char*>(spvU8.data()), spvU8.size());
		const str wgslStr(reinterpret_cast<const char*>(wgslU8.data()), wgslU8.size());

		const str args[] = {
			str("--input-format"),
			str("spirv"),
			str("--format"),
			str("wgsl"),
			str("--allow-non-uniform-derivatives"),
			str("-o"),
			wgslStr,
			spvStr,
		};

		str processOutput;
		i32 exitCode = -1;
		if (!Platform::RunProcessCapture(tool, args, processOutput, exitCode)) {
			outError = "failed to spawn tint executable: " + tool;
			return false;
		}
		if (exitCode != 0) {
			outError = processOutput.empty() ? ("tint exited with code " + std::to_string(exitCode)) : processOutput;
			return false;
		}

		std::ifstream in(wgslPath, std::ios::binary | std::ios::ate);
		if (!in) {
			outError = "tint succeeded but produced no WGSL output";
			return false;
		}
		const std::streamoff size = in.tellg();
		if (size <= 0) {
			outError = "tint produced empty WGSL output";
			return false;
		}
		outWGSL.resize(static_cast<size_t>(size));
		in.seekg(0, std::ios::beg);
		in.read(reinterpret_cast<char*>(outWGSL.data()), size);
		if (!in) {
			outWGSL.clear();
			outError = "failed to read WGSL output from tint";
			return false;
		}
		return true;
	}

} // namespace Shalltear
