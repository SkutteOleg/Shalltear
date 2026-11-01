#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <functional>
#include <span>
#include <string_view>

namespace Shalltear {

	class SceneObject;

} // namespace Shalltear

namespace Shalltear::AssetBrowserOps {

	enum class EntryKind : u8 {
		Directory,
		File,
		Product,
		Link,
	};

	struct Entry {
		str name;
		str relPath;
		u32 depth = 0;
		EntryKind kind = EntryKind::File;
		u64 id = 0;
		Guid guid;
		str productKey;
		StringHash assetType;
		str sourceRelPath;
		str extractedRelPath;

		bool IsDirectory() const noexcept { return kind == EntryKind::Directory; }
	};

	SHALLTEAR_EDITOR_API bool IsProjectMounted();

	SHALLTEAR_EDITOR_API Path GetProjectRootPhysical();

	SHALLTEAR_EDITOR_API bool ScanTree(Vector<Entry>& out);

	SHALLTEAR_EDITOR_API void FolderContents(const Vector<Entry>& tree, std::string_view folderRelPath,
											 std::string_view filter, Vector<Entry>& out);

	SHALLTEAR_EDITOR_API u64 EntryKeyForPath(std::string_view relPath);

	SHALLTEAR_EDITOR_API void ScanProducts(const str& relPath, u32 depth, Vector<Entry>& out);

	SHALLTEAR_EDITOR_API void ScanLinks(const str& relPath, u32 depth, Vector<Entry>& out);

	struct AssignableAsset {
		str display;
		Guid guid;
	};
	SHALLTEAR_EDITOR_API void CollectAssignableAssets(StringHash type, Vector<AssignableAsset>& out);

	enum class EntryCommand : u8 {
		Rename,
		Delete,
		Move,
		Duplicate,
		Extract,
	};

	SHALLTEAR_EDITOR_API str RefusalFor(const Entry& entry, EntryCommand command);

	SHALLTEAR_EDITOR_API str ExtractProduct(const Entry& entry);

	SHALLTEAR_EDITOR_API bool RevealEntry(const Entry& entry);

	SHALLTEAR_EDITOR_API Path GetDefaultCreateFolder();

	SHALLTEAR_EDITOR_API bool CreateFolder(const Path& virtualDir, const str& name);

	SHALLTEAR_EDITOR_API bool RenameEntry(const Path& virtualPath, const str& newName);

	SHALLTEAR_EDITOR_API str RenameStem(std::string_view fileName);

	SHALLTEAR_EDITOR_API str RenamedFileName(std::string_view fileName, std::string_view typed);

	SHALLTEAR_EDITOR_API const Entry* ResolveTypedPath(std::span<const Entry> entries, std::string_view typed);

	class SHALLTEAR_EDITOR_API FolderHistory {
	public:
		static constexpr u32 CAPACITY = 50;

		void Visit(std::string_view folder);

		str PeekBack(const std::function<bool(std::string_view)>& exists) const;
		str PeekForward(const std::function<bool(std::string_view)>& exists) const;
		str Back(const std::function<bool(std::string_view)>& exists);
		str Forward(const std::function<bool(std::string_view)>& exists);

		void Remap(std::string_view from, std::string_view to);

		std::string_view Current() const;
	private:
		size_t Step(int direction, const std::function<bool(std::string_view)>& exists) const;

		Vector<str> _visits{GetMemoryResource(MemTag::Editor)};
		size_t _index = 0;
	};

	SHALLTEAR_EDITOR_API bool DeleteEntry(const Path& virtualPath);

	SHALLTEAR_EDITOR_API str MakeUniqueName(const Path& virtualDir, const str& desiredName);

	SHALLTEAR_EDITOR_API Guid CreateAsset(const Path& virtualDir, StringHash assetTypeId, const str& desiredName = {},
										  str* outRelPath = nullptr, u32 templateIndex = 0);

	SHALLTEAR_EDITOR_API bool CreateCSharpScript(const Path& virtualDir, const str& className,
												 str* outRelPath = nullptr);

	SHALLTEAR_EDITOR_API StringHash ResolveScriptComponentType(std::string_view relPath);

	struct CreateMenuEntry {
		str label;
		StringHash typeId;
		str fileName;
		u32 templateIndex = 0;
	};

	SHALLTEAR_EDITOR_API void GetCreateMenuEntries(Vector<CreateMenuEntry>& out);

	SHALLTEAR_EDITOR_API Guid CreatePrefabFromObject(Handle<SceneObject> root, const Path& virtualDir,
													 str* outRelPath = nullptr);

	SHALLTEAR_EDITOR_API bool CanCreatePrefabVariant(const Path& virtualPath);

	SHALLTEAR_EDITOR_API bool IsPrefabVariant(const str& relPath);

	SHALLTEAR_EDITOR_API Guid CreatePrefabVariant(const Path& virtualPath, str* outRelPath = nullptr);

	SHALLTEAR_EDITOR_API Guid CreatePrefabVariantFromObject(Handle<SceneObject> root, const Path& virtualDir,
															str* outRelPath = nullptr);

	SHALLTEAR_EDITOR_API bool MoveEntry(const Path& virtualPath, const Path& virtualDestDir);

	SHALLTEAR_EDITOR_API str DuplicateEntry(const Path& virtualPath);

	SHALLTEAR_EDITOR_API str ImportExternalFile(const Path& physicalSource, const Path& virtualDestDir);

	SHALLTEAR_EDITOR_API bool RevealInExplorer(const Path& virtualPath);

	SHALLTEAR_EDITOR_API bool OpensInExternalEditor(std::string_view extension);

	SHALLTEAR_EDITOR_API bool OpenInExternalEditor(const Path& physicalPath, u32 line = 0);

	SHALLTEAR_EDITOR_API bool MatchesFilter(std::string_view text, std::string_view filter);

	struct GridCellSize {
		f32 widthPx = 0.0f;
		f32 heightPx = 0.0f;
	};
	SHALLTEAR_EDITOR_API GridCellSize CellSizeForSlider(f32 sliderValue, f32 minSidePx, f32 maxSidePx,
														f32 labelBandPx);

	inline constexpr f32 SLIDER_SMALLEST_GRID = 0.06f;

	SHALLTEAR_EDITOR_API bool SliderIsGrid(f32 sliderValue);

	SHALLTEAR_EDITOR_API f32 BadgeGlyphSizeFor(f32 wellPx);

	SHALLTEAR_EDITOR_API u32 GridColumnsFor(f32 viewportWidthPx, f32 cellWidthPx);

	struct GridWindow {
		u32 firstIndex = 0;
		u32 count = 0;
	};
	SHALLTEAR_EDITOR_API GridWindow VisibleGridWindow(u32 itemCount, u32 columns, f32 cellHeightPx, f32 scrollYPx,
													  f32 viewportHeightPx);

	struct FoldOutSegment {
		u32 first = 0;
		u32 last = 0;
		bool fromAbove = false;
		bool continuesBelow = false;
	};
	SHALLTEAR_EDITOR_API void FoldOutBandSegments(std::span<const u32> depths, u32 columns,
												  Vector<FoldOutSegment>& out);

	SHALLTEAR_EDITOR_API f32 FoldButtonSizeFor(f32 cellSidePx, f32 maxCellSidePx, f32 smallPx, f32 mediumPx,
											   f32 largePx);

} // namespace Shalltear::AssetBrowserOps
