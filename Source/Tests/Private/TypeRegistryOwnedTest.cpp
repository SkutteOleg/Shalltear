#include "TestsPCH.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

using namespace Shalltear;

namespace {

	int _tokenA = 0;
	int _tokenB = 0;

	class OwnedProbe final : public Object {
	public:
		const TypeInfo* GetType() const override { return runtimeType; }

		const TypeInfo* runtimeType = nullptr;
	};

	Object* CreateOwnedProbe(const TypeInfo* type, MemTag tag) {
		OwnedProbe* probe = New<OwnedProbe>(tag);
		probe->runtimeType = type;
		return probe;
	}

	Object* CreatePlainProbe(MemTag tag) {
		return New<OwnedProbe>(tag);
	}

	enum class ProbeCreate {
		Typed,
		Plain,
		None,
	};

	const TypeInfo* RegisterProbe(StringHash name, const void* token, u32 flags = 0, ProbeCreate create = ProbeCreate::Typed) {
		ReflectBuilder builder(name, sizeof(OwnedProbe), alignof(OwnedProbe));
		builder.SetParent(Object::GetReflectType());
		builder.SetFlags(flags);
		if (create == ProbeCreate::Typed) {
			builder.SetCreateTypedFn(&CreateOwnedProbe);
		} else if (create == ProbeCreate::Plain) {
			builder.SetCreateFn(&CreatePlainProbe);
		}
		builder.AddAttribute(StringHash("OwnedProbeMark"), Variant(static_cast<i64>(7)));
		return builder.FinalizeOwned(token);
	}

	struct ScopedOwnedTypes {
		~ScopedOwnedTypes() {
			TypeRegistry::UnregisterOwnedBy(&_tokenA);
			TypeRegistry::UnregisterOwnedBy(&_tokenB);
		}
	};

} // anonymous namespace

TEST(TypeRegistryOwnedTest, TokenSweepsOnlyItsTypes) {
	const ScopedOwnedTypes scope;
	const TypeInfo* first = RegisterProbe("OwnedProbe.First", &_tokenA);
	const TypeInfo* second = RegisterProbe("OwnedProbe.Second", &_tokenA);
	const TypeInfo* other = RegisterProbe("OwnedProbe.Other", &_tokenB);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	ASSERT_NE(other, nullptr);
	EXPECT_EQ(first->ownerModule, &_tokenA);
	EXPECT_EQ(other->ownerModule, &_tokenB);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(second, Object::GetReflectType()));

	EXPECT_EQ(TypeRegistry::UnregisterOwnedBy(&_tokenA), 2u);
	EXPECT_EQ(TypeRegistry::FindType("OwnedProbe.First"), nullptr);
	EXPECT_EQ(TypeRegistry::FindType("OwnedProbe.Second"), nullptr);
	EXPECT_EQ(TypeRegistry::FindType("OwnedProbe.Other"), other);
}

TEST(TypeRegistryOwnedTest, RefusesNameCollision) {
	const ScopedOwnedTypes scope;
	const TypeInfo* first = RegisterProbe("OwnedProbe.Twin", &_tokenA);
	ASSERT_NE(first, nullptr);

	EXPECT_EQ(RegisterProbe("OwnedProbe.Twin", &_tokenB), nullptr);
	EXPECT_EQ(RegisterProbe("OwnedProbe.Twin", &_tokenA), nullptr);
	EXPECT_EQ(TypeRegistry::FindType("OwnedProbe.Twin"), first);

	const TypeInfo* engineType = SceneObject::GetReflectType();
	EXPECT_EQ(RegisterProbe(engineType->name, &_tokenA), nullptr);
	EXPECT_EQ(TypeRegistry::FindType(engineType->name), engineType);
	EXPECT_NE(engineType->ownerModule, &_tokenA);
}

