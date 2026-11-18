#include "TestsPCH.h"

#ifdef SHALLTEAR_SHARED

#include "CSharpRuntimeFixture.h"

using namespace Shalltear;
using namespace Shalltear::CSharpTests;
using Shalltear::CSharpConformance::View;

TEST_F(CSharpRuntimeFixture, StartsAndAttaches) {
	ASSERT_FALSE(_hooks->findSDKRoot().empty());
	ASSERT_TRUE(Attach()) << _error;
	EXPECT_TRUE(_hooks->isAttached());

	const SltManagedAPI& managed = _hooks->getManaged();
	EXPECT_EQ(managed.abiVersion, static_cast<uint32_t>(SLT_BRIDGE_ABI_VERSION));
	EXPECT_EQ(managed.layoutHash, SLT_BRIDGE_LAYOUT_HASH);
	EXPECT_NE(managed.loadUserAssemblies, nullptr);
	EXPECT_NE(managed.unloadUserAssemblies, nullptr);
	EXPECT_NE(managed.shutdown, nullptr);
	EXPECT_EQ(managed.getConformanceTestCount(), 0);

	_hooks->detach();
	EXPECT_FALSE(_hooks->isAttached());
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, SecondHostStartIsRefused) {
	ASSERT_NE(Host(), nullptr) << _error;

	str error;
	EXPECT_EQ(_hooks->startHost(_hooks->findSDKRoot(), _hooks->runtimeVersion, _hooks->getRuntimeAssemblyPath(), error), nullptr);
	EXPECT_NE(error.find("already started"), str::npos) << error;
	EXPECT_NE(_hooks->getHostEntry(), nullptr);
}

TEST_F(CSharpRuntimeFixture, MissingSDKNamesTheLibrary) {
	ASSERT_EQ(_hooks->getHostEntry(), nullptr);
	str error;
	EXPECT_EQ(_hooks->startHost("Z:/no/such/sdk", _hooks->runtimeVersion, _hooks->getRuntimeAssemblyPath(), error), nullptr);
	EXPECT_NE(error.find("Z:/no/such/sdk/host/fxr/"), str::npos) << error;
	EXPECT_EQ(_hooks->getHostEntry(), nullptr);

	ASSERT_TRUE(Attach()) << _error;
}

TEST_F(CSharpRuntimeFixture, MismatchedLayoutNamesBothSides) {
	const SltInitializeFn entry = Host();
	ASSERT_NE(entry, nullptr) << _error;

	SltEngineAPI engine = _hooks->makeEngineAPI();
	engine.layoutHash = 0x0bad0bad0bad0badull;
	str error;
	EXPECT_FALSE(_hooks->attachAs(engine, entry, error));
	EXPECT_FALSE(_hooks->isAttached());
	EXPECT_NE(error.find("layout 0bad0bad0bad0bad"), str::npos) << error;
	EXPECT_NE(error.find(std::format("runtime ABI {} layout {:016x}", SLT_BRIDGE_ABI_VERSION, SLT_BRIDGE_LAYOUT_HASH)), str::npos) << error;

	engine = _hooks->makeEngineAPI();
	engine.abiVersion = 77;
	error.clear();
	EXPECT_FALSE(_hooks->attachAs(engine, entry, error));
	EXPECT_NE(error.find("bridge ABI 77"), str::npos) << error;

	EXPECT_TRUE(Attach()) << _error;
}

TEST_F(CSharpRuntimeFixture, ConformanceSuitePasses) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	const Time::FrameScope frame(CSharpConformance::MakeFrame());
	const SltManagedAPI& managed = _hooks->getManaged();
	const int32_t count = managed.getConformanceTestCount();
	int32_t ran = 0;
	for (int32_t index = 0; index < count; ++index) {
		SltString name{};
		ASSERT_EQ(managed.getConformanceTestName(index, &name), SLT_OK);
		const str caseName(View(name));
		if (CSharpConformance::IsProbe(caseName)) {
			continue;
		}
		SltString message{};
		EXPECT_EQ(managed.runConformanceTest(index, &message), SLT_OK) << caseName << ": " << View(message);
		++ran;
	}
	EXPECT_GE(ran, 29);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();

	EXPECT_EQ(managed.unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(managed.getConformanceTestCount(), 0);
}

TEST_F(CSharpRuntimeFixture, SecondLoadIsRefused) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	const int32_t count = _hooks->getManaged().getConformanceTestCount();
	ASSERT_GT(count, 0);

	EXPECT_FALSE(LoadConformance());
	EXPECT_EQ(_log.Delivered().Count("user assemblies are already loaded"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_hooks->getManaged().getConformanceTestCount(), count);

	str message;
	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, FailedCaseCarriesItsMessage) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.Fails", message), CASE_FAILED);
	EXPECT_EQ(message, "failed as asked");
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpRuntimeFixture, ManagedExceptionIsReported) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.ThrowsOnCallingThread", message), MANAGED_FAILED);
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown on the calling thread"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("ThrowsOnCallingThread"), 1u) << _log.Delivered().Joined();

	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, ThreadExceptionIsReported) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	str message;
	EXPECT_EQ(Run("Probe.ThrowsOnStartedThread", message), SLT_OK) << message;
	EXPECT_EQ(_log.Delivered().Count("System.InvalidOperationException: thrown on a started thread"), 1u) << _log.Delivered().Joined();

	EXPECT_EQ(Run("BridgeTests.FailureCarriesItsMessage", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, NativeExceptionBecomesErrorCode) {
	ASSERT_TRUE(Attach()) << _error;
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();

	SltEngineAPI engine = _hooks->makeEngineAPI();
	ASSERT_NE(engine.throwForTest, nullptr);
	EXPECT_EQ(engine.throwForTest(), SLT_ERR_EXCEPTION);
	SltString text{};
	engine.getLastError(&text);
	EXPECT_EQ(View(text), "thrown for a test");

	str message;
	EXPECT_EQ(Run("BridgeTests.NativeThrowBecomesEngineException", message), SLT_OK) << message;
}

TEST_F(CSharpRuntimeFixture, MissingAssemblyIsReported) {
	ASSERT_TRUE(Attach()) << _error;

	const str path = "Z:/no/such/Scripts.dll";
	const SltString paths[] = {SltString{path.data(), static_cast<int32_t>(path.size())}};
	EXPECT_EQ(_hooks->getManaged().loadUserAssemblies(paths, 1), MANAGED_FAILED);
	EXPECT_GE(_log.Delivered().Count("Scripts.dll"), 1u) << _log.Delivered().Joined();

	EXPECT_TRUE(LoadConformance()) << _log.Delivered().Joined();
}

#endif
