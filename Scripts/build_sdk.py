import argparse
import collections
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

import toolchain

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BUILD_DIR = os.path.join(ROOT_DIR, 'build')
SDK_DIR = os.path.join(ROOT_DIR, 'ShalltearSDK')
PREBUILT_DIR = os.path.join(ROOT_DIR, 'External', 'Prebuilt')
TOOLCHAIN_DIR = os.path.join(SDK_DIR, 'Toolchain')
DIST_DIR = os.path.join(ROOT_DIR, 'Dist')
DOTNET_PIN_PATH = os.path.join(ROOT_DIR, 'Source', 'CSharp', 'DotNetPin.json')



# new license string here will need a Component in generate_notices.py
LLVM_LICENSE = "Apache-2.0 WITH LLVM-exception"
MINGW_HEADERS_LICENSE = "public domain and ZPL-2.1, with LGPL-2.1+ DirectX/DDK headers and GCC's GPLv2+ stdarg.h"
MINGW_RUNTIME_LICENSE = "public domain and ZPL-2.1, plus MIT for winpthreads and winstorecompat"
MONO_CECIL_LICENSE = "MIT"
ROSLYN_LICENSE = "MIT"
DAWN_LICENSE = "BSD-3-Clause"
EMSCRIPTEN_LICENSE = "MIT and NCSA"

SHIP, DROP = True, False

Rule = collections.namedtuple('Rule', 'patterns verdict license why')


def _sysroot_lib_inventory():
	path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'toolchain_sysroot_lib.txt')
	with open(path, 'r', encoding='utf-8') as f:
		return frozenset(
			line.strip() for line in f
			if line.strip() and not line.lstrip().startswith('#')
		)


SYSROOT_LIB_INVENTORY = _sysroot_lib_inventory()

LLVM_BIN_ALLOWLIST = frozenset([
	'x86_64-w64-mingw32-clang++.exe', 'x86_64-w64-mingw32-clang.exe',
	'x86_64-w64-mingw32-c++.exe', 'x86_64-w64-mingw32-g++.exe', 'x86_64-w64-mingw32-gcc.exe',
	'clang.exe', 'clang++.exe', 'clang-23.exe', 'c++.exe', 'cc.exe', 'gcc.exe', 'g++.exe',
	'clang-target-wrapper.exe', 'clang-target-wrapper.sh', 'llvm-wrapper.exe',
	'mingw32-common.cfg', 'x86_64-pc-windows-gnu.cfg', 'x86_64-w64-windows-gnu.cfg',
	'libLLVM-23.dll', 'libclang-cpp.dll',
	'libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll',
	'ld.lld.exe', 'lld-link.exe', 'ld', 'ld-wrapper.sh', 'x86_64-w64-mingw32-ld',
	'llvm-ar.exe', 'llvm-ranlib.exe', 'llvm-lib.exe', 'llvm-dlltool.exe',
	'llvm-nm.exe', 'llvm-objcopy.exe', 'llvm-strip.exe', 'llvm-symbolizer.exe',
	'llvm-rc.exe', 'llvm-windres.exe', 'llvm-cvtres.exe', 'llvm-ml.exe',
	'ar.exe', 'ranlib.exe', 'dlltool.exe', 'nm.exe', 'objcopy.exe', 'strip.exe',
	'windres.exe', 'as.exe',
	'x86_64-w64-mingw32-ar.exe', 'x86_64-w64-mingw32-ranlib.exe',
	'x86_64-w64-mingw32-llvm-ar.exe', 'x86_64-w64-mingw32-llvm-ranlib.exe',
	'x86_64-w64-mingw32-as.exe', 'x86_64-w64-mingw32-dlltool.exe',
	'x86_64-w64-mingw32-nm.exe', 'x86_64-w64-mingw32-objcopy.exe',
	'x86_64-w64-mingw32-strip.exe', 'x86_64-w64-mingw32-windres.exe',
	'clang-scan-deps.exe', 'clang-scan-deps-real.exe', 'clang-scan-deps-wrapper.exe',
	'x86_64-w64-mingw32-clang-scan-deps.exe',
	'clangd.exe',
])

GPL_TOOL_NAMES = frozenset(['busybox.exe', 'mingw32-make.exe', 'make.exe', 'gendef.exe', 'widl.exe'])

SYSROOT_BIN_ALLOWLIST = frozenset(['libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll'])

MINGW_BIN_RUNTIME = frozenset(['libwinpthread-1.dll'])

