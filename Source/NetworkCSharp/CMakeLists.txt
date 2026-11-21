if(NOT TARGET CSharp OR NOT TARGET Network)
	return()
endif()

file(GLOB_RECURSE NETWORK_CSHARP_SOURCES "Private/*.cpp" "Private/*.h")

shalltear_add_package(NetworkCSharp SOURCES ${NETWORK_CSHARP_SOURCES})

target_link_libraries(NetworkCSharp PRIVATE Network CSharp)
target_include_directories(NetworkCSharp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Source/CSharp/Private)
