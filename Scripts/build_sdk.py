import collections
import fnmatch
import hashlib
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



# new license string here will need a Component in generate_notices.py
LLVM_LICENSE = "Apache-2.0 WITH LLVM-exception"
MINGW_HEADERS_LICENSE = "public domain and ZPL-2.1, with LGPL-2.1+ DirectX/DDK headers and GCC's GPLv2+ stdarg.h"
MINGW_RUNTIME_LICENSE = "public domain and ZPL-2.1, plus MIT for winpthreads and winstorecompat"

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
	'ld.lld.exe', 'ld', 'ld-wrapper.sh', 'x86_64-w64-mingw32-ld',
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


def classify(relative):
	for rule in LLVM_RULES:
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
		"-DCMAKE_BUILD_TYPE=Release"
	]
	run_cmd(cmake_cmd, cwd=build_path)

	build_cmd = ["cmake", "--build", ".", "--config", "Release", "--parallel"]
	run_cmd(build_cmd, cwd=build_path)

	install_cmd = ["cmake", "--install", ".", "--config", "Release",
				   "--prefix", install_prefix(is_shared)]
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

	print("\n======== Ready to upload ========")
	print(f"  file      {archive_path}")
	print(f"  bytes     {size:,}")
	print(f"  sha256    {digest.hexdigest()}")
	print(f"  contents  {raw:,} B unpacked, under a single {stem}/ directory")


def llvm_wanted(relative, unclassified=None):
	rule = classify(relative)
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


def verify_toolchain():
	llvm_root = os.path.join(TOOLCHAIN_DIR, 'LLVM')
	allowed = LLVM_BIN_ALLOWLIST | {'cmake.exe', 'ninja.exe'}
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
			if not file_name.lower().endswith(('.exe', '.dll')):
				continue
			executables += 1
			if file_name not in allowed and not absolute.startswith(llvm_root + os.sep):
				offenders.append(f"  not on the allowlist: {absolute}")
	required = [
		os.path.join(TOOLCHAIN_DIR, 'LLVM', 'bin', 'x86_64-w64-mingw32-clang++.exe'),
		os.path.join(TOOLCHAIN_DIR, 'LLVM', 'bin', 'ld.lld.exe'),
		os.path.join(TOOLCHAIN_DIR, 'CMake', 'bin', 'cmake.exe'),
		os.path.join(TOOLCHAIN_DIR, 'Ninja', 'ninja.exe'),
	]
	offenders += [f"  missing: {path}" for path in required if not os.path.exists(path)]
	if offenders:
		print("Error: the staged toolchain isn't distributable:")
		print('\n'.join(offenders))
		sys.exit(1)
	return executables, staged


def stage_toolchain():
	unclassified = []
	sources = {
		'LLVM': (os.path.join(PREBUILT_DIR, 'llvm-mingw'),
				 lambda relative: llvm_wanted(relative, unclassified)),
		'CMake': (os.path.join(PREBUILT_DIR, 'cmake'), None),
		'Ninja': (os.path.join(PREBUILT_DIR, 'ninja'), None),
	}
	missing = [name for name, (path, _) in sources.items() if not os.path.isdir(path)]
	if missing:
		print(f"Error: the SDK bundles a toolchain and these prebuilts are absent: {', '.join(missing)}")
		print("  fix: python Scripts/fetch_prebuilt.py --toolchain")
		sys.exit(1)

	for name, (path, wanted) in sources.items():
		count = copy_tree(path, os.path.join(TOOLCHAIN_DIR, name), wanted)
		print(f"  {name:<6} {count:>5} file(s)")

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

	executables, staged = verify_toolchain()
	print(f"  verified {staged} staged file(s) (each has a rule shipping it)")
	print(f"           {executables} of them executables (all on the allowlist)")


def finish_sdk():

	dxc_dll = os.path.join(ROOT_DIR, 'External', 'Prebuilt', 'dxc', 'bin', 'x64', 'dxcompiler.dll')
	if not os.path.exists(dxc_dll):
		print("Warning: dxcompiler.dll prebuilt missing (run Scripts/fetch_prebuilt.py)")

	roslyn_dll = os.path.join(ROOT_DIR, 'External', 'Prebuilt', 'roslyn', 'bin', 'ShalltearRoslyn.dll')
	if not os.path.exists(roslyn_dll):
		print("Warning: ShalltearRoslyn.dll prebuilt missing (run Scripts/fetch_prebuilt.py)")

	print(f"SDK successfully packaged to: {SDK_DIR}")
	print("  consume it with:  find_package(Shalltear 0.0.0 REQUIRED)")
	print(f"  pointing at:      -DShalltear_ROOT={SDK_DIR}")
	print(f"  toolchain:        {TOOLCHAIN_DIR} (found before PATH)")

if __name__ == "__main__":
	toolchain.prepare("the SDK package")

	if working_tree_dirt():
		print("NOTE: the working tree is dirty -> no archive at the end.\n")

	if os.path.exists(SDK_DIR):
		shutil.rmtree(SDK_DIR)

	print("======== Building Shalltear Engine (SHARED) ========")
	build_engine(is_shared=True)

	print("\n======== Building Shalltear Engine (STATIC) ========")
	build_engine(is_shared=False)

	print("\n======== Staging the toolchain ========")
	stage_toolchain()

	print("\n======== Finishing SDK ========")
	finish_sdk()

	print("\n---- Archiving ----")
	archive_sdk()
