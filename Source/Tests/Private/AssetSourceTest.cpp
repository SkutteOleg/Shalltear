#include <gtest/gtest.h>

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/StyleSheetAsset.h"
#include "Shalltear/Core/UI/UIDocument.h"

#include "Shalltear/Editor/CookOps.h"

#include "LZ4Encode.h"

#include <atomic>
#include <cstring>
#include <filesystem>

using namespace Shalltear;

namespace {

	class AssetSourceFixture : public ::testing::Test {
	protected:
		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 id = counter.fetch_add(1);
			std::filesystem::path base = std::filesystem::temp_directory_path() /
										 ("shalltear_assetsource_" + std::to_string(id));
			_source = base / "source";
			_staged = base / "staged";
			std::error_code ec;
			std::filesystem::remove_all(base, ec);
			std::filesystem::create_directories(_source / "Assets", ec);
			std::filesystem::create_directories(_staged, ec);
			OpenSourceProject();
		}

		void TearDown() override {
			CloseProject();
			std::error_code ec;
			std::filesystem::remove_all(_source.parent_path(), ec);
		}

		void OpenSourceProject() {
			FileSystem::Mount("project", Path(_source.generic_string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			_open = true;
		}

		void CloseProject() {
			if (!_open) {
				return;
			}
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			_open = false;
		}

		void OpenStagedTree() {
			FileSystem::Mount("project", Path(_staged.generic_string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			_open = true;
		}

		void StageText(const char* relative, std::string_view text) const {
			std::filesystem::path at = _source / relative;
			std::error_code ec;
			std::filesystem::create_directories(at.parent_path(), ec);
			ASSERT_TRUE(FileSystem::WriteTextFile(Path(at.generic_string()), str(text)));
		}

		Guid GuidOf(const char* virtualPath) const { return AssetDatabase::LookupGuid(Path(virtualPath)); }

		void CookInto(const Vector<Guid>& guids, const char* name = "Content.slpak", bool shipDebugNames = true) {
			CookOps::PakWriteDesc desc;
			desc.shipDebugNames = shipDebugNames;
			Vector<u8> package{GetMemoryResource(MemTag::Editor)};
			str error;
			ASSERT_TRUE(CookOps::CookPackage(CookOps::CookClosure::Of(guids), desc, package, error)) << error;
			WritePackage(package, name);
		}

		void WritePackage(const Vector<u8>& package, const char* name) const {
			Buffer buffer(package.size(), MemTag::Editor);
			std::memcpy(buffer.data, package.data(), package.size());
			ASSERT_TRUE(FileSystem::WriteFile(Path((_staged / name).generic_string()), buffer));
		}

		Path StagedPackage(const char* name = "Content.slpak") const {
			return Path((_staged / name).generic_string());
		}

		std::filesystem::path _source;
		std::filesystem::path _staged;
		bool _open = false;
	};

	class AsProbeComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(AsProbeComponent, Component)
		SLT_REFLECT_FIELD(charges)
		SLT_END_REFLECT()

		i32 charges = 0;
	};
	SLT_REGISTER_TYPE(AsProbeComponent)

} // anonymous namespace

TEST_F(AssetSourceFixture, GuidResolvesFromPackageOnly) {
	StageText("Assets/Readme.txt", "cooked, not imported");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = GuidOf("project://Assets/Readme.txt");
	ASSERT_TRUE(guid.IsValid());

	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Asset* loaded = AssetDatabase::LoadAssetByGuid(guid);
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(loaded->GetAssetTypeId(), StringHash("TextAsset"));
	EXPECT_EQ(static_cast<TextAsset*>(loaded)->content, "cooked, not imported");

	EXPECT_EQ(loaded->assetId, guid);
	EXPECT_EQ(loaded->virtualPath, "project://Assets/Readme.txt");
	EXPECT_EQ(loaded->GetLoadState(), AssetLoadState::Loaded);

	Asset* again = AssetDatabase::LoadAssetByGuid(guid);
	EXPECT_EQ(again, loaded);
	EXPECT_EQ(loaded->GetRefCount(), 2);
	EXPECT_EQ(AssetDatabase::LoadAsset(Path("project://Assets/Readme.txt")), loaded);

	AssetDatabase::UnloadAsset(loaded);
	AssetDatabase::UnloadAsset(again);
	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(AssetSourceFixture, MountedPackageSwappedForUpdate) {
	StageText("Assets/Readme.txt", "first release");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = GuidOf("project://Assets/Readme.txt");
	ASSERT_TRUE(guid.IsValid());
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids);
	CloseProject();

	OpenSourceProject();
	StageText("Assets/Readme.txt", "second release");
	AssetDatabase::ScanProjectDirectory();
	ASSERT_EQ(GuidOf("project://Assets/Readme.txt"), guid);
	CookInto(guids, "Update.slpak");
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;
	const std::filesystem::path live = _staged / "Content.slpak";
	const std::filesystem::path aside = _staged / "Content.slpak.old";
	std::error_code ec;
	std::filesystem::rename(live, aside, ec);
	ASSERT_FALSE(ec);
	std::filesystem::rename(_staged / "Update.slpak", live, ec);
	ASSERT_FALSE(ec);
	EXPECT_TRUE(std::filesystem::remove(aside, ec));

	Asset* loaded = AssetDatabase::LoadAssetByGuid(guid);
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(static_cast<TextAsset*>(loaded)->content, "first release");
	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(AssetSourceFixture, DocumentIdentityFromSourceNotRecord) {
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> root = SceneSystem::CreateObject("Root", scene);
	root->AddComponent<AsProbeComponent>()->charges = 7;
	ASSERT_TRUE(SceneSystem::SaveScene(scene, Path("project://Assets/Level.scene"), false));
	AssetDatabase::ScanProjectDirectory();

	Guid guid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(guid.IsValid());
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Asset* loaded = AssetDatabase::LoadAssetByGuid(guid);
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(loaded->assetId, guid);
	EXPECT_EQ(loaded->virtualPath, "project://Assets/Level.scene");
	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(AssetSourceFixture, PackagedSceneRunsByGuid) {
	Handle<Scene> authored = SceneSystem::CreateScene("Level");
	Handle<SceneObject> parent = SceneSystem::CreateObject("Parent", authored);
	parent->SetLocalPosition(Vec3(4.0f, 5.0f, 6.0f));
	Handle<SceneObject> child = SceneSystem::CreateObject("Child", authored, parent);
	child->AddComponent<AsProbeComponent>()->charges = 11;
	ASSERT_TRUE(SceneSystem::SaveScene(authored, Path("project://Assets/Level.scene"), false));
	AssetDatabase::ScanProjectDirectory();

	Guid guid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(guid.IsValid());
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Handle<Scene> scene = SceneSystem::LoadSceneByGuid(guid);
	ASSERT_TRUE(scene.IsValid());
	EXPECT_EQ(scene->name, "Level");
	ASSERT_EQ(scene->GetRootCount(), 1u);

	Handle<SceneObject> loadedParent = scene->GetRoot(0);
	EXPECT_EQ(loadedParent->name, "Parent");
	EXPECT_TRUE(loadedParent->GetLocalPosition() == Vec3(4.0f, 5.0f, 6.0f));
	ASSERT_EQ(loadedParent->GetChildCount(), 1u);

	Handle<AsProbeComponent> probe = loadedParent->GetChild(0)->GetComponent<AsProbeComponent>();
	ASSERT_TRUE(probe.IsValid());
	EXPECT_EQ(probe->charges, 11);

	SceneSystem::BeginFrame();
	SceneSystem::UpdateAll();
	SceneSystem::EndFrame();
	EXPECT_EQ(probe->charges, 11);
}

TEST_F(AssetSourceFixture, StrippedSceneRunsAnonymous) {
	Handle<Scene> authored = SceneSystem::CreateScene("Level");
	SceneSystem::CreateObject("Parent", authored)->AddComponent<AsProbeComponent>()->charges = 7;
	ASSERT_TRUE(SceneSystem::SaveScene(authored, Path("project://Assets/Level.scene"), false));
	AssetDatabase::ScanProjectDirectory();

	Guid guid = GuidOf("project://Assets/Level.scene");
	ASSERT_TRUE(guid.IsValid());
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids, "Content.slpak", false);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Handle<Scene> scene = SceneSystem::LoadSceneByGuid(guid);
	ASSERT_TRUE(scene.IsValid());
	EXPECT_EQ(scene->name, "");

	ASSERT_EQ(scene->GetRootCount(), 1u);
	EXPECT_EQ(scene->GetRoot(0)->name, "Parent");
	Handle<AsProbeComponent> probe = scene->GetRoot(0)->GetComponent<AsProbeComponent>();
	ASSERT_TRUE(probe.IsValid());
	EXPECT_EQ(probe->charges, 7);
}

TEST_F(AssetSourceFixture, CookedDocumentsSheetAndTemplateHandlesResolve) {
	StageText("Assets/Main.slss", ".row { flex-direction: row; }\n");
	StageText("Assets/Portrait.slxml", R"(<ui version="1">
	<panel id="portrait"/>
</ui>)");
	StageText("Assets/Hud.slxml", R"(<ui version="1">
	<sheet src="project://Assets/Main.slss"/>
	<template src="project://Assets/Portrait.slxml" name="Portrait"/>
	<panel id="root">
		<instance template="Portrait"/>
	</panel>
</ui>)");
	AssetDatabase::ScanProjectDirectory();

	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	for (const char* path : {"project://Assets/Main.slss", "project://Assets/Portrait.slxml",
							 "project://Assets/Hud.slxml"}) {
		Guid guid = GuidOf(path);
		ASSERT_TRUE(guid.IsValid()) << path;
		guids.push_back(guid);
	}
	Guid hud = GuidOf("project://Assets/Hud.slxml");
	Guid portrait = GuidOf("project://Assets/Portrait.slxml");
	CookInto(guids);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Asset* loaded = AssetDatabase::LoadAssetByGuid(hud);
	ASSERT_NE(loaded, nullptr);
	UIDocumentAsset* document = static_cast<UIDocumentAsset*>(loaded);

	ASSERT_EQ(document->sheets.size(), 1u);
	EXPECT_TRUE(document->sheets[0].sheet.IsValid());
	ASSERT_EQ(document->templates.size(), 1u);
	ASSERT_TRUE(document->templates[0].document.IsValid());
	EXPECT_EQ(document->templates[0].document->assetId, portrait);

	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(AssetSourceFixture, PackageAnswersPathForHeldGuid) {
	StageText("Assets/Readme.txt", "the cooked bytes");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = GuidOf("project://Assets/Readme.txt");
	ASSERT_TRUE(guid.IsValid());
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(guid);
	CookInto(guids);

	StageText("Assets/Readme.txt", "the loose bytes");
	CloseProject();

	OpenSourceProject();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;
	AssetDatabase::ScanProjectDirectory();

	Asset* loaded = AssetDatabase::LoadAsset(Path("project://Assets/Readme.txt"));
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(static_cast<TextAsset*>(loaded)->content, "the cooked bytes");
	AssetDatabase::UnloadAsset(loaded);
}

TEST_F(AssetSourceFixture, GuidPackageDoesNotHoldStillImports) {
	StageText("Assets/Cooked.txt", "in the package");
	StageText("Assets/Loose.txt", "not in the package");
	AssetDatabase::ScanProjectDirectory();
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(GuidOf("project://Assets/Cooked.txt"));
	CookInto(guids);
	CloseProject();

	OpenSourceProject();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	Asset* loose = AssetDatabase::LoadAsset(Path("project://Assets/Loose.txt"));
	ASSERT_NE(loose, nullptr);
	EXPECT_EQ(static_cast<TextAsset*>(loose)->content, "not in the package");
	AssetDatabase::UnloadAsset(loose);
}

TEST_F(AssetSourceFixture, SecondMountIsRefusedAndNamesFirst) {
	StageText("Assets/Readme.txt", "one");
	AssetDatabase::ScanProjectDirectory();
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(GuidOf("project://Assets/Readme.txt"));
	CookInto(guids);
	CookInto(guids, "Second.slpak");
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;
	EXPECT_FALSE(AssetDatabase::MountPackage(StagedPackage("Second.slpak"), error));
	EXPECT_NE(error.find("Content.slpak"), str::npos);
}

TEST_F(AssetSourceFixture, MountRefusesUnknownRecordVersion) {
	CookOps::PakAssetSource source;
	source.guid = Guid(0x9001, 0x9002);
	source.typeNameHash = StringHash("TextAsset").GetValue();
	source.recordVersion = PAK_RECORD_VERSION + 1;
	source.name = "project://Assets/FromTheFuture.txt";

	CookOps::PakWriteDesc desc;
	Vector<CookOps::PakAssetSource> sources{GetMemoryResource(MemTag::Editor)};
	sources.push_back(std::move(source));
	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	str error;
	ASSERT_TRUE(CookOps::BuildPackage(sources, desc, package, error)) << error;
	WritePackage(package, "Future.slpak");
	CloseProject();

	OpenStagedTree();
	EXPECT_FALSE(AssetDatabase::MountPackage(StagedPackage("Future.slpak"), error));
	EXPECT_NE(error.find("record version"), str::npos) << error;
}

TEST_F(AssetSourceFixture, PackageHandsOverOnlyCarried) {
	auto writePackage = [this](const char* name, bool withSettings) {
		CookOps::PakAssetSource source;
		source.guid = Guid(0x9101, 0x9102);
		source.typeNameHash = StringHash("TextAsset").GetValue();
		source.recordVersion = PAK_RECORD_VERSION;
		Vector<CookOps::PakAssetSource> sources{GetMemoryResource(MemTag::Editor)};
		sources.push_back(std::move(source));

		CookOps::PakWriteDesc desc;
		desc.documents.push_back({PakDocumentKind::BootManifest, "{\"formatVersion\": 2}"});
		if (withSettings) {
			desc.documents.push_back({PakDocumentKind::ProjectSettings, "[Time]\nfixedDeltaTime = 0.01\n"});
		}
		Vector<u8> package{GetMemoryResource(MemTag::Editor)};
		str error;
		ASSERT_TRUE(CookOps::BuildPackage(sources, desc, package, error)) << error;
		WritePackage(package, name);
	};
	writePackage("Both.slpak", true);
	writePackage("ManifestOnly.slpak", false);
	CloseProject();

	OpenStagedTree();
	str text = "left over from the caller";
	EXPECT_FALSE(AssetDatabase::ReadPackageDocument(PakDocumentKind::BootManifest, text));
	EXPECT_TRUE(text.empty());

	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage("Both.slpak"), error)) << error;
	ASSERT_TRUE(AssetDatabase::ReadPackageDocument(PakDocumentKind::BootManifest, text));
	EXPECT_EQ(text, "{\"formatVersion\": 2}");
	ASSERT_TRUE(AssetDatabase::ReadPackageDocument(PakDocumentKind::ProjectSettings, text));
	EXPECT_EQ(text, "[Time]\nfixedDeltaTime = 0.01\n");
	CloseProject();

	OpenStagedTree();
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage("ManifestOnly.slpak"), error)) << error;
	text = "left over from the caller";
	EXPECT_FALSE(AssetDatabase::ReadPackageDocument(PakDocumentKind::ProjectSettings, text));
	EXPECT_TRUE(text.empty());
	EXPECT_TRUE(AssetDatabase::ReadPackageDocument(PakDocumentKind::BootManifest, text));
}

