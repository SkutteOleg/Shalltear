import contextlib
import copy
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import toolchain
import update_dotnet_pin

SDK_URL = "https://builds.dotnet.microsoft.com/dotnet/Sdk/"

LINUX_404 = "577d0eb17fec33d029104c5c63a1ab0accc3e28776274abef2218dbac5af18d2109b5406d26b31d76f5c245215e3ba0926a94c20246c672c41847c9e1eb1dc50"
WINDOWS_404 = "330ddd2665b43cd2e428216a0d8ea1ac7bb19b5434fdc530e62010dc3b52823a620bf95489d628e1ed6f5122401793ee4887cca4a019122361a1231497f632ee"
LINUX_115 = "01bf048cf0495a7ce76be07b9623d385c9d9f172237d0267ded25c31f4b19604d74f1d328d0ee5fe6bcfb23cb20c41fb072c9b84fce6582c20edb1e3d93ea012"
WINDOWS_115 = "f9547ed219c7bb08603adc1eea8bd92f21a02b684dd4088131f89cede677326bcfa84960a027d01e0c51091eaf27d4f41aa38a51ff15abc271c1025bc5df198e"

RELEASES = {
	"channel-version": "10.0",
	"latest-sdk": "10.0.404",
	"support-phase": "active",
	"releases": [
		{"release-version": "10.0.15", "sdks": [
			{"version": "10.0.404", "runtime-version": "10.0.15", "files": [
				{"name": "dotnet-sdk-linux-arm64.tar.gz", "rid": "linux-arm64",
				 "url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-linux-arm64.tar.gz",
				 "hash": "3533a3869da219cac45c567aadac3d8e96cdf1c6a04fe1c1d5fdeefea5d45af8424488f0144730d11b4a751f7ad546fb11e4c2b2725cd8401efee726711f8f11"},
				{"name": "dotnet-sdk-linux-x64.tar.gz", "rid": "linux-x64",
				 "url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-linux-x64.tar.gz", "hash": LINUX_404},
				{"name": "dotnet-sdk-win-x64.exe", "rid": "win-x64",
				 "url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-win-x64.exe",
				 "hash": "c86b0aeb8c5ce0cd6dd290fb2b10bb112302d849d05cbcbcbc0ed4e5a15f66d1376e05bc77910bf66580d6a779390ec863e2bd6e51e7e21880d587b8b30c7b08"},
				{"name": "dotnet-sdk-win-x64.zip", "rid": "win-x64",
				 "url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-win-x64.zip", "hash": WINDOWS_404},
			]},
			{"version": "10.0.115", "runtime-version": "10.0.15", "files": [
				{"name": "dotnet-sdk-linux-x64.tar.gz", "rid": "linux-x64",
				 "url": SDK_URL + "10.0.115/dotnet-sdk-10.0.115-linux-x64.tar.gz", "hash": LINUX_115},
				{"name": "dotnet-sdk-win-x64.zip", "rid": "win-x64",
				 "url": SDK_URL + "10.0.115/dotnet-sdk-10.0.115-win-x64.zip", "hash": WINDOWS_115},
			]},
		]},
		{"release-version": "10.0.12", "sdks": [
			{"version": "10.0.401", "runtime-version": "10.0.12", "files": [
				{"name": "dotnet-sdk-linux-x64.tar.gz", "rid": "linux-x64",
				 "url": SDK_URL + "10.0.401/dotnet-sdk-10.0.401-linux-x64.tar.gz",
				 "hash": "51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b"},
				{"name": "dotnet-sdk-win-x64.zip", "rid": "win-x64",
				 "url": SDK_URL + "10.0.401/dotnet-sdk-10.0.401-win-x64.zip",
				 "hash": "24b670ad3d923bfcf47df6c3b034152398b42f6dbc388e10d783aee1cfb5e5817d399fc0ae2a12cfa822a55e61d34830ccb15c50ef6efee437ab874bb7c79430"},
			]},
		]},
	],
}