LLVM_RULES = (
	Rule(('python/*',), DROP, '',
		 'embedded Python (only lldb reads it, no debugger is staged)'),
	Rule(('busybox/*',), DROP, '',
		 'busybox is GPLv2 + a shell is not part of compiling a module'),
	Rule(('aarch64-w64-mingw32/*', 'armv7-w64-mingw32/*', 'arm64ec-w64-mingw32/*', 'i686-w64-mingw32/*'),
		 DROP, '', 'an architecture this engine doesn\'t target'),
	Rule(('share/*',), DROP, '',
		 'clang-format/clang-doc assets + COPYING files of make, gendef/widl. None staged'),
	Rule(('.fetched',), DROP, '', "fetch_prebuilt.py's version marker"),

	Rule(('x86_64-w64-mingw32/lib/gcrt0.o', 'x86_64-w64-mingw32/lib/gcrt1.o',
		  'x86_64-w64-mingw32/lib/gcrt2.o', 'x86_64-w64-mingw32/lib/libgmon.a'),
		 DROP, '',
		 'GPLv2+: gcrt0.c and profil.c are Red Hat\'s and say "part of Cygwin", which mingw-w64\'s COPYING.MinGW-w64.txt assigns to GPLv2+. Linked only under -pg (which nothing here passes..)'),

	Rule(('include/*.idl',), DROP, '',
		 'an .idl becomes a header only through widl (not staged)'),

	Rule(('LICENSE.TXT',), SHIP, LLVM_LICENSE,
		 "the package's notice (covers every LLVM component below)"),
	Rule(tuple('bin/' + name for name in MINGW_BIN_RUNTIME), SHIP, MINGW_RUNTIME_LICENSE,
		 'winpthreads (imported by clang.exe)'),
	Rule(tuple('bin/' + name for name in LLVM_BIN_ALLOWLIST - MINGW_BIN_RUNTIME), SHIP, LLVM_LICENSE,
		 'drivers/linker/binutils/language server a module compile spawns'),
	Rule(('bin/*',), DROP, '',
		 'every other tool in bin/ (not on the name allowlist)'),

	Rule(('include/c++/*',), SHIP, LLVM_LICENSE,
		 "libc++'s headers (every user translation unit includes them)"),
	Rule(('include/*',), SHIP, MINGW_HEADERS_LICENSE,
		 'the Win32 header set a user module compiles against'),
	Rule(('lib/clang/*/include/*',), SHIP, LLVM_LICENSE,
		 "clang's resource headers (reached before the sysroot's)"),

	Rule(('lib/clang/*/lib/windows/*-aarch64.a', 'lib/clang/*/lib/windows/*-aarch64.dll.a',
		  'lib/clang/*/lib/windows/*-arm.a', 'lib/clang/*/lib/windows/*-arm.dll.a',
		  'lib/clang/*/lib/windows/*-i386.a', 'lib/clang/*/lib/windows/*-i386.dll.a'),
		 DROP, '', "another architecture's compiler-rt"),
	Rule(('lib/clang/*/lib/windows/*',), SHIP, LLVM_LICENSE,
		 'the compiler-rt builtins the linker pulls for 128-bit division and stack probes'),
	Rule(('lib/*',), DROP, '',
		 "rest of clang's lib/ (runtimes for OSes this engine doesn't target)"),

	Rule(tuple('x86_64-w64-mingw32/lib/' + name for name in SYSROOT_LIB_INVENTORY),
		 SHIP, MINGW_RUNTIME_LICENSE,
		 'CRT objects + Win32 import libs every link (listed in Scripts/toolchain_sysroot_lib.txt)'),
	Rule(tuple('x86_64-w64-mingw32/bin/' + name for name in SYSROOT_BIN_ALLOWLIST),
		 SHIP, MINGW_RUNTIME_LICENSE,
		 "runtime DLLs copied beside a user's game exe"),
	Rule(('x86_64-w64-mingw32/bin/*',), DROP, '',
		 'sysroot\'s other DLLs (OpenMP/AddressSanitizer runtimes, nothing here enables either)'),
	Rule(('x86_64-w64-mingw32/share/*',), SHIP, MINGW_RUNTIME_LICENSE,
		 "COPYING files. generate_notices.py reads them from here + COPYING.MinGW-w64.txt's cross-references resolve inside this directory"),
)


ENGINE_LICENSE = "MIT"

