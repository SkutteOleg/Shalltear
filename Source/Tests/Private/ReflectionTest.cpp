#include "TestsPCH.h"

#include <stdexcept>

using namespace Shalltear;

namespace {

	class MockComponent : public Object {
	public:
		f32 health = 100.0f;
		i32 damage = 10;

		SLT_REFLECT_CLASS(MockComponent)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(health)
		SLT_REFLECT_FIELD(damage)
		SLT_END_REFLECT()
	};

	class AnnotatedComponent : public Object {
	public:
		f32 volume = 1.0f;
		i32 channel = 0;
		bool muted = false;
		str label = "default";

		void Reset() {
			volume = 1.0f;
			channel = 0;
			muted = false;
		}

		i32 Double(i32 x) { return x * 2; }

		SLT_REFLECT_CLASS(AnnotatedComponent)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(volume, SLT_RANGE(0.0f, 1.0f))
		SLT_REFLECT_FIELD(channel, SLT_TOOLTIP("Audio channel index"))
		SLT_REFLECT_FIELD(muted)
		SLT_REFLECT_FIELD(label, SLT_READONLY)
		SLT_REFLECT_METHOD(Reset)
		SLT_REFLECT_METHOD(Double)
		SLT_END_REFLECT()
	};

	class BaseThing : public Object {
	public:
		i32 baseValue = 7;

		SLT_REFLECT_CLASS(BaseThing)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(baseValue)
		SLT_END_REFLECT()
	};

	class DerivedThing : public BaseThing {
	public:
		f32 derivedExtra = 3.14f;

		SLT_REFLECT_CLASS(DerivedThing)
		SLT_REFLECT_PARENT(BaseThing)
		SLT_REFLECT_FIELD(derivedExtra)
		SLT_END_REFLECT()
	};

	class UnrelatedThing : public Object {
	public:
		SLT_REFLECT_CLASS(UnrelatedThing)
		SLT_REFLECT_PARENT(Object)
		SLT_END_REFLECT()
	};

	class FilteredComponent : public Object {
	public:
		i32 visible = 1;
		i32 hidden = 2;
		i32 runtimeOnly = 3;

		SLT_REFLECT_CLASS(FilteredComponent)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(visible)
		SLT_REFLECT_FIELD(hidden, SLT_HIDDEN)
		SLT_REFLECT_FIELD(runtimeOnly, SLT_TRANSIENT)
		SLT_END_REFLECT()
	};

} // namespace

TEST(VariantTest, DefaultConstructedIsEmpty) {
	Variant v;
	EXPECT_TRUE(v.IsEmpty());
	EXPECT_EQ(v.GetTypeId().GetValue(), 0u);
}

TEST(VariantTest, ConstructFromPrimitiveStoresValue) {
	Variant v(static_cast<i32>(42));
	EXPECT_FALSE(v.IsEmpty());
	EXPECT_TRUE(v.Is<i32>());
	EXPECT_EQ(v.As<i32>(), 42);
}

TEST(VariantTest, FloatStoresAndRetrievesExactValue) {
	Variant v(static_cast<f32>(3.14f));
	EXPECT_TRUE(v.Is<f32>());
	EXPECT_FLOAT_EQ(v.As<f32>(), 3.14f);
}

TEST(VariantTest, StringLargerThanSBOUsesHeap) {
	Variant v(str("hello world, this is a moderately long test string"));
	EXPECT_TRUE(v.Is<str>());
	EXPECT_EQ(v.As<str>(), "hello world, this is a moderately long test string");
}

TEST(VariantTest, CopyConstructorPreservesValue) {
	Variant a(static_cast<i32>(99));
	Variant b(a);
	EXPECT_TRUE(b.Is<i32>());
	EXPECT_EQ(b.As<i32>(), 99);
	EXPECT_EQ(a.As<i32>(), 99);
}

TEST(VariantTest, CopyAssignmentReplacesOldValue) {
	Variant a(static_cast<i32>(1));
	Variant b(static_cast<f32>(2.5f));
	a = b;
	EXPECT_TRUE(a.Is<f32>());
	EXPECT_FLOAT_EQ(a.As<f32>(), 2.5f);
}

TEST(VariantTest, MoveConstructorLeavesSourceEmpty) {
	Variant a(static_cast<i32>(55));
	Variant b(std::move(a));
	EXPECT_TRUE(b.Is<i32>());
	EXPECT_EQ(b.As<i32>(), 55);
	EXPECT_TRUE(a.IsEmpty());
}

TEST(VariantTest, ClearMakesVariantEmpty) {
	Variant v(static_cast<f32>(1.0f));
	v.Clear();
	EXPECT_TRUE(v.IsEmpty());
}