SDK_WITH_WINDOWS_TAR = {"version": "11.0.100", "runtime-version": "11.0.0", "files": [
	{"name": "dotnet-sdk-linux-x64.tar.gz", "rid": "linux-x64",
	 "url": SDK_URL + "11.0.100/dotnet-sdk-11.0.100-linux-x64.tar.gz",
	 "hash": "1bec9cca2ec689b1e80245b84f2b53c9441d1aa6daccf2ceb27490768bffa95954fa585bd275def31069d874d543d9ba9e6591fe9ade2dbd9f34ec5c0fd0050c"},
	{"name": "dotnet-sdk-win-x64.tar.gz", "rid": "win-x64",
	 "url": SDK_URL + "11.0.100/dotnet-sdk-11.0.100-win-x64.tar.gz",
	 "hash": "9beef28998c50eaa011f4241ff71c8c450c76a8556eda084d1f9857de3d6b5d9b136144a3e1a98b9f9b3e1e2fe50b06fff4c2f3b41208780a800408859084591"},
	{"name": "dotnet-sdk-win-x64.zip", "rid": "win-x64",
	 "url": SDK_URL + "11.0.100/dotnet-sdk-11.0.100-win-x64.zip",
	 "hash": "5cdd9122c84873f090038548ebe8a4786e5c50a6b1b9792ca2c6dab82fd74a0d7f1d6cdf59e7dcd25e4903dd44f37af5d7ed73ecd3bca4d0cce948b5572452c3"},
]}


class PinTest(unittest.TestCase):

	def test_pin_takes_latest_sdk(self):
		pin = update_dotnet_pin.build_pin(RELEASES)
		self.assertEqual(pin, {
			"channel": "10.0",
			"sdkVersion": "10.0.404",
			"runtimeVersion": "10.0.15",
			"targetFramework": "net10.0",
			"sdk": {
				"linux-x64": {"url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-linux-x64.tar.gz", "sha512": LINUX_404},
				"win-x64": {"url": SDK_URL + "10.0.404/dotnet-sdk-10.0.404-win-x64.zip", "sha512": WINDOWS_404},
			},
			"packs": {},
		})

	def test_named_sdk_is_used(self):
		pin = update_dotnet_pin.build_pin(RELEASES, "10.0.115")
		self.assertEqual(pin["sdkVersion"], "10.0.115")
		self.assertEqual(pin["sdk"]["linux-x64"]["sha512"], LINUX_115)
		self.assertEqual(pin["sdk"]["win-x64"]["sha512"], WINDOWS_115)

	def test_unknown_sdk_is_refused(self):
		with self.assertRaisesRegex(RuntimeError, "10.0.999 was not found"):
			update_dotnet_pin.build_pin(RELEASES, "10.0.999")

	def test_windows_archive_is_the_zip(self):
		archive = update_dotnet_pin.archive_for(SDK_WITH_WINDOWS_TAR, "win-x64")
		self.assertEqual(archive["name"], "dotnet-sdk-win-x64.zip")
		self.assertEqual(update_dotnet_pin.archive_for(SDK_WITH_WINDOWS_TAR, "linux-x64")["name"], "dotnet-sdk-linux-x64.tar.gz")

	def test_missing_host_archive_is_refused(self):
		with self.assertRaisesRegex(RuntimeError, "no .tar.gz archive for linux-arm"):
			update_dotnet_pin.archive_for(SDK_WITH_WINDOWS_TAR, "linux-arm")

	def test_unsupported_channel_is_refused(self):
		for phase in ("eol", "preview", "go-live", None):
			with self.assertRaisesRegex(RuntimeError, "support phase"):
				update_dotnet_pin.build_pin(dict(RELEASES, **{"support-phase": phase}))
		update_dotnet_pin.build_pin(dict(RELEASES, **{"support-phase": "maintenance"}))

	def test_runtime_before_reload_fix_is_refused(self):
		with self.assertRaisesRegex(RuntimeError, "runtime 10.0.12"):
			update_dotnet_pin.build_pin(RELEASES, "10.0.401")

	def test_uppercase_hash_is_lowered(self):
		releases = copy.deepcopy(RELEASES)
		for entry in releases["releases"][0]["sdks"][0]["files"]:
			entry["hash"] = entry["hash"].upper()
		self.assertEqual(update_dotnet_pin.build_pin(releases)["sdk"]["win-x64"]["sha512"], WINDOWS_404)

	def test_packs_stay_while_runtime_stays(self):
		packs = {"microsoft.dotnet.ilcompiler/10.0.15": "ab"}
		same = update_dotnet_pin.build_pin(RELEASES, "10.0.115", {"runtimeVersion": "10.0.15", "packs": packs})
		self.assertEqual(same["packs"], packs)
		moved = update_dotnet_pin.build_pin(RELEASES, None, {"runtimeVersion": "10.0.14", "packs": packs})
		self.assertEqual(moved["packs"], {})


