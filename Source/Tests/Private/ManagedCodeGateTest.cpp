#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr size_t SCAN_CHUNK = size_t{1} << 20;

	str MetadataRoot(std::string_view version) {
		str bytes("BSJB");
		bytes.append("\x01\x00\x01\x00", 4);
		bytes.append(4, '\0');
		const u32 length = static_cast<u32>((version.size() + 4) / 4 * 4);
		bytes.append(reinterpret_cast<const char*>(&length), sizeof(length));
		bytes.append(version);
		bytes.append(length - version.size(), '\0');
		return bytes;
	}

	str ExecutableImage(u16 magic, bool managed) {
		const size_t directories = magic == 0x20B ? 112 : 96;
		str bytes(64, '\0');
		bytes[0] = 'M';
		bytes[1] = 'Z';
		bytes[0x3C] = 64;
		bytes.append("PE\0\0", 4);
		bytes.append(20, '\0');
		str optional(directories + 16 * 8, '\0');
		std::memcpy(optional.data(), &magic, sizeof(magic));
		const u32 count = 16;
		std::memcpy(optional.data() + directories - 4, &count, sizeof(count));
		if (managed) {
			const u32 runtimeHeader[2] = {0x2008, 0x48};
			std::memcpy(optional.data() + directories + 14 * 8, runtimeHeader, sizeof(runtimeHeader));
		}
		bytes += optional;
		bytes.append(".text\0\0\0", 8);
		bytes.append(32, '\0');
		return bytes;
	}

	class ManagedCodeGateTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			_root = fs::temp_directory_path() / ("shalltear_no_il_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(_root);
		}

		void TearDown() override {
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		void Put(const char* relative, const str& bytes) {
			const fs::path file = _root / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << bytes;
		}

		Vector<ManagedCodeGate::Finding> Scan(bool release, u32* outFiles = nullptr, u64 sliceBytes = ~u64{0}) {
			Vector<ManagedCodeGate::Finding> findings{GetMemoryResource(MemTag::Debug)};
			const u32 files = _editor->scanForManagedFiles(_root.generic_string(), release, sliceBytes, findings);
			if (outFiles) {
				*outFiles = files;
			}
			return findings;
		}

		static fs::path Binaries() {
			return fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path();
		}
	};

} // anonymous namespace

TEST_F(ManagedCodeGateTest, ManagedAssemblyFails) {
	fs::create_directories(_root / "Data" / "Plugins");
	fs::copy_file(Binaries() / "CSharp" / "Shalltear.Runtime.dll", _root / "Data" / "Plugins" / "Helper.bin");
	Put("Readme.txt", "plain text");
	u32 files = 0;
	const Vector<ManagedCodeGate::Finding> findings = Scan(false, &files);
	EXPECT_EQ(files, 2u);
	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].file, "Data/Plugins/Helper.bin");
	EXPECT_EQ(findings[0].message, "'Data/Plugins/Helper.bin' has managed code. Remove it from build output");
}

TEST_F(ManagedCodeGateTest, ManagedImageFailsInEitherHeaderLayout) {
	Put("Managed32.bin", ExecutableImage(0x10B, true));
	Put("Managed64.bin", ExecutableImage(0x20B, true));
	Put("Native32.bin", ExecutableImage(0x10B, false));
	Put("Native64.bin", ExecutableImage(0x20B, false));
	const Vector<ManagedCodeGate::Finding> findings = Scan(false);
	ASSERT_EQ(findings.size(), 2u);
	EXPECT_EQ(findings[0].message, "'Managed32.bin' has managed code. Remove it from build output");
	EXPECT_EQ(findings[1].message, "'Managed64.bin' has managed code. Remove it from build output");
}

TEST_F(ManagedCodeGateTest, FileLongerThanSliceReadToEnd) {
	Put("Later.bin", str(SCAN_CHUNK * 2 + 17, 'B') + MetadataRoot("PDB v1.0"));
	Put("Plain.bin", str(SCAN_CHUNK * 3, 'z'));
	u32 files = 0;
	const Vector<ManagedCodeGate::Finding> findings = Scan(false, &files, 1);
	EXPECT_EQ(files, 2u);
	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].file, "Later.bin");
}

TEST_F(ManagedCodeGateTest, UnreadableFileFails) {
	Put("Locked.bin", "x");
	Put("Open.bin", "x");
	const str locked = (_root / "Locked.bin").generic_string();
	ASSERT_TRUE(TestPlatform::DenyFileReads(locked));
	const Vector<ManagedCodeGate::Finding> findings = Scan(false);
	TestPlatform::AllowFileReads(locked);
	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].message, "couldn't read 'Locked.bin'. Check nothing holds it open");
}

