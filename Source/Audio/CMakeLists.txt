file(GLOB_RECURSE AUDIO_SOURCES "Private/*.cpp" "Private/*.c" "Private/*.h")
shalltear_select_platform_sources(AUDIO_SOURCES)

shalltear_add_package(Audio SOURCES ${AUDIO_SOURCES})

target_include_directories(Audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)

file(GLOB AUDIO_CLIENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Private/Containers/*.cpp")
set_source_files_properties(${AUDIO_CLIENT_SOURCES} PROPERTIES COMPILE_DEFINITIONS SHALLTEAR_AUDIO_VOICE_CLIENT)
file(GLOB AUDIO_GUARDED_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/Effects/*.h")
foreach(header IN LISTS AUDIO_GUARDED_HEADERS)
	get_filename_component(headerName ${header} NAME)
	if(headerName STREQUAL "AudioComposite.h" OR headerName STREQUAL "AudioVoiceInternals.h")
		continue()
	endif()
	file(READ ${header} headerText)
	string(FIND "${headerText}" "#include \"AudioVoiceInternals.h\"" guardAt)
	if(guardAt EQUAL -1)
		message(FATAL_ERROR "Source/Audio/Private/${headerName} doesn't include AudioVoiceInternals.h. Private headers (except AudioComposite.h) should include it.")
	endif()
endforeach()

if(WIN32)
	target_compile_definitions(Audio PRIVATE MA_ENABLE_WASAPI)
elseif(EMSCRIPTEN)
	target_compile_definitions(Audio PRIVATE MA_ENABLE_WEBAUDIO)
	if(NOT SHALLTEAR_WEB_THREADS)
		target_compile_definitions(Audio PRIVATE MA_NO_PTHREADS)
	endif()
else()
	target_compile_definitions(Audio PRIVATE MA_ENABLE_PULSEAUDIO MA_ENABLE_ALSA)
endif()
target_compile_definitions(Audio PRIVATE
	MA_ENABLE_ONLY_SPECIFIC_BACKENDS
	MA_ENABLE_NULL
	MA_NO_ENCODING
	MA_NO_RESOURCE_MANAGER
	MA_NO_NODE_GRAPH
	MA_NO_ENGINE
	MA_NO_GENERATION)

if(NOT SHALLTEAR_BUILD_SHARED)
	target_compile_definitions(Audio PRIVATE MA_NO_DECODING)
endif()

set_source_files_properties(
	${CMAKE_CURRENT_SOURCE_DIR}/Private/Miniaudio.c
	${CMAKE_CURRENT_SOURCE_DIR}/Private/QOA.c
	PROPERTIES C_STANDARD 99 COMPILE_OPTIONS "-w")

set(AUDIO_OPUS_DIR ${CMAKE_SOURCE_DIR}/External/Submodules/opus)
if(NOT EXISTS ${AUDIO_OPUS_DIR}/opus_sources.mk)
	message(FATAL_ERROR "External/Submodules/opus is missing. Run `git submodule update --init External/Submodules/opus`.")
endif()
list(APPEND CMAKE_MODULE_PATH ${AUDIO_OPUS_DIR}/cmake)
function(audio_read_opus_sources out)
	include(OpusFunctions)
	set(sources)
	foreach(group IN ITEMS "opus_sources.mk;opus_sources" "opus_sources.mk;opus_sources_float" "celt_sources.mk;celt_sources"
						   "silk_sources.mk;silk_sources" "silk_sources.mk;silk_sources_float")
		list(GET group 0 file)
		list(GET group 1 name)
		string(TOUPPER ${name} upper)
		get_opus_sources(${upper} ${AUDIO_OPUS_DIR}/${file} listed)
		foreach(source IN LISTS listed)
			list(APPEND sources ${AUDIO_OPUS_DIR}/${source})
		endforeach()
	endforeach()
	set(${out} ${sources} PARENT_SCOPE)
endfunction()
audio_read_opus_sources(AUDIO_OPUS_SOURCES)
target_sources(Audio PRIVATE ${AUDIO_OPUS_SOURCES})
set_source_files_properties(${AUDIO_OPUS_SOURCES} PROPERTIES
	C_STANDARD 99
	COMPILE_OPTIONS "-w"
	COMPILE_DEFINITIONS "OPUS_BUILD;VAR_ARRAYS;HAVE_LRINT;HAVE_LRINTF;ENABLE_HARDENING;DISABLE_DEBUG_FLOAT"
	INCLUDE_DIRECTORIES "${AUDIO_OPUS_DIR};${AUDIO_OPUS_DIR}/include;${AUDIO_OPUS_DIR}/celt;${AUDIO_OPUS_DIR}/silk;${AUDIO_OPUS_DIR}/silk/float")
target_include_directories(Audio PRIVATE ${AUDIO_OPUS_DIR}/include)

target_compile_options(Audio PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wfunction-effects -Werror=function-effects>)

if(SHALLTEAR_BUILD_SHARED)
	file(GLOB_RECURSE AUDIO_EDITOR_SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/../AudioEditor/Private/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/../AudioEditor/Private/*.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/../AudioEditor/Private/*.h")
	if(NOT AUDIO_EDITOR_SOURCES)
		message(FATAL_ERROR "Source/AudioEditor/Private has no sources (package's editor half is missing).")
	endif()
	target_sources(Audio PRIVATE ${AUDIO_EDITOR_SOURCES})
	target_link_libraries(Audio PRIVATE ShalltearEditor)
	target_compile_definitions(Audio PRIVATE SLT_WITH_EDITOR)

	target_include_directories(Audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../AudioEditor/Private)

	set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../AudioEditor/Private/ThirdParty/stb_vorbis.c
		PROPERTIES C_STANDARD 99 COMPILE_OPTIONS "-w")
endif()
