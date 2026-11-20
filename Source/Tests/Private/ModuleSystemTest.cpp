#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RenderPass.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Object.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#undef CreateDirectory
#undef GetFileSize

using namespace Shalltear;

namespace {

	constexpr const char* SAMPLE_MODULE_DLL = "ShalltearSampleModule" SLT_TEST_DLL_SUFFIX;

	using GetTypeCountFn = u32 (*)();
	using BoxPayloadFn = void (*)(Variant*, i32);
	using ReadPayloadFn = i32 (*)(const Variant*);
	using AllocateBlockFn = size_t (*)(size_t, void**);
	using FreeBlockFn = void (*)(void*);

	class ModuleSystemTest : public ::testing::Test {
	protected:
		void SetUp() override {
			ModuleSystem::Initialize();
		}

		void TearDown() override {
			ModuleSystem::Terminate();
		}

		template <typename Fn>
		Fn Resolve(ModuleHandle handle, const char* name) {
			void* image = ModuleSystem::GetNativeHandle(handle);
			EXPECT_NE(image, nullptr);
			Fn fn = image ? reinterpret_cast<Fn>(ModuleSystem::FindSymbol(image, name)) : nullptr;
			EXPECT_NE(fn, nullptr) << "missing export: " << name;
			return fn;
		}
	};

	class AnnouncementProbe : public Object {
	public:
		struct Seen {
			const void* image = nullptr;
			bool mapped = false;
			bool shutdownRan = true;
			bool typeRegistered = false;
		};
		std::vector<Seen> seen;

		void OnImageUnloading(const ImageUnloadingEvent& event) {
			Seen entry;
			entry.image = event.image;
			const void* image = event.image;
			auto shutdownRan = image ? reinterpret_cast<bool (*)()>(ModuleSystem::FindSymbol(image, "SampleModuleShutdownRan")) : nullptr;
			entry.mapped = shutdownRan != nullptr;
			entry.shutdownRan = shutdownRan ? shutdownRan() : true;
			entry.typeRegistered = TypeRegistry::FindType(StringHash("SampleReflected")) != nullptr;
			seen.push_back(entry);
		}
	};

	struct ScopedAnnouncementProbe {
		SharedPtr<AnnouncementProbe> probe = MakeShared<AnnouncementProbe>();
		ScopedAnnouncementProbe() {
			ModuleEvents::ImageUnloading.Subscribe(EventBus::GetGlobal(), Handle<AnnouncementProbe>(probe.get()),
												   &AnnouncementProbe::OnImageUnloading);
		}
		~ScopedAnnouncementProbe() { EventBus::GetGlobal().UnsubscribeAll(probe->GetInstanceId()); }
	};

} // anonymous namespace

TEST_F(ModuleSystemTest, LoadValidatesBindsAndRegisters) {
	u32 typesBefore = TypeRegistry::GetTypeCount();

	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	EXPECT_EQ(ModuleSystem::GetLoadedCount(), 1u);
	EXPECT_EQ(ModuleSystem::GetModuleName(handle), "SampleModule");
	EXPECT_EQ(ModuleSystem::GetModuleBinaryPath(handle).ToString(), SAMPLE_MODULE_DLL);

	EXPECT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
	EXPECT_GT(TypeRegistry::GetTypeCount(), typesBefore);

	auto moduleTypeCount = Resolve<GetTypeCountFn>(handle, "SampleModuleGetTypeCount");
	ASSERT_NE(moduleTypeCount, nullptr);
	EXPECT_EQ(moduleTypeCount(), TypeRegistry::GetTypeCount());
}

