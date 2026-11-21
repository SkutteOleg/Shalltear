#include "BenchRunner.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace Shalltear;

namespace {

	std::filesystem::path FindRepoFile(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	FontAsset* LoadEngineFont() {
		std::filesystem::path path = FindRepoFile("EngineAssets/Fonts/Inter/Inter-Regular.ttf");
		if (path.empty()) {
			return nullptr;
		}
		FontAsset* font = New<FontAsset>(MemTag::Asset);
		if (!font->Import(Path(path.generic_string()))) {
			Delete(font);
			return nullptr;
		}
		return font;
	}

	struct PanelTree {
		UI::ElementHandle panel;
		UI::ElementHandle column;
		std::vector<UI::ElementHandle> rows;
		std::vector<UI::ElementHandle> values;
	};

	PanelTree BuildPanel(UI::Context& ctx, UI::ElementHandle parent, Handle<FontAsset> font, u32 rowCount) {
		PanelTree tree;

		tree.panel = ctx.CreateElement();
		ctx.AppendChild(parent, tree.panel);
		UI::Style panelStyle;
		panelStyle.flexDirection = UI::FlexDirection::Column;
		panelStyle.alignItems = UI::Align::Stretch;
		panelStyle.flexGrow = 1.0f;
		panelStyle.height = UI::Length::Percent(1.0f);
		panelStyle.padding = UI::Edges::All(6.0f);
		panelStyle.background = UI::Brush::SolidColor(Color(0.12f, 0.12f, 0.14f, 1.0f));
		ctx.SetStyle(tree.panel, panelStyle);

		tree.column = ctx.CreateElement();
		ctx.AppendChild(tree.panel, tree.column);
		UI::Style columnStyle;
		columnStyle.flexDirection = UI::FlexDirection::Column;
		columnStyle.alignItems = UI::Align::Stretch;
		columnStyle.overflow = UI::Overflow::Scroll;
		columnStyle.flexGrow = 1.0f;
		ctx.SetStyle(tree.column, columnStyle);

		for (u32 i = 0; i < rowCount; ++i) {
			UI::ElementHandle row = ctx.CreateElement();
			ctx.AppendChild(tree.column, row);
			UI::Style rowStyle;
			rowStyle.flexDirection = UI::FlexDirection::Row;
			rowStyle.alignItems = UI::Align::Center;
			rowStyle.height = UI::Length::Pixel(20.0f);
			rowStyle.padding = {4.0f, 2.0f, 4.0f, 2.0f};
			ctx.SetStyle(row, rowStyle);

			UI::ElementHandle label = ctx.CreateElement();
			ctx.AppendChild(row, label);
			UI::Style labelStyle;
			labelStyle.flexGrow = 1.0f;
			ctx.SetStyle(label, labelStyle);
			char text[64];
			std::snprintf(text, sizeof(text), "SceneObject %u (MeshRenderer)", i);
			ctx.SetText(label, font, 12.0f, Color(0.85f, 0.85f, 0.88f, 1.0f), text);

			UI::ElementHandle value = ctx.CreateElement();
			ctx.AppendChild(row, value);
			ctx.SetText(value, font, 12.0f, Color(0.62f, 0.62f, 0.66f, 1.0f), "visible");

			tree.rows.push_back(row);
			tree.values.push_back(value);
		}
		return tree;
	}

	struct EditorTree {
		UI::ElementHandle host;
		std::vector<PanelTree> panels;
		std::vector<UI::ElementHandle> wrappers;
	};

	EditorTree BuildEditorLike(UI::Context& ctx, Handle<FontAsset> font, u32 panelCount, u32 rowsPerPanel) {
		EditorTree tree;
		tree.host = ctx.CreateElement();
		ctx.AppendChild(ctx.GetRoot(), tree.host);
		UI::Style hostStyle;
		hostStyle.flexDirection = UI::FlexDirection::Row;
		hostStyle.alignItems = UI::Align::Stretch;
		hostStyle.width = UI::Length::Percent(1.0f);
		hostStyle.height = UI::Length::Percent(1.0f);
		ctx.SetStyle(tree.host, hostStyle);

		for (u32 i = 0; i < panelCount; ++i) {
			UI::ElementHandle wrapper = ctx.CreateElement();
			ctx.AppendChild(tree.host, wrapper);
			UI::Style wrapperStyle;
			wrapperStyle.flexDirection = UI::FlexDirection::Column;
			wrapperStyle.alignItems = UI::Align::Stretch;
			wrapperStyle.flexGrow = 1.0f;
			wrapperStyle.flexShrink = 1.0f;
			wrapperStyle.minWidth = UI::Length::Pixel(40.0f);
			ctx.SetStyle(wrapper, wrapperStyle);
			tree.wrappers.push_back(wrapper);
			tree.panels.push_back(BuildPanel(ctx, wrapper, font, rowsPerPanel));
		}
		return tree;
	}

	struct FrameCost {
		f64 update = 0.0;
		f64 paint = 0.0;
		f64 Total() const { return update + paint; }
	};

	FrameCost FrameMicroseconds(UI::Context& ctx, UI::RenderList& list) {
		FrameCost cost;
		Stopwatch watch;
		watch.Start();
		ctx.Update();
		watch.Stop();
		cost.update = watch.GetElapsedSeconds() * 1e6;

		watch.Reset();
		watch.Start();
		list.Clear();
		ctx.BuildRenderList(list);
		watch.Stop();
		cost.paint = watch.GetElapsedSeconds() * 1e6;
		return cost;
	}

	f64 Mean(const std::vector<f64>& values, size_t first, size_t last) {
		f64 sum = 0.0;
		for (size_t i = first; i < last && i < values.size(); ++i) {
			sum += values[i];
		}
		size_t count = std::min(last, values.size()) - first;
		return count > 0 ? sum / static_cast<f64>(count) : 0.0;
	}

	void Banner(const char* title) {
		std::printf("\n  ======== %s ========\n", title);
	}

	void E1SteadyState(Handle<FontAsset> font) {
		Banner("E1 steady-state frame (nothing changed) vs tree size");
		std::printf("    TRUE = flat + near 0 | FALSE = scales with elements\n");
		std::printf("    %-30s %12s %12s %10s %10s %8s\n",
					"tree", "update us", "paint us", "quads", "walked", "sorted");
		for (u32 rows : {10u, 50u, 200u, 800u}) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			desc.contentScale = 1.0f;
			UI::Context ctx(desc, UISystem::GetFontSystem());
			BuildEditorLike(ctx, font, 4, rows);
			UI::RenderList list;

			for (int i = 0; i < 8; ++i) {
				FrameMicroseconds(ctx, list);
			}
			std::vector<f64> updates;
			std::vector<f64> paints;
			ctx.ResetPaintStats();
			constexpr int FRAMES = 120;
			for (int i = 0; i < FRAMES; ++i) {
				FrameCost cost = FrameMicroseconds(ctx, list);
				updates.push_back(cost.update);
				paints.push_back(cost.paint);
			}
			const UI::PaintStats& paintStats = ctx.GetPaintStats();
			char label[64];
			std::snprintf(label, sizeof(label), "%4u rows/panel (%5u elems)", rows, ctx.GetLiveCount());
			std::printf("    %-30s %12.1f %12.1f %10u %10llu %8llu\n", label,
						Mean(updates, 0, updates.size()), Mean(paints, 0, paints.size()),
						static_cast<u32>(list.quads.size()),
						static_cast<unsigned long long>(paintStats.elementsVisited / FRAMES),
						static_cast<unsigned long long>(paintStats.scopesSorted / FRAMES));
		}
	}