CSHARP_RULES = (
	Rule(('Shalltear.Runtime.dll', 'Shalltear.Runtime.xml', 'Shalltear.Runtime.runtimeconfig.json'),
		 SHIP, ENGINE_LICENSE, 'C# runtime the editor hosts + script projects reference'),
	Rule(('Shalltear.Physics3D.dll', 'Shalltear.Physics3D.xml', 'Shalltear.Audio.dll', 'Shalltear.Audio.xml', 'Shalltear.Animation.dll',
		  'Shalltear.Animation.xml', 'Shalltear.Network.dll',
		  'Shalltear.Network.xml', 'Shalltear.PostProcessing.dll',
		  'Shalltear.PostProcessing.xml', 'Shalltear.VisualEffects.dll',
		  'Shalltear.VisualEffects.xml', 'Shalltear.ImportPipeline.dll',
		  'Shalltear.ImportPipeline.xml'), SHIP, ENGINE_LICENSE,
		 'C# APIs of packages (referenced by script projects)'),
	Rule(('Shalltear.Editor.dll', 'Shalltear.Editor.xml'), SHIP, ENGINE_LICENSE,
		 'C# API of the editor (referenced by editor-only script assemblies)'),
	Rule(('Shalltear.Generators.dll',), SHIP, ENGINE_LICENSE,
		 'source generator a script compile loads'),
	Rule(('Renamer/Shalltear.Renamer.dll', 'Renamer/Shalltear.Renamer.runtimeconfig.json'), SHIP, ENGINE_LICENSE,
		 "renamer a Release player's script compile runs"),
	Rule(('Renamer/Mono.Cecil*.dll',), SHIP, MONO_CECIL_LICENSE,
		 'assembly reader/writer the renamer loads'),
	Rule(('Refactor/Shalltear.Refactor.dll', 'Refactor/Shalltear.Refactor.runtimeconfig.json'), SHIP, ENGINE_LICENSE,
		 'Roslyn script rename and C++ view header generator tool'),
	Rule(('Refactor/Microsoft.CodeAnalysis.dll', 'Refactor/Microsoft.CodeAnalysis.CSharp.dll'), SHIP, ROSLYN_LICENSE,
		 'Roslyn C# syntax parser and rewriter loaded by Shalltear.Refactor'),
	Rule(('HeaderBinder/Shalltear.HeaderBinder.dll', 'HeaderBinder/Shalltear.HeaderBinder.runtimeconfig.json'), SHIP, ENGINE_LICENSE,
		 'libclang C++ header binding generator for mixed C++/C# projects'),
	Rule(('native/CRTShim.obj', 'native/CRTShimStatic.obj', 'native/WebMonoTables.c', 'native/WebMonoBootstrap.cpp'), SHIP, ENGINE_LICENSE,
		 "startup shims and WebAssembly bridge a game's script library or player links"),
	Rule(('native/libs/clang_rt.builtins-x86_64.lib',), SHIP, LLVM_LICENSE,
		 "compiler-rt builtins (the shim's stack probe)"),
	Rule(('native/libs/*.lib',), SHIP, MINGW_RUNTIME_LICENSE,
		 "Win32 import libs a game's script library links"),
	Rule(('Shalltear.Conformance.dll', 'Shalltear.Conformance.Audio.dll', 'Shalltear.Conformance.Editor.dll'), DROP, '',
		 'test assemblies (only the test suite loads them)'),
)

LIBCLANG_RULES = (
	Rule(('LICENSE.TXT',), SHIP, LLVM_LICENSE,
		 "libclang's LLVM license notice"),
	Rule(('bin/libclang.dll',), SHIP, LLVM_LICENSE,
		 'libclang shared library loaded by Shalltear.HeaderBinder on Windows'),
	Rule(('lib/libclang.so*',), SHIP, LLVM_LICENSE,
		 'libclang shared library loaded by Shalltear.HeaderBinder on Linux'),
	Rule(('python/*',), DROP, '',
		 'developer Python bindings (not needed at SDK runtime)'),
	Rule(('.fetched',), DROP, '',
		 "fetch_prebuilt.py's version marker"),
)

DAWN_WEB_RULES = (
	Rule(('LICENSE',), SHIP, DAWN_LICENSE,
		 "Dawn/Tint BSD-3-Clause license notice"),
	Rule(('bin/tint.exe', 'bin/tint'), SHIP, DAWN_LICENSE,
		 'Tint SPIR-V to WGSL translator invoked during Web shader cooking'),
	Rule(('emdawnwebgpu_pkg/*',), SHIP, DAWN_LICENSE,
		 'Emscripten Dawn WebGPU port linked into WebAssembly players'),
	Rule(('bin/webgpu_dawn.dll', 'lib/libwebgpu_dawn.so', 'include/*', '.fetched'), DROP, '',
		 'native Dawn test runtime / headers not needed for Web exports'),
)

WEB_NUGET_PACKS = (
	'microsoft.netcore.app.runtime.mono.browser-wasm',
	'microsoft.netcore.app.runtime.mono.multithread.browser-wasm',
	'microsoft.net.runtime.webassembly.sdk',
	'microsoft.net.illink.tasks',
)

