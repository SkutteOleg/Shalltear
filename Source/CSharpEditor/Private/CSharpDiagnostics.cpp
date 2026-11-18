#include "CSharpDiagnostics.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace Shalltear::CSharpEditor::CSharpDiagnostics {

	namespace {

		struct Category {
			std::string_view word;
			CompilerDiagnosticSeverity severity;
		};

		constexpr Category Categories[] = {
			{"error", CompilerDiagnosticSeverity::Error},
			{"warning", CompilerDiagnosticSeverity::Warning},
		};

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}

		bool IsCodeCharacter(char c) {
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		}

		bool ParseTail(std::string_view tail, std::string_view& outCode, std::string_view& outMessage) {
			tail = Trim(tail);
			size_t length = 0;
			while (length < tail.size() && IsCodeCharacter(tail[length])) {
				++length;
			}
			const std::string_view rest = Trim(tail.substr(length));
			if (rest.empty() || rest.front() != ':') {
				return false;
			}
			outCode = tail.substr(0, length);
			outMessage = Trim(rest.substr(1));
			return true;
		}

		bool FindCategory(std::string_view line, size_t& outAt, CompilerDiagnosticSeverity& outSeverity, std::string_view& outCode,
						  std::string_view& outMessage) {
			for (size_t at = 0; at < line.size(); ++at) {
				if (at != 0 && line[at - 1] != ' ' && line[at - 1] != ':') {
					continue;
				}
				for (const Category& category : Categories) {
					if (line.substr(at, category.word.size()) != category.word) {
						continue;
					}
					const size_t after = at + category.word.size();
					if (after < line.size() && line[after] != ' ' && line[after] != ':') {
						continue;
					}
					const std::string_view before = Trim(line.substr(0, at));
					if (!before.empty() && before.find(':') == std::string_view::npos) {
						continue;
					}
					if (!ParseTail(line.substr(after), outCode, outMessage)) {
						continue;
					}
					outAt = at;
					outSeverity = category.severity;
					return true;
				}
			}
			return false;
		}

		void SplitLocation(std::string_view origin, Diagnostic& out) {
			out.file = str(origin);
			if (origin.empty() || origin.back() != ')') {
				return;
			}
			const size_t open = origin.rfind('(');
			if (open == std::string_view::npos || open == 0) {
				return;
			}
			const std::string_view inside = origin.substr(open + 1, origin.size() - open - 2);
			if (inside.empty() || !std::all_of(inside.begin(), inside.end(), [](char c) { return (c >= '0' && c <= '9') || c == ','; })) {
				return;
			}
			u32 numbers[2] = {0, 0};
			size_t start = 0;
			for (u32& number : numbers) {
				const size_t comma = inside.find(',', start);
				const std::string_view digits = inside.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
				std::from_chars(digits.data(), digits.data() + digits.size(), number);
				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
			out.file = str(Trim(origin.substr(0, open)));
			out.line = numbers[0];
			out.column = numbers[1];
		}

	} // anonymous namespace

	bool ParseLine(std::string_view line, Diagnostic& out) {
		line = Trim(line);
		if (!line.empty() && line.back() == ']') {
			const size_t open = line.rfind(" [");
			const std::string_view project = open == std::string_view::npos ? std::string_view() : line.substr(open + 2, line.size() - open - 3);
			if (project.ends_with("proj") || project.ends_with(".slnx") || project.ends_with(".sln")) {
				line = Trim(line.substr(0, open));
			}
		}

		size_t at = 0;
		CompilerDiagnosticSeverity severity = CompilerDiagnosticSeverity::Error;
		std::string_view code;
		std::string_view message;
		if (!FindCategory(line, at, severity, code, message)) {
			return false;
		}

		const std::string_view before = line.substr(0, at);
		const size_t separator = before.rfind(':');
		const std::string_view origin = separator == std::string_view::npos ? std::string_view() : Trim(before.substr(0, separator));

		out = {};
		out.severity = severity;
		out.code = str(code);
		out.message = str(message);
		SplitLocation(origin, out);
		if (out.line == 0 && out.file.find_first_of("/\\") == str::npos) {
			out.file.clear();
		}
		return true;
	}

	bool AddUnique(Vector<Diagnostic>& all, HashSet<str>& seen, const Diagnostic& diagnostic) {
		str key = Describe(diagnostic);
		key.push_back(static_cast<char>('0' + static_cast<u8>(diagnostic.severity)));
		if (!seen.insert(std::move(key)).second) {
			return false;
		}
		all.push_back(diagnostic);
		return true;
	}

	str Describe(const Diagnostic& diagnostic) {
		const str text = diagnostic.code.empty() ? diagnostic.message : std::format("{}: {}", diagnostic.code, diagnostic.message);
		if (diagnostic.file.empty()) {
			return text;
		}
		if (diagnostic.line == 0) {
			return std::format("{}: {}", diagnostic.file, text);
		}
		return diagnostic.column != 0 ? std::format("{}:{}:{}: {}", diagnostic.file, diagnostic.line, diagnostic.column, text)
									  : std::format("{}:{}: {}", diagnostic.file, diagnostic.line, text);
	}

} // namespace Shalltear::CSharpEditor::CSharpDiagnostics
