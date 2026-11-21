if(NOT TARGET CSharp OR NOT TARGET PostProcessing)
	return()
endif()

file(GLOB_RECURSE POSTPROCESSING_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(PostProcessingCSharp SOURCES ${POSTPROCESSING_CSHARP_SOURCES})

target_link_libraries(PostProcessingCSharp PRIVATE PostProcessing CSharp)
target_include_directories(PostProcessingCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