TEST(TypeRegistryOwnedTest, SweepFreesHeapMetadata) {
	const ScopedOwnedTypes scope;
	ASSERT_NE(RegisterProbe("OwnedProbe.Warm", &_tokenA), nullptr);
	ASSERT_EQ(TypeRegistry::UnregisterOwnedBy(&_tokenA), 1u);
	const size_t baseline = MemoryManager::GetBytesForTag(MemTag::Core);

	ASSERT_NE(RegisterProbe("OwnedProbe.Heavy", &_tokenA), nullptr);
	const size_t registered = MemoryManager::GetBytesForTag(MemTag::Core);
	EXPECT_GT(registered, baseline);

	EXPECT_EQ(RegisterProbe("OwnedProbe.Heavy", &_tokenB), nullptr);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Core), registered);

	EXPECT_EQ(TypeRegistry::UnregisterOwnedBy(&_tokenA), 1u);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Core), baseline);
}

TEST(TypeRegistryOwnedTest, CreateTypedFnReceivesItsType) {
	const ScopedOwnedTypes scope;
	const TypeInfo* first = RegisterProbe("OwnedProbe.First", &_tokenA);
	const TypeInfo* second = RegisterProbe("OwnedProbe.Second", &_tokenA);
	const TypeInfo* abstract = RegisterProbe("OwnedProbe.Abstract", &_tokenA, TypeFlagAbstract);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	ASSERT_NE(abstract, nullptr);

	Object* one = TypeRegistry::CreateObjectInstance(first, MemTag::Core);
	Object* two = TypeRegistry::CreateObjectInstance(second, MemTag::Core);
	ASSERT_NE(one, nullptr);
	ASSERT_NE(two, nullptr);
	EXPECT_EQ(one->GetType(), first);
	EXPECT_EQ(two->GetType(), second);
	EXPECT_EQ(TypeRegistry::CreateObjectInstance(abstract, MemTag::Core), nullptr);

	TypeRegistry::DestroyObjectInstance(one);
	TypeRegistry::DestroyObjectInstance(two);
}

TEST(TypeRegistryOwnedTest, TypedCreateSharesTheCreateSlot) {
	const ScopedOwnedTypes scope;
	const TypeInfo* typed = RegisterProbe("OwnedProbe.Typed", &_tokenA);
	const TypeInfo* plain = RegisterProbe("OwnedProbe.Plain", &_tokenA, 0, ProbeCreate::Plain);
	const TypeInfo* inert = RegisterProbe("OwnedProbe.Inert", &_tokenA, 0, ProbeCreate::None);
	ASSERT_NE(typed, nullptr);
	ASSERT_NE(plain, nullptr);
	ASSERT_NE(inert, nullptr);

	EXPECT_NE(typed->createFn, nullptr);
	EXPECT_NE(typed->flags & TypeFlagTypedCreate, 0u);
	EXPECT_NE(plain->createFn, nullptr);
	EXPECT_EQ(plain->flags & TypeFlagTypedCreate, 0u);
	EXPECT_EQ(inert->createFn, nullptr);
	EXPECT_EQ(inert->flags & TypeFlagTypedCreate, 0u);
	EXPECT_EQ(TypeRegistry::CreateObjectInstance(inert, MemTag::Core), nullptr);

	const TypeInfo* native = SceneObject::GetReflectType();
	EXPECT_NE(native->createFn, nullptr);
	EXPECT_EQ(native->flags & TypeFlagTypedCreate, 0u);
}

TEST(TypeRegistryOwnedTest, BothCreatePathsReceiveTheTag) {
	const ScopedOwnedTypes scope;
	const TypeInfo* typed = RegisterProbe("OwnedProbe.Typed", &_tokenA);
	const TypeInfo* plain = RegisterProbe("OwnedProbe.Plain", &_tokenA, 0, ProbeCreate::Plain);
	ASSERT_NE(typed, nullptr);
	ASSERT_NE(plain, nullptr);

	for (const TypeInfo* type : {typed, plain}) {
		const size_t baseline = MemoryManager::GetBytesForTag(MemTag::Game);
		Object* object = TypeRegistry::CreateObjectInstance(type, MemTag::Game);
		ASSERT_NE(object, nullptr);
		EXPECT_GE(MemoryManager::GetBytesForTag(MemTag::Game), baseline + sizeof(OwnedProbe));
		EXPECT_EQ(static_cast<OwnedProbe*>(object)->runtimeType, type == typed ? typed : nullptr);

		TypeRegistry::DestroyObjectInstance(object);
		EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Game), baseline);
	}
}
