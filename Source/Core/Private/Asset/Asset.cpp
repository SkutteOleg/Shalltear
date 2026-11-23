#include "Shalltear/Core/Asset/Asset.h"

#include "Serialization/SerializerInternal.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Serialization/BinarySerializer.h"

namespace Shalltear {

	Asset::~Asset() = default;

	void Asset::NotifyUnloaded() {
		OnUnload();
		for (Handle<Asset>& held : _bound) {
			if (Asset* asset = held.Get()) {
				AssetDatabase::UnloadAsset(asset);
			}
		}
		_bound.clear();
		SetLoadState(AssetLoadState::Unloaded);
	}

	bool Asset::Cook(CookWriter& descriptor, CookWriter& payload) const {
		(void)payload;

		Vector<Guid> assetRefs{GetMemoryResource(MemTag::Asset)};
		Buffer blob = SerializerInternal::BinarySerialize(this, GetType(), &assetRefs);
		if (!blob) {
			return false;
		}
		descriptor.WriteBytes(blob.data, blob.size);
		for (const Guid& ref : assetRefs) {
			descriptor.NoteAssetRef(ref);
		}
		return true;
	}

	bool Asset::LoadCooked(CookReader& descriptor, CookReader& payload) {
		(void)payload;

		size_t size = descriptor.Remaining();
		if (size == 0) {
			return true;
		}
		const u8* bytes = descriptor.ReadSpan(size);
		if (!bytes) {
			return false;
		}

		Buffer owned(size, MemTag::Asset);
		std::memcpy(owned.data, bytes, size);
		return BinarySerializer::Deserialize(this, GetType(), owned);
	}

} // namespace Shalltear

SLT_REGISTER_TYPE(Shalltear::Asset)
