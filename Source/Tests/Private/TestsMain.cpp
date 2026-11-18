#include "TestsPCH.h"

#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Asset/ImportTier.h"

#include "ImportPipeline.h"
#include "Graphs.h"
#include "NodeShader.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace {

	const int _IMPORT_PIPELINE_TOKEN = 0;
	const int _NODE_SHADER_TOKEN = 0;
	const int _GRAPHS_TOKEN = 0;

	class ImportPipelineEnvironment : public ::testing::Environment {
	public:
		void SetUp() override {
			Shalltear::RegisterImportPipeline(&_IMPORT_PIPELINE_TOKEN);
			Shalltear::RegisterGraphs(&_GRAPHS_TOKEN);
			Shalltear::RegisterNodeShader(&_NODE_SHADER_TOKEN);
		}
	};

	class PendingTypeRegistrationEnvironment : public ::testing::Environment {
	public:
		void SetUp() override {
			Shalltear::TypeRegistry::RegisterPendingTypes();
		}
	};

} // anonymous namespace

void Shalltear::TestHelpers::RestoreNodeShader() {
	Shalltear::RegisterNodeShader(&_NODE_SHADER_TOKEN);
}

int main(int argc, char** argv) {
	for (int arg = 1; arg < argc; ++arg) {
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::PRINT_ENVIRONMENT_FLAG) == 0) {
			for (int name = arg + 1; name < argc; ++name) {
				std::printf("%s=%s\n", argv[name], TestPlatform::GetEnvironment(argv[name]).c_str());
			}
			return 0;
		}
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::EXIT_FLAG) == 0) {
			if (arg + 3 < argc) {
				std::printf("%s\n", argv[arg + 3]);
				std::fflush(stdout);
			}
			std::this_thread::sleep_for(std::chrono::seconds(arg + 2 < argc ? std::atoi(argv[arg + 2]) : 0));
			return arg + 1 < argc ? std::atoi(argv[arg + 1]) : 0;
		}
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::REFLECTION_STARTUP_FLAG) == 0) {
			return Shalltear::TestHelpers::RunReflectionStartupProbe();
		}
#ifdef SLT_TEST_CSHARP_AOT
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::CSHARP_AOT_HOST_FLAG) == 0) {
			return Shalltear::TestHelpers::RunCSharpAOTHost(arg + 1 < argc ? argv[arg + 1] : nullptr);
		}
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::CSHARP_AOT_COMPONENTS_FLAG) == 0) {
			return Shalltear::TestHelpers::RunCSharpAOTComponents(arg + 1 < argc ? argv[arg + 1] : nullptr);
		}
		if (std::strcmp(argv[arg], Shalltear::TestHelpers::CSHARP_AOT_FIELDS_FLAG) == 0) {
			return Shalltear::TestHelpers::RunCSharpAOTFields(arg + 1 < argc ? argv[arg + 1] : nullptr);
		}
#endif
	}
	int helperExitCode = 0;
	if (TestPlatform::RunHelperProcess(argc, argv, helperExitCode)) {
		return helperExitCode;
	}

	::testing::InitGoogleTest(&argc, argv);
	Shalltear::ImportTier::Install();
	::testing::AddGlobalTestEnvironment(new PendingTypeRegistrationEnvironment());
	::testing::AddGlobalTestEnvironment(new ImportPipelineEnvironment());
	return RUN_ALL_TESTS();
}
