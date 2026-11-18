#include "TestsPCH.h"

#include "TestPlatform.h"

#ifdef SHALLTEAR_SHARED

#include "Shalltear/Core/Serialization/BinarySerializer.h"

#undef CreateDirectory
#undef GetFileSize

using namespace Shalltear;

namespace {

	using RegisterTypesFn = bool (*)();
	using UnregisterTypesFn = void (*)();
	using GetTypeCountFn = u32 (*)();
	using GetPayloadTypeIdFn = u64 (*)();
	using BoxPayloadFn = void (*)(Variant*, i32);
	using ReadPayloadFn = i32 (*)(const Variant*);
	using CreateSampleFn = void* (*)(i32, const char*);
	using DestroySampleFn = void (*)(void*);
	using GetSampleHealthFn = i32 (*)(const void*);
	using SerializeSampleFn = bool (*)(Buffer*, const void*);
	using AllocateBlockFn = size_t (*)(size_t, void**);
	using FreeBlockFn = void (*)(void*);

	class DLLBoundaryTest : public ::testing::Test {
	protected:
		void SetUp() override {
			EnsureVariantVTable<i32>();
			EnsureVariantVTable<str>();
			hostI32VTable = VariantDispatch::Lookup(GetVariantTypeId<i32>());
			hostStrVTable = VariantDispatch::Lookup(GetVariantTypeId<str>());
			ASSERT_NE(hostI32VTable, nullptr);
			ASSERT_NE(hostStrVTable, nullptr);

			module = TestPlatform::OpenLibrary("ShalltearSampleModule" SLT_TEST_DLL_SUFFIX);
			ASSERT_NE(module, nullptr);

			registerTypes = Resolve<RegisterTypesFn>("SampleModuleRegisterTypes");
			unregisterTypes = Resolve<UnregisterTypesFn>("SampleModuleUnregisterTypes");
			getTypeCount = Resolve<GetTypeCountFn>("SampleModuleGetTypeCount");
			getPayloadTypeId = Resolve<GetPayloadTypeIdFn>("SampleModuleGetPayloadTypeId");
			boxPayload = Resolve<BoxPayloadFn>("SampleModuleBoxPayload");
			readPayload = Resolve<ReadPayloadFn>("SampleModuleReadPayload");
			createSample = Resolve<CreateSampleFn>("SampleModuleCreateSample");
			destroySample = Resolve<DestroySampleFn>("SampleModuleDestroySample");
			getSampleHealth = Resolve<GetSampleHealthFn>("SampleModuleGetSampleHealth");
			serializeSample = Resolve<SerializeSampleFn>("SampleModuleSerializeSample");
			allocateBlock = Resolve<AllocateBlockFn>("SampleModuleAllocateBlock");
			freeBlock = Resolve<FreeBlockFn>("SampleModuleFreeBlock");

			baselineTypeCount = TypeRegistry::GetTypeCount();
		}

		void TearDown() override {
			if (module) {
				if (unregisterTypes) {
					unregisterTypes();
				}
				TypeRegistry::UnlinkPendingRegistrationsIn(module);
				TestPlatform::CloseLibrary(module);
				module = nullptr;
			}

			EXPECT_EQ(VariantDispatch::Lookup(GetVariantTypeId<i32>()), hostI32VTable);
			EXPECT_EQ(VariantDispatch::Lookup(GetVariantTypeId<str>()), hostStrVTable);
		}

		template <typename Fn>
		Fn Resolve(const char* name) {
			Fn fn = reinterpret_cast<Fn>(TestPlatform::FindSymbol(module, name));
			EXPECT_NE(fn, nullptr) << "missing export: " << name;
			return fn;
		}

		void* module = nullptr;
		u32 baselineTypeCount = 0;

		const VariantVTable* hostI32VTable = nullptr;
		const VariantVTable* hostStrVTable = nullptr;

		RegisterTypesFn registerTypes = nullptr;
		UnregisterTypesFn unregisterTypes = nullptr;
		GetTypeCountFn getTypeCount = nullptr;
		GetPayloadTypeIdFn getPayloadTypeId = nullptr;
		BoxPayloadFn boxPayload = nullptr;
		ReadPayloadFn readPayload = nullptr;
		CreateSampleFn createSample = nullptr;
		DestroySampleFn destroySample = nullptr;
		GetSampleHealthFn getSampleHealth = nullptr;
		SerializeSampleFn serializeSample = nullptr;
		AllocateBlockFn allocateBlock = nullptr;
		FreeBlockFn freeBlock = nullptr;
	};

} // anonymous namespace

TEST_F(DLLBoundaryTest, ModuleTypeRegistersIntoHostRegistry) {
	ASSERT_TRUE(registerTypes());

	const TypeInfo* info = TypeRegistry::FindType(StringHash("SampleReflected"));
	ASSERT_NE(info, nullptr);

	EXPECT_GT(TypeRegistry::GetTypeCount(), baselineTypeCount);

	EXPECT_EQ(getTypeCount(), TypeRegistry::GetTypeCount());

	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(info, Object::GetReflectType()));

	EXPECT_EQ(info->fieldCount, 2u);
}

