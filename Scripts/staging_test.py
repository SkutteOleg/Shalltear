import contextlib
import io
import os
import shutil
import sys
import tempfile

import build_sdk
import generate_notices

PREBUILT = os.path.join(build_sdk.PREBUILT_DIR, 'llvm-mingw')

GPL_PROFILING = (
	'x86_64-w64-mingw32/lib/gcrt0.o',
	'x86_64-w64-mingw32/lib/gcrt1.o',
	'x86_64-w64-mingw32/lib/gcrt2.o',
	'x86_64-w64-mingw32/lib/libgmon.a',
)

CASES = (
	(GPL_PROFILING[0], build_sdk.DROP, 'gcrt0.o is GPLv2+ and must not travel'),
	(GPL_PROFILING[1], build_sdk.DROP, 'gcrt1.o is GPLv2+ and must not travel'),
	(GPL_PROFILING[2], build_sdk.DROP, 'gcrt2.o is GPLv2+ and must not travel'),
	(GPL_PROFILING[3], build_sdk.DROP, 'libgmon.a carries Red Hat\'s profil.o'),

	('busybox/bin/busybox.exe', build_sdk.DROP, 'busybox is GPLv2'),
	('bin/gendef.exe', build_sdk.DROP, 'gendef is GPLv3'),
	('bin/widl.exe', build_sdk.DROP, 'widl is Wine, LGPL'),
	('bin/mingw32-make.exe', build_sdk.DROP, 'make is GPLv3'),

	('include/amstream.idl', build_sdk.DROP, 'an .idl needs widl, which is not staged'),
	('include/ddk/ntifs.idl', build_sdk.DROP, 'the same, nested'),

	('aarch64-w64-mingw32/lib/libucrt.a', build_sdk.DROP, 'another architecture'),
	('i686-w64-mingw32/lib/libucrt.a', build_sdk.DROP, 'another architecture'),
	('python/lib/python3.14/os.py', build_sdk.DROP, 'the embedded Python'),
	('lib/clang/23/lib/linux/libclang_rt.builtins-x86_64.a', build_sdk.DROP, 'a Linux runtime'),
	('lib/clang/23/lib/windows/libclang_rt.builtins-aarch64.a', build_sdk.DROP, 'foreign compiler-rt'),

	('LICENSE.TXT', build_sdk.SHIP, "LLVM's notice"),
	('bin/x86_64-w64-mingw32-clang++.exe', build_sdk.SHIP, 'the driver a module compile spawns'),
	('bin/ld.lld.exe', build_sdk.SHIP, 'the linker'),
	('bin/clangd.exe', build_sdk.SHIP, 'the matching language server'),
	('bin/libwinpthread-1.dll', build_sdk.SHIP, 'what clang.exe itself imports'),
	('include/windows.h', build_sdk.SHIP, 'the Win32 header set'),
	('include/d3d11.h', build_sdk.SHIP, 'an LGPL header kept (filtering forks the sysroot)'),
	('include/stdarg.h', build_sdk.SHIP, "GCC's GPLv2+ header, kept under its exception"),
	('include/c++/v1/vector', build_sdk.SHIP, 'libc++'),
	('lib/clang/23/include/stddef.h', build_sdk.SHIP, "clang's resource headers"),
	('lib/clang/23/lib/windows/libclang_rt.builtins-x86_64.a', build_sdk.SHIP, 'compiler-rt builtins'),
	('x86_64-w64-mingw32/lib/crt2.o', build_sdk.SHIP, 'a CRT object every link resolves'),
	('x86_64-w64-mingw32/lib/libkernel32.a', build_sdk.SHIP, 'a Win32 import library'),
	('x86_64-w64-mingw32/lib/libwinstorecompat.a', build_sdk.SHIP, 'MIT, and its notice now travels'),
	('x86_64-w64-mingw32/bin/libc++.dll', build_sdk.SHIP, "a runtime DLL a user's game copies"),
	('x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64.txt', build_sdk.SHIP, 'the mingw-w64 notice'),
	('x86_64-w64-mingw32/share/mingw32/COPYING.LGPLv2.1', build_sdk.SHIP,
	 'the text that notice cites, which no distributor ships'),
	('x86_64-w64-mingw32/share/mingw32/COPYING.GPLv2', build_sdk.SHIP, 'the same, for stdarg.h'),
)

