#include "TestsPCH.h"

#include "Shalltear/Editor/CompileStatus.h"

using namespace Shalltear;

namespace {

	struct FakeSource {
		CompileStatus::SourceState state;
		u32 compiles = 0;
		u32 reloads = 0;
	};

	CompileStatus::SourceState ReadState(void* user) {
		return static_cast<FakeSource*>(user)->state;
	}

	void Compile(void* user) {
		++static_cast<FakeSource*>(user)->compiles;
	}

	void Reload(void* user) {
		++static_cast<FakeSource*>(user)->reloads;
	}

	str Heading(void*) {
		return "Mine.Scripts";
	}

	char _firstOwner = 0;
	char _secondOwner = 0;

	class CompileStatusTest : public ::testing::Test {
	protected:
		FakeSource _native;
		FakeSource _managed;

		void TearDown() override {
			CompileStatus::UnregisterOwnedBy(&_firstOwner);
			CompileStatus::UnregisterOwnedBy(&_secondOwner);
		}

		void RegisterBoth() {
			CompileStatus::RegisterSource({"C++", &ReadState, &Compile, &Reload, nullptr, &_native}, &_firstOwner);
			CompileStatus::RegisterSource({"C#", &ReadState, &Compile, &Reload, &Heading, &_managed}, &_secondOwner);
		}

		static Vector<CompileStatus::SourceView> Sources() {
			Vector<CompileStatus::SourceView> sources{GetMemoryResource(MemTag::Editor)};
			CompileStatus::GetSources(sources);
			return sources;
		}
	};

} // anonymous namespace

TEST_F(CompileStatusTest, SourcesCombine) {
	const u32 before = static_cast<u32>(Sources().size());
	RegisterBoth();
	ASSERT_EQ(Sources().size(), before + 2);
	EXPECT_EQ(CompileStatus::GetSourceCount(), before + 2);
	EXPECT_EQ(Sources()[before].name, "C++");
	EXPECT_EQ(Sources()[before].heading, "C++");
	EXPECT_EQ(Sources()[before + 1].name, "C#");
	EXPECT_EQ(Sources()[before + 1].heading, "Mine.Scripts");

	CompileStatus::Summary summary = CompileStatus::Combine();
	EXPECT_EQ(summary.compile, CompileState::Idle);
	EXPECT_EQ(summary.errors, 0u);
	EXPECT_FALSE(summary.reloadHeld);

	_native.state.compile = CompileState::Succeeded;
	_native.state.warnings = 3;
	EXPECT_EQ(CompileStatus::Combine().compile, CompileState::Succeeded);

	_managed.state.compile = CompileState::Failed;
	_managed.state.errors = 2;
	_managed.state.warnings = 1;
	summary = CompileStatus::Combine();
	EXPECT_EQ(summary.compile, CompileState::Failed);
	EXPECT_EQ(summary.errors, 2u);
	EXPECT_EQ(summary.warnings, 4u);
	_native.state.errors = 5;
	EXPECT_EQ(CompileStatus::Combine().errors, 7u);
	_native.state.reloadHeld = true;
	EXPECT_TRUE(CompileStatus::Combine().reloadHeld);
	_native.state.reloadHeld = false;
	EXPECT_FALSE(CompileStatus::Combine().reloadHeld);

	_native.state.compile = CompileState::Configuring;
	EXPECT_EQ(CompileStatus::Combine().compile, CompileState::Configuring);
	_managed.state.compile = CompileState::Compiling;
	EXPECT_EQ(CompileStatus::Combine().compile, CompileState::Compiling);
	_native.state.compile = CompileState::Failed;
	EXPECT_EQ(CompileStatus::Combine().compile, CompileState::Compiling);

	_managed.state.reloadHeld = true;
	EXPECT_TRUE(CompileStatus::Combine().reloadHeld);
	EXPECT_EQ(Sources()[before + 1].state.errors, 2u);
}

TEST_F(CompileStatusTest, RequestsReachEverySource) {
	RegisterBoth();
	FakeSource silent;
	CompileStatus::RegisterSource({"Baked", &ReadState, nullptr, nullptr, nullptr, &silent}, &_secondOwner);

	CompileStatus::RequestCompile();
	EXPECT_EQ(_native.compiles, 1u);
	EXPECT_EQ(_managed.compiles, 1u);

	_native.state.compile = CompileState::Compiling;
	_managed.state.compile = CompileState::Configuring;
	CompileStatus::RequestCompile();
	EXPECT_EQ(_native.compiles, 1u);
	EXPECT_EQ(_managed.compiles, 1u);

	_native.state.compile = CompileState::Failed;
	CompileStatus::RequestCompile();
	EXPECT_EQ(_native.compiles, 2u);
	EXPECT_EQ(_managed.compiles, 1u);

	CompileStatus::RequestReload();
	EXPECT_EQ(_native.reloads, 1u);
	EXPECT_EQ(_managed.reloads, 1u);
	EXPECT_EQ(silent.compiles + silent.reloads, 0u);
}

TEST_F(CompileStatusTest, OwnerSweepRemovesItsSources) {
	const u32 before = static_cast<u32>(Sources().size());
	RegisterBoth();
	CompileStatus::RegisterSource({"Stateless", nullptr, &Compile, &Reload, nullptr, &_managed}, &_secondOwner);
	ASSERT_EQ(Sources().size(), before + 2);

	_managed.state.errors = 5;
	EXPECT_EQ(CompileStatus::UnregisterOwnedBy(&_secondOwner), 1u);
	EXPECT_EQ(Sources().size(), before + 1);
	EXPECT_EQ(CompileStatus::Combine().errors, 0u);
	CompileStatus::RequestCompile();
	EXPECT_EQ(_managed.compiles, 0u);
	EXPECT_EQ(_native.compiles, 1u);
	EXPECT_EQ(CompileStatus::UnregisterOwnedBy(&_secondOwner), 0u);
}