EMSCRIPTEN_CONFIG_TEXT = (
	"import os as _os\n"
	"import shutil as _shutil\n"
	"_root = _os.path.dirname(_os.path.abspath(__file__))\n"
	"LLVM_ROOT = _os.path.join(_root, 'install', 'bin')\n"
	"BINARYEN_ROOT = _os.path.join(_root, 'install')\n"
	"EMSCRIPTEN_ROOT = _os.path.join(_root, 'install', 'emscripten')\n"
	"CACHE = _os.path.join(_root, 'install', 'emscripten', 'cache')\n"
	"_node = _shutil.which('node')\n"
	"if _node:\n"
	"    NODE_JS = _node\n"
)


def classify(relative, rules=LLVM_RULES):
	for rule in rules:
		for pattern in rule.patterns:
			if fnmatch.fnmatchcase(relative, pattern):
				return rule
	return None

def run_cmd(cmd, cwd):
	print(f"Running: {' '.join(cmd)}")
	result = subprocess.run(cmd, cwd=cwd, text=True)
	if result.returncode != 0:
		print(f"Error: Command failed with code {result.returncode}")
		sys.exit(1)

def install_prefix(is_shared):
	return SDK_DIR if is_shared else os.path.join(SDK_DIR, 'Static')

def build_engine(is_shared):
	mode_str = "SHARED" if is_shared else "STATIC"
	build_path = os.path.join(BUILD_DIR, mode_str)
	os.makedirs(build_path, exist_ok=True)

	cmake_cmd = [
		"cmake", ROOT_DIR,
		"-G", "Ninja",
		f"-DCMAKE_CXX_COMPILER={toolchain.CXX_COMPILER}",
		f"-DCMAKE_C_COMPILER={toolchain.C_COMPILER}",
		f"-DSHALLTEAR_BUILD_SHARED={'ON' if is_shared else 'OFF'}",
		"-DSHALLTEAR_BUILD_TESTS=OFF",
		"-DSHALLTEAR_BUILD_BENCHMARKS=OFF",
		"-DSHALLTEAR_CSHARP=ON",
		"-DCMAKE_BUILD_TYPE=Release"
	]
	run_cmd(cmake_cmd, cwd=build_path)

	build_cmd = ["cmake", "--build", ".", "--config", "Release", "--parallel"]
	run_cmd(build_cmd, cwd=build_path)

	install_cmd = ["cmake", "--install", ".", "--config", "Release",
				   "--prefix", install_prefix(is_shared)]
	run_cmd(install_cmd, cwd=build_path)
	return build_path


def build_linux_static():
	build_path = os.path.join(BUILD_DIR, 'STATIC-LINUX')
	os.makedirs(build_path, exist_ok=True)
	toolchain_file = os.path.join(ROOT_DIR, 'CMake', 'Toolchains', 'Linux-x64.cmake')
	cmake_cmd = [
		"cmake", ROOT_DIR,
		"-G", "Ninja",
		f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
		"-DSHALLTEAR_BUILD_SHARED=OFF",
		"-DSHALLTEAR_BUILD_TESTS=OFF",
		"-DSHALLTEAR_BUILD_BENCHMARKS=OFF",
		"-DSHALLTEAR_CSHARP=ON",
		"-DCMAKE_BUILD_TYPE=Release",
	]
	run_cmd(cmake_cmd, cwd=build_path)
	run_cmd(["cmake", "--build", ".", "--config", "Release", "--parallel"], cwd=build_path)
	install_cmd = [
		"cmake", "--install", ".", "--config", "Release",
		"--prefix", os.path.join(SDK_DIR, 'Static', 'linux-x86_64'),
	]
	run_cmd(install_cmd, cwd=build_path)
	return build_path


def build_web_static(threaded=False):
	flavor = 'wasm32-threads' if threaded else 'wasm32'
	build_path = os.path.join(BUILD_DIR, 'STATIC-WEB-THREADS' if threaded else 'STATIC-WEB')
	os.makedirs(build_path, exist_ok=True)
	toolchain_file = os.path.join(ROOT_DIR, 'CMake', 'Toolchains', 'Web-wasm32.cmake')
	cmake_cmd = [
		"cmake", ROOT_DIR,
		"-G", "Ninja",
		f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
		f"-DSHALLTEAR_WEB_THREADS={'ON' if threaded else 'OFF'}",
		"-DSHALLTEAR_BUILD_SHARED=OFF",
		"-DSHALLTEAR_BUILD_TESTS=OFF",
		"-DSHALLTEAR_BUILD_BENCHMARKS=OFF",
		"-DSHALLTEAR_CSHARP=ON",
		"-DCMAKE_BUILD_TYPE=Release",
	]
	run_cmd(cmake_cmd, cwd=build_path)
	run_cmd(["cmake", "--build", ".", "--config", "Release", "--parallel"], cwd=build_path)
	install_cmd = [
		"cmake", "--install", ".", "--config", "Release",
		"--prefix", os.path.join(SDK_DIR, 'Static', flavor),
	]
	run_cmd(install_cmd, cwd=build_path)
	return build_path