TEST(VariantTest, TryGetReturnsPointerOnMatch) {
	Variant v(static_cast<i32>(7));
	i32* p = v.TryGet<i32>();
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(*p, 7);
}

TEST(VariantTest, TryGetReturnsNullOnMismatch) {
	Variant v(static_cast<i32>(7));
	EXPECT_EQ(v.TryGet<f32>(), nullptr);
}

TEST(VariantTest, IsReturnsFalseForMismatchedType) {
	Variant v(static_cast<i32>(7));
	EXPECT_FALSE(v.Is<f32>());
	EXPECT_FALSE(v.Is<bool>());
}

TEST(VariantTest, LayoutIs48Bytes) {
	EXPECT_EQ(sizeof(Variant), 48u);
}

TEST(TypeRegistryTest, FindTypeReturnsRegisteredType) {
	const TypeInfo* info = MockComponent::GetReflectType();
	ASSERT_NE(info, nullptr);

	const TypeInfo* found = TypeRegistry::FindType(StringHash("MockComponent"));
	EXPECT_EQ(found, info);
}

TEST(TypeRegistryTest, FindTypeReturnsNullForMissingType) {
	EXPECT_EQ(TypeRegistry::FindType(StringHash("NonExistentType_XYZ123")), nullptr);
}

TEST(TypeRegistryTest, IsDerivedFromDirectParent) {
	const TypeInfo* derived = DerivedThing::GetReflectType();
	const TypeInfo* base = BaseThing::GetReflectType();

	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(derived, base));
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(base, derived));
}

TEST(TypeRegistryTest, IsDerivedFromGrandparent) {
	const TypeInfo* derived = DerivedThing::GetReflectType();
	const TypeInfo* object = Object::GetReflectType();

	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(derived, object));
}

TEST(TypeRegistryTest, IsDerivedFromSelfReturnsTrue) {
	const TypeInfo* info = MockComponent::GetReflectType();
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(info, info));
}

TEST(TypeRegistryTest, IsDerivedFromUnrelatedReturnsFalse) {
	const TypeInfo* a = MockComponent::GetReflectType();
	const TypeInfo* b = UnrelatedThing::GetReflectType();
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(a, b));
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(b, a));
}

TEST(TypeRegistryTest, IsDerivedFromHandlesNullGracefully) {
	const TypeInfo* info = MockComponent::GetReflectType();
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(nullptr, info));
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(info, nullptr));
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(nullptr, nullptr));
}

TEST(TypeRegistryTest, ClassIdsAreDense) {
	const TypeInfo* info = MockComponent::GetReflectType();
	EXPECT_LT(info->classId, 65535u);
}

TEST(ReflectFieldTest, TypeHasCorrectMetadata) {
	const TypeInfo* info = MockComponent::GetReflectType();
	ASSERT_NE(info, nullptr);
	EXPECT_EQ(info->name, StringHash("MockComponent"));
	EXPECT_EQ(info->size, sizeof(MockComponent));
	EXPECT_EQ(info->alignment, alignof(MockComponent));
	EXPECT_EQ(info->fieldCount, 2u);
	EXPECT_EQ(info->parent, Object::GetReflectType());
}

TEST(ReflectFieldTest, FindFieldByName) {
	const TypeInfo* info = MockComponent::GetReflectType();
	const FieldInfo* hp = FindField(info, StringHash("health"));
	ASSERT_NE(hp, nullptr);
	EXPECT_EQ(hp->name, StringHash("health"));
	EXPECT_EQ(hp->size, sizeof(f32));
}

TEST(ReflectFieldTest, FindFieldReturnsNullForUnknownName) {
	const TypeInfo* info = MockComponent::GetReflectType();
	EXPECT_EQ(FindField(info, StringHash("nonexistent")), nullptr);
}

TEST(ReflectFieldTest, GetFieldReturnsCurrentValue) {
	MockComponent comp;
	comp.health = 55.5f;

	Variant value;
	bool ok = GetField(&comp, MockComponent::GetReflectType(), StringHash("health"), &value);

	EXPECT_TRUE(ok);
	EXPECT_TRUE(value.Is<f32>());
	EXPECT_FLOAT_EQ(value.As<f32>(), 55.5f);
}

TEST(ReflectFieldTest, SetFieldUpdatesMemoryDirectly) {
	MockComponent comp;
	Variant newValue(static_cast<f32>(77.0f));

	bool ok = SetField(&comp, MockComponent::GetReflectType(), StringHash("health"), &newValue);
	EXPECT_TRUE(ok);
	EXPECT_FLOAT_EQ(comp.health, 77.0f);
}

