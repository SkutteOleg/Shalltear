#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/VectorIcon.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/EditorAPI.h"
#include "Shalltear/Editor/EditorTheme.h"

#include <functional>
#include <string_view>

namespace Shalltear {
	class VectorAsset;
	struct TypeInfo;
} //namespace Shalltear

namespace Shalltear::EditorIcons {

	enum class Face : u8 {
		Solid,
		Regular,
	};
	inline constexpr u32 FACE_COUNT = 2;

	struct Icon {
		std::string_view glyph;
		Face face = Face::Solid;
	};

	namespace Glyph {

		inline constexpr Icon Play{"\uf04b"};
		inline constexpr Icon Stop{"\uf04d"};
		inline constexpr Icon Pause{"\uf04c"};
		inline constexpr Icon Step{"\uf051"};
		inline constexpr Icon Loop{"\uf363"};
		inline constexpr Icon AutoPlay{"\uf144"};
		inline constexpr Icon MuteAudio{"\uf6a9"};

		inline constexpr Icon WindowMinimize{"\uf2d1", Face::Regular};
		inline constexpr Icon WindowMaximize{"\uf2d0", Face::Regular};
		inline constexpr Icon WindowRestore{"\uf2d2", Face::Regular};
		inline constexpr Icon WindowClose{"\uf00d", Face::Regular};
		inline constexpr Icon Logo{"\uf6e2"};

		inline constexpr Icon Compile{"\uf6e3"};
		inline constexpr Icon Reload{"\uf021"};

		inline constexpr Icon CaretRight{"\uf0da"};
		inline constexpr Icon CaretDown{"\uf0d7"};
		inline constexpr Icon FoldOutOpen{"\uf054", Face::Regular};
		inline constexpr Icon FoldOutClose{"\uf053", Face::Regular};

		inline constexpr Icon AssetGeneric{"\uf15b"};
		inline constexpr Icon AssetText{"\uf15c"};
		inline constexpr Icon AssetScript{"\uf70e"};
		inline constexpr Icon AssetShader{"\uf1c9"};
		inline constexpr Icon AssetTexture{"\uf03e"};
		inline constexpr Icon AssetMesh{"\uf1b2"};
		inline constexpr Icon AssetMaterial{"\uf53f"};
		inline constexpr Icon AssetFont{"\uf031"};
		inline constexpr Icon AssetScene{"\uf1b3", Face::Regular};
		inline constexpr Icon AssetPrefab{"\uf24d"};
		inline constexpr Icon AssetPrefabVariant{"\uf247"};
		inline constexpr Icon AssetUIDocument{"\uf121"};
		inline constexpr Icon AssetStyleSheet{"\uf1fc"};
		inline constexpr Icon AssetPanelSettings{"\uf108"};
		inline constexpr Icon AssetGraphicsProfile{"\uf1de"};
		inline constexpr Icon AssetRenderPass{"\uf5fd"};
		inline constexpr Icon AssetVector{"\uf55b"};
		inline constexpr Icon AssetSkeleton{"\uf5d7"};
		inline constexpr Icon AssetSkin{"\uf0c1"};
		inline constexpr Icon AssetAnimationClip{"\uf008"};
		inline constexpr Icon AssetAnimatorController{"\uf542"};
		inline constexpr Icon AssetPhysicsMaterial{"\uf434"};
		inline constexpr Icon AssetAudioClip{"\uf1c7"};
		inline constexpr Icon AssetShaderSubgraph{"\ue157"};
		inline constexpr Icon AssetEffectGraph{"\ue900"};
		inline constexpr Icon AssetEffectOperator{"\ue901"};
		inline constexpr Icon AssetEffectBlock{"\ue902"};
		inline constexpr Icon AssetAudioMixer{"\ue903"};
		inline constexpr Icon AssetAudioConcurrency{"\ue904"};
		inline constexpr Icon AssetAudioRandomContainer{"\ue905"};
		inline constexpr Icon AssetAudioLayeredContainer{"\ue906"};
		inline constexpr Icon AssetAudioInteractiveContainer{"\ue907"};

		inline constexpr Icon SceneObject{"\uf1b2", Face::Regular};

		inline constexpr Icon UIElement{"\uf2d0"};

		inline constexpr Icon ToolMove{"\uf0b2"};
		inline constexpr Icon ToolRotate{"\uf2f9"};
		inline constexpr Icon ToolScale{"\uf31e"};
		inline constexpr Icon Grid{"\uf00a"};
		inline constexpr Icon Effects{"\uf72b"};
		inline constexpr Icon Gizmos{"\uf5cb"};
		inline constexpr Icon SpaceWorld{"\uf0ac"};
		inline constexpr Icon SpaceLocal{"\uf05b"};