	void E6LayoutDepth(Handle<FontAsset> font) {
		Banner("E6 full-relayout cost vs NESTING DEPTH at ~constant element count");
		std::printf("    TRUE(multiplicative passes) = us/element explodes with depth | FALSE = flat\n");
		std::printf("    %-10s %10s %14s %14s\n", "depth", "elements", "relayout us", "us/element");
		for (u32 depth : {2u, 4u, 6u, 8u, 10u, 12u}) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			desc.contentScale = 1.0f;
			UI::Context ctx(desc, UISystem::GetFontSystem());

			UI::ElementHandle deepest = ctx.GetRoot();
			for (u32 level = 0; level < depth; ++level) {
				UI::ElementHandle node = ctx.CreateElement();
				ctx.AppendChild(deepest, node);
				UI::Style style;
				style.flexDirection = (level % 2 == 0) ? UI::FlexDirection::Row : UI::FlexDirection::Column;
				style.alignItems = UI::Align::Stretch;
				style.flexGrow = 1.0f;
				style.flexShrink = 1.0f;
				ctx.SetStyle(node, style);
				deepest = node;
			}
			BuildPanel(ctx, deepest, font, 40);

			UI::RenderList list;
			for (int i = 0; i < 8; ++i) {
				FrameMicroseconds(ctx, list);
			}
			std::vector<f64> updates;
			for (int i = 0; i < 12; ++i) {
				UI::Style style = ctx.GetStyle(deepest);
				style.flexGrow = 1.0f + static_cast<f32>(i % 3) * 0.01f;
				ctx.SetStyle(deepest, style);
				updates.push_back(FrameMicroseconds(ctx, list).update);
			}
			u32 elements = ctx.GetLiveCount();
			f64 mean = Mean(updates, 0, updates.size());
			std::printf("    %-10u %10u %14.1f %14.4f\n", depth, elements, mean,
						mean / static_cast<f64>(elements));
		}
	}

	void RunDrag(Handle<FontAsset> font, const char* label, bool distinctWidths) {
		UI::ContextDesc desc;
		desc.sizePx = {2560, 1440};
		desc.contentScale = 1.0f;
		UI::Context ctx(desc, UISystem::GetFontSystem());
		EditorTree tree = BuildEditorLike(ctx, font, 4, 100);
		UI::RenderList list;
		for (int i = 0; i < 8; ++i) {
			FrameMicroseconds(ctx, list);
		}

		constexpr int FRAMES = 2000;
		std::vector<f64> samples;
		samples.reserve(FRAMES);
		for (int frame = 0; frame < FRAMES; ++frame) {
			UI::Style first = ctx.GetStyle(tree.wrappers[0]);
			if (distinctWidths) {
				first.width = UI::Length::Pixel(300.0f + static_cast<f32>(frame));
				first.flexGrow = 0.0f;
			} else {
				first.flexGrow = 0.5f + static_cast<f32>(frame % 200) / 200.0f;
			}
			ctx.SetStyle(tree.wrappers[0], first);
			samples.push_back(FrameMicroseconds(ctx, list).Total());
		}

		f64 early = Mean(samples, 0, 250);
		f64 late = Mean(samples, 1750, 2000);
		std::printf("    %-22s  early %9.1f  mid %9.1f  late %9.1f us   DEGRADATION %6.2fx\n",
					label, early, Mean(samples, 1000, 1250), late, early > 0.0 ? late / early : 0.0);
	}

	void E2SplitterDrag(Handle<FontAsset> font) {
		Banner("E2 splitter drag: per-frame cost and degradation over 2000 frames");
		std::printf("    TRUE(unbounded growth) = DEGRADATION > 1.5x | FALSE = ~1.0x\n");
		RunDrag(font, "oscillating (realistic)", false);
		RunDrag(font, "monotone (new width/frame)", true);
	}

	void E3IdenticalStyleWrite(Handle<FontAsset> font) {
		Banner("E3 rewriting an IDENTICAL style per frame");
		std::printf("    TRUE(no equality guard) = identical ~= changed | FALSE = identical ~= untouched\n");

		auto run = [&](const char* label, int mode) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			desc.contentScale = 1.0f;
			UI::Context ctx(desc, UISystem::GetFontSystem());
			EditorTree tree = BuildEditorLike(ctx, font, 4, 100);
			UI::RenderList list;
			for (int i = 0; i < 8; ++i) {
				FrameMicroseconds(ctx, list);
			}
			std::vector<f64> updates;
			std::vector<f64> paints;
			for (int frame = 0; frame < 300; ++frame) {
				if (mode != 0) {
					UI::Style style = ctx.GetStyle(tree.panels[0].rows[0]);
					if (mode == 2) {
						style.height = UI::Length::Pixel(20.0f + static_cast<f32>(frame % 3));
					}
					ctx.SetStyle(tree.panels[0].rows[0], style);
				}
				FrameCost cost = FrameMicroseconds(ctx, list);
				updates.push_back(cost.update);
				paints.push_back(cost.paint);
			}
			std::printf("    %-28s update %10.1f  paint %10.1f us/frame\n", label,
						Mean(updates, 0, updates.size()), Mean(paints, 0, paints.size()));
		};
		run("untouched (baseline)", 0);
		run("identical style rewritten", 1);
		run("changed style", 2);
	}

	void E4AppendChildScaling(BenchRunner&) {
		Banner("E4 AppendChild cost per child vs sibling count");
		std::printf("    TRUE(O(N^2)) = per-child cost rises with N | FALSE = flat\n");
		for (u32 count : {100u, 400u, 1600u, 6400u}) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			UI::Context ctx(desc, nullptr);
			UI::ElementHandle parent = ctx.CreateElement();
			ctx.AppendChild(ctx.GetRoot(), parent);

			Stopwatch watch;
			watch.Start();
			for (u32 i = 0; i < count; ++i) {
				UI::ElementHandle child = ctx.CreateElement();
				ctx.AppendChild(parent, child);
			}
			watch.Stop();
			f64 microseconds = watch.GetElapsedSeconds() * 1e6;
			std::printf("    %5u children %10.1f us total %10.3f us/child\n",
						count, microseconds, microseconds / static_cast<f64>(count));
		}
	}

	void E5MouseMoveFlood(Handle<FontAsset> font) {
		Banner("E5 frame cost vs mouse-move events delivered per frame");
		std::printf("    TRUE(no coalescing) = cost scales with event count | FALSE = flat\n");
		for (u32 events : {1u, 4u, 8u, 16u}) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			desc.contentScale = 1.0f;
			UI::Context ctx(desc, UISystem::GetFontSystem());
			BuildEditorLike(ctx, font, 4, 100);
			UI::RenderList list;
			for (int i = 0; i < 8; ++i) {
				FrameMicroseconds(ctx, list);
			}

			std::vector<f64> updates;
			for (int frame = 0; frame < 200; ++frame) {
				for (u32 e = 0; e < events; ++e) {
					InputEvent move;
					move.type = InputEventType::MouseMove;
					move.mousePosPx = {static_cast<i32>(200 + (frame * 7 + e * 3) % 1800),
									   static_cast<i32>(100 + (frame * 3 + e) % 900)};
					ctx.InjectInput(move);
				}
				updates.push_back(FrameMicroseconds(ctx, list).update);
			}
			std::printf("    %3u moves/frame  update %10.1f us/frame\n", events,
						Mean(updates, 0, updates.size()));
		}
	}

	struct InspectorTree {
		UI::ElementHandle panel;
		std::vector<UI::Widgets::NumberFieldParts> fields;
		std::vector<UI::ElementHandle> labels;
	};

	InspectorTree BuildInspectorLike(UI::Context& ctx, UI::ElementHandle parent,
									 Handle<FontAsset> font, u32 rowCount) {
		InspectorTree tree;
		tree.panel = ctx.CreateElement();
		ctx.AppendChild(parent, tree.panel);
		UI::Style panelStyle;
		panelStyle.flexDirection = UI::FlexDirection::Column;
		panelStyle.alignItems = UI::Align::Stretch;
		panelStyle.flexGrow = 1.0f;
		panelStyle.height = UI::Length::Percent(1.0f);
		ctx.SetStyle(tree.panel, panelStyle);

		for (u32 i = 0; i < rowCount; ++i) {
			UI::ElementHandle row = ctx.CreateElement();
			ctx.AppendChild(tree.panel, row);
			UI::Style rowStyle;
			rowStyle.flexDirection = UI::FlexDirection::Row;
			rowStyle.alignItems = UI::Align::Center;
			rowStyle.padding = {4.0f, 2.0f, 4.0f, 2.0f};
			ctx.SetStyle(row, rowStyle);

			UI::ElementHandle caption = ctx.CreateElement();
			ctx.AppendChild(row, caption);
			UI::Style captionStyle;
			captionStyle.width = UI::Length::Pixel(90.0f);
			ctx.SetStyle(caption, captionStyle);
			ctx.SetText(caption, font, 12.0f, Color(0.85f, 0.85f, 0.88f, 1.0f), "Position X");

			UI::Widgets::NumberFieldDesc desc;
			desc.value = 0.0;
			desc.decimals = 3;
			desc.font = font;
			desc.fontSizePx = 12.0f;
			tree.fields.push_back(UI::Widgets::NumberField(ctx, row, desc));

			UI::ElementHandle trailing = ctx.CreateElement();
			ctx.AppendChild(row, trailing);
			ctx.SetText(trailing, font, 12.0f, Color(0.62f, 0.62f, 0.66f, 1.0f), "0.000");
			tree.labels.push_back(trailing);
		}
		return tree;
	}

	void E7OneValueChanged(Handle<FontAsset> font) {
		Banner("E7 cost of refreshing K bound values, vs tree size (the Inspector case)");
		std::printf("    TRUE(invalidation not proportional) = cost tracks TREE SIZE, flat in K | FALSE = tracks K\n");
		std::printf("    'grown' writes real NumberFields (container sized, the Inspector)\n");
		std::printf("    'auto' writes content sized labels with moving sizes (the control)\n");
		std::printf("    %-34s %4s %11s %11s %11s %11s\n",
					"tree / what changed", "K", "update us", "paint us", "re-measured", "re-arranged");

		auto run = [&](u32 rows, u32 changed, bool grown) {
			UI::ContextDesc desc;
			desc.sizePx = {2560, 1440};
			desc.contentScale = 1.0f;
			UI::Context ctx(desc, UISystem::GetFontSystem());

			EditorTree host = BuildEditorLike(ctx, font, 3, rows);
			InspectorTree inspector = BuildInspectorLike(ctx, host.host, font, rows);

			UI::RenderList list;
			for (int i = 0; i < 8; ++i) {
				FrameMicroseconds(ctx, list);
			}

			constexpr int FRAMES = 60;
			std::vector<f64> updates;
			std::vector<f64> paints;
			ctx.ResetLayoutStats();
			for (int frame = 0; frame < FRAMES; ++frame) {
				for (u32 i = 0; i < changed; ++i) {
					f64 value = 0.017 * static_cast<f64>(frame) + static_cast<f64>(i);
					if (grown && i < inspector.fields.size()) {
						UI::Widgets::SetNumberFieldValue(ctx, inspector.fields[i], value, 3);
					} else if (!grown && i < inspector.labels.size()) {
						char text[32];
						std::snprintf(text, sizeof(text), "%.3f", value);
						ctx.SetTextString(inspector.labels[i], text);
					}
				}
				FrameCost cost = FrameMicroseconds(ctx, list);
				updates.push_back(cost.update);
				paints.push_back(cost.paint);
			}
			const UI::LayoutStats& stats = ctx.GetLayoutStats();
			f64 frames = static_cast<f64>(FRAMES);

			char label[64];
			std::snprintf(label, sizeof(label), "%4u rows (%5u elems) %s", rows, ctx.GetLiveCount(),
						  grown ? "grown" : "auto ");
			std::printf("    %-34s %4u %11.1f %11.1f %11.1f %11.1f\n", label, changed,
						Mean(updates, 0, updates.size()), Mean(paints, 0, paints.size()),
						static_cast<f64>(stats.measureWork) / frames,
						static_cast<f64>(stats.arrangeWork) / frames);
		};

		for (u32 rows : {25u, 100u, 400u}) {
			run(rows, 1, true);
			run(rows, 9, true);
		}
		run(100, 1, false);
		run(100, 9, false);
	}

} // anonymous namespace

void RunUIFrameBenchmarks(BenchRunner& runner) {
	std::printf("\n================ UI FRAME BENCHMARKS =================\n");

	UISystem::Initialize();
	FontAsset* font = LoadEngineFont();
	if (!font) {
		std::printf("  engine font not found. UI frame benchmarks skipped\n");
		UISystem::Terminate();
		return;
	}
	Handle<FontAsset> fontHandle(font);

	E1SteadyState(fontHandle);
	E7OneValueChanged(fontHandle);
	E6LayoutDepth(fontHandle);
	E2SplitterDrag(fontHandle);
	E3IdenticalStyleWrite(fontHandle);
	E4AppendChildScaling(runner);
	E5MouseMoveFlood(fontHandle);

	Delete(font);
	UISystem::Terminate();
	std::printf("======================================================\n");
}