def artifact_dirt(porcelain):
	dirt = []
	for line in porcelain.splitlines():
		if not line.strip():
			continue
		if line.startswith("?? "):
			path = line[3:].strip('"')
			if path.startswith(".") or "/" not in path:
				continue
		dirt.append(line)
	return dirt


def working_tree_dirt():
	result = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT_DIR,
							capture_output=True, text=True)
	if result.returncode != 0:
		return ["(not a git checkout, no commit to provide)"]
	return artifact_dirt(result.stdout) or None


def packaged_commit():
	config = os.path.join(SDK_DIR, 'Lib', 'cmake', 'Shalltear', 'ShalltearConfig.cmake')
	with open(config, 'r', encoding='utf-8') as f:
		match = re.search(r'set\(SHALLTEAR_GIT_COMMIT "([^"]*)"\)', f.read())
	return match.group(1) if match else ""


ARCHIVE_NAME = re.compile(r'^Shalltear-dev-[0-9a-f]{12}-windows-x64\.zip$')


def superseded_archives(names, keeping):
	return sorted(name for name in names
				  if name != keeping and ARCHIVE_NAME.match(name))


def archive_sdk():
	dirt = working_tree_dirt()
	if dirt:
		print("NO ARCHIVE WRITTEN: the working tree isn't clean (zip can't provide a commit).")
		print("The SDK above is complete and usable.")
		for line in dirt[:10]:
			print(f"    {line}")
		if len(dirt) > 10:
			print(f"    ... and {len(dirt) - 10} more")
		return

	commit = packaged_commit()
	if not commit:
		print("NO ARCHIVE WRITTEN: the packaged ShalltearConfig.cmake has no commit.")
		return

	stem = f"Shalltear-dev-{commit}-windows-x64"
	os.makedirs(DIST_DIR, exist_ok=True)
	archive_path = os.path.join(DIST_DIR, stem + ".zip")
	if os.path.exists(archive_path):
		os.remove(archive_path)
	for name in superseded_archives(os.listdir(DIST_DIR), stem + ".zip"):
		os.remove(os.path.join(DIST_DIR, name))
		print(f"Removed superseded {name}. Rebuild it from its commit if you need it back")

	print(f"Archiving {SDK_DIR} -> {archive_path}")
	raw = 0
	with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
		for directory, _, files in os.walk(SDK_DIR):
			for file_name in sorted(files):
				absolute = os.path.join(directory, file_name)
				relative = os.path.relpath(absolute, SDK_DIR).replace(os.sep, '/')
				raw += os.path.getsize(absolute)
				archive.write(absolute, f"{stem}/{relative}")

	digest = hashlib.sha256()
	with open(archive_path, 'rb') as f:
		for block in iter(lambda: f.read(1 << 20), b""):
			digest.update(block)
	size = os.path.getsize(archive_path)
	sha256_hex = digest.hexdigest()
	with open(archive_path + ".sha256", 'w', encoding='utf-8') as f:
		f.write(f"{sha256_hex}  {os.path.basename(archive_path)}\n")

	print("\n======== Ready to upload ========")
	print(f"  file      {archive_path}")
	print(f"  bytes     {size:,}")
	print(f"  sha256    {sha256_hex}")
	print(f"  contents  {raw:,} B unpacked, under a single {stem}/ directory")


def llvm_wanted(relative, unclassified=None):
	rule = classify(relative)
	if rule is None:
		if unclassified is not None:
			unclassified.append(relative)
		return False
	return rule.verdict


def libclang_wanted(relative, unclassified=None):
	rule = classify(relative, LIBCLANG_RULES)
	if rule is None:
		if unclassified is not None:
			unclassified.append(relative)
		return False
	return rule.verdict


def copy_tree(source, destination, wanted=None):
	copied = 0
	for directory, _, files in os.walk(source):
		for file_name in files:
			absolute = os.path.join(directory, file_name)
			relative = os.path.relpath(absolute, source).replace(os.sep, '/')
			if wanted and not wanted(relative):
				continue
			target = os.path.join(destination, relative.replace('/', os.sep))
			os.makedirs(os.path.dirname(target), exist_ok=True)
			shutil.copy2(absolute, target)
			copied += 1
	return copied


