set(SHALLTEAR_CSHARP "AUTO" CACHE STRING "C# scripting: AUTO (built when the pinned .NET SDK is fetched), ON or OFF")
set_property(CACHE SHALLTEAR_CSHARP PROPERTY STRINGS "AUTO" "ON" "OFF")
set(SHALLTEAR_CSHARP_ENABLED OFF CACHE INTERNAL "")

if(SHALLTEAR_CSHARP STREQUAL "OFF")
	return()
endif()
if(NOT SHALLTEAR_CSHARP MATCHES "^(AUTO|ON)$")
	message(FATAL_ERROR "SHALLTEAR_CSHARP is '${SHALLTEAR_CSHARP}'. Expected 'AUTO', 'ON' or 'OFF'.")
endif()

set(sltPinFile "${CMAKE_CURRENT_SOURCE_DIR}/DotNetPin.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sltPinFile}")
file(READ "${sltPinFile}" sltPin)
string(JSON sltSDKVersion GET "${sltPin}" sdkVersion)
string(JSON sltRuntimeVersion GET "${sltPin}" runtimeVersion)
string(JSON sltTargetFramework GET "${sltPin}" targetFramework)

set(sltDotNetRoot "${CMAKE_SOURCE_DIR}/External/Prebuilt/dotnet/${sltSDKVersion}")
if(CMAKE_HOST_WIN32)
	set(sltDotNet "${sltDotNetRoot}/dotnet.exe")
else()
	set(sltDotNet "${sltDotNetRoot}/dotnet")
endif()

if(NOT EXISTS "${sltDotNet}")
	if(SHALLTEAR_CSHARP STREQUAL "ON")
		message(FATAL_ERROR
			"SHALLTEAR_CSHARP is ON but .NET SDK ${sltSDKVersion} was not found at:\n"
			"  ${sltDotNetRoot}\n"
			"  Run python Scripts/fetch_prebuilt.py --dotnet")
	endif()
	message(STATUS "C# scripting is off, .NET SDK ${sltSDKVersion} not fetched (run: python Scripts/fetch_prebuilt.py --dotnet)")
	return()
endif()
set(SHALLTEAR_CSHARP_ENABLED ON CACHE INTERNAL "")

set(sltManagedDir "${CMAKE_CURRENT_SOURCE_DIR}/Managed")
set(sltManagedRoot "${CMAKE_BINARY_DIR}/CSharp")
set(sltManagedOutput "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/CSharp")
set(sltManagedStamp "${sltManagedRoot}/Managed.stamp")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(sltManagedConfig "Debug")
else()
	set(sltManagedConfig "Release")
endif()

file(GLOB_RECURSE sltManagedInputs
	"${sltManagedDir}/*.cs" "${sltManagedDir}/*.csproj" "${sltManagedDir}/*.slnx"
	"${sltManagedDir}/*.props" "${sltManagedDir}/*.config" "${sltManagedDir}/packages.lock.json")

add_custom_command(
	OUTPUT "${sltManagedStamp}"
	COMMAND ${CMAKE_COMMAND} -E env
		"DOTNET_ROOT=${sltDotNetRoot}" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_MULTILEVEL_LOOKUP=0 DOTNET_CLI_UI_LANGUAGE=en-us
		"${sltDotNet}" msbuild "${sltManagedDir}/Shalltear.Managed.slnx" -restore
		-nologo -v:q -tl:off -clp:NoSummary -warnaserror -nodeReuse:false
		-p:Configuration=${sltManagedConfig} -p:UseSharedCompilation=false -p:RestoreLockedMode=true
		"-p:ShalltearManagedRoot=${sltManagedRoot}/" "-p:ShalltearManagedOutput=${sltManagedOutput}/"
	COMMAND ${CMAKE_COMMAND} -E touch "${sltManagedStamp}"
	DEPENDS ${sltManagedInputs} "${sltPinFile}"
	COMMENT "Building C# assemblies"
	VERBATIM)
add_custom_target(CSharpManaged DEPENDS "${sltManagedStamp}")

set(sltNativeDir "${sltManagedOutput}/native")
set(sltWebMonoFiles
	"${sltNativeDir}/WebMonoTables.c"
	"${sltNativeDir}/WebMonoBootstrap.cpp")
