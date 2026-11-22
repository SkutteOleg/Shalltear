#!/usr/bin/env python3
"""Writes Source/CSharp/DotNetPin.json from Microsoft's .NET release metadata
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

import toolchain

ROOT_DIR = Path(__file__).resolve().parent.parent
PIN_PATH = ROOT_DIR / "Source" / "CSharp" / "DotNetPin.json"
MANAGED_SOLUTION = ROOT_DIR / "Source" / "CSharp" / "Managed" / "Shalltear.Managed.slnx"

METADATA_URL = "https://builds.dotnet.microsoft.com/dotnet/release-metadata/{channel}/releases.json"
PACKAGE_SOURCE = "https://api.nuget.org/v3/index.json"
HOST_RIDS = ("linux-x64", "win-x64")
HOST_RID = "win-x64" if toolchain.HOST_IS_WINDOWS else "linux-x64"
TARGET_RIDS = ("linux-x64", "win-x64")
SUPPORTED_PHASES = ("active", "maintenance")
RELOAD_FIX_RUNTIME = {"10.0": "10.0.13"}

NUGET_CONFIG = f"""<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="{PACKAGE_SOURCE}" />
  </packageSources>
  <fallbackPackageFolders>
    <clear />
  </fallbackPackageFolders>
</configuration>
"""

PLAYER_PROJECT = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>{framework}</TargetFramework>
    <OutputType>Library</OutputType>
    <PublishAot>true</PublishAot>
    <NativeLib>Shared</NativeLib>
    <DisableTransitiveFrameworkReferenceDownloads>true</DisableTransitiveFrameworkReferenceDownloads>
    <DisableImplicitNuGetFallbackFolder>true</DisableImplicitNuGetFallbackFolder>
  </PropertyGroup>
  <ItemGroup>
{downloads}  </ItemGroup>
</Project>
"""


def load_releases(channel, from_json=None):
	if from_json:
		return json.loads(Path(from_json).read_text(encoding="utf-8"))
	url = METADATA_URL.format(channel=channel)
	print(f"  downloading {url}")
	with urllib.request.urlopen(url) as response:
		return json.loads(response.read().decode("utf-8"))


def version_tuple(version):
	return tuple(int(part) for part in version.split("-")[0].split("."))


def select_sdk(releases, sdk_version=None):
	channel = releases["channel-version"]
	phase = releases.get("support-phase")
	if phase not in SUPPORTED_PHASES:
		raise RuntimeError(f".NET {channel} support phase is '{phase}'. Pick a channel in active/maintenance")
	wanted = sdk_version or releases["latest-sdk"]
	for release in releases["releases"]:
		for sdk in release.get("sdks") or []:
			if sdk["version"] != wanted:
				continue
			runtime = sdk["runtime-version"]
			fixed = RELOAD_FIX_RUNTIME.get(channel)
			if fixed and version_tuple(runtime) < version_tuple(fixed):
				raise RuntimeError(f"SDK {wanted} has runtime {runtime} (script reload hangs below {fixed}). Pick a newer SDK")
			return sdk
	raise RuntimeError(f"SDK {wanted} was not found in channel {channel}")


def archive_for(sdk, rid):
	suffix = ".zip" if rid.startswith("win-") else ".tar.gz"
	for entry in sdk["files"]:
		if entry["rid"] == rid and entry["name"].endswith(suffix):
			return entry
	raise RuntimeError(f"SDK {sdk['version']} has no {suffix} archive for {rid}")


def build_pin(releases, sdk_version=None, previous=None):
	sdk = select_sdk(releases, sdk_version)
	channel = releases["channel-version"]
	pin = {
		"channel": channel,
		"sdkVersion": sdk["version"],
		"runtimeVersion": sdk["runtime-version"],
		"targetFramework": "net" + channel,
		"sdk": {},
		"packs": {},
	}
	for rid in HOST_RIDS:
		archive = archive_for(sdk, rid)
		pin["sdk"][rid] = {"url": archive["url"], "sha512": archive["hash"].lower()}
	if previous and previous.get("runtimeVersion") == pin["runtimeVersion"]:
		pin["packs"] = previous.get("packs", {})
	return pin


