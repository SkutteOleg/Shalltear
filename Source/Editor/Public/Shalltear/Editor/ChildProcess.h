#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <deque>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

namespace Shalltear {

	SHALLTEAR_EDITOR_API str QuoteCommandArgument(const str& value);

	SHALLTEAR_EDITOR_API bool StartDetachedProcess(const str& commandLine, const str& workingDirectory);

	struct ChildVariable {
		str name;
		str value;
	};

	class SHALLTEAR_EDITOR_API ChildProcess {
	public:
		ChildProcess() = default;
		~ChildProcess();

		ChildProcess(const ChildProcess&) = delete;
		ChildProcess& operator=(const ChildProcess&) = delete;

		// used over contributed/inherited variables
		bool Start(const str& commandLine, const str& workingDirectory, std::span<const ChildVariable> variables = {});

		bool IsRunning() const { return _running; }

		u32 DrainLines(Vector<str>& out);

		bool TryFinish(i32& outExitCode);

		void Kill();

		void Detach();
	private:
		void CloseHandles();

		void* _process = nullptr;
		void* _readEnd = nullptr;
		std::thread _reader;
		bool _running = false;

		std::mutex _mutex;
		std::deque<str> _lines;
		str _partial;
		u64 _droppedLines = 0;
	};

	namespace ChildEnvironment {

		// applied to children started later, until owner is swept
		SHALLTEAR_EDITOR_API void Set(std::string_view name, std::string_view value, const void* owner);

		SHALLTEAR_EDITOR_API void PrependSearchPath(std::string_view directory, const void* owner);

		SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

	} // namespace ChildEnvironment

} // namespace Shalltear
