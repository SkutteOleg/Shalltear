file(GLOB_RECURSE NETWORK_SOURCES "Private/*.cpp" "Private/*.h")
shalltear_select_platform_sources(NETWORK_SOURCES)
if(EMSCRIPTEN)
	list(FILTER NETWORK_SOURCES EXCLUDE REGEX "/Private/Platform/Posix/")
	list(FILTER NETWORK_SOURCES EXCLUDE REGEX "/Private/WebSocketNative\\.cpp$")
endif()

shalltear_add_package(Network SOURCES ${NETWORK_SOURCES})

target_include_directories(Network PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)

if(WIN32)
	target_link_libraries(Network PRIVATE ws2_32 winhttp secur32 crypt32 ncrypt)
elseif(NOT EMSCRIPTEN)
	target_link_libraries(Network PRIVATE ${CMAKE_DL_LIBS})
endif()