		inline constexpr Icon Back{"\uf053"};
		inline constexpr Icon Forward{"\uf054"};
		inline constexpr Icon Folder{"\uf07b"};
		inline constexpr Icon FolderOpen{"\uf07c"};
		inline constexpr Icon Search{"\uf002"};
		inline constexpr Icon Add{"\uf067"};

		inline constexpr Icon More{"\uf142"};

		inline constexpr Icon Star{"\uf005"};

		inline constexpr Icon Picker{"\uf192", Face::Regular};

		inline constexpr Icon TextOverlay{"\uf035"};

		inline constexpr Icon Lock{""};
		inline constexpr Icon Unlock{""};

		inline constexpr Icon Eye{"\uf06e"};
		inline constexpr Icon EyeSlash{"\uf070"};

		inline constexpr Icon Duplicate{"\uf0c5"};
		inline constexpr Icon Delete{"\uf2ed"};
		inline constexpr Icon Remove{"\uf068"};
		inline constexpr Icon Import{"\uf56f"};
		inline constexpr Icon Export{"\uf56e"};

		inline constexpr Icon MoveUp{"\uf077"};
		inline constexpr Icon MoveDown{"\uf078"};

		inline constexpr Icon AlignStart{"\uf036"};
		inline constexpr Icon AlignCenter{"\uf037"};
		inline constexpr Icon AlignEnd{"\uf038"};
		inline constexpr Icon AlignStretch{"\uf039"};

		inline constexpr Icon PanelScene{"\ue5f1", Face::Regular};
		inline constexpr Icon PanelGame{"\uf8a9", Face::Regular};
		inline constexpr Icon PanelSettings{"\uf013", Face::Regular};
		inline constexpr Icon PanelHierarchy{"\ue479", Face::Regular};
		inline constexpr Icon PanelInspector{"\ue828", Face::Regular};
		inline constexpr Icon PanelAssets{"\uf660", Face::Regular};
		inline constexpr Icon PanelConsole{"\ue236", Face::Regular};
		inline constexpr Icon PanelProfiler{"\ue496", Face::Regular};
		inline constexpr Icon PanelBuild{"\uf187", Face::Regular};
		inline constexpr Icon PanelReport{"\ue4eb", Face::Regular};
		inline constexpr Icon PanelPackages{"\uf468", Face::Regular};

		inline constexpr Icon LevelTrace{"\uf111"};
		inline constexpr Icon LevelDebug{"\uf188"};
		inline constexpr Icon LevelInfo{"\uf05a"};
		inline constexpr Icon LevelWarn{"\uf071"};
		inline constexpr Icon LevelError{"\uf057"};
		inline constexpr Icon LevelFatal{"\uf06a"};

	} // namespace Glyph

	struct LogLevelFace {
		const char* tag;
		Icon icon;
		Color color;
	};

	SHALLTEAR_EDITOR_API LogLevelFace FaceForLogLevel(LogLevel level);

	SHALLTEAR_EDITOR_API f32 GutterSizePx();
	SHALLTEAR_EDITOR_API f32 BodySizePx();

	SHALLTEAR_EDITOR_API f32 TabSizePx();

	SHALLTEAR_EDITOR_API f32 GutterGapPx();

	SHALLTEAR_EDITOR_API f32 LineBoxHeight(f32 textSizePx);

	SHALLTEAR_EDITOR_API Handle<VectorAsset> GetGlyph(Icon icon);

	// font units, 13 px = 1 em
	SHALLTEAR_EDITOR_API f32 GlyphSizeScale(Icon icon);

	SHALLTEAR_EDITOR_API bool IsAvailable(Face face);

	SHALLTEAR_EDITOR_API void InstallWidgetMarks();

	// before UISystem::Terminate (icon atlas keys entries by these)
	SHALLTEAR_EDITOR_API void Shutdown();

	SHALLTEAR_EDITOR_API void SetGlyph(UI::Context& context, UI::ElementHandle element,
									   Icon icon, Color color, f32 sizePx = BodySizePx());

	SHALLTEAR_EDITOR_API bool SetGlyphOrText(UI::Context& context, UI::ElementHandle element,
											 Icon icon, std::string_view fallbackText,
											 Color color, f32 iconSizePx = BodySizePx(), f32 textSizePx = EditorTheme::GetMetric(EditorTheme::MetricToken::Font));

