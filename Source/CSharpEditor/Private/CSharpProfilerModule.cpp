#include "CSharpProfilerModule.h"

#include "ScriptProfile.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/ProfilerModules.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <vector>

namespace Shalltear::CSharpEditor {

	namespace {

		namespace ScriptProfile = CSharpImpl::ScriptProfile;
		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		constexpr size_t MAX_ROWS = 40;

		struct Column {
			const char* title;
			f32 width;
		};
		constexpr Column Columns[] = {
			{"Script", 220.0f},
			{"Hook", 110.0f},
			{"Calls", 60.0f},
			{"ms", 70.0f},
			{"Bytes", 80.0f},
		};
		constexpr u32 COLUMN_COUNT = static_cast<u32>(std::size(Columns));

		struct ProfilerState {
			std::vector<ScriptProfile::Row> window;
			std::vector<ScriptProfile::Row> read;
			ScriptProfile::Collections collections;
			u32 frames = 0;
		};
		ProfilerState* _state = nullptr;

		ProfilerState& State() {
			if (!_state) {
				_state = New<ProfilerState>(MemTag::Editor);
			}
			return *_state;
		}

		Handle<FontAsset> Font() { return UISystem::IsInitialized() ? UISystem::GetDefaultFont() : Handle<FontAsset>{}; }

		UI::ElementHandle FirstChild(const UI::Context& context, UI::ElementHandle parent) {
			const UI::Element* pod = context.GetElement(parent);
			return pod ? context.MakeHandle(pod->firstChild) : UI::ElementHandle{};
		}

		UI::ElementHandle NextSibling(const UI::Context& context, UI::ElementHandle element) {
			const UI::Element* pod = context.GetElement(element);
			return pod ? context.MakeHandle(pod->nextSibling) : UI::ElementHandle{};
		}

		UI::ElementHandle Line(UI::Context& context, UI::ElementHandle parent, Color color, std::string_view text) {
			UI::ElementHandle line = context.CreateElement();
			context.AppendChild(parent, line);
			context.SetHoverable(line, false);
			context.SetText(line, Font(), EditorTheme::GetMetric(MetricToken::FontSmall), color, text);
			return line;
		}

		UI::ElementHandle Row(UI::Context& context, UI::ElementHandle table, Color color) {
			UI::ElementHandle row = context.CreateElement();
			context.AppendChild(table, row);
			context.SetHoverable(row, false);
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Row;
			style.width = UI::Length::Percent(1.0f);
			context.SetStyle(row, style);
			for (const Column& column : Columns) {
				UI::ElementHandle cell = Line(context, row, color, "");
				UI::Style cellStyle;
				cellStyle.width = UI::Length::Pixel(column.width);
				cellStyle.minWidth = UI::Length::Pixel(0.0f);
				cellStyle.textOverflow = UI::TextOverflow::Ellipsis;
				context.SetStyle(cell, cellStyle);
			}
			return row;
		}

		void Build(UI::Context& context, UI::ElementHandle host) {
			UI::ElementHandle summary = context.CreateElement();
			context.AppendChild(host, summary);
			UI::Style column;
			column.flexDirection = UI::FlexDirection::Column;
			context.SetStyle(summary, column);

			UI::ElementHandle table = context.CreateElement();
			context.AppendChild(host, table);
			context.SetStyle(table, column);
			UI::ElementHandle cell = FirstChild(context, Row(context, table, EditorTheme::GetColor(ColorToken::TextDim)));
			for (const Column& titled : Columns) {
				context.SetTextString(cell, titled.title);
				cell = NextSibling(context, cell);
			}
		}