def hash_packages(folder):
	packs = {}
	for archive in sorted(Path(folder).glob("*/*/*.nupkg")):
		digest = hashlib.sha512()
		with open(archive, "rb") as source:
			for block in iter(lambda: source.read(1 << 20), b""):
				digest.update(block)
		packs[f"{archive.parent.parent.name}/{archive.parent.name}"] = digest.hexdigest()
	return packs


def restore_player_packs(dotnet, pin, work, packages):
	downloads = "".join(
		f'    <PackageDownload Include="runtime.{rid}.Microsoft.DotNet.ILCompiler" Version="[{pin["runtimeVersion"]}]" />\n'
		for rid in HOST_RIDS if rid != HOST_RID)
	project = work / "Player.csproj"
	project.write_text(PLAYER_PROJECT.format(framework=pin["targetFramework"], downloads=downloads), encoding="utf-8")
	(work / "nuget.config").write_text(NUGET_CONFIG, encoding="utf-8")
	env = dict(os.environ, DOTNET_ROOT=str(dotnet.parent), DOTNET_CLI_HOME=str(work / "home"),
			   DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1", MSBUILDDISABLENODEREUSE="1")
	for rid in TARGET_RIDS:
		print(f"  restoring player packages for {rid}")
		subprocess.run([str(dotnet), "restore", str(project), f"-p:RuntimeIdentifier={rid}", "--packages", str(packages),
						"-v", "quiet"], check=True, env=env)
	return env


def restore_packs(pin):
	dotnet = toolchain.PREBUILT_DIR / "dotnet" / pin["sdkVersion"] / ("dotnet" + toolchain.EXE)
	if not dotnet.exists():
		raise RuntimeError(f"{dotnet} is absent. Run python Scripts/fetch_prebuilt.py --dotnet")
	with tempfile.TemporaryDirectory() as tmp:
		work = Path(tmp)
		packages = work / "packages"
		env = restore_player_packs(dotnet, pin, work, packages)
		print(f"  locking {MANAGED_SOLUTION.name} packages")
		subprocess.run([str(dotnet), "restore", str(MANAGED_SOLUTION), "--force-evaluate", "-v", "quiet",
						f"-p:ShalltearManagedRoot={(work / 'managed').as_posix()}/"], check=True, env=env)
		return hash_packages(packages)


def write_pin(path, pin):
	Path(path).parent.mkdir(parents=True, exist_ok=True)
	with open(path, "w", encoding="utf-8", newline="\n") as out:
		out.write(json.dumps(pin, indent="\t") + "\n")


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("channel", help="release channel (10.0)")
	parser.add_argument("sdk_version", nargs="?", help="SDK ver in that channel (latest if omitted)")
	parser.add_argument("--from-json", type=Path, help="read releases.json from this file")
	parser.add_argument("--packs", action="store_true",
						help="hash packages fetched by a player restore + rewrite the engine's C# lock files. "
							 "Needs fetch_prebuilt.py --dotnet")
	args = parser.parse_args()

	previous = json.loads(PIN_PATH.read_text(encoding="utf-8")) if PIN_PATH.exists() else None
	try:
		if args.packs:
			if not previous or previous["channel"] != args.channel:
				raise RuntimeError(f"{PIN_PATH.name} is not on channel {args.channel}. Run python Scripts/update_dotnet_pin.py {args.channel}")
			pin = dict(previous, packs=restore_packs(previous))
		else:
			pin = build_pin(load_releases(args.channel, args.from_json), args.sdk_version, previous)
	except (RuntimeError, subprocess.CalledProcessError) as error:
		print(f"Error: {error}")
		return 1

	write_pin(PIN_PATH, pin)
	print(f"{PIN_PATH} -> SDK {pin['sdkVersion']}, runtime {pin['runtimeVersion']}, {len(pin['packs'])} package hash(es)")
	if not pin["packs"]:
		print(f"  Next: python Scripts/fetch_prebuilt.py --dotnet -> python Scripts/update_dotnet_pin.py {args.channel} --packs")
	return 0


if __name__ == "__main__":
	sys.exit(main())
