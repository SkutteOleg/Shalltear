#!/usr/bin/env python3
"""Fetches pinned, SHA-verified prebuilt binaries consumed at runtime
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
PREBUILT_DIR = ROOT_DIR / "External" / "Prebuilt"

DXC_VERSION = "v1.9.2602.24"
DXC_URL = f"https://github.com/microsoft/DirectXShaderCompiler/releases/download/{DXC_VERSION}/dxc_2026_05_27.zip"
DXC_SHA256 = "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4"

LAVAPIPE_VERSION = "26.1.3"
LAVAPIPE_URL = f"https://github.com/pal1000/mesa-dist-win/releases/download/{LAVAPIPE_VERSION}/mesa3d-{LAVAPIPE_VERSION}-release-msvc.7z"
LAVAPIPE_SHA256 = "6dd431f4620cea73970b13e3ffa94f721f2a3924306b8a4283c97648cdb6eb9c"

LLVM_MINGW_VERSION = "20260908"
LLVM_MINGW_ARCHIVE = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-x86_64"
LLVM_MINGW_URL = (
    f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VERSION}/{LLVM_MINGW_ARCHIVE}.zip"
)
LLVM_MINGW_SHA256 = "1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6"

CMAKE_VERSION = "4.2.0"
CMAKE_ARCHIVE = f"cmake-{CMAKE_VERSION}-windows-x86_64"
CMAKE_URL = f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/{CMAKE_ARCHIVE}.zip"
CMAKE_SHA256 = "cf35a516c4f5f4646b301e51c8e24b168cc012c3b1453b8f675303b54eb0ef45"

NINJA_VERSION = "1.13.2"
NINJA_URL = f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip"
NINJA_SHA256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"
NINJA_COPYING_URL = f"https://raw.githubusercontent.com/ninja-build/ninja/v{NINJA_VERSION}/COPYING"
NINJA_COPYING_SHA256 = "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea"

MINGW_LICENSE_DIR = ROOT_DIR / "Scripts" / "ThirdParty" / "mingw-w64"
MINGW_LGPL21_SHA256 = "20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95"
MINGW_GPL2_SHA256 = "edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6"

VULKANRT_VERSION = "1.4.321.0"
VULKANRT_URL = f"https://sdk.lunarg.com/sdk/download/{VULKANRT_VERSION}/windows/vulkan-runtime-components.zip"
VULKANRT_SHA256 = "f8ba79303face4b72a2f302f1a46e8c918995b54fbf24c0c9fc6c3d3ef525dca"

DOTNET_VERSION = "9.0.320"
DOTNET_URL = f"https://builds.dotnet.microsoft.com/dotnet/Sdk/{DOTNET_VERSION}/dotnet-sdk-{DOTNET_VERSION}-win-x64.zip"
DOTNET_SHA256 = "92dca51e2447d67f14859c87eeeddb8ec58441c43c08e2786cf5a1c52578e863"


def download_verified(url: str, expected_sha256: str, dest: Path) -> None:
    print(f"  downloading {url}")
    # LunarG CDN refuses the urllib default
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Shalltear fetch_prebuilt)"})
    with urllib.request.urlopen(request) as response, open(dest, "wb") as out:
        shutil.copyfileobj(response, out)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    if digest != expected_sha256:
        dest.unlink(missing_ok=True)
        raise RuntimeError(
            f"SHA-256 mismatch for {url}\n  expected {expected_sha256}\n  got      {digest}"
        )
    print(f"  sha256 verified ({digest[:12]}...)")


def is_current(target_dir: Path, version: str) -> bool:
    marker = target_dir / ".fetched"
    return marker.exists() and marker.read_text(encoding="utf-8").strip() == version


def write_marker(target_dir: Path, version: str) -> None:
    (target_dir / ".fetched").write_text(version + "\n", encoding="utf-8")


def fetch_dxc() -> None:
    target = PREBUILT_DIR / "dxc"
    if is_current(target, DXC_VERSION):
        print(f"dxc {DXC_VERSION} already fetched. Skipping")
        return
    print(f"fetching dxc {DXC_VERSION}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "dxc.zip"
        download_verified(DXC_URL, DXC_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        (target / "bin" / "x64").mkdir(parents=True)
        with zipfile.ZipFile(archive) as zf:
            wanted = ["bin/x64/dxcompiler.dll"]
            wanted += [n for n in zf.namelist() if n.startswith("LICENSE")]
            for name in wanted:
                data = zf.read(name)
                out = target / Path(name)
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)
    write_marker(target, DXC_VERSION)
    print(f"dxc -> {target}")


def fetch_lavapipe() -> None:
    target = PREBUILT_DIR / "lavapipe"
    if is_current(target, LAVAPIPE_VERSION):
        print(f"lavapipe {LAVAPIPE_VERSION} already fetched. Skipping")
        return
    seven_zip = shutil.which("7z")
    if not seven_zip:
        raise RuntimeError("lavapipe extraction needs 7z on PATH (present on GitHub runners)")
    print(f"fetching lavapipe (mesa-dist-win {LAVAPIPE_VERSION})")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "mesa.7z"
        download_verified(LAVAPIPE_URL, LAVAPIPE_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        subprocess.run(
            [seven_zip, "x", str(archive), "-o" + str(target), "x64/*", "-y"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        icds = list(target.rglob("lvp_icd.x86_64.json"))
        if not icds:
            raise RuntimeError("lvp_icd.x86_64.json not found in the mesa archive. Layout changed?")
        print(f"  ICD manifest: {icds[0]}")
    write_marker(target, LAVAPIPE_VERSION)
    print(f"lavapipe -> {target}")


def fetch_vulkan_runtime() -> None:
    target = PREBUILT_DIR / "vulkanrt"
    if is_current(target, VULKANRT_VERSION):
        print(f"vulkan runtime {VULKANRT_VERSION} already fetched. Skipping")
        return
    print(f"fetching vulkan runtime {VULKANRT_VERSION}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "vulkanrt.zip"
        download_verified(VULKANRT_URL, VULKANRT_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        with zipfile.ZipFile(archive) as zf:
            for name in zf.namelist():
                if name.endswith("x64/vulkan-1.dll") or name.endswith("VulkanRT-License.txt"):
                    out = target / Path(name).name
                    out.write_bytes(zf.read(name))
    if not (target / "vulkan-1.dll").exists():
        raise RuntimeError("vulkan-1.dll not found in the runtime archive. Layout changed?")
    write_marker(target, VULKANRT_VERSION)
    print(f"vulkan runtime -> {target}")


def ensure_mingw_license_texts(target: Path) -> None:
    share = target / "x86_64-w64-mingw32" / "share" / "mingw32"
    if not share.is_dir():
        raise RuntimeError(f"{share} is absent. The llvm-mingw layout changed?")
    wanted = (
        ("COPYING.LGPLv2.1", MINGW_LGPL21_SHA256),
        ("COPYING.GPLv2", MINGW_GPL2_SHA256),
    )
    for name, digest in wanted:
        destination = share / name
        if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == digest:
            continue
        source = MINGW_LICENSE_DIR / name
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"SHA-256 mismatch for {source}. Run git checkout {source}")
        print(f"  staging {name} beside COPYING.MinGW-w64.txt")
        shutil.copyfile(source, destination)


def fetch_toolchain() -> None:
    target = PREBUILT_DIR / "llvm-mingw"
    if is_current(target, LLVM_MINGW_VERSION):
        print(f"llvm-mingw {LLVM_MINGW_VERSION} already fetched. Skipping")
        ensure_mingw_license_texts(target)
        print(f"  {target / 'bin'}")
        return
    print(f"fetching llvm-mingw {LLVM_MINGW_VERSION} (190 MB, unpacks to ~750 MB)")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "llvm-mingw.zip"
        download_verified(LLVM_MINGW_URL, LLVM_MINGW_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        prefix = LLVM_MINGW_ARCHIVE + "/"
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if not info.filename.startswith(prefix):
                    raise RuntimeError(
                        f"unexpected entry '{info.filename}' in the llvm-mingw archive. Layout changed?"
                    )
                relative = info.filename[len(prefix):]
                if not relative:
                    continue
                out = target / relative
                if info.is_dir():
                    out.mkdir(parents=True, exist_ok=True)
                    continue
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(zf.read(info))
    driver = target / "bin" / "x86_64-w64-mingw32-clang++.exe"
    if not driver.exists():
        raise RuntimeError(f"{driver} is absent after extraction. Layout changed?")
    ensure_mingw_license_texts(target)
    write_marker(target, LLVM_MINGW_VERSION)
    print(f"llvm-mingw -> {target}")
    print("  Build.bat and the Scripts/*.py entry points use it from there.")
    print(f"  Configuring manually? Put {target / 'bin'} on PATH.")


def _cmake_wanted(relative: str) -> bool:
    if relative == "bin/cmake.exe":
        return True
    if relative.startswith("doc/cmake/"):
        return "/html/" not in relative and not relative.endswith((".qch", ".html"))
    if relative.startswith("share/"):
        tail = relative.split("/", 2)[-1]
        return tail.startswith(("Modules/", "Templates/", "Licenses/"))
    return False


def fetch_cmake() -> None:
    target = PREBUILT_DIR / "cmake"
    if is_current(target, CMAKE_VERSION):
        print(f"cmake {CMAKE_VERSION} already fetched. Skipping")
        return
    print(f"fetching cmake {CMAKE_VERSION} (50 MB)")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "cmake.zip"
        download_verified(CMAKE_URL, CMAKE_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        prefix = CMAKE_ARCHIVE + "/"
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if info.is_dir() or not info.filename.startswith(prefix):
                    continue
                relative = info.filename[len(prefix):]
                if not _cmake_wanted(relative):
                    continue
                out = target / relative
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(zf.read(info))
    if not (target / "bin" / "cmake.exe").exists():
        raise RuntimeError("bin/cmake.exe not found in the CMake archive. Layout changed?")
    if not list(target.glob("share/cmake-*/Modules/CMakeDetermineCXXCompiler.cmake")):
        raise RuntimeError("share/cmake-*/Modules is absent")
    if not (target / "doc" / "cmake" / "LICENSE.rst").exists():
        raise RuntimeError("doc/cmake/LICENSE.rst is absent")
    write_marker(target, CMAKE_VERSION)
    print(f"cmake -> {target}")


def fetch_ninja() -> None:
    target = PREBUILT_DIR / "ninja"
    if is_current(target, NINJA_VERSION):
        print(f"ninja {NINJA_VERSION} already fetched. Skipping")
        return
    print(f"fetching ninja {NINJA_VERSION}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "ninja.zip"
        download_verified(NINJA_URL, NINJA_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        with zipfile.ZipFile(archive) as zf:
            (target / "ninja.exe").write_bytes(zf.read("ninja.exe"))
        copying = Path(tmp) / "COPYING"
        download_verified(NINJA_COPYING_URL, NINJA_COPYING_SHA256, copying)
        shutil.copyfile(copying, target / "COPYING")
    write_marker(target, NINJA_VERSION)
    print(f"ninja -> {target}")


def _dotnet_sdk_wanted(relative: str) -> bool:
    if relative in ("dotnet.exe", "LICENSE.txt", "ThirdPartyNotices.txt"):
        return True
    return relative.startswith((
        "host/fxr/",
        "shared/Microsoft.NETCore.App/",
        "packs/Microsoft.NETCore.App.Ref/",
        "packs/Microsoft.NETCore.App.Host.win-x64/",
        "sdk/",
    ))


def fetch_dotnet() -> Path:
    target = PREBUILT_DIR / "dotnet"
    exe = target / "dotnet.exe"
    if is_current(target, DOTNET_VERSION) and exe.exists():
        print(f"dotnet {DOTNET_VERSION} already fetched. Skipping")
        return exe
    print(f"fetching dotnet {DOTNET_VERSION} (portable win-x64)")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "dotnet.zip"
        download_verified(DOTNET_URL, DOTNET_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if info.is_dir() or not _dotnet_sdk_wanted(info.filename):
                    continue
                out = target / info.filename
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(zf.read(info))
    if not exe.exists():
        raise RuntimeError("dotnet.exe not found in the .NET SDK archive. Layout changed?")
    if not (target / "LICENSE.txt").exists():
        raise RuntimeError("LICENSE.txt not found in the .NET SDK archive. Layout changed?")
    write_marker(target, DOTNET_VERSION)
    print(f"dotnet -> {target}")
    return exe


def stage_dotnet_runtime_slice(dotnet_dir: Path, roslyn_bin_dir: Path) -> None:
    runtime_dst = roslyn_bin_dir / "dotnet"
    if runtime_dst.exists():
        shutil.rmtree(runtime_dst)
    for subdir in ("host/fxr", "shared/Microsoft.NETCore.App"):
        src = dotnet_dir / subdir
        if not src.is_dir():
            raise RuntimeError(f"{src} is absent in {dotnet_dir}")
        shutil.copytree(src, runtime_dst / subdir)
    for notice in ("LICENSE.txt", "ThirdPartyNotices.txt"):
        src_file = dotnet_dir / notice
        if src_file.is_file():
            shutil.copy2(src_file, runtime_dst / notice)


def build_roslyn() -> None:
    dotnet = fetch_dotnet()
    roslyn_src = ROOT_DIR / "External" / "RoslynTranspiler"
    csproj = roslyn_src / "ShalltearRoslyn.csproj"
    if not csproj.exists():
        return
    hasher = hashlib.sha256()
    hasher.update(DOTNET_VERSION.encode("utf-8"))
    for path in sorted(roslyn_src.glob("*")):
        if path.is_file():
            hasher.update(path.read_bytes())
    roslyn_version = hasher.hexdigest()[:16]
    target = PREBUILT_DIR / "roslyn"
    dll_path = target / "bin" / "ShalltearRoslyn.dll"
    runtime_slice = target / "bin" / "dotnet" / "host" / "fxr"
    if is_current(target, roslyn_version) and dll_path.exists() and runtime_slice.is_dir():
        print(f"roslyn {roslyn_version} already built. Skipping")
        return
    print(f"building ShalltearRoslyn ({roslyn_version})")
    dotnet_dir = dotnet.parent.resolve()
    env = os.environ.copy()
    env["DOTNET_ROOT"] = str(dotnet_dir)
    env["DOTNET_CLI_HOME"] = str(dotnet_dir / ".home")
    env["DOTNET_SKIP_FIRST_TIME_EXPERIENCE"] = "1"
    env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    env["DOTNET_NOLOGO"] = "1"
    subprocess.run(
        [str(dotnet), "build", str(csproj), "-c", "Release", "--nologo"],
        env=env,
        check=True,
    )
    stage_dotnet_runtime_slice(dotnet_dir, target / "bin")
    write_marker(target, roslyn_version)
    print(f"roslyn -> {target / 'bin'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lavapipe", action="store_true", help="also fetch the CI software Vulkan driver + loader")
    parser.add_argument("--skip-dxc", action="store_true", help="skip the DXC fetch")
    parser.add_argument("--toolchain", action="store_true", help="also fetch llvm-mingw/cmake/ninja (what the SDK bundles)")
    args = parser.parse_args()

    PREBUILT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.skip_dxc:
        fetch_dxc()
        build_roslyn()
    if args.lavapipe:
        fetch_lavapipe()
        fetch_vulkan_runtime()
    if args.toolchain:
        fetch_toolchain()
        fetch_cmake()
        fetch_ninja()
    return 0


if __name__ == "__main__":
    sys.exit(main())
