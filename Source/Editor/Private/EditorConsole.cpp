#include "Shalltear/Editor/EditorConsole.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"

#include <mutex>

namespace Shalltear {

	namespace {

		constexpr size_t RING_CAP = 4096;

		bool IsProjectFile(const str& file) {
			return !file.empty() && FileSystem::Virtualize(Path(file)).ToString().starts_with("project://");
		}

		// sink owns the ring (late events may still arrive)
		class EditorConsoleSink final : public ILogSink {
		public:
			EditorConsoleSink() {
				_slots.resize(RING_CAP);
			}

			void ReceiveLog(const LogEvent& event) override {
				std::lock_guard lock(_mutex);
				ConsoleEntry& entry = Claim();
				entry.level = event.level;
				entry.category = event.category;
				entry.message = event.message;
				entry.file = event.file ? str(event.file) : str();
				entry.function = event.function ? str(event.function) : str();
				entry.line = event.line;
				entry.column = 0;
				entry.clickable = IsProjectFile(entry.file);
				entry.sequence = _nextSequence++;
			}

			void Push(LogLevel level, std::string_view category, std::string_view message, const ConsoleOrigin& origin) {
				std::lock_guard lock(_mutex);
				ConsoleEntry& entry = Claim();
				entry.level = level;
				entry.category = str(category);
				entry.message = str(message);
				entry.file = origin.file;
				entry.function.clear();
				entry.line = origin.line;
				entry.column = origin.column;
				entry.clickable = !origin.file.empty();
				entry.sequence = _nextSequence++;
			}

			void Flush() override {}

			u64 ReadSince(u64 sinceSequence, Vector<ConsoleEntry>& out) {
				std::lock_guard lock(_mutex);
				if (_count == 0) {
					return sinceSequence;
				}
				u64 newest = _nextSequence - 1;
				u64 oldest = _nextSequence - _count;
				u64 first = sinceSequence + 1 > oldest ? sinceSequence + 1 : oldest;
				for (u64 sequence = first; sequence <= newest; ++sequence) {
					out.push_back(_slots[(_head + static_cast<size_t>(sequence - oldest)) % RING_CAP]);
				}
				return newest > sinceSequence ? newest : sinceSequence;
			}

			void MarkSeenThrough(u64 sequence) {
				std::lock_guard lock(_mutex);
				if (sequence > _seenThrough) {
					_seenThrough = sequence;
				}
			}

			ConsoleSummary Summarize(LogLevel messageFloor) {
				std::lock_guard lock(_mutex);
				ConsoleSummary summary;
				if (_count == 0) {
					return summary;
				}
				u64 newest = _nextSequence - 1;
				u64 oldest = _nextSequence - _count;
				auto slotFor = [&](u64 sequence) -> const ConsoleEntry& {
					return _slots[(_head + static_cast<size_t>(sequence - oldest)) % RING_CAP];
				};

				for (u64 sequence = newest; sequence > _seenThrough && sequence >= oldest; --sequence) {
					const ConsoleEntry& entry = slotFor(sequence);
					if (entry.level >= LogLevel::Error) {
						++summary.unseenErrors;
						if (summary.newestUnseenError.sequence == 0) {
							summary.newestUnseenError = entry;
						}
					} else if (entry.level == LogLevel::Warn) {
						++summary.unseenWarnings;
					}
				}

				for (u64 sequence = newest; sequence >= oldest; --sequence) {
					const ConsoleEntry& entry = slotFor(sequence);
					if (entry.level >= messageFloor) {
						summary.newest = entry;
						break;
					}
				}
				return summary;
			}

			void Clear() {
				std::lock_guard lock(_mutex);
				_head = 0;
				_count = 0;
			}

			u32 GetBufferedCount() {
				std::lock_guard lock(_mutex);
				return static_cast<u32>(_count);
			}

			u64 GetDroppedCount() {
				std::lock_guard lock(_mutex);
				return _dropped;
			}
		private:
			ConsoleEntry& Claim() {
				size_t slot;
				if (_count == RING_CAP) {
					slot = _head;
					_head = (_head + 1) % RING_CAP;
					++_dropped;
				} else {
					slot = (_head + _count) % RING_CAP;
					++_count;
				}
				return _slots[slot];
			}

			std::mutex _mutex;
			Vector<ConsoleEntry> _slots{GetMemoryResource(MemTag::Editor)};
			size_t _head = 0;
			size_t _count = 0;
			u64 _nextSequence = 1;
			u64 _dropped = 0;
			u64 _seenThrough = 0;
		};

		std::shared_ptr<EditorConsoleSink> _sink;

	} // anonymous namespace

	void EditorConsole::Initialize() {
		if (_sink) {
			return;
		}
		_sink = std::make_shared<EditorConsoleSink>();
		LogManager::AddSink(_sink);
	}

	void EditorConsole::Terminate() {
		if (!_sink) {
			return;
		}
		LogManager::RemoveSink(_sink);
		_sink.reset();
	}

	bool EditorConsole::IsInitialized() {
		return _sink != nullptr;
	}

	void EditorConsole::Push(LogLevel level, std::string_view category, std::string_view message,
							 const ConsoleOrigin& origin) {
		if (_sink) {
			_sink->Push(level, category, message, origin);
		}
	}

	u64 EditorConsole::ReadSince(u64 sinceSequence, Vector<ConsoleEntry>& out) {
		return _sink ? _sink->ReadSince(sinceSequence, out) : sinceSequence;
	}

	void EditorConsole::MarkSeenThrough(u64 sequence) {
		if (_sink) {
			_sink->MarkSeenThrough(sequence);
		}
	}

	ConsoleSummary EditorConsole::GetSummary(LogLevel messageFloor) {
		return _sink ? _sink->Summarize(messageFloor) : ConsoleSummary{};
	}

	void EditorConsole::Clear() {
		if (_sink) {
			_sink->Clear();
		}
	}

	u32 EditorConsole::GetBufferedCount() {
		return _sink ? _sink->GetBufferedCount() : 0;
	}

	u64 EditorConsole::GetDroppedCount() {
		return _sink ? _sink->GetDroppedCount() : 0;
	}

} // namespace Shalltear