TEST(ReflectFieldTest, SetFieldRejectsTypeMismatch) {
	MockComponent comp;
	comp.health = 1.0f;
	Variant wrong(str("not a float"));

	bool ok = SetField(&comp, MockComponent::GetReflectType(), StringHash("health"), &wrong);
	EXPECT_FALSE(ok);
	EXPECT_FLOAT_EQ(comp.health, 1.0f);
}

TEST(ReflectFieldTest, GetFieldOnUnknownNameReturnsFalse) {
	MockComponent comp;
	Variant value;
	EXPECT_FALSE(GetField(&comp, MockComponent::GetReflectType(), StringHash("ghost"), &value));
}

TEST(ForEachFieldTest, VisitsEveryDeclaredField) {
	MockComponent comp;
	comp.health = 10.0f;
	comp.damage = 20;

	int visited = 0;
	ForEachField(&comp, MockComponent::GetReflectType(), [&](const FieldInfo* field, const Variant* value) {
			(void)value;
			if (field->name == StringHash("health") || field->name == StringHash("damage"))
				visited++; }, FieldWalkFlagNone);

	EXPECT_EQ(visited, 2);
}

TEST(ForEachFieldTest, IncludeInheritedVisitsParentFields) {
	DerivedThing t;
	int sawBase = 0;
	int sawDerived = 0;
	ForEachField(&t, DerivedThing::GetReflectType(), [&](const FieldInfo* field, const Variant* value) {
			(void)value;
			if (field->name == StringHash("baseValue"))
				sawBase++;
			if (field->name == StringHash("derivedExtra"))
				sawDerived++; }, FieldWalkFlagIncludeInherited);

	EXPECT_EQ(sawBase, 1);
	EXPECT_EQ(sawDerived, 1);
}

TEST(ForEachFieldTest, RawWalkProducesDirectPointers) {
	MockComponent comp;
	comp.health = 42.0f;
	comp.damage = 99;

	ForEachFieldRaw(&comp, MockComponent::GetReflectType(), [&](const FieldInfo* field, void* fieldPtr) {
			ASSERT_NE(fieldPtr, nullptr);
			if (field->name == StringHash("health")) {
				EXPECT_FLOAT_EQ(*static_cast<f32*>(fieldPtr), 42.0f);
				*static_cast<f32*>(fieldPtr) = 0.0f;
			} else if (field->name == StringHash("damage")) {
				EXPECT_EQ(*static_cast<i32*>(fieldPtr), 99);
			} }, FieldWalkFlagNone);

	EXPECT_FLOAT_EQ(comp.health, 0.0f);
}

TEST(WalkFilterTest, ExcludeHiddenSkipsHiddenField) {
	FilteredComponent comp;
	bool sawHidden = false;
	ForEachField(&comp, FilteredComponent::GetReflectType(), [&](const FieldInfo* field, const Variant* v) {
			(void)v;
			if (field->name == StringHash("hidden"))
				sawHidden = true; }, FieldWalkFlagExcludeHidden);

	EXPECT_FALSE(sawHidden);
}

TEST(WalkFilterTest, ExcludeTransientSkipsTransientField) {
	FilteredComponent comp;
	bool sawRuntime = false;
	ForEachField(&comp, FilteredComponent::GetReflectType(), [&](const FieldInfo* field, const Variant* v) {
			(void)v;
			if (field->name == StringHash("runtimeOnly"))
				sawRuntime = true; }, FieldWalkFlagExcludeTransient);

	EXPECT_FALSE(sawRuntime);
}

TEST(WalkFilterTest, NoFiltersVisitsEverything) {
	FilteredComponent comp;
	int visitedCount = 0;
	ForEachField(&comp, FilteredComponent::GetReflectType(), [&](const FieldInfo* field, const Variant* v) {
			(void)field;
			(void)v;
			visitedCount++; }, FieldWalkFlagNone);

	EXPECT_EQ(visitedCount, 3);
}

TEST(AttributeTest, RangeAttributeStored) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();
	const FieldInfo* volume = FindField(info, StringHash("volume"));
	ASSERT_NE(volume, nullptr);

	const AttributeInfo* range = FindFieldAttribute(volume, StringHash("Range"));
	ASSERT_NE(range, nullptr);

	const AttrRangeData* data = range->value.TryGet<AttrRangeData>();
	ASSERT_NE(data, nullptr);
	EXPECT_FLOAT_EQ(data->min, 0.0f);
	EXPECT_FLOAT_EQ(data->max, 1.0f);
}

