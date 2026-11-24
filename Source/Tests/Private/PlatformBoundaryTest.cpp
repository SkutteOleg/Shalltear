#include "TestsPCH.h"

#include "Checkout.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

	constexpr std::string_view PlatformTokens[] = {
		"_WIN32",
		"_WIN64",
		"__linux__",
		"__APPLE__",
		"__ANDROID__",
		"__MINGW32__",
		"__EMSCRIPTEN__",
		"SLT_PLATFORM_",
	};

	constexpr std::string_view SystemHeaders[] = {
		"<windows.h>",
		"<Windows.h>",
		"<windowsx.h>",
		"<shellapi.h>",
		"<shlobj.h>",
		"<dwmapi.h>",
		"<unistd.h>",
		"<dlfcn.h>",
		"<pthread.h>",
		"<poll.h>",
		"<spawn.h>",
		"<sys/",
		"<linux/",
		"<X11/",
		"<wayland-",
		"<xkbcommon/",
		"<emscripten",
	};

	constexpr std::string_view DeclaringFiles[] = {
		"Core/Public/Shalltear/Core/Platform.h",
		"Core/Public/Shalltear/Core/Export.h",
		"Core/Public/Shalltear/Core/Module/ModuleAPI.h",
		"Tests/Private/PlatformTest.cpp",
		"Tests/Private/PlatformBoundaryTest.cpp",
	};

	bool IsIdentifierChar(char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	bool NamesPlatform(std::string_view text) {
		for (std::string_view token : PlatformTokens) {
			for (size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
				if (at == 0 || !IsIdentifierChar(text[at - 1])) {
					return true;
				}
			}
		}
		for (std::string_view header : SystemHeaders) {
			if (text.find(header) != std::string_view::npos) {
				return true;
			}
		}
		return false;
	}

	bool IsPlatformFamilyFile(std::string_view relative) {
		constexpr std::string_view Marker = "/Private/Platform/";
		const size_t at = relative.find(Marker);
		return at != std::string_view::npos && relative.find('/', at + Marker.size()) != std::string_view::npos;
	}

	bool IsExempt(std::string_view relative) {
		if (relative.starts_with("Tests/Fixtures/") || relative.find("/ThirdParty/") != std::string_view::npos ||
			relative.ends_with("/stb_image.h") || IsPlatformFamilyFile(relative)) {
			return true;
		}
		for (std::string_view file : DeclaringFiles) {
			if (relative == file) {
				return true;
			}
		}
		return false;
	}

	bool IsSource(const std::filesystem::path& path) {
		const std::string extension = path.extension().string();
		return extension == ".cpp" || extension == ".h" || extension == ".c" || extension == ".inl";
	}

	std::string ReadAll(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	struct CastTarget {
		bool declared = false;
		bool exported = false;
		bool keyed = false;
	};

	std::string_view Trimmed(std::string_view line) {
		while (!line.empty() && (line.front() == '\t' || line.front() == ' ')) {
			line.remove_prefix(1);
		}
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.remove_suffix(1);
		}
		return line;
	}

	std::string_view CastTargetName(std::string_view text, size_t at) {
		const size_t close = text.find('>', at);
		if (close == std::string_view::npos) {
			return {};
		}
		std::string_view type = Trimmed(text.substr(at, close - at));
		if (type.starts_with("const ")) {
			type.remove_prefix(6);
		}
		while (!type.empty() && (type.back() == '*' || type.back() == '&' || type.back() == ' ')) {
			type.remove_suffix(1);
		}
		const size_t scope = type.rfind("::");
		return scope == std::string_view::npos ? type : type.substr(scope + 2);
	}

	bool DeclaresOutOfLineVirtual(std::string_view line) {
		return line.ends_with(";") && (line.starts_with("virtual ") || line.find(" override") != std::string_view::npos) &&
			   line.find("= 0") == std::string_view::npos && line.find("= default") == std::string_view::npos &&
			   line.find('{') == std::string_view::npos;
	}

	CastTarget ReadCastTarget(std::string_view header, std::string_view name) {
		CastTarget target;
		size_t at = 0;
		while (at < header.size() && !target.declared) {
			size_t end = header.find('\n', at);
			if (end == std::string_view::npos) {
				end = header.size();
			}
			const std::string_view whole = header.substr(at, end - at);
			std::string_view line = Trimmed(whole);
			at = end + 1;
			if (!line.starts_with("class ") || !line.ends_with("{")) {
				continue;
			}
			const std::string_view indentation = whole.substr(0, whole.find("class "));
			line.remove_prefix(6);
			bool exported = false;
			if (line.substr(0, line.find(' ')).ends_with("_API")) {
				exported = true;
				line.remove_prefix(line.find(' ') + 1);
			}
			if (line.substr(0, line.find(' ')) != name) {
				continue;
			}
			target.declared = true;
			target.exported = exported;

			const std::string closer = "\n" + std::string(indentation) + "};";
			size_t bodyEnd = header.find(closer, at);
			if (bodyEnd == std::string_view::npos) {
				bodyEnd = header.size();
			}
			while (at < bodyEnd && !target.keyed) {
				size_t lineEnd = header.find('\n', at);
				if (lineEnd == std::string_view::npos || lineEnd > bodyEnd) {
					lineEnd = bodyEnd;
				}
				target.keyed = DeclaresOutOfLineVirtual(Trimmed(header.substr(at, lineEnd - at)));
				at = lineEnd + 1;
			}
		}
		return target;
	}

} // anonymous namespace

