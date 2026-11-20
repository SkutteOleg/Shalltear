#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <optional>
#include <span>
#include <string_view>

namespace Shalltear {

	namespace PackageOps {

		enum class PackageKind : u8 {
			Game = 0,
			Engine,
			Tool,
			Package,
		};

		struct PackageDependency {
			str name;
			str version;
		};

		struct PackageManifest {
			str name;
			str version;
			str displayName;
			str description;
			PackageKind kind = PackageKind::Package;
			bool required = false;
			bool scaffold = true;
			bool bridge = false;
			std::optional<FrameStage> stage;
			Vector<PackageDependency> dependencies;
			Path directory;
		};

		inline constexpr std::string_view ManifestFileName = "package.json";

		SHALLTEAR_EDITOR_API bool ParseManifest(std::string_view json, PackageManifest& out, str& error);

		SHALLTEAR_EDITOR_API bool LoadManifest(const Path& manifestFile, PackageManifest& out, str& error);

		SHALLTEAR_EDITOR_API std::string_view KindName(PackageKind kind);

		SHALLTEAR_EDITOR_API void DiscoverPackages(std::span<const Path> roots, Vector<PackageManifest>& out, Vector<str>& diagnostics);

		SHALLTEAR_EDITOR_API void ValidateCatalogue(Vector<PackageManifest>& catalogue, Vector<str>& diagnostics);

		SHALLTEAR_EDITOR_API const PackageManifest* FindPackage(std::span<const PackageManifest> catalogue, std::string_view name);

		struct ResolvedPackage {
			str name;
			bool requested = false;
			bool alwaysOn = false;
			Vector<str> requiredBy;
		};

		SHALLTEAR_EDITOR_API void ResolveEnabledSet(std::span<const PackageManifest> catalogue, std::span<const str> stored, Vector<ResolvedPackage>& out);

		SHALLTEAR_EDITOR_API bool IsEnabled(std::span<const ResolvedPackage> resolution, std::string_view name);

		SHALLTEAR_EDITOR_API bool CanDisable(std::span<const ResolvedPackage> resolution, std::string_view name, Vector<str>& blockers);

		SHALLTEAR_EDITOR_API void LoadBuiltInCatalogue(Vector<PackageManifest>& out, Vector<str>& diagnostics);

		SHALLTEAR_EDITOR_API str GetPackageBinaryName(const PackageManifest& manifest);

		SHALLTEAR_EDITOR_API bool FindUnloadedPackageImports(const Path& binary, std::span<const PackageManifest> catalogue,
															 std::span<const str> loaded, Vector<str>& out);

		// editor only Tool packages must never reach a player's link
		SHALLTEAR_EDITOR_API void SelectGamePackages(std::span<const PackageManifest> catalogue, std::span<const ResolvedPackage> resolution, Vector<str>& out);

		SHALLTEAR_EDITOR_API void DeclareSettings();

		inline constexpr std::string_view SettingsCategoryPath = "Packages";

		SHALLTEAR_EDITOR_API void ReadEnabledSet(Vector<str>& out);

		SHALLTEAR_EDITOR_API void WriteEnabledSet(std::span<const str> names);

		SHALLTEAR_EDITOR_API bool UpgradeStoredSet(std::span<const PackageManifest> catalogue);

		SHALLTEAR_EDITOR_API u32 ScaffoldEnabledSet(const Path& projectDocument, std::span<const str> optIn = {});

		SHALLTEAR_EDITOR_API u32 ScaffoldEnabledSet(const Path& projectDocument, std::span<const PackageManifest> catalogue,
													std::span<const str> optIn = {});

		SHALLTEAR_EDITOR_API void ListOptInPackages(std::span<const PackageManifest> catalogue, Vector<PackageManifest>& out);

		struct PackageRow {
			str name;
			str displayName;
			str version;
			str description;
			PackageKind kind = PackageKind::Package;
			bool enabled = false;
			bool requested = false;
			bool required = false;
			bool canToggle = false;
			Vector<str> blockedBy;
		};

		SHALLTEAR_EDITOR_API void BuildPackageRows(std::span<const PackageManifest> catalogue, std::span<const ResolvedPackage> resolution, Vector<PackageRow>& out);

		SHALLTEAR_EDITOR_API bool SetPackageEnabled(std::span<const PackageManifest> catalogue, std::string_view name, bool enabled, Vector<str>& blockers);

	} // namespace PackageOps

} // namespace Shalltear
