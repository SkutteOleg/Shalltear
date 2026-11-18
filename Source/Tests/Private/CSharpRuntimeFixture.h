#pragma once

#ifdef SHALLTEAR_SHARED

#include "CSharpConformance.h"
#include "DeviceLossHarness.h"

#include "ScriptTestHooks.h"

#include "Shalltear/Core/Module.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace Shalltear::CSharpTests {

	inline constexpr int32_t MANAGED_FAILED = -1;
	inline constexpr int32_t CASE_FAILED = 1;

	class CSharpRuntimeFixture : public ::testing::Test {
	protected:
		DeviceLossTest::ScopedLog _log;
		ModuleHandle _package;
		const CSharpImpl::ScriptTestHooks* _hooks = nullptr;
		str _error;

		void SetUp() override {
			ModuleSystem::Initialize();
			_package = ModuleSystem::Load(Path(str("CSharp") + SLT_TEST_DLL_SUFFIX));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_package));
			const auto get = reinterpret_cast<CSharpImpl::GetScriptTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), CSharpImpl::SCRIPT_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_hooks = get();
		}

		void TearDown() override {
			if (_hooks) {
				_hooks->detach();
			}
			ModuleSystem::Terminate();
		}

		SltInitializeFn Host() {
			if (const SltInitializeFn entry = _hooks->getHostEntry()) {
				return entry;
			}
			return _hooks->startHost(_hooks->findSDKRoot(), _hooks->runtimeVersion, _hooks->getRuntimeAssemblyPath(), _error);
		}

		bool Attach() {
			const SltInitializeFn entry = Host();
			return entry && _hooks->attachAs(_hooks->makeEngineAPI(), entry, _error);
		}

		bool LoadConformance() {
			const str path = (std::filesystem::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" /
							  "Shalltear.Conformance.dll")
								 .generic_string();
			const SltString paths[] = {SltString{path.data(), static_cast<int32_t>(path.size())}};
			return _hooks->getManaged().loadUserAssemblies(paths, 1) == SLT_OK;
		}

		int32_t Find(std::string_view wanted) {
			const SltManagedAPI& managed = _hooks->getManaged();
			for (int32_t index = 0; index < managed.getConformanceTestCount(); ++index) {
				SltString name{};
				if (managed.getConformanceTestName(index, &name) == SLT_OK && CSharpConformance::View(name) == wanted) {
					return index;
				}
			}
			return -1;
		}

		int32_t Run(std::string_view name, str& outMessage) {
			const int32_t index = Find(name);
			if (index < 0) {
				outMessage = "no such case";
				return -2;
			}
			SltString message{};
			const int32_t result = _hooks->getManaged().runConformanceTest(index, &message);
			outMessage = str(CSharpConformance::View(message));
			return result;
		}
	};

} // namespace Shalltear::CSharpTests

#endif
