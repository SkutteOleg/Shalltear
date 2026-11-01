cmake_minimum_required(VERSION 3.20)
project(ShalltearEngine VERSION 0.0.0)

# install destinations are normalized from CMake 3.31
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SHALLTEAR_BUILD_SHARED "Build engine modules as shared libs (.dll)" ON)

option(SHALLTEAR_BUILD_TESTS "Build Shalltear unit tests" ON)

option(SHALLTEAR_BUILD_BENCHMARKS "Build Shalltear micro-benchmarks" ON)

option(SHALLTEAR_DISABLE_PCH "Disable precompiled headers" OFF)

# warnings = errors over Clang's default set only
option(SHALLTEAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (engine code only)" OFF)
if(SHALLTEAR_WARNINGS_AS_ERRORS)
    if(CMAKE_VERSION VERSION_LESS 3.24)
        message(FATAL_ERROR
            "Shalltear: SHALLTEAR_WARNINGS_AS_ERRORS needs CMake 3.24. "
            "This is ${CMAKE_VERSION}. Scripts/fetch_prebuilt.py "
            "stages a newer one.")
    endif()
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
endif()

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE SHALLTEAR_COMPILER_TARGET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE slt_dumpmachineResult)
if(NOT slt_dumpmachineResult EQUAL 0 OR NOT SHALLTEAR_COMPILER_TARGET)
    message(FATAL_ERROR
        "Shalltear: ${CMAKE_CXX_COMPILER} does not answer -dumpmachine. "
        "The engine is Clang-only.")
endif()

if(MINGW)
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
    set(CMAKE_SHARED_MODULE_PREFIX "")
    set(CMAKE_IMPORT_LIBRARY_PREFIX "")
    set(CMAKE_STATIC_LIBRARY_PREFIX "")

    # nothing on Windows reads DWARF
    add_compile_options($<$<NOT:$<CONFIG:Release>>:-gcodeview>)
    add_link_options($<$<NOT:$<CONFIG:Release>>:-Wl,--pdb=>)

    add_link_options($<$<CONFIG:Release>:-Wl,-s>)

    set(SHALLTEAR_CXX_RUNTIME_DLLS "")
    foreach(slt_runtime libc++ libunwind)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${slt_runtime}.dll.a
            OUTPUT_VARIABLE slt_importLib
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        get_filename_component(slt_libDir "${slt_importLib}" DIRECTORY)
        get_filename_component(slt_sysroot "${slt_libDir}" DIRECTORY)
        set(slt_runtimeDll "${slt_sysroot}/bin/${slt_runtime}.dll")
        if(NOT EXISTS "${slt_runtimeDll}")
            message(FATAL_ERROR
                "Shalltear: ${slt_runtime}.dll was not found at ${slt_runtimeDll}. "
                "Is ${CMAKE_CXX_COMPILER} a complete "
                "llvm-mingw toolchain?")
        endif()
        list(APPEND SHALLTEAR_CXX_RUNTIME_DLLS "${slt_runtimeDll}")
    endforeach()
endif()

if(SHALLTEAR_BUILD_SHARED)
    set(SHALLTEAR_LIB_TYPE SHARED)
    add_compile_definitions(SHALLTEAR_SHARED)
else()
    set(SHALLTEAR_LIB_TYPE STATIC)
endif()

# only Release defines for RelWithDebInfo to keep asserts
add_compile_definitions($<$<CONFIG:Release>:SHALLTEAR_CONFIG_RELEASE>)

add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/= -fmacro-prefix-map=${CMAKE_BINARY_DIR}/=)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# included, not installed because the engine declares packages
include(${CMAKE_SOURCE_DIR}/CMake/ShalltearPackage.cmake)

# set before subdirectories because packages install their headers
set(SHALLTEAR_INSTALL_BIN .)
set(SHALLTEAR_INSTALL_LIB Lib)
set(SHALLTEAR_INSTALL_INCLUDE Include)