		void Refresh(UI::Context& context, UI::ElementHandle host) {
			ProfilerState& state = State();
			const UI::ElementHandle summary = FirstChild(context, host);
			const UI::ElementHandle table = NextSibling(context, summary);
			if (!context.IsAlive(summary) || !context.IsAlive(table)) {
				return;
			}
			const f64 frames = static_cast<f64>(std::max(state.frames, 1u));
			std::sort(state.window.begin(), state.window.end(),
					  [](const ScriptProfile::Row& left, const ScriptProfile::Row& right) { return left.milliseconds > right.milliseconds; });
			u64 calls = 0;
			u64 bytes = 0;
			f64 milliseconds = 0.0;
			for (const ScriptProfile::Row& row : state.window) {
				calls += row.calls;
				bytes += row.bytes;
				milliseconds += row.milliseconds;
			}

			context.ClearChildren(summary);
			const Color text = EditorTheme::GetColor(ColorToken::Text);
			Line(context, summary, text,
				 std::format("Hooks per frame: {:.0f} call(s)   {:.3f} ms   {:.0f} B allocated", static_cast<f64>(calls) / frames, milliseconds / frames,
							 static_cast<f64>(bytes) / frames));
			const ScriptProfile::Collections& collections = state.collections;
			Line(context, summary, collections.counts[0] > 0 ? EditorTheme::GetColor(ColorToken::StatusWarn) : EditorTheme::GetColor(ColorToken::TextDim),
				 std::format("GC: {} gen0, {} gen1, {} gen2 collection(s)   pause {:.3f} ms   heap {:.1f} MiB", collections.counts[0],
							 collections.counts[1], collections.counts[2], collections.pauseMilliseconds,
							 static_cast<f64>(collections.heapBytes) / (1024.0 * 1024.0)));

			const size_t shown = std::min(state.window.size(), MAX_ROWS);
			size_t have = 0;
			UI::ElementHandle row = NextSibling(context, FirstChild(context, table));
			for (; context.IsAlive(row) && have < shown; ++have) {
				row = NextSibling(context, row);
			}
			while (context.IsAlive(row)) {
				const UI::ElementHandle next = NextSibling(context, row);
				context.DestroyElement(row);
				row = next;
			}
			for (; have < shown; ++have) {
				Row(context, table, text);
			}
			row = NextSibling(context, FirstChild(context, table));
			for (size_t index = 0; index < shown; ++index) {
				const ScriptProfile::Row& cost = state.window[index];
				const str cells[COLUMN_COUNT] = {
					cost.type,
					ScriptProfile::GetHookName(cost.hook),
					std::format("{:.0f}", static_cast<f64>(cost.calls) / frames),
					std::format("{:.3f}", cost.milliseconds / frames),
					std::format("{:.0f}", static_cast<f64>(cost.bytes) / frames),
				};
				UI::ElementHandle cell = FirstChild(context, row);
				for (const str& value : cells) {
					if (context.GetTextString(cell) != value) {
						context.SetTextString(cell, value);
					}
					cell = NextSibling(context, cell);
				}
				row = NextSibling(context, row);
			}
			state.window.clear();
			state.collections = {};
			state.frames = 0;
		}

		void Sample() {
			ProfilerState& state = State();
			ScriptProfile::Collections collections;
			ScriptProfile::Read(state.read, collections);
			++state.frames;
			for (size_t generation = 0; generation < std::size(collections.counts); ++generation) {
				state.collections.counts[generation] += collections.counts[generation];
			}
			state.collections.pauseMilliseconds += collections.pauseMilliseconds;
			state.collections.heapBytes = collections.heapBytes ? collections.heapBytes : state.collections.heapBytes;
			for (const ScriptProfile::Row& read : state.read) {
				const auto found = std::find_if(state.window.begin(), state.window.end(), [&read](const ScriptProfile::Row& held) {
					return held.hook == read.hook && held.type == read.type;
				});
				if (found == state.window.end()) {
					state.window.push_back(read);
					continue;
				}
				found->calls += read.calls;
				found->milliseconds += read.milliseconds;
				found->bytes += read.bytes;
			}
		}

	} // anonymous namespace

	bool RegisterProfilerModule(const void* owner) {
		ProfilerModule module;
		module.title = "C# Scripts";
		module.build = &Build;
		module.refresh = &Refresh;
		module.sample = &Sample;
		if (!ProfilerModules::Register(StringHash("CSharp"), std::move(module), owner)) {
			SLT_LOG_ERROR("C#", "couldn't register the Profiler window's C# Scripts module");
			return false;
		}
		return true;
	}

	void ReleaseProfilerModule() {
		if (_state) {
			Delete(_state);
			_state = nullptr;
		}
		ScriptProfile::Reset();
	}

	void PumpProfilerModule() {
		ScriptProfile::SetEnabled(Profiler::IsEnabled());
	}

} // namespace Shalltear::CSharpEditor
