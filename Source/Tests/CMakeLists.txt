set(GTEST_SUBMODULE_DIR ${CMAKE_SOURCE_DIR}/External/Submodules/googletest)
if(NOT EXISTS ${GTEST_SUBMODULE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "googletest submodule is missing. Run:\n"
        "    git submodule update --init --recursive")
endif()

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

set(sltWarningsAsErrors ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
add_subdirectory(${GTEST_SUBMODULE_DIR} ${CMAKE_BINARY_DIR}/External/googletest EXCLUDE_FROM_ALL)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${sltWarningsAsErrors})

include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES "Private/*.cpp" "Private/*.h")
shalltear_select_platform_sources(TEST_SOURCES)
if(NOT TARGET CSharp)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "/CSharp[^/]*$")
endif()

add_executable(ShalltearTests ${TEST_SOURCES})

target_link_libraries(ShalltearTests
    PRIVATE
        ShalltearCore
        ShalltearEditor
        ImportPipeline
        Graphs
        NodeShader
        GTest::gtest
)

target_include_directories(ShalltearTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)

target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Core/Private)

target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Editor/Private)

target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/ImportPipeline/Private)

target_include_directories(ShalltearTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Source/Graphs/Private
    ${CMAKE_SOURCE_DIR}/Source/NodeShader/Private)

if(WIN32)
    set(sltTestHostTarget "Windows")
    set(sltTestPlayerSuffix ".exe")
else()
    set(sltTestHostTarget "${CMAKE_SYSTEM_NAME}")
    set(sltTestPlayerSuffix ".${CMAKE_SYSTEM_PROCESSOR}")
endif()
target_compile_definitions(ShalltearTests PRIVATE
    SHALLTEAR_LOG_STRIP_LEVEL=0
    SLT_TEST_DLL_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}"
    SLT_TEST_EXE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}"
    SLT_TEST_HOST_TARGET="${sltTestHostTarget}"
    SLT_TEST_PLAYER_SUFFIX="${sltTestPlayerSuffix}")
if(NOT WIN32)
    # probes exported for test modules to find by name
    set_target_properties(ShalltearTests PROPERTIES ENABLE_EXPORTS ON)
endif()

if(NOT SHALLTEAR_DISABLE_PCH)
    target_precompile_headers(ShalltearTests PRIVATE Private/TestsPCH.h)
endif()

if(TARGET Physics3D)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests Physics3D)
    else()
        target_link_libraries(ShalltearTests PRIVATE Physics3D)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Physics3D/Private)
    endif()

    if(SHALLTEAR_PHYSICS_BACKEND STREQUAL "stub")
        target_compile_definitions(ShalltearTests PRIVATE SHALLTEAR_PHYSICS_BACKEND_STUB)
    endif()
endif()

if(TARGET Animation)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests Animation)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Animation/Public)
    else()
        target_link_libraries(ShalltearTests PRIVATE Animation)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Animation/Private)
    endif()
endif()

if(TARGET PostProcessing)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests PostProcessing)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/PostProcessing/Public)
    else()
        target_link_libraries(ShalltearTests PRIVATE PostProcessing)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/PostProcessing/Private)
    endif()
endif()

if(TARGET Audio)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests Audio)
    else()
        target_link_libraries(ShalltearTests PRIVATE Audio)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Audio/Private)
    endif()
endif()

if(TARGET VisualEffects)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests VisualEffects)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/VisualEffects/Public)
    else()
        target_link_libraries(ShalltearTests PRIVATE VisualEffects)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/VisualEffects/Private)
    endif()
endif()

if(TARGET Network)
    target_link_libraries(ShalltearTests PRIVATE Network)
    target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Network/Private)
endif()

if(TARGET CSharp)
    if(SHALLTEAR_BUILD_SHARED)
        add_dependencies(ShalltearTests CSharp)
        target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/CSharpEditor/Private)
    else()
        target_link_libraries(ShalltearTests PRIVATE CSharp)
    endif()
    target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
endif()
if(TARGET CSharpConformanceAOT)
    add_dependencies(ShalltearTests CSharpConformanceAOT)
    target_compile_definitions(ShalltearTests PRIVATE SLT_TEST_CSHARP_AOT)
endif()

if(TARGET ShalltearSampleModule)
    add_dependencies(ShalltearTests ShalltearSampleModule)
endif()
if(TARGET ShalltearHotReloadModuleV1)
    add_dependencies(ShalltearTests
        ShalltearHotReloadModuleV1
        ShalltearHotReloadModuleV2
        ShalltearHotReloadModuleBad
        ShalltearHotReloadModuleAbi
        ShalltearHotReloadModuleGone)
endif()
if(TARGET ShalltearPhysicsListenerModule)
    add_dependencies(ShalltearTests ShalltearPhysicsListenerModule)
endif()
if(TARGET ShalltearPhysicsScriptModule)
    add_dependencies(ShalltearTests ShalltearPhysicsScriptModule)
endif()
if(TARGET ShalltearAudioScriptModuleV1)
    add_dependencies(ShalltearTests ShalltearAudioScriptModuleV1 ShalltearAudioScriptModuleV2)
    target_include_directories(ShalltearTests PRIVATE ${CMAKE_SOURCE_DIR}/Source/AudioScriptModule/Private)
endif()
if(TARGET ShalltearVisualEffectsScriptModule)
    add_dependencies(ShalltearTests ShalltearVisualEffectsScriptModule)
endif()

gtest_discover_tests(ShalltearTests
    DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
)
