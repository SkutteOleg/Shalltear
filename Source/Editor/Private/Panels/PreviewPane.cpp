#include "PreviewPane.h"

#include "../PreviewPaneRegistry.h"
#include "../PreviewRig.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Input.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/UI/RenderList.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorInputOps.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorTheme.h"

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color TextStrong() { return EditorTheme::GetColor(ColorToken::TextStrong); }
		f32 HeaderFont() { return EditorTheme::GetMetric(MetricToken::FontRow); }
		f32 DetailFont() { return EditorTheme::GetMetric(MetricToken::FontBody); }
		f32 PaneHeight() { return EditorTheme::GetMetric(MetricToken::PreviewPaneHeight); }
		f32 BadgeSize() { return EditorTheme::GetMetric(MetricToken::PreviewBadgeSize); }

	} // anonymous namespace

	UI::ElementHandle PreviewPane::Build(UI::Context& context, UI::ElementHandle parent) {
		_root = context.CreateElement();
		context.AppendChild(parent, _root);

		_body = {};
		_badge = {};
		_detail = {};
		_paintedTexture = {};
		_imageSize = {};

		UI::Style style;
		style.flexDirection = UI::FlexDirection::Column;
		style.width = UI::Length::Percent(1.0f);
		style.flexGrow = 0.0f;
		style.flexShrink = 0.0f;
		style.display = UI::Display::None;
		context.SetStyle(_root, style);

		return _root;
	}

	void PreviewPane::Rebuild(UI::Context& context, const Guid& guid, bool expanded, std::function<void()> onToggle) {
		if (!context.IsAlive(_root)) {
			return;
		}
		ReleaseLive();
		if (guid != _guid) {
			_notOrbitable = false;
			HideRegistered();
		}
		context.ClearChildren(_root);
		_body = {};
		_badge = {};
		_detail = {};
		_paintedTexture = {};
		_imageSize = {};
		_guid = guid;
		_expanded = expanded;
		_registeredBody = false;
		_headerRefresh = {};
		_bodyRefresh = {};

		Path path = guid.IsValid() ? AssetDatabase::LookupPath(guid) : Path();
		if (!guid.IsValid() || StringUtil::ToLower(path.GetExtension()) == ".cs") {
			UI::Style hidden = context.GetStyle(_root);
			hidden.display = UI::Display::None;
			context.SetStyle(_root, hidden);
			_fileName.clear();
			_type = {};
			_guid = {};
			return;
		}
		_fileName = path.ToString().empty() ? str("Preview") : path.GetFilename();

		_type = EditorPreviewsInternal::AssetTypeForFileName(_fileName);
		if (!_shown) {
			if (const AssetPreviewPaneDesc* pane = EditorPreviewsInternal::FindPane(_type)) {
				_shown = true;
				if (pane->onShow) {
					pane->onShow(_guid);
				}
			}
		}

		UI::Style shown = context.GetStyle(_root);
		shown.display = UI::Display::Flex;
		context.SetStyle(_root, shown);

		UI::ElementHandle header = context.CreateElement();
		context.AppendChild(_root, header);
		context.AddClass(header, EditorTheme::Class::SectionHeader);
		UI::Style headerStyle;
		headerStyle.flexDirection = UI::FlexDirection::Row;
		headerStyle.alignItems = UI::Align::Center;
		headerStyle.padding = {6.0f, 4.0f, 6.0f, 4.0f};
		headerStyle.width = UI::Length::Percent(1.0f);
		headerStyle.cursor = SystemCursor::Hand;
		context.SetStyle(header, headerStyle);
		context.Events(header).onClick = [onToggle]() {
			if (onToggle) {
				onToggle();
			}
		};

		UI::ElementHandle caret = context.CreateElement();
		context.AppendChild(header, caret);
		UI::Style caretStyle;
		caretStyle.width = UI::Length::Pixel(EditorTheme::GetMetric(MetricToken::TreeGutter));
		context.SetStyle(caret, caretStyle);
		context.SetHoverable(caret, false);
		EditorIcons::SetGlyphOrText(context, caret,
									expanded ? EditorIcons::Glyph::CaretDown : EditorIcons::Glyph::CaretRight,
									expanded ? "v" : ">", TextDim(),
									EditorIcons::GutterSizePx(), HeaderFont());

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(header, title);
		context.AddClass(title, EditorTheme::Class::RowText);
		UI::Style titleStyle;
		titleStyle.flexGrow = 1.0f;
		context.SetStyle(title, titleStyle);
		context.SetHoverable(title, false);
		context.SetText(title, UISystem::GetDefaultFont(), HeaderFont(), TextStrong(), _fileName);

		const AssetPreviewPaneDesc* pane = EditorPreviewsInternal::FindPane(_type);
		if (pane && pane->buildHeader) {
			_headerRefresh = pane->buildHeader(context, header, _guid);
		}

		if (!expanded) {
			return;
		}

		_body = context.CreateElement();
		context.AppendChild(_root, _body);
		context.AddClass(_body, EditorTheme::Class::ViewportPlaceholder);
		UI::Style bodyStyle;
		bodyStyle.flexDirection = UI::FlexDirection::Row;
		bodyStyle.justifyContent = UI::Align::Center;
		bodyStyle.alignItems = UI::Align::Center;
		bodyStyle.width = UI::Length::Percent(1.0f);
		bodyStyle.height = UI::Length::Pixel(PaneHeight());
		bodyStyle.flexShrink = 0.0f;
		bodyStyle.overflow = UI::Overflow::Hidden;
		context.SetStyle(_body, bodyStyle);

		if (pane && pane->buildBody) {
			_registeredBody = true;
			_bodyRefresh = pane->buildBody(context, _body, _guid);
			return;
		}

		UI::ElementHandle stage = _viewport.Build(context, _body);
		UI::Style stageStyle = context.GetStyle(stage);
		stageStyle.flexDirection = UI::FlexDirection::Row;
		stageStyle.justifyContent = UI::Align::Center;
		stageStyle.alignItems = UI::Align::Center;
		stageStyle.height = UI::Length::Percent(1.0f);
		context.SetStyle(stage, stageStyle);

		context.Events(stage).onPressDrag = [this, contextPtr = &context](Vec2 pointPx) {
			if (!_dragging) {
				_dragging = true;
				_lastDragPx = pointPx;
				return;
			}
			if (!_live && !GoLive(*contextPtr)) {
				return;
			}
			_orbit = EditorPreviewOps::OrbitAfterDrag(_orbit, pointPx - _lastDragPx);
			_lastDragPx = pointPx;
			_viewDirty = true;
		};
		context.Events(stage).onPressEnd = [this]() { _dragging = false; };

		_viewport.overlay = [this](UI::Context&, UI::PaintScope& paint, const UI::Rect& area) {
			if (_live || _imageSize.x <= 0 || _imageSize.y <= 0) {
				return;
			}
			const Vec2 fitted = EditorPreviewOps::FitImage(_imageSize, {area.w, area.h});
			if (fitted.x <= 0.0f || fitted.y <= 0.0f) {
				return;
			}
			const UI::Rect picture = UI::SnapRectToPixels({area.x + (area.w - fitted.x) * 0.5f,
														   area.y + (area.h - fitted.y) * 0.5f, fitted.x, fitted.y});
			paint.EmitTexturedQuad(picture, _paintedTexture, {0.0f, 0.0f, 1.0f, 1.0f}, Color::White(), _paintedSampler);
		};

		_badge = context.CreateElement();
		context.AppendChild(stage, _badge);
		context.SetHoverable(_badge, false);
		ShowBadge(context);

		_detail = context.CreateElement();
		context.AppendChild(_root, _detail);
		context.AddClass(_detail, EditorTheme::Class::RowText);
		UI::Style detailStyle;
		detailStyle.padding = {6.0f, 3.0f, 6.0f, 3.0f};
		detailStyle.width = UI::Length::Percent(1.0f);
		detailStyle.display = UI::Display::None;
		context.SetStyle(_detail, detailStyle);
		context.SetHoverable(_detail, false);
	}

	void PreviewPane::ShowBadge(UI::Context& context) {
		if (!context.IsAlive(_badge)) {
			return;
		}
		EditorPreviews::TypeBadge badge = EditorPreviews::BadgeForFileName(_fileName);
		EditorIcons::SetGlyphOrText(context, _badge, badge.icon, "?", badge.color, BadgeSize(), BadgeSize());
		UI::Style style = context.GetStyle(_badge);
		style.display = UI::Display::Flex;
		context.SetStyle(_badge, style);
		_paintedTexture = {};
		_imageSize = {};
	}

	void PreviewPane::ShowImage(UI::Context& context, RHI::TextureHandle texture, Vec2I size,
								UI::SamplerKind sampler, const str& detail) {
		if (!context.IsAlive(_badge)) {
			return;
		}
		_paintedTexture = texture;
		_paintedSampler = sampler;
		_imageSize = size;

		UI::Style badgeStyle = context.GetStyle(_badge);
		badgeStyle.display = UI::Display::None;
		context.SetStyle(_badge, badgeStyle);

		if (context.IsAlive(_detail)) {
			UI::Style detailStyle = context.GetStyle(_detail);
			detailStyle.display = detail.empty() ? UI::Display::None : UI::Display::Flex;
			context.SetStyle(_detail, detailStyle);
			if (!detail.empty()) {
				context.SetText(_detail, UISystem::GetDefaultFont(), DetailFont(), TextDim(), detail);
			}
		}
	}

	bool PreviewPane::GoLive(UI::Context& context) {
		if (_notOrbitable || !_guid.IsValid() || !context.IsAlive(_viewport.GetElement())) {
			return false;
		}
		if (_type != StringHash{} && !EditorPreviewsInternal::HasProducer(_type)) {
			_notOrbitable = true;
			return false;
		}
		_viewport.Tick(context, 0.0f);
		if (!_viewport.IsReady()) {
			_viewport.Release();
			return false;
		}

		Asset* asset = AssetDatabase::LoadAssetByGuid(_guid);
		if (!asset) {
			_notOrbitable = true;
			_viewport.Release();
			return false;
		}
		Handle<Scene> stage = SceneSystem::CreateIsolatedScene("PreviewPaneStage");

		AssetPreviewRequest request;
		request.guid = _guid;
		request.asset = asset;
		request.stage = stage;
		request.target = _viewport.GetTarget();
		request.targetSize = _viewport.GetSizePx();
		_liveRow.material = {};
		request.row = &_liveRow;
		AssetPreviewImage image;
		bool produced = EditorPreviews::Produce(request, image);

		if (!produced || !image.subjectBounds.IsValid()) {
			_notOrbitable = true;
			SceneSystem::UnloadScene(stage);
			AssetDatabase::UnloadAsset(asset);
			_viewport.Release();
			return false;
		}

		_liveStage = stage;
		_liveAsset = Handle<Asset>(asset);
		_subjectBounds = image.subjectBounds;
		_orbit = {};
		_live = true;
		_viewDirty = true;
		AssetPreviewEntry entry;
		EditorPreviewStore::Request(_guid, entry);
		_liveRevision = entry.revision;

		if (context.IsAlive(_badge)) {
			UI::Style style = context.GetStyle(_badge);
			style.display = UI::Display::None;
			context.SetStyle(_badge, style);
		}
		return true;
	}

	void PreviewPane::SubmitLive(UI::Context& context) {
		if (!_live) {
			return;
		}
		if (!_viewport.IsReady()) {
			return;
		}
		if (_viewport.GetSizePx() != _liveSizePx) {
			_liveSizePx = _viewport.GetSizePx();
			_viewDirty = true;
		}
		if (!_viewDirty) {
			return;
		}
		_viewDirty = false;
		EditorPreviewsInternal::SubmitPreviewView(_liveStage, _viewport.GetTarget(), _viewport.GetSizePx(), _subjectBounds, _orbit, nullptr,
												  EditorPreviewsInternal::LentRows(&_liveRow).Get());
	}

	void PreviewPane::ReleaseLive() {
		if (_liveStage.IsValid()) {
			SceneSystem::UnloadScene(_liveStage);
			_liveStage = {};
		}
		_liveRow.material = {};
		if (Asset* live = _liveAsset.Get()) {
			AssetDatabase::UnloadAsset(live);
		}
		_liveAsset = {};
		_viewport.Release();
		_live = false;
		_dragging = false;
		_viewDirty = false;
		_liveRevision = 0;
		_liveSizePx = {};
		_subjectBounds = AABB::Empty();
		_orbit = {};
		_paintedTexture = {};
		_imageSize = {};
	}

	void PreviewPane::Tick(UI::Context& context) {
		if (!_guid.IsValid()) {
			return;
		}
		if (_shown && EditorPreviewsInternal::FindPane(_type)) {
			if (_headerRefresh) {
				_headerRefresh(context);
			}
			if (_bodyRefresh) {
				_bodyRefresh(context);
			}
		}
		if (!_expanded || !context.IsAlive(_body) || _registeredBody) {
			return;
		}

		if (_live) {
			_viewport.Tick(context, 1.0f / 60.0f);
			EditorInputOps::RouteFacts facts = EditorInputOps::ApplicationFacts(context);
			facts.pointerInside = _viewport.GetRect(context).Contains(context.GetPointerPosition());
			facts.gestureHeld = _dragging;
			if (EditorInputOps::ShouldOpen(EditorInputOps::Routes::ViewportPointer, facts)) {
				if (f32 wheel = Input::GetMouseWheel().y; wheel != 0.0f) {
					_orbit = EditorPreviewOps::OrbitAfterZoom(_orbit, wheel);
					_viewDirty = true;
				}
			}
			AssetPreviewEntry entry;
			EditorPreviewStore::Request(_guid, entry);
			if (entry.revision != _liveRevision) {
				_liveRevision = entry.revision;
				_viewDirty = true;
			}
			SubmitLive(context);
			return;
		}

		AssetPreviewEntry entry;
		if (EditorPreviewStore::Request(_guid, entry)) {
			if (!(entry.texture == _paintedTexture) || entry.size != _imageSize) {
				ShowImage(context, entry.texture, entry.size, entry.sampler, entry.detail);
			}
		} else if (_imageSize.x > 0) {
			ShowBadge(context);
		}
	}

	void PreviewPane::Release() {
		ReleaseLive();
		HideRegistered();
		_guid = {};
		_type = {};
		_fileName.clear();
		_notOrbitable = false;
		_registeredBody = false;
		_headerRefresh = {};
		_bodyRefresh = {};
		_paintedTexture = {};
		_imageSize = {};
	}

	void PreviewPane::ReleaseForReload(UI::Context& context) {
		if (!_shown) {
			ReleaseLive();
			return;
		}
		HideRegistered();
		ReleaseLive();
		if (context.IsAlive(_root)) {
			context.ClearChildren(_root);
			UI::Style hidden = context.GetStyle(_root);
			hidden.display = UI::Display::None;
			context.SetStyle(_root, hidden);
		}
		_body = {};
		_badge = {};
		_detail = {};
		_guid = {};
		_type = {};
		_registeredBody = false;
		_headerRefresh = {};
		_bodyRefresh = {};
		_paintedTexture = {};
		_imageSize = {};
	}

	void PreviewPane::HideRegistered() {
		if (!_shown) {
			return;
		}
		_shown = false;
		if (const AssetPreviewPaneDesc* pane = EditorPreviewsInternal::FindPane(_type); pane && pane->onHide) {
			pane->onHide(_guid);
		}
	}

} // namespace Shalltear