TEST_F(ModuleSystemTest, SharedStateCrossesTheBoundary) {
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));

	auto allocateBlock = Resolve<AllocateBlockFn>(handle, "SampleModuleAllocateBlock");
	auto freeBlock = Resolve<FreeBlockFn>(handle, "SampleModuleFreeBlock");
	ASSERT_NE(allocateBlock, nullptr);
	ASSERT_NE(freeBlock, nullptr);

	size_t before = MemoryManager::GetBytesForTag(MemTag::Scripting);
	void* block = nullptr;
	size_t moduleView = allocateBlock(64 * 1024, &block);
	ASSERT_NE(block, nullptr);
	EXPECT_EQ(moduleView, MemoryManager::GetBytesForTag(MemTag::Scripting));
	freeBlock(block);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scripting), before);

	auto boxPayload = Resolve<BoxPayloadFn>(handle, "SampleModuleBoxPayload");
	auto readPayload = Resolve<ReadPayloadFn>(handle, "SampleModuleReadPayload");
	ASSERT_NE(boxPayload, nullptr);
	ASSERT_NE(readPayload, nullptr);
	{
		Variant boxed;
		boxPayload(&boxed, 4242);
		Variant copy = boxed;
		EXPECT_EQ(readPayload(&copy), 4242);
	}
}

TEST_F(ModuleSystemTest, UnloadSweepsOnlyModuleState) {
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));

	auto boxPayload = Resolve<BoxPayloadFn>(handle, "SampleModuleBoxPayload");
	ASSERT_NE(boxPayload, nullptr);
	{
		Variant probe;
		boxPayload(&probe, 1);
	}
	u64 payloadTypeId = 0;
	{
		auto getPayloadTypeId = Resolve<u64 (*)()>(handle, "SampleModuleGetPayloadTypeId");
		ASSERT_NE(getPayloadTypeId, nullptr);
		payloadTypeId = getPayloadTypeId();
	}
	ASSERT_NE(VariantDispatch::Lookup(StringHash(payloadTypeId)), nullptr);

	ASSERT_TRUE(ModuleSystem::Unload(handle));

	EXPECT_EQ(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
	EXPECT_EQ(VariantDispatch::Lookup(StringHash(payloadTypeId)), nullptr);
	EXPECT_NE(TypeRegistry::FindType(StringHash("Object")), nullptr);
	EXPECT_FALSE(ModuleSystem::IsLoaded(handle));
	EXPECT_EQ(ModuleSystem::GetLoadedCount(), 0u);

	TypeRegistry::RegisterPendingTypes();

	ModuleHandle again = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(again));
	EXPECT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
}

TEST_F(ModuleSystemTest, ImageAnnouncedBeforeTeardown) {
	ScopedAnnouncementProbe listener;
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	const void* image = ModuleSystem::GetNativeHandle(handle);
	EXPECT_TRUE(listener.probe->seen.empty());

	ASSERT_TRUE(ModuleSystem::Unload(handle));

	ASSERT_EQ(listener.probe->seen.size(), 1u);
	const AnnouncementProbe::Seen& seen = listener.probe->seen[0];
	EXPECT_EQ(seen.image, image);
	EXPECT_TRUE(seen.mapped);
	EXPECT_FALSE(seen.shutdownRan);
	EXPECT_TRUE(seen.typeRegistered);
}

TEST_F(ModuleSystemTest, ARefusedLoadIsAnnouncedToo) {
	ScopedAnnouncementProbe listener;
	const ModuleHandle handle = ModuleSystem::Load(Path("ShalltearHotReloadModuleBad" SLT_TEST_DLL_SUFFIX));
	EXPECT_FALSE(ModuleSystem::IsLoaded(handle));
	ASSERT_EQ(listener.probe->seen.size(), 1u);
	EXPECT_NE(listener.probe->seen[0].image, nullptr);
}

TEST_F(ModuleSystemTest, BuildOutputIsNeverLocked) {
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));

	std::ofstream probe(SAMPLE_MODULE_DLL, std::ios::binary | std::ios::in | std::ios::app);
	EXPECT_TRUE(probe.is_open());
}