TEST(AttributeTest, TooltipAttributeStored) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();
	const FieldInfo* channel = FindField(info, StringHash("channel"));
	ASSERT_NE(channel, nullptr);

	const AttributeInfo* tooltip = FindFieldAttribute(channel, StringHash("Tooltip"));
	ASSERT_NE(tooltip, nullptr);

	const str* txt = tooltip->value.TryGet<str>();
	ASSERT_NE(txt, nullptr);
	EXPECT_EQ(*txt, "Audio channel index");
}

TEST(AttributeTest, ReadOnlyAttributeSetsFlag) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();
	const FieldInfo* label = FindField(info, StringHash("label"));
	ASSERT_NE(label, nullptr);
	EXPECT_NE(label->flags & FieldFlagReadOnly, 0u);
}

TEST(AttributeTest, HiddenAttributeSetsFlag) {
	const TypeInfo* info = FilteredComponent::GetReflectType();
	const FieldInfo* hidden = FindField(info, StringHash("hidden"));
	ASSERT_NE(hidden, nullptr);
	EXPECT_EQ(hidden->flags & FieldFlagEditorVisible, 0u);
}

TEST(AttributeTest, TransientAttributeSetsFlag) {
	const TypeInfo* info = FilteredComponent::GetReflectType();
	const FieldInfo* runtimeOnly = FindField(info, StringHash("runtimeOnly"));
	ASSERT_NE(runtimeOnly, nullptr);
	EXPECT_NE(runtimeOnly->flags & FieldFlagTransient, 0u);
}

TEST(MethodTest, FindMethodReturnsRegistered) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();
	const MethodInfo* reset = FindMethod(info, StringHash("Reset"));
	ASSERT_NE(reset, nullptr);
	EXPECT_EQ(reset->paramCount, 0u);
}

TEST(MethodTest, MethodInfoRecordsItsSourceName) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();

	const MethodInfo* reset = FindMethod(info, StringHash("Reset"));
	ASSERT_NE(reset, nullptr);
	ASSERT_NE(reset->displayName, nullptr);
	EXPECT_STREQ(reset->displayName, "Reset");

	const MethodInfo* dbl = FindMethod(info, StringHash("Double"));
	ASSERT_NE(dbl, nullptr);
	ASSERT_NE(dbl->displayName, nullptr);
	EXPECT_STREQ(dbl->displayName, "Double");
	EXPECT_EQ(dbl->paramCount, 1u);
}

TEST(MethodTest, DisplayNameRehashesToTheRecordedName) {
	const TypeInfo* info = AnnotatedComponent::GetReflectType();
	for (u32 m = 0; m < info->methodCount; ++m) {
		const MethodInfo& method = info->methods[m];
		ASSERT_NE(method.displayName, nullptr);
		EXPECT_EQ(StringHash(method.displayName), method.name);
	}
}

TEST(MethodTest, InvokeMethodExecutesVoidMethod) {
	AnnotatedComponent comp;
	comp.volume = 0.5f;
	comp.channel = 42;
	comp.muted = true;

	bool ok = InvokeMethod(&comp, AnnotatedComponent::GetReflectType(),
						   StringHash("Reset"), nullptr, 0, nullptr);

	EXPECT_TRUE(ok);
	EXPECT_FLOAT_EQ(comp.volume, 1.0f);
	EXPECT_EQ(comp.channel, 0);
	EXPECT_FALSE(comp.muted);
}

TEST(MethodTest, InvokeMethodReturnsValue) {
	AnnotatedComponent comp;
	Variant args[1];
	args[0] = static_cast<i32>(21);

	Variant out;
	bool ok = InvokeMethod(&comp, AnnotatedComponent::GetReflectType(),
						   StringHash("Double"), args, 1, &out);

	EXPECT_TRUE(ok);
	EXPECT_TRUE(out.Is<i32>());
	EXPECT_EQ(out.As<i32>(), 42);
}

TEST(MethodTest, InvokeMethodArgumentCountMismatch) {
	AnnotatedComponent comp;
	Variant out;

	bool ok = InvokeMethod(&comp, AnnotatedComponent::GetReflectType(),
						   StringHash("Double"), nullptr, 0, &out);
	EXPECT_FALSE(ok);
}

TEST(MethodTest, InvokeUnknownMethodFails) {
	AnnotatedComponent comp;
	bool ok = InvokeMethod(&comp, AnnotatedComponent::GetReflectType(),
						   StringHash("DoesNotExist"), nullptr, 0, nullptr);
	EXPECT_FALSE(ok);
}

TEST(GlobalQueryTest, GetStaticTypeForPrimitives) {
	EXPECT_EQ(GetStaticType<i32>()->name, StringHash("i32"));
	EXPECT_EQ(GetStaticType<f32>()->name, StringHash("f32"));
	EXPECT_EQ(GetStaticType<bool>()->name, StringHash("bool"));
}

