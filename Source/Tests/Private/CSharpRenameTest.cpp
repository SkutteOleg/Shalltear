#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "Checkout.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

using namespace Shalltear;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* HOST = "RenameHost.dll";

	struct RenameSet {
		str skip;
		fs::path root;
		fs::path built;
		str buildLog;
		int buildExit = -1;
		str renamerLog;
		int renamerExit = -1;
		str report;
		str map;
		str plain;
		int plainExit = -1;
		str renamed;
		int renamedExit = -1;
		str grantedLog;
		int grantedExit = -1;
		str grantedReport;
		str grantedRun;
		int grantedRunExit = -1;
	};

	RenameSet* _set = nullptr;

	str ReadBytes(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	str WithoutReturns(str text) {
		text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
		return text;
	}

	str Wide(std::string_view text) {
		str wide;
		for (const char c : text) {
			wide.push_back(c);
			wide.push_back('\0');
		}
		return wide;
	}

	bool IsNameByte(const str& bytes, size_t at, size_t step) {
		const unsigned char c = static_cast<unsigned char>(bytes[at]);
		return (std::isalnum(c) || c == '_') && (step == 1 || bytes[at + 1] == '\0');
	}

	bool HoldsName(const str& bytes, std::string_view name, bool wide) {
		const str needle = wide ? Wide(name) : str(name);
		const size_t step = wide ? 2 : 1;
		for (size_t at = bytes.find(needle); at != str::npos; at = bytes.find(needle, at + 1)) {
			const size_t end = at + needle.size();
			const bool joinedBefore = at >= step && IsNameByte(bytes, at - step, step);
			const bool joinedAfter = end + step <= bytes.size() && IsNameByte(bytes, end, step);
			if (!joinedBefore && !joinedAfter) {
				return true;
			}
		}
		return false;
	}

	str Mode(const str& output, int mode) {
		const str start = "mode " + std::to_string(mode) + "\n";
		const size_t from = output.find(start);
		if (from == str::npos) {
			return "no mode " + std::to_string(mode);
		}
		const size_t body = from + start.size();
		return output.substr(body, output.find("checksum ", body) - body);
	}

	str Checksums(const str& output) {
		str lines;
		for (size_t at = output.find("checksum "); at != str::npos; at = output.find("checksum ", at + 1)) {
			lines += output.substr(at, output.find('\n', at) - at) + "\n";
		}
		return lines;
	}

	std::set<str> DeclaredNames() {
		std::set<str> names;
		const std::regex identifier("[Zz]q[A-Za-z0-9_]*");
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(Checkout::Find("Source/Tests/Fixtures/CSharp/Rename"), ec)) {
			if (entry.path().extension() != ".cs") {
				continue;
			}
			const str text = ReadBytes(entry.path());
			for (std::sregex_iterator match(text.begin(), text.end(), identifier), end; match != end; ++match) {
				names.insert(match->str());
			}
		}
		return names;
	}

	class CSharpRenameTest : public ::testing::Test {
	protected:
		static void SetUpTestSuite() {
			_set = new RenameSet();
			RenameSet& set = *_set;
			const fs::path pinFile = Checkout::Find("Source/CSharp/DotNetPin.json");
			const fs::path fixtures = Checkout::Find("Source/Tests/Fixtures/CSharp/Rename");
			if (pinFile.empty() || fixtures.empty()) {
				set.skip = "engine checkout not found";
				return;
			}
			const str pinText = ReadBytes(pinFile);
			LinearAllocator arena(2 * pinText.size() + 1024, MemTag::Debug);
			const JSONValue pin = JSONParse(pinText, arena);
			const str framework(pin["targetFramework"].AsString());
			const fs::path sdk = Checkout::Root() / "External" / "Prebuilt" / "dotnet" / str(pin["sdkVersion"].AsString());
			const fs::path dotnet = sdk / ("dotnet" SLT_TEST_EXE_SUFFIX);
			std::error_code ec;
			if (!fs::is_regular_file(dotnet, ec)) {
				set.skip = "pinned .NET SDK not found";
				return;
			}
			const fs::path managed = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
			set.root = fs::temp_directory_path() / ("slt_cs_rename_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(set.root, ec);
			fs::copy(fixtures, set.root, fs::copy_options::recursive, ec);
			TestPlatform::SetEnvironment("DOTNET_ROOT", sdk.generic_string());
			TestPlatform::SetEnvironment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
			TestPlatform::SetEnvironment("DOTNET_NOLOGO", "1");
			TestPlatform::SetEnvironment("DOTNET_CLI_UI_LANGUAGE", "en-us");
			const str muxer = "\"" + dotnet.string() + "\" ";

			set.buildLog = TestPlatform::RunCommand(muxer + "msbuild \"" + (set.root / "Host" / "Host.csproj").string() +
														"\" -restore -p:Configuration=Release -nologo -v:q -tl:off -clp:NoSummary -nodeReuse:false "
														"-p:UseSharedCompilation=false -p:ShalltearTargetFramework=" +
														framework + " \"-p:ShalltearManagedDir=" + managed.generic_string() + "/\"",
													set.buildExit);
			if (set.buildExit != 0) {
				return;
			}
			set.built = set.root / "Host" / "bin" / "Release" / framework;
			std::ofstream(set.root / "refs.txt", std::ios::binary) << (set.built / "ZqGame.Scripts.dll").generic_string() << "\n"
																   << (set.built / "ZqExtra.Scripts.dll").generic_string() << "\n"
																   << (set.built / "Shalltear.Runtime.dll").generic_string() << "\n";
			const fs::path renamed = set.root / "renamed";
			const auto quoted = [](const fs::path& path) { return "\"" + path.generic_string() + "\""; };
			set.renamerLog = TestPlatform::RunCommand(muxer + quoted(managed / "Renamer" / "Shalltear.Renamer.dll") + " --out " + quoted(renamed) + " --map " +
														  quoted(set.root / "map.txt") + " --report " + quoted(set.root / "report.txt") + " --refs-file " +
														  quoted(set.root / "refs.txt") + " --rename ZqGame.Scripts --rename ZqExtra.Scripts " +
														  quoted(set.built / HOST),
													  set.renamerExit);
			if (set.renamerExit != 0) {
				return;
			}
			fs::copy_file(set.built / "Shalltear.Runtime.dll", renamed / "Shalltear.Runtime.dll", ec);
			fs::copy_file(set.built / "RenameHost.runtimeconfig.json", renamed / "RenameHost.runtimeconfig.json", ec);
			set.plain = WithoutReturns(TestPlatform::RunCommand(muxer + quoted(set.built / HOST), set.plainExit));
			set.renamed = WithoutReturns(TestPlatform::RunCommand(muxer + quoted(renamed / HOST), set.renamedExit));
			set.report = WithoutReturns(ReadBytes(set.root / "report.txt"));
			set.map = WithoutReturns(ReadBytes(set.root / "map.txt"));

			const fs::path granted = set.root / "granted";
			set.grantedLog = TestPlatform::RunCommand(muxer + quoted(managed / "Renamer" / "Shalltear.Renamer.dll") + " --out " + quoted(granted) + " --report " +
														  quoted(set.root / "granted.txt") + " --refs-file " + quoted(set.root / "refs.txt") +
														  " --rename ZqExtra.Scripts " + quoted(set.built / HOST),
													  set.grantedExit);
			for (const char* file : {"Shalltear.Runtime.dll", "ZqGame.Scripts.dll", "RenameHost.runtimeconfig.json"}) {
				fs::copy_file(set.built / file, granted / file, ec);
			}
			set.grantedRun = WithoutReturns(TestPlatform::RunCommand(muxer + quoted(granted / HOST), set.grantedRunExit));
			set.grantedReport = WithoutReturns(ReadBytes(set.root / "granted.txt"));
		}

		static void TearDownTestSuite() {
			if (_set) {
				std::error_code ec;
				if (!_set->root.empty()) {
					fs::remove_all(_set->root, ec);
				}
				delete _set;
				_set = nullptr;
			}
		}

		void SetUp() override {
			ASSERT_NE(_set, nullptr);
			if (!_set->skip.empty()) {
				GTEST_SKIP() << _set->skip;
			}
			ASSERT_EQ(_set->buildExit, 0) << _set->buildLog;
			ASSERT_EQ(_set->renamerExit, 0) << _set->renamerLog;
			ASSERT_EQ(_set->plainExit, 0) << _set->plain;
			ASSERT_EQ(_set->renamedExit, 0) << _set->renamed;
		}

		static bool Matches(const str& text, const char* pattern) { return std::regex_match(text, std::regex(pattern)); }
	};

} // anonymous namespace

