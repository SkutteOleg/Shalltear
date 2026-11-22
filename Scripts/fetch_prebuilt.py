#!/usr/bin/env python3
"""Fetches pinned, SHA-verified prebuilt binaries consumed at runtime
"""

import argparse
import filecmp
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

import linux_pack

ROOT_DIR = Path(__file__).resolve().parent.parent
PREBUILT_DIR = ROOT_DIR / "External" / "Prebuilt"

HOST_IS_WINDOWS = os.name == "nt"

DXC_VERSION = "v1.9.2602.24"
DXC_URL = f"https://github.com/microsoft/DirectXShaderCompiler/releases/download/{DXC_VERSION}/dxc_2026_05_27.zip"
DXC_SHA256 = "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4"
DXC_LINUX_URL = f"https://github.com/microsoft/DirectXShaderCompiler/releases/download/{DXC_VERSION}/linux_dxc_2026_05_26.x86_64.tar.gz"
DXC_LINUX_SHA256 = "928b3e9986d11dc4279050e02340950c29bcbd1e5efb9d3ded9669dade37639d"

LAVAPIPE_VERSION = "26.1.3"
LAVAPIPE_URL = f"https://github.com/pal1000/mesa-dist-win/releases/download/{LAVAPIPE_VERSION}/mesa3d-{LAVAPIPE_VERSION}-release-msvc.7z"
LAVAPIPE_SHA256 = "6dd431f4620cea73970b13e3ffa94f721f2a3924306b8a4283c97648cdb6eb9c"

LLVM_MINGW_VERSION = "20260908"
LLVM_MINGW_ARCHIVE = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-x86_64"
LLVM_MINGW_URL = (
    f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VERSION}/{LLVM_MINGW_ARCHIVE}.zip"
)
LLVM_MINGW_SHA256 = "1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6"
LLVM_MINGW_LINUX_ARCHIVE = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW_LINUX_URL = (
    f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VERSION}/{LLVM_MINGW_LINUX_ARCHIVE}.tar.xz"
)
LLVM_MINGW_LINUX_SHA256 = "2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092"

CMAKE_VERSION = "4.2.0"
CMAKE_ARCHIVE = f"cmake-{CMAKE_VERSION}-windows-x86_64"
CMAKE_URL = f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/{CMAKE_ARCHIVE}.zip"
CMAKE_SHA256 = "cf35a516c4f5f4646b301e51c8e24b168cc012c3b1453b8f675303b54eb0ef45"
CMAKE_LINUX_ARCHIVE = f"cmake-{CMAKE_VERSION}-linux-x86_64"
CMAKE_LINUX_URL = f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/{CMAKE_LINUX_ARCHIVE}.tar.gz"
CMAKE_LINUX_SHA256 = "bbcebd4c433eab3af03a8c80bb5d84e8dfc3ff8a4ab9d01547b21240c23f7c2c"

NINJA_VERSION = "1.13.2"
NINJA_URL = f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip"
NINJA_SHA256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"
NINJA_LINUX_URL = f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-linux.zip"
NINJA_LINUX_SHA256 = "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6"
NINJA_COPYING_URL = f"https://raw.githubusercontent.com/ninja-build/ninja/v{NINJA_VERSION}/COPYING"
NINJA_COPYING_SHA256 = "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea"

MINGW_LICENSE_DIR = ROOT_DIR / "Scripts" / "ThirdParty" / "mingw-w64"
MINGW_LGPL21_SHA256 = "20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95"
MINGW_GPL2_SHA256 = "edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6"

VULKANRT_VERSION = "1.4.321.0"
VULKANRT_URL = f"https://sdk.lunarg.com/sdk/download/{VULKANRT_VERSION}/windows/vulkan-runtime-components.zip"
VULKANRT_SHA256 = "f8ba79303face4b72a2f302f1a46e8c918995b54fbf24c0c9fc6c3d3ef525dca"

DOTNET_PIN_PATH = ROOT_DIR / "Source" / "CSharp" / "DotNetPin.json"