TEST_F(ModuleSystemTest, RejectsGarbageAndMissingBinaries) {
	TypeRegistry::RegisterPendingTypes();
	u32 typesBefore = TypeRegistry::GetTypeCount();

	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("DoesNotExist" SLT_TEST_DLL_SUFFIX))));

	{
		std::ofstream garbage("NotAModule" SLT_TEST_DLL_SUFFIX, std::ios::binary | std::ios::trunc);
		garbage << "this is not a dll";
	}
	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("NotAModule" SLT_TEST_DLL_SUFFIX))));
	std::filesystem::remove("NotAModule" SLT_TEST_DLL_SUFFIX);

#ifdef SHALLTEAR_SHARED
	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("ShalltearCore" SLT_TEST_DLL_SUFFIX))));
#endif

	EXPECT_EQ(TypeRegistry::GetTypeCount(), typesBefore);
	EXPECT_EQ(ModuleSystem::GetLoadedCount(), 0u);
}

namespace {

	class LoaderCapturingSink : public ILogSink {
	public:
		static constexpr const char* SENTINEL = "loader-sink-sentinel";

		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			if (event.message.find(SENTINEL) != str::npos) {
				_armed = true;
				return;
			}
			if (!_armed || event.level < LogLevel::Error) {
				return;
			}
			_errors.push_back(str(event.message));
		}
		void Flush() override {}

		bool Armed() const {
			std::lock_guard<std::mutex> lock(_mutex);
			return _armed;
		}

		u32 ErrorsMatching(std::string_view needle) const {
			std::lock_guard<std::mutex> lock(_mutex);
			u32 count = 0;
			for (const str& message : _errors) {
				if (needle.empty() || message.find(needle) != str::npos) {
					++count;
				}
			}
			return count;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _errors;
		bool _armed = false;
	};

	bool ArmLoaderSink(const std::shared_ptr<LoaderCapturingSink>& sink) {
		SLT_LOG_WARN("Module", "{}", LoaderCapturingSink::SENTINEL);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (sink->Armed()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

} // anonymous namespace

TEST_F(ModuleSystemTest, AbsentQuietBrokenReported) {
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<LoaderCapturingSink>();
	LogManager::AddSink(sink);
	ASSERT_TRUE(ArmLoaderSink(sink));

	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("NothingHasCompiledThisYet" SLT_TEST_DLL_SUFFIX))));
	EXPECT_FALSE(ModuleSystem::IsLoaded(
		ModuleSystem::Load(Path("NothingHasCompiledThisYet" SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace)));

	{
		std::ofstream garbage("PresentButBroken" SLT_TEST_DLL_SUFFIX, std::ios::binary | std::ios::trunc);
		garbage << "this is not a dll";
	}
	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("PresentButBroken" SLT_TEST_DLL_SUFFIX))));

	LogManager::Terminate();
	EXPECT_EQ(sink->ErrorsMatching("NothingHasCompiledThisYet"), 0u);
	EXPECT_GT(sink->ErrorsMatching("PresentButBroken"), 0u);
	sink.reset();
	JobSystem::Terminate();

	std::filesystem::remove("PresentButBroken" SLT_TEST_DLL_SUFFIX);
}

TEST_F(ModuleSystemTest, RejectsModuleWithMismatchedAbiHash) {
	TypeRegistry::RegisterPendingTypes();
	u32 typesBefore = TypeRegistry::GetTypeCount();

	EXPECT_FALSE(ModuleSystem::IsLoaded(ModuleSystem::Load(Path("ShalltearHotReloadModuleAbi" SLT_TEST_DLL_SUFFIX))));

	EXPECT_EQ(ModuleSystem::GetLoadedCount(), 0u);
	EXPECT_EQ(TypeRegistry::GetTypeCount(), typesBefore);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("HotReloadBehavior")), nullptr);
}

TEST_F(ModuleSystemTest, AcceptsSameModuleWithHostAbiHash) {
	ModuleHandle handle = ModuleSystem::Load(Path("ShalltearHotReloadModuleV1" SLT_TEST_DLL_SUFFIX));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	EXPECT_NE(TypeRegistry::FindType(StringHash("HotReloadBehavior")), nullptr);
	ModuleSystem::Unload(handle);
}