TEST(GlobalQueryTest, GetStaticTypeForReflectedType) {
	EXPECT_EQ(GetStaticType<MockComponent>()->name, StringHash("MockComponent"));
}

TEST(GlobalQueryTest, FieldTypePointerMatchesStaticType) {
	const TypeInfo* info = MockComponent::GetReflectType();
	const FieldInfo* health = FindField(info, StringHash("health"));
	ASSERT_NE(health, nullptr);
	EXPECT_EQ(health->fieldType, GetStaticType<f32>());
}

namespace {

	class UnreflectedDerived : public BaseThing {
	public:
		i32 extra = 0;
	};

	struct PlainPoint {
		f32 x = 0.0f;
		f32 y = 0.0f;

		SLT_REFLECT_VALUE(PlainPoint)
		SLT_REFLECT_FIELD(x)
		SLT_REFLECT_FIELD(y)
		SLT_END_REFLECT()
	};
	static_assert(sizeof(PlainPoint) == 2 * sizeof(f32), "SLT_REFLECT_VALUE must not add a vtable");

} // namespace

TEST(RuntimeTypeTest, GetTypeThroughBaseIsConcrete) {
	DerivedThing derived;
	Object* base = &derived;
	EXPECT_EQ(base->GetType(), DerivedThing::GetReflectType());
	EXPECT_NE(base->GetType(), BaseThing::GetReflectType());
}

TEST(RuntimeTypeTest, GetTypeMatchesStaticType) {
	BaseThing thing;
	EXPECT_EQ(thing.GetType(), BaseThing::GetReflectType());

	Object object;
	EXPECT_EQ(object.GetType(), Object::GetReflectType());
}

TEST(RuntimeTypeTest, GetTypeFeedsIsDerivedFrom) {
	DerivedThing derived;
	Object* base = &derived;
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(base->GetType(), BaseThing::GetReflectType()));
	EXPECT_FALSE(TypeRegistry::IsDerivedFrom(base->GetType(), UnrelatedThing::GetReflectType()));
}

TEST(RuntimeTypeTest, UnreflectedSubclassReportsNearestReflectedAncestor) {
	UnreflectedDerived instance;
	Object* base = &instance;
	EXPECT_EQ(base->GetType(), BaseThing::GetReflectType());
}

TEST(RuntimeTypeTest, ValueStructReflectsWithoutObjectMachinery) {
	const TypeInfo* info = PlainPoint::GetReflectType();
	ASSERT_NE(info, nullptr);
	EXPECT_EQ(info->name, StringHash("PlainPoint"));
	EXPECT_EQ(info->fieldCount, 2u);

	PlainPoint point{1.5f, -2.5f};
	Variant out;
	EXPECT_TRUE(GetField(&point, info, StringHash("x"), &out));
	EXPECT_FLOAT_EQ(out.As<f32>(), 1.5f);
}

namespace {

	struct ThrowingPayload {
		u8 blob[64] = {};
		bool shouldThrow = false;

		ThrowingPayload() = default;
		ThrowingPayload(const ThrowingPayload& other) : shouldThrow(other.shouldThrow) {
			if (shouldThrow) {
				throw std::runtime_error("ThrowingPayload copy failure");
			}
		}
		ThrowingPayload& operator=(const ThrowingPayload&) = default;
	};

} // anonymous namespace

namespace Shalltear {
	template <>
	struct VariantPrimitiveTraits<ThrowingPayload> {
		static StringHash TypeId() { return StringHash("ThrowingPayload"); }
	};
} // namespace Shalltear

TEST(VariantExceptionTest, ThrowingConstructorLeavesEmpty) {
	size_t bytesBefore = MemoryManager::GetBytesForTag(MemTag::Core);

	ThrowingPayload source;
	source.shouldThrow = true;

	EXPECT_THROW({ Variant v(source); }, std::runtime_error);

	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Core), bytesBefore);
}

TEST(VariantExceptionTest, ThrowingAssignmentLeavesVariantEmpty) {
	ThrowingPayload source;
	source.shouldThrow = true;

	Variant v(i32(7));
	EXPECT_THROW({ v = source; }, std::runtime_error);

	EXPECT_TRUE(v.IsEmpty());
	EXPECT_FALSE(v.Is<ThrowingPayload>());
}

namespace {

	i32 _factoryWidgetLive = 0;

	class FactoryWidget : public Object {
	public:
		i32 charge = 7;

		SLT_REFLECT_COMPONENT(FactoryWidget, Object)
		SLT_REFLECT_FIELD(charge)
		SLT_END_REFLECT()

