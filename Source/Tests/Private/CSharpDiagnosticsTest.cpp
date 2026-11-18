#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	class CSharpDiagnosticsTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
		}

		CSharpDiagnostics::Diagnostic Parsed(std::string_view line) {
			CSharpDiagnostics::Diagnostic diagnostic;
			EXPECT_TRUE(_editor->parseDiagnostic(line, diagnostic)) << line;
			return diagnostic;
		}
	};

} // anonymous namespace

TEST_F(CSharpDiagnosticsTest, Error) {
	const CSharpDiagnostics::Diagnostic diagnostic =
		Parsed("C:\\Games\\Mine\\Assets\\Scripts\\Broken.cs(11,3): error CS0103: The name 'Missing' does not exist in the current context "
			   "[C:\\Games\\Mine\\Mine.Scripts.csproj]");
	EXPECT_EQ(diagnostic.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(diagnostic.file, "C:\\Games\\Mine\\Assets\\Scripts\\Broken.cs");
	EXPECT_EQ(diagnostic.line, 11u);
	EXPECT_EQ(diagnostic.column, 3u);
	EXPECT_EQ(diagnostic.code, "CS0103");
	EXPECT_EQ(diagnostic.message, "The name 'Missing' does not exist in the current context");
	EXPECT_EQ(_editor->describeDiagnostic(diagnostic),
			  "C:\\Games\\Mine\\Assets\\Scripts\\Broken.cs:11:3: CS0103: The name 'Missing' does not exist in the current context");
}

TEST_F(CSharpDiagnosticsTest, Warning) {
	const CSharpDiagnostics::Diagnostic diagnostic = Parsed(
		"/home/u/mine/Assets/Broken.cs(9,7): warning CS0168: The variable 'unused' is declared but never used [/home/u/mine/Mine.Scripts.csproj]\r");
	EXPECT_EQ(diagnostic.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(diagnostic.file, "/home/u/mine/Assets/Broken.cs");
	EXPECT_EQ(diagnostic.line, 9u);
	EXPECT_EQ(diagnostic.column, 7u);
	EXPECT_EQ(diagnostic.code, "CS0168");
	EXPECT_EQ(diagnostic.message, "The variable 'unused' is declared but never used");

	const CSharpDiagnostics::Diagnostic named =
		Parsed("C:\\x\\A.cs(7,1): warning CS8981: The type name 'error' only contains lower-cased ascii characters");
	EXPECT_EQ(named.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(named.code, "CS8981");
}

TEST_F(CSharpDiagnosticsTest, ProjectLevel) {
	CSharpDiagnostics::Diagnostic diagnostic = Parsed("MSBUILD : error MSB1009: Project file does not exist.");
	EXPECT_TRUE(diagnostic.file.empty());
	EXPECT_EQ(diagnostic.line, 0u);
	EXPECT_EQ(diagnostic.code, "MSB1009");
	EXPECT_EQ(_editor->describeDiagnostic(diagnostic), "MSB1009: Project file does not exist.");

	diagnostic = Parsed("CSC : error CS2001: Source file 'C:\\x\\Gone.cs' could not be found. [C:\\x\\Mine.Scripts.csproj]");
	EXPECT_TRUE(diagnostic.file.empty());
	EXPECT_EQ(diagnostic.message, "Source file 'C:\\x\\Gone.cs' could not be found.");

	diagnostic = Parsed("C:\\x\\Mine.Scripts.csproj : error NU1101: Unable to find package No.Such. No packages exist with this id in source(s): C:\\feed");
	EXPECT_EQ(diagnostic.file, "C:\\x\\Mine.Scripts.csproj");
	EXPECT_EQ(diagnostic.line, 0u);
	EXPECT_EQ(diagnostic.code, "NU1101");
	EXPECT_EQ(diagnostic.message, "Unable to find package No.Such. No packages exist with this id in source(s): C:\\feed");
	EXPECT_EQ(_editor->describeDiagnostic(diagnostic),
			  "C:\\x\\Mine.Scripts.csproj: NU1101: Unable to find package No.Such. No packages exist with this id in source(s): C:\\feed");

	diagnostic = Parsed("ILC : Trim analysis warning IL2026: Using member 'X' which has 'RequiresUnreferencedCode' [C:\\x\\Mine.csproj]");
	EXPECT_EQ(diagnostic.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_TRUE(diagnostic.file.empty());
	EXPECT_EQ(diagnostic.code, "IL2026");
	EXPECT_EQ(diagnostic.message, "Using member 'X' which has 'RequiresUnreferencedCode'");
}

TEST_F(CSharpDiagnosticsTest, DuplicatesRemoved) {
	Vector<CSharpDiagnostics::Diagnostic> all{GetMemoryResource(MemTag::Editor)};
	HashSet<str> seen{GetMemoryResource(MemTag::Editor)};
	EXPECT_TRUE(_editor->addUniqueDiagnostic(all, seen, Parsed("C:\\x\\A.cs(7,1): error CS0029: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_FALSE(_editor->addUniqueDiagnostic(all, seen, Parsed("  C:\\x\\A.cs(7,1): error CS0029: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_TRUE(_editor->addUniqueDiagnostic(all, seen, Parsed("C:\\x\\A.cs(8,1): error CS0029: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_TRUE(_editor->addUniqueDiagnostic(all, seen, Parsed("C:\\x\\A.cs(7,1): warning CS0029: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_TRUE(_editor->addUniqueDiagnostic(all, seen, Parsed("C:\\x\\A.cs(7,1): error CS0030: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_TRUE(_editor->addUniqueDiagnostic(all, seen, Parsed("C:\\x\\B.cs(7,1): error CS0029: Cannot convert [C:\\x\\Mine.Scripts.csproj]")));
	EXPECT_EQ(all.size(), 5u);
}

TEST_F(CSharpDiagnosticsTest, PathWithSpacesAndParentheses) {
	CSharpDiagnostics::Diagnostic diagnostic =
		Parsed("C:\\Games\\proj (1)\\Assets\\My Scripts\\Broken.cs(10,26): error CS1002: ; expected [C:\\Games\\proj (1)\\Drive_Game.Scripts.csproj]");
	EXPECT_EQ(diagnostic.file, "C:\\Games\\proj (1)\\Assets\\My Scripts\\Broken.cs");
	EXPECT_EQ(diagnostic.line, 10u);
	EXPECT_EQ(diagnostic.column, 26u);
	EXPECT_EQ(diagnostic.message, "; expected");

	diagnostic = Parsed("C:\\my error log\\A.cs(1,2): error CS1002: ; expected");
	EXPECT_EQ(diagnostic.file, "C:\\my error log\\A.cs");
	EXPECT_EQ(diagnostic.line, 1u);
	EXPECT_EQ(diagnostic.column, 2u);
}

TEST_F(CSharpDiagnosticsTest, LocationAndCodeForms) {
	CSharpDiagnostics::Diagnostic diagnostic = Parsed("C:\\x\\A.cs(4): error : stopped here [C:\\x\\Mine.Scripts.csproj]");
	EXPECT_EQ(diagnostic.file, "C:\\x\\A.cs");
	EXPECT_EQ(diagnostic.line, 4u);
	EXPECT_EQ(diagnostic.column, 0u);
	EXPECT_TRUE(diagnostic.code.empty());
	EXPECT_EQ(diagnostic.message, "stopped here");
	EXPECT_EQ(_editor->describeDiagnostic(diagnostic), "C:\\x\\A.cs:4: stopped here");

	diagnostic = Parsed("C:\\x\\A.cs(4,5,4,9): warning CS0219: assigned but never used [C:\\x\\Mine.Scripts.csproj]");
	EXPECT_EQ(diagnostic.line, 4u);
	EXPECT_EQ(diagnostic.column, 5u);

	EXPECT_EQ(Parsed("C:\\x\\A.cs(7,1): error CS0029: Cannot convert 'int' to 'int[]'").message, "Cannot convert 'int' to 'int[]'");
	EXPECT_EQ(Parsed("C:\\x\\A.cs(7,1): error CS0029: Cannot convert 'int' to 'int[]' [C:\\x\\Mine.Scripts.csproj]").message,
			  "Cannot convert 'int' to 'int[]'");
	EXPECT_EQ(Parsed("C:\\x\\A.cs(1,2): error CS0021: bad index [y]").message, "bad index [y]");
	EXPECT_EQ(Parsed("MSBUILD : error MSB1011: more than one project [C:\\x\\Game.slnx]").message, "more than one project");
	EXPECT_EQ(Parsed("MSBUILD : error MSB1011: more than one project [C:\\x\\Game.sln]").message, "more than one project");
	EXPECT_EQ(Parsed("\tC:\\x\\A.cs(4,5): error CS1002: ; expected").file, "C:\\x\\A.cs");

	diagnostic = Parsed("C:\\x\\a(123: error CS1002: ; expected");
	EXPECT_EQ(diagnostic.file, "C:\\x\\a(123");
	EXPECT_EQ(diagnostic.line, 0u);

	diagnostic = Parsed("C:\\x\\A.cs(copy): error CS1002: ; expected");
	EXPECT_EQ(diagnostic.file, "C:\\x\\A.cs(copy)");
	EXPECT_EQ(diagnostic.line, 0u);

	diagnostic = Parsed("(12,3): error CS1002: ; expected");
	EXPECT_TRUE(diagnostic.file.empty());
	EXPECT_EQ(diagnostic.line, 0u);
	EXPECT_EQ(diagnostic.column, 0u);
}

TEST_F(CSharpDiagnosticsTest, OrdinaryOutputIsNoDiagnostic) {
	for (const char* line : {"", "Build FAILED.", "Build succeeded.", "    0 Warning(s)", "    1 Error(s)", "Time Elapsed 00:00:01.77",
							 "  Drive_Game.Scripts -> C:\\x\\Library\\CSharp\\bin\\Drive_Game.Scripts.dll", "  Determining projects to restore...",
							 "Switch: Nope.csproj", "an error happened somewhere", "warning", "error", "C:\\x\\A.cs(1,2): Terror CS1002: ; expected",
							 "C:\\x\\A.cs(1,2): errorCS1002: ; expected", "the build stopped at error CS0103: The name does not exist"}) {
		CSharpDiagnostics::Diagnostic diagnostic;
		EXPECT_FALSE(_editor->parseDiagnostic(line, diagnostic)) << line;
	}
}

#endif
