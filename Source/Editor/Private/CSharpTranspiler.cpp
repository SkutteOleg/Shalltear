#include "Shalltear/Editor/CSharpTranspiler.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef CreateDirectory
#undef GetObject
#endif

namespace Shalltear::CSharpTranspiler {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* GENERATED_RELATIVE_DIR = "Library/Generated/CSharp";
		constexpr const char* MODULE_ENTRY_STEM = "ShalltearCSharpModuleEntry";

		CompilerDiagnostic MakeDiagnostic(
			std::string_view file,
			u32 line,
			u32 column,
			CompilerDiagnosticSeverity severity,
			std::string_view message) {
			CompilerDiagnostic diag;
			diag.severity = severity;
			diag.file = str(file);
			diag.line = line;
			diag.column = column;
			diag.message = str(message);
			std::string_view sevText = "error";
			if (severity == CompilerDiagnosticSeverity::Warning) {
				sevText = "warning";
			} else if (severity == CompilerDiagnosticSeverity::Note) {
				sevText = "note";
			}
			diag.raw = std::format("{}:{}:{}: {}: {}", file, line, column, sevText, message);
			return diag;
		}

		bool ProjectHasCppModuleEntry(const str& sourceDir) {
			std::error_code ec;
			fs::path root(sourceDir);
			if (!fs::is_directory(root, ec)) {
				return false;
			}
			for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
				 it != end && !ec; it.increment(ec)) {
				if (!it->is_regular_file(ec)) {
					continue;
				}
				str ext = StringUtil::ToLower(it->path().extension().generic_string());
				if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") {
					continue;
				}
				str content = FileSystem::ReadTextFile(Path(it->path().generic_string()), MemTag::Editor);
				if (content.find("SLT_IMPLEMENT_MODULE") != str::npos) {
					return true;
				}
			}
			return false;
		}

		bool WriteGeneratedFile(const fs::path& targetPath, const str& content,
								const fs::path& projectRoot, TranspileProjectResult& out) {
			std::error_code ec;
			str relative = fs::relative(targetPath, projectRoot, ec).generic_string();
			if (fs::is_regular_file(targetPath, ec)) {
				str existing = FileSystem::ReadTextFile(Path(targetPath.generic_string()), MemTag::Editor);
				if (existing == content) {
					out.unchanged.push_back(std::move(relative));
					return true;
				}
			} else {
				out.structureChanged = true;
			}

			fs::create_directories(targetPath.parent_path(), ec);
			if (!FileSystem::WriteTextFile(Path(targetPath.generic_string()), content)) {
				out.diagnostics.push_back(MakeDiagnostic(
					targetPath.generic_string(), 1, 1, CompilerDiagnosticSeverity::Error,
					"could not write generated file '" + targetPath.generic_string() + "'"));
				return false;
			}
			out.written.push_back(std::move(relative));
			return true;
		}

