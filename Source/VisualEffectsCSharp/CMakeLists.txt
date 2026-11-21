if(NOT TARGET CSharp OR NOT TARGET VisualEffects)
	return()
endif()

file(GLOB_RECURSE VISUALEFFECTS_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(VisualEffectsCSharp SOURCES ${VISUALEFFECTS_CSHARP_SOURCES})

target_link_libraries(VisualEffectsCSharp PRIVATE VisualEffects CSharp)
target_include_directories(VisualEffectsCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
