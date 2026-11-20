#include "TestsPCH.h"

#include "CSharpConformance.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include <atomic>
#include <chrono>
#include <format>

namespace Shalltear::CSharpConformance {

	namespace {

		constexpr const char* WALK = R"({
"length": 1, "frameRate": 30, "loop": "Loop", "additiveReferenceTime": 0, "bakeRootMotionIntoPose": true,
"tracks": [
	{"kind": "Property", "interp": "Linear", "component": 0, "objectPath": "", "componentType": "", "name": "_localPosition",
	 "times": [0, 1], "values": [[0, 0, 0, 0], [2, 0, 0, 0]]},
	{"kind": "Event", "interp": "Step", "component": 255, "objectPath": "", "componentType": "", "name": "Footstep",
	 "times": [0.5], "values": [[1, 2, 3, 4]]}
]
})";

		str Controller(const Guid& clip) {
			return std::format(R"({{
"parameters": [
	{{"name": "Speed", "kind": 0, "default": 0}}, {{"name": "Lives", "kind": 1, "default": 3}},
	{{"name": "Grounded", "kind": 2, "default": 1}}, {{"name": "Jump", "kind": 3, "default": 0}}
],
"nodes": [{{"kind": 0, "clip": "{}", "parameterX": "", "parameterY": "", "children": []}}],
"machines": [{{"name": "Base", "parent": -1, "defaultState": 0, "anyState": []}}],
"states": [
	{{"name": "Idle", "motion": 0, "speed": 1, "speedParameter": "", "machine": 0, "transitions": [
		{{"destination": 1, "exitTime": -1, "duration": 0, "offset": 0, "syncPhase": false,
		 "conditions": [{{"parameter": "Speed", "mode": 0, "threshold": 0.5}}]}}]}},
	{{"name": "Run", "motion": 0, "speed": 1, "speedParameter": "", "machine": 0, "transitions": [
		{{"destination": 0, "exitTime": -1, "duration": 0, "offset": 0, "syncPhase": false,
		 "conditions": [{{"parameter": "Speed", "mode": 1, "threshold": 0.5}}]}}]}}
],
"layers": [{{"name": "Base", "machine": 0, "weight": 1, "blend": 0, "mask": []}}]
}})",
							   clip.ToString());
		}

	} // anonymous namespace

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
		if (AssetImporterRegistry::FindByExtension(".anim") && AssetImporterRegistry::FindByExtension(".controller")) {
			_staged = FileSystem::WriteTextFile(Path("project://Assets/Walk.anim"), WALK) && _staged;
			AssetDatabase::ScanProjectDirectory();
			const Guid walk = AssetDatabase::LookupGuid(Path("project://Assets/Walk.anim"));
			_staged = walk.IsValid() && FileSystem::WriteTextFile(Path("project://Assets/Hero.controller"), Controller(walk)) && _staged;
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