def verify_toolchain(bundle_dotnet=False, bundle_linux=False, bundle_web=False):
	llvm_root = os.path.join(TOOLCHAIN_DIR, 'LLVM')
	libclang_root = os.path.join(TOOLCHAIN_DIR, 'libclang')
	optional_roots = tuple(
		os.path.join(TOOLCHAIN_DIR, name) + os.sep
		for name in ('DotNet', 'NuGet', 'linux-x64', 'emsdk', 'dawn')
	)
	allowed = LLVM_BIN_ALLOWLIST | {'cmake.exe', 'ninja.exe', 'libclang.dll'}
	offenders = []
	executables = 0
	staged = 0
	for directory, _, files in os.walk(TOOLCHAIN_DIR):
		for file_name in files:
			absolute = os.path.join(directory, file_name)
			staged += 1
			if file_name in GPL_TOOL_NAMES:
				offenders.append(f"  GPL tool: {absolute}")
				continue
			if absolute.startswith(llvm_root + os.sep):
				relative = os.path.relpath(absolute, llvm_root).replace(os.sep, '/')
				rule = classify(relative)
				if rule is None:
					offenders.append(f"  staged but no rule classifies it: {relative}")
				elif rule.verdict is DROP:
					offenders.append(f"  staged although a rule drops it ({rule.why}): {relative}")
			elif absolute.startswith(libclang_root + os.sep):
				relative = os.path.relpath(absolute, libclang_root).replace(os.sep, '/')
				rule = classify(relative, LIBCLANG_RULES)
				if rule is None:
					offenders.append(f"  staged in libclang/ but no rule classifies it: {relative}")
				elif rule.verdict is DROP:
					offenders.append(f"  staged in libclang/ although a rule drops it ({rule.why}): {relative}")
			if not file_name.lower().endswith(('.exe', '.dll')):
				continue
			if absolute.startswith(optional_roots):
				continue
			executables += 1
			if file_name not in allowed and not absolute.startswith(llvm_root + os.sep):
				offenders.append(f"  not on the allowlist: {absolute}")
	required = [
		os.path.join(TOOLCHAIN_DIR, 'LLVM', 'bin', 'x86_64-w64-mingw32-clang++.exe'),
		os.path.join(TOOLCHAIN_DIR, 'LLVM', 'bin', 'ld.lld.exe'),
		os.path.join(TOOLCHAIN_DIR, 'LLVM', 'bin', 'lld-link.exe'),
		os.path.join(TOOLCHAIN_DIR, 'CMake', 'bin', 'cmake.exe'),
		os.path.join(TOOLCHAIN_DIR, 'Ninja', 'ninja.exe'),
		os.path.join(TOOLCHAIN_DIR, 'libclang', 'bin', 'libclang.dll'),
		os.path.join(TOOLCHAIN_DIR, 'libclang', 'LICENSE.TXT'),
	]
	if bundle_dotnet:
		required.append(os.path.join(TOOLCHAIN_DIR, 'DotNet', 'dotnet.exe' if os.name == 'nt' else 'dotnet'))
	if bundle_linux:
		required.append(os.path.join(
			TOOLCHAIN_DIR, 'linux-x64', 'sysroot', 'usr', 'include',
			'x86_64-unknown-linux-gnu', 'c++', 'v1', '__config_site'
		))
	if bundle_web:
		exe = '.exe' if os.name == 'nt' else ''
		required.append(os.path.join(TOOLCHAIN_DIR, 'emsdk', '.emscripten'))
		required.append(os.path.join(TOOLCHAIN_DIR, 'emsdk', 'install', 'emscripten', 'em++' + exe))
		required.append(os.path.join(TOOLCHAIN_DIR, 'dawn', 'bin', 'tint' + exe))
		required.append(os.path.join(TOOLCHAIN_DIR, 'dawn', 'emdawnwebgpu_pkg', 'emdawnwebgpu.port.py'))
		for pack in WEB_NUGET_PACKS:
			required.append(os.path.join(TOOLCHAIN_DIR, 'NuGet', pack))
	offenders += [f"  missing: {path}" for path in required if not os.path.exists(path)]
	if offenders:
		print("Error: the staged toolchain isn't distributable:")
		print('\n'.join(offenders))
		sys.exit(1)
	return executables, staged