#if defined(_WIN32)
		using HostfxrHandle = void*;

		struct HostfxrInitializeParameters {
			size_t size;
			const wchar_t* hostPath;
			const wchar_t* dotnetRoot;
		};

		enum class HostfxrDelegateType : i32 {
			ComActivation = 0,
			LoadInMemoryAssembly = 1,
			WinrtActivation = 2,
			ComRegister = 3,
			ComUnregister = 4,
			LoadAssemblyAndGetFunctionPointer = 5,
			GetFunctionPointer = 6,
		};

		using HostfxrInitializeForRuntimeConfigFn = i32(__cdecl*)(
			const wchar_t* runtimeConfigPath,
			const HostfxrInitializeParameters* parameters,
			HostfxrHandle* hostContextHandle);
		using HostfxrGetRuntimeDelegateFn = i32(__cdecl*)(
			HostfxrHandle hostContextHandle,
			HostfxrDelegateType type,
			void** delegate);
		using HostfxrCloseFn = i32(__cdecl*)(HostfxrHandle hostContextHandle);

		using LoadAssemblyAndGetFunctionPointerFn = i32(__stdcall*)(
			const wchar_t* assemblyPath,
			const wchar_t* typeName,
			const wchar_t* methodName,
			const wchar_t* delegateTypeName,
			void* reserved,
			void** delegate);

		using RoslynFreeBufferFn = void(__cdecl*)(const char* ptr);
		using RoslynGetEngineStubsFn = i32(__cdecl*)(const char** outUtf8);
		using RoslynTranspileSourceFn = i32(__cdecl*)(
			const char* sourcePathUtf8,
			const char* cSharpTextUtf8,
			const char** outJsonUtf8);
		using RoslynTranspileProjectFn = i32(__cdecl*)(
			const char* projectPathUtf8,
			const char** outJsonUtf8);

		struct RoslynState {
			HMODULE hostfxrModule = nullptr;
			RoslynFreeBufferFn freeBuffer = nullptr;
			RoslynGetEngineStubsFn getEngineStubs = nullptr;
			RoslynTranspileSourceFn transpileSource = nullptr;
			RoslynTranspileProjectFn transpileProject = nullptr;
			bool loaded = false;
		};

		std::once_flag _roslynOnce;

		RoslynState& GetRoslynState() {
			static RoslynState state;
			return state;
		}

		fs::path LocateRoslynDll() {
			std::error_code ec;
			str exePath = ScriptProjectOps::GetEditorExecutablePath();
			if (!exePath.empty()) {
				fs::path besideExe = fs::path(exePath).parent_path() / "Roslyn" / "ShalltearRoslyn.dll";
				if (fs::is_regular_file(besideExe, ec)) {
					return besideExe;
				}
			}

			fs::path probe = fs::current_path(ec);
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				for (const char* rel : {
						 "Roslyn/ShalltearRoslyn.dll",
						 "build/bin/Roslyn/ShalltearRoslyn.dll",
						 "External/Prebuilt/roslyn/bin/ShalltearRoslyn.dll"}) {
					fs::path candidate = probe / rel;
					if (fs::is_regular_file(candidate, ec)) {
						return fs::weakly_canonical(candidate, ec).make_preferred();
					}
				}
				fs::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			return {};
		}

		fs::path LocateHostfxrDll(const fs::path& roslynDir, fs::path& outDotnetRoot) {
			std::error_code ec;
			Vector<fs::path> roots{GetMemoryResource(MemTag::Editor)};
			if (!roslynDir.empty()) {
				roots.push_back((roslynDir / "dotnet").make_preferred());
				roots.push_back((roslynDir.parent_path() / "dotnet").make_preferred());
			}
			wchar_t envBuf[1024]{};
			if (::GetEnvironmentVariableW(L"DOTNET_ROOT", envBuf, 1024) > 0) {
				roots.push_back(fs::path(envBuf).make_preferred());
			}
			if (::GetEnvironmentVariableW(L"DOTNET_ROOT(x86)", envBuf, 1024) > 0) {
				roots.push_back(fs::path(envBuf).make_preferred());
			}
			wchar_t pathDotnetBuf[1024]{};
			if (::SearchPathW(nullptr, L"dotnet.exe", nullptr, 1024, pathDotnetBuf, nullptr) > 0) {
				roots.push_back(fs::path(pathDotnetBuf).parent_path().make_preferred());
			}
			if (::GetEnvironmentVariableW(L"USERPROFILE", envBuf, 1024) > 0) {
				roots.push_back((fs::path(envBuf) / ".dotnet").make_preferred());
			}
			roots.push_back(fs::path(L"C:\\Program Files\\dotnet").make_preferred());

			for (const fs::path& root : roots) {
				fs::path fxrDir = root / "host" / "fxr";
				if (!fs::is_directory(fxrDir, ec)) {
					continue;
				}
				Vector<fs::path> versions{GetMemoryResource(MemTag::Editor)};
				for (fs::directory_iterator it(fxrDir, ec), end; it != end && !ec; it.increment(ec)) {
					if (it->is_directory(ec) && fs::is_regular_file(it->path() / "hostfxr.dll", ec)) {
						versions.push_back(it->path());
					}
				}
				if (versions.empty()) {
					continue;
				}
				std::sort(versions.begin(), versions.end());
				outDotnetRoot = fs::weakly_canonical(root, ec).make_preferred();
				return fs::weakly_canonical(versions.back() / "hostfxr.dll", ec).make_preferred();
			}
			return {};
		}

		void LoadRoslynOnce() {
			std::call_once(_roslynOnce, []() {
				RoslynState& s = GetRoslynState();
				fs::path roslynDll = LocateRoslynDll();
				if (roslynDll.empty()) {
					return;
				}
				roslynDll.make_preferred();
				std::error_code ec;
				fs::path runtimeConfig = (roslynDll.parent_path() / "ShalltearRoslyn.runtimeconfig.json").make_preferred();
				if (!fs::is_regular_file(runtimeConfig, ec)) {
					return;
				}

				fs::path dotnetRoot;
				fs::path hostfxrPath = LocateHostfxrDll(roslynDll.parent_path(), dotnetRoot);
				if (hostfxrPath.empty()) {
					return;
				}
				hostfxrPath.make_preferred();
				dotnetRoot.make_preferred();

				s.hostfxrModule = ::LoadLibraryW(hostfxrPath.wstring().c_str());
				if (!s.hostfxrModule) {
					return;
				}

				auto initFn = reinterpret_cast<HostfxrInitializeForRuntimeConfigFn>(
					::GetProcAddress(s.hostfxrModule, "hostfxr_initialize_for_runtime_config"));
				auto getDelegateFn = reinterpret_cast<HostfxrGetRuntimeDelegateFn>(
					::GetProcAddress(s.hostfxrModule, "hostfxr_get_runtime_delegate"));
				auto closeFn = reinterpret_cast<HostfxrCloseFn>(
					::GetProcAddress(s.hostfxrModule, "hostfxr_close"));
				if (!initFn || !getDelegateFn || !closeFn) {
					return;
				}

				wchar_t exeBuf[1024]{};
				::GetModuleFileNameW(nullptr, exeBuf, 1024);
				fs::path hostExePath = fs::path(exeBuf).make_preferred();

				std::wstring wConfig = runtimeConfig.wstring();
				std::wstring wDll = roslynDll.wstring();
				std::wstring wRoot = dotnetRoot.wstring();
				std::wstring wHostExe = hostExePath.wstring();
				HostfxrInitializeParameters params{};
				params.size = sizeof(HostfxrInitializeParameters);
				params.hostPath = wHostExe.c_str();
				params.dotnetRoot = wRoot.c_str();

				HostfxrHandle ctx = nullptr;
				i32 rc = initFn(wConfig.c_str(), &params, &ctx);
				if (ctx == nullptr || (rc != 0 && rc != 1 && rc != 2)) {
					return;
				}

				void* loadDelegatePtr = nullptr;
				rc = getDelegateFn(ctx, HostfxrDelegateType::LoadAssemblyAndGetFunctionPointer, &loadDelegatePtr);
				closeFn(ctx);
				if (rc != 0 || !loadDelegatePtr) {
					return;
				}

				auto loadFn = reinterpret_cast<LoadAssemblyAndGetFunctionPointerFn>(loadDelegatePtr);
				const wchar_t* unmanagedCallersOnly = reinterpret_cast<const wchar_t*>(static_cast<intptr_t>(-1));
				const wchar_t* typeName = L"Shalltear.Roslyn.RoslynTranspilerExports, ShalltearRoslyn";

				void* freePtr = nullptr;
				void* stubsPtr = nullptr;
				void* sourcePtr = nullptr;
				void* projectPtr = nullptr;
				if (loadFn(wDll.c_str(), typeName, L"FreeBuffer", unmanagedCallersOnly, nullptr, &freePtr) != 0 ||
					loadFn(wDll.c_str(), typeName, L"GetEngineStubs", unmanagedCallersOnly, nullptr, &stubsPtr) != 0 ||
					loadFn(wDll.c_str(), typeName, L"TranspileSource", unmanagedCallersOnly, nullptr, &sourcePtr) != 0 ||
					loadFn(wDll.c_str(), typeName, L"TranspileProject", unmanagedCallersOnly, nullptr, &projectPtr) != 0) {
					return;
				}

				s.freeBuffer = reinterpret_cast<RoslynFreeBufferFn>(freePtr);
				s.getEngineStubs = reinterpret_cast<RoslynGetEngineStubsFn>(stubsPtr);
				s.transpileSource = reinterpret_cast<RoslynTranspileSourceFn>(sourcePtr);
				s.transpileProject = reinterpret_cast<RoslynTranspileProjectFn>(projectPtr);
				s.loaded = true;
			});
		}

		CompilerDiagnostic ParseDiagnosticJson(const JSONValue& item) {
			CompilerDiagnostic diag;
			std::string_view sev = item["Severity"].AsString("error");
			if (sev == "warning") {
				diag.severity = CompilerDiagnosticSeverity::Warning;
			} else if (sev == "note") {
				diag.severity = CompilerDiagnosticSeverity::Note;
			} else {
				diag.severity = CompilerDiagnosticSeverity::Error;
			}
			diag.file = str(item["File"].AsString());
			diag.line = static_cast<u32>(item["Line"].AsInt(1));
			diag.column = static_cast<u32>(item["Column"].AsInt(1));
			diag.message = str(item["Message"].AsString());
			diag.raw = str(item["Raw"].AsString());
			return diag;
		}
