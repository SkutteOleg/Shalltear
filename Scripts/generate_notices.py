#!/usr/bin/env python3
"""Generate the THIRD-PARTY-NOTICES files from dependency set
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
FULL_OUTPUT = ROOT / "THIRD-PARTY-NOTICES.txt"
RUNTIME_OUTPUT = ROOT / "THIRD-PARTY-NOTICES-Runtime.txt"

WIDTH = 76
RULE = "=" * WIDTH


def read(relative: str) -> str:
	path = ROOT / relative
	if not path.exists():
		sys.exit(
			f"missing license input: {relative}\n"
			f"Submodules must be checked out (git submodule update --init --recursive)\n"
			f"+ prebuilts fetched (python Scripts/fetch_prebuilt.py) before the\n"
			f"notices can be generated."
		)
	return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def capture(relative: str, pattern: str) -> tuple[str, ...]:
	match = re.search(pattern, read(relative), re.DOTALL)
	if not match:
		sys.exit(
			f"couldn't read the version out of {relative}\n"
			f"pattern: {pattern}\n"
			f"The upstream layout changed. Fix the pattern."
		)
	return match.groups()


def git_blob(relative: str) -> str:
	read(relative)
	data = (ROOT / relative).read_bytes()
	return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()[:8]


def submodule_head(relative: str) -> str:
	gitfile = read(relative + "/.git").strip()
	if not gitfile.startswith("gitdir: "):
		sys.exit(f"{relative}/.git isn't a submodule's gitdir pointer")
	gitdir = (ROOT / relative / gitfile[len("gitdir: "):]).resolve()
	head = (gitdir / "HEAD").read_text(encoding="utf-8").strip()
	if head.startswith("ref: "):
		head = (gitdir / head[len("ref: "):]).read_text(encoding="utf-8").strip()
	if not re.fullmatch(r"[0-9a-f]{40}", head):
		sys.exit(f"{relative}: HEAD isn't a commit: {head!r}")
	return head[:8]


def slice_between(relative: str, start: str, end: str) -> str:
	text = read(relative)
	first = text.find(start)
	if first < 0:
		sys.exit(f"{relative}: start marker not found: {start!r}")
	last = text.find(end, first + len(start))
	if last < 0:
		sys.exit(f"{relative}: end marker not found: {end!r}")
	return text[first:last].rstrip()


def comment_block(relative: str, stop: str) -> str:
	lines = []
	for line in read(relative).split("\n"):
		stripped = line.strip()
		if stripped.startswith("/*"):
			stripped = stripped[2:].strip()
		if stripped.startswith("*/") or (stop and stop in stripped):
			break
		if stripped.startswith("*"):
			stripped = stripped[1:]
			if stripped.startswith(" "):
				stripped = stripped[1:]
		lines.append(stripped.rstrip())
	while lines and not lines[0]:
		lines.pop(0)
	while lines and not lines[-1]:
		lines.pop()
	return "\n".join(lines)


@dataclass(frozen=True)
class Component:
	name: str
	scope: str
	license: str
	where: str
	version: Callable[[], str]
	text: Callable[[], str]
	extra: tuple[str, ...] = field(default=())


FREETYPE_H = "External/Submodules/freetype/include/freetype/freetype.h"

MINGW_SHARE = "External/Prebuilt/llvm-mingw/x86_64-w64-mingw32/share/mingw32/"


def freetype_version() -> str:
	major, minor, patch = capture(
		FREETYPE_H,
		r"#define FREETYPE_MAJOR\s+(\d+).*?#define FREETYPE_MINOR\s+(\d+).*?#define FREETYPE_PATCH\s+(\d+)",
	)
	return f"{major}.{minor}.{patch}"


def freetype_year() -> str:
	(year,) = capture(FREETYPE_H, r"Copyright \(C\) \d{4}-(\d{4}) by")
	return year


CMAKE_VENDORED = (
	("curl", "cmcurl/COPYING"),
	("libarchive", "cmlibarchive/COPYING"),
	("liblzma", "cmliblzma/COPYING"),
	("librhash", "cmlibrhash/COPYING"),
	("libuv", "cmlibuv/LICENSE"),
	("nghttp2", "cmnghttp2/COPYING"),
	("cppdap", "cmcppdap/NOTICE"),
	("KWSys", "cmsys/Copyright.txt"),
	("zlib", "cmzlib/Copyright.txt"),
	("zstd", "cmzstd/LICENSE"),
)


def cmake_notices() -> str:
	parts = [read("External/Prebuilt/cmake/doc/cmake/LICENSE.rst")]
	parts.append(read("External/Prebuilt/cmake/doc/cmake/CONTRIBUTORS.rst"))
	for name, relative in CMAKE_VENDORED:
		heading = f"--- CMake bundles {name}, under: ---"
		parts.append(heading + "\n\n" + read(f"External/Prebuilt/cmake/doc/cmake/{relative}"))
	return "\n\n".join(part.strip("\n") for part in parts)


ICON_SOURCES = "Scripts/IconBake/Icons"
FONT_AWESOME_LICENSE = ICON_SOURCES + "/fontawesome-free-LICENSE"
FONT_AWESOME_RULE = "\n" + "-" * 80 + "\n"


def font_awesome_free_versions() -> str:
	versions = set()
	for path in sorted((ROOT / ICON_SOURCES).glob("*.svg")):
		match = re.search(r"Font Awesome Free (\d+(?:\.\d+)+) by @fontawesome", read(f"{ICON_SOURCES}/{path.name}"))
		if match:
			versions.add(match.group(1))
	if not versions:
		sys.exit(f"no Font Awesome Free source under {ICON_SOURCES} (sources moved or this entry is stale)")
	return ", ".join(sorted(versions))


def font_awesome_notice() -> str:
	notices = set()
	for path in sorted((ROOT / ICON_SOURCES).glob("*.svg")):
		notices.update(re.findall(r"Copyright \d{4} Fonticons, Inc\.", read(f"{ICON_SOURCES}/{path.name}")))
	if not notices:
		sys.exit(f"no Fonticons copyright notice in source under {ICON_SOURCES}")
	return "\n\n".join((
		"\n".join(sorted(notices)),
		slice_between(FONT_AWESOME_LICENSE, "Fonticons, Inc.", FONT_AWESOME_RULE + "\n# Fonts:"),
		slice_between(FONT_AWESOME_LICENSE, "# Attribution", FONT_AWESOME_RULE + "\n# Brand Icons"),
	))


COMPONENTS: tuple[Component, ...] = (
	Component(
		name="FreeType",
		scope="runtime",
		license="The FreeType License (FTL), elected over GPLv2",
		where="glyph rasterization for text engine draws. "
			  "Linked into engine runtime",
		version=freetype_version,
		text=lambda: read("External/Submodules/freetype/docs/FTL.TXT"),
		extra=(
			"This software is based in part on the work of the FreeType Team.",
			"Portions of this software are copyright (c) {freetype_year} The FreeType",
			"Project (https://freetype.org).  All rights reserved.",
		),
	),
	Component(
		name="zlib",
		scope="runtime",
		license="zlib license",
		where="bundled inside FreeType as src/gzip, compiled in. "
			  "FT_DISABLE_ZLIB only declines a SYSTEM zlib (doesn't remove this one)",
		version=lambda: capture(
			"External/Submodules/freetype/src/gzip/zlib.h", r"version (\d+\.\d+\.\d+),"
		)[0],
		text=lambda: slice_between(
			"External/Submodules/freetype/src/gzip/zlib.h",
			"Copyright (C) 1995",
			"The data format used by the zlib library",
		),
	),
	Component(
		name="FreeType BDF driver",
		scope="runtime",
		license="MIT-style (X Window System)",
		where="a FreeType font driver, registered in ftmodule.h -> linked",
		version=freetype_version,
		text=lambda: comment_block(
			"External/Submodules/freetype/src/bdf/bdflib.c", "***********"
		),
	),
	Component(
		name="FreeType PCF driver",
		scope="runtime",
		license="MIT-style (X Window System)",
		where="a FreeType font driver, registered in ftmodule.h -> linked",
		version=freetype_version,
		text=lambda: comment_block(
			"External/Submodules/freetype/src/pcf/pcfread.c", "***********"
		),
	),
	Component(
		name="stb_image",
		scope="runtime",
		license="MIT or public domain (dual), either may be chosen",
		where="PNG/JPG/TGA/HDR decoding, vendored into the engine runtime",
		version=lambda: capture(
			"Source/Core/Private/Graphics/stb_image.h", r"stb_image - v([\d.]+)"
		)[0],
		text=lambda: slice_between(
			"Source/Core/Private/Graphics/stb_image.h",
			"This software is available under 2 licenses",
			"*/",
		),
	),
	Component(
		name="miniaudio",
		scope="runtime",
		license="public domain (Unlicense) or MIT No Attribution (dual), either may be chosen",
		where="audio package's output device layer (vendored). Included when "
			  "dev.shalltear.audio is enabled. The "
			  "editor also runs its WAV/FLAC/MP3 decoders at import",
		version=lambda: capture(
			"Source/Audio/Private/ThirdParty/miniaudio.h", r"miniaudio - v([\d.]+)"
		)[0],
		text=lambda: slice_between(
			"Source/Audio/Private/ThirdParty/miniaudio.h",
			"This software is available as a choice of the following licenses",
			"*/",
		),
	),
	Component(
		name="QOA (Quite OK Audio)",
		scope="runtime",
		license="MIT",
		where="audio package's compressed clip format: reference encoder "
			  "runs at import. Included when dev.shalltear.audio "
			  "is enabled",
		version=lambda: "upstream blob " + git_blob("Source/Audio/Private/ThirdParty/qoa.h"),
		text=lambda: read("Source/Audio/Private/ThirdParty/qoa-LICENSE"),
	),
	Component(
		name="Interactive audio container",
		scope="runtime",
		license="MIT",
		where="audio package's interactive container "
			  "(Source/Audio/Private/Containers/AudioInteractivePlayback.cpp) follows "
			  "an MIT-licensed implementation. Included when "
			  "dev.shalltear.audio is enabled",
		version=lambda: "derived, not vendored",
		text=lambda: read("Source/Audio/Private/Containers/AudioInteractivePlayback.LICENSE.txt"),
	),
	Component(
		name="Opus (libopus)",
		scope="runtime",
		license="BSD-3-Clause, with the royalty-free patent licenses in its text",
		where="audio package's streamed codec (compiled in). Decoder included "
			  "when dev.shalltear.audio is enabled. The editor "
			  "also runs the encoder at import",
		version=lambda: "upstream commit " + submodule_head("External/Submodules/opus"),
		text=lambda: read("External/Submodules/opus/COPYING"),
	),
	Component(
		name="Box3D",
		scope="runtime",
		license="MIT",
		where="3D physics package's simulation (compiled in). Included "
			  "when dev.shalltear.physics3d is enabled",
		version=lambda: "fork commit " + submodule_head("External/Submodules/box3d"),
		text=lambda: read("External/Submodules/box3d/LICENSE"),
	),
	Component(
		name="Vulkan Memory Allocator",
		scope="runtime",
		license="MIT",
		where="GPU memory allocation behind the RHI, linked into the engine runtime",
		version=lambda: "%s.%s.%s" % capture(
			"External/Submodules/VulkanMemoryAllocator/include/vk_mem_alloc.h",
			r"#define VMA_VERSION \(VK_MAKE_VERSION\((\d+), (\d+), (\d+)\)\)",
		),
		text=lambda: read("External/Submodules/VulkanMemoryAllocator/LICENSE.txt"),
	),
	Component(
		name="Vulkan-Headers",
		scope="runtime",
		license="MIT, elected from Apache-2.0 OR MIT",
		where="the Khronos API headers the renderer is compiled using",
		version=lambda: "1.4." + capture(
			"External/Submodules/Vulkan-Headers/include/vulkan/vulkan_core.h",
			r"#define VK_HEADER_VERSION (\d+)\n",
		)[0],
		text=lambda: read("External/Submodules/Vulkan-Headers/LICENSES/MIT.txt"),
	),
	Component(
		name="Inter",
		scope="runtime",
		license="SIL Open Font License 1.1",
		where="the engine's default UI font. FontAsset::Cook writes font bytes "
			  "verbatim. Shipped when the cook's closure reaches the font",
		version=lambda: "vendored unmodified",
		text=lambda: read("EngineAssets/Fonts/Inter/OFL.txt"),
		extra=(
			"The font may not be sold by itself, and a modified version may not",
			"use the Reserved Font Name.",
		),
	),
	Component(
		name="GPU-Noise-Lib",
		scope="runtime",
		license="no license. Use permitted with credit to the author",
		where="noise operators of effect graphs, emitted by "
			  "Source/Graphs/Private/GraphHLSL.cpp, derived from Brian Sharpe's "
			  "GPU-Noise-Lib",
		version=lambda: "derived, not vendored",
		text=lambda: read("Source/Graphs/Private/GraphHLSL.LICENSE.txt"),
	),
	Component(
		name="libc++",
		scope="runtime",
		license="Apache License 2.0 with LLVM Exceptions",
		where="the C++ standard library. Shipped as libc++.dll beside binaries "
			  "the engine produces (including a built game)",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: read("External/Prebuilt/llvm-mingw/LICENSE.TXT"),
		extra=(
			"Shipped as a separate DLL (not embedded in compiled output),",
			"so the LLVM Exception does not apply and Section 4's conditions do.",
		),
	),
	Component(
		name="libunwind (LLVM)",
		scope="runtime",
		license="Apache License 2.0 with LLVM Exceptions",
		where="stack unwinding for C++ exceptions. Shipped as libunwind.dll "
			  "beside binaries, imported by libc++",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: read("External/Prebuilt/llvm-mingw/LICENSE.TXT"),
		extra=(
			"Shipped as a separate DLL. The LLVM Exception doesn't apply",
			"and Section 4's conditions do.",
		),
	),
	Component(
		name="mingw-w64 runtime",
		scope="runtime",
		license="ZPL 2.1 and public domain, per the notice below",
		where="C runtime glue every bin statically linked against: "
			  "startup objects/pseudo-relocation support + the UCRT "
			  "shims (inside the executable, not a separate file)",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: read(
			"External/Prebuilt/llvm-mingw/x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"
		),
	),
	Component(
		name="ufbx",
		scope="editor",
		license="MIT, elected from MIT OR Unlicense",
		where="FBX import. Lives in ImportPipeline, a tool package",
		version=lambda: "%s.%s.%s" % capture(
			"External/Submodules/ufbx/ufbx.h",
			r"#define UFBX_HEADER_VERSION ufbx_pack_version\((\d+), (\d+), (\d+)\)",
		),
		text=lambda: read("External/Submodules/ufbx/LICENSE"),
	),
	Component(
		name="stb_vorbis",
		scope="editor",
		license="MIT or public domain (dual), either may be chosen",
		where="decodes a source .ogg at import, in the audio package's editor "
			  "half",
		version=lambda: capture(
			"Source/AudioEditor/Private/ThirdParty/stb_vorbis.c", r"Ogg Vorbis audio decoder - v([\d.]+)"
		)[0],
		text=lambda: slice_between(
			"Source/AudioEditor/Private/ThirdParty/stb_vorbis.c",
			"This software is available under 2 licenses",
			"*/",
		),
	),
	Component(
		name="bc7e (scalar port, from Basis Universal)",
		scope="editor",
		license="Apache License 2.0",
		where="encodes BC7 at texture import. Lives in ImportPipeline, a tool "
			  "package",
		version=lambda: "basis_universal v2_50, upstream blob "
						+ git_blob("Source/ImportPipeline/Private/ThirdParty/bc7e/basisu_bc7e_scalar.cpp"),
		text=lambda: read("Source/ImportPipeline/Private/ThirdParty/bc7e/basisu-LICENSE").rstrip("\n")
					 + "\n\n==== Basis Universal's NOTICE file ====\n\n"
					 + read("Source/ImportPipeline/Private/ThirdParty/bc7e/basisu-NOTICE"),
	),
	Component(
		name="bc7enc_rdo (rgbcx and bc7decomp)",
		scope="editor",
		license="MIT, elected from MIT OR Unlicense",
		where="encodes BC5 and decodes BC5/BC7 at texture import and in the "
			  "editor's preview. Lives in ImportPipeline, a tool package",
		version=lambda: "rgbcx v%s, upstream commit b9438627" % capture(
			"Source/ImportPipeline/Private/ThirdParty/bc7enc_rdo/rgbcx.h", r"// rgbcx\.h v([\d.]+)"
		),
		text=lambda: read("Source/ImportPipeline/Private/ThirdParty/bc7enc_rdo/bc7enc_rdo-LICENSE"),
	),
	Component(
		name="Compressonator CMP_Core (BC6H encoder)",
		scope="editor",
		license="MIT",
		where="encodes BC6H at texture import. Lives in ImportPipeline, a tool "
			  "package",
		version=lambda: "Compressonator V4.5.52, upstream blob "
						+ git_blob("Source/ImportPipeline/Private/ThirdParty/cmp_core/bc6_encode_kernel.cpp"),
		text=lambda: read("Source/ImportPipeline/Private/ThirdParty/cmp_core/cmp_core-LICENSE"),
	),
	Component(
		name="DirectX Shader Compiler (dxcompiler.dll)",
		scope="editor",
		license="University of Illinois/NCSA Open Source License",
		where="HLSL to SPIR-V at import time. Runtime-loaded by the editor only",
		version=lambda: read("External/Prebuilt/dxc/.fetched").strip(),
		text=lambda: read("External/Prebuilt/dxc/LICENSE-LLVM.txt"),
		extra=(
			"Microsoft's binary release assigns this license to the files it",
			"ships except d3d12shader.h (Shalltear doesn't distribute it).",
		),
	),
	Component(
		name="Microsoft.CodeAnalysis.CSharp / .NET Runtime (Roslyn)",
		scope="editor",
		license="MIT",
		where="C# to C++20 transpilation at script compile time. Bundled in "
			  "Roslyn/ and runtime-loaded by the editor only (never shipped "
			  "in a built game)",
		version=lambda: capture(
			"External/RoslynTranspiler/ShalltearRoslyn.csproj",
			r'Include="Microsoft\.CodeAnalysis\.CSharp"\s+Version="([^"]+)"',
		)[0] + " / .NET " + read("External/Prebuilt/dotnet/.fetched").strip(),
		text=lambda: "\n\n".join((
			read("External/RoslynTranspiler/LICENSE-Roslyn.txt"),
			"==== .NET Runtime (bundled in Roslyn/dotnet) ====\n\n"
			+ read("External/Prebuilt/dotnet/LICENSE.txt"),
		)),
	),
	Component(
		name="Clang, LLD and the LLVM toolchain (bundled in Toolchain/LLVM)",
		scope="editor",
		license="Apache License 2.0 with LLVM Exceptions",
		where="the C++ compiler/linker/binutils the SDK ships (a user "
			  "can build scripts without installing a toolchain). Shipped as "
			  "executables, not compiled into whatever.",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: read("External/Prebuilt/llvm-mingw/LICENSE.TXT"),
		extra=(
			"Redistributed as the tools themselves (not their output),",
			"so the LLVM Exception does not apply and Section 4's conditions do.",
		),
	),
	Component(
		name="mingw-w64 headers, import libraries and winpthreads (bundled)",
		scope="editor",
		license="Public domain and ZPL 2.1. 93 Wine-derived headers are LGPL-2.1+ "
				"+ 1 header is GPLv2+ with GCC's header exception",
		where="sysroot bundled clang compiles against. NONE of it is "
			  "compiled into the engine -> two LGPL headers the engine's "
			  "translation units would've reached are declined at build time "
			  "-> but the SDK distributes the files (so "
			  "their notices travel)",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: "\n\n".join((
			read(MINGW_SHARE + "COPYING.MinGW-w64.txt"),
			read(MINGW_SHARE + "COPYING.winpthreads.txt"),
			"==== The DirectX and DDK headers above are under, in full ====\n\n"
			+ read(MINGW_SHARE + "COPYING.LGPLv2.1"),
			"==== GCC's stdarg.h is under, in full ====\n\n"
			+ read(MINGW_SHARE + "COPYING.GPLv2"),
		)),
		extra=(
			"The GCC and GNU binutils rows in the notice below describe the",
			"mingw-w64 project (not this bundle). llvm-mingw contains neither, and",
			"the profiling code that row covers is not staged either.",
			"93 headers are LGPL-2.1+ and are redistributed in SOURCE form,",
			"unmodified, with their notices intact. stdarg.h is GPLv2+ and has",
			"GCC's exception permitting inclusion without affecting your output.",
		),
	),
	Component(
		name="winstorecompat",
		scope="editor",
		license="MIT",
		where="a mingw-w64 compatibility shim in the bundled sysroot "
			  "(libwinstorecompat.a). Nothing links it. It exists for Windows "
			  "Store targets. But the SDK distributes the file",
		version=lambda: "llvm-mingw " + read("External/Prebuilt/llvm-mingw/.fetched").strip(),
		text=lambda: read(MINGW_SHARE + "COPYING.winstorecompat.txt"),
	),
	Component(
		name="CMake (bundled in Toolchain/CMake)",
		scope="editor",
		license="BSD-3-Clause + the licenses of the 10 components it vendors",
		where="the build-system generator the editor spawns to compile a user's "
			  "script module. Shipped as an executable",
		version=lambda: "CMake " + read("External/Prebuilt/cmake/.fetched").strip(),
		text=cmake_notices,
	),
	Component(
		name="Ninja (bundled in Toolchain/Ninja)",
		scope="editor",
		license="Apache License 2.0",
		where="the build tool CMake generates for. Shipped as 1 executable",
		version=lambda: "Ninja " + read("External/Prebuilt/ninja/.fetched").strip(),
		text=lambda: read("External/Prebuilt/ninja/COPYING"),
	),
	Component(
		name="Source Code Pro",
		scope="editor",
		license="SIL Open Font License 1.1",
		where="the editor's monospace face",
		version=lambda: "vendored unmodified (static Regular instance)",
		text=lambda: read("EngineAssets/Fonts/SourceCodePro/OFL.txt"),
		extra=(
			"The font may not be sold by itself, and a modified version may not",
			"use the Reserved Font Name 'Source'.",
		),
	),
	Component(
		name="Font Awesome Free (icons)",
		scope="editor",
		license="CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)",
		where="most of editor's chrome icons, converted from Font Awesome "
			  "Free's SVG files to vector geo compiled into the editor "
			  "library. The rest of the set is original artwork. No font file "
			  "is distributed",
		version=font_awesome_free_versions,
		text=font_awesome_notice,
		# CC BY 4.0 section 3(a)(1)(B), shared adaptation should say what changed
		extra=(
			"Changes made: each icon's SVG outline is converted to the editor's",
			"vector geometry and placed in a padded box. The outlines themselves",
			"are not modified.",
		),
	),
)


PREAMBLE = """\
{rule}
THIRD-PARTY NOTICES FOR SHALLTEAR
{rule}

