if(NOT TARGET CSharp OR NOT TARGET Audio)
	return()
endif()

file(GLOB_RECURSE AUDIO_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(AudioCSharp SOURCES ${AUDIO_CSHARP_SOURCES})

target_link_libraries(AudioCSharp PRIVATE Audio CSharp)
target_include_directories(AudioCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