add_custom_command(
	OUTPUT ${sltWebMonoFiles}
	COMMAND ${CMAKE_COMMAND} -E make_directory "${sltNativeDir}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Native/WebMonoTables.c" "${sltNativeDir}/WebMonoTables.c"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Native/WebMonoBootstrap.cpp" "${sltNativeDir}/WebMonoBootstrap.cpp"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Native/WebMonoTables.c" "${CMAKE_CURRENT_SOURCE_DIR}/Native/WebMonoBootstrap.cpp"
	COMMENT "Staging WebAssembly C# runtime bridge"
	VERBATIM)
add_custom_target(CSharpWebNative DEPENDS ${sltWebMonoFiles})

if(WIN32)
	get_filename_component(sltToolchainBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
	get_filename_component(sltToolchainRoot "${sltToolchainBin}" DIRECTORY)
	set(sltShimSource "${CMAKE_CURRENT_SOURCE_DIR}/Native/CRTShim.cpp")
	set(sltShimGS "${CMAKE_CURRENT_SOURCE_DIR}/Native/Win64GS.inl")
	set(sltShim "${sltNativeDir}/CRTShim.obj")
	set(sltShimStatic "${sltNativeDir}/CRTShimStatic.obj")
	add_custom_command(
		OUTPUT "${sltShim}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${sltNativeDir}"
		COMMAND "${sltToolchainBin}/clang++" --target=x86_64-pc-windows-msvc -std=c++20 -O2 -fno-exceptions -fno-rtti
			-fms-extensions -fno-stack-protector -fno-builtin -c "${sltShimSource}" -o "${sltShim}"
		DEPENDS "${sltShimSource}" "${sltShimGS}"
		COMMENT "Compiling C# link shim"
		VERBATIM)
	add_custom_command(
		OUTPUT "${sltShimStatic}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${sltNativeDir}"
		COMMAND "${CMAKE_CXX_COMPILER}" -DSLT_AOT_STATIC_BRIDGE -std=c++20 -O2 -Wall -Wextra -Werror
			-fno-stack-protector -c "${sltShimSource}" -o "${sltShimStatic}"
		DEPENDS "${sltShimSource}" "${sltShimGS}"
		COMMENT "Compiling C# static link shim"
		VERBATIM)

	file(GLOB sltBuiltins "${sltToolchainRoot}/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a")
	if(NOT sltBuiltins)
		message(FATAL_ERROR
			"libclang_rt.builtins-x86_64.a was not found under ${sltToolchainRoot}/lib/clang. "
			"Run python Scripts/fetch_prebuilt.py --toolchain")
	endif()
	list(GET sltBuiltins 0 sltBuiltins)
	set(sltNativeLibs "${sltNativeDir}/libs/clang_rt.builtins-x86_64.lib")
	set(sltNativeLibCopies COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sltBuiltins}" "${sltNativeLibs}")
	foreach(sltName advapi32 bcrypt crypt32 iphlpapi kernel32 mswsock ncrypt normaliz ntdll ole32 oleaut32 secur32 user32 version ws2_32 ucrt)
		list(APPEND sltNativeLibs "${sltNativeDir}/libs/${sltName}.lib")
		list(APPEND sltNativeLibCopies COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${sltToolchainRoot}/x86_64-w64-mingw32/lib/lib${sltName}.a" "${sltNativeDir}/libs/${sltName}.lib")
	endforeach()
	add_custom_command(
		OUTPUT ${sltNativeLibs}
		COMMAND ${CMAKE_COMMAND} -E make_directory "${sltNativeDir}/libs"
		${sltNativeLibCopies}
		COMMENT "Staging import libs for C# links"
		VERBATIM)
	add_custom_target(CSharpNative DEPENDS "${sltShim}" "${sltShimStatic}" ${sltNativeLibs})

	set(sltAOTRid "win-x64")
	set(sltAOTSuffix ".dll")
	set(sltAOTArguments "-p:ShalltearLinkerDir=${sltToolchainBin}/" "-p:ShalltearNativeDir=${sltNativeDir}/")
	set(sltAOTDepends "${sltShim}" ${sltNativeLibs})
	set(sltAOTEnvironment "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	get_filename_component(sltToolchainBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
	set(sltAOTRid "linux-x64")
	set(sltAOTSuffix ".so")
	set(sltAOTArguments "-p:ShalltearLinuxSysRoot=${SHALLTEAR_LINUX_PACK}/sysroot")
	set(sltAOTDepends "")
	# ILCompiler finds clang/lld/llvm-objcopy on PATH
	set(sltAOTEnvironment --modify "PATH=path_list_prepend:${sltToolchainBin}")
endif()

if(SHALLTEAR_BUILD_TESTS AND NOT SHALLTEAR_BUILD_SHARED AND sltAOTRid)
	set(sltAOTProject "${sltManagedDir}/Shalltear.ConformanceAOT/Shalltear.ConformanceAOT.csproj")
	set(sltAOTLibrary "${sltManagedOutput}/aot/Shalltear.ConformanceAOT${sltAOTSuffix}")
	set(sltAOTOutputs "${sltAOTLibrary}")
	set(sltAOTCopies "")
	if(WIN32)
		set(sltAOTInputs "${sltManagedOutput}/aot/Shalltear.ConformanceAOT.rsp")
		set(sltAOTMap "${sltManagedOutput}/aot/Shalltear.ConformanceAOT.map")
		set(sltAOTNative "${sltManagedRoot}/obj/${sltSDKVersion}/Shalltear.ConformanceAOT/${sltManagedConfig}/${sltTargetFramework}/win-x64/native")
		list(APPEND sltAOTOutputs "${sltAOTInputs}" "${sltAOTMap}")
		set(sltAOTCopies
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sltAOTNative}/link.rsp" "${sltAOTInputs}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sltAOTNative}/Shalltear.ConformanceAOT.map" "${sltAOTMap}")
	endif()
	add_custom_command(
		OUTPUT ${sltAOTOutputs}
		COMMAND ${CMAKE_COMMAND} -E env ${sltAOTEnvironment}
			"DOTNET_ROOT=${sltDotNetRoot}" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_CLI_UI_LANGUAGE=en-us
			"${sltDotNet}" msbuild "${sltAOTProject}" -restore -t:Publish
			-nologo -v:q -tl:off -clp:NoSummary -warnaserror -nodeReuse:false
			-p:Configuration=${sltManagedConfig} -p:UseSharedCompilation=false -p:RuntimeIdentifier=${sltAOTRid}
			"-p:ShalltearManagedRoot=${sltManagedRoot}/" "-p:ShalltearManagedOutput=${sltManagedOutput}/"
			${sltAOTArguments}
			"-p:PublishDir=${sltManagedRoot}/aot-publish/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${sltManagedRoot}/aot-publish/Shalltear.ConformanceAOT${sltAOTSuffix}" "${sltAOTLibrary}"
		${sltAOTCopies}
		DEPENDS "${sltManagedStamp}" ${sltAOTDepends} "${sltAOTProject}"
			"${CMAKE_SOURCE_DIR}/EngineAssets/CSharp/Shalltear.CSharp.targets"
		COMMENT "Publishing C# conformance suite as a native lib"
		VERBATIM)

	set(sltAudioAOTProject "${sltManagedDir}/Shalltear.ConformanceAudioAOT/Shalltear.ConformanceAudioAOT.csproj")
	set(sltAudioAOTLibrary "${sltManagedOutput}/aot/Shalltear.ConformanceAudioAOT${sltAOTSuffix}")
	# after 1st publish (same OutDir)
	add_custom_command(
		OUTPUT "${sltAudioAOTLibrary}"
		COMMAND ${CMAKE_COMMAND} -E env ${sltAOTEnvironment}
			"DOTNET_ROOT=${sltDotNetRoot}" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_CLI_UI_LANGUAGE=en-us
			"${sltDotNet}" msbuild "${sltAudioAOTProject}" -restore -t:Publish
			-nologo -v:q -tl:off -clp:NoSummary -warnaserror -nodeReuse:false
			-p:Configuration=${sltManagedConfig} -p:UseSharedCompilation=false -p:RuntimeIdentifier=${sltAOTRid}
			"-p:ShalltearManagedRoot=${sltManagedRoot}/" "-p:ShalltearManagedOutput=${sltManagedOutput}/"
			${sltAOTArguments}
			"-p:PublishDir=${sltManagedRoot}/aot-publish-audio/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${sltManagedRoot}/aot-publish-audio/Shalltear.ConformanceAudioAOT${sltAOTSuffix}" "${sltAudioAOTLibrary}"
		DEPENDS "${sltAOTLibrary}" "${sltManagedStamp}" ${sltAOTDepends} "${sltAudioAOTProject}"
			"${CMAKE_SOURCE_DIR}/EngineAssets/CSharp/Shalltear.CSharp.targets"
		COMMENT "Publishing C# audio conformance suite as a native lib"
		VERBATIM)
	add_custom_target(CSharpConformanceAOT DEPENDS "${sltAOTLibrary}" "${sltAudioAOTLibrary}")
endif()

file(GLOB_RECURSE CSHARP_SOURCES "Private/*.cpp" "Private/*.h")
shalltear_select_platform_sources(CSHARP_SOURCES)

shalltear_add_package(CSharp SOURCES ${CSHARP_SOURCES})

target_include_directories(CSharp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)
target_compile_definitions(CSharp PRIVATE SHALLTEAR_CSHARP_BUILD)
add_dependencies(CSharp CSharpManaged CSharpWebNative)
if(TARGET CSharpNative)
	add_dependencies(CSharp CSharpNative)
endif()
if(NOT WIN32 AND NOT EMSCRIPTEN)
	target_link_libraries(CSharp PRIVATE ${CMAKE_DL_LIBS})
endif()
if(SHALLTEAR_BUILD_TESTS)
	target_compile_definitions(CSharp PRIVATE SLT_CSHARP_TEST_HOOKS)
endif()

if(SHALLTEAR_BUILD_SHARED)
	file(GLOB_RECURSE CSHARP_EDITOR_SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/../CSharpEditor/Private/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/../CSharpEditor/Private/*.h")
	shalltear_select_platform_sources(CSHARP_EDITOR_SOURCES)
	if(NOT CSHARP_EDITOR_SOURCES)
		message(FATAL_ERROR "Source/CSharpEditor/Private has no sources (package's editor half is missing).")
	endif()
	target_sources(CSharp PRIVATE ${CSHARP_EDITOR_SOURCES})
	target_link_libraries(CSharp PRIVATE ShalltearEditor)
	target_compile_definitions(CSharp PRIVATE SLT_WITH_EDITOR)
	install(DIRECTORY "${sltManagedOutput}/" DESTINATION ${SHALLTEAR_INSTALL_BIN}/CSharp)

	if(WIN32)
		set(sltHostRid "win-x64")
	else()
		set(sltHostRid "linux-x64")
	endif()
	string(JSON sltArchiveURL GET "${sltPin}" sdk ${sltHostRid} url)
	string(JSON sltArchiveHash GET "${sltPin}" sdk ${sltHostRid} sha512)
	string(JSON sltPackCount LENGTH "${sltPin}" packs)
	set(sltPackRows "")
	if(sltPackCount GREATER 0)
		math(EXPR sltPackLast "${sltPackCount} - 1")
		foreach(sltPackIndex RANGE ${sltPackLast})
			string(JSON sltPackName MEMBER "${sltPin}" packs ${sltPackIndex})
			string(JSON sltPackHash GET "${sltPin}" packs "${sltPackName}")
			string(APPEND sltPackRows "\t\t{\"${sltPackName}\", \"${sltPackHash}\"},\n")
		endforeach()
	endif()
	file(CONFIGURE OUTPUT "${sltManagedRoot}/Generated/DotNetPin.generated.h" CONTENT
"// GENERATED by Source/CSharp/CMakeLists.txt from DotNetPin.json. Do not edit manually.
#pragma once

namespace Shalltear::CSharpEditor::DotNetPinData {

	struct Pack {
		const char* name;
		const char* sha512;
	};

	inline constexpr const char* SDK_VERSION = \"${sltSDKVersion}\";
	inline constexpr const char* RUNTIME_VERSION = \"${sltRuntimeVersion}\";
	inline constexpr const char* TARGET_FRAMEWORK = \"${sltTargetFramework}\";
	inline constexpr const char* HOST_RID = \"${sltHostRid}\";
	inline constexpr const char* ARCHIVE_URL = \"${sltArchiveURL}\";
	inline constexpr const char* ARCHIVE_SHA512 = \"${sltArchiveHash}\";
	inline constexpr Pack Packs[] = {
${sltPackRows}	};

} // namespace Shalltear::CSharpEditor::DotNetPinData
" @ONLY)

	target_include_directories(CSharp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../CSharpEditor/Private "${sltManagedRoot}/Generated")
endif()
