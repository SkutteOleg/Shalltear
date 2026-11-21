#include "CSharpAssemblies.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace Shalltear::CSharpEditor::CSharpAssemblies {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view PlayerShell = "GameScripts";
		constexpr std::string_view EnginePrefix = "Shalltear.";

		str NameFromFile(const fs::path& file) {
			str name;
			for (const char c : file.stem().generic_string()) {
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
					name.push_back(c);
				}
			}
			return name;
		}

		bool IsAssemblyName(std::string_view name) {
			if (name.empty() || name.front() == '.' || name.back() == '.' || std::isdigit(static_cast<unsigned char>(name.front()))) {
				return false;
			}
			return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; });
		}

		bool IsSymbol(std::string_view name) {
			if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front()))) {
				return false;
			}
			return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
		}

		void ReadList(const JSONValue& array, Vector<str>& out) {
			for (size_t index = 0; index < array.Size(); ++index) {
				const str item = StringUtil::Trim(str(array[index].AsString()));
				if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) {
					out.push_back(item);
				}
			}
		}

		bool Read(const fs::path& root, const fs::path& file, Definition& out, Set& set) {
			out.file = file.lexically_relative(root).generic_string();
			out.folder = file.parent_path().lexically_relative(root).generic_string();
			const str text = FileSystem::ReadTextFile(Path(file.generic_string()), MemTag::Editor);
			LinearAllocator arena(text.size() + 64, MemTag::Editor);
			const JSONValue document = text.empty() ? JSONValue() : JSONParse(text, arena);
			if (!document.IsObject()) {
				set.problems.push_back({out.file, "isn't a JSON object (ignored). Create it again from the Asset Browser"});
				return false;
			}
			out.name = StringUtil::Trim(str(document["assemblyName"].AsString()));
			if (out.name.empty()) {
				out.name = NameFromFile(file);
			}
			out.editorOnly = document["editorOnly"].AsBool();
			ReadList(document["references"], out.references);
			ReadList(document["platforms"], out.platforms);
			ReadList(document["defines"], out.defines);
			return true;
		}

		bool IsInside(std::string_view folder, std::string_view parent) {
			return folder.size() > parent.size() && folder.starts_with(parent) && folder[parent.size()] == '/';
		}

		bool Check(const Definition& definition, const Vector<Definition>& read, std::string_view defaultAssembly, Set& set) {
			const auto refuse = [&](str message) {
				set.problems.push_back({definition.file, std::move(message)});
				return false;
			};
			if (!IsAssemblyName(definition.name)) {
				return refuse(std::format("has assembly name '{}' (ignored). Use letters, digits, _ and .", definition.name));
			}
			if (definition.name == defaultAssembly || definition.name == PlayerShell || definition.name.starts_with(EnginePrefix)) {
				return refuse(std::format("has assembly name '{}' (taken by engine). Rename it", definition.name));
			}
			for (const Definition& other : read) {
				if (&other == &definition) {
					break;
				}
				if (other.name == definition.name) {
					return refuse(std::format("has assembly name '{}' (taken by '{}'). Rename one", definition.name, other.file));
				}
				if (other.folder == definition.folder) {
					return refuse(std::format("shares its folder with '{}' (ignored). Keep 1 assembly definition per folder", other.file));
				}
			}
			for (const str& symbol : definition.defines) {
				if (!IsSymbol(symbol)) {
					return refuse(std::format("defines '{}' (ignored). Use letters, digits and _", symbol));
				}
			}
			return true;
		}

	} // anonymous namespace

	Set Scan(const str& projectPath, std::string_view defaultAssembly) {
		Set set;
		const fs::path root(projectPath);
		Vector<fs::path> files{GetMemoryResource(MemTag::Editor)};
		std::error_code ec;
		for (fs::recursive_directory_iterator it(root / "Assets", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
			 it.increment(ec)) {
			std::error_code entry;
			if (it->is_regular_file(entry) && StringUtil::ToLower(it->path().extension().generic_string()) == Extension) {
				files.push_back(it->path());
			}
		}
		std::sort(files.begin(), files.end());

		Vector<Definition> read{GetMemoryResource(MemTag::Editor)};
		read.reserve(files.size());
		for (const fs::path& file : files) {
			Definition definition;
			if (Read(root, file, definition, set)) {
				read.push_back(std::move(definition));
			}
		}
		Vector<Definition> kept{GetMemoryResource(MemTag::Editor)};
		for (const Definition& definition : read) {
			if (Check(definition, read, defaultAssembly, set)) {
				kept.push_back(definition);
			}
		}

		bool dropped = true;
		while (dropped) {
			dropped = false;
			for (size_t index = 0; index < kept.size() && !dropped; ++index) {
				const Definition& definition = kept[index];
				for (const str& reference : definition.references) {
					const auto found = std::find_if(kept.begin(), kept.end(), [&reference](const Definition& other) { return other.name == reference; });
					str problem;
					if (reference == definition.name) {
						problem = "references itself (ignored). Remove the reference";
					} else if (found == kept.end()) {
						problem = std::format("references '{}' (no such assembly definition). Fix the name (or remove the reference)", reference);
					} else if (found->editorOnly && !definition.editorOnly) {
						problem = std::format("references editor-only '{}' (ignored). Tick Editor Only here too (or remove the reference)", reference);
					}
					if (!problem.empty()) {
						set.problems.push_back({definition.file, std::move(problem)});
						kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
						dropped = true;
						break;
					}
				}
			}
		}

		while (!kept.empty()) {
			const auto ready = std::find_if(kept.begin(), kept.end(), [&set](const Definition& definition) {
				return std::all_of(definition.references.begin(), definition.references.end(),
								   [&set](const str& reference) { return Find(set, reference) != nullptr; });
			});
			if (ready == kept.end()) {
				for (const Definition& definition : kept) {
					set.problems.push_back({definition.file, "is part of a reference cycle (ignored). Remove 1 reference of the cycle"});
				}
				break;
			}
			set.definitions.push_back(*ready);
			kept.erase(ready);
		}

		for (Definition& definition : set.definitions) {
			for (const Definition& other : set.definitions) {
				if (IsInside(other.folder, definition.folder)) {
					definition.nestedFolders.push_back(other.folder);
				}
			}
		}
		return set;
	}

	bool BuildsFor(const Definition& definition, std::string_view targetId) {
		if (definition.editorOnly) {
			return false;
		}
		return definition.platforms.empty() || std::find(definition.platforms.begin(), definition.platforms.end(), targetId) != definition.platforms.end();
	}

	const Definition* Find(const Set& set, std::string_view name) {
		const auto found = std::find_if(set.definitions.begin(), set.definitions.end(), [name](const Definition& definition) { return definition.name == name; });
		return found == set.definitions.end() ? nullptr : &*found;
	}

} // namespace Shalltear::CSharpEditor::CSharpAssemblies

SLT_REGISTER_TYPE(Shalltear::CSharpEditor::CSharpAssemblyDefinition)