TEST_F(AssetSourceFixture, WritePathsRefuseWhilePackageIsMounted) {
	StageText("Assets/Readme.txt", "one");
	AssetDatabase::ScanProjectDirectory();
	Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
	guids.push_back(GuidOf("project://Assets/Readme.txt"));
	CookInto(guids);
	CloseProject();

	OpenStagedTree();
	str error;
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage(), error)) << error;

	EXPECT_FALSE(AssetDatabase::CreateAsset(Path("project://Assets/New.txt"), StringHash("TextAsset")).IsValid());
	EXPECT_FALSE(FileSystem::Exists(Path((_staged / "Assets/New.txt").generic_string())));

	TextAsset asset;
	asset.virtualPath = "project://Assets/Saved.txt";
	asset.content = "should not reach disk";
	AssetDatabase::SaveAsset(&asset);
	EXPECT_FALSE(FileSystem::Exists(Path((_staged / "Assets/Saved.txt").generic_string())));
}

TEST_F(AssetSourceFixture, ManagedBundleRoundTripsRawAndLZ4) {
	CookOps::PakAssetSource source;
	source.guid = Guid(0x9201, 0x9202);
	source.typeNameHash = StringHash("TextAsset").GetValue();
	source.recordVersion = PAK_RECORD_VERSION;
	Vector<CookOps::PakAssetSource> sources{GetMemoryResource(MemTag::Editor)};
	sources.push_back(std::move(source));

	const str nameA = "Tiny.dll";
	const str rawA = "MZ_RAW";
	const str nameB = "Stripped.dll";
	str rawB(4096, '\0');
	std::memcpy(rawB.data(), "MZ_STRIPPED", 11);
	std::memcpy(rawB.data() + 4080, "TAIL_BYTES_1234", 15);

	Vector<u8> compB{GetMemoryResource(MemTag::Editor)};
	LZ4::Compress(reinterpret_cast<const u8*>(rawB.data()), rawB.size(), LZ4::Effort::Fast, compB);
	ASSERT_LT(compB.size(), rawB.size());

	const u32 tableEnd = static_cast<u32>(sizeof(PakManagedBundleHeader) + 2 * sizeof(PakManagedBundleEntry));
	const u32 totalBytes = tableEnd + static_cast<u32>(nameA.size() + rawA.size() + nameB.size() + compB.size());
	str bundle(totalBytes, '\0');
	u8* dst = reinterpret_cast<u8*>(bundle.data());

	PakManagedBundleHeader header{PAK_MANAGED_BUNDLE_MAGIC, 2};
	std::memcpy(dst, &header, sizeof(header));

	u32 cursor = tableEnd;
	PakManagedBundleEntry entries[2]{};
	entries[0].nameOffset = cursor;
	entries[0].nameSize = static_cast<u32>(nameA.size());
	std::memcpy(dst + cursor, nameA.data(), nameA.size());
	cursor += entries[0].nameSize;
	entries[0].dataOffset = cursor;
	entries[0].rawSize = static_cast<u32>(rawA.size());
	entries[0].storedSize = static_cast<u32>(rawA.size());
	entries[0].codec = static_cast<u8>(PakCodec::None);
	std::memcpy(dst + cursor, rawA.data(), rawA.size());
	cursor += entries[0].storedSize;

	entries[1].nameOffset = cursor;
	entries[1].nameSize = static_cast<u32>(nameB.size());
	std::memcpy(dst + cursor, nameB.data(), nameB.size());
	cursor += entries[1].nameSize;
	entries[1].dataOffset = cursor;
	entries[1].rawSize = static_cast<u32>(rawB.size());
	entries[1].storedSize = static_cast<u32>(compB.size());
	entries[1].codec = static_cast<u8>(PakCodec::LZ4);
	std::memcpy(dst + cursor, compB.data(), compB.size());

	std::memcpy(dst + sizeof(PakManagedBundleHeader), entries, sizeof(entries));

	CookOps::PakWriteDesc desc;
	desc.documents.push_back({PakDocumentKind::ManagedBundle, bundle});
	Vector<u8> package{GetMemoryResource(MemTag::Editor)};
	str error;
	ASSERT_TRUE(CookOps::BuildPackage(sources, desc, package, error)) << error;
	WritePackage(package, "Managed.slpak");
	CloseProject();

	OpenStagedTree();
	ASSERT_TRUE(AssetDatabase::MountPackage(StagedPackage("Managed.slpak"), error)) << error;

	struct Seen {
		Vector<str> names{GetMemoryResource(MemTag::Core)};
		Vector<str> payloads{GetMemoryResource(MemTag::Core)};
	} seen;
	ASSERT_TRUE(AssetDatabase::ReadManagedBundle(
		[](const char* name, const u8* data, u32 size, void* user) {
			auto* out = static_cast<Seen*>(user);
			out->names.emplace_back(name);
			out->payloads.emplace_back(reinterpret_cast<const char*>(data), size);
		},
		&seen));
	ASSERT_EQ(seen.names.size(), 2u);
	EXPECT_EQ(seen.names[0], nameA);
	EXPECT_EQ(seen.payloads[0], rawA);
	EXPECT_EQ(seen.names[1], nameB);
	EXPECT_EQ(seen.payloads[1], rawB);
}