UNGOVERNED = (
	'x86_64-w64-mingw32/lib/libbrandnew.a',
	'x86_64-w64-mingw32/lib/libgomp.a',
	'x86_64-w64-mingw32/lib/gcrt3.o',
	'somethingnew/tool.exe',
	'x86_64-w64-mingw32/include/newheader.h',
)


def check_rules(fail):
	checks = 0
	for relative, expected, why in CASES:
		rule = build_sdk.classify(relative)
		if rule is None:
			fail("%s: no rule classifies it (%s)" % (relative, why))
		elif rule.verdict is not expected:
			fail("%s: expected %s, got %s -> %s" % (
				relative, 'SHIP' if expected else 'DROP',
				'SHIP' if rule.verdict else 'DROP', why))
		checks += 1

	for relative in UNGOVERNED:
		rule = build_sdk.classify(relative)
		if rule is not None:
			fail("%s: a path nothing should govern matched %r. An unknown file "
				 "must stop the package (not be %s)"
				 % (relative, rule.patterns[:2], 'shipped' if rule.verdict else 'dropped'))
		checks += 1

	for relative in GPL_PROFILING:
		name = relative.rsplit('/', 1)[-1]
		if name in build_sdk.SYSROOT_LIB_INVENTORY:
			fail("%s is in Scripts/toolchain_sysroot_lib.txt. It is GPLv2+ and "
				 "must never be listed there" % name)
		checks += 1

	declared = {build_sdk.LLVM_LICENSE, build_sdk.MINGW_HEADERS_LICENSE,
				build_sdk.MINGW_RUNTIME_LICENSE}
	for rule in build_sdk.LLVM_RULES:
		if rule.verdict is build_sdk.SHIP and rule.license not in declared:
			fail("a rule ships under an undeclared license %r. Add the Component "
				 "to Scripts/generate_notices.py first" % rule.license)
		if rule.verdict is build_sdk.DROP and rule.license:
			fail("a DROP rule names a license %r, which reads as if it ships" % rule.license)
		if not rule.why.strip():
			fail("a rule carries no reason: %r" % (rule.patterns[:2],))
		checks += 1

	names = " ".join(c.name for c in generate_notices.COMPONENTS).lower()
	for token in ('clang', 'libclang', 'mingw-w64 headers', 'winstorecompat', 'cmake', 'ninja', 'roslyn'):
		if token not in names:
			fail("no notices Component mentions %r, yet the SDK stages it" % token)
		checks += 1
	return checks