TEST(PlatformBoundaryTest, MatcherFindsMacrosAndHeaders) {
	EXPECT_TRUE(NamesPlatform("#if defined(_WIN32)"));
	EXPECT_TRUE(NamesPlatform("#ifdef SLT_PLATFORM_LINUX"));
	EXPECT_TRUE(NamesPlatform("#include <sys/stat.h>"));
	EXPECT_FALSE(NamesPlatform("MemTag::Platform + SHALLTEAR_WIN32X"));
	EXPECT_FALSE(NamesPlatform("#include <filesystem>"));

	const std::filesystem::path header = Checkout::Find("Source/Core/Public/Shalltear/Core/Platform.h");
	ASSERT_FALSE(header.empty());
	EXPECT_TRUE(NamesPlatform(ReadAll(header)));
}

TEST(PlatformBoundaryTest, FamilyDirectoryIsRecognized) {
	EXPECT_TRUE(IsPlatformFamilyFile("Core/Private/Platform/Windows/WindowsClock.cpp"));
	EXPECT_TRUE(IsPlatformFamilyFile("Editor/Private/Platform/Linux/Window/X11/X11Window.cpp"));
	EXPECT_TRUE(IsPlatformFamilyFile("Tests/Private/Platform/Windows/WindowsTestPlatform.cpp"));
	EXPECT_FALSE(IsPlatformFamilyFile("Core/Private/Platform/Clock.h"));
	EXPECT_FALSE(IsPlatformFamilyFile("Core/Private/Time.cpp"));
}

TEST(PlatformBoundaryTest, SharedSourcesNameNoPlatform) {
	const std::filesystem::path source = Checkout::Find("Source");
	ASSERT_FALSE(source.empty());

	std::vector<std::string> offenders;
	size_t scanned = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
		if (!entry.is_regular_file(ec) || !IsSource(entry.path())) {
			continue;
		}
		const std::string relative = std::filesystem::relative(entry.path(), source, ec).generic_string();
		if (IsExempt(relative)) {
			continue;
		}
		++scanned;
		if (NamesPlatform(ReadAll(entry.path()))) {
			offenders.push_back(relative);
		}
	}

	EXPECT_GT(scanned, size_t{1300});
	EXPECT_TRUE(offenders.empty()) << offenders.size() << " file(s), 1st " << offenders.front();
}

TEST(PlatformBoundaryTest, CastTargetReaderFindsKeyFunction) {
	constexpr std::string_view Keyed = "\tclass SHALLTEAR_CORE_API Probe : public Base {\n"
									   "\tpublic:\n"
									   "\t\tvoid Plain();\n"
									   "\t\t~Probe() override;\n"
									   "\t};\n";
	constexpr std::string_view Inline = "\tclass Probe {\n"
										"\tpublic:\n"
										"\t\tvirtual ~Probe() = default;\n"
										"\t\tvirtual void On() { }\n"
										"\t\tvirtual void Pure() = 0;\n"
										"\t};\n"
										"\tclass Other {\n"
										"\t\tvirtual ~Other();\n"
										"\t};\n";

	const CastTarget keyed = ReadCastTarget(Keyed, "Probe");
	EXPECT_TRUE(keyed.declared);
	EXPECT_TRUE(keyed.exported);
	EXPECT_TRUE(keyed.keyed);

	const CastTarget inlined = ReadCastTarget(Inline, "Probe");
	EXPECT_TRUE(inlined.declared);
	EXPECT_FALSE(inlined.exported);
	EXPECT_FALSE(inlined.keyed);

	EXPECT_FALSE(ReadCastTarget(Keyed, "Prob").declared);
	EXPECT_FALSE(ReadCastTarget("\tclass Probe;\n", "Probe").declared);

	constexpr std::string_view Cast = "dynamic_cast<const Shalltear::Probe*>(component)";
	EXPECT_EQ(CastTargetName(Cast, Cast.find('<') + 1), "Probe");
}

TEST(PlatformBoundaryTest, PublicCastTargetsHaveKeyFunction) {
	const std::filesystem::path source = Checkout::Find("Source");
	ASSERT_FALSE(source.empty());

	constexpr std::string_view Cast = "dynamic_cast<";
	std::vector<std::string> publicHeaders;
	std::set<std::string> names;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
		if (!entry.is_regular_file(ec) || !IsSource(entry.path())) {
			continue;
		}
		const std::string relative = std::filesystem::relative(entry.path(), source, ec).generic_string();
		if (relative.starts_with("Tests/") || relative.find("/ThirdParty/") != std::string::npos) {
			continue;
		}
		std::string text = ReadAll(entry.path());
		for (size_t at = text.find(Cast); at != std::string::npos; at = text.find(Cast, at + 1)) {
			names.emplace(CastTargetName(text, at + Cast.size()));
		}
		if (relative.find("/Public/") != std::string::npos) {
			publicHeaders.push_back(std::move(text));
		}
	}

	std::vector<std::string> offenders;
	size_t checked = 0;
	for (const std::string& name : names) {
		for (const std::string& header : publicHeaders) {
			const CastTarget target = ReadCastTarget(header, name);
			if (!target.declared) {
				continue;
			}
			++checked;
			if (!target.exported || !target.keyed) {
				offenders.push_back(name);
			}
			break;
		}
	}

	EXPECT_GE(checked, size_t{5});
	EXPECT_TRUE(offenders.empty()) << offenders.size() << " type(s), 1st " << offenders.front();
}
