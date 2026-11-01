file(GLOB_RECURSE EDITOR_SOURCES "Private/*.cpp" "Private/*.h" "Public/*.h")

add_library(ShalltearEditor ${SHALLTEAR_LIB_TYPE} ${EDITOR_SOURCES})

set_target_properties(ShalltearEditor PROPERTIES LINKER_LANGUAGE CXX)

target_compile_definitions(ShalltearEditor PRIVATE SHALLTEAR_EDITOR_BUILD)

target_include_directories(ShalltearEditor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

if(NOT SHALLTEAR_DISABLE_PCH)
    target_precompile_headers(ShalltearEditor PRIVATE Private/EditorPCH.h)
endif()

target_link_libraries(ShalltearEditor PUBLIC ShalltearCore)

set(SHALLTEAR_ROSLYN_DIR ${CMAKE_SOURCE_DIR}/External/Prebuilt/roslyn/bin)
if(EXISTS ${SHALLTEAR_ROSLYN_DIR}/ShalltearRoslyn.dll)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    file(GLOB_RECURSE SHALLTEAR_ENGINE_STUB_HEADERS
        "${CMAKE_SOURCE_DIR}/Source/Core/Public/*.h"
        "${CMAKE_SOURCE_DIR}/Source/Animation/Public/*.h"
        "${CMAKE_SOURCE_DIR}/Source/Audio/Public/*.h"
        "${CMAKE_SOURCE_DIR}/Source/Physics3D/Public/*.h"
        "${CMAKE_SOURCE_DIR}/Source/PostProcessing/Public/*.h"
        "${CMAKE_SOURCE_DIR}/Source/VisualEffects/Public/*.h"
    )
    set(SHALLTEAR_ENGINE_STUBS_JSON ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Roslyn/Shalltear.EngineStubs.json)
    if(Python3_EXECUTABLE)
        add_custom_command(
            OUTPUT ${SHALLTEAR_ENGINE_STUBS_JSON}
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHALLTEAR_ROSLYN_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Roslyn
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Scripts/generate_csharp_stubs.py
                --roslyn-dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Roslyn
                --repo-root ${CMAKE_SOURCE_DIR}
                --out ${SHALLTEAR_ENGINE_STUBS_JSON}
            DEPENDS
                ${SHALLTEAR_ROSLYN_DIR}/ShalltearRoslyn.dll
                ${CMAKE_SOURCE_DIR}/Scripts/generate_csharp_stubs.py
                ${SHALLTEAR_ENGINE_STUB_HEADERS}
            COMMENT "Generating Shalltear.EngineStubs.json from engine C++ headers"
            VERBATIM
        )
        add_custom_target(ShalltearGenerateCSharpStubs DEPENDS ${SHALLTEAR_ENGINE_STUBS_JSON})
        add_dependencies(ShalltearEditor ShalltearGenerateCSharpStubs)
    else()
        add_custom_command(TARGET ShalltearEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHALLTEAR_ROSLYN_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Roslyn
            COMMENT "Staging ShalltearRoslyn.dll"
        )
    endif()
endif()