CSHARP_CASES = (
	('Shalltear.Runtime.dll', build_sdk.SHIP),
	('Shalltear.Runtime.xml', build_sdk.SHIP),
	('Shalltear.Runtime.runtimeconfig.json', build_sdk.SHIP),
	('Shalltear.Physics3D.dll', build_sdk.SHIP),
	('Shalltear.Physics3D.xml', build_sdk.SHIP),
	('Shalltear.Audio.dll', build_sdk.SHIP),
	('Shalltear.Audio.xml', build_sdk.SHIP),
	('Shalltear.Animation.dll', build_sdk.SHIP),
	('Shalltear.Animation.xml', build_sdk.SHIP),
	('Shalltear.Network.dll', build_sdk.SHIP),
	('Shalltear.Network.xml', build_sdk.SHIP),
	('Shalltear.PostProcessing.dll', build_sdk.SHIP),
	('Shalltear.PostProcessing.xml', build_sdk.SHIP),
	('Shalltear.VisualEffects.dll', build_sdk.SHIP),
	('Shalltear.VisualEffects.xml', build_sdk.SHIP),
	('Shalltear.ImportPipeline.dll', build_sdk.SHIP),
	('Shalltear.ImportPipeline.xml', build_sdk.SHIP),
	('Shalltear.Editor.dll', build_sdk.SHIP),
	('Shalltear.Editor.xml', build_sdk.SHIP),
	('Shalltear.Generators.dll', build_sdk.SHIP),
	('Renamer/Shalltear.Renamer.dll', build_sdk.SHIP),
	('Renamer/Shalltear.Renamer.runtimeconfig.json', build_sdk.SHIP),
	('Renamer/Mono.Cecil.dll', build_sdk.SHIP),
	('Renamer/Mono.Cecil.Rocks.dll', build_sdk.SHIP),
	('Refactor/Shalltear.Refactor.dll', build_sdk.SHIP),
	('Refactor/Shalltear.Refactor.runtimeconfig.json', build_sdk.SHIP),
	('Refactor/Microsoft.CodeAnalysis.dll', build_sdk.SHIP),
	('Refactor/Microsoft.CodeAnalysis.CSharp.dll', build_sdk.SHIP),
	('HeaderBinder/Shalltear.HeaderBinder.dll', build_sdk.SHIP),
	('HeaderBinder/Shalltear.HeaderBinder.runtimeconfig.json', build_sdk.SHIP),
	('native/CRTShim.obj', build_sdk.SHIP),
	('native/libs/clang_rt.builtins-x86_64.lib', build_sdk.SHIP),
	('native/libs/kernel32.lib', build_sdk.SHIP),
	('Shalltear.Conformance.dll', build_sdk.DROP),
	('Shalltear.Conformance.Audio.dll', build_sdk.DROP),
	('Shalltear.Conformance.Editor.dll', build_sdk.DROP),
)

CSHARP_UNGOVERNED = (
	'Mono.Cecil.dll',
	'Renamer/Shalltear.Renamer.pdb',
	'Renamer/Shalltear.Renamer.deps.json',
	'Refactor/Shalltear.Refactor.pdb',
	'Refactor/Shalltear.Refactor.deps.json',
	'HeaderBinder/Shalltear.HeaderBinder.pdb',
	'HeaderBinder/Shalltear.HeaderBinder.deps.json',
	'Microsoft.CodeAnalysis.dll',
	'Shalltear.Runtime.deps.json',
	'Shalltear.Runtime.pdb',
	'Shalltear.Physics3D.pdb',
	'Shalltear.Physics3D.deps.json',
	'ref/Shalltear.Runtime.dll',
	'native/libs/kernel32.a',
	'native/lld-link.exe',
	'aot/Shalltear.ConformanceAOT.dll',
)

CSHARP_LICENSES = (
	build_sdk.ENGINE_LICENSE,
	build_sdk.LLVM_LICENSE,
	build_sdk.MINGW_RUNTIME_LICENSE,
	build_sdk.MONO_CECIL_LICENSE,
	build_sdk.ROSLYN_LICENSE,
)

LIBCLANG_CASES = (
	('LICENSE.TXT', build_sdk.SHIP),
	('bin/libclang.dll', build_sdk.SHIP),
	('lib/libclang.so', build_sdk.SHIP),
	('lib/libclang.so.23.1', build_sdk.SHIP),
	('python/clang/cindex.py', build_sdk.DROP),
	('.fetched', build_sdk.DROP),
)

LIBCLANG_UNGOVERNED = (
	'bin/clang.exe',
	'lib/libLLVM.a',
	'include/clang-c/Index.h',
)


