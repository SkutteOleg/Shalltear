if(NOT TARGET CSharp OR NOT TARGET Animation)
	return()
endif()

file(GLOB_RECURSE ANIMATION_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(AnimationCSharp SOURCES ${ANIMATION_CSHARP_SOURCES})

target_link_libraries(AnimationCSharp PRIVATE Animation CSharp)
target_include_directories(AnimationCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
