import contextlib
import hashlib
import io
import json
import shutil
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock
from pathlib import Path

import fetch_prebuilt
import update_dotnet_pin

VERSION = "9.9.100"
DOTNET = "dotnet.exe" if fetch_prebuilt.HOST_IS_WINDOWS else "dotnet"
RID = "win-x64" if fetch_prebuilt.HOST_IS_WINDOWS else "linux-x64"


class DotNetFetchTest(unittest.TestCase):

	def setUp(self):
		self.root = Path(tempfile.mkdtemp())
		self.addCleanup(shutil.rmtree, self.root, True)
		self.prebuilt = self.root / "Prebuilt"
		self.sdk = self.prebuilt / "dotnet" / VERSION
		self.lld = self.prebuilt / "llvm-mingw" / "bin" / "ld.lld.exe"
		self.lld.parent.mkdir(parents=True)
		self.lld.write_bytes(b"linker")
		self.archive = self.root / (f"dotnet-sdk-{VERSION}-{RID}" + (".zip" if fetch_prebuilt.HOST_IS_WINDOWS else ".tar.gz"))
		payload = self.root / "payload"
		(payload / "sdk" / VERSION).mkdir(parents=True)
		(payload / DOTNET).write_bytes(b"host")
		(payload / "sdk" / VERSION / "dotnet.dll").write_bytes(b"cli")
		if fetch_prebuilt.HOST_IS_WINDOWS:
			with zipfile.ZipFile(self.archive, "w") as zf:
				zf.write(payload / DOTNET, DOTNET)
				zf.write(payload / "sdk" / VERSION / "dotnet.dll", f"sdk/{VERSION}/dotnet.dll")
		else:
			with tarfile.open(self.archive, "w:gz") as tar:
				tar.add(payload, ".")
		self.saved = (fetch_prebuilt.PREBUILT_DIR, fetch_prebuilt.DOTNET_PIN_PATH, fetch_prebuilt.ARCHIVES_DIR)
		fetch_prebuilt.PREBUILT_DIR = self.prebuilt
		fetch_prebuilt.DOTNET_PIN_PATH = self.root / "DotNetPin.json"
		fetch_prebuilt.ARCHIVES_DIR = None

	def tearDown(self):
		fetch_prebuilt.PREBUILT_DIR, fetch_prebuilt.DOTNET_PIN_PATH, fetch_prebuilt.ARCHIVES_DIR = self.saved

	def write_pin(self, digest):
		pin = {"sdkVersion": VERSION, "sdk": {RID: {"url": self.archive.as_uri(), "sha512": digest}}}
		fetch_prebuilt.DOTNET_PIN_PATH.write_text(json.dumps(pin), encoding="utf-8")

	def fetch(self):
		with contextlib.redirect_stdout(io.StringIO()):
			fetch_prebuilt.fetch_dotnet()

	def test_wrong_hash_extracts_nothing(self):
		older = self.prebuilt / "dotnet" / "9.9.99"
		older.mkdir(parents=True)
		self.write_pin(hashlib.sha512(b"another archive").hexdigest())
		with self.assertRaisesRegex(RuntimeError, "SHA-512 mismatch"):
			self.fetch()
		self.assertFalse(self.sdk.exists())
		self.assertTrue(older.exists())

	def test_verified_archive_replaces_older_sdk(self):
		older = self.prebuilt / "dotnet" / "9.9.99"
		older.mkdir(parents=True)
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		self.fetch()
		self.assertEqual((self.sdk / DOTNET).read_bytes(), b"host")
		self.assertEqual((self.sdk / "sdk" / VERSION / "dotnet.dll").read_bytes(), b"cli")
		self.assertTrue(fetch_prebuilt.is_current(self.sdk, VERSION))
		self.assertFalse(older.exists())

	def test_build_servers_of_the_older_sdk_are_stopped_before_it_goes(self):
		older = self.prebuilt / "dotnet" / "9.9.99"
		older.mkdir(parents=True)
		(older / DOTNET).write_bytes(b"host")
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		seen = []
		with mock.patch.object(fetch_prebuilt.subprocess, "run", lambda command, **_: seen.append((command, older.exists()))):
			self.fetch()
		self.assertEqual(seen, [([str(older / DOTNET), "build-server", "shutdown"], True)])
		self.assertFalse(older.exists())
		self.assertTrue((self.sdk / DOTNET).exists())

	def test_an_older_sdk_that_cannot_run_is_still_replaced(self):
		older = self.prebuilt / "dotnet" / "9.9.99"
		older.mkdir(parents=True)
		(older / DOTNET).write_bytes(b"not a program")
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		self.fetch()
		self.assertFalse(older.exists())
		self.assertTrue((self.sdk / DOTNET).exists())

	def test_fetched_sdk_is_not_downloaded_again(self):
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		self.fetch()
		self.archive.unlink()
		self.fetch()
		self.assertTrue((self.sdk / DOTNET).exists())

	@unittest.skipUnless(fetch_prebuilt.HOST_IS_WINDOWS, "lld-link is the Windows linker name")
	def test_linker_copy_follows_the_toolchain(self):
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		self.fetch()
		link = self.lld.with_name("lld-link.exe")
		self.assertEqual(link.read_bytes(), b"linker")
		self.lld.write_bytes(b"newer linker")
		self.fetch()
		self.assertEqual(link.read_bytes(), b"newer linker")

	@unittest.skipUnless(fetch_prebuilt.HOST_IS_WINDOWS, "lld-link is the Windows linker name")
	def test_missing_toolchain_stops_before_download(self):
		self.lld.unlink()
		self.write_pin(hashlib.sha512(self.archive.read_bytes()).hexdigest())
		with self.assertRaisesRegex(RuntimeError, "--toolchain"):
			self.fetch()
		self.assertFalse(self.sdk.exists())