def check_libclang_rules(fail):
	checks = 0
	for relative, expected in LIBCLANG_CASES:
		rule = build_sdk.classify(relative, build_sdk.LIBCLANG_RULES)
		if rule is None or rule.verdict is not expected:
			fail("libclang/%s: expected %s" % (relative, 'SHIP' if expected else 'DROP'))
		checks += 1
	for relative in LIBCLANG_UNGOVERNED:
		if build_sdk.classify(relative, build_sdk.LIBCLANG_RULES) is not None:
			fail("libclang/%s: an unknown file must stop the package" % relative)
		checks += 1
	libclang_prebuilt = os.path.join(build_sdk.PREBUILT_DIR, 'libclang')
	if os.path.isdir(libclang_prebuilt):
		unclassified = []
		for directory, _, files in os.walk(libclang_prebuilt):
			for name in files:
				relative = os.path.relpath(os.path.join(directory, name), libclang_prebuilt).replace(os.sep, '/')
				if build_sdk.classify(relative, build_sdk.LIBCLANG_RULES) is None:
					unclassified.append(relative)
		if unclassified:
			fail("%d path(s) in External/Prebuilt/libclang match no rule: %s"
				 % (len(unclassified), ', '.join(sorted(unclassified)[:5])))
		checks += 1
	return checks


def check_csharp_rules(fail):
	checks = 0
	for relative, expected in CSHARP_CASES:
		rule = build_sdk.classify(relative, build_sdk.CSHARP_RULES)
		if rule is None or rule.verdict is not expected:
			fail("CSharp/%s: expected %s" % (relative, 'SHIP' if expected else 'DROP'))
		checks += 1
	for relative in CSHARP_UNGOVERNED:
		if build_sdk.classify(relative, build_sdk.CSHARP_RULES) is not None:
			fail("CSharp/%s: an unknown file must stop the package" % relative)
		checks += 1
	for rule in build_sdk.CSHARP_RULES:
		if rule.verdict is build_sdk.SHIP and rule.license not in CSHARP_LICENSES:
			fail("a CSharp/ rule ships under %r. Add the Component to Scripts/generate_notices.py first" % rule.license)
		checks += 1
	builtins = build_sdk.classify('native/libs/clang_rt.builtins-x86_64.lib', build_sdk.CSHARP_RULES)
	if builtins is None or builtins.license != build_sdk.LLVM_LICENSE:
		fail("compiler-rt's builtins ship under LLVM's license, not the import libs'")
	checks += 1

	def staged(names):
		directory = tempfile.mkdtemp()
		for name in names:
			target = os.path.join(directory, name)
			os.makedirs(os.path.dirname(target), exist_ok=True)
			with open(target, 'w', encoding='utf-8') as out:
				out.write(name)
		left = None
		try:
			with contextlib.redirect_stdout(io.StringIO()):
				build_sdk.stage_csharp(directory)
			left = sorted(os.path.relpath(os.path.join(root, name), directory).replace(os.sep, '/')
						  for root, _, files in os.walk(directory) for name in files)
		except SystemExit:
			pass
		finally:
			shutil.rmtree(directory, True)
		return left

	built = [relative for relative, _ in CSHARP_CASES]
	shipped = sorted(relative for relative, verdict in CSHARP_CASES if verdict is build_sdk.SHIP)
	if staged(built) != shipped:
		fail("staging CSharp/ should leave %s" % ', '.join(shipped))
	if staged(built + ['Mono.Cecil.dll']) is not None:
		fail("staging CSharp/ accepted a file no rule governs")
	if staged(built[1:]) is not None:
		fail("staging CSharp/ accepted a build without %s" % built[0])
	if staged([name for name in built if name != 'native/CRTShim.obj']) is not None:
		fail("staging CSharp/ accepted a build without the link shim")
	return checks + 4


