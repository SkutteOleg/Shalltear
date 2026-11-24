file(GLOB_RECURSE CORE_SOURCES "Private/*.cpp" "Private/*.h" "Public/*.h")
shalltear_select_platform_sources(CORE_SOURCES)
if(SHALLTEAR_RHI_BACKEND STREQUAL "WebGPU")
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/Private/Graphics/Vulkan/")
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/Private/Platform/[^/]+/(Windows|Linux)Vulkan\\.cpp$")
else()
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/Private/Graphics/WebGPU/")
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/Private/Platform/[^/]+/(Windows|Linux|Web)WebGPU\\.cpp$")
endif()

add_library(ShalltearCore ${SHALLTEAR_LIB_TYPE} ${CORE_SOURCES})

set_target_properties(ShalltearCore PROPERTIES LINKER_LANGUAGE CXX)

target_compile_definitions(ShalltearCore PRIVATE SHALLTEAR_CORE_BUILD)
if(SHALLTEAR_RHI_BACKEND STREQUAL "WebGPU")
    target_compile_definitions(ShalltearCore PUBLIC SHALLTEAR_RHI_WEBGPU)
endif()

if(WIN32)
    target_compile_definitions(ShalltearCore PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(ShalltearCore PRIVATE user32 gdi32 shell32 ole32 shcore dwmapi)
endif()

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SHALLTEAR_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT SHALLTEAR_GIT_COMMIT)
    set(SHALLTEAR_GIT_COMMIT "unknown")
endif()
set(SHALLTEAR_GIT_COMMIT ${SHALLTEAR_GIT_COMMIT} PARENT_SCOPE)

set(SHALLTEAR_GIT_DIR "${CMAKE_SOURCE_DIR}/.git")
if(EXISTS "${SHALLTEAR_GIT_DIR}" AND NOT IS_DIRECTORY "${SHALLTEAR_GIT_DIR}")
    file(READ "${SHALLTEAR_GIT_DIR}" SHALLTEAR_GIT_FILE)
    if(SHALLTEAR_GIT_FILE MATCHES "gitdir:[ \t]*([^\r\n]+)")
        get_filename_component(SHALLTEAR_GIT_DIR "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif()
endif()

set(SHALLTEAR_GIT_WATCHED "")
if(EXISTS "${SHALLTEAR_GIT_DIR}/HEAD")
    list(APPEND SHALLTEAR_GIT_WATCHED "${SHALLTEAR_GIT_DIR}/HEAD")
    file(READ "${SHALLTEAR_GIT_DIR}/HEAD" SHALLTEAR_GIT_HEAD)
    if(SHALLTEAR_GIT_HEAD MATCHES "ref:[ \t]*([^\r\n]+)" AND EXISTS "${SHALLTEAR_GIT_DIR}/${CMAKE_MATCH_1}")
        list(APPEND SHALLTEAR_GIT_WATCHED "${SHALLTEAR_GIT_DIR}/${CMAKE_MATCH_1}")
    endif()
endif()
if(EXISTS "${SHALLTEAR_GIT_DIR}/logs/HEAD")
    list(APPEND SHALLTEAR_GIT_WATCHED "${SHALLTEAR_GIT_DIR}/logs/HEAD")
endif()
if(SHALLTEAR_GIT_WATCHED)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SHALLTEAR_GIT_WATCHED})
endif()