class DotNetPacksTest(unittest.TestCase):

	def setUp(self):
		self.root = Path(tempfile.mkdtemp())
		self.addCleanup(shutil.rmtree, self.root, True)
		self.packages = self.root / "Prebuilt" / "nuget"
		self.saved = (fetch_prebuilt.PREBUILT_DIR, fetch_prebuilt.DOTNET_PIN_PATH)
		fetch_prebuilt.PREBUILT_DIR = self.root / "Prebuilt"
		fetch_prebuilt.DOTNET_PIN_PATH = self.root / "DotNetPin.json"
		self.pinned = {"some.pack/1.0.0": hashlib.sha512(b"pack").hexdigest(), "other.pack/1.0.0": hashlib.sha512(b"other").hexdigest()}
		pin = {"sdkVersion": VERSION, "runtimeVersion": "1.0.0", "targetFramework": "net10.0", "packs": self.pinned}
		fetch_prebuilt.DOTNET_PIN_PATH.write_text(json.dumps(pin), encoding="utf-8")
		self.restores = []

	def tearDown(self):
		fetch_prebuilt.PREBUILT_DIR, fetch_prebuilt.DOTNET_PIN_PATH = self.saved

	def put(self, name, data):
		package_id, version = name.split("/")
		folder = self.packages / package_id / version
		folder.mkdir(parents=True, exist_ok=True)
		(folder / f"{package_id}.{version}.nupkg").write_bytes(data)

	def fetch(self, restored):
		def restore(dotnet, pin, work, packages):
			self.restores.append(packages)
			for name, data in restored.items():
				self.put(name, data)

		with mock.patch.object(update_dotnet_pin, "restore_player_packs", restore), contextlib.redirect_stdout(io.StringIO()):
			fetch_prebuilt.fetch_dotnet_packs()

	def test_a_missing_or_altered_archive_is_stale(self):
		self.put("some.pack/1.0.0", b"pack")
		self.put("other.pack/1.0.0", b"altered")
		self.assertEqual(fetch_prebuilt.stale_packs(self.packages, self.pinned), ["other.pack/1.0.0"])
		self.assertEqual(fetch_prebuilt.stale_packs(self.root / "nowhere", self.pinned), ["other.pack/1.0.0", "some.pack/1.0.0"])

	def test_restored_packs_are_not_restored_again(self):
		self.put("some.pack/1.0.0", b"pack")
		self.put("other.pack/1.0.0", b"other")
		self.fetch({})
		self.assertEqual(self.restores, [])

	def test_a_missing_pack_is_restored_into_the_checkout(self):
		self.put("some.pack/1.0.0", b"pack")
		self.fetch({"other.pack/1.0.0": b"other"})
		self.assertEqual(self.restores, [self.packages])

	def test_a_restore_with_another_hash_fails(self):
		with self.assertRaisesRegex(RuntimeError, "other.pack/1.0.0, some.pack/1.0.0 don't match"):
			self.fetch({"some.pack/1.0.0": b"tampered", "other.pack/1.0.0": b"tampered"})


if __name__ == "__main__":
	unittest.main()