def check_real_tree(fail):
	checks = 0
	unclassified, shipped = [], 0
	for directory, _, files in os.walk(PREBUILT):
		for name in files:
			relative = os.path.relpath(os.path.join(directory, name), PREBUILT).replace(os.sep, '/')
			rule = build_sdk.classify(relative)
			if rule is None:
				unclassified.append(relative)
			elif rule.verdict is build_sdk.SHIP:
				shipped += 1
	if unclassified:
		fail("%d path(s) in the fetched llvm-mingw match no rule, starting with %s. "
			 "Run with --list-unclassified" % (len(unclassified), ', '.join(sorted(unclassified)[:3])))
	checks += 1

	for relative in GPL_PROFILING:
		if not os.path.exists(os.path.join(PREBUILT, relative.replace('/', os.sep))):
			continue
		rule = build_sdk.classify(relative)
		if rule is None or rule.verdict is not build_sdk.DROP:
			fail("%s exists in the prebuilt and isn't dropped" % relative)
		checks += 1

	share = os.path.join(PREBUILT, 'x86_64-w64-mingw32', 'share', 'mingw32')
	for name in ('COPYING.LGPLv2.1', 'COPYING.GPLv2'):
		if not os.path.exists(os.path.join(share, name)):
			fail("%s isn't staged. Run python Scripts/fetch_prebuilt.py --toolchain" % name)
		checks += 1

	if shipped < 4000:
		fail("only %d files classify as shipped. The rules can't be describing "
			 "a working toolchain" % shipped)
	checks += 1
	print("  classified %d files, %d of them shipped" % (
		sum(len(f) for _, _, f in os.walk(PREBUILT)), shipped))
	return checks


def list_unclassified():
	if not os.path.isdir(PREBUILT):
		print("the prebuilt is absent: python Scripts/fetch_prebuilt.py --toolchain")
		return 1
	found = []
	for directory, _, files in os.walk(PREBUILT):
		for name in files:
			relative = os.path.relpath(os.path.join(directory, name), PREBUILT).replace(os.sep, '/')
			if build_sdk.classify(relative) is None:
				found.append(relative)
	for relative in sorted(found):
		print(relative)
	print("%d unclassified" % len(found), file=sys.stderr)
	return 0


def check_archive_pruning(fail):
	keeping = 'Shalltear-dev-3ec63f529c4c-windows-x64.zip'
	cases = (
		('Shalltear-dev-d1665b421639-windows-x64.zip', True, "an earlier commit's archive"),
		(keeping, False, 'the archive just written gets published'),
		('notes.txt', False, 'this script did not write it'),
		('Shalltear-1.0-windows-x64.zip', False, 'a release name this script does not compose'),
		('Shalltear-dev-SOMETHINGELSE-windows-x64.zip', False, 'not a commit hash'),
		('Shalltear-dev-d1665b421639-linux-x64.zip', False, 'another platform, not ours to remove'),
	)
	names = [name for name, _, _ in cases]
	removed = set(build_sdk.superseded_archives(names, keeping))
	for name, expected, why in cases:
		if (name in removed) is not expected:
			fail("%s: expected it to be %s -> %s"
				 % (name, 'removed' if expected else 'kept', why))

	survivors = [name for name in names
				 if name not in removed and build_sdk.ARCHIVE_NAME.match(name)]
	if survivors != [keeping]:
		fail("Dist/ would hold %r. The publisher accepts only one" % survivors)
	return len(cases) + 1


def main():
	if '--list-unclassified' in sys.argv:
		return list_unclassified()

	failures = []

	def fail(message):
		failures.append(message)

	total = check_rules(fail)
	total += check_csharp_rules(fail)
	total += check_libclang_rules(fail)
	total += check_archive_pruning(fail)
	if os.path.isdir(PREBUILT):
		total += check_real_tree(fail)
	else:
		print("SKIPPED: the whole-tree checks need External/Prebuilt/llvm-mingw")

	if failures:
		for message in failures:
			print("FAIL  " + message)
		print("\n%d of %d checks failed" % (len(failures), total))
		return 1
	print("OK - %d checks passed" % total)
	return 0


if __name__ == "__main__":
	sys.exit(main())