		FactoryWidget() { ++_factoryWidgetLive; }
		~FactoryWidget() override { --_factoryWidgetLive; }
	};

	class AbstractGadget : public Object {
	public:
		SLT_REFLECT_COMPONENT_ABSTRACT(AbstractGadget, Object)
		SLT_END_REFLECT()
	};

	class NoFactoryType : public Object {
	public:
		SLT_REFLECT_CLASS(NoFactoryType)
		SLT_REFLECT_PARENT(Object)
		SLT_END_REFLECT()
	};

} // anonymous namespace

TEST(InstanceFactoryTest, ComponentMacroPopulatesFactoryAndFlags) {
	const TypeInfo* info = FactoryWidget::GetReflectType();
	ASSERT_NE(info, nullptr);
	EXPECT_NE(info->createFn, nullptr);
	EXPECT_NE(info->constructor, nullptr);
	EXPECT_NE(info->destructor, nullptr);
	EXPECT_TRUE(info->flags & TypeFlagComponent);
	EXPECT_FALSE(info->flags & TypeFlagAbstract);
}

TEST(InstanceFactoryTest, CreateObjectInstanceConstructsOnTaggedHeap) {
	size_t bytesBefore = MemoryManager::GetBytesForTag(MemTag::Scene);
	i32 liveBefore = _factoryWidgetLive;

	Object* obj = TypeRegistry::CreateObjectInstance(FactoryWidget::GetReflectType(), MemTag::Scene);
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(_factoryWidgetLive, liveBefore + 1);
	EXPECT_GT(MemoryManager::GetBytesForTag(MemTag::Scene), bytesBefore);

	FactoryWidget* widget = static_cast<FactoryWidget*>(obj);
	EXPECT_EQ(widget->charge, 7);
	EXPECT_NE(obj->GetInstanceId(), 0u);
	EXPECT_EQ(ObjectRegistry::Resolve(obj->GetInstanceId()), obj);

	TypeRegistry::DestroyObjectInstance(obj);
	EXPECT_EQ(_factoryWidgetLive, liveBefore);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scene), bytesBefore);
}

TEST(InstanceFactoryTest, AbstractTypeRefusesCreation) {
	const TypeInfo* info = AbstractGadget::GetReflectType();
	ASSERT_NE(info, nullptr);
	EXPECT_TRUE(info->flags & TypeFlagAbstract);
	EXPECT_EQ(info->createFn, nullptr);
	EXPECT_EQ(TypeRegistry::CreateObjectInstance(info, MemTag::Scene), nullptr);
}

TEST(InstanceFactoryTest, NonCreatableTypeRefusesCreation) {
	const TypeInfo* info = NoFactoryType::GetReflectType();
	ASSERT_NE(info, nullptr);
	EXPECT_EQ(info->createFn, nullptr);
	EXPECT_EQ(TypeRegistry::CreateObjectInstance(info, MemTag::Scene), nullptr);
}

TEST(InstanceFactoryTest, NullInputsAreSafe) {
	EXPECT_EQ(TypeRegistry::CreateObjectInstance(nullptr, MemTag::Scene), nullptr);
	TypeRegistry::DestroyObjectInstance(nullptr);
}

namespace {

	class EagerRegisteredType : public Object {
	public:
		i32 payload = 0;

		SLT_REFLECT_COMPONENT(EagerRegisteredType, Object)
		SLT_REFLECT_FIELD(payload)
		SLT_END_REFLECT()
	};

	SLT_REGISTER_TYPE(EagerRegisteredType)

} // anonymous namespace

TEST(EagerRegistrationTest, PendingDrainMakesTypeFindable) {
	TypeRegistry::RegisterPendingTypes();

	const TypeInfo* found = TypeRegistry::FindType(StringHash("EagerRegisteredType"));
	ASSERT_NE(found, nullptr);
	EXPECT_TRUE(found->flags & TypeFlagComponent);
	ASSERT_NE(found->createFn, nullptr);

	Object* obj = TypeRegistry::CreateObjectInstance(found, MemTag::Scene);
	ASSERT_NE(obj, nullptr);
	TypeRegistry::DestroyObjectInstance(obj);
}

TEST(EagerRegistrationTest, RepeatDrainIsIdempotent) {
	TypeRegistry::RegisterPendingTypes();
	u32 countAfterFirst = TypeRegistry::GetTypeCount();
	TypeRegistry::RegisterPendingTypes();
	EXPECT_EQ(TypeRegistry::GetTypeCount(), countAfterFirst);
}

namespace {

	class EpochProbeType : public Object {
	public:
		i32 payload = 0;

		SLT_REFLECT_CLASS(EpochProbeType)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(payload)
		SLT_END_REFLECT()
	};

