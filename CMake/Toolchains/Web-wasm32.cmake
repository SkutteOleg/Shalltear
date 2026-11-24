option(SHALLTEAR_WEB_THREADS "Build WebAssembly with pthreads and SharedArrayBuffer" OFF)

if(NOT SHALLTEAR_EMSDK)
	if(DEFINED ENV{SHALLTEAR_EMSDK})
		set(SHALLTEAR_EMSDK "$ENV{SHALLTEAR_EMSDK}")
	elseif(DEFINED ENV{EMSDK})
		set(SHALLTEAR_EMSDK "$ENV{EMSDK}")
	else()
		foreach(slt_emsdkCandidate
			"${CMAKE_CURRENT_LIST_DIR}/../../../../Toolchain/emsdk"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../Toolchain/emsdk"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../../Toolchain/emsdk"
			"${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/emsdk")
			get_filename_component(slt_emsdkResolved "${slt_emsdkCandidate}" ABSOLUTE)
			if(EXISTS "${slt_emsdkResolved}/install/emscripten/cmake/Modules/Platform/Emscripten.cmake" OR
			   EXISTS "${slt_emsdkResolved}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
				set(SHALLTEAR_EMSDK "${slt_emsdkResolved}")
				break()
			endif()
		endforeach()
		if(NOT SHALLTEAR_EMSDK)
			get_filename_component(SHALLTEAR_EMSDK "${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/emsdk" ABSOLUTE)
		endif()
	endif()
endif()
set(SHALLTEAR_EMSDK "${SHALLTEAR_EMSDK}" CACHE PATH "Emscripten SDK root directory")

if(EXISTS "${SHALLTEAR_EMSDK}/install/emscripten/cmake/Modules/Platform/Emscripten.cmake")
	set(EMSCRIPTEN_ROOT_PATH "${SHALLTEAR_EMSDK}/install/emscripten")
elseif(EXISTS "${SHALLTEAR_EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
	set(EMSCRIPTEN_ROOT_PATH "${SHALLTEAR_EMSDK}/upstream/emscripten")
else()
	message(FATAL_ERROR
		"Shalltear: Emscripten SDK was not found at ${SHALLTEAR_EMSDK}.\n"
		"  Fix: python Scripts/fetch_prebuilt.py --web (or set EMSDK)")
endif()

if(EXISTS "${SHALLTEAR_EMSDK}/.emscripten")
	set(ENV{EM_CONFIG} "${SHALLTEAR_EMSDK}/.emscripten")
endif()

if(NOT SHALLTEAR_EMDAWNWEBGPU_PORT)
	if(DEFINED ENV{SHALLTEAR_EMDAWNWEBGPU_PORT})
		set(SHALLTEAR_EMDAWNWEBGPU_PORT "$ENV{SHALLTEAR_EMDAWNWEBGPU_PORT}")
	else()
		foreach(slt_dawnCandidate
			"${CMAKE_CURRENT_LIST_DIR}/../../../../Toolchain/dawn/emdawnwebgpu_pkg/emdawnwebgpu.port.py"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../Toolchain/dawn/emdawnwebgpu_pkg/emdawnwebgpu.port.py"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../../Toolchain/dawn/emdawnwebgpu_pkg/emdawnwebgpu.port.py"
			"${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/dawn/emdawnwebgpu_pkg/emdawnwebgpu.port.py")
			get_filename_component(slt_dawnResolved "${slt_dawnCandidate}" ABSOLUTE)
			if(EXISTS "${slt_dawnResolved}")
				set(SHALLTEAR_EMDAWNWEBGPU_PORT "${slt_dawnResolved}")
				break()
			endif()
		endforeach()
	endif()
endif()
set(SHALLTEAR_EMDAWNWEBGPU_PORT "${SHALLTEAR_EMDAWNWEBGPU_PORT}" CACHE FILEPATH "Path to emdawnwebgpu.port.py")
if(NOT EXISTS "${SHALLTEAR_EMDAWNWEBGPU_PORT}")
	message(FATAL_ERROR
		"Shalltear: emdawnwebgpu.port.py was not found at '${SHALLTEAR_EMDAWNWEBGPU_PORT}'.\n"
		"  Fix: python Scripts/fetch_prebuilt.py --web")
endif()

set(slt_exe "")
if(CMAKE_HOST_WIN32)
	set(slt_exe ".exe")
endif()
if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_MAKE_PROGRAM)
	foreach(slt_ninjaCandidate
		"${CMAKE_CURRENT_LIST_DIR}/../../../../Toolchain/Ninja/ninja${slt_exe}"
		"${CMAKE_CURRENT_LIST_DIR}/../../../../../Toolchain/Ninja/ninja${slt_exe}"
		"${CMAKE_CURRENT_LIST_DIR}/../../../../../../Toolchain/Ninja/ninja${slt_exe}"
		"${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/ninja/ninja${slt_exe}")
		get_filename_component(slt_ninja "${slt_ninjaCandidate}" ABSOLUTE)
		if(EXISTS "${slt_ninja}")
			set(CMAKE_MAKE_PROGRAM "${slt_ninja}" CACHE FILEPATH "")
			break()
		endif()
	endforeach()
endif()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
	SHALLTEAR_EMSDK
	SHALLTEAR_EMDAWNWEBGPU_PORT
	SHALLTEAR_WEB_THREADS
	EMSCRIPTEN_ROOT_PATH)

include("${EMSCRIPTEN_ROOT_PATH}/cmake/Modules/Platform/Emscripten.cmake")

# allow host-side Shalltear_ROOT / CMAKE_PREFIX_PATH for find_package(Shalltear)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(slt_webThreadFlag "")
set(slt_webThreadLink "")
if(SHALLTEAR_WEB_THREADS)
	set(slt_webThreadFlag "-pthread")
	set(slt_webThreadLink "-pthread -sSHARED_MEMORY=1")
endif()

set(CMAKE_C_FLAGS_INIT "-msimd128 -fwasm-exceptions ${slt_webThreadFlag}")
set(CMAKE_CXX_FLAGS_INIT "-msimd128 -fwasm-exceptions ${slt_webThreadFlag}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fwasm-exceptions ${slt_webThreadLink}")

set(SHALLTEAR_BUILD_SHARED OFF CACHE BOOL "Build engine modules as shared libs" FORCE)
set(SHALLTEAR_RHI_BACKEND "WebGPU" CACHE STRING "RHI backend (Vulkan or WebGPU)" FORCE)
set(SHALLTEAR_BUILD_TESTS OFF CACHE BOOL "Build Shalltear unit tests" FORCE)
set(SHALLTEAR_BUILD_BENCHMARKS OFF CACHE BOOL "Build Shalltear micro-benchmarks" FORCE)