class PinFileTest(unittest.TestCase):

	def setUp(self):
		self.root = Path(tempfile.mkdtemp())
		self.addCleanup(shutil.rmtree, self.root, True)
		self.saved = (update_dotnet_pin.PIN_PATH, sys.argv)
		update_dotnet_pin.PIN_PATH = self.root / "Source" / "CSharp" / "DotNetPin.json"
		self.releases = self.root / "releases.json"
		self.releases.write_text(json.dumps(RELEASES), encoding="utf-8")

	def tearDown(self):
		update_dotnet_pin.PIN_PATH, sys.argv = self.saved

	def run_main(self, *arguments):
		sys.argv = ["update_dotnet_pin.py", *arguments]
		out = io.StringIO()
		with contextlib.redirect_stdout(out):
			code = update_dotnet_pin.main()
		return code, out.getvalue()

	def test_pin_file_uses_tabs_and_lf(self):
		code, _ = self.run_main("10.0", "--from-json", str(self.releases))
		self.assertEqual(code, 0)
		data = update_dotnet_pin.PIN_PATH.read_bytes()
		self.assertTrue(data.startswith(b'{\n\t"channel": "10.0",\n\t"sdkVersion": "10.0.404",\n'))
		self.assertTrue(data.endswith(b"}\n"))
		self.assertNotIn(b"\r", data)
		self.assertNotIn(b"  ", data)
		self.assertEqual(json.loads(data), update_dotnet_pin.build_pin(RELEASES))

	def test_refused_channel_writes_no_pin(self):
		self.releases.write_text(json.dumps(dict(RELEASES, **{"support-phase": "eol"})), encoding="utf-8")
		code, out = self.run_main("10.0", "--from-json", str(self.releases))
		self.assertEqual(code, 1)
		self.assertIn("support phase is 'eol'", out)
		self.assertFalse(update_dotnet_pin.PIN_PATH.exists())

	def test_packs_need_pin_on_that_channel(self):
		code, out = self.run_main("10.0", "--packs")
		self.assertEqual(code, 1)
		self.assertIn("is not on channel 10.0", out)
		self.run_main("10.0", "--from-json", str(self.releases))
		before = update_dotnet_pin.PIN_PATH.read_bytes()
		code, out = self.run_main("11.0", "--packs")
		self.assertEqual(code, 1)
		self.assertIn("is not on channel 11.0", out)
		self.assertEqual(update_dotnet_pin.PIN_PATH.read_bytes(), before)

	def test_packs_hash_each_package_archive(self):
		folder = self.root / "packages"
		compiler = folder / "runtime.win-x64.microsoft.dotnet.ilcompiler" / "10.0.15"
		targets = folder / "microsoft.dotnet.ilcompiler" / "10.0.15"
		(compiler / "tools").mkdir(parents=True)
		targets.mkdir(parents=True)
		(compiler / "runtime.win-x64.microsoft.dotnet.ilcompiler.10.0.15.nupkg").write_bytes(b"compiler")
		(compiler / "runtime.win-x64.microsoft.dotnet.ilcompiler.10.0.15.nupkg.sha512").write_bytes(b"unread")
		(compiler / "tools" / "nested.nupkg").write_bytes(b"unread")
		(targets / "microsoft.dotnet.ilcompiler.10.0.15.nupkg").write_bytes(b"targets")
		self.assertEqual(update_dotnet_pin.hash_packages(folder), {
			"microsoft.dotnet.ilcompiler/10.0.15": hashlib.sha512(b"targets").hexdigest(),
			"runtime.win-x64.microsoft.dotnet.ilcompiler/10.0.15": hashlib.sha512(b"compiler").hexdigest(),
		})