ARCHIVES_DIR = None


def file_digest(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download_verified(url: str, expected: str, dest: Path, algorithm: str = "sha256") -> None:
    kept = ARCHIVES_DIR / dest.name if ARCHIVES_DIR else None
    if kept and kept.is_file() and file_digest(kept, algorithm) == expected:
        print(f"  using {kept}")
        if kept != dest:
            shutil.copyfile(kept, dest)
        return
    print(f"  downloading {url}")
    # LunarG CDN refuses the urllib default
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Shalltear fetch_prebuilt)"})
    with urllib.request.urlopen(request) as response, open(dest, "wb") as out:
        shutil.copyfileobj(response, out)
    digest = file_digest(dest, algorithm)
    if digest != expected:
        dest.unlink(missing_ok=True)
        raise RuntimeError(
            f"SHA-{algorithm[3:]} mismatch for {url}\n  expected {expected}\n  got      {digest}"
        )
    print(f"  {algorithm} verified ({digest[:12]}...)")
    if kept and kept != dest:
        shutil.copyfile(dest, kept)


def extract_tar(archive: Path, target: Path, prefix: str, wanted=None) -> None:
    with tarfile.open(archive) as tar:
        for member in tar:
            name = member.name[2:] if member.name.startswith("./") else member.name
            if not name.startswith(prefix):
                continue
            relative = name[len(prefix):]
            if not relative or (wanted is not None and not wanted(relative)):
                continue
            out = target / relative
            if member.isdir():
                out.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                out.parent.mkdir(parents=True, exist_ok=True)
                if out.is_symlink() or out.exists():
                    out.unlink()
                os.symlink(member.linkname, out)
            elif member.isfile():
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(tar.extractfile(member).read())
                os.chmod(out, member.mode & 0o777)


def is_current(target_dir: Path, version: str) -> bool:
    marker = target_dir / ".fetched"
    return marker.exists() and marker.read_text(encoding="utf-8").strip() == version


def write_marker(target_dir: Path, version: str) -> None:
    (target_dir / ".fetched").write_text(version + "\n", encoding="utf-8")


