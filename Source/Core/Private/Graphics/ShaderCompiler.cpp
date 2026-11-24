#include "Graphics/ShaderCompiler.h"
#include "Graphics/DxcMinimal.h"
#include "Platform/Library.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/StringHash.h"

#include <atomic>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace Shalltear {

	namespace {

		constexpr const wchar_t* FIXED_ARGUMENTS[] = {
			L"-spirv",
			L"-fspv-target-env=vulkan1.3",
			L"-fspv-preserve-bindings",
			L"-HV",
			L"2021",
		};

		const str& LibraryName() {
			static const str name = Platform::GetSystemLibraryFileName("dxcompiler");
			return name;
		}

		struct DxcState {
			void* module = nullptr;
			Dxc::DxcCreateInstanceProc createInstance = nullptr;
			u64 stamp = 0;
			bool loaded = false;
		};

		DxcState& GetDxc() {
			static DxcState state;
			return state;
		}

		std::once_flag _dxcOnce;

		struct ThreadDxc {
			Dxc::IDxcCompiler3* compiler = nullptr;
			Dxc::IDxcUtils* utils = nullptr;
			bool attempted = false;
		};

		thread_local ThreadDxc _threadDxc;
		thread_local bool _isCompileThread = false;
		thread_local str* _messageSink = nullptr;

		std::atomic<u64> _compilesOnCompileThreads{0};
		std::atomic<u64> _compilesElsewhere{0};

		void LoadDxcOnce() {
			std::call_once(_dxcOnce, []() {
				DxcState& s = GetDxc();
				s.module = Platform::OpenLibrary(LibraryName());
				if (!s.module) {
					SLT_LOG_INFO("Graphics", "{} not found (run Scripts/fetch_prebuilt.py)", LibraryName());
					return;
				}
				s.createInstance = reinterpret_cast<Dxc::DxcCreateInstanceProc>(Platform::FindSymbol(s.module, "DxcCreateInstance"));
				if (!s.createInstance) {
					SLT_LOG_ERROR("Graphics", "{} is present but DxcCreateInstance is missing", LibraryName());
					return;
				}
				ThreadDxc& own = _threadDxc;
				own.attempted = true;
				if (!Dxc::Succeeded(s.createInstance(Dxc::CLSID_DxcCompiler, Dxc::IID_IDxcCompiler3, reinterpret_cast<void**>(&own.compiler))) ||
					!Dxc::Succeeded(s.createInstance(Dxc::CLSID_DxcUtils, Dxc::IID_IDxcUtils, reinterpret_cast<void**>(&own.utils)))) {
					SLT_LOG_ERROR("Graphics", "DxcCreateInstance failed for IDxcCompiler3/IDxcUtils");
					own.compiler = nullptr;
					own.utils = nullptr;
					return;
				}

				std::error_code ec;
				std::filesystem::path dllPath;
				dllPath = std::filesystem::current_path(ec) / LibraryName();
				if (!std::filesystem::exists(dllPath, ec)) {
					dllPath = LibraryName();
				}
				u64 size = static_cast<u64>(std::filesystem::file_size(dllPath, ec));
				u64 time = static_cast<u64>(std::filesystem::last_write_time(dllPath, ec).time_since_epoch().count());
				s.stamp = (size * 1099511628211ull) ^ time;
				for (const wchar_t* argument : FIXED_ARGUMENTS) {
					for (const wchar_t* c = argument; *c; ++c) {
						s.stamp = (s.stamp ^ static_cast<u64>(*c)) * 1099511628211ull;
					}
					s.stamp = (s.stamp ^ 0x1F) * 1099511628211ull;
				}

				s.loaded = true;
			});
		}

		ThreadDxc* ThreadCompiler() {
			LoadDxcOnce();
			DxcState& s = GetDxc();
			if (!s.loaded) {
				return nullptr;
			}
			ThreadDxc& own = _threadDxc;
			if (!own.attempted) {
				own.attempted = true;
				if (!Dxc::Succeeded(s.createInstance(Dxc::CLSID_DxcCompiler, Dxc::IID_IDxcCompiler3, reinterpret_cast<void**>(&own.compiler))) ||
					!Dxc::Succeeded(s.createInstance(Dxc::CLSID_DxcUtils, Dxc::IID_IDxcUtils, reinterpret_cast<void**>(&own.utils)))) {
					SLT_LOG_ERROR("Graphics", "DxcCreateInstance failed for this thread's IDxcCompiler3/IDxcUtils");
					own.compiler = nullptr;
					own.utils = nullptr;
				}
			}
			return own.compiler && own.utils ? &own : nullptr;
		}

		std::wstring UTF8ToWide(std::string_view utf8) {
			std::wstring wide;
			wide.reserve(utf8.size());
			size_t i = 0;
			while (i < utf8.size()) {
				u32 cp = 0;
				u8 c = static_cast<u8>(utf8[i]);
				u32 extra = 0;
				if (c < 0x80) {
					cp = c;
				} else if ((c >> 5) == 0x6) {
					cp = c & 0x1F;
					extra = 1;
				} else if ((c >> 4) == 0xE) {
					cp = c & 0x0F;
					extra = 2;
				} else if ((c >> 3) == 0x1E) {
					cp = c & 0x07;
					extra = 3;
				}
				++i;
				for (u32 k = 0; k < extra && i < utf8.size(); ++k, ++i) {
					cp = (cp << 6) | (static_cast<u8>(utf8[i]) & 0x3F);
				}
				if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
					cp -= 0x10000;
					wide.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
					wide.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
				} else {
					wide.push_back(static_cast<wchar_t>(cp));
				}
			}
			return wide;
		}

		str WideToUTF8(const wchar_t* wide) {
			str utf8;
			if (!wide) {
				return utf8;
			}
			for (size_t i = 0; wide[i]; ++i) {
				u32 cp = wide[i];
				if (cp >= 0xD800 && cp <= 0xDBFF && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (wide[i + 1] - 0xDC00);
					++i;
				}
				if (cp < 0x80) {
					utf8.push_back(static_cast<char>(cp));
				} else if (cp < 0x800) {
					utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
					utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else if (cp < 0x10000) {
					utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
					utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else {
					utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
					utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
					utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
			}
			return utf8;
		}

		struct IncludeHandler final : Dxc::IDxcIncludeHandler {
			Dxc::IDxcUtils* utils = nullptr;
			Vector<std::filesystem::path> searchDirs{GetMemoryResource(MemTag::Asset)};
			Vector<ShaderIncludeDependency>* dependencies = nullptr;
			std::deque<str>* contentPool = nullptr;

			Dxc::HResult QueryInterface(const Dxc::Guid&, void** object) override {
				*object = this;
				AddRef();
				return Dxc::S_OK_;
			}
			u32 AddRef() override { return ++_refs; }
			u32 Release() override { return --_refs; }

			Dxc::HResult LoadSource(const wchar_t* filename, Dxc::IDxcBlob** includeSource) override {
				*includeSource = nullptr;
				str requested = WideToUTF8(filename);

				std::error_code ec;
				const std::filesystem::path asWritten = std::filesystem::path(requested).lexically_normal();
				std::filesystem::path relative = asWritten;
				if (asWritten.is_absolute() && !searchDirs.empty()) {
					std::filesystem::path lexical = asWritten.lexically_relative(searchDirs.front());
					if (!lexical.empty() && *lexical.begin() != "..") {
						relative = std::move(lexical);
					}
				}

				std::filesystem::path candidate = asWritten;
				bool found = std::filesystem::exists(candidate, ec);
				for (const std::filesystem::path& dir : searchDirs) {
					if (found) {
						break;
					}
					candidate = (dir / relative).lexically_normal();
					found = std::filesystem::exists(candidate, ec);
				}
				if (!found) {
					SLT_LOG_TRACE("Graphics", "shader include probe missed: '{}'", requested);
					return static_cast<Dxc::HResult>(0x80070002);
				}

				str content = FileSystem::ReadTextFile(Path(candidate.generic_string()), MemTag::Asset);
				contentPool->push_back(std::move(content));
				const str& pinned = contentPool->back();

				if (dependencies) {
					ShaderIncludeDependency dep;
					dep.physicalPath = candidate.generic_string();
					dep.contentHash = StringHash(std::string_view(pinned)).GetValue();
					dependencies->push_back(std::move(dep));
				}

				Dxc::IDxcBlobEncoding* blob = nullptr;
				if (!Dxc::Succeeded(utils->CreateBlobFromPinned(pinned.data(), static_cast<u32>(pinned.size()), 65001, &blob))) {
					return static_cast<Dxc::HResult>(0x80004005);
				}
				*includeSource = blob;
				return Dxc::S_OK_;
			}
		private:
			std::atomic<u32> _refs{1};
		};

	} // anonymous namespace

	std::string_view ShaderCompiler::GetLibraryName() {
		return LibraryName();
	}

	bool ShaderCompiler::IsAvailable() {
		LoadDxcOnce();
		return GetDxc().loaded;
	}

	u64 ShaderCompiler::GetCompilerStamp() {
		LoadDxcOnce();
		return GetDxc().stamp;
	}

	void ShaderCompiler::MarkCompileThread() {
		_isCompileThread = true;
	}

	void ShaderCompiler::ReleaseThreadCompiler() {
		ThreadDxc& own = _threadDxc;
		if (own.compiler) {
			own.compiler->Release();
			own.compiler = nullptr;
		}
		if (own.utils) {
			own.utils->Release();
			own.utils = nullptr;
		}
		own.attempted = false;
	}

	void ShaderCompiler::GetCompileCounts(u64& onCompileThreads, u64& elsewhere) {
		onCompileThreads = _compilesOnCompileThreads.load(std::memory_order_relaxed);
		elsewhere = _compilesElsewhere.load(std::memory_order_relaxed);
	}

	ShaderCompileResult ShaderCompiler::Compile(
		std::string_view source,
		const Path& sourcePhysicalPath,
		ShaderStageKind stage,
		std::span<const str> defines,
		ShaderCompileMode mode) {

		ShaderCompileResult result;
		ThreadDxc* dxc = ThreadCompiler();
		if (!dxc) {
			result.errors = LibraryName() + " unavailable";
			return result;
		}
		(_isCompileThread ? _compilesOnCompileThreads : _compilesElsewhere).fetch_add(1, std::memory_order_relaxed);

		std::vector<std::wstring> args;
		args.push_back(UTF8ToWide(sourcePhysicalPath.ToString()));
		args.push_back(L"-E");
		args.push_back(stage == ShaderStageKind::Vertex ? L"VSMain" : stage == ShaderStageKind::Pixel ? L"PSMain"
																									  : L"CSMain");
		args.push_back(L"-T");
		args.push_back(stage == ShaderStageKind::Vertex ? L"vs_6_0" : stage == ShaderStageKind::Pixel ? L"ps_6_0"
																									  : L"cs_6_0");
		bool forWebGPU = false;
		for (const str& define : defines) {
			if (define == "SHALLTEAR_WEBGPU=1" || define == "SHALLTEAR_WEBGPU") {
				forWebGPU = true;
				break;
			}
		}
		for (const wchar_t* arg : FIXED_ARGUMENTS) {
			if (forWebGPU && std::wcscmp(arg, L"-fspv-target-env=vulkan1.3") == 0) {
				args.push_back(L"-fspv-target-env=vulkan1.1");
			} else {
				args.push_back(arg);
			}
		}
		if (mode != ShaderCompileMode::Optimized) {
			args.push_back(L"-Od");
		}
		if (mode == ShaderCompileMode::Debug) {
			args.push_back(L"-fspv-debug=vulkan-with-source");
		}
		for (const str& define : defines) {
			args.push_back(L"-D" + UTF8ToWide(define));
		}

		std::vector<const wchar_t*> argPtrs;
		argPtrs.reserve(args.size());
		for (const std::wstring& arg : args) {
			argPtrs.push_back(arg.c_str());
		}

		std::deque<str> contentPool;
		IncludeHandler includeHandler;
		includeHandler.utils = dxc->utils;
		includeHandler.searchDirs.push_back(std::filesystem::path(sourcePhysicalPath.ToString()).parent_path());
		Path engineShaders = FileSystem::Resolve(Path("engine://Shaders"));
		std::error_code engineEc;
		std::filesystem::path engineDir(engineShaders.ToString());
		if (std::filesystem::is_directory(engineDir, engineEc)) {
			includeHandler.searchDirs.push_back(std::move(engineDir));
		}
		includeHandler.dependencies = &result.includes;
		includeHandler.contentPool = &contentPool;

		Dxc::DxcBuffer buffer{source.data(), source.size(), 65001};
		Dxc::IDxcResult* compileResult = nullptr;
		Dxc::HResult hr = dxc->compiler->Compile(&buffer, argPtrs.data(), static_cast<u32>(argPtrs.size()),
												 &includeHandler, Dxc::IID_IDxcResult, reinterpret_cast<void**>(&compileResult));
		if (!Dxc::Succeeded(hr) || !compileResult) {
			result.errors = "IDxcCompiler3::Compile failed outright";
			return result;
		}

		Dxc::IDxcBlobUtf8* errors = nullptr;
		compileResult->GetOutput(Dxc::DXC_OUT_ERRORS, Dxc::IID_IDxcBlobUtf8, reinterpret_cast<void**>(&errors), nullptr);
		if (errors) {
			if (errors->GetStringLength() > 0) {
				result.errors = errors->GetStringPointer();
				if (_messageSink) {
					*_messageSink += result.errors;
				}
			}
			errors->Release();
		}

		Dxc::HResult status = 0;
		compileResult->GetStatus(&status);
		if (!Dxc::Succeeded(status)) {
			compileResult->Release();
			return result;
		}

		Dxc::IDxcBlob* object = nullptr;
		compileResult->GetOutput(Dxc::DXC_OUT_OBJECT, Dxc::IID_IDxcBlob, reinterpret_cast<void**>(&object), nullptr);
		if (!object || object->GetBufferSize() == 0) {
			if (object) {
				object->Release();
			}
			compileResult->Release();
			result.errors += "\ncompile succeeded but produced no object";
			return result;
		}

		const u8* data = static_cast<const u8*>(object->GetBufferPointer());
		result.spirv.assign(data, data + object->GetBufferSize());
		object->Release();
		compileResult->Release();

		result.success = true;
		return result;
	}

	ShaderCompiler::ScopedMessages::ScopedMessages(str& sink) : _outer(_messageSink) { _messageSink = &sink; }

	ShaderCompiler::ScopedMessages::~ScopedMessages() { _messageSink = _outer; }

} // namespace Shalltear