	const StringHash EpochProbe{"EpochProbeType"};

	class TypeRegistryEpochTest : public ::testing::Test {
	protected:
		void TearDown() override {
			TypeRegistry::Unregister(EpochProbe);
		}

		static const TypeInfo* RestampProbe() {
			TypeRegistry::Unregister(EpochProbe);
			EXPECT_EQ(TypeRegistry::FindType(EpochProbe), nullptr);
			return TypeRegistry::Register(const_cast<TypeInfo*>(EpochProbeType::GetReflectType()));
		}
	};

} // anonymous namespace

TEST_F(TypeRegistryEpochTest, SweepThatMatchesNothingStillBumpsEpoch) {
	const TypeInfo* first = RestampProbe();
	ASSERT_NE(first, nullptr);

	const u32 before = first->moduleEpoch;

	int notAnImageBase = 0;
	ASSERT_EQ(TypeRegistry::UnregisterOwnedBy(&notAnImageBase), 0u);

	const TypeInfo* second = RestampProbe();
	ASSERT_NE(second, nullptr);

	EXPECT_GT(second->moduleEpoch, before);
}

TEST_F(TypeRegistryEpochTest, AnUnrelatedSweepStillInvalidatesTheRef) {
	ASSERT_NE(RestampProbe(), nullptr);

	FieldRef ref{EpochProbe, StringHash("payload")};
	const FieldInfo* resolved = ref.Resolve();
	ASSERT_NE(resolved, nullptr);

	TypeRegistry::Unregister(EpochProbe);
	EXPECT_EQ(ref.Resolve(), resolved);

	int notAnImageBase = 0;
	ASSERT_EQ(TypeRegistry::UnregisterOwnedBy(&notAnImageBase), 0u);
	EXPECT_EQ(ref.Resolve(), nullptr);
}

namespace ReflectEnumFixture {
	enum class Season : u8 {
		Spring = 0,
		Summer = 1,
		Autumn = 2,
		Winter = 3
	};

	enum class Temperature : i8 {
		Freezing = -1,
		Mild = 0,
		Hot = 100
	};
} // namespace ReflectEnumFixture

SLT_REFLECT_ENUM(::ReflectEnumFixture::Season)
SLT_ENUM_VALUE(Spring)
SLT_ENUM_VALUE(Summer)
SLT_ENUM_VALUE(Autumn)
SLT_ENUM_VALUE(Winter)
SLT_END_ENUM()

SLT_REFLECT_ENUM(::ReflectEnumFixture::Temperature)
SLT_ENUM_VALUE(Freezing)
SLT_ENUM_VALUE(Mild)
SLT_ENUM_VALUE(Hot)
SLT_END_ENUM()

namespace {

	using ReflectEnumFixture::Season;
	using ReflectEnumFixture::Temperature;

	class EnumHolder : public Object {
	public:
		Season season = Season::Spring;
		Temperature temperature = Temperature::Mild;
		i32 plainInt = 5;

		SLT_REFLECT_CLASS(EnumHolder)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(season)
		SLT_REFLECT_FIELD(temperature)
		SLT_REFLECT_FIELD(plainInt)
		SLT_END_REFLECT()
	};

} // anonymous namespace

TEST(EnumReflectionTest, ReflectedEnumRegistersUnderUnqualifiedName) {
	const TypeInfo* type = GetStaticType<Season>();
	ASSERT_NE(type, nullptr);
	EXPECT_EQ(type->name, StringHash("Season"));
	EXPECT_STREQ(type->displayName, "Season");
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Season")), type);
}

TEST(EnumReflectionTest, ConstantsCarryTheirNamesAndValues) {
	const TypeInfo* type = GetStaticType<Season>();
	ASSERT_NE(type, nullptr);
	ASSERT_EQ(type->enumConstantCount, 4u);

	EXPECT_STREQ(type->enumConstants[0].displayName, "Spring");
	EXPECT_STREQ(type->enumConstants[3].displayName, "Winter");
	EXPECT_EQ(type->enumConstants[2].value, 2);
	EXPECT_EQ(type->enumConstants[2].name, StringHash("Autumn"));
}

TEST(EnumReflectionTest, EnumFlaggedAsPrimitive) {
	const TypeInfo* type = GetStaticType<Season>();
	ASSERT_NE(type, nullptr);
	EXPECT_TRUE(type->flags & TypeFlagEnum);
	EXPECT_TRUE(type->flags & TypeFlagPrimitive);
	EXPECT_TRUE(type->flags & TypeFlagTriviallyCopyable);
	EXPECT_FALSE(type->flags & TypeFlagHeapMetadata);
	EXPECT_EQ(type->size, sizeof(Season));
}

