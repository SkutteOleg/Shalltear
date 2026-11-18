#include "SceneViewPanel.h"

#include "../EditorConfig.h"
#include "EditorGrid.h"
#include "PanelHeader.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Input.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Variant.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/RenderList.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/VectorAsset.h"
#include "Shalltear/Core/UI/VectorIcon.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorGizmoOps.h"
#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EditorTransformGesture.h"
#include "Shalltear/Editor/EditorViewportOps.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/SceneViewEffects.h"
#include "Shalltear/Editor/SceneViewOverlays.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Shalltear {

	namespace Ops = EditorViewportOps;
	namespace Gizmo = EditorGizmoOps;
	using GizmoHandle = EditorGizmoOps::GizmoHandle;

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		Color TextNormal() { return EditorTheme::GetColor(ColorToken::Text); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color ViewClear() { return EditorTheme::GetColor(ColorToken::ViewportClear); }
		Color32 SelectionOutline() { return EditorTheme::GetColor32(ColorToken::SelectionOutline); }
		Color32 SelectionChildOutline() { return EditorTheme::GetColor32(ColorToken::SelectionChildOutline); }
		Color32 VertexSnapMark() { return EditorTheme::GetColor32(ColorToken::GizmoVertexSnap); }

		constexpr f32 STATUS_REFRESH_SECONDS = 0.2f;
		constexpr f32 MIN_PIVOT_DISTANCE = 0.1f;

		f32 ChipTextPx() { return EditorTheme::GetMetric(MetricToken::FontBody); }

		Handle<Scene> DropTargetScene() {
			Handle<Scene> stage = EditorSceneOps::GetPrefabStage();
			return stage.IsValid() ? stage : SceneSystem::GetPrimaryScene();
		}

		Guid DroppedPrefab(const UI::DragPayload& payload, Handle<Scene> scene) {
			if (!EditorSceneOps::AcceptsPrefabInstances(scene)) {
				return {};
			}
			if (payload.type != "Asset"_hash || !payload.data.Is<AssetDragData>()) {
				return {};
			}
			const AssetDragData& data = payload.data.As<AssetDragData>();
			return StringHash{data.assetTypeHash} == StringHash("PrefabAsset") ? data.guid : Guid{};
		}

		void PinSize(UI::Context& context, UI::ElementHandle element) {
			if (!context.IsAlive(element)) {
				return;
			}
			UI::Style style = context.GetStyle(element);
			style.flexShrink = 0.0f;
			context.SetStyle(element, style);
		}

		Color32 WireColorFor(ViewFill fill) {
			return EditorTheme::GetColor32(fill == ViewFill::SolidAndWireframe
											   ? ColorToken::SceneWireframeOverlay
											   : ColorToken::SceneWireframe);
		}

	} // anonymous namespace

	void SceneViewPanel::OnBuildContent(UI::Context& context, UI::ElementHandle root) {
		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Stretch;
		rootStyle.width = UI::Length::Percent(1.0f);
		rootStyle.height = UI::Length::Percent(1.0f);
		context.SetStyle(root, rootStyle);

		BuildToolbar(context, root);
		_viewport.Build(context, root);
		_viewport.overlay = [this](UI::Context&, UI::PaintScope& paint, const UI::Rect& area) {
			PaintIcons(paint, area);
		};
		BuildOrientationOverlay(context);
		_componentOverlays = context.CreateElement();
		context.AppendChild(_viewport.GetElement(), _componentOverlays);
		context.AddClass(_componentOverlays, EditorTheme::Class::SceneOverlay);
		_componentOverlaysKey = 0;
		SceneViewOverlays::Release(this);

		_vertexSnapAction = _viewportKeys.RegisterAction("SceneView.VertexSnap"_hash, ActionType::Button);
		InputBinding vertexSnap;
		vertexSnap.source = InputBinding::Source::Key;
		vertexSnap.code = static_cast<u16>(Key::V);
		_viewportKeys.Bind(_vertexSnapAction, vertexSnap);

		UI::ElementHandle viewport = _viewport.GetElement();
		UI::Context* contextPtr = &context;
		UI::ElementEvents& events = context.Events(viewport);
		events.onMouseDown = [this, contextPtr]() { OnViewportPress(*contextPtr); };
		events.onPressDrag = [this, contextPtr](Vec2 point) { OnViewportDrag(*contextPtr, point); };
		events.onPressEnd = [this]() { OnViewportRelease(); };
		events.onDragOver = [](const UI::DragPayload& payload) {
			return ComponentDrops::Accepts(payload) || DroppedPrefab(payload, DropTargetScene()).IsValid() ||
				   SceneAssetDrops::Accepts(payload, DropTargetScene());
		};
		events.onDrop = [this, contextPtr](const UI::DragPayload& payload, Vec2 point) {
			OnViewportDrop(*contextPtr, payload, point);
		};
		events.onKeyDown = [this](Key key, KeyMod mods) {
			if (key == Key::F && mods == KeyMod::Shift) {
				Follow(Ops::ResolveSceneObject(EditorSelection::GetActive()));
				return true;
			}
			if (mods != KeyMod::None) {
				return false;
			}
			if (key == Key::F) {
				FrameSelection();
				return true;
			}
			if (key == Key::W || key == Key::E || key == Key::R) {
				gizmoMode = static_cast<i32>(key == Key::W	 ? GizmoMode::Translate
											 : key == Key::E ? GizmoMode::Rotate
															 : GizmoMode::Scale);
				_toolbarDirty = true;
				return true;
			}
			if (key == Key::X) {
				gizmoSpace = static_cast<i32>(static_cast<GizmoSpace>(gizmoSpace) == GizmoSpace::Local
												  ? GizmoSpace::World
												  : GizmoSpace::Local);
				_toolbarDirty = true;
				return true;
			}
			return false;
		};
	}

	void SceneViewPanel::BuildToolbar(UI::Context& context, UI::ElementHandle root) {
		_toolbar = PanelHeader::BuildStrip(context, root);
		FillToolbar(context);
	}

	void SceneViewPanel::FillToolbar(UI::Context& context) {
		if (!context.IsAlive(_toolbar)) {
			return;
		}
		PanelHeader::ClearStrip(context, _toolbar);
		Handle<FontAsset> font = UISystem::IsInitialized() ? UISystem::GetDefaultFont() : Handle<FontAsset>{};
		UI::ElementHandle bar = _toolbar;

		auto toolButton = [&](EditorIcons::Icon icon, const char* word, const char* tooltip,
							  bool checked, std::function<void()> onClick) {
			PanelHeader::ToolButtonDesc desc;
			desc.icon = icon;
			desc.fallback = word;
			desc.tooltip = tooltip;
			desc.checked = checked;
			desc.onClick = std::move(onClick);
			PinSize(context, PanelHeader::ToolButton(context, bar, desc).button);
		};

		GizmoMode mode = static_cast<GizmoMode>(gizmoMode);
		auto modeButton = [&](EditorIcons::Icon icon, const char* word, const char* tooltip, GizmoMode target) {
			toolButton(icon, word, tooltip, mode == target, [this, target]() {
				gizmoMode = static_cast<i32>(target);
				_toolbarDirty = true;
			});
		};
		modeButton(EditorIcons::Glyph::ToolMove, "Move", "Move (W)", GizmoMode::Translate);
		modeButton(EditorIcons::Glyph::ToolRotate, "Rotate", "Rotate (E)", GizmoMode::Rotate);
		modeButton(EditorIcons::Glyph::ToolScale, "Scale", "Scale (R)", GizmoMode::Scale);

		PinSize(context, UI::Widgets::Separator(context, bar));
		bool world = static_cast<GizmoSpace>(gizmoSpace) == GizmoSpace::World;
		bool frameIsWorld = world && mode != GizmoMode::Scale;
		toolButton(frameIsWorld ? EditorIcons::Glyph::SpaceWorld : EditorIcons::Glyph::SpaceLocal,
				   frameIsWorld ? "World" : "Local",
				   mode == GizmoMode::Scale ? "Handle frame: Local, scale has no world frame (X)"
				   : world					? "Handle frame: World (X)"
											: "Handle frame: Local (X)",
				   false, [this, world]() {
					   gizmoSpace = static_cast<i32>(world ? GizmoSpace::Local : GizmoSpace::World);
					   _toolbarDirty = true;
				   });

		PinSize(context, UI::Widgets::Separator(context, bar));
		toolButton(EditorIcons::Icon{}, "2D",
				   mode2D ? "Leave 2D: back to the 3D view you came from" : "2D: look at the XY plane + pan (not orbit)",
				   mode2D, [this]() { SetMode2D(!mode2D); });

		PinSize(context, UI::Widgets::Separator(context, bar));
		toolButton(EditorIcons::Glyph::Grid, "Grid", showGrid ? "Hide the grid" : "Show the grid",
				   showGrid, [this]() {
					   showGrid = !showGrid;
					   _toolbarDirty = true;
				   });

		toolButton(EditorIcons::Glyph::Gizmos, "Gizmos",
				   showGizmos ? "Hide component gizmos and icons" : "Show component gizmos and icons",
				   showGizmos, [this]() {
					   showGizmos = !showGizmos;
					   _toolbarDirty = true;
				   });
		UI::Context* gizmosContext = &context;
		toolButton(EditorIcons::Glyph::CaretDown, "v", "Gizmo options", false,
				   [this, gizmosContext]() { OpenGizmosMenu(*gizmosContext); });

		PinSize(context, UI::Widgets::Separator(context, bar));
		toolButton(EditorIcons::Glyph::Effects, "Effects",
				   showEffects ? "Hide the view's effects" : "Show the view's effects",
				   showEffects, [this]() {
					   showEffects = !showEffects;
					   _toolbarDirty = true;
				   });
		UI::Context* contextPtr = &context;
		toolButton(EditorIcons::Glyph::CaretDown, "v", "Choose which effects the view shows", false,
				   [this, contextPtr]() { OpenEffectsMenu(*contextPtr); });

		PinSize(context, UI::Widgets::Separator(context, bar));
		Vector<std::string_view> shadingLabels{GetMemoryResource(MemTag::Editor)};
		for (u32 index = 0; index < Ops::GetShadingModeCount(); ++index) {
			shadingLabels.push_back(Ops::GetShadingMode(static_cast<i32>(index)).label);
		}
		UI::Widgets::DropdownDesc shading;
		shading.items = {shadingLabels.data(), shadingLabels.size()};
		shading.selected = static_cast<u32>(ClampedShadingMode());
		shading.font = font;
		shading.fontSizePx = EditorTheme::GetMetric(MetricToken::FontSmall);
		shading.onSelect = [this](u32 index) { shadingMode = static_cast<i32>(index); };
		PinSize(context, UI::Widgets::Dropdown(context, bar, shading).root);

		static constexpr std::string_view CascadeFitRows[] = {
			"Cascade Fit: Off", "Cascade Fit: All",
			"Cascade Fit: 0", "Cascade Fit: 1", "Cascade Fit: 2", "Cascade Fit: 3"};
		static_assert(std::size(CascadeFitRows) == MAX_SHADOW_CASCADES + 2, "off, all, 1 row per cascade");
		UI::Widgets::DropdownDesc cascadeFit;
		cascadeFit.items = {CascadeFitRows, std::size(CascadeFitRows)};
		cascadeFit.selected = CascadeFitRow();
		cascadeFit.font = font;
		cascadeFit.fontSizePx = EditorTheme::GetMetric(MetricToken::FontSmall);
		cascadeFit.onSelect = [this](u32 index) {
			showCascadeFrustum = index > 0;
			cascadeFrustumIndex = index <= 1 ? -1 : static_cast<i32>(index) - 2;
		};
		PinSize(context, UI::Widgets::Dropdown(context, bar, cascadeFit).root);

		static constexpr std::string_view AtlasOverlayRows[] = {
			"Atlas: Off", "Atlas: Directional", "Atlas: Punctual"};
		static_assert(std::size(AtlasOverlayRows) == static_cast<size_t>(DebugOverlay::PunctualAtlas) + 1,
					  "1 row per DebugOverlay value, in its order");
		UI::Widgets::DropdownDesc atlas;
		atlas.items = {AtlasOverlayRows, std::size(AtlasOverlayRows)};
		atlas.selected = static_cast<u32>(ClampedAtlasOverlay());
		atlas.font = font;
		atlas.fontSizePx = EditorTheme::GetMetric(MetricToken::FontSmall);
		atlas.onSelect = [this](u32 index) { atlasOverlay = static_cast<i32>(index); };
		PinSize(context, UI::Widgets::Dropdown(context, bar, atlas).root);

		toolButton(_freezeShadowFit ? EditorIcons::Glyph::Lock : EditorIcons::Glyph::Unlock, "Freeze",
				   _freezeShadowFit ? "Shadow fit: pinned where you froze it. Unfreeze to follow the camera again"
									: "Shadow fit: following the camera. Freeze it to fly out and look at it",
				   _freezeShadowFit, [this]() {
					   _freezeShadowFit = !_freezeShadowFit;
					   if (_freezeShadowFit) {
						   _frozenFit = CurrentViewpoint();
					   }
					   _toolbarDirty = true;
				   });

		UI::Widgets::Spacer(context, bar);

		_statusLabel = context.CreateElement();
		context.AppendChild(bar, _statusLabel);
		context.SetText(_statusLabel, font, EditorTheme::GetMetric(MetricToken::FontSmall), TextDim(), "");
		context.AddClass(_statusLabel, EditorTheme::Class::RowText);
		RefreshToolbar(context);
	}

	void SceneViewPanel::RefreshToolbar(UI::Context& context) {
		if (context.IsAlive(_statusLabel)) {
			context.SetTextString(_statusLabel,
								  std::format("cam {:.1f}, {:.1f}, {:.1f}   speed {:.1f}",
											  cameraPosition.x, cameraPosition.y, cameraPosition.z, flySpeed));
		}
	}

	void SceneViewPanel::OnOpen() {
		Handle<SceneViewPanel> self(this);
		EditorEvents::FollowRequested.Subscribe(EventBus::GetGlobal(), self, &SceneViewPanel::OnFollowRequested);
		EditorEvents::WorldRemapped.Subscribe(EventBus::GetGlobal(), self, &SceneViewPanel::OnWorldRemapped);
	}

	void SceneViewPanel::OnClose() {
		EventBus::GetGlobal().UnsubscribeAll(GetInstanceId());
		OnViewportRelease();
		_viewport.Release();
		SceneViewOverlays::Release(this);
	}

	i32 SceneViewPanel::ClampedShadingMode() const {
		return shadingMode < 0 || shadingMode >= static_cast<i32>(Ops::GetShadingModeCount()) ? 0 : shadingMode;
	}

	DebugOverlay SceneViewPanel::ClampedAtlasOverlay() const {
		if (atlasOverlay < 0 || atlasOverlay > static_cast<i32>(DebugOverlay::PunctualAtlas)) {
			return DebugOverlay::None;
		}
		return static_cast<DebugOverlay>(atlasOverlay);
	}

	u32 SceneViewPanel::CascadeFitRow() const {
		if (!showCascadeFrustum) {
			return 0;
		}
		if (cascadeFrustumIndex < 0 || cascadeFrustumIndex >= static_cast<i32>(MAX_SHADOW_CASCADES)) {
			return 1;
		}
		return static_cast<u32>(cascadeFrustumIndex) + 2u;
	}

	ViewFill SceneViewPanel::GetShadingFill() const { return Ops::GetShadingMode(shadingMode).fill; }

	DebugView SceneViewPanel::GetDebugView() const { return Ops::GetShadingMode(shadingMode).debugView; }

	void SceneViewPanel::UpdateCamera(UI::Context& context, f32 deltaSeconds) {
		bool pointerInside = _pointerRouteOpen;

		if (!_looking) {
			if (pointerInside && Input::WasMouseButtonPressed(MouseButton::Right)) {
				_looking = true;
				_pointerClaim = context.ClaimPointer(_viewport.GetElement(), CursorMode::Wrapped);
				context.ClearFocus();
			}
		} else if (!Input::IsMouseButtonDown(MouseButton::Right)) {
			_looking = false;
			context.ReleasePointer(_pointerClaim);
			_pointerClaim = 0;
			context.SetFocus(_viewport.GetElement());
		}

		bool flew = false;
		if (_looking) {
			Vec2 delta = Input::GetMouseDelta();
			if (RotationIsLocked()) {
				const f32 heightPx = Math::Max(1.0f, _viewport.GetRect(context).h);
				const Ops::Viewpoint viewpoint = CurrentViewpoint();
				const f32 unitsPerPx = Ops::ViewHeightAt(viewpoint, PivotPoint()) / heightPx;
				cameraPosition -= (viewpoint.rotation * Vec3::Right()) * (delta.x * unitsPerPx);
				cameraPosition += (viewpoint.rotation * Vec3::Up()) * (delta.y * unitsPerPx);
				flew = flew || delta.x != 0.0f || delta.y != 0.0f;
			} else {
				const Vec3 pivot = PivotPoint();
				cameraYawDeg += delta.x * 0.15f;
				cameraPitchDeg = Math::Clamp(cameraPitchDeg + delta.y * 0.15f, -89.0f, 89.0f);
				if (orthographic) {
					cameraPosition = pivot - (Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg) * Vec3::Forward()) *
												 _pivotDistance;
				}
			}

			f32 wheel = Input::GetMouseWheel().y;
			if (wheel != 0.0f) {
				if (orthographic) {
					ZoomOrthographic(wheel);
				} else {
					flySpeed = Math::Clamp(flySpeed * std::pow(1.15f, wheel), 0.05f, 500.0f);
				}
			}

			if (!orthographic) {
				Quat rotation = Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg);
				Vec3 forward = rotation * Vec3::Forward();
				Vec3 right = rotation * Vec3::Right();
				Vec3 move{};
				if (Input::IsKeyDown(Key::W)) {
					move += forward;
				}
				if (Input::IsKeyDown(Key::S)) {
					move -= forward;
				}
				if (Input::IsKeyDown(Key::D)) {
					move += right;
				}
				if (Input::IsKeyDown(Key::A)) {
					move -= right;
				}
				if (Input::IsKeyDown(Key::E)) {
					move += Vec3::Up();
				}
				if (Input::IsKeyDown(Key::Q)) {
					move -= Vec3::Up();
				}
				if (move.LengthSquared() > 0.0f) {
					f32 speed = flySpeed * (Input::IsKeyDown(Key::LeftShift) ? 3.0f : 1.0f);
					cameraPosition += move.Normalized() * speed * deltaSeconds;
					flew = true;
				}
			}
		} else if (pointerInside && _drag.handle == GizmoHandle::None) {
			f32 wheel = Input::GetMouseWheel().y;
			if (wheel != 0.0f && orthographic) {
				ZoomOrthographic(wheel);
			} else if (wheel != 0.0f) {
				const f32 step = wheel * flySpeed * 0.25f;
				if (_followTargetId != 0) {
					_followDistance = Math::Max(_followDistance - step, MIN_PIVOT_DISTANCE);
				} else {
					Quat rotation = Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg);
					cameraPosition += (rotation * Vec3::Forward()) * step;
					_pivotDistance = Math::Max(_pivotDistance - step, MIN_PIVOT_DISTANCE);
				}
			}
		}

		if (_followTargetId != 0) {
			Vec3 position;
			if (flew) {
				StopFollowing();
			} else if (Ops::FollowPositionFor(FollowTarget(), Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg),
											  _followDistance, position)) {
				cameraPosition = position;
				_pivotDistance = _followDistance;
			} else {
				StopFollowing();
			}
		}
	}

	void SceneViewPanel::FrameSelection() {
		AABB bounds = Ops::GetSelectionWorldBounds();
		if (!bounds.IsValid()) {
			return;
		}
		StopFollowing();
		cameraPosition = Ops::FramePositionFor(bounds, Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg));
		_pivotDistance = Ops::FrameDistanceFor(bounds);
		orthographicSize = Ops::FrameSizeFor(bounds);
	}

	Handle<SceneObject> SceneViewPanel::FollowTarget() const {
		Object* raw = _followTargetId != 0 ? ObjectRegistry::Resolve(_followTargetId) : nullptr;
		if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
			return {};
		}
		return Handle<SceneObject>(static_cast<SceneObject*>(raw));
	}

	void SceneViewPanel::Follow(Handle<SceneObject> object) {
		AABB bounds = Ops::GetObjectWorldBounds(object);
		if (!bounds.IsValid()) {
			return;
		}
		_followTargetId = object.GetInstanceId();
		_followDistance = Ops::FrameDistanceFor(bounds);
		_pivotDistance = _followDistance;
		cameraPosition = Ops::FramePositionFor(bounds, Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg));
		orthographicSize = Ops::FrameSizeFor(bounds);
	}

	void SceneViewPanel::StopFollowing() {
		_followTargetId = 0;
		_followDistance = 0.0f;
	}

	void SceneViewPanel::OnFollowRequested(const EditorFollowRequestedEvent& event) {
		Object* raw = event.object != 0 ? ObjectRegistry::Resolve(event.object) : nullptr;
		if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
			Follow(Handle<SceneObject>(static_cast<SceneObject*>(raw)));
		}
	}

	void SceneViewPanel::OnWorldRemapped(const EditorWorldRemappedEvent& event) {
		if (_followTargetId == 0) {
			return;
		}
		for (const EditorIdRemap& row : event.ids) {
			if (row.oldId == _followTargetId) {
				_followTargetId = row.newId;
				return;
			}
		}
		StopFollowing();
	}

	void SceneViewPanel::RefreshFollowChip(UI::Context& context) {
		SceneObject* target = FollowTarget().Get();
		const str name = target ? target->name : str();
		const bool chipStands = context.IsAlive(_followChip);
		if (_followChipFor == _followTargetId && _followChipName == name && chipStands == (target != nullptr)) {
			return;
		}
		if (context.IsAlive(_followChip)) {
			context.DestroyElement(_followChip);
		}
		_followChip = {};
		_followChipFor = _followTargetId;
		_followChipName = name;
		if (!target) {
			return;
		}
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		_followChip = context.CreateElement();
		context.AppendChild(_viewport.GetElement(), _followChip);
		context.AddClass(_followChip, EditorTheme::Class::SceneFollow);
		context.AddClass(_followChip, EditorTheme::Class::Clickable);
		UI::Style chipStyle;
		chipStyle.flexDirection = UI::FlexDirection::Row;
		chipStyle.alignItems = UI::Align::Center;
		context.SetStyle(_followChip, chipStyle);
		context.SetTooltip(_followChip, "Following " + name + ". Click to stop (or fly the camera away)");
		context.Events(_followChip).onClick = [this]() { StopFollowing(); };
		EditorIcons::Add(context, _followChip, EditorIcons::Glyph::Lock, TextNormal(),
						 EditorIcons::BodySizePx(), EditorIcons::GutterGapPx(), ChipTextPx());
		UI::ElementHandle label = context.CreateElement();
		context.AppendChild(_followChip, label);
		context.SetHoverable(label, false);
		context.SetText(label, font, ChipTextPx(), TextNormal(), name);
	}

	void SceneViewPanel::UpdateComponentOverlays(UI::Context& context) {
		if (!context.IsAlive(_componentOverlays)) {
			return;
		}
		const std::span<const Handle<Object>> selection = EditorSelection::GetSelection();
		const u64 key = SceneViewOverlays::KeyFor(selection);
		if (key != _componentOverlaysKey) {
			_componentOverlaysKey = key;
			context.ClearChildren(_componentOverlays);
			const u32 cards = SceneViewOverlays::Build(context, _componentOverlays, selection, this);
			UI::Style style = context.GetStyle(_componentOverlays);
			style.display = cards > 0 ? UI::Display::Flex : UI::Display::None;
			context.SetStyle(_componentOverlays, style);
		}
		SceneViewOverlays::Refresh(context, this);
	}

	void SceneViewPanel::BuildOrientationOverlay(UI::Context& context) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		_orientation = context.CreateElement();
		context.AppendChild(_viewport.GetElement(), _orientation);
		context.AddClass(_orientation, EditorTheme::Class::SceneOrientation);
		RefreshOrientationVisibility(context);

		_orientationCanvas = context.CreateElement();
		context.AppendChild(_orientation, _orientationCanvas);
		context.AddClass(_orientationCanvas, EditorTheme::Class::SceneOrientationCube);
		const f32 side = EditorTheme::GetMetric(MetricToken::MarkOrientation);
		UI::Style canvasStyle;
		canvasStyle.width = UI::Length::Pixel(side);
		canvasStyle.height = UI::Length::Pixel(side);
		canvasStyle.flexShrink = 0.0f;
		context.SetStyle(_orientationCanvas, canvasStyle);
		UI::Context* contextPtr = &context;
		UI::ElementEvents& events = context.Events(_orientationCanvas);
		events.customDraw = [this](UI::Context& c, UI::ElementHandle element, UI::PaintScope& paint) {
			PaintOrientation(c, element, paint);
		};
		events.onClick = [this, contextPtr]() { OnOrientationClick(*contextPtr); };
		events.onRightClick = [this, contextPtr]() { OpenOrientationMenu(*contextPtr); };
		context.SetTooltip(_orientationCanvas,
						   "Click an axis to look along it, right-click for the views, Shift+click to return to the default angle");

		static constexpr std::string_view Letters[3] = {"X", "Y", "Z"};
		for (u32 i = 0; i < 3; ++i) {
			UI::ElementHandle label = context.CreateElement();
			context.AppendChild(_orientationCanvas, label);
			UI::Style labelStyle;
			labelStyle.position = UI::Position::Absolute;
			labelStyle.left = UI::Length::Pixel(0.0f);
			labelStyle.top = UI::Length::Pixel(0.0f);
			labelStyle.pointerEvents = false;
			context.SetStyle(label, labelStyle);
			context.SetHoverable(label, false);
			context.SetText(label, font, EditorTheme::GetMetric(MetricToken::FontTiny),
							EditorTheme::GetColor(ColorToken::TextStrong), Letters[i]);
			_orientationLabels[i] = label;
		}

		_orientationBar = context.CreateElement();
		context.AppendChild(_orientation, _orientationBar);
		UI::Style barStyle;
		barStyle.flexDirection = UI::FlexDirection::Row;
		barStyle.alignItems = UI::Align::Center;
		context.SetStyle(_orientationBar, barStyle);
		FillOrientationBar(context);
	}

	void SceneViewPanel::RefreshOrientationVisibility(UI::Context& context) {
		if (!context.IsAlive(_orientation)) {
			return;
		}
		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Center;
		rootStyle.display = mode2D ? UI::Display::None : UI::Display::Flex;
		context.SetStyle(_orientation, rootStyle);
	}

	void SceneViewPanel::FillOrientationBar(UI::Context& context) {
		context.ClearChildren(_orientationBar);
		PanelHeader::ToolButtonDesc lock;
		lock.icon = _rotationLocked ? EditorIcons::Glyph::Lock : EditorIcons::Glyph::Unlock;
		lock.fallback = _rotationLocked ? "Unlock" : "Lock";
		lock.tooltip = _rotationLocked ? "Rotation is locked (a right-drag pans the view). Click to rotate again"
									   : "Lock the view's rotation (a right-drag pans)";
		lock.checked = _rotationLocked;
		lock.onClick = [this]() {
			_rotationLocked = !_rotationLocked;
			_orientationBarDirty = true;
		};
		PanelHeader::ToolButton(context, _orientationBar, lock);
		UI::ElementHandle projection = context.CreateElement();
		context.AppendChild(_orientationBar, projection);
		context.AddClass(projection, EditorTheme::Class::Clickable);
		context.SetText(projection, UISystem::GetDefaultFont(), EditorTheme::GetMetric(MetricToken::FontTiny),
						TextDim(), orthographic ? "Iso" : "Persp");
		context.SetTooltip(projection, orthographic ? "Orthographic. Click for a perspective view"
													: "Perspective. Click for an orthographic view");
		context.Events(projection).onClick = [this]() { ToggleProjection(); };
	}

	void SceneViewPanel::LayOutOrientation(UI::Context& context) {
		if (!context.IsAlive(_orientationCanvas)) {
			return;
		}
		const UI::Rect box = context.GetLayout(_orientationCanvas).content;
		_orientationSidePx = box.w;
		if (_orientationSidePx <= 0.0f) {
			return;
		}
		const Vec2 center{_orientationSidePx * 0.5f, _orientationSidePx * 0.5f};
		Ops::LayOutOrientationKnobs(Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg), center,
									_orientationSidePx * Ops::ORIENTATION_ARM_FRACTION, _orientationKnobs);

		_orientationHover = Ops::ORIENTATION_KNOB_NONE;
		if (context.GetHovered() == _orientationCanvas) {
			const Vec2 local = context.GetPointerPosition() - Vec2(box.x, box.y);
			_orientationHover = Ops::HitOrientationKnob(_orientationKnobs, local,
														_orientationSidePx * Ops::ORIENTATION_KNOB_FRACTION);
		}

		const f32 scale = Math::Max(0.001f, context.GetContentScale());
		static constexpr u32 POSITIVE[3] = {0, 2, 4};
		for (u32 i = 0; i < 3; ++i) {
			UI::ElementHandle label = _orientationLabels[i];
			const UI::Rect labelBox = context.GetLayout(label).borderBox;
			const Vec2 knob = _orientationKnobs[POSITIVE[i]].centerPx;
			UI::Style style = context.GetStyle(label);
			style.left = UI::Length::Pixel((knob.x - labelBox.w * 0.5f) / scale);
			style.top = UI::Length::Pixel((knob.y - labelBox.h * 0.5f) / scale);
			context.SetStyle(label, style);
		}
	}

	void SceneViewPanel::PaintOrientation(UI::Context& context, UI::ElementHandle canvas, UI::PaintScope& paint) {
		if (_orientationSidePx <= 0.0f) {
			return;
		}
		const UI::Rect area = UI::SnapRectToPixels(context.GetLayout(canvas).content);
		const Vec2 origin{area.x, area.y};
		const Vec2 center = origin + Vec2(_orientationSidePx * 0.5f, _orientationSidePx * 0.5f);
		const f32 knobRadius = _orientationSidePx * Ops::ORIENTATION_KNOB_FRACTION;
		const f32 armWidth = Math::Max(1.0f, knobRadius * 0.4f);
		const Color screen = EditorTheme::GetColor(ColorToken::GizmoScreen);
		const UI::Element* pod = context.GetElement(canvas);
		const bool pressed = pod && HasFlag(pod->flags, UI::ElementFlags::Pressed);
		const Color highlight = EditorTheme::GetColor(pressed ? ColorToken::GizmoHighlightActive : ColorToken::GizmoHighlight);
		const ColorToken axisTokens[3] = {ColorToken::AxisX, ColorToken::AxisY, ColorToken::AxisZ};
		auto knobColor = [&](u32 i) {
			if (i == _orientationHover) {
				return highlight;
			}
			return (i % 2 == 0) ? EditorTheme::GetColor(axisTokens[i / 2]) : screen;
		};

		u32 order[Ops::ORIENTATION_KNOB_COUNT] = {0, 1, 2, 3, 4, 5};
		std::sort(std::begin(order), std::end(order), [this](u32 a, u32 b) {
			return _orientationKnobs[a].depth > _orientationKnobs[b].depth;
		});
		auto paintKnob = [&](u32 i) {
			const Ops::OrientationKnob& knob = _orientationKnobs[i];
			const Vec2 end = origin + knob.centerPx;
			const Color color = knobColor(i);
			paint.EmitLine(center, end, i % 2 == 0 ? armWidth : armWidth * 0.6f, color, UI::LineCap::Round);
			paint.EmitQuad(UI::Rect{end.x - knobRadius, end.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f},
						   color, UI::Radii::All(knobRadius));
		};
		u32 painted = 0;
		for (; painted < Ops::ORIENTATION_KNOB_COUNT && _orientationKnobs[order[painted]].depth > 0.0f; ++painted) {
			paintKnob(order[painted]);
		}
		{
			const Quat toView = Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg).Inverse();
			const f32 half = _orientationSidePx * Ops::ORIENTATION_CUBE_FRACTION;
			Vec2 corners[8];
			for (u32 c = 0; c < 8; ++c) {
				const Vec3 corner{(c & 1) ? half : -half, (c & 2) ? half : -half, (c & 4) ? half : -half};
				const Vec3 view = toView * corner;
				corners[c] = Vec2(center.x + view.x, center.y - view.y);
			}
			for (u32 c = 0; c < 8; ++c) {
				for (u32 bit = 1; bit < 8; bit <<= 1) {
					if ((c & bit) == 0) {
						paint.EmitLine(corners[c], corners[c | bit], 1.0f, screen, UI::LineCap::Round);
					}
				}
			}
		}
		for (; painted < Ops::ORIENTATION_KNOB_COUNT; ++painted) {
			paintKnob(order[painted]);
		}
	}

	void SceneViewPanel::OnOrientationClick(UI::Context& context) {
		if ((context.GetPressMods() & KeyMod::Shift) != KeyMod::None) {
			ResetViewAngle();
			return;
		}
		const UI::Rect box = context.GetLayout(_orientationCanvas).content;
		const Vec2 local = context.GetPointerPosition() - Vec2(box.x, box.y);
		const u32 hit = Ops::HitOrientationKnob(_orientationKnobs, local,
												_orientationSidePx * Ops::ORIENTATION_KNOB_FRACTION);
		if (hit != Ops::ORIENTATION_KNOB_NONE) {
			SnapViewToAxis(_orientationKnobs[hit].axis);
			return;
		}
		ToggleProjection();
	}

	void SceneViewPanel::OpenOrientationMenu(UI::Context& context) {
		struct View {
			std::string_view label;
			Vec3 axis;
		};
		static constexpr View Views[] = {
			{"Top", Vec3(0.0f, 1.0f, 0.0f)},
			{"Bottom", Vec3(0.0f, -1.0f, 0.0f)},
			{"Left", Vec3(-1.0f, 0.0f, 0.0f)},
			{"Right", Vec3(1.0f, 0.0f, 0.0f)},
			{"Front", Vec3(0.0f, 0.0f, -1.0f)},
			{"Back", Vec3(0.0f, 0.0f, 1.0f)},
		};
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		UI::Widgets::MenuItem free;
		free.label = "Free";
		free.onSelect = [this]() { ResetViewAngle(); };
		items.push_back(std::move(free));
		for (const View& view : Views) {
			UI::Widgets::MenuItem item;
			item.label = view.label;
			item.separatorBefore = &view == &Views[0];
			item.onSelect = [this, axis = view.axis]() { SnapViewToAxis(axis); };
			items.push_back(std::move(item));
		}
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(), items, UISystem::GetDefaultFont(),
							  EditorTheme::GetMetric(MetricToken::FontBody));
	}

	void SceneViewPanel::OpenEffectsMenu(UI::Context& context) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		UI::Widgets::MenuItem refresh;
		refresh.label = "Always Refresh";
		refresh.checked = alwaysRefresh;
		refresh.onSelect = [this]() { alwaysRefresh = !alwaysRefresh; };
		items.push_back(std::move(refresh));
		UI::Widgets::MenuItem post;
		post.label = "Post Processing";
		post.checked = showPostProcessing;
		post.onSelect = [this]() { showPostProcessing = !showPostProcessing; };
		items.push_back(std::move(post));
		Vector<str> labels{GetMemoryResource(MemTag::Editor)};
		SceneViewEffects::GetLabels(labels);
		for (const str& label : labels) {
			UI::Widgets::MenuItem category;
			category.label = label;
			category.checked = std::find(hiddenEffects.begin(), hiddenEffects.end(), label) == hiddenEffects.end();
			category.onSelect = [this, label]() {
				auto hidden = std::find(hiddenEffects.begin(), hiddenEffects.end(), label);
				if (hidden == hiddenEffects.end()) {
					hiddenEffects.push_back(label);
				} else {
					hiddenEffects.erase(hidden);
				}
			};
			items.push_back(std::move(category));
		}
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(), items, UISystem::GetDefaultFont(),
							  EditorTheme::GetMetric(MetricToken::FontBody));
	}

	void SceneViewPanel::OpenGizmosMenu(UI::Context& context) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		UI::Widgets::MenuItem outline;
		outline.label = "Selection Outline";
		outline.checked = EditorConfig::ReadGeneralPreferences().showSelectionOutline;
		outline.onSelect = []() {
			EditorConfig::GeneralPreferences preferences = EditorConfig::ReadGeneralPreferences();
			preferences.showSelectionOutline = !preferences.showSelectionOutline;
			EditorConfig::WriteGeneralPreferences(preferences);
		};
		items.push_back(std::move(outline));
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(), items, UISystem::GetDefaultFont(),
							  EditorTheme::GetMetric(MetricToken::FontBody));
	}

	Vec3 SceneViewPanel::PivotPoint() const {
		return cameraPosition + (Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg) * Vec3::Forward()) * _pivotDistance;
	}

	void SceneViewPanel::SnapViewToAxis(const Vec3& axis) {
		const Vec3 pivot = PivotPoint();
		Ops::ViewAnglesFromAxis(axis, cameraYawDeg, cameraYawDeg, cameraPitchDeg);
		cameraPosition = pivot - (Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg) * Vec3::Forward()) * _pivotDistance;
	}

	void SceneViewPanel::ToggleProjection() {
		orthographic = !orthographic;
		_orientationBarDirty = true;
	}

	void SceneViewPanel::SetMode2D(bool enabled) {
		if (enabled == mode2D) {
			return;
		}
		if (enabled) {
			parkedPosition = cameraPosition;
			parkedYawDeg = cameraYawDeg;
			parkedPitchDeg = cameraPitchDeg;
			parkedOrthographic = orthographic;
			cameraYawDeg = 0.0f;
			cameraPitchDeg = 0.0f;
			orthographic = true;
		} else {
			cameraPosition = parkedPosition;
			cameraYawDeg = parkedYawDeg;
			cameraPitchDeg = parkedPitchDeg;
			orthographic = parkedOrthographic;
		}
		mode2D = enabled;
		_toolbarDirty = true;
		_orientationBarDirty = true;
	}

	void SceneViewPanel::ZoomOrthographic(f32 wheel) {
		orthographicSize = Math::Clamp(orthographicSize * std::pow(1.15f, -wheel), Ops::MIN_ORTHOGRAPHIC_SIZE,
									   Ops::MAX_ORTHOGRAPHIC_SIZE);
	}

	void SceneViewPanel::ResetViewAngle() {
		const Vec3 pivot = PivotPoint();
		cameraYawDeg = DEFAULT_CAMERA_YAW_DEG;
		cameraPitchDeg = DEFAULT_CAMERA_PITCH_DEG;
		cameraPosition = pivot - (Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg) * Vec3::Forward()) * _pivotDistance;
	}

	void SceneViewPanel::RefreshGizmoTargets() {
		Ops::GatherDragTargets(_gizmoTargets);
	}

	std::string_view SceneViewPanel::DragLabel() const {
		switch (static_cast<GizmoMode>(gizmoMode)) {
		case GizmoMode::Translate:
			return "Move";
		case GizmoMode::Rotate:
			return "Rotate";
		default:
			return "Scale";
		}
	}

	Quat SceneViewPanel::GizmoOrientationFor(const SceneObject& target) const {
		return Gizmo::OrientationForSpace(static_cast<GizmoSpace>(gizmoSpace),
										  static_cast<GizmoMode>(gizmoMode), target.GetWorldRotation());
	}

	bool SceneViewPanel::BuildGizmoDesc(const Vec3& origin, const Quat& orientation, const Vec2& sizePx,
										Gizmo::GizmoDesc& out) const {
		if (sizePx.x <= 0.0f || sizePx.y <= 0.0f) {
			return false;
		}
		out.origin = origin;
		out.orientation = orientation;
		out.viewpoint = CurrentViewpoint();
		out.mode = static_cast<GizmoMode>(gizmoMode);
		out.scale = Gizmo::HandleScaleAt(out.viewpoint, origin);
		out.pickTolerance = Gizmo::WorldRadiusForPixels(out.viewpoint, origin, sizePx.y, Gizmo::HANDLE_PICK_PX);
		return true;
	}

	Ops::Viewpoint SceneViewPanel::CurrentViewpoint() const {
		Ops::Viewpoint viewpoint;
		viewpoint.position = cameraPosition;
		viewpoint.rotation = Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg);
		viewpoint.aspect = _viewport.GetAspect();
		viewpoint.orthographic = orthographic;
		viewpoint.orthographicSize = orthographicSize;
		return viewpoint;
	}

	bool SceneViewPanel::SnapHeld(UI::Context& context) {
		return (context.GetPointerMods() & KeyMod::Ctrl) != KeyMod::None;
	}

	bool SceneViewPanel::VertexSnapHeld() const {
		return _viewportKeys.IsPressed(_vertexSnapAction);
	}

	Ray SceneViewPanel::RayAt(UI::Context& context, Vec2 contextPx) const {
		UI::Rect area = _viewport.GetRect(context);
		const Ops::Viewpoint viewpoint = CurrentViewpoint();
		return Ops::BuildViewportRay(Ops::BuildViewMatrix(viewpoint), Ops::BuildProjection(viewpoint),
									 _viewport.ToLocal(context, contextPx), Vec2{area.w, area.h});
	}

	Mat4 SceneViewPanel::ViewProjection() const {
		const Ops::Viewpoint viewpoint = CurrentViewpoint();
		return Ops::BuildProjection(viewpoint) * Ops::BuildViewMatrix(viewpoint);
	}

	void SceneViewPanel::OnViewportPress(UI::Context& context) {
		if (!_viewport.IsReady()) {
			return;
		}
		UI::Rect area = _viewport.GetRect(context);
		Vec2 sizePx{area.w, area.h};
		Ray ray = RayAt(context, context.GetPointerPosition());

		RefreshGizmoTargets();

		if (VertexSnapHeld() && _vertexAnchorValid && !_gizmoTargets.empty()) {
			Gizmo::SnapshotTargets(_gizmoTargets, _dragStarts);
			_vertexExcluded.assign(_gizmoTargets.begin(), _gizmoTargets.end());
			_vertexDrag = true;
			UndoSystem::BeginGroup(DragLabel());
			_dragGroupOpen = true;
			EditorTransformGesture::Begin(_gizmoTargets);
			return;
		}

		if (_sceneHover >= 0 && static_cast<size_t>(_sceneHover) < _sceneHandles.size()) {
			const OfferedSceneHandle& offered = _sceneHandles[static_cast<size_t>(_sceneHover)];
			GizmoMode mode = GizmoMode::Translate;
			Gizmo::GizmoDesc handleDesc;
			const bool begun = offered.handle.kind == SceneHandleKind::Slider
								   ? Gizmo::BeginSliderDrag(CurrentViewpoint(), offered.handle, ray, _sceneSlider)
								   : Gizmo::SceneHandleMode(offered.handle.kind, static_cast<GizmoMode>(gizmoMode), mode) &&
										 SceneHandleDesc(offered.handle, mode, sizePx, handleDesc) &&
										 Gizmo::BeginDrag(handleDesc, _sceneHoverPart, ray, _sceneDrag);
			if (begun) {
				_sceneDragging = true;
				_sceneSource = offered.source;
				_sceneStart = offered.handle;
				_sceneMode = mode;
				UndoSystem::BeginGroup(offered.handle.label);
				_dragGroupOpen = true;
			}
			return;
		}

		SceneObject* frame = _gizmoTargets.empty() ? nullptr : _gizmoTargets.front().Get();
		Gizmo::GizmoDesc desc;
		if (frame && _hover != GizmoHandle::None &&
			BuildGizmoDesc(Gizmo::PivotFor(_gizmoTargets), GizmoOrientationFor(*frame), sizePx, desc)) {
			if (!Gizmo::BeginDrag(desc, _hover, ray, _drag)) {
				return;
			}
			Gizmo::SnapshotTargets(_gizmoTargets, _dragStarts);
			UndoSystem::BeginGroup(DragLabel());
			_dragGroupOpen = true;
			EditorTransformGesture::Begin(_gizmoTargets);
			return;
		}

		if (_radiusHover >= 0 && static_cast<size_t>(_radiusHover) < _radiusDots.size()) {
			const ComponentRadius& radius = _radii[_radiusDotOwners[static_cast<size_t>(_radiusHover)]];
			if (radius.component.IsValid() &&
				Gizmo::BeginRadiusDrag(CurrentViewpoint(), radius.handle.center, _radiusDots[static_cast<size_t>(_radiusHover)],
									   radius.handle.radius, ray, _radiusDrag)) {
				_radiusDragging = true;
				_radiusComponent = radius.component;
				_radiusField = radius.handle.field;
				_radiusLabel = radius.handle.label;
				_radiusStart = radius.handle.radius;
				UndoSystem::BeginGroup(_radiusLabel);
				_dragGroupOpen = true;
			}
			return;
		}

		const Vec2 local = _viewport.ToLocal(context, context.GetPointerPosition());
		const u32 icon = EditorGizmos::HitSceneIcon(_iconPlacements, local, _iconSizePx);
		const bool toggle = (context.GetPointerMods() & KeyMod::Ctrl) != KeyMod::None;
		if (icon != SCENE_ICON_NONE) {
			ApplyPick(_iconPlacements[icon].object, toggle);
			return;
		}
		_pick.pixel = Vec2I(static_cast<i32>(local.x), static_cast<i32>(local.y));
		_pick.tag = _nextPickTag++;
		_pickAsked = true;
		_pendingPicks.push_back({_pick.tag, toggle});
	}

	void SceneViewPanel::ApplyPick(Handle<SceneObject> picked, bool toggle) {
		if (picked.IsValid()) {
			if (toggle) {
				EditorSelection::ToggleInSelection(Handle<Object>(picked.Get()));
			} else {
				EditorSelection::Select(Handle<Object>(picked.Get()));
			}
		} else if (!toggle) {
			EditorSelection::Clear();
		}
	}

	void SceneViewPanel::TakePickAnswers() {
		PickAnswer answer;
		while (RenderSystem::TakePickAnswer(_viewport.GetTarget(), answer)) {
			for (size_t index = 0; index < _pendingPicks.size(); ++index) {
				if (_pendingPicks[index].tag == answer.tag) {
					const bool toggle = _pendingPicks[index].toggle;
					_pendingPicks.erase(_pendingPicks.begin() + static_cast<std::ptrdiff_t>(index));
					ApplyPick(answer.object, toggle);
					break;
				}
			}
		}
	}

	void SceneViewPanel::OnViewportDrag(UI::Context& context, Vec2 contextPx) {
		if (!_viewport.IsReady()) {
			return;
		}
		if (_vertexDrag) {
			UpdateVertexDrag(context, contextPx);
			return;
		}
		if (_radiusDragging) {
			UpdateRadiusDrag(context, contextPx);
			return;
		}
		if (_sceneDragging) {
			UpdateSceneHandleDrag(context, contextPx);
			return;
		}
		if (_drag.handle == GizmoHandle::None) {
			return;
		}
		SceneObject* frame = _dragStarts.empty() ? nullptr : _dragStarts.front().object.Get();
		if (!frame) {
			OnViewportRelease();
			return;
		}
		UI::Rect area = _viewport.GetRect(context);
		Gizmo::GizmoDesc desc;
		Quat orientation = GizmoOrientationFor(*frame);
		if (!BuildGizmoDesc(_drag.origin, orientation, Vec2{area.w, area.h}, desc)) {
			return;
		}
		Gizmo::DragValue value;
		if (!Gizmo::UpdateDrag(desc, _drag, RayAt(context, contextPx), value)) {
			return;
		}

		Gizmo::SnapSettings snap;
		if (SnapHeld(context)) {
			snap.translate = snapTranslate;
			snap.rotateDeg = snapRotateDeg;
			snap.scale = snapScale;
		}
		Gizmo::SolveDragTargets(_dragStarts, static_cast<GizmoMode>(gizmoMode), _drag, value, _drag.origin,
								orientation, snap, _dragOutcomes);
		PushDragCommands();
	}

	void SceneViewPanel::UpdateVertexDrag(UI::Context& context, Vec2 contextPx) {
		if (_dragStarts.empty()) {
			OnViewportRelease();
			return;
		}
		UI::Rect area = _viewport.GetRect(context);
		Vec3 target;
		if (!Ops::FindNearestVertexUnderCursor(_vertexExcluded, RayAt(context, contextPx), ViewProjection(),
											   Vec2{area.w, area.h}, _viewport.ToLocal(context, contextPx),
											   target)) {
			return;
		}
		_vertexLive = target;

		Gizmo::DragValue value;
		value.translation = target - _vertexAnchor;
		Gizmo::SolveDragTargets(_dragStarts, GizmoMode::Translate, Gizmo::DragState{}, value, _vertexAnchor,
								Quat::Identity(), Gizmo::SnapSettings{}, _dragOutcomes);
		PushDragCommands();
	}

	void SceneViewPanel::PushDragCommands() {
		std::string_view label = DragLabel();
		for (size_t i = 0; i < _dragStarts.size(); ++i) {
			const Gizmo::DragSnapshot& start = _dragStarts[i];
			const Gizmo::DragOutcome& outcome = _dragOutcomes[i];
			SceneObject* object = start.object.Get();
			if (!object) {
				continue;
			}
			Handle<Object> handle(object);
			if (outcome.positionChanged) {
				UndoSystem::Push(MakeUnique<PropertySetCommand>(handle, "_localPosition"_hash,
																Variant(start.localPosition),
																Variant(outcome.localPosition), label));
			}
			if (outcome.rotationChanged) {
				UndoSystem::Push(MakeUnique<PropertySetCommand>(handle, "_localRotation"_hash,
																Variant(start.localRotation),
																Variant(outcome.localRotation), label));
			}
			if (outcome.scaleChanged) {
				UndoSystem::Push(MakeUnique<PropertySetCommand>(handle, "_localScale"_hash,
																Variant(start.localScale),
																Variant(outcome.localScale), label));
			}
		}
	}

	void SceneViewPanel::OnViewportDrop(UI::Context& context, const UI::DragPayload& payload, Vec2 contextPx) {
		if (ComponentDrops::Accepts(payload)) {
			ComponentDrops::Drop(payload, Ops::PickObject(RayAt(context, contextPx)));
			return;
		}
		Handle<Scene> scene = DropTargetScene();
		Guid prefab = DroppedPrefab(payload, scene);
		const bool made = !prefab.IsValid() && SceneAssetDrops::Accepts(payload, scene);
		if (!prefab.IsValid() && !made) {
			return;
		}
		Vec3 world{};
		if (!Ops::FindPlacementPoint(RayAt(context, contextPx), world)) {
			return;
		}
		if (made) {
			SceneAssetDrops::Drop(payload, scene, EditorSceneOps::GetDefaultParent(scene), &world);
			return;
		}

		UndoSystem::Push(MakeUnique<InstantiatePrefabCommand>(
			prefab, scene, EditorSceneOps::GetDefaultParent(scene), world));
	}

	void SceneViewPanel::OnViewportRelease() {
		if (_dragGroupOpen) {
			UndoSystem::EndGroup();
			_dragGroupOpen = false;
		}
		EditorTransformGesture::End();
		_drag = {};
		_dragStarts.clear();
		_vertexDrag = false;
		_vertexExcluded.clear();
		_radiusDragging = false;
		_radiusComponent = {};
		_sceneDragging = false;
	}

	void SceneViewPanel::UpdateRadii(UI::Context& context, const Vec2& sizePx) {
		_radiusDots.clear();
		_radiusDotOwners.clear();
		if (!showGizmos) {
			_radii.clear();
			_radiusHover = -1;
			return;
		}
		EditorGizmos::GatherRadiusHandles(_radii);
		const Ops::Viewpoint viewpoint = CurrentViewpoint();
		for (size_t index = 0; index < _radii.size(); ++index) {
			const RadiusHandle& radius = _radii[index].handle;
			Gizmo::RadiusDot dots[6];
			const u32 count = Gizmo::RadiusDots(viewpoint, radius.center, radius.rotation, radius.radius, dots);
			for (u32 dot = 0; dot < count; ++dot) {
				_radiusDots.push_back(dots[dot]);
				_radiusDotOwners.push_back(static_cast<u32>(index));
			}
		}
		if (!_radiusDragging) {
			_radiusHover = -1;
			if (_pointerRouteOpen && _hover == GizmoHandle::None && _drag.handle == GizmoHandle::None && !_vertexAnchorValid && !_vertexDrag &&
				_sceneHover < 0 && !_sceneDragging) {
				f32 rayT = 0.0f;
				_radiusHover = Gizmo::HitRadiusDots(_radiusDots, viewpoint, sizePx.y, RayAt(context, context.GetPointerPosition()), rayT);
			}
		}
		for (size_t index = 0; index < _radiusDots.size(); ++index) {
			const ComponentRadius& radius = _radii[_radiusDotOwners[index]];
			const bool dragged = _radiusDragging && radius.component == _radiusComponent && radius.handle.field == _radiusField &&
								 Vec3::Dot(_radiusDots[index].axis, _radiusDrag.axis) > 0.999f;
			const bool lit = dragged || (!_radiusDragging && static_cast<i32>(index) == _radiusHover);
			Gizmo::AppendRadiusDots(_overlayLines, {&_radiusDots[index], 1}, viewpoint, radius.handle.color, lit ? 0 : -1);
		}
	}

	void SceneViewPanel::UpdateRadiusDrag(UI::Context& context, Vec2 contextPx) {
		if (!_radiusComponent.Get()) {
			OnViewportRelease();
			return;
		}
		f32 radius = 0.0f;
		if (!Gizmo::UpdateRadiusDrag(_radiusDrag, RayAt(context, contextPx), radius)) {
			return;
		}
		UndoSystem::Push(MakeUnique<PropertySetCommand>(_radiusComponent, _radiusField, Variant(_radiusStart), Variant(radius), _radiusLabel));
	}

	bool SceneViewPanel::SceneHandleDesc(const SceneHandle& handle, GizmoMode mode, const Vec2& sizePx, Gizmo::GizmoDesc& out) const {
		const Vec3 origin = Gizmo::SceneHandleOrigin(handle);
		if (!BuildGizmoDesc(origin, Gizmo::OrientationForSpace(static_cast<GizmoSpace>(gizmoSpace), mode, Gizmo::SceneHandleRotation(handle)), sizePx,
							out)) {
			return false;
		}
		out.mode = mode;
		return true;
	}

	void SceneViewPanel::UpdateSceneHandles(UI::Context& context, const Vec2& sizePx) {
		if (!showGizmos) {
			_sceneHandles.clear();
			_sceneHover = -1;
			return;
		}
		EditorGizmos::GatherSceneHandles(_sceneHandles);
		const Ops::Viewpoint viewpoint = CurrentViewpoint();
		const GizmoMode tool = static_cast<GizmoMode>(gizmoMode);
		if (!_sceneDragging) {
			_sceneHover = -1;
			_sceneHoverPart = GizmoHandle::None;
			if (_pointerRouteOpen && _drag.handle == GizmoHandle::None && !_radiusDragging && !_vertexAnchorValid && !_vertexDrag) {
				const Ray ray = RayAt(context, context.GetPointerPosition());
				f32 bestT = 0.0f;
				for (size_t index = 0; index < _sceneHandles.size(); ++index) {
					const SceneHandle& handle = _sceneHandles[index].handle;
					f32 t = 0.0f;
					GizmoHandle part = GizmoHandle::None;
					if (handle.kind == SceneHandleKind::Slider) {
						const Gizmo::RadiusDot dot = Gizmo::SliderDot(handle);
						if (Gizmo::HitRadiusDots({&dot, 1}, viewpoint, sizePx.y, ray, t) < 0) {
							continue;
						}
					} else {
						GizmoMode mode = GizmoMode::Translate;
						Gizmo::GizmoDesc desc;
						if (!Gizmo::SceneHandleMode(handle.kind, tool, mode) || !SceneHandleDesc(handle, mode, sizePx, desc)) {
							continue;
						}
						const Gizmo::GizmoHit hit = Gizmo::HitTest(desc, ray);
						if (hit.handle == GizmoHandle::None) {
							continue;
						}
						part = hit.handle;
						t = hit.distance;
					}
					if (_sceneHover < 0 || t < bestT) {
						_sceneHover = static_cast<i32>(index);
						_sceneHoverPart = part;
						bestT = t;
					}
				}
			}
		}
		for (size_t index = 0; index < _sceneHandles.size(); ++index) {
			const OfferedSceneHandle& offered = _sceneHandles[index];
			const bool dragged = _sceneDragging && offered.source == _sceneSource && offered.handle.id == _sceneStart.id;
			const bool hovered = !_sceneDragging && static_cast<i32>(index) == _sceneHover;
			if (offered.handle.kind == SceneHandleKind::Slider) {
				const Gizmo::RadiusDot dot = Gizmo::SliderDot(offered.handle);
				Gizmo::AppendRadiusDots(_overlayLines, {&dot, 1}, viewpoint, offered.handle.color, dragged || hovered ? 0 : -1);
				continue;
			}
			GizmoMode mode = GizmoMode::Translate;
			Gizmo::GizmoDesc desc;
			if (Gizmo::SceneHandleMode(offered.handle.kind, tool, mode) && SceneHandleDesc(offered.handle, mode, sizePx, desc)) {
				Gizmo::AppendGizmo(_overlayLines, desc, hovered ? _sceneHoverPart : GizmoHandle::None, dragged ? _sceneDrag.handle : GizmoHandle::None);
			}
		}
	}

	void SceneViewPanel::UpdateSceneHandleDrag(UI::Context& context, Vec2 contextPx) {
		const auto offered = std::find_if(_sceneHandles.begin(), _sceneHandles.end(), [&](const OfferedSceneHandle& entry) {
			return entry.source == _sceneSource && entry.handle.id == _sceneStart.id;
		});
		if (offered == _sceneHandles.end()) {
			OnViewportRelease();
			return;
		}
		SceneHandle dragged = _sceneStart;
		const Ray ray = RayAt(context, contextPx);
		if (_sceneStart.kind == SceneHandleKind::Slider) {
			if (!Gizmo::UpdateSliderDrag(_sceneSlider, _sceneStart, ray, dragged.distance)) {
				return;
			}
		} else {
			const UI::Rect area = _viewport.GetRect(context);
			Gizmo::GizmoDesc desc;
			if (!SceneHandleDesc(_sceneStart, _sceneMode, Vec2{area.w, area.h}, desc)) {
				return;
			}
			desc.origin = _sceneDrag.origin;
			Gizmo::DragValue value;
			if (!Gizmo::UpdateDrag(desc, _sceneDrag, ray, value)) {
				return;
			}
			Gizmo::SnapSettings snap;
			if (SnapHeld(context)) {
				snap.translate = snapTranslate;
				snap.rotateDeg = snapRotateDeg;
				snap.scale = snapScale;
			}
			dragged = Gizmo::SolveSceneHandleDrag(_sceneStart, _sceneMode, _sceneDrag, value, desc.orientation, snap);
		}
		EditorGizmos::WriteSceneHandle(_sceneSource, dragged);
	}

	void SceneViewPanel::UpdateVertexAnchor(UI::Context& context, const Vec2& sizePx) {
		if (_vertexDrag) {
			return;
		}
		_vertexAnchorValid = false;
		if (static_cast<GizmoMode>(gizmoMode) != GizmoMode::Translate || !VertexSnapHeld() ||
			_gizmoTargets.empty()) {
			return;
		}
		_vertexAnchorValid = Ops::FindNearestVertexIn(_gizmoTargets, ViewProjection(), sizePx,
													  _viewport.ToLocal(context, context.GetPointerPosition()),
													  _vertexAnchor);
		if (_vertexAnchorValid) {
			_vertexLive = _vertexAnchor;
		}
	}

	void SceneViewPanel::BuildDebugLines() {
		_lines.clear();
		_overlayLines.clear();
		_sceneIcons.clear();

		if (showCascadeFrustum) {
			CascadeFit fits[MAX_SHADOW_CASCADES];
			const u32 count = RenderSystem::GetCascadeFit(_viewport.GetTarget(), {fits, std::size(fits)});
			EditorGizmos::AppendCascadeFitLines(_lines, {fits, count}, cascadeFrustumIndex);
			if (_freezeShadowFit) {
				EditorGizmos::AppendViewFrustumLines(_lines,
													 Ops::BuildProjection(_frozenFit) * Ops::BuildViewMatrix(_frozenFit));
			}
		}

		if (!showGizmos) {
			return;
		}

		GizmoDrawContext gizmos;
		gizmos.lines = &_lines;
		gizmos.overlayLines = &_overlayLines;
		gizmos.icons = &_sceneIcons;
		gizmos.cameraPosition = cameraPosition;
		gizmos.cameraRotation = Ops::BuildCameraRotation(cameraYawDeg, cameraPitchDeg);
		gizmos.viewAspect = _viewport.GetAspect();

		UI::Context* context = GetPanelContext();
		const UI::Rect viewportRect = context ? _viewport.GetRect(*context) : UI::Rect{};
		gizmos.cursorInView = _pointerRouteOpen && context && viewportRect.w > 0.0f && viewportRect.h > 0.0f;
		if (gizmos.cursorInView) {
			gizmos.cursorRay = RayAt(*context, context->GetPointerPosition());
		}

		EditorGizmos::DrawSceneGizmos(gizmos);
	}

	void SceneViewPanel::LayOutIcons(UI::Context& context, const Vec2& sizePx) {
		_iconSizePx = EditorTheme::GetMetric(MetricToken::MarkSceneIcon) *
					  Math::Max(0.001f, context.GetContentScale());
		EditorGizmos::LayOutSceneIcons(_sceneIcons, ViewProjection(), sizePx, _iconSizePx, _iconPlacements);
	}

	void SceneViewPanel::PaintIcons(UI::PaintScope& paint, const UI::Rect& area) {
		if (_iconPlacements.empty() || _iconSizePx <= 0.0f) {
			return;
		}
		UI::IVectorIconSystem* iconSystem = UISystem::GetVectorIconSystem();
		if (!iconSystem) {
			return;
		}
		for (const SceneIconPlacement& placement : _iconPlacements) {
			const std::string_view markPath = EditorIcons::ComponentIcons::For(placement.type);
			if (markPath.empty()) {
				continue;
			}
			UI::IconQuery query;
			query.asset = EditorIcons::GetVector(markPath);
			query.sizePx = _iconSizePx;
			UI::RasterizedIcon rasterized;
			if (!query.asset.IsValid() || !iconSystem->GetIcon(query, rasterized)) {
				continue;
			}
			const UI::Rect rect{area.x + placement.centerPx.x - rasterized.sizePx.x * 0.5f,
								area.y + placement.centerPx.y - rasterized.sizePx.y * 0.5f,
								rasterized.sizePx.x, rasterized.sizePx.y};
			paint.EmitTexturedQuad(rect, rasterized.atlas, rasterized.uv, Color::White(),
								   UI::SamplerKind::LinearClamp, {}, UI::TexelEncoding::Premultiplied);
		}
	}

	void SceneViewPanel::OnTick(f32 deltaSeconds) {
		UI::Context* context = GetPanelContext();
		if (!context) {
			return;
		}
		if (!context->IsAlive(_viewport.GetElement())) {
			return;
		}
		if (_toolbarDirty) {
			_toolbarDirty = false;
			FillToolbar(*context);
		}
		UpdateComponentOverlays(*context);
		_statusAccum += deltaSeconds;
		if (_statusAccum >= STATUS_REFRESH_SECONDS) {
			_statusAccum = 0.0f;
			RefreshToolbar(*context);
		}
		RefreshFollowChip(*context);
		if (_orientationBarDirty) {
			_orientationBarDirty = false;
			RefreshOrientationVisibility(*context);
			FillOrientationBar(*context);
		}
		LayOutOrientation(*context);
	}

	void SceneViewPanel::OnSubmitView(f32 deltaSeconds) {
		UI::Context* context = GetPanelContext();
		if (!context) {
			return;
		}
		_viewport.Tick(*context, deltaSeconds);

		EditorInputOps::RouteFacts facts = EditorInputOps::ApplicationFacts(*context);
		facts.pointerInside = context->HitTest(context->GetPointerPosition()) == _viewport.GetElement();
		facts.gestureHeld = _looking;
		_pointerRouteOpen = EditorInputOps::ShouldOpen(EditorInputOps::Routes::ViewportPointer, facts);
		EditorInputOps::ApplyRoute(_viewportKeys, EditorInputOps::Routes::ViewportKeyboard, facts);
		_viewportKeys.Evaluate();

		UpdateCamera(*context, deltaSeconds);
		BuildDebugLines();

		UI::Rect area = _viewport.GetRect(*context);
		Vec2 sizePx{area.w, area.h};
		const Ops::Viewpoint viewpoint = CurrentViewpoint();
		Mat4 view = Ops::BuildViewMatrix(viewpoint);
		Mat4 projection = Ops::BuildProjection(viewpoint);

		LayOutIcons(*context, sizePx);

		UpdateSceneHandles(*context, sizePx);

		RefreshGizmoTargets();
		SceneObject* frame = _gizmoTargets.empty() ? nullptr : _gizmoTargets.front().Get();
		UpdateVertexAnchor(*context, sizePx);
		Vec3 origin = _vertexAnchorValid ? _vertexLive : Gizmo::PivotFor(_gizmoTargets);
		Gizmo::GizmoDesc desc;
		if (frame && BuildGizmoDesc(origin, GizmoOrientationFor(*frame), sizePx, desc)) {
			if (_vertexAnchorValid || _radiusDragging || _sceneDragging || _sceneHover >= 0) {
				_hover = GizmoHandle::None;
			} else if (_drag.handle == GizmoHandle::None) {
				_hover = _pointerRouteOpen
							 ? Gizmo::HitTest(desc, RayAt(*context, context->GetPointerPosition())).handle
							 : GizmoHandle::None;
			}
			Gizmo::AppendGizmo(_overlayLines, desc, _hover, _drag.handle);
			if (_vertexAnchorValid) {
				Gizmo::AppendVertexMarker(_overlayLines, _vertexLive, desc.viewpoint.rotation,
										  desc.scale * Gizmo::VERTEX_MARKER_HALF, VertexSnapMark());
			}
		} else {
			_hover = GizmoHandle::None;
		}
		UpdateRadii(*context, sizePx);

		TakePickAnswers();
		if (_viewport.IsReady()) {
			ViewDesc desc;
			desc.view = view;
			desc.projection = projection;
			desc.cameraPosition = cameraPosition;
			desc.clearColor = ViewClear();
			desc.fill = GetShadingFill();
			desc.wireColor = WireColorFor(desc.fill);
			desc.debugView = GetDebugView();
			desc.retainCascadeFit = showCascadeFrustum;
			desc.debugOverlay = ClampedAtlasOverlay();
			Mat4 frozenView;
			Mat4 frozenProjection;
			if (_freezeShadowFit) {
				frozenView = Ops::BuildViewMatrix(_frozenFit);
				frozenProjection = Ops::BuildProjection(_frozenFit);
				desc.shadowFitView = &frozenView;
				desc.shadowFitProjection = &frozenProjection;
			}
			_passRows.clear();
			if (showEffects && showPostProcessing) {
				Handle<Camera> camera = Ops::FindActiveSceneCamera();
				if (camera.Get()) {
					Ops::CollectCameraPassRows(camera, _passRows);
				}
			}
			if (showEffects && alwaysRefresh) {
				_refreshClock = Time::GetRealtimeSinceStartup();
				desc.time = &_refreshClock;
			}
			if (showGrid) {
				_passRows.insert(_passRows.begin(), Handle<RenderPass>(&_gridPass));
			}
			if (!_passRows.empty()) {
				desc.passes = {_passRows.data(), _passRows.size()};
			}
			desc.lines = {_lines.data(), _lines.size()};
			desc.overlayLines = {_overlayLines.data(), _overlayLines.size()};
			if (EditorConfig::ReadGeneralPreferences().showSelectionOutline) {
				Ops::BuildSelectionOutlines(EditorSelection::GetSelection(), SelectionOutline(), SelectionChildOutline(), _outlines);
			} else {
				_outlines.clear();
			}
			desc.outlines = {_outlines.data(), _outlines.size()};
			desc.outlineWidth = EditorTheme::GetMetric(MetricToken::SelectionOutlineWidth) * Math::Max(0.001f, context->GetContentScale());
			if (_pickAsked) {
				desc.pick = &_pick;
				_pickAsked = false;
			}
			Vector<Handle<Scene>> scenes{GetMemoryResource(MemTag::Editor)};
			if (EditorSceneOps::GetPrefabStage().IsValid()) {
				EditorSceneOps::GetStageScenes(scenes);
				desc.scenes = {scenes.data(), scenes.size()};
			}
			SceneViewEffects::GetHiddenRenderers([this](std::string_view label) { return ShowsEffect(label); }, _hiddenRenderers);
			desc.skippedProceduralTypes = {_hiddenRenderers.data(), _hiddenRenderers.size()};
			RenderSystem::SubmitView(_viewport.GetTarget(), desc);
		}
	}

	EditorIcons::Icon SceneViewPanel::GetTabIcon() const {
		return EditorIcons::Glyph::PanelScene;
	}

	bool SceneViewPanel::OnCommand(PanelCommand command) {
		switch (command) {
		case PanelCommand::Delete:
			return EditorSceneOps::DeleteSelectedObjects() > 0;
		case PanelCommand::Duplicate:
			return EditorSceneOps::DuplicateSelectedObjects() > 0;
		default:
			return EditorPanel::OnCommand(command);
		}
	}

} // namespace Shalltear
