#pragma once

#include "CSharpComponentScenarios.h"
#include "CSharpFieldScenarios.h"

#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string_view>
#include <vector>

namespace Shalltear::CSharpScenarios {

	inline const TypeInfo* Script(const char* name) {
		return TypeRegistry::FindType(StringHash(str("Shalltear.Conformance.") + name));
	}

	struct World {
		Handle<Scene> scene = SceneSystem::CreateScene("Fields");

		~World() {
			std::vector<Handle<Scene>> loaded(SceneSystem::GetLoadedScenes().begin(), SceneSystem::GetLoadedScenes().end());
			for (Handle<Scene> each : loaded) {
				SceneSystem::UnloadScene(each);
			}
			SceneSystem::EndFrame();
		}

		Handle<Component> Add(const char* typeName, const char* objectName = "Host") {
			const TypeInfo* type = Script(typeName);
			return type ? SceneSystem::CreateObject(objectName, scene)->AddComponent(type) : Handle<Component>();
		}
	};

	inline Handle<Component> ScriptOn(Handle<SceneObject> object, const char* typeName) {
		const TypeInfo* type = Script(typeName);
		for (u32 index = 0; object.IsValid() && index < object->GetComponentCount(); ++index) {
			Handle<Component> component = object->GetComponentAt(index);
			if (component.IsValid() && component->GetType() == type) {
				return component;
			}
		}
		return {};
	}

	template <typename T>
	T Read(Handle<Component> component, const char* field) {
		Variant value;
		if (!component.IsValid() || !GetField(component.Get(), component->GetType(), StringHash(field), &value) || !value.Is<T>()) {
			ADD_FAILURE() << "field " << field << " didn't read";
			return T{};
		}
		return value.As<T>();
	}

	template <typename T>
	void Write(Handle<Component> component, const char* field, const T& value) {
		const Variant boxed(value);
		EXPECT_TRUE(component.IsValid() && SetField(component.Get(), component->GetType(), StringHash(field), &boxed)) << field;
	}

	inline std::vector<str> Frame(const FieldHost& host, u32 count = 1) {
		RunFrames(count);
		return host.takeRecords();
	}

	inline std::vector<str> Words(const str& row) {
		std::vector<str> words;
		std::istringstream stream(row);
		for (str word; stream >> word;) {
			words.push_back(word);
		}
		return words;
	}

	inline str Row(const std::vector<str>& rows, std::string_view prefix, u32 skip = 0) {
		for (const str& row : rows) {
			if (row.starts_with(prefix) && skip-- == 0) {
				return row;
			}
		}
		return {};
	}

	std::span<const FieldScenario> GetKindScenarios();

} // namespace Shalltear::CSharpScenarios