TEST_F(ModuleSystemTest, TerminateUnloadsEverything) {
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));

	ModuleSystem::Terminate();
	EXPECT_FALSE(ModuleSystem::IsInitialized());
	EXPECT_EQ(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);

	ModuleSystem::Initialize();
}

namespace {

	bool IsBeingDeleted(const std::filesystem::path& path) {
		std::error_code ec;
		(void)std::filesystem::status(path, ec);
		return static_cast<bool>(ec);
	}

	u32 CountCacheCopiesOf(std::string_view stem) {
		u32 count = 0;
		std::error_code ec;
		std::filesystem::directory_iterator it("Library/ModuleCache", ec);
		if (ec) {
			return 0;
		}
		const std::string prefix = std::string(stem) + "-";
		for (const auto& entry : it) {
			const std::string name = entry.path().filename().string();
			if (entry.path().extension() == SLT_TEST_DLL_SUFFIX && name.rfind(prefix, 0) == 0 && !IsBeingDeleted(entry.path())) {
				++count;
			}
		}
		return count;
	}

	std::vector<str> CacheCopiesOf(std::string_view stem) {
		std::vector<str> names;
		std::error_code ec;
		std::filesystem::directory_iterator it("Library/ModuleCache", ec);
		if (ec) {
			return names;
		}
		const std::string prefix = std::string(stem) + "-";
		for (const auto& entry : it) {
			const std::string name = entry.path().filename().string();
			if (entry.path().extension() == SLT_TEST_DLL_SUFFIX && name.rfind(prefix, 0) == 0 && !IsBeingDeleted(entry.path())) {
				names.push_back(str(name));
			}
		}
		return names;
	}

	class ScopedHeldSlot {
	public:
		explicit ScopedHeldSlot(const std::filesystem::path& at) : _path(at) {
			std::error_code ec;
			std::filesystem::create_directories(at.parent_path(), ec);
			{
				std::ofstream seed(at, std::ios::binary);
				seed << "held";
			}
			_held = TestPlatform::HoldFile(at.string().c_str());
		}

		~ScopedHeldSlot() {
			TestPlatform::ReleaseFile(_held);
			std::error_code ec;
			std::filesystem::remove(_path, ec);
		}

		ScopedHeldSlot(const ScopedHeldSlot&) = delete;
		ScopedHeldSlot& operator=(const ScopedHeldSlot&) = delete;

		bool Held() const { return _held != nullptr; }
	private:
		std::filesystem::path _path;
		TestPlatform::HeldFile* _held = nullptr;
	};

} // anonymous namespace

TEST_F(ModuleSystemTest, TheDefaultPolicyStillCacheCopies) {
	const u32 before = CountCacheCopiesOf("ShalltearSampleModule");

	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	EXPECT_EQ(CountCacheCopiesOf("ShalltearSampleModule"), before + 1);

	ModuleSystem::Unload(handle);
	EXPECT_EQ(CountCacheCopiesOf("ShalltearSampleModule"), before);
}

TEST_F(ModuleSystemTest, HeldSlotSkipped) {
	std::vector<str> before = CacheCopiesOf("ShalltearSampleModule");
	ModuleHandle first = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(first));

	std::vector<str> after = CacheCopiesOf("ShalltearSampleModule");
	str taken;
	for (const str& name : after) {
		if (std::find(before.begin(), before.end(), name) == before.end()) {
			taken = name;
		}
	}
	ASSERT_FALSE(taken.empty());
	ModuleSystem::Unload(first);

	size_t dash = taken.rfind('-');
	ASSERT_NE(dash, str::npos);
	u32 nextIndex = static_cast<u32>(std::stoul(taken.substr(dash + 1, taken.size() - dash - sizeof(SLT_TEST_DLL_SUFFIX)))) + 1;
	std::filesystem::path contested =
		std::filesystem::path("Library/ModuleCache") / std::format("ShalltearSampleModule-{}" SLT_TEST_DLL_SUFFIX, nextIndex);

	ScopedHeldSlot held(contested);
	ASSERT_TRUE(held.Held());

	ModuleHandle second = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	EXPECT_TRUE(ModuleSystem::IsLoaded(second));
	std::ifstream kept(contested, std::ios::binary);
	EXPECT_EQ(str((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>()), "held");
	kept.close();
	ModuleSystem::Unload(second);
}