TEST_F(CSharpRenameTest, ChecksumMatchesTheUnrenamedLibrary) {
	const str checksums = Checksums(_set->plain);
	EXPECT_EQ(std::count(checksums.begin(), checksums.end(), '\n'), 14);
	EXPECT_EQ(Checksums(_set->renamed), checksums) << _set->renamed;
	EXPECT_EQ(_set->renamed.find("escaped"), str::npos) << _set->renamed;
	EXPECT_NE(_set->renamerLog.find("C# script names renamed: "), str::npos) << _set->renamerLog;
	EXPECT_TRUE(_set->report.starts_with("C# script names renamed: ")) << _set->report;
}

TEST_F(CSharpRenameTest, NoDeclaredNameSurvives) {
	const std::set<str> declared = DeclaredNames();
	ASSERT_GT(declared.size(), 100u);
	const str plain = ReadBytes(_set->built / "ZqGame.Scripts.dll");
	ASSERT_TRUE(HoldsName(plain, "ZqHero", false));
	ASSERT_TRUE(HoldsName(plain, "zqAmount", false));
	std::set<str> asText;
	std::set<str> asLiteral;
	for (const char* file : {HOST, "s0.dll", "s1.dll"}) {
		const str bytes = ReadBytes(_set->root / "renamed" / file);
		ASSERT_FALSE(bytes.empty()) << file;
		for (const str& name : declared) {
			if (HoldsName(bytes, name, false)) {
				asText.insert(name);
			}
			if (HoldsName(bytes, name, true)) {
				asLiteral.insert(name);
			}
		}
	}
	EXPECT_EQ(asText, (std::set<str>{"ZqGame", "ZqGameTypeTable", "ZqSaveSlot", "ZqSlotAlpha", "ZqSlotBeta"}));
	EXPECT_EQ(asLiteral, (std::set<str>{"ZqBoom", "ZqFire", "ZqHero", "ZqLiteral", "ZqSlotBeta"}));
	std::error_code ec;
	EXPECT_FALSE(fs::exists(_set->root / "renamed" / "ZqGame.Scripts.dll", ec));
	EXPECT_NE(_set->map.find("assembly ZqGame.Scripts -> s0\n"), str::npos) << _set->map;
	EXPECT_NE(_set->map.find("type ZqGame.Scripts ZqGame.ZqHero -> "), str::npos) << _set->map;
}