def stage_optional_bundles(bundle_dotnet=False, bundle_linux=False, bundle_web=False):
	if bundle_dotnet:
		with open(DOTNET_PIN_PATH, 'r', encoding='utf-8') as f:
			sdk_version = json.load(f)['sdkVersion']
		dotnet_src = os.path.join(PREBUILT_DIR, 'dotnet', sdk_version)
		if not os.path.isdir(dotnet_src):
			print(f"Error: --bundle-dotnet requested but {dotnet_src} is absent.")
			print("  fix: python Scripts/fetch_prebuilt.py --dotnet")
			sys.exit(1)
		count = copy_tree(dotnet_src, os.path.join(TOOLCHAIN_DIR, 'DotNet'),
						  lambda rel: rel != '.fetched')
		print(f"  DotNet {count:>5} file(s)")
		nuget_src = os.path.join(PREBUILT_DIR, 'nuget')
		if os.path.isdir(nuget_src):
			nuget_count = copy_tree(nuget_src, os.path.join(TOOLCHAIN_DIR, 'NuGet'))
			print(f"  NuGet  {nuget_count:>5} file(s)")
	if bundle_linux:
		linux_src = os.path.join(PREBUILT_DIR, 'linux-x64')
		if not os.path.isdir(linux_src):
			print(f"Error: --bundle-linux requested but {linux_src} is absent.")
			print("  fix: python Scripts/fetch_prebuilt.py --toolchain --linux")
			sys.exit(1)
		count = copy_tree(linux_src, os.path.join(TOOLCHAIN_DIR, 'linux-x64'),
						  lambda rel: rel != '.fetched')
		print(f"  Linux  {count:>5} file(s)")
	if bundle_web:
		emsdk_src = os.path.join(PREBUILT_DIR, 'emsdk')
		dawn_src = os.path.join(PREBUILT_DIR, 'dawn')
		if not os.path.isdir(emsdk_src) or not os.path.isdir(dawn_src):
			print("Error: --bundle-web requested but External/Prebuilt/emsdk or dawn is absent.")
			print("  fix: python Scripts/fetch_prebuilt.py --web")
			sys.exit(1)
		emsdk_dst = os.path.join(TOOLCHAIN_DIR, 'emsdk')
		emsdk_count = copy_tree(emsdk_src, emsdk_dst,
								lambda rel: rel != '.fetched')
		with open(os.path.join(emsdk_dst, '.emscripten'), 'w', encoding='utf-8', newline='\n') as f:
			f.write(EMSCRIPTEN_CONFIG_TEXT)
		print(f"  Emscripten {emsdk_count:>5} file(s)")
		if not bundle_dotnet:
			web_nuget_count = 0
			for pack in WEB_NUGET_PACKS:
				pack_src = os.path.join(PREBUILT_DIR, 'nuget', pack)
				if not os.path.isdir(pack_src):
					print(f"Error: --bundle-web requested but {pack_src} is absent.")
					print("  fix: python Scripts/fetch_prebuilt.py --web")
					sys.exit(1)
				web_nuget_count += copy_tree(pack_src, os.path.join(TOOLCHAIN_DIR, 'NuGet', pack))
			print(f"  WebNuGet {web_nuget_count:>4} file(s)")
		dawn_unclassified = []
		def dawn_wanted(relative):
			rule = classify(relative, DAWN_WEB_RULES)
			if rule is None:
				dawn_unclassified.append(relative)
				return False
			return rule.verdict
		dawn_count = copy_tree(dawn_src, os.path.join(TOOLCHAIN_DIR, 'dawn'), dawn_wanted)
		print(f"  Dawn   {dawn_count:>5} file(s)")
		if dawn_unclassified:
			print(f"Error: {len(dawn_unclassified)} path(s) in dawn match no rule in DAWN_WEB_RULES.")
			for relative in sorted(dawn_unclassified)[:40]:
				print(f"    {relative}")
			sys.exit(1)


def stage_toolchain(bundle_dotnet=False, bundle_linux=False, bundle_web=False):
	unclassified = []
	libclang_unclassified = []
	sources = {
		'LLVM': (os.path.join(PREBUILT_DIR, 'llvm-mingw'),
				 lambda relative: llvm_wanted(relative, unclassified)),
		'CMake': (os.path.join(PREBUILT_DIR, 'cmake'), None),
		'Ninja': (os.path.join(PREBUILT_DIR, 'ninja'), None),
		'libclang': (os.path.join(PREBUILT_DIR, 'libclang'),
					 lambda relative: libclang_wanted(relative, libclang_unclassified)),
	}
	missing = [name for name, (path, _) in sources.items() if not os.path.isdir(path)]
	if missing:
		print(f"Error: the SDK bundles a toolchain and these prebuilts are absent: {', '.join(missing)}")
		print("  fix: python Scripts/fetch_prebuilt.py --toolchain --libclang")
		sys.exit(1)

	for name, (path, wanted) in sources.items():
		count = copy_tree(path, os.path.join(TOOLCHAIN_DIR, name), wanted)
		print(f"  {name:<8} {count:>5} file(s)")

	if unclassified:
		print(f"Error: {len(unclassified)} path(s) in llvm-mingw match no rule in LLVM_RULES.")
		print("  Likely a toolchain bump. Read each one,")
		print("  decide whether it may ship and under what license, then add a rule")
		print("  (or a name to Scripts/toolchain_sysroot_lib.txt) saying so.")
		for relative in sorted(unclassified)[:40]:
			print(f"    {relative}")
		if len(unclassified) > 40:
			print(f"    ... and {len(unclassified) - 40} more")
		sys.exit(1)

	if libclang_unclassified:
		print(f"Error: {len(libclang_unclassified)} path(s) in libclang match no rule in LIBCLANG_RULES.")
		for relative in sorted(libclang_unclassified)[:40]:
			print(f"    {relative}")
		sys.exit(1)

	stage_optional_bundles(bundle_dotnet=bundle_dotnet, bundle_linux=bundle_linux, bundle_web=bundle_web)

	executables, staged = verify_toolchain(bundle_dotnet=bundle_dotnet, bundle_linux=bundle_linux, bundle_web=bundle_web)
	print(f"  verified {staged} staged file(s) (each has a rule shipping it)")
	print(f"           {executables} of them executables (all on the allowlist)")