#endif

	} // anonymous namespace

	bool IsRoslynLoaded() {
#if defined(_WIN32)
		LoadRoslynOnce();
		return GetRoslynState().loaded;
#else
		return false;
#endif
	}

	bool EnsureCSharpProjectFiles(const str& projectPath) {
		if (projectPath.empty()) {
			return false;
		}
		str stubsText;
#if defined(_WIN32)
		LoadRoslynOnce();
		RoslynState& s = GetRoslynState();
		if (s.loaded && s.getEngineStubs && s.freeBuffer) {
			const char* ptr = nullptr;
			if (s.getEngineStubs(&ptr) != 0 && ptr != nullptr) {
				stubsText = str(ptr);
				s.freeBuffer(ptr);
			}
		}
#endif
		if (stubsText.empty()) {
			return false;
		}

		std::error_code ec;
		fs::path stubsPath = fs::path(projectPath) / "Library" / "Generated" / "CSharpStubs" / "Shalltear.Stubs.cs";
		fs::create_directories(stubsPath.parent_path(), ec);
		if (!fs::is_regular_file(stubsPath, ec) ||
			FileSystem::ReadTextFile(Path(stubsPath.generic_string()), MemTag::Editor) != stubsText) {
			FileSystem::WriteTextFile(Path(stubsPath.generic_string()), stubsText);
		}

		str moduleName = ScriptProjectOps::GetModuleName(projectPath);
		fs::path csprojPath = fs::path(projectPath) / (moduleName + ".csproj");
		if (!fs::is_regular_file(csprojPath, ec)) {
			str csprojContent =
				"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
				"  <PropertyGroup>\n"
				"    <TargetFramework>net9.0</TargetFramework>\n"
				"    <ImplicitUsings>enable</ImplicitUsings>\n"
				"    <Nullable>disable</Nullable>\n"
				"    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
				"    <BaseIntermediateOutputPath>Library/Compile/CSharpIde/obj/</BaseIntermediateOutputPath>\n"
				"    <OutputPath>Library/Compile/CSharpIde/bin/</OutputPath>\n"
				"  </PropertyGroup>\n"
				"  <ItemGroup>\n"
				"    <Compile Include=\"Source/**/*.cs\" />\n"
				"    <Compile Include=\"Library/Generated/CSharpStubs/Shalltear.Stubs.cs\" />\n"
				"  </ItemGroup>\n"
				"</Project>\n";
			FileSystem::WriteTextFile(Path(csprojPath.generic_string()), csprojContent);
		}
		return true;
	}

	str GetGeneratedDirectory(const str& projectPath) {
		if (projectPath.empty()) {
			return str(GENERATED_RELATIVE_DIR);
		}
		return projectPath + "/" + GENERATED_RELATIVE_DIR;
	}

	bool HasCSharpSources(const str& projectPath) {
		if (projectPath.empty()) {
			return false;
		}
		std::error_code ec;
		fs::path sourceDir(ScriptProjectOps::GetSourceDirectory(projectPath));
		if (!fs::is_directory(sourceDir, ec)) {
			return false;
		}
		for (fs::recursive_directory_iterator it(sourceDir, fs::directory_options::skip_permission_denied, ec), end;
			 it != end && !ec; it.increment(ec)) {
			if (it->is_regular_file(ec) &&
				StringUtil::ToLower(it->path().extension().generic_string()) == ".cs") {
				return true;
			}
		}
		return false;
	}

	bool IsGeneratedOutputOutOfDate(const str& projectPath, str* outReason) {
		if (!HasCSharpSources(projectPath)) {
			return false;
		}
		std::error_code ec;
		fs::path sourceDir(ScriptProjectOps::GetSourceDirectory(projectPath));
		fs::path genDir(GetGeneratedDirectory(projectPath));
		for (fs::recursive_directory_iterator it(sourceDir, fs::directory_options::skip_permission_denied, ec), end;
			 it != end && !ec; it.increment(ec)) {
			if (!it->is_regular_file(ec) ||
				StringUtil::ToLower(it->path().extension().generic_string()) != ".cs") {
				continue;
			}
			fs::path rel = fs::relative(it->path(), sourceDir, ec);
			fs::path genCpp = genDir / rel.parent_path() / (rel.stem().generic_string() + ".generated.cpp");
			fs::path genH = genDir / rel.parent_path() / (rel.stem().generic_string() + ".generated.h");
			if (!fs::is_regular_file(genCpp, ec) || !fs::is_regular_file(genH, ec)) {
				if (outReason) {
					*outReason = std::format("'{}' has not been transpiled yet", it->path().generic_string());
				}
				return true;
			}
			std::error_code timeEc;
			auto csTime = fs::last_write_time(it->path(), timeEc);
			auto cppTime = fs::last_write_time(genCpp, timeEc);
			if (!timeEc && csTime > cppTime) {
				if (outReason) {
					*outReason = std::format("'{}' is newer than its transpiled C++ output", it->path().generic_string());
				}
				return true;
			}
		}
		return false;
	}

	bool TranspileSource(std::string_view sourcePath, std::string_view cSharpText, TranspileFileResult& out) {
		out = TranspileFileResult{};
#if defined(_WIN32)
		LoadRoslynOnce();
		RoslynState& s = GetRoslynState();
		if (!s.loaded || !s.transpileSource || !s.freeBuffer) {
			out.diagnostics.push_back(MakeDiagnostic(
				sourcePath, 1, 1, CompilerDiagnosticSeverity::Error,
				"ShalltearRoslyn.dll is not loaded (run python Scripts/fetch_prebuilt.py)"));
			return false;
		}

		str pathNullTerm(sourcePath);
		str textNullTerm(cSharpText);
		const char* jsonPtr = nullptr;
		s.transpileSource(pathNullTerm.c_str(), textNullTerm.c_str(), &jsonPtr);
		if (!jsonPtr) {
			out.diagnostics.push_back(MakeDiagnostic(
				sourcePath, 1, 1, CompilerDiagnosticSeverity::Error,
				"ShalltearRoslyn returned no output"));
			return false;
		}

		str jsonText(jsonPtr);
		s.freeBuffer(jsonPtr);

		LinearAllocator arena(std::max<size_t>(65536, jsonText.size() * 4), MemTag::Editor);
		JSONValue root = JSONParse(jsonText, arena);
		if (!root.IsObject()) {
			out.diagnostics.push_back(MakeDiagnostic(
				sourcePath, 1, 1, CompilerDiagnosticSeverity::Error,
				"ShalltearRoslyn returned unreadable output"));
			return false;
		}

		out.succeeded = root["Succeeded"].AsBool(false);
		out.headerContent = str(root["HeaderContent"].AsString());
		out.sourceContent = str(root["SourceContent"].AsString());

		const JSONValue& types = root["ReflectedTypes"];
		if (types.IsArray()) {
			for (size_t i = 0; i < types.Size(); ++i) {
				out.reflectedTypes.push_back(str(types[i].AsString()));
			}
		}

		const JSONValue& diags = root["Diagnostics"];
		if (diags.IsArray()) {
			for (size_t i = 0; i < diags.Size(); ++i) {
				out.diagnostics.push_back(ParseDiagnosticJson(diags[i]));
			}
		}

		return out.succeeded;
#else
		out.diagnostics.push_back(MakeDiagnostic(
			sourcePath, 1, 1, CompilerDiagnosticSeverity::Error,
			"C# transpilation requires ShalltearRoslyn on Windows"));
		return false;
#endif
	}

	bool TranspileProject(const str& projectPath, TranspileProjectResult& out) {
		out = TranspileProjectResult{};
		if (projectPath.empty()) {
			return false;
		}

		std::error_code ec;
		fs::path projectRoot(projectPath);
		fs::path sourceDir(ScriptProjectOps::GetSourceDirectory(projectPath));
		fs::path genDir(GetGeneratedDirectory(projectPath));

		Vector<fs::path> csFiles{GetMemoryResource(MemTag::Editor)};
		if (fs::is_directory(sourceDir, ec)) {
			for (fs::recursive_directory_iterator it(sourceDir, fs::directory_options::skip_permission_denied, ec), end;
				 it != end && !ec; it.increment(ec)) {
				if (it->is_regular_file(ec) &&
					StringUtil::ToLower(it->path().extension().generic_string()) == ".cs") {
					csFiles.push_back(it->path());
				}
			}
		}
		std::sort(csFiles.begin(), csFiles.end());
		out.sourceFileCount = static_cast<u32>(csFiles.size());

		std::unordered_set<str> expectedGeneratedFiles;

		if (!csFiles.empty()) {
			EnsureCSharpProjectFiles(projectPath);

#if defined(_WIN32)
			LoadRoslynOnce();
			RoslynState& s = GetRoslynState();
			if (!s.loaded || !s.transpileProject || !s.freeBuffer) {
				out.diagnostics.push_back(MakeDiagnostic(
					projectPath, 1, 1, CompilerDiagnosticSeverity::Error,
					"ShalltearRoslyn.dll is not loaded (run python Scripts/fetch_prebuilt.py)"));
				out.succeeded = false;
				return false;
			}

			const char* jsonPtr = nullptr;
			s.transpileProject(projectPath.c_str(), &jsonPtr);
			if (!jsonPtr) {
				out.diagnostics.push_back(MakeDiagnostic(
					projectPath, 1, 1, CompilerDiagnosticSeverity::Error,
					"ShalltearRoslyn returned no output"));
				out.succeeded = false;
				return false;
			}

			str jsonText(jsonPtr);
			s.freeBuffer(jsonPtr);

			LinearAllocator arena(std::max<size_t>(65536, jsonText.size() * 4), MemTag::Editor);
			JSONValue root = JSONParse(jsonText, arena);
			if (!root.IsObject()) {
				out.diagnostics.push_back(MakeDiagnostic(
					projectPath, 1, 1, CompilerDiagnosticSeverity::Error,
					"ShalltearRoslyn returned unreadable output"));
				out.succeeded = false;
				return false;
			}

			const JSONValue& diags = root["Diagnostics"];
			if (diags.IsArray()) {
				for (size_t i = 0; i < diags.Size(); ++i) {
					out.diagnostics.push_back(ParseDiagnosticJson(diags[i]));
				}
			}
			if (!root["Succeeded"].AsBool(false)) {
				out.succeeded = false;
				return false;
			}

			const JSONValue& files = root["Files"];
			if (!files.IsArray() || files.Size() != csFiles.size()) {
				out.diagnostics.push_back(MakeDiagnostic(
					projectPath, 1, 1, CompilerDiagnosticSeverity::Error,
					"ShalltearRoslyn returned an incomplete file list"));
				out.succeeded = false;
				return false;
			}

			std::string_view generatedStubs = root["GeneratedStubsContent"].AsString();
			if (!generatedStubs.empty()) {
				fs::path stubsPath = projectRoot / "Library" / "Generated" / "CSharpStubs" / "Shalltear.Stubs.cs";
				fs::create_directories(stubsPath.parent_path(), ec);
				if (!fs::is_regular_file(stubsPath, ec) ||
					FileSystem::ReadTextFile(Path(stubsPath.generic_string()), MemTag::Editor) != generatedStubs) {
					FileSystem::WriteTextFile(Path(stubsPath.generic_string()), str(generatedStubs));
				}
			}

			for (size_t i = 0; i < files.Size(); ++i) {
				const JSONValue& f = files[i];
				fs::path rel(str(f["RelativePath"].AsString()));
				fs::path outSubdir = genDir / rel.parent_path();
				str stem = rel.stem().generic_string();
				fs::path headerPath = outSubdir / (stem + ".generated.h");
				fs::path cppPath = outSubdir / (stem + ".generated.cpp");

				if (!WriteGeneratedFile(headerPath, str(f["HeaderContent"].AsString()), projectRoot, out) ||
					!WriteGeneratedFile(cppPath, str(f["SourceContent"].AsString()), projectRoot, out)) {
					out.succeeded = false;
					return false;
				}
				expectedGeneratedFiles.insert(fs::weakly_canonical(headerPath, ec).generic_string());
				expectedGeneratedFiles.insert(fs::weakly_canonical(cppPath, ec).generic_string());
			}
#else
			out.diagnostics.push_back(MakeDiagnostic(
				projectPath, 1, 1, CompilerDiagnosticSeverity::Error,
				"C# transpilation requires ShalltearRoslyn on Windows"));
			out.succeeded = false;
			return false;
#endif
		}

		if (!csFiles.empty() && !ProjectHasCppModuleEntry(sourceDir.generic_string())) {
			str moduleName = ScriptProjectOps::GetModuleName(projectPath);
			fs::path entryPath = genDir / (str(MODULE_ENTRY_STEM) + ".generated.cpp");
			str entryContent = "// GENERATED by Shalltear CSharpTranspiler. Do not edit manually.\n\n"
							   "#include \"Shalltear.h\"\n\n"
							   "SLT_IMPLEMENT_MODULE(" +
							   moduleName + ")\n";
			if (!WriteGeneratedFile(entryPath, entryContent, projectRoot, out)) {
				out.succeeded = false;
				return false;
			}
			expectedGeneratedFiles.insert(fs::weakly_canonical(entryPath, ec).generic_string());
		}

		if (fs::is_directory(genDir, ec)) {
			Vector<fs::path> stale{GetMemoryResource(MemTag::Editor)};
			for (fs::recursive_directory_iterator it(genDir, fs::directory_options::skip_permission_denied, ec), end;
				 it != end && !ec; it.increment(ec)) {
				if (!it->is_regular_file(ec)) {
					continue;
				}
				str canonical = fs::weakly_canonical(it->path(), ec).generic_string();
				if (!expectedGeneratedFiles.contains(canonical)) {
					stale.push_back(it->path());
				}
			}
			for (const fs::path& stalePath : stale) {
				str relative = fs::relative(stalePath, projectRoot, ec).generic_string();
				fs::remove(stalePath, ec);
				out.removed.push_back(std::move(relative));
				out.structureChanged = true;
			}
		}

		out.succeeded = true;
		return true;
	}

} // namespace Shalltear::CSharpTranspiler
