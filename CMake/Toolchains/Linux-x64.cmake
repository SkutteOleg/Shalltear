set(SHALLTEAR_LINUX_TRIPLE x86_64-unknown-linux-gnu)

if(NOT SHALLTEAR_LINUX_PACK)
	if(DEFINED ENV{SHALLTEAR_LINUX_PACK})
		set(SHALLTEAR_LINUX_PACK "$ENV{SHALLTEAR_LINUX_PACK}")
	else()
		foreach(slt_packCandidate
			"${CMAKE_CURRENT_LIST_DIR}/../../../../Toolchain/linux-x64"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../Toolchain/linux-x64"
			"${CMAKE_CURRENT_LIST_DIR}/../../../../../../Toolchain/linux-x64"
			"${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/linux-x64")
			get_filename_component(slt_packResolved "${slt_packCandidate}" ABSOLUTE)
			if(EXISTS "${slt_packResolved}/sysroot/usr/include/${SHALLTEAR_LINUX_TRIPLE}/c++/v1/__config_site")
				set(SHALLTEAR_LINUX_PACK "${slt_packResolved}")
				break()
			endif()
		endforeach()
		if(NOT SHALLTEAR_LINUX_PACK)
			get_filename_component(SHALLTEAR_LINUX_PACK "${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/linux-x64" ABSOLUTE)
		endif()
	endif()
endif()
set(SHALLTEAR_LINUX_PACK "${SHALLTEAR_LINUX_PACK}" CACHE PATH "Linux target pack (sysroot + C++ runtime)")
# loaded by try_compile projects too (without cache)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SHALLTEAR_LINUX_PACK)

if(NOT EXISTS "${SHALLTEAR_LINUX_PACK}/sysroot/usr/include/${SHALLTEAR_LINUX_TRIPLE}/c++/v1/__config_site")
	message(FATAL_ERROR
		"Shalltear: the Linux target pack was not found at ${SHALLTEAR_LINUX_PACK}.\n"
		"  Fix: python Scripts/fetch_prebuilt.py --toolchain --linux")
endif()

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	set(CMAKE_SYSTEM_NAME Linux)
	set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()

set(slt_exe "")
if(CMAKE_HOST_WIN32)
	set(slt_exe ".exe")
endif()
if(NOT CMAKE_CXX_COMPILER)
	foreach(slt_binCandidate
		"${CMAKE_CURRENT_LIST_DIR}/../../../../Toolchain/LLVM/bin"
		"${CMAKE_CURRENT_LIST_DIR}/../../../../../Toolchain/LLVM/bin"
		"${CMAKE_CURRENT_LIST_DIR}/../../../../../../Toolchain/LLVM/bin"
		"${CMAKE_CURRENT_LIST_DIR}/../../External/Prebuilt/llvm-mingw/bin")
		get_filename_component(slt_toolchainBin "${slt_binCandidate}" ABSOLUTE)
		if(EXISTS "${slt_toolchainBin}/clang++${slt_exe}")
			set(CMAKE_CXX_COMPILER "${slt_toolchainBin}/clang++${slt_exe}")
			break()
		endif()
	endforeach()
	if(NOT CMAKE_CXX_COMPILER)
		set(CMAKE_CXX_COMPILER clang++)
	endif()
endif()
if(NOT CMAKE_C_COMPILER)
	get_filename_component(slt_compilerBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
	if(slt_compilerBin AND EXISTS "${slt_compilerBin}/clang${slt_exe}")
		set(CMAKE_C_COMPILER "${slt_compilerBin}/clang${slt_exe}")
	else()
		set(CMAKE_C_COMPILER clang)
	endif()
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

set(CMAKE_C_COMPILER_TARGET ${SHALLTEAR_LINUX_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${SHALLTEAR_LINUX_TRIPLE})
set(CMAKE_SYSROOT "${SHALLTEAR_LINUX_PACK}/sysroot")

# host toolchain's libc++ headers (MinGW __config_site) searched 1st
set(CMAKE_C_FLAGS_INIT "-pthread")
set(CMAKE_CXX_FLAGS_INIT "-pthread -nostdinc++ -isystem=/usr/include/${SHALLTEAR_LINUX_TRIPLE}/c++/v1 -isystem=/usr/include/c++/v1")

foreach(slt_kind EXE SHARED MODULE)
	set(CMAKE_${slt_kind}_LINKER_FLAGS_INIT "-fuse-ld=lld -stdlib=libc++ -rtlib=compiler-rt -unwindlib=libgcc")
endforeach()
# glibc 2.31 still has dlopen in libdl
set(CMAKE_C_STANDARD_LIBRARIES_INIT "-ldl")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "-ldl")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