def stage_csharp(directory):
	unclassified = []
	shipped = 0
	for root, _, files in os.walk(directory):
		for file_name in files:
			absolute = os.path.join(root, file_name)
			relative = os.path.relpath(absolute, directory).replace(os.sep, '/')
			rule = classify(relative, CSHARP_RULES)
			if rule is None:
				unclassified.append(relative)
			elif rule.verdict is DROP:
				os.remove(absolute)
			else:
				shipped += 1
	if unclassified:
		print(f"Error: {len(unclassified)} file(s) under CSharp/ match no rule in CSHARP_RULES.")
		print("  Read each one, decide whether it may ship and under what license, then add a rule.")
		for relative in sorted(unclassified):
			print(f"    {relative}")
		sys.exit(1)
	missing = [pattern for rule in CSHARP_RULES if rule.verdict is SHIP for pattern in rule.patterns
			   if '*' not in pattern and not os.path.exists(os.path.join(directory, pattern))]
	if missing:
		print(f"Error: CSharp/ lacks {', '.join(missing)}")
		sys.exit(1)
	print(f"  CSharp {shipped:>5} file(s)")


def finish_sdk():

	dxc_dll = os.path.join(ROOT_DIR, 'External', 'Prebuilt', 'dxc', 'bin', 'x64', 'dxcompiler.dll')
	if not os.path.exists(dxc_dll):
		print("Warning: dxcompiler.dll prebuilt missing (run Scripts/fetch_prebuilt.py)")

	print(f"SDK successfully packaged to: {SDK_DIR}")
	print("  consume it with:  find_package(Shalltear 0.0.0 REQUIRED)")
	print(f"  pointing at:      -DShalltear_ROOT={SDK_DIR}")
	print(f"  toolchain:        {TOOLCHAIN_DIR} (found before PATH)")

if __name__ == "__main__":
	parser = argparse.ArgumentParser(description="Build, stage, verify, and archive the Shalltear SDK")
	parser.add_argument("--bundle-dotnet", action="store_true",
						help="bundle the pinned .NET SDK and offline NuGet cache under Toolchain/")
	parser.add_argument("--bundle-linux", action="store_true",
						help="cross-compile the Linux static player target and bundle the Linux target pack")
	parser.add_argument("--bundle-web", action="store_true",
						help="cross-compile the WebAssembly static targets (wasm32 and wasm32-threads) and bundle Emscripten + Tint")
	parser.add_argument("--no-archive", action="store_true",
						help="skip creating the Dist/ archive after staging")
	args = parser.parse_args()

	toolchain.prepare("the SDK package")

	if not args.no_archive and working_tree_dirt():
		print("NOTE: the working tree is dirty -> no archive at the end.\n")

	if os.path.exists(SDK_DIR):
		shutil.rmtree(SDK_DIR)

	print("======== Building Shalltear Engine (SHARED) ========")
	build_engine(is_shared=True)

	print("\n======== Building Shalltear Engine (STATIC) ========")
	build_engine(is_shared=False)

	if args.bundle_linux:
		print("\n======== Building Shalltear Engine (STATIC LINUX) ========")
		build_linux_static()

	if args.bundle_web:
		print("\n======== Building Shalltear Engine (STATIC WEB wasm32) ========")
		build_web_static(threaded=False)
		print("\n======== Building Shalltear Engine (STATIC WEB wasm32-threads) ========")
		build_web_static(threaded=True)

	print("\n======== Staging the toolchain ========")
	stage_toolchain(bundle_dotnet=args.bundle_dotnet, bundle_linux=args.bundle_linux, bundle_web=args.bundle_web)
	stage_csharp(os.path.join(SDK_DIR, 'CSharp'))

	print("\n======== Finishing SDK ========")
	finish_sdk()

	if not args.no_archive:
		print("\n---- Archiving ----")
		archive_sdk()