add_subdirectory(Source/Core)
# after Core which it links, before the editor that loads it
add_subdirectory(Source/ImportPipeline)
# configured in every tree -> linked only when enabled
add_subdirectory(Source/Physics3D)
add_subdirectory(Source/Animation)
# packages load at project open
add_subdirectory(Source/PostProcessing)
# packages load at project open
add_subdirectory(Source/Audio)
# packages load at project open
add_subdirectory(Source/VisualEffects)
add_subdirectory(Source/Graphs)
add_subdirectory(Source/NodeShader)
add_subdirectory(Source/Editor)
add_subdirectory(Source/EditorExecutable)
add_subdirectory(Source/PlayerExecutable)

if(SHALLTEAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Source/SampleModule)
    add_subdirectory(Source/HotReloadModule)
    # after Physics3D
    add_subdirectory(Source/PhysicsListenerModule)
    # its CMakeLists returns early
    add_subdirectory(Source/PhysicsScriptModule)
    # shared trees only
    add_subdirectory(Source/AudioScriptModule)
    # shared trees only
    add_subdirectory(Source/VisualEffectsScriptModule)
    add_subdirectory(Source/SamplePackage)
    add_subdirectory(Source/SampleDependentPackage)
    add_subdirectory(Source/Tests)
endif()

if(SHALLTEAR_BUILD_BENCHMARKS)
    add_subdirectory(Source/Benchmarks)
endif()

