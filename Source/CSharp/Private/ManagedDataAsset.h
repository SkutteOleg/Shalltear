#pragma once

#include "ManagedTypes.h"

#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Reflection.h"

#include <cstdint>

namespace Shalltear::CSharpImpl {

	class ManagedDataAsset final : public DataAsset {
	public:
		explicit ManagedDataAsset(ManagedType& type);
		~ManagedDataAsset() override;

		const TypeInfo* GetType() const override { return _type.info; }

		void ReleaseInstance();
		intptr_t GetInstance() const { return _instance; }
		const ManagedType& GetManagedType() const { return _type; }

		static void SyncFields(void* instance, FieldSync phase);
		void Pull();

		ManagedDataAsset* previous = nullptr;
		ManagedDataAsset* next = nullptr;
		ManagedDataAsset* dirtyPrevious = nullptr;
		ManagedDataAsset* dirtyNext = nullptr;
	private:
		void Push();

		ManagedType& _type;
		intptr_t _instance = 0;
		bool _dirty = false;
	};

} // namespace Shalltear::CSharpImpl