TEST(EnumReflectionTest, NegativeEnumeratorSurvivesNarrowUnderlyingType) {
	const TypeInfo* type = GetStaticType<Temperature>();
	ASSERT_NE(type, nullptr);
	ASSERT_EQ(type->enumConstantCount, 3u);
	EXPECT_EQ(type->enumConstants[0].value, -1);
	EXPECT_EQ(type->enumConstants[2].value, 100);
}

TEST(EnumReflectionTest, EnumFieldFlaggedFromType) {
	const TypeInfo* type = EnumHolder::GetReflectType();
	const FieldInfo* season = FindField(type, StringHash("season"));
	ASSERT_NE(season, nullptr);
	EXPECT_TRUE(season->flags & FieldFlagEnum);
	EXPECT_EQ(season->fieldType, GetStaticType<Season>());

	const FieldInfo* plain = FindField(type, StringHash("plainInt"));
	ASSERT_NE(plain, nullptr);
	EXPECT_FALSE(plain->flags & FieldFlagEnum);
}

TEST(EnumReflectionTest, EnumFieldCrossesVariantAsI64) {
	EnumHolder holder;
	holder.season = Season::Autumn;

	Variant value;
	ASSERT_TRUE(GetField(&holder, holder.GetType(), StringHash("season"), &value));
	EXPECT_TRUE(value.Is<i64>());
	EXPECT_EQ(value.As<i64>(), 2);

	Variant winter(static_cast<i64>(3));
	ASSERT_TRUE(SetField(&holder, holder.GetType(), StringHash("season"), &winter));
	EXPECT_EQ(holder.season, Season::Winter);
}

TEST(EnumReflectionTest, WrongTypedVariantLeavesEnumFieldAlone) {
	EnumHolder holder;
	holder.season = Season::Summer;
	Variant wrongType(str("Winter"));
	SetField(&holder, holder.GetType(), StringHash("season"), &wrongType);
	EXPECT_EQ(holder.season, Season::Summer);
}

TEST(EnumReflectionTest, RegisteringEnumAddsNoVariantDispatchEntry) {
	EnumHolder holder;
	Variant value;
	ASSERT_TRUE(GetField(&holder, holder.GetType(), StringHash("temperature"), &value));

	EXPECT_EQ(VariantDispatch::Lookup(StringHash("Temperature")), nullptr);
	EXPECT_EQ(VariantDispatch::Lookup(StringHash("Season")), nullptr);

	EXPECT_NE(VariantDispatch::Lookup(StringHash("i64")), nullptr);

	size_t after = VariantDispatch::GetEntryCount();
	Variant second;
	ASSERT_TRUE(GetField(&holder, holder.GetType(), StringHash("season"), &second));
	EXPECT_EQ(VariantDispatch::GetEntryCount(), after);
}

namespace {

	class LateRegisteredSpecimen : public Object {
	public:
		i32 value = 3;

		SLT_REFLECT_CLASS(LateRegisteredSpecimen)
		SLT_REFLECT_PARENT(Object)
		SLT_REFLECT_FIELD(value)
		SLT_END_REFLECT()
	};

} // anonymous namespace

TEST(ReflectionTest, EnumConstantWithoutDisplayNameMatchesHash) {
	const EnumConstantInfo constants[] = {
		{StringHash("Slow"), "Slow", 3},
		{StringHash("Fast"), nullptr, 7},
	};
	TypeInfo type{};
	type.size = 4;
	type.enumConstants = constants;
	type.enumConstantCount = 2;

	EXPECT_EQ(FindEnumConstantByName(&type, "Slow"), &constants[0]);
	EXPECT_EQ(FindEnumConstantByName(&type, "Fast"), &constants[1]);
	EXPECT_EQ(FindEnumConstantByName(&type, "Sideways"), nullptr);
	EXPECT_EQ(FindEnumConstantByName(&type, ""), nullptr);
	EXPECT_EQ(FindEnumConstantByName(nullptr, "Slow"), nullptr);
}

TEST(ReflectionTest, VisitorMayAskUnregisteredType) {
	TypeRegistry::RegisterPendingTypes();
	ASSERT_EQ(TypeRegistry::FindType(StringHash("LateRegisteredSpecimen")), nullptr);

	u32 visited = 0;
	TypeRegistry::ForEachType(
		[](const TypeInfo*, void* user) {
			++*static_cast<u32*>(user);
			LateRegisteredSpecimen::GetReflectType();
		},
		&visited);

	EXPECT_GT(visited, 0u);
	EXPECT_NE(TypeRegistry::FindType(StringHash("LateRegisteredSpecimen")), nullptr);
}