class RestoreTest(unittest.TestCase):

	def setUp(self):
		self.root = Path(tempfile.mkdtemp())
		self.addCleanup(shutil.rmtree, self.root, True)
		self.pin = update_dotnet_pin.build_pin(RELEASES)
		self.dotnet = self.root / "dotnet" / "10.0.404" / ("dotnet" + toolchain.EXE)
		self.commands = []
		self.projects = []
		self.saved = (toolchain.PREBUILT_DIR, subprocess.run)
		toolchain.PREBUILT_DIR = self.root
		subprocess.run = self.record

	def tearDown(self):
		toolchain.PREBUILT_DIR, subprocess.run = self.saved

	def record(self, command, check, env):
		self.commands.append((command, env))
		if "--packages" in command:
			self.projects.append(Path(command[2]).read_text(encoding="utf-8"))
			package = Path(command[command.index("--packages") + 1]) / "some.package" / "1.2.3"
			package.mkdir(parents=True, exist_ok=True)
			(package / "some.package.1.2.3.nupkg").write_bytes(b"archive")

	def restore(self):
		with contextlib.redirect_stdout(io.StringIO()):
			return update_dotnet_pin.restore_packs(self.pin)

	def test_restore_needs_fetched_sdk(self):
		with self.assertRaisesRegex(RuntimeError, "fetch_prebuilt.py --dotnet"):
			self.restore()
		self.assertEqual(self.commands, [])

	def test_restore_names_one_target_per_run(self):
		self.dotnet.parent.mkdir(parents=True)
		self.dotnet.write_bytes(b"")
		self.assertEqual(self.restore(), {"some.package/1.2.3": hashlib.sha512(b"archive").hexdigest()})

		players = [command for command, _ in self.commands if "--packages" in command]
		self.assertEqual([[part for part in command if part.startswith("-p:")] for command in players],
						 [["-p:RuntimeIdentifier=linux-x64"], ["-p:RuntimeIdentifier=win-x64"]])
		self.assertTrue(all("-r" not in command for command in players))
		other = "linux-x64" if toolchain.HOST_IS_WINDOWS else "win-x64"
		for project in self.projects:
			self.assertEqual(project.count("<PackageDownload "), 1 + len(update_dotnet_pin.WASM_PACKS))
			self.assertIn(f'"runtime.{other}.Microsoft.DotNet.ILCompiler" Version="[10.0.15]"', project)
			for wasm_pack in update_dotnet_pin.WASM_PACKS:
				self.assertIn(f'"{wasm_pack}" Version="[10.0.15]"', project)
		for _, env in self.commands:
			self.assertEqual(env["DOTNET_ROOT"], str(self.dotnet.parent))
			self.assertEqual(env["DOTNET_CLI_TELEMETRY_OPTOUT"], "1")

	def test_restore_rewrites_managed_lock_files(self):
		self.dotnet.parent.mkdir(parents=True)
		self.dotnet.write_bytes(b"")
		self.restore()
		command, _ = self.commands[-1]
		self.assertEqual(command[:3], [str(self.dotnet), "restore", str(update_dotnet_pin.MANAGED_SOLUTION)])
		self.assertIn("--force-evaluate", command)
		self.assertNotIn("--packages", command)


if __name__ == "__main__":
	unittest.main()
