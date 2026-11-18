#include "TestsPCH.h"

#include "CSharpComponentScenarios.h"

#include "Shalltear/Core/Scene.h"

using namespace Shalltear;

namespace {

	std::vector<str> _twinRecords;

	class CSharpTwinRecorder : public Behavior {
	public:
		SLT_REFLECT_COMPONENT(CSharpTwinRecorder, Behavior)
		SLT_END_REFLECT()

		CSharpTwinRecorder() { _twinRecords.emplace_back("constructed"); }
	protected:
		void OnAttach() override { _twinRecords.emplace_back("attach"); }
		void OnDetach() override { _twinRecords.emplace_back("detach"); }
		void OnEnable() override { _twinRecords.emplace_back("enable"); }
		void OnDisable() override { _twinRecords.emplace_back("disable"); }
		void OnDestroy() override { _twinRecords.emplace_back("destroy"); }
		void OnUpdate() override { _twinRecords.emplace_back("update"); }
		void OnFixedUpdate() override { _twinRecords.emplace_back("fixed update"); }
		void OnLateUpdate() override { _twinRecords.emplace_back("late update"); }
	};

	SLT_REGISTER_TYPE(CSharpTwinRecorder)

} // anonymous namespace

namespace Shalltear::CSharpScenarios {

	const TypeInfo* GetTwinType() {
		return CSharpTwinRecorder::GetReflectType();
	}

	std::vector<str> TakeTwinRecords() {
		std::vector<str> taken;
		taken.swap(_twinRecords);
		return taken;
	}

	void RunFrames(u32 count) {
		for (u32 frame = 0; frame < count; ++frame) {
			SceneSystem::FixedUpdateAll();
			SceneSystem::UpdateAll();
			SceneSystem::LateUpdateAll();
			SceneSystem::EndFrame();
		}
	}

	void RunLifecycle(const TypeInfo* type) {
		Handle<Scene> world = SceneSystem::CreateScene("World");
		Handle<SceneObject> object = SceneSystem::CreateObject("Host", world);
		Handle<Component> component = object->AddComponent(type);
		if (!component.IsValid()) {
			SceneSystem::UnloadScene(world);
			SceneSystem::EndFrame();
			return;
		}
		RunFrames(2);
		component->SetEnabled(false);
		RunFrames(1);
		component->SetEnabled(true);
		RunFrames(1);
		SceneSystem::Destroy(object);
		SceneSystem::EndFrame();
		SceneSystem::UnloadScene(world);
		SceneSystem::EndFrame();
	}

	str Join(const std::vector<str>& records) {
		str joined;
		for (const str& record : records) {
			joined += joined.empty() ? "" : "|";
			joined += record;
		}
		return joined;
	}

} // namespace Shalltear::CSharpScenarios