TEST_F(ModuleSystemTest, InPlaceCopiesNothing) {
	const u32 before = CountCacheCopiesOf("ShalltearSampleModule");

	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL), ModuleLoadPolicy::InPlace);
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	EXPECT_EQ(CountCacheCopiesOf("ShalltearSampleModule"), before);

	EXPECT_EQ(ModuleSystem::GetModuleName(handle), "SampleModule");
	EXPECT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
	auto moduleTypeCount = Resolve<GetTypeCountFn>(handle, "SampleModuleGetTypeCount");
	ASSERT_NE(moduleTypeCount, nullptr);
	EXPECT_EQ(moduleTypeCount(), TypeRegistry::GetTypeCount());

	ModuleSystem::Unload(handle);
}

TEST_F(ModuleSystemTest, InPlaceUnloadKeepsBinary) {
	ASSERT_TRUE(std::filesystem::exists(SAMPLE_MODULE_DLL));

	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL), ModuleLoadPolicy::InPlace);
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	ASSERT_TRUE(ModuleSystem::Unload(handle));

	EXPECT_TRUE(std::filesystem::exists(SAMPLE_MODULE_DLL));
}

TEST_F(ModuleSystemTest, ReloadIsRefusedForInPlaceModule) {
	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL), ModuleLoadPolicy::InPlace);
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));

	const u32 before = CountCacheCopiesOf("ShalltearSampleModule");
	EXPECT_FALSE(ModuleSystem::Reload(handle));

	EXPECT_EQ(CountCacheCopiesOf("ShalltearSampleModule"), before);

	EXPECT_TRUE(ModuleSystem::IsLoaded(handle));
	EXPECT_EQ(ModuleSystem::GetModuleName(handle), "SampleModule");
	EXPECT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);

	ModuleSystem::Unload(handle);
}

TEST_F(ModuleSystemTest, PassKindFollowsModule) {
	ASSERT_NE(RenderPass::GetReflectType(), nullptr);
	const u32 typesBefore = TypeRegistry::GetTypeCount();
	ASSERT_EQ(TypeRegistry::FindType(StringHash("SamplePass")), nullptr);

	ModuleHandle handle = ModuleSystem::Load(Path(SAMPLE_MODULE_DLL));
	ASSERT_TRUE(ModuleSystem::IsLoaded(handle));
	ASSERT_GT(TypeRegistry::GetTypeCount(), typesBefore);

	const TypeInfo* type = TypeRegistry::FindType(StringHash("SamplePass"));
	ASSERT_NE(type, nullptr);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(type, RenderPass::GetReflectType()));
	EXPECT_NE(type->createFn, nullptr);

	std::filesystem::path dir = std::filesystem::temp_directory_path() / "shalltear_module_pass_test";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	Path document((dir / "FromAModule.slpass").generic_string());
	ASSERT_TRUE(FileSystem::WriteTextFile(document, str(R"({"_type": "SamplePass", "point": "AfterOpaque"})")));

	RenderPassImporter importer;
	Asset* row = importer.Import(document);
	ASSERT_NE(row, nullptr);
	EXPECT_EQ(row->GetType(), type);
	EXPECT_EQ(row->GetAssetTypeId(), StringHash("SamplePass"));
	EXPECT_EQ(static_cast<RenderPass*>(row)->point, PassPoint::AfterOpaque);

	TypeRegistry::DestroyObjectInstance(row);

	ASSERT_TRUE(ModuleSystem::Unload(handle));

	EXPECT_EQ(TypeRegistry::FindType(StringHash("SamplePass")), nullptr);
	EXPECT_EQ(TypeRegistry::GetTypeCount(), typesBefore);

	std::filesystem::remove_all(dir, ec);
}
