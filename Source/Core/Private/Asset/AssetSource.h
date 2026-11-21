#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include "Asset/PakReader.h"

namespace Shalltear {

	class Asset;

	class IAssetSource {
	public:
		virtual ~IAssetSource() = default;

		virtual const char* GetSourceName() const = 0;

		virtual bool Contains(const Guid& guid) const = 0;

		virtual Asset* Load(const Guid& guid) = 0;
	};

	class PackageAssetSource final : public IAssetSource {
	public:
		bool Mount(const Path& physicalPath, str& outError);

		// only Terminate, after the last asset is destroyed
		void Unmount() {
			_reader.Unmount();
			_path.clear();
		}

		bool IsMounted() const { return _reader.IsMounted(); }
		const str& GetPackagePath() const { return _path; }

		std::string_view GetDocument(PakDocumentKind kind) const { return _reader.GetDocument(kind); }

		const char* GetSourceName() const override { return "package"; }
		bool Contains(const Guid& guid) const override {
			return _reader.IsMounted() && _reader.FindAsset(guid) != PAK_INVALID_INDEX;
		}
		Asset* Load(const Guid& guid) override;

		void FindOfType(StringHash typeId, Vector<Guid>& out) const;
	private:
		PakReader _reader;
		str _path;
	};

} // namespace Shalltear
