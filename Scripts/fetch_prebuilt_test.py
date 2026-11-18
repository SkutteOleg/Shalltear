import contextlib
import hashlib
import io
import json
import shutil
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

import fetch_prebuilt

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


if __name__ == "__main__":
	unittest.main()