TEST_F(CSharpRenameTest, KeepNameKeepsTypeAndMembers) {
	EXPECT_EQ(Mode(_set->plain, 207), "parsed 0\nparsed 1\n");
	EXPECT_EQ(Mode(_set->renamed, 207), "not parsed\nparsed 1\n");
	EXPECT_EQ(Mode(_set->renamed, 209), "ZqHero|ZqSlotAlpha\n");
	EXPECT_NE(_set->report.find("C# script name kept: type 'ZqGame.ZqSaveSlot' ([KeepName])\n"), str::npos) << _set->report;
	EXPECT_EQ(_set->report.find("ZqGameTypeTable"), str::npos) << _set->report;
}

TEST_F(CSharpRenameTest, AttributeArgumentsFollowAcrossAssemblies) {
	EXPECT_EQ(Mode(_set->plain, 210), "ZqMonster|2|ZqHeavy|2\n");
	EXPECT_EQ(Mode(_set->plain, 211), "ZqEnraged|ZqExtra.ZqBoss|ZqMonster\nZqHero|1|ZqFire|2\n");
	const str game = Mode(_set->renamed, 210);
	const str extra = Mode(_set->renamed, 211);
	EXPECT_TRUE(Matches(game, "[A-Za-z]+\\|2\\|[A-Za-z]+\\|2\n")) << game;
	EXPECT_TRUE(Matches(extra, "[A-Za-z]+\\|[A-Za-z]+\\|[A-Za-z]+\n[A-Za-z]+\\|1\\|[A-Za-z]+\\|2\n")) << extra;
	const str boss = extra.substr(0, extra.find('\n'));
	EXPECT_EQ(boss.substr(boss.rfind('|') + 1), game.substr(0, game.find('|'))) << extra;
}

