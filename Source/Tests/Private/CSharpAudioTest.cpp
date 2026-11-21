#include "TestsPCH.h"

#ifdef SHALLTEAR_SHARED

#include "CSharpConformance.h"
#include "CSharpRuntimeFixture.h"

#include "Shalltear/Core/Scene.h"

#include <optional>

using namespace Shalltear;
using namespace Shalltear::CSharpTests;

namespace {

	class CSharpAudioFixture : public CSharpRuntimeFixture {
	protected:
		std::optional<CSharpConformance::AudioStore> _store;

		void SetUp() override {
			_store.emplace();
			CSharpRuntimeFixture::SetUp();
		}

		void TearDown() override {
			CSharpRuntimeFixture::TearDown();
			_store.reset();
		}

		bool LoadPackages() override { return CSharpConformance::StandUpAudio(nullptr); }

		const char* GetConformanceAssembly() const override { return "Shalltear.Conformance.Audio.dll"; }
	};

} // anonymous namespace

TEST_F(CSharpAudioFixture, ConformanceSuitePasses) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	std::optional<CSharpConformance::AudioRig> rig(std::in_place, _hooks->setDriver, _hooks->setAssetFinder);
	ASSERT_TRUE(rig->IsStaged());
	SceneSystem::CreateScene("World");

	const SltManagedAPI& managed = _hooks->getManaged();
	const int32_t count = managed.getConformanceTestCount();
	for (int32_t index = 0; index < count; ++index) {
		SltString name{};
		ASSERT_EQ(managed.getConformanceTestName(index, &name), SLT_OK);
		const str caseName(CSharpConformance::View(name));
		SltString message{};
		EXPECT_EQ(managed.runConformanceTest(index, &message), SLT_OK) << caseName << ": " << CSharpConformance::View(message);
	}
	EXPECT_EQ(count, CSharpConformance::AUDIO_CASES);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();

	SceneSystem::Terminate();
	rig.reset();
	EXPECT_EQ(managed.unloadUserAssemblies(), SLT_OK);
}

TEST_F(CSharpAudioFixture, ScriptRegistrationsEndAtUnload) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	std::optional<CSharpConformance::AudioRig> rig(std::in_place, _hooks->setDriver, _hooks->setAssetFinder);
	ASSERT_TRUE(rig->IsStaged());
	SceneSystem::CreateScene("World");
	for (u32 round = 0; round < 2; ++round) {
		ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
		str message;
		EXPECT_EQ(Run("AudioTests.RegistrationsHoldUntilUnload", message), SLT_OK) << round << ": " << message;
		EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK) << round;
	}
	SceneSystem::Terminate();
	rig.reset();
}

#endif