Generated by Scripts/generate_notices.py -> do not edit manually.

Shalltear itself is covered by LICENSE.txt (which is a separate file and is not
reproduced here). This file lists the third-party components included in
{artifact}.

MOST OF THEM ARE COMPILED INTO THE BINARIES, not shipped as separate
files (you may not find a copy of the library beside the executable). That
is still redistribution. Handing someone a binary hands them a copy of the code
compiled into it! This is what these licenses mean when they say
"redistribution in binary form". It is also why the notices have to travel.

{scope_note}

Nothing below restricts what you may do with anything you build. These are
attribution/notice terms.

"""

FULL_SCOPE_NOTE = """\
This is the notices file for the editor/the SDK. A game you build and ship
may carry a SMALLER set, and its own THIRD-PARTY-NOTICES.txt is staged beside the
executable (Windows so far) when the game is built. Ship that file with your game.
"""

RUNTIME_SCOPE_NOTE = """\
Notices file for a built game.
"""


def contents(components: list[Component]) -> str:
	lines = ["CONTENTS", ""]
	for index, component in enumerate(components, start=1):
		lines.append(f"  {index:>2}. {component.name}")
	return "\n".join(lines) + "\n"


def section(index: int, component: Component) -> str:
	substitutions = {"freetype_year": freetype_year()}
	lines = [
		RULE,
		f"{index}. {component.name} -> {component.version()}",
		RULE,
		"",
		f"License: {component.license}",
		"",
	]
	for wrapped in wrap(component.where, "Where: "):
		lines.append(wrapped)
	lines.append("")
	if component.extra:
		for raw in component.extra:
			lines.append(raw.format(**substitutions))
		lines.append("")
	lines.append(component.text().strip("\n"))
	lines.append("")
	return "\n".join(lines)


def wrap(body: str, prefix: str) -> list[str]:
	indent = " " * len(prefix)
	out: list[str] = []
	current = prefix
	for word in body.split():
		if len(current) + len(word) + 1 > WIDTH and current.strip() not in ("", prefix.strip()):
			out.append(current.rstrip())
			current = indent
		current += word + " "
	out.append(current.rstrip())
	return out


def emit(scope_filter: tuple[str, ...], artifact: str, scope_note: str) -> str:
	selected = [c for c in COMPONENTS if c.scope in scope_filter]
	body = PREAMBLE.format(rule=RULE, artifact=artifact, scope_note=scope_note)
	body += contents(selected) + "\n"
	for index, component in enumerate(selected, start=1):
		body += section(index, component) + "\n"
	return body.rstrip("\n") + "\n"


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--check", action="store_true",
						help="fail if the committed output differs (don't rewrite it)")
	args = parser.parse_args()

	outputs = {
		FULL_OUTPUT: emit(
			("runtime", "editor"),
			"the Shalltear editor and SDK",
			FULL_SCOPE_NOTE,
		),
		RUNTIME_OUTPUT: emit(
			("runtime",),
			"a game built with Shalltear",
			RUNTIME_SCOPE_NOTE,
		),
	}

	if args.check:
		stale = [
			path.relative_to(ROOT).as_posix()
			for path, text in outputs.items()
			if not path.exists() or path.read_bytes() != text.encode("utf-8")
		]
		if stale:
			sys.exit(
				"stale third-party notices: " + ", ".join(stale) + "\n"
				"A dependency, its version/license text changed. Re-run:\n"
				"    python Scripts/generate_notices.py\n"
				"and commit the result."
			)
		print(f"third-party notices are current ({len(COMPONENTS)} component(s))")
		return

	for path, text in outputs.items():
		path.write_bytes(text.encode("utf-8"))
		print(f"{path.relative_to(ROOT).as_posix()}  {len(text.encode('utf-8')):,} B")


if __name__ == "__main__":
	main()
