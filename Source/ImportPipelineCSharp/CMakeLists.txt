if(NOT TARGET CSharp OR NOT TARGET ImportPipeline)
	return()
endif()

file(GLOB_RECURSE IMPORTPIPELINE_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(ImportPipelineCSharp SOURCES ${IMPORTPIPELINE_CSHARP_SOURCES})

target_link_libraries(ImportPipelineCSharp PRIVATE ImportPipeline CSharp)
target_include_directories(ImportPipelineCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