	SHALLTEAR_EDITOR_API UI::ElementHandle Add(UI::Context& context, UI::ElementHandle parent,
											   Icon icon, Color color,
											   f32 sizePx = BodySizePx(), f32 gapPx = 6.0f, f32 lineTextPx = 0.0f);

	SHALLTEAR_EDITOR_API UI::ElementHandle Prefix(UI::Context& context, UI::ElementHandle widget,
												  Icon icon, Color color,
												  f32 sizePx = BodySizePx(), f32 gapPx = 6.0f,
												  f32 lineTextPx = 0.0f);

	struct IconButtonParts {
		UI::ElementHandle button;
		UI::ElementHandle face;
	};

	SHALLTEAR_EDITOR_API IconButtonParts IconButton(UI::Context& context, UI::ElementHandle parent,
													Icon icon, std::string_view fallbackLabel,
													std::string_view tooltip, std::function<void()> onClick,
													f32 textSizePx = EditorTheme::GetMetric(EditorTheme::MetricToken::Font));

	namespace Mark {

		inline constexpr std::string_view Logo = "engine://Icons/Logo.svg";

		inline constexpr std::string_view Transform = "engine://Icons/Components/Transform.svg";
		inline constexpr std::string_view Camera = "engine://Icons/Components/Camera.svg";
		inline constexpr std::string_view Light = "engine://Icons/Components/Light.svg";
		inline constexpr std::string_view Decal = "engine://Icons/Components/Decal.svg";
		inline constexpr std::string_view MeshRenderer = "engine://Icons/Components/MeshRenderer.svg";
		inline constexpr std::string_view SkinnedMeshRenderer = "engine://Icons/Components/SkinnedMeshRenderer.svg";
		inline constexpr std::string_view Skeleton = "engine://Icons/Components/Skeleton.svg";
		inline constexpr std::string_view Environment = "engine://Icons/Components/Environment.svg";
		inline constexpr std::string_view ReflectionProbe = "engine://Icons/Components/ReflectionProbe.svg";
		inline constexpr std::string_view UIPanelRenderer = "engine://Icons/Components/UIPanelRenderer.svg";
		inline constexpr std::string_view ProceduralRenderer = "engine://Icons/Components/ProceduralRenderer.svg";
		inline constexpr std::string_view LODGroup = "engine://Icons/Components/LODGroup.svg";
		inline constexpr std::string_view ObjectShadow = "engine://Icons/Components/ObjectShadow.svg";
		inline constexpr std::string_view Behavior = "engine://Icons/Components/Behavior.svg";
		inline constexpr std::string_view Component = "engine://Icons/Components/Component.svg";
		inline constexpr std::string_view MissingComponent = "engine://Icons/Components/Missing.svg";

	} // namespace Mark

	namespace ComponentIcons {

		SHALLTEAR_EDITOR_API void RegisterBuiltIns();

		SHALLTEAR_EDITOR_API void Register(StringHash componentTypeName, std::string_view markPath,
										   const void* owner = nullptr);

		SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

		SHALLTEAR_EDITOR_API std::string_view For(const TypeInfo* componentType);

		SHALLTEAR_EDITOR_API UI::ElementHandle Add(UI::Context& context, UI::ElementHandle parent,
												   const TypeInfo* componentType,
												   f32 sizePx = 0.0f, f32 gapPx = 0.0f, f32 lineTextPx = 0.0f);

		SHALLTEAR_EDITOR_API f32 SizePx();

	} // namespace ComponentIcons

	SHALLTEAR_EDITOR_API Handle<VectorAsset> GetVector(std::string_view enginePath);

	SHALLTEAR_EDITOR_API UI::ElementHandle AddVector(UI::Context& context, UI::ElementHandle parent,
													 std::string_view enginePath, const UI::IconPalette& palette,
													 f32 sizePx = BodySizePx(), f32 gapPx = 6.0f, f32 lineTextPx = 0.0f);

	SHALLTEAR_EDITOR_API UI::ElementHandle SetRowGutter(UI::Context& context, const UI::Widgets::TreeRowParts& row,
														bool hasChildren, bool expanded,
														Color caretColor, Icon icon = {},
														Color iconColor = Color::White(), f32 columnPx = 0.0f);

	SHALLTEAR_EDITOR_API UI::ElementHandle SetRowGutterMark(UI::Context& context, const UI::Widgets::TreeRowParts& row,
															bool hasChildren, bool expanded, Color caretColor,
															std::string_view markPath, f32 sizePx, f32 columnPx = 0.0f);

} // namespace Shalltear::EditorIcons