TEST_F(DLLBoundaryTest, UnregisterRemovesModuleTypeFromHostRegistry) {
	ASSERT_TRUE(registerTypes());
	ASSERT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);

	u32 countAfterRegister = TypeRegistry::GetTypeCount();

	unregisterTypes();

	EXPECT_EQ(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
	EXPECT_EQ(TypeRegistry::GetTypeCount(), countAfterRegister - 1);
}

TEST_F(DLLBoundaryTest, OwnershipSweepRemovesOnlyModuleTypes) {
	ASSERT_TRUE(registerTypes());
	ASSERT_NE(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);

	u32 countBefore = TypeRegistry::GetTypeCount();
	u32 swept = TypeRegistry::UnregisterOwnedBy(module);

	EXPECT_EQ(swept, 1u);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("SampleReflected")), nullptr);
	EXPECT_EQ(TypeRegistry::GetTypeCount(), countBefore - swept);
	EXPECT_NE(TypeRegistry::FindType(StringHash("Object")), nullptr);
	EXPECT_NE(TypeRegistry::FindType(StringHash("i32")), nullptr);
}

TEST_F(DLLBoundaryTest, ModuleVariantRoundTripsThroughHostCopy) {
	{
		Variant boxed;
		boxPayload(&boxed, 1337);

		ASSERT_FALSE(boxed.IsEmpty());
		EXPECT_EQ(boxed.GetTypeId().GetValue(), getPayloadTypeId());

		Variant copy = boxed;
		EXPECT_EQ(readPayload(&copy), 1337);
	}
	SUCCEED();
}

TEST_F(DLLBoundaryTest, FirstOwnerKeepsHostVTables) {
	ASSERT_TRUE(registerTypes());

	{
		Variant probe;
		boxPayload(&probe, 1);

		void* source = createSample(7, "clobber-probe");
		ASSERT_NE(source, nullptr);
		{
			Buffer buffer;
			ASSERT_TRUE(serializeSample(&buffer, source));
		}
		destroySample(source);
	}

	EXPECT_EQ(VariantDispatch::Lookup(GetVariantTypeId<i32>()), hostI32VTable);
	EXPECT_EQ(VariantDispatch::Lookup(GetVariantTypeId<str>()), hostStrVTable);

	const StringHash payloadId{getPayloadTypeId()};
	const VariantVTable* moduleVTable = VariantDispatch::Lookup(payloadId);
	ASSERT_NE(moduleVTable, nullptr);
	EXPECT_EQ(VariantDispatch::GetOwner(payloadId), static_cast<const void*>(module));

	static const VariantVTable hostCopy = *hostI32VTable;
	const StringHash sharedId{"DLLBoundaryTest.SharedBetweenImages"};
	VariantDispatch::Register(sharedId, moduleVTable);
	VariantDispatch::Register(sharedId, &hostCopy);
	VariantDispatch::Register(sharedId, &hostCopy);
	EXPECT_EQ(VariantDispatch::Lookup(sharedId), moduleVTable);
	EXPECT_EQ(VariantDispatch::GetOwner(sharedId), nullptr);

	EXPECT_EQ(VariantDispatch::UnregisterAllOwnedBy(module), 1u);
	EXPECT_EQ(VariantDispatch::Lookup(payloadId), nullptr);
	EXPECT_EQ(VariantDispatch::Lookup(sharedId), &hostCopy);
	EXPECT_NE(VariantDispatch::GetOwner(sharedId), nullptr);
	EXPECT_NE(VariantDispatch::GetOwner(sharedId), static_cast<const void*>(module));
	EXPECT_EQ(VariantDispatch::Lookup(GetVariantTypeId<i32>()), hostI32VTable);
	VariantDispatch::Unregister(sharedId);
	EXPECT_EQ(VariantDispatch::Lookup(sharedId), nullptr);
}

TEST_F(DLLBoundaryTest, ModuleAllocationsVisibleInHostTracker) {
	constexpr size_t BLOCK = 1024 * 1024;
	size_t hostBytesBefore = MemoryManager::GetBytesForTag(MemTag::Scripting);

	void* block = nullptr;
	size_t moduleView = allocateBlock(BLOCK, &block);
	ASSERT_NE(block, nullptr);

	EXPECT_EQ(moduleView, MemoryManager::GetBytesForTag(MemTag::Scripting));
	EXPECT_GE(MemoryManager::GetBytesForTag(MemTag::Scripting), hostBytesBefore + BLOCK);

	freeBlock(block);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scripting), hostBytesBefore);
}

TEST_F(DLLBoundaryTest, HostDeserializesModuleSerializedInstance) {
	ASSERT_TRUE(registerTypes());
	const TypeInfo* info = TypeRegistry::FindType(StringHash("SampleReflected"));
	ASSERT_NE(info, nullptr);

	void* source = createSample(1234, "dll-crossing");
	ASSERT_NE(source, nullptr);

	{
		Buffer buffer;
		ASSERT_TRUE(serializeSample(&buffer, source));

		void* target = createSample(0, "");
		ASSERT_TRUE(BinarySerializer::Deserialize(target, info, buffer));
		EXPECT_EQ(getSampleHealth(target), 1234);

		const FieldInfo* healthField = FindField(info, StringHash("health"));
		ASSERT_NE(healthField, nullptr);
		{
			Variant value;
			healthField->getFn(target, &value);
			EXPECT_EQ(value.As<i32>(), 1234);
		}

		destroySample(target);
	}

	destroySample(source);
}

#endif