TEST_F(CSharpRenameTest, GenericMemberReferencesFollow) {
	EXPECT_EQ(Mode(_set->plain, 203), "ZqBag`1|ZqGame.ZqHero+ZqInner\n");
	const str bag = Mode(_set->renamed, 203);
	EXPECT_TRUE(Matches(bag, "[A-Za-z]+`1\\|[A-Za-z]+\\+[A-Za-z]+\n")) << bag;
	EXPECT_EQ(Checksums(_set->renamed), Checksums(_set->plain));
}

TEST_F(CSharpRenameTest, VirtualNamedOutsideTheSetIsKept) {
	EXPECT_EQ(Mode(_set->plain, 212), "ZqLiteral:2|41\n");
	EXPECT_EQ(Mode(_set->renamed, 212), "ZqLiteral:2|41\n");
	EXPECT_NE(_set->report.find("C# script name kept: method 'ToString' (virtual, declared outside scripts)\n"), str::npos) << _set->report;
	EXPECT_EQ(_set->map.find("virtual ToString -> "), str::npos) << _set->map;
}

TEST_F(CSharpRenameTest, AssemblyGrantedInternalsFromOutsideKeepsName) {
	ASSERT_EQ(_set->grantedExit, 0) << _set->grantedLog;
	EXPECT_EQ(_set->grantedRunExit, 0) << _set->grantedRun;
	EXPECT_EQ(Checksums(_set->grantedRun), Checksums(_set->plain)) << _set->grantedRun;
	EXPECT_NE(_set->grantedReport.find("C# script name kept: assembly 'ZqExtra.Scripts' (internals from 'ZqGame.Scripts')\n"), str::npos)
		<< _set->grantedReport;
	std::error_code ec;
	EXPECT_TRUE(fs::exists(_set->root / "granted" / "ZqExtra.Scripts.dll", ec));
	EXPECT_FALSE(fs::exists(_set->root / "granted" / "s0.dll", ec));
}

TEST_F(CSharpRenameTest, RecordPrintsItsRenamedMembers) {
	EXPECT_EQ(Mode(_set->plain, 204), "ZqPoint { ZqX = 3, ZqY = 4 }|ZqSize { ZqWidth = 1, ZqHeight = 2 }\n");
	const str records = Mode(_set->renamed, 204);
	EXPECT_TRUE(Matches(records, "[A-Za-z]+ \\{ [A-Za-z]+ = 3, [A-Za-z]+ = 4 \\}\\|[A-Za-z]+ \\{ [A-Za-z]+ = 1, [A-Za-z]+ = 2 \\}\n")) << records;
	const str anonymous = Mode(_set->renamed, 208);
	EXPECT_TRUE(Matches(anonymous, "\\{ [A-Za-z]+ = x, [A-Za-z]+ = 6 \\}\n")) << anonymous;
	EXPECT_EQ(Mode(_set->renamed, 201).find("Zq"), str::npos);
	EXPECT_EQ(Mode(_set->renamed, 205).find("Zq"), str::npos);
}

#endif