function(shalltear_forbid_shared_pch dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(targetType ${target} TYPE)
        if(NOT targetType STREQUAL "INTERFACE_LIBRARY" AND NOT targetType STREQUAL "UTILITY")
            get_target_property(reuseFrom ${target} PRECOMPILE_HEADERS_REUSE_FROM)
            if(reuseFrom)
                message(FATAL_ERROR
                    "target '${target}' reuses the precompiled header of '${reuseFrom}'.\n"
                    "  Give '${target}' own PCH (or none).")
            endif()
        endif()
    endforeach()

    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        shalltear_forbid_shared_pch("${subdir}")
    endforeach()
endfunction()

shalltear_forbid_shared_pch("${CMAKE_SOURCE_DIR}")

# every public header compiles alone (none relies on PCH)
set(SHALLTEAR_HeaderCheck_TUS "")
set(SHALLTEAR_StrippedHeaderCheck_TUS "")
set(SHALLTEAR_PUBLIC_HEADER_FILES "")

get_property(SHALLTEAR_PACKAGE_PUBLIC_DIR_LIST GLOBAL PROPERTY SHALLTEAR_PACKAGE_PUBLIC_DIRS)
set(SHALLTEAR_PUBLIC_DIRS
    ${CMAKE_SOURCE_DIR}/Source/Core/Public
    ${CMAKE_SOURCE_DIR}/Source/Editor/Public
    ${SHALLTEAR_PACKAGE_PUBLIC_DIR_LIST})

foreach(module_public_dir ${SHALLTEAR_PUBLIC_DIRS})
    file(GLOB_RECURSE module_headers RELATIVE ${module_public_dir} ${module_public_dir}/*.h)
    foreach(header ${module_headers})
        list(APPEND SHALLTEAR_PUBLIC_HEADER_FILES ${module_public_dir}/${header})
        string(REPLACE "/" "_" tu_name ${header})
        string(REGEX REPLACE "\\.h$" ".cpp" tu_name ${tu_name})
        set(tu_content "#include \"${header}\"\n")
        foreach(flavor HeaderCheck StrippedHeaderCheck)
            set(tu_path ${CMAKE_BINARY_DIR}/${flavor}/${tu_name})
            # rewrite changed content only to avoid dirtying the target
            if(EXISTS ${tu_path})
                file(READ ${tu_path} tu_existing)
            else()
                set(tu_existing "")
            endif()
            if(NOT tu_existing STREQUAL tu_content)
                file(WRITE ${tu_path} ${tu_content})
            endif()
            list(APPEND SHALLTEAR_${flavor}_TUS ${tu_path})
        endforeach()
    endforeach()
endforeach()

add_library(ShalltearHeaderCheck STATIC EXCLUDE_FROM_ALL ${SHALLTEAR_HeaderCheck_TUS})
target_include_directories(ShalltearHeaderCheck PRIVATE
    ${SHALLTEAR_PUBLIC_DIRS}
    ${SHALLTEAR_GENERATED_PUBLIC_DIR}
)

# stripper bug should fail the build
find_package(Python3 COMPONENTS Interpreter QUIET)
set(SHALLTEAR_STRIP_PYTHON "${Python3_EXECUTABLE}")

if(SHALLTEAR_STRIP_PYTHON)
    set(SHALLTEAR_STRIPPED_PUBLIC_DIR ${CMAKE_BINARY_DIR}/StrippedPublic)
    set(SHALLTEAR_STRIP_SCRIPT ${CMAKE_SOURCE_DIR}/Scripts/strip_sdk_source.py)

    # script only rewrites changed files
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/StrippedPublic.stamp
        COMMAND ${SHALLTEAR_STRIP_PYTHON} ${SHALLTEAR_STRIP_SCRIPT} --self-test
        COMMAND ${SHALLTEAR_STRIP_PYTHON} ${SHALLTEAR_STRIP_SCRIPT}
            --out ${SHALLTEAR_STRIPPED_PUBLIC_DIR}
            ${SHALLTEAR_PUBLIC_DIRS}
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/StrippedPublic.stamp
        DEPENDS ${SHALLTEAR_STRIP_SCRIPT} ${SHALLTEAR_PUBLIC_HEADER_FILES}
        COMMENT "Stripping public headers for packaged-source guard"
        VERBATIM)
    add_custom_target(ShalltearStripPublicHeaders DEPENDS ${CMAKE_BINARY_DIR}/StrippedPublic.stamp)

    add_library(ShalltearStrippedHeaderCheck STATIC EXCLUDE_FROM_ALL ${SHALLTEAR_StrippedHeaderCheck_TUS})
    add_dependencies(ShalltearStrippedHeaderCheck ShalltearStripPublicHeaders)
    # add_dependencies only orders and ninja plans mtimes before stripping
    set_source_files_properties(${SHALLTEAR_StrippedHeaderCheck_TUS} PROPERTIES
        OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/StrippedPublic.stamp)
    target_include_directories(ShalltearStrippedHeaderCheck PRIVATE
        ${SHALLTEAR_STRIPPED_PUBLIC_DIR}
        ${SHALLTEAR_GENERATED_PUBLIC_DIR}
    )

    if(SHALLTEAR_BUILD_TESTS)
        add_test(NAME SdkSourceStrip.SelfTest
            COMMAND ${SHALLTEAR_STRIP_PYTHON} ${SHALLTEAR_STRIP_SCRIPT} --self-test)
    endif()
else()
    message(STATUS "no Python 3 interpreter found: ShalltearStrippedHeaderCheck is unavailable, `cmake --install` will refuse to package")
endif()

# makes find_package(Shalltear) work + the build tree a valid SDK
include(CMakePackageConfigHelpers)

# set above add_subdirectory for packages to install headers before transform
set(SHALLTEAR_INSTALL_CMAKE Lib/cmake/Shalltear)
set(SHALLTEAR_INSTALL_PLAYER_SOURCE PlayerSource)

set(SHALLTEAR_MODULE_ABI_IDENTITY "${PROJECT_VERSION}|${SHALLTEAR_GIT_COMMIT}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${SHALLTEAR_COMPILER_TARGET}")
if(SHALLTEAR_BUILD_SHARED)
    set(SHALLTEAR_LINK_MODE "SHARED")
else()
    set(SHALLTEAR_LINK_MODE "STATIC")
endif()

# consumers should see only the published names
set_target_properties(ShalltearCore PROPERTIES EXPORT_NAME Core)
set_target_properties(ShalltearEditor PROPERTIES EXPORT_NAME Editor)

set(SHALLTEAR_EXPORTED_TARGETS ShalltearCore ShalltearEditor)

get_property(SHALLTEAR_PACKAGE_ID_LIST GLOBAL PROPERTY SHALLTEAR_PACKAGE_IDS)
get_property(SHALLTEAR_PACKAGE_TARGET_LIST GLOBAL PROPERTY SHALLTEAR_PACKAGE_TARGETS)
get_property(SHALLTEAR_PACKAGE_MANIFEST_LIST GLOBAL PROPERTY SHALLTEAR_PACKAGE_MANIFESTS)
if(SHALLTEAR_PACKAGE_TARGET_LIST)
    list(APPEND SHALLTEAR_EXPORTED_TARGETS ${SHALLTEAR_PACKAGE_TARGET_LIST})
endif()

set(SHALLTEAR_EDITOR_LINKED_DECLARATIONS "")
set(SHALLTEAR_EDITOR_LINKED_ROWS "")
if(NOT SHALLTEAR_BUILD_SHARED AND SHALLTEAR_PACKAGE_TARGET_LIST)
    list(LENGTH SHALLTEAR_PACKAGE_TARGET_LIST slt_linkedPackageCount)
    math(EXPR slt_linkedPackageLast "${slt_linkedPackageCount} - 1")
    foreach(slt_index RANGE ${slt_linkedPackageLast})
        list(GET SHALLTEAR_PACKAGE_ID_LIST ${slt_index} slt_packageId)
        list(GET SHALLTEAR_PACKAGE_TARGET_LIST ${slt_index} slt_packageTarget)
        string(APPEND SHALLTEAR_EDITOR_LINKED_DECLARATIONS "SLT_DECLARE_LINKED_PACKAGE(${slt_packageTarget})\n")
        string(APPEND SHALLTEAR_EDITOR_LINKED_ROWS
            "\t\t{\"${slt_packageId}\", &SLT_DETAIL_PACKAGE_SYMBOL(${slt_packageTarget})},\n")
    endforeach()
    target_link_libraries(ShalltearEditor PRIVATE ${SHALLTEAR_PACKAGE_TARGET_LIST})
endif()

# shared editor maps package DLLs at runtime
if(SHALLTEAR_BUILD_SHARED AND SHALLTEAR_PACKAGE_TARGET_LIST)
    add_dependencies(ShalltearEditorExecutable ${SHALLTEAR_PACKAGE_TARGET_LIST})
endif()

set(SHALLTEAR_EDITOR_LINKED_FILE ${CMAKE_BINARY_DIR}/ShalltearGenerated/EditorLinkedPackages.generated.cpp)
file(GENERATE OUTPUT ${SHALLTEAR_EDITOR_LINKED_FILE} CONTENT
"// GENERATED by the engine's build. Do not edit manually.

#include \"PackageHost.h\"

#include \"Shalltear/Core/Module/ModuleAPI.h\"

${SHALLTEAR_EDITOR_LINKED_DECLARATIONS}
namespace Shalltear {

	const EditorLinkedPackage* GetEditorLinkedPackages(size_t& outCount) {
		static const EditorLinkedPackage TABLE[] = {
${SHALLTEAR_EDITOR_LINKED_ROWS}\t\t{nullptr, nullptr}
		};
		outCount = (sizeof(TABLE) / sizeof(TABLE[0])) - 1;
		return TABLE;
	}

} // namespace Shalltear
")
target_sources(ShalltearEditor PRIVATE ${SHALLTEAR_EDITOR_LINKED_FILE})

if(NOT SHALLTEAR_BUILD_SHARED)
    # exported for "linking only" because freetype headers never public
    list(APPEND SHALLTEAR_EXPORTED_TARGETS freetype)
endif()

install(TARGETS ${SHALLTEAR_EXPORTED_TARGETS}
    EXPORT ShalltearTargets
    RUNTIME DESTINATION ${SHALLTEAR_INSTALL_BIN}
    LIBRARY DESTINATION ${SHALLTEAR_INSTALL_BIN}
    ARCHIVE DESTINATION ${SHALLTEAR_INSTALL_LIB}
    INCLUDES DESTINATION ${SHALLTEAR_INSTALL_INCLUDE}
)

if(SHALLTEAR_BUILD_SHARED)
    install(TARGETS ShalltearEditorExecutable RUNTIME DESTINATION ${SHALLTEAR_INSTALL_BIN})
endif()

install(DIRECTORY Source/Core/Public/ DESTINATION ${SHALLTEAR_INSTALL_INCLUDE} FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Source/Editor/Public/ DESTINATION ${SHALLTEAR_INSTALL_INCLUDE} FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${SHALLTEAR_GENERATED_PUBLIC_DIR}/ DESTINATION ${SHALLTEAR_INSTALL_INCLUDE} FILES_MATCHING PATTERN "*.h")

install(DIRECTORY EngineAssets DESTINATION ${SHALLTEAR_INSTALL_BIN})

if(EXISTS ${CMAKE_SOURCE_DIR}/External/Prebuilt/dxc/bin/x64/dxcompiler.dll)
    install(FILES ${CMAKE_SOURCE_DIR}/External/Prebuilt/dxc/bin/x64/dxcompiler.dll
        DESTINATION ${SHALLTEAR_INSTALL_BIN})
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/External/Prebuilt/roslyn/bin/ShalltearRoslyn.dll)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/External/Prebuilt/roslyn/bin/
        DESTINATION ${SHALLTEAR_INSTALL_BIN}/Roslyn)
    install(FILES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Roslyn/Shalltear.EngineStubs.json
        DESTINATION ${SHALLTEAR_INSTALL_BIN}/Roslyn
        OPTIONAL)
endif()

if(SHALLTEAR_CXX_RUNTIME_DLLS)
    install(FILES ${SHALLTEAR_CXX_RUNTIME_DLLS} DESTINATION ${SHALLTEAR_INSTALL_BIN})
endif()

install(EXPORT ShalltearTargets
    FILE ShalltearTargets.cmake
    NAMESPACE Shalltear::
    DESTINATION ${SHALLTEAR_INSTALL_CMAKE}
)

install(FILES
    ${CMAKE_SOURCE_DIR}/CMake/ShalltearModule.cmake
    ${CMAKE_SOURCE_DIR}/CMake/ShalltearPlayer.cmake
    ${CMAKE_SOURCE_DIR}/CMake/ShalltearToolchainGuard.cmake
    ${CMAKE_SOURCE_DIR}/CMake/BuildStamp.h.in
    DESTINATION ${SHALLTEAR_INSTALL_CMAKE}
)

install(DIRECTORY Source/PlayerExecutable/Private/
    DESTINATION ${SHALLTEAR_INSTALL_PLAYER_SOURCE}
    FILES_MATCHING PATTERN "*.cpp" PATTERN "*.h")

set(SHALLTEAR_PACKAGE_IMPORTED_LIST "")
foreach(packageTarget ${SHALLTEAR_PACKAGE_TARGET_LIST})
    list(APPEND SHALLTEAR_PACKAGE_IMPORTED_LIST "Shalltear::${packageTarget}")
endforeach()

set(SHALLTEAR_PACKAGES_CONTENT "# generated by Shalltear's build. Do not edit manually.\n")
string(APPEND SHALLTEAR_PACKAGES_CONTENT "# manifest id/imported target/anchor identifier/index for index.\n")
string(APPEND SHALLTEAR_PACKAGES_CONTENT "set(SHALLTEAR_PACKAGE_IDS \"${SHALLTEAR_PACKAGE_ID_LIST}\")\n")
string(APPEND SHALLTEAR_PACKAGES_CONTENT "set(SHALLTEAR_PACKAGE_TARGETS \"${SHALLTEAR_PACKAGE_IMPORTED_LIST}\")\n")
string(APPEND SHALLTEAR_PACKAGES_CONTENT "set(SHALLTEAR_PACKAGE_ANCHORS \"${SHALLTEAR_PACKAGE_TARGET_LIST}\")\n")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearPackages.cmake CONTENT "${SHALLTEAR_PACKAGES_CONTENT}")

install(FILES ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearPackages.cmake DESTINATION ${SHALLTEAR_INSTALL_CMAKE})

foreach(packageIndex RANGE 0 255)
    list(LENGTH SHALLTEAR_PACKAGE_TARGET_LIST packageCount)
    if(packageIndex GREATER_EQUAL packageCount)
        break()
    endif()
    list(GET SHALLTEAR_PACKAGE_TARGET_LIST ${packageIndex} packageTarget)
    list(GET SHALLTEAR_PACKAGE_MANIFEST_LIST ${packageIndex} packageManifest)
    install(FILES ${packageManifest} DESTINATION ${SHALLTEAR_INSTALL_BIN}/Packages/${packageTarget})
endforeach()

write_basic_package_version_file(
    ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

set(SHALLTEAR_PACKAGE_ROOT_RELATIVE "../../..")
set(SHALLTEAR_CONFIG_INCLUDE_DIR "\${SHALLTEAR_PACKAGE_ROOT}/${SHALLTEAR_INSTALL_INCLUDE}")
if(SHALLTEAR_INSTALL_BIN STREQUAL ".")
    set(SHALLTEAR_CONFIG_RUNTIME_DIR "\${SHALLTEAR_PACKAGE_ROOT}")
else()
    set(SHALLTEAR_CONFIG_RUNTIME_DIR "\${SHALLTEAR_PACKAGE_ROOT}/${SHALLTEAR_INSTALL_BIN}")
endif()
set(SHALLTEAR_CONFIG_PLAYER_SOURCE_DIR "\${SHALLTEAR_PACKAGE_ROOT}/${SHALLTEAR_INSTALL_PLAYER_SOURCE}")
set(SHALLTEAR_CONFIG_ENGINE_SOURCE_DIR "")
set(SHALLTEAR_CONFIG_COMPILER_PATH "")
configure_file(
    ${CMAKE_SOURCE_DIR}/CMake/ShalltearConfig.cmake.in
    ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearConfig.cmake
    @ONLY
)

install(FILES
    ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearConfig.cmake
    ${CMAKE_BINARY_DIR}/CMakePackage/ShalltearConfigVersion.cmake
    DESTINATION ${SHALLTEAR_INSTALL_CMAKE}
)

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE.txt DESTINATION ${SHALLTEAR_INSTALL_BIN})

install(FILES
    ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt
    ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES-Runtime.txt
    DESTINATION ${SHALLTEAR_INSTALL_BIN}
)

configure_file(
    ${CMAKE_SOURCE_DIR}/CMake/StripSdkSource.cmake.in
    ${CMAKE_BINARY_DIR}/CMakePackage/StripSdkSource.cmake
    @ONLY
)
install(SCRIPT ${CMAKE_BINARY_DIR}/CMakePackage/StripSdkSource.cmake)

# the build tree package so a dev checkout is valid
export(EXPORT ShalltearTargets
    FILE ${CMAKE_BINARY_DIR}/ShalltearTargets.cmake
    NAMESPACE Shalltear::
)
configure_file(${CMAKE_SOURCE_DIR}/CMake/ShalltearModule.cmake ${CMAKE_BINARY_DIR}/ShalltearModule.cmake COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMake/ShalltearPlayer.cmake ${CMAKE_BINARY_DIR}/ShalltearPlayer.cmake COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMake/ShalltearToolchainGuard.cmake ${CMAKE_BINARY_DIR}/ShalltearToolchainGuard.cmake COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMake/BuildStamp.h.in ${CMAKE_BINARY_DIR}/BuildStamp.h.in COPYONLY)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/ShalltearPackages.cmake CONTENT "${SHALLTEAR_PACKAGES_CONTENT}")
configure_file(${CMAKE_BINARY_DIR}/CMakePackage/ShalltearConfigVersion.cmake ${CMAKE_BINARY_DIR}/ShalltearConfigVersion.cmake COPYONLY)

set(SHALLTEAR_PACKAGE_ROOT_RELATIVE ".")
set(SHALLTEAR_CONFIG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Source/Core/Public")
set(SHALLTEAR_CONFIG_RUNTIME_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(SHALLTEAR_CONFIG_PLAYER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/Source/PlayerExecutable/Private")
set(SHALLTEAR_CONFIG_ENGINE_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
# only record of the compiler in a dev checkout
set(SHALLTEAR_CONFIG_COMPILER_PATH "${CMAKE_CXX_COMPILER}")
configure_file(
    ${CMAKE_SOURCE_DIR}/CMake/ShalltearConfig.cmake.in
    ${CMAKE_BINARY_DIR}/ShalltearConfig.cmake
    @ONLY
)
