set(sltPhysicsBackendDir "${CMAKE_SOURCE_DIR}/External/Submodules/box3d")

if(EXISTS "${sltPhysicsBackendDir}/include/box3d/box3d.h")
	set(sltPhysicsBackendDefault "box3d")
else()
	set(sltPhysicsBackendDefault "stub")
endif()

set(SHALLTEAR_PHYSICS_BACKEND "${sltPhysicsBackendDefault}" CACHE STRING
	"3D physics backend: box3d (the External/Submodules/box3d submodule) or stub")
set_property(CACHE SHALLTEAR_PHYSICS_BACKEND PROPERTY STRINGS "box3d" "stub")

set(sltPhysicsBackendBox3DFile "${CMAKE_CURRENT_SOURCE_DIR}/Private/BackendBox3D.cpp")
set(sltPhysicsBackendStubFile "${CMAKE_CURRENT_SOURCE_DIR}/Private/BackendStub.cpp")

file(GLOB_RECURSE PHYSICS3D_SOURCES "Private/*.cpp" "Private/*.h")
list(REMOVE_ITEM PHYSICS3D_SOURCES "${sltPhysicsBackendBox3DFile}" "${sltPhysicsBackendStubFile}")

set(sltPhysicsBackendSources "")
if(SHALLTEAR_PHYSICS_BACKEND STREQUAL "box3d")
	if(NOT EXISTS "${sltPhysicsBackendDir}/include/box3d/box3d.h")
		message(FATAL_ERROR
			"SHALLTEAR_PHYSICS_BACKEND is 'box3d' but the submodule isn't checked out at:\n"
			"  ${sltPhysicsBackendDir}\n"
			"  Run 'git submodule update --init --recursive' (or select 'stub').")
	endif()

	file(GLOB sltPhysicsBackendSources "${sltPhysicsBackendDir}/src/*.c")
	if(NOT sltPhysicsBackendSources)
		message(FATAL_ERROR "no backend sources under ${sltPhysicsBackendDir}/src")
	endif()
	list(APPEND PHYSICS3D_SOURCES "${sltPhysicsBackendBox3DFile}")
elseif(SHALLTEAR_PHYSICS_BACKEND STREQUAL "stub")
	list(APPEND PHYSICS3D_SOURCES "${sltPhysicsBackendStubFile}")
else()
	message(FATAL_ERROR "SHALLTEAR_PHYSICS_BACKEND is '${SHALLTEAR_PHYSICS_BACKEND}'. Expected 'box3d' or 'stub'.")
endif()

shalltear_add_package(Physics3D SOURCES ${PHYSICS3D_SOURCES} ${sltPhysicsBackendSources})

if(sltPhysicsBackendSources)
	set_source_files_properties(${sltPhysicsBackendSources} PROPERTIES COMPILE_OPTIONS -Wno-error)
endif()

target_include_directories(Physics3D PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)

if(SHALLTEAR_PHYSICS_BACKEND STREQUAL "box3d")
	target_include_directories(Physics3D PRIVATE
		${sltPhysicsBackendDir}/include
		${sltPhysicsBackendDir}/src)

	set_target_properties(Physics3D PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)

	if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
			target_compile_options(Physics3D PRIVATE /clang:-ffp-contract=off)
		else()
			target_compile_options(Physics3D PRIVATE -ffp-contract=off)
		endif()
	elseif(MSVC)
		target_compile_options(Physics3D PRIVATE /fp:contract-)
	endif()

	if(EMSCRIPTEN)
		target_compile_options(Physics3D PRIVATE -msimd128 -msse2)
	endif()

	target_compile_definitions(Physics3D PRIVATE $<$<CONFIG:RelWithDebInfo>:B3_ENABLE_ASSERT>)

	target_compile_definitions(Physics3D PRIVATE BOX3D_VALIDATE)

endif()

if(SHALLTEAR_BUILD_SHARED)
	file(GLOB_RECURSE PHYSICS3D_EDITOR_SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/../Physics3DEditor/Private/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/../Physics3DEditor/Private/*.h")
	if(NOT PHYSICS3D_EDITOR_SOURCES)
		message(FATAL_ERROR "Source/Physics3DEditor/Private has no sources (package's editor half is missing).")
	endif()
	target_sources(Physics3D PRIVATE ${PHYSICS3D_EDITOR_SOURCES})
	target_link_libraries(Physics3D PRIVATE ShalltearEditor)
	target_compile_definitions(Physics3D PRIVATE SLT_WITH_EDITOR)

	target_include_directories(Physics3D PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Physics3DEditor/Private)
endif()
