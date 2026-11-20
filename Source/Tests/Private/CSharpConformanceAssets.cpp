#include "TestsPCH.h"

#include "CSharpConformance.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include <atomic>
#include <chrono>

namespace Shalltear::CSharpConformance {

	StagedAssets::StagedAssets() {
		static std::atomic<u64> counter{0};
		const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
		_root = std::filesystem::temp_directory_path() / ("shalltear_csharp_conformance_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
		std::filesystem::create_directories(_root / "Assets");
		FileSystem::Mount("project", Path(_root.generic_string()));
		AssetDatabase::Initialize();

		Handle<Scene> level = SceneSystem::CreateScene("Level");
		Handle<SceneObject> tower = SceneSystem::CreateObject("Tower", level);
		SceneSystem::CreateObject("Flag", level, tower);
		SceneSystem::CreateObject("Gate", level);
		_staged = SceneSystem::SaveScene(level, Path("project://Assets/Level.scene"));
		SceneSystem::UnloadScene(level);

		Handle<Scene> bench = SceneSystem::CreateScene("Bench");
		Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", bench);
		SceneSystem::CreateObject("Barrel", bench, unit);
		if (const TypeInfo* carrier = TypeRegistry::FindType(StringHash("Shalltear.Conformance.Carrier"))) {
			Handle<Component> script = unit->AddComponent(carrier);
			const Variant cargo(static_cast<i32>(7));
			_staged = script.IsValid() && SetField(script.Get(), carrier, StringHash("cargo"), &cargo) && _staged;
		} else {
			_staged = false;
		}
		_staged = SceneSystem::SavePrefab(unit, Path("project://Assets/Unit.prefab")).IsValid() && _staged;
		SceneSystem::UnloadScene(bench);
		SceneSystem::EndFrame();
		if (AssetImporterRegistry::FindByExtension(".physmat")) {
			_staged = AssetDatabase::CreateAsset(Path("project://Assets/Surface.physmat"), StringHash("PhysicsMaterial")).IsValid() && _staged;
		}
		AssetDatabase::ScanProjectDirectory();
	}

	StagedAssets::~StagedAssets() {
		AssetDatabase::CollectGarbage();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

} // namespace Shalltear::CSharpConformance