string(TIMESTAMP SHALLTEAR_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

if(CMAKE_BUILD_TYPE)
    set(SHALLTEAR_BUILD_CONFIG ${CMAKE_BUILD_TYPE})
else()
    set(SHALLTEAR_BUILD_CONFIG "Unspecified")
endif()

configure_file(Private/BuildInfo.h.in ${CMAKE_CURRENT_BINARY_DIR}/Generated/BuildInfo.generated.h @ONLY)

set(SHALLTEAR_GENERATED_PUBLIC_DIR ${CMAKE_CURRENT_BINARY_DIR}/GeneratedPublic)
set(SHALLTEAR_GENERATED_PUBLIC_DIR ${SHALLTEAR_GENERATED_PUBLIC_DIR} PARENT_SCOPE)
configure_file(Private/BuildIdentity.generated.h.in
    ${SHALLTEAR_GENERATED_PUBLIC_DIR}/Shalltear/Core/BuildIdentity.generated.h @ONLY)

if(SHALLTEAR_RHI_BACKEND STREQUAL "WebGPU")
    target_include_directories(ShalltearCore
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>
            $<BUILD_INTERFACE:${SHALLTEAR_GENERATED_PUBLIC_DIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Private
            ${CMAKE_CURRENT_BINARY_DIR}/Generated
    )
    if(EMSCRIPTEN)
        if(NOT SHALLTEAR_EMDAWNWEBGPU_PORT)
            message(FATAL_ERROR "SHALLTEAR_EMDAWNWEBGPU_PORT not set. Use CMake/Toolchains/Web-wasm32.cmake")
        endif()
        target_compile_options(ShalltearCore PRIVATE "--use-port=${SHALLTEAR_EMDAWNWEBGPU_PORT}:cpp_bindings=false")
        target_link_options(ShalltearCore PUBLIC "$<BUILD_INTERFACE:--use-port=${SHALLTEAR_EMDAWNWEBGPU_PORT}:cpp_bindings=false>")
    else()
        set(SHALLTEAR_DAWN_DIR ${CMAKE_SOURCE_DIR}/External/Prebuilt/dawn)
        if(NOT EXISTS ${SHALLTEAR_DAWN_DIR}/include/webgpu/webgpu.h)
            message(FATAL_ERROR "Dawn WebGPU headers missing. Run: python Scripts/fetch_prebuilt.py --web")
        endif()
        target_include_directories(ShalltearCore PRIVATE ${SHALLTEAR_DAWN_DIR}/include)
        if(WIN32)
            set(SHALLTEAR_DAWN_LIBRARY ${SHALLTEAR_DAWN_DIR}/bin/webgpu_dawn.dll)
        else()
            set(SHALLTEAR_DAWN_LIBRARY ${SHALLTEAR_DAWN_DIR}/lib/libwebgpu_dawn.so)
        endif()
        if(EXISTS ${SHALLTEAR_DAWN_LIBRARY} AND NOT CMAKE_CROSSCOMPILING)
            get_filename_component(slt_dawnName ${SHALLTEAR_DAWN_LIBRARY} NAME)
            add_custom_command(TARGET ShalltearCore POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHALLTEAR_DAWN_LIBRARY} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${slt_dawnName}
                COMMENT "Staging ${slt_dawnName}"
            )
        endif()
    endif()
else()
    set(SHALLTEAR_VULKAN_HEADERS_DIR ${CMAKE_SOURCE_DIR}/External/Submodules/Vulkan-Headers)
    set(SHALLTEAR_VMA_DIR ${CMAKE_SOURCE_DIR}/External/Submodules/VulkanMemoryAllocator)
    if(NOT EXISTS ${SHALLTEAR_VULKAN_HEADERS_DIR}/include/vulkan/vulkan.h OR NOT EXISTS ${SHALLTEAR_VMA_DIR}/include/vk_mem_alloc.h)
        message(FATAL_ERROR "Graphics submodules missing. Run: git submodule update --init --recursive")
    endif()

    target_include_directories(ShalltearCore
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>
            $<BUILD_INTERFACE:${SHALLTEAR_GENERATED_PUBLIC_DIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Private
            ${CMAKE_CURRENT_BINARY_DIR}/Generated
            ${SHALLTEAR_VULKAN_HEADERS_DIR}/include
            ${SHALLTEAR_VMA_DIR}/include
    )
endif()

if(CMAKE_HOST_WIN32)
    set(SHALLTEAR_DXC_LIBRARY ${CMAKE_SOURCE_DIR}/External/Prebuilt/dxc/bin/x64/dxcompiler.dll)
else()
    set(SHALLTEAR_DXC_LIBRARY ${CMAKE_SOURCE_DIR}/External/Prebuilt/dxc/lib/libdxcompiler.so)
endif()
set(SHALLTEAR_DXC_LIBRARY ${SHALLTEAR_DXC_LIBRARY} PARENT_SCOPE)
get_filename_component(slt_dxcName ${SHALLTEAR_DXC_LIBRARY} NAME)
if(EXISTS ${SHALLTEAR_DXC_LIBRARY} AND NOT CMAKE_CROSSCOMPILING)
    add_custom_command(TARGET ShalltearCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHALLTEAR_DXC_LIBRARY} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${slt_dxcName}
        COMMENT "Staging ${slt_dxcName}"
    )
elseif(NOT CMAKE_CROSSCOMPILING)
    message(STATUS "${slt_dxcName} prebuilt not fetched (run: python Scripts/fetch_prebuilt.py)")
endif()

set(SHALLTEAR_STAGED_NOTICES "")
foreach(notices THIRD-PARTY-NOTICES.txt THIRD-PARTY-NOTICES-Runtime.txt)
    add_custom_command(
        OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${notices}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/${notices} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${notices}
        DEPENDS ${CMAKE_SOURCE_DIR}/${notices}
        COMMENT "Staging ${notices}"
    )
    list(APPEND SHALLTEAR_STAGED_NOTICES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${notices})
endforeach()
add_custom_target(ShalltearNotices ALL DEPENDS ${SHALLTEAR_STAGED_NOTICES})

set(SHALLTEAR_STAGED_CXX_RUNTIME "")
foreach(runtimeDll ${SHALLTEAR_CXX_RUNTIME_FILES})
    get_filename_component(runtimeName ${runtimeDll} NAME)
    add_custom_command(
        OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${runtimeName}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${runtimeDll} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${runtimeName}
        DEPENDS ${runtimeDll}
        COMMENT "Staging ${runtimeName}"
    )
    list(APPEND SHALLTEAR_STAGED_CXX_RUNTIME ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${runtimeName})
endforeach()
if(SHALLTEAR_STAGED_CXX_RUNTIME)
    add_custom_target(ShalltearCxxRuntime ALL DEPENDS ${SHALLTEAR_STAGED_CXX_RUNTIME})
    add_dependencies(ShalltearCore ShalltearCxxRuntime)
endif()

set(SHALLTEAR_VULKAN_LOADER_DLL ${CMAKE_SOURCE_DIR}/External/Prebuilt/vulkanrt/vulkan-1.dll)
if(EXISTS ${SHALLTEAR_VULKAN_LOADER_DLL})
    add_custom_command(TARGET ShalltearCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHALLTEAR_VULKAN_LOADER_DLL} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vulkan-1.dll
        COMMENT "Staging vulkan-1.dll (software loader)"
    )
endif()

set(SHALLTEAR_FREETYPE_DIR ${CMAKE_SOURCE_DIR}/External/Submodules/freetype)
if(NOT EXISTS ${SHALLTEAR_FREETYPE_DIR}/include/ft2build.h)
    message(FATAL_ERROR "freetype submodule missing. Run: git submodule update --init --recursive")
endif()
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
set(sltWarningsAsErrors ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
add_subdirectory(${SHALLTEAR_FREETYPE_DIR} ${CMAKE_BINARY_DIR}/External/freetype EXCLUDE_FROM_ALL)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${sltWarningsAsErrors})
target_link_libraries(ShalltearCore PRIVATE freetype)

if(NOT SHALLTEAR_DISABLE_PCH)
    target_precompile_headers(ShalltearCore PRIVATE Private/CorePCH.h)
endif()
