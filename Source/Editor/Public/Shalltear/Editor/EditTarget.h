#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <type_traits>

namespace Shalltear {

	class SHALLTEAR_EDITOR_API EditTarget {
	public:
		EditTarget() = default;

		template <typename T, typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
		EditTarget(Handle<T> live) : _liveId(live.GetInstanceId()) {}

		static EditTarget PrefabRow(const Guid& prefabId, u64 rowKey);

		// after a write call NotifyWritten before another Resolve
		Object* Resolve() const;

		void NotifyWritten() const;

		bool IsPrefabRow() const { return _rowKey != 0; }
		bool operator==(const EditTarget& other) const;
	private:
		Object* Find() const;

		u64 _liveId = 0;
		Guid _prefabId{};
		u64 _rowKey = 0;
	};

} // namespace Shalltear