TEST_F(ManagedCodeGateTest, PortablePdbFails) {
	Put("Symbols.bin", str(100, 'x') + MetadataRoot("PDB v1.0") + str(64, 'y'));
	Put("Packed.bin", "pack header" + MetadataRoot("v4.0.30319"));
	Put("Edge.bin", str(SCAN_CHUNK - 6, 'B') + MetadataRoot("PDB v1.0"));
	Put("Later.bin", str(SCAN_CHUNK * 2 + 17, 'B') + MetadataRoot("PDB v1.0") + str(SCAN_CHUNK, 'z'));
	const Vector<ManagedCodeGate::Finding> findings = Scan(false);
	ASSERT_EQ(findings.size(), 4u);
	EXPECT_EQ(findings[0].message, "'Edge.bin' has .NET metadata. Remove it from build output");
	EXPECT_EQ(findings[1].file, "Later.bin");
	EXPECT_EQ(findings[2].file, "Packed.bin");
	EXPECT_EQ(findings[3].file, "Symbols.bin");
}

TEST_F(ManagedCodeGateTest, NativeLibraryPasses) {
	const fs::path package = Binaries() / (str("CSharp") + SLT_TEST_DLL_SUFFIX);
	std::ifstream stream(package, std::ios::binary);
	const str bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	ASSERT_NE(bytes.find("BSJB"), str::npos);
	Put("Package.bin", bytes);
	Put("Short.bin", "MZ");
	Put("Stub.bin", "MZ" + str(62, '\0'));
	str elsewhere = "MZ" + str(62, '\0');
	elsewhere[0x3C] = '\x7F';
	elsewhere[0x3D] = '\x7F';
	Put("Elsewhere.bin", elsewhere);
	u32 files = 0;
	EXPECT_TRUE(Scan(true, &files).empty());
	EXPECT_EQ(files, 4u);
}

TEST_F(ManagedCodeGateTest, RandomSignatureBytesPass) {
	str wrongMajor = MetadataRoot("v4.0.30319");
	wrongMajor[4] = 2;
	str reserved = MetadataRoot("v4.0.30319");
	reserved[8] = 1;
	str unevenLength = MetadataRoot("v4.0.30319");
	unevenLength[12] = 13;
	str noLength = MetadataRoot("v4.0.30319");
	noLength[12] = 0;
	str longLength = MetadataRoot("v4.0.30319");
	longLength[13] = 2;
	Put("Noise.bin", wrongMajor + reserved + unevenLength + noLength + longLength + MetadataRoot("x4.0.30319") + MetadataRoot("PDx v1.0") + "tail BSJB");
	Put("Cut.bin", str(SCAN_CHUNK - 2, 'B') + "BSJB");
	EXPECT_TRUE(Scan(true).empty());

	str longest = MetadataRoot(str("v") + str(250, '1'));
	ASSERT_EQ(static_cast<unsigned char>(longest[12]), 252u);
	Put("Longest.bin", longest);
	EXPECT_EQ(Scan(true).size(), 1u);
}

TEST_F(ManagedCodeGateTest, ForbiddenRuntimeFileFails) {
	const char* forbidden[] = {"Game.runtimeconfig.json", "Game.deps.json", "hostfxr.dll", "libhostpolicy.so",
							   "CoreCLR.dll", "bin/clrjit.dll", "System.Private.CoreLib.dll", "mscorlib.dll"};
	for (const char* name : forbidden) {
		Put(name, "x");
	}
	Put("Settings.json", "{}");
	Put("libcore.txt", "x");
	Put("Host.dll", "x");
	const Vector<ManagedCodeGate::Finding> findings = Scan(false);
	ASSERT_EQ(findings.size(), std::size(forbidden));
	for (const ManagedCodeGate::Finding& finding : findings) {
		EXPECT_NE(finding.message.find("' belongs to .NET runtime. Remove it from build output"), str::npos) << finding.message;
		EXPECT_NE(finding.file, "Settings.json");
		EXPECT_NE(finding.file, "libcore.txt");
		EXPECT_NE(finding.file, "Host.dll");
	}
}

TEST_F(ManagedCodeGateTest, SourceFileFails) {
	Put("Data/Scripts/Player.CS", "class Player {}");
	Put("Data/Theme.css", "a {}");
	const Vector<ManagedCodeGate::Finding> findings = Scan(false);
	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].message, "'Data/Scripts/Player.CS' is C# source. Remove it from build output");
}

TEST_F(ManagedCodeGateTest, SymbolFileFailsInRelease) {
	Put("GameScripts.pdb", "x");
	Put("Game.dbg", "x");
	Put("Game.dSYM/Contents/Info.plist", "x");
	Put("Game.pdb", "x");
	EXPECT_TRUE(Scan(false).empty());
	const Vector<ManagedCodeGate::Finding> findings = Scan(true);
	ASSERT_EQ(findings.size(), 3u);
	EXPECT_EQ(findings[0].file, "Game.dSYM/Contents/Info.plist");
	EXPECT_EQ(findings[1].file, "Game.dbg");
	EXPECT_EQ(findings[2].message, "'GameScripts.pdb' has debug symbols (not for Release). Remove it from build output");
}

#endif
