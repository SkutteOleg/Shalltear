#include "Binder.h"

#include "Shalltear/Animation/AnimationClip.h"

#include "Shalltear/Core/Reflection/Walk.h"

#include <cstring>

namespace Shalltear {

	namespace AnimationImpl {

		Handle<SceneObject> ResolvePath(Handle<SceneObject> root, std::string_view path) {
			Handle<SceneObject> at = root;
			size_t start = 0;
			while (start < path.size()) {
				const size_t slash = path.find('/', start);
				const std::string_view segment =
					slash == std::string_view::npos ? path.substr(start) : path.substr(start, slash - start);
				if (!segment.empty()) {
					SceneObject* object = at.Get();
					if (!object) {
						return {};
					}
					at = object->FindChildByName(segment);
					if (!at.IsValid()) {
						return {};
					}
				}
				if (slash == std::string_view::npos) {
					break;
				}
				start = slash + 1;
			}
			return at;
		}

		Handle<Component> FindComponentOfType(SceneObject* object, const TypeInfo* type) {
			const u32 count = object->GetComponentCount();
			for (u32 i = 0; i < count; ++i) {
				Handle<Component> candidate = object->GetComponentAt(i);
				Component* resolved = candidate.Get();
				if (resolved && TypeRegistry::IsDerivedFrom(resolved->GetType(), type)) {
					return candidate;
				}
			}
			return {};
		}

		u32 ScalarCount(const TypeInfo* type) {
			if (!type) {
				return 0;
			}
			const StringHash name = type->name;
			if (name == StringHash("f32")) {
				return 1;
			}
			if (name == StringHash("Vec2")) {
				return 2;
			}
			if (name == StringHash("Vec3")) {
				return 3;
			}
			if (name == StringHash("Vec4") || name == StringHash("Quat") || name == StringHash("Color")) {
				return 4;
			}
			return 0;
		}

		bool WriteAnimatedField(void* instance, const FieldInfo* field, const Vec4& value, u8 component) {
			const u32 scalars = ScalarCount(field->fieldType);
			if (scalars == 0 || field->addrFn == nullptr) {
				return false;
			}
			void* at = field->addrFn(instance);
			if (!at || (component != ANIMATION_WHOLE_VALUE && component >= scalars)) {
				return false;
			}

			SyncFieldsBeforeRead(instance, field->ownerType);
			f32 channels[4] = {value.x, value.y, value.z, value.w};
			if (component != ANIMATION_WHOLE_VALUE) {
				std::memcpy(channels, at, scalars * sizeof(f32));
				channels[component] = value.x;
			}
			std::memcpy(at, channels, scalars * sizeof(f32));
			SyncFieldsAfterWrite(instance, field->ownerType);
			return true;
		}

		bool IsCallableFromClip(const TypeInfo* type, StringHash name) {
			const MethodInfo* method = type ? FindMethod(type, name) : nullptr;
			return method != nullptr && method->invokeFn != nullptr && method->paramCount == 0;
		}

		bool CallAnimatedMethod(void* instance, const TypeInfo* type, StringHash name) {
			if (!IsCallableFromClip(type, name)) {
				return false;
			}
			return InvokeMethod(instance, type, name, nullptr, 0, nullptr);
		}

	} // namespace AnimationImpl

} // namespace Shalltear
