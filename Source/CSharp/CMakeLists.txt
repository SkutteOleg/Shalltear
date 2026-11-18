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
		"DOTNET_ROOT=${sltDotNetRoot}" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_CLI_UI_LANGUAGE=en-us
		"${sltDotNet}" msbuild "${sltManagedDir}/Shalltear.Managed.slnx" -restore
		-nologo -v:q -tl:off -clp:NoSummary -warnaserror -nodeReuse:false
		-p:Configuration=${sltManagedConfig} -p:UseSharedCompilation=false -p:RestoreLockedMode=true
		"-p:ShalltearManagedRoot=${sltManagedRoot}/" "-p:ShalltearManagedOutput=${sltManagedOutput}/"
	COMMAND ${CMAKE_COMMAND} -E touch "${sltManagedStamp}"
	DEPENDS ${sltManagedInputs} "${sltPinFile}"
	COMMENT "Building C# assemblies"
	VERBATIM)
add_custom_target(CSharpManaged DEPENDS "${sltManagedStamp}")

file(GLOB_RECURSE CSHARP_SOURCES "Private/*.cpp" "Private/*.h")
shalltear_select_platform_sources(CSHARP_SOURCES)

shalltear_add_package(CSharp SOURCES ${CSHARP_SOURCES})

target_include_directories(CSharp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)
add_dependencies(CSharp CSharpManaged)
if(NOT WIN32)
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
	target_compile_definitions(CSharp PRIVATE
		SLT_WITH_EDITOR
		SLT_DOTNET_SDK_VERSION="${sltSDKVersion}"
		SLT_DOTNET_RUNTIME_VERSION="${sltRuntimeVersion}")

	target_include_directories(CSharp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../CSharpEditor/Private)
endif()
