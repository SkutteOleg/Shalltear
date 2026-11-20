if(NOT TARGET CSharp OR NOT TARGET Physics3D)
	return()
endif()

file(GLOB_RECURSE PHYSICS3D_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(Physics3DCSharp SOURCES ${PHYSICS3D_CSHARP_SOURCES})

target_link_libraries(Physics3DCSharp PRIVATE CSharp Physics3D)
target_include_directories(Physics3DCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
