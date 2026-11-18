#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Types.h"

#include <filesystem>
#include <fstream>

namespace Shalltear::CSharpEditor::ManagedCodeGate {

	struct Finding {
		// relative to scanned directory
		str file;
		str message;
	};

	class Scanner {
	public:
		void Begin(const str& directory, bool release);
		void Advance(u64 byteBudget);

		bool IsDone() const { return _next >= _files.size(); }
		const Vector<Finding>& GetFindings() const { return _findings; }
		u32 GetFileCount() const { return static_cast<u32>(_files.size()); }
	private:
		void Open();
		void Report(std::string_view reason);

		Vector<std::filesystem::path> _files{GetMemoryResource(MemTag::Editor)};
		Vector<Finding> _findings{GetMemoryResource(MemTag::Editor)};
		Vector<u8> _buffer{GetMemoryResource(MemTag::Editor)};
		std::filesystem::path _root;
		std::ifstream _file;
		size_t _next = 0;
		size_t _kept = 0;
		bool _release = false;
		bool _open = false;
	};

	u32 Scan(const str& directory, bool release, u64 sliceBytes, Vector<Finding>& outFindings);

} // namespace Shalltear::CSharpEditor::ManagedCodeGate