def fetch_dxc_linux() -> None:
    target = PREBUILT_DIR / "dxc"
    if is_current(target, DXC_VERSION):
        print(f"dxc {DXC_VERSION} already fetched. Skipping")
        return
    print(f"fetching dxc {DXC_VERSION}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "linux_dxc.tar.gz"
        download_verified(DXC_LINUX_URL, DXC_LINUX_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        extract_tar(archive, target, "", lambda name: name == "lib/libdxcompiler.so" or name.startswith("LICENSE"))
    if not (target / "lib" / "libdxcompiler.so").exists():
        raise RuntimeError("lib/libdxcompiler.so not found in the DXC archive. Layout changed?")
    write_marker(target, DXC_VERSION)
    print(f"dxc -> {target}")


def fetch_dxc() -> None:
    if not HOST_IS_WINDOWS:
        fetch_dxc_linux()
        return
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


def fetch_toolchain_linux() -> None:
    target = PREBUILT_DIR / "llvm-mingw"
    if is_current(target, LLVM_MINGW_VERSION):
        print(f"llvm-mingw {LLVM_MINGW_VERSION} already fetched. Skipping")
        return
    print(f"fetching llvm-mingw {LLVM_MINGW_VERSION} (84 MB, unpacks to ~620 MB)")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / (LLVM_MINGW_LINUX_ARCHIVE + ".tar.xz")
        download_verified(LLVM_MINGW_LINUX_URL, LLVM_MINGW_LINUX_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        extract_tar(archive, target, LLVM_MINGW_LINUX_ARCHIVE + "/")
    if not (target / "bin" / "clang++").exists():
        raise RuntimeError(f"{target / 'bin' / 'clang++'} is absent after extraction. Layout changed?")
    write_marker(target, LLVM_MINGW_VERSION)
    print(f"llvm-mingw -> {target}")


def fetch_cmake_linux() -> None:
    target = PREBUILT_DIR / "cmake"
    if is_current(target, CMAKE_VERSION):
        print(f"cmake {CMAKE_VERSION} already fetched. Skipping")
        return
    print(f"fetching cmake {CMAKE_VERSION} (58 MB)")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / (CMAKE_LINUX_ARCHIVE + ".tar.gz")
        download_verified(CMAKE_LINUX_URL, CMAKE_LINUX_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        extract_tar(archive, target, CMAKE_LINUX_ARCHIVE + "/",
                    lambda name: name in ("bin/cmake", "bin/ctest") or _cmake_wanted(name))
    if not (target / "bin" / "cmake").exists():
        raise RuntimeError("bin/cmake not found in the CMake archive. Layout changed?")
    write_marker(target, CMAKE_VERSION)
    print(f"cmake -> {target}")


def fetch_ninja_linux() -> None:
    target = PREBUILT_DIR / "ninja"
    if is_current(target, NINJA_VERSION):
        print(f"ninja {NINJA_VERSION} already fetched. Skipping")
        return
    print(f"fetching ninja {NINJA_VERSION}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "ninja-linux.zip"
        download_verified(NINJA_LINUX_URL, NINJA_LINUX_SHA256, archive)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        with zipfile.ZipFile(archive) as zf:
            (target / "ninja").write_bytes(zf.read("ninja"))
        os.chmod(target / "ninja", 0o755)
        copying = Path(tmp) / "COPYING"
        download_verified(NINJA_COPYING_URL, NINJA_COPYING_SHA256, copying)
        shutil.copyfile(copying, target / "COPYING")
    write_marker(target, NINJA_VERSION)
    print(f"ninja -> {target}")


def fetch_linux_pack() -> None:
    clang = PREBUILT_DIR / "llvm-mingw" / "bin" / ("clang.exe" if HOST_IS_WINDOWS else "clang")
    if not clang.exists():
        raise RuntimeError(f"{clang} is absent. Run python Scripts/fetch_prebuilt.py --toolchain")
    with tempfile.TemporaryDirectory() as tmp:
        def fetch_archive(url, sha256, name):
            archive = Path(tmp) / name
            download_verified(url, sha256, archive)
            return archive
        linux_pack.fetch(PREBUILT_DIR, fetch_archive, clang)


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


def stage_lld_link(lld: Path) -> None:
    link = lld.with_name("lld-link.exe")
    if link.exists() and filecmp.cmp(lld, link, shallow=False):
        return
    print(f"  copying {lld.name} -> {link.name}")
    shutil.copyfile(lld, link)


def stop_build_servers(dotnet_dir: Path) -> None:
    for muxer in sorted(dotnet_dir.glob("*/dotnet.exe" if HOST_IS_WINDOWS else "*/dotnet")):
        try:
            subprocess.run([str(muxer), "build-server", "shutdown"], capture_output=True, text=True)
        except OSError:
            pass


def fetch_dotnet() -> None:
    pin = json.loads(DOTNET_PIN_PATH.read_text(encoding="utf-8"))
    version = pin["sdkVersion"]
    host = pin["sdk"]["win-x64" if HOST_IS_WINDOWS else "linux-x64"]
    lld = PREBUILT_DIR / "llvm-mingw" / "bin" / "ld.lld.exe"
    if HOST_IS_WINDOWS and not lld.exists():
        raise RuntimeError(f"{lld} is absent. Run python Scripts/fetch_prebuilt.py --toolchain")
    target = PREBUILT_DIR / "dotnet" / version
    if is_current(target, version):
        print(f".NET SDK {version} already fetched. Skipping")
    else:
        print(f"fetching .NET SDK {version} (~300 MB)")
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / host["url"].rsplit("/", 1)[-1]
            download_verified(host["url"], host["sha512"], archive, "sha512")
            if target.parent.exists():
                stop_build_servers(target.parent)
                shutil.rmtree(target.parent)
            target.mkdir(parents=True)
            if HOST_IS_WINDOWS:
                with zipfile.ZipFile(archive) as zf:
                    zf.extractall(target)
            else:
                extract_tar(archive, target, "")
        if not (target / ("dotnet.exe" if HOST_IS_WINDOWS else "dotnet")).exists():
            raise RuntimeError("dotnet not found in the SDK archive. Layout changed?")
        write_marker(target, version)
        print(f".NET SDK -> {target}")
    if HOST_IS_WINDOWS:
        stage_lld_link(lld)


def stale_packs(packages: Path, pinned: dict) -> list:
    stale = []
    for name, digest in sorted(pinned.items()):
        package_id, version = name.split("/")
        archive = packages / package_id / version / f"{package_id}.{version}.nupkg"
        if not archive.is_file() or file_digest(archive, "sha512") != digest:
            stale.append(name)
    return stale


def fetch_dotnet_packs() -> None:
    import update_dotnet_pin

    pin = json.loads(DOTNET_PIN_PATH.read_text(encoding="utf-8"))
    if not pin.get("packs"):
        raise RuntimeError(f"{DOTNET_PIN_PATH.name} has no packs. Run python Scripts/update_dotnet_pin.py <channel> --packs")
    packages = PREBUILT_DIR / "nuget"
    if not stale_packs(packages, pin["packs"]):
        print(f".NET player packages {pin['runtimeVersion']} already restored. Skipping")
        return
    dotnet = PREBUILT_DIR / "dotnet" / pin["sdkVersion"] / ("dotnet.exe" if HOST_IS_WINDOWS else "dotnet")
    print(f"restoring .NET player packages {pin['runtimeVersion']} (~165 MB)")
    with tempfile.TemporaryDirectory() as tmp:
        update_dotnet_pin.restore_player_packs(dotnet, pin, Path(tmp), packages)
    stale = stale_packs(packages, pin["packs"])
    if stale:
        raise RuntimeError(f"{', '.join(stale)} don't match the SHA-512 in {DOTNET_PIN_PATH.name}. "
                           f"Delete {packages} and run python Scripts/fetch_prebuilt.py --dotnet-packs")
    print(f".NET player packages -> {packages}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lavapipe", action="store_true", help="also fetch the CI software Vulkan driver + loader")
    parser.add_argument("--skip-dxc", action="store_true", help="skip the DXC fetch")
    parser.add_argument("--toolchain", action="store_true", help="also fetch llvm-mingw/cmake/ninja (what the SDK bundles)")
    parser.add_argument("--linux", action="store_true", help="also build the Linux target pack (sysroot + C++ runtime). Needs --toolchain once")
    parser.add_argument("--dotnet", action="store_true", help="also fetch the pinned .NET SDK (C# scripting)")
    parser.add_argument("--dotnet-packs", action="store_true",
                        help="also restore the pinned NativeAOT packages (C# players then build offline). Implies --dotnet")
    parser.add_argument("--archives", type=Path, help="keep downloaded archives in this directory and reuse them")
    args = parser.parse_args()

    global ARCHIVES_DIR
    if args.archives:
        ARCHIVES_DIR = args.archives.resolve()
        ARCHIVES_DIR.mkdir(parents=True, exist_ok=True)

    PREBUILT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.skip_dxc:
        fetch_dxc()
    if args.lavapipe:
        if HOST_IS_WINDOWS:
            fetch_lavapipe()
            fetch_vulkan_runtime()
        else:
            print("lavapipe comes with the distribution's Mesa Vulkan drivers. Skipping")
    if args.toolchain:
        if HOST_IS_WINDOWS:
            fetch_toolchain()
            fetch_cmake()
            fetch_ninja()
        else:
            fetch_toolchain_linux()
            fetch_cmake_linux()
            fetch_ninja_linux()
    if args.linux or (args.toolchain and not HOST_IS_WINDOWS):
        fetch_linux_pack()
    if args.dotnet or args.dotnet_packs:
        fetch_dotnet()
    if args.dotnet_packs:
        fetch_dotnet_packs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
