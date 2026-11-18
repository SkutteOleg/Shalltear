#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpSettings.h"
#include "EditorTestHooks.h"
#include "ScopedSettingsStore.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorTasks.h"
#include "Shalltear/Editor/SettingsPresentation.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* OTHER_SDK = "9.0.100";
	constexpr const char* OTHER_RUNTIME = "9.0.0";

	class DotNetToolchainTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		DotNetToolchain::Pin _pin;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			_root = fs::temp_directory_path() /
					("shalltear_dotnet_toolchain_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(_root);
			_pin.sdkVersion = "10.0.999";
			_pin.runtimeVersion = "10.0.99";
			_pin.targetFramework = "net10.0";
			_pin.hostRid = "test-x64";
		}

		void TearDown() override {
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		void Write(const fs::path& file, const str& text) {
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		str MakeSDK(const fs::path& root, const char* sdkVersion, const char* runtimeVersion) {
			Write(root / _editor->muxerName, "muxer");
			Write(root / "sdk" / sdkVersion / "dotnet.dll", "sdk");
			Write(root / "host" / "fxr" / runtimeVersion / _editor->hostLibraryName, "fxr");
			return root.generic_string();
		}

		str MakeArchive(const char* sdkVersion, const char* runtimeVersion) {
			const fs::path content = _root / "archive-content";
			MakeSDK(content, sdkVersion, runtimeVersion);
			const fs::path archive = _root / "served" / "sdk.tar.gz";
			fs::create_directories(archive.parent_path());
			int exitCode = -1;
			const str output = TestPlatform::RunCommand("\"" + _editor->findSystemTool("tar") + "\" -czf \"" + archive.generic_string() +
															"\" -C \"" + content.generic_string() + "\" .",
														exitCode);
			EXPECT_EQ(exitCode, 0) << output;
			const str path = archive.generic_string();
			_pin.archiveURL = str("file://") + (path.starts_with("/") ? "" : "/") + path;
			_pin.archiveSHA512 = _editor->hashFile(path);
			return path;
		}

		AcquireOptions Options() {
			AcquireOptions options;
			options.pin = _pin;
			options.cacheRoot = (_root / "cache").generic_string();
			options.restoreCommand = [](const str&, const str&) { return Helper(0); };
			return options;
		}

		static str Helper(int exitCode, int seconds = 0, const char* text = nullptr) {
			str command = QuoteCommandArgument(FileSystem::GetExecutablePath().ToString()) + " " + TestHelpers::EXIT_FLAG + " " +
						  std::to_string(exitCode) + " " + std::to_string(seconds);
			if (text) {
				command += " " + QuoteCommandArgument(text);
			}
			return command;
		}

		fs::path Installed() { return _root / "cache" / _pin.sdkVersion; }
		fs::path Partial() { return _root / "cache" / (_pin.sdkVersion + ".partial"); }
		fs::path Packages() { return _root / "cache" / "packages" / _pin.runtimeVersion; }

		DotNetToolchain::Info Found() {
			DotNetToolchain::DiscoveryOptions options;
			options.cacheRoot = (_root / "cache").generic_string();
			return _editor->discover(_pin, options);
		}
	};

} // anonymous namespace

TEST_F(DotNetToolchainTest, SHA512MatchesKnownVectors) {
	const struct {
		str text;
		const char* digest;
	} vectors[] = {
		{"", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
		{"abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
		{"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
		 "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"},
		{str(1000000, 'a'),
		 "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"},
	};
	u32 index = 0;
	for (const auto& vector : vectors) {
		const fs::path file = _root / ("vector" + std::to_string(index++));
		Write(file, vector.text);
		EXPECT_EQ(_editor->hashFile(file.generic_string()), vector.digest) << vector.text.size();
	}
	EXPECT_EQ(_editor->hashFile((_root / "absent").generic_string()), "");
}

TEST_F(DotNetToolchainTest, HashesAFileBehindALongPath) {
	const fs::path deep = _root / str(100, 'a') / str(100, 'b') / str(100, 'c');
	const str file = (deep / "deep.bin").generic_string();
	ASSERT_GT(file.size(), 300u);
	fs::create_directories(_editor->makeLongPath(deep.generic_string()));
	std::ofstream(_editor->makeLongPath(file), std::ios::binary) << "abc";
	EXPECT_EQ(_editor->hashFile(file),
			  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

	std::error_code ec;
	fs::remove_all(_editor->makeLongPath((_root / str(100, 'a')).generic_string()), ec);
	EXPECT_FALSE(ec) << ec.message();
	EXPECT_FALSE(fs::exists(_root / str(100, 'a')));
	EXPECT_EQ(_editor->makeLongPath("relative/file.bin"), fs::path("relative/file.bin").make_preferred());
}

TEST_F(DotNetToolchainTest, TiersInOrder) {
	DotNetToolchain::DiscoveryOptions options;
	options.overrideRoot = MakeSDK(_root / "override", _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	options.cacheRoot = (_root / "cache").generic_string();
	MakeSDK(_root / "cache" / _pin.sdkVersion, _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	options.checkoutRoot = (_root / "checkout").generic_string();
	MakeSDK(_root / "checkout" / _pin.sdkVersion, _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	const str onPath = MakeSDK(_root / "path-bin", _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	options.searchPath = (_root / "empty").generic_string() + TestPlatform::GetSearchPathSeparator() + onPath;

	DotNetToolchain::Info info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Override);
	EXPECT_EQ(info.root, options.overrideRoot);

	options.overrideRoot.clear();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Cache);
	EXPECT_EQ(info.root, (_root / "cache" / _pin.sdkVersion).generic_string());

	options.cacheRoot.clear();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Checkout);
	EXPECT_EQ(info.root, (_root / "checkout" / _pin.sdkVersion).generic_string());

	options.checkoutRoot.clear();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::SearchPath);
	EXPECT_TRUE(fs::equivalent(fs::path(info.root), fs::path(onPath)));

	options.searchPath.clear();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::None);
	EXPECT_EQ(info.root, "");
}

TEST_F(DotNetToolchainTest, OtherVersionIsNeverUsed) {
	DotNetToolchain::DiscoveryOptions options;
	options.overrideRoot = MakeSDK(_root / "override", OTHER_SDK, OTHER_RUNTIME);
	options.cacheRoot = (_root / "cache").generic_string();
	MakeSDK(_root / "cache" / _pin.sdkVersion, _pin.sdkVersion.c_str(), OTHER_RUNTIME);
	options.checkoutRoot = (_root / "checkout").generic_string();
	MakeSDK(_root / "checkout" / _pin.sdkVersion, OTHER_SDK, _pin.runtimeVersion.c_str());
	options.searchPath = MakeSDK(_root / "path-bin", OTHER_SDK, OTHER_RUNTIME);
	EXPECT_EQ(_editor->discover(_pin, options).tier, DotNetToolchain::Tier::None);

	const str described = _editor->describe(_pin, options);
	EXPECT_NE(described.find(".NET SDK 10.0.999 was not found"), str::npos) << described;
	for (const str& place : {options.overrideRoot, options.cacheRoot + "/10.0.999", options.checkoutRoot + "/10.0.999", str("PATH")}) {
		EXPECT_NE(described.find(place), str::npos) << place;
	}
	EXPECT_NE(described.find("Set SDK Path in Settings"), str::npos) << described;

	MakeSDK(_root / "path-bin", _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	EXPECT_EQ(_editor->discover(_pin, options).tier, DotNetToolchain::Tier::SearchPath);
}

TEST_F(DotNetToolchainTest, AcquiresFromAnArchive) {
	const str archive = MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	u32 restores = 0;
	AcquireOptions options = Options();
	options.restoreCommand = [&](const str& sdkRoot, const str& packagesRoot) {
		++restores;
		EXPECT_EQ(sdkRoot, Installed().generic_string());
		EXPECT_EQ(packagesRoot, Packages().generic_string());
		return Helper(0);
	};
	AcquireStatus status;
	ASSERT_TRUE(_editor->acquire(options, AcquireStep::Idle, status)) << status.error;
	EXPECT_EQ(status.step, AcquireStep::Done);
	EXPECT_EQ(status.retries, 0u);
	EXPECT_EQ(status.bytesTotal, fs::file_size(archive));
	EXPECT_EQ(restores, 1u);
	EXPECT_FALSE(fs::exists(Partial()));
	const DotNetToolchain::Info found = Found();
	EXPECT_EQ(found.tier, DotNetToolchain::Tier::Cache);
	EXPECT_EQ(found.root, Installed().generic_string());
	EXPECT_EQ(_editor->describeStep(status, _pin), ".NET SDK 10.0.999 is ready");

	ASSERT_TRUE(_editor->acquire(options, AcquireStep::Idle, status)) << status.error;
	EXPECT_EQ(restores, 2u);
	EXPECT_EQ(status.bytesDone, 0u);
}

TEST_F(DotNetToolchainTest, HashMismatchRefused) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	const str real = _pin.archiveSHA512;
	_pin.archiveSHA512 = str(128, '0');
	AcquireStatus status;
	EXPECT_FALSE(_editor->acquire(Options(), AcquireStep::Idle, status));
	EXPECT_EQ(status.step, AcquireStep::Failed);
	EXPECT_EQ(status.retries, 1u);
	EXPECT_NE(status.error.find("archive has SHA-512 " + real + ", expected " + _pin.archiveSHA512), str::npos) << status.error;
	EXPECT_FALSE(fs::exists(Installed()));
	EXPECT_FALSE(fs::exists(Partial()));
	EXPECT_EQ(Found().tier, DotNetToolchain::Tier::None);
}

TEST_F(DotNetToolchainTest, ArchiveOfAnotherVersionRefused) {
	MakeArchive(OTHER_SDK, OTHER_RUNTIME);
	AcquireStatus status;
	EXPECT_FALSE(_editor->acquire(Options(), AcquireStep::Idle, status));
	EXPECT_EQ(status.step, AcquireStep::Failed);
	EXPECT_NE(status.error.find("has no SDK 10.0.999 with runtime 10.0.99 inside"), str::npos) << status.error;
	EXPECT_FALSE(fs::exists(Installed()));
	EXPECT_FALSE(fs::exists(Partial()));
}

TEST_F(DotNetToolchainTest, CancelLeavesNoPartialInstall) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	for (const AcquireStep step : {AcquireStep::Sizing, AcquireStep::Downloading, AcquireStep::Verifying, AcquireStep::Extracting}) {
		AcquireStatus status;
		EXPECT_FALSE(_editor->acquire(Options(), step, status));
		EXPECT_EQ(status.step, AcquireStep::Cancelled) << static_cast<u32>(step);
		EXPECT_FALSE(fs::exists(Installed())) << static_cast<u32>(step);
		EXPECT_FALSE(fs::exists(Partial())) << static_cast<u32>(step);
		EXPECT_EQ(_editor->describeStep(status, _pin), "Download cancelled");
	}

	AcquireOptions slow = Options();
	slow.restoreCommand = [](const str&, const str&) { return Helper(0, 60); };
	AcquireStatus status;
	EXPECT_FALSE(_editor->acquire(slow, AcquireStep::Restoring, status));
	EXPECT_EQ(status.step, AcquireStep::Cancelled);
	EXPECT_TRUE(fs::exists(Installed()));
	EXPECT_FALSE(fs::exists(Partial()));
	EXPECT_FALSE(fs::exists(Packages()));

	EXPECT_TRUE(_editor->acquire(Options(), AcquireStep::Idle, status)) << status.error;
	EXPECT_TRUE(fs::exists(Packages()));
}

TEST_F(DotNetToolchainTest, FailedRestoreIsRetriedOnce) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	u32 calls = 0;
	AcquireOptions options = Options();
	options.restoreCommand = [&](const str&, const str& packagesRoot) {
		if (++calls > 1) {
			EXPECT_FALSE(fs::exists(fs::path(packagesRoot) / "half.pack"));
			return Helper(0);
		}
		Write(fs::path(packagesRoot) / "half.pack" / "1.0.0" / "half.pack.1.0.0.nupkg", "truncated");
		return Helper(3, 0, "connection reset");
	};
	AcquireStatus status;
	EXPECT_TRUE(_editor->acquire(options, AcquireStep::Idle, status)) << status.error;
	EXPECT_EQ(calls, 2u);
	EXPECT_EQ(status.retries, 1u);

	calls = 0;
	options.restoreCommand = [&](const str&, const str& packagesRoot) {
		++calls;
		Write(fs::path(packagesRoot) / "half.pack" / "1.0.0" / "half.pack.1.0.0.nupkg", "truncated");
		return Helper(3, 0, "connection reset");
	};
	EXPECT_FALSE(_editor->acquire(options, AcquireStep::Idle, status));
	EXPECT_EQ(calls, 2u);
	EXPECT_EQ(status.step, AcquireStep::Failed);
	EXPECT_NE(status.error.find("native build packages couldn't be restored (connection reset)"), str::npos) << status.error;
	EXPECT_FALSE(fs::exists(Packages()));
	EXPECT_EQ(Found().tier, DotNetToolchain::Tier::Cache);
}

TEST_F(DotNetToolchainTest, PacksMustMatchThePin) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	const fs::path sample = _root / "good.nupkg";
	Write(sample, "package bytes");
	_pin.packs.push_back({"good.pack/1.0.0", _editor->hashFile(sample.generic_string())});

	AcquireOptions options = Options();
	options.restoreCommand = [&](const str&, const str& packagesRoot) {
		Write(fs::path(packagesRoot) / "good.pack" / "1.0.0" / "good.pack.1.0.0.nupkg", "package bytes");
		return Helper(0);
	};
	AcquireStatus status;
	EXPECT_TRUE(_editor->acquire(options, AcquireStep::Idle, status)) << status.error;
	EXPECT_TRUE(fs::exists(Packages() / "good.pack" / "1.0.0" / "good.pack.1.0.0.nupkg"));

	options.restoreCommand = [&](const str&, const str& packagesRoot) {
		Write(fs::path(packagesRoot) / "good.pack" / "1.0.0" / "good.pack.1.0.0.nupkg", "other bytes");
		return Helper(0);
	};
	EXPECT_FALSE(_editor->acquire(options, AcquireStep::Idle, status));
	EXPECT_NE(status.error.find("package 'good.pack/1.0.0' has SHA-512 "), str::npos) << status.error;
	EXPECT_FALSE(fs::exists(Packages()));

	options.restoreCommand = [&](const str&, const str& packagesRoot) {
		Write(fs::path(packagesRoot) / "stray.pack" / "2.0.0" / "stray.pack.2.0.0.nupkg", "package bytes");
		return Helper(0);
	};
	EXPECT_FALSE(_editor->acquire(options, AcquireStep::Idle, status));
	EXPECT_NE(status.error.find("package 'stray.pack/2.0.0' is not expected (not used)"), str::npos) << status.error;
	EXPECT_FALSE(fs::exists(Packages()));
}

TEST_F(DotNetToolchainTest, MissingToolIsNamed) {
	const str archive = MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	AcquireOptions options = Options();
	options.curl = "slt-no-such-curl";
	AcquireStatus status;
	EXPECT_FALSE(_editor->acquire(options, AcquireStep::Idle, status));
	EXPECT_EQ(status.error, "curl was not found. Download " + _pin.archiveURL + " -> extract it into '" + Installed().generic_string() + "'");
	EXPECT_FALSE(fs::exists(Partial()));

	options = Options();
	options.tar = "slt-no-such-tar";
	EXPECT_FALSE(_editor->acquire(options, AcquireStep::Idle, status));
	const str kept = (Partial() / "sdk.tar.gz").generic_string();
	EXPECT_EQ(status.error, "tar was not found. Extract '" + kept + "' into '" + Installed().generic_string() + "'");
	EXPECT_EQ(fs::file_size(kept), fs::file_size(archive));
}

namespace {

	char _setupOwner = 0;

	struct ForeignCategory {
		SLT_REFLECT_VALUE(ForeignCategory)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(taken)
		SLT_END_REFLECT()

		bool taken = false;
	};

	class CSharpSetupTest : public DotNetToolchainTest {
	protected:
		fs::path _project;
		u32 _started = 0;
		str _startedWith;
		bool _startResult = true;

		void SetUp() override {
			DotNetToolchainTest::SetUp();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			_project = _root / "Project";
			fs::create_directories(_project);
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownSetup();
			}
			EditorTasks::UnregisterOwnedBy(&_setupOwner);
			CompileStatus::UnregisterOwnedBy(&_setupOwner);
			ChildEnvironment::UnregisterOwnedBy(&_setupOwner);
			SettingsPresentation::UnregisterOwnedBy(&_setupOwner);
			FileSystem::Unmount("engine");
			DotNetToolchainTest::TearDown();
		}

		CSharpSetup::Options SetupOptions() {
			CSharpSetup::Options options;
			options.discovery.cacheRoot = (_root / "cache").generic_string();
			options.acquire = Options();
			options.projectPath = _project.generic_string();
			options.startRuntime = [this](const str& sdkRoot, str& outError) {
				++_started;
				_startedWith = sdkRoot;
				if (!_startResult) {
					outError = "runtime refused";
				}
				return _startResult;
			};
			return options;
		}

		CSharpSetup::State Settle() {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (_editor->getSetupState() == CSharpSetup::State::Acquiring && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpSetup();
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return _editor->getSetupState();
		}

		static str ScriptSourceHeading() {
			Vector<CompileStatus::SourceView> sources{GetMemoryResource(MemTag::Editor)};
			CompileStatus::GetSources(sources);
			for (const CompileStatus::SourceView& source : sources) {
				if (source.name == "C#") {
					return source.heading;
				}
			}
			return "none";
		}

		static Vector<EditorTasks::TaskView> Tasks() {
			Vector<EditorTasks::TaskView> tasks{GetMemoryResource(MemTag::Editor)};
			EditorTasks::GetActive(tasks);
			return tasks;
		}

		str ReadProjectFile(const char* relative) {
			std::ifstream file(_project / relative, std::ios::binary);
			return str(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}

		str Reported(std::initializer_list<const char*> names) {
			str command = QuoteCommandArgument(FileSystem::GetExecutablePath().ToString()) + " " + TestHelpers::PRINT_ENVIRONMENT_FLAG;
			for (const char* name : names) {
				command += " ";
				command += name;
			}
			ChildProcess child;
			if (!child.Start(command, {})) {
				ADD_FAILURE() << "child didn't start";
				return {};
			}
			i32 exitCode = -1;
			for (u32 attempt = 0; attempt < 4000 && !child.TryFinish(exitCode); ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			EXPECT_EQ(exitCode, 0);
			Vector<str> lines{GetMemoryResource(MemTag::Editor)};
			child.DrainLines(lines);
			str joined;
			for (const str& line : lines) {
				joined += line;
				joined += '\n';
			}
			return joined;
		}
	};

} // anonymous namespace

TEST_F(CSharpSetupTest, FoundToolchainStartsAtOnce) {
	const str root = MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	Write(_project / "Project.ini", "[Project]\nname = Space Game\n");
	_editor->beginSetup(SetupOptions(), &_setupOwner);

	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	EXPECT_EQ(_editor->getSetupSDKRoot(), root);
	EXPECT_EQ(_started, 1u);
	EXPECT_EQ(_startedWith, root);
	EXPECT_TRUE(Tasks().empty());
	EXPECT_NE(ReadProjectFile("global.json").find("\"paths\": [\"" + root + "\", \"$host$\"]"), str::npos);
	EXPECT_TRUE(fs::is_regular_file(_project / "Space_Game.Scripts.csproj"));
	EXPECT_FALSE(fs::exists(_project / "Project.Scripts.csproj"));
	EXPECT_EQ(_log.Delivered().Count("C# runtime ready in"), 1u);
	EXPECT_EQ(_log.Delivered().Count("wrote Space_Game.Scripts.csproj"), 1u);

	_editor->pumpSetup();
	EXPECT_EQ(_started, 1u);
}

TEST_F(CSharpSetupTest, MissingToolchainIsAcquiredBehindATask) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	_editor->beginSetup(SetupOptions(), &_setupOwner);

	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Acquiring);
	EXPECT_EQ(_started, 0u);
	EXPECT_FALSE(fs::exists(_project / "global.json"));
	Vector<EditorTasks::TaskView> tasks = Tasks();
	ASSERT_EQ(tasks.size(), 1u);
	EXPECT_TRUE(tasks[0].text.starts_with("Downloading .NET SDK 10.0.999")) << tasks[0].text;
	EXPECT_TRUE(tasks[0].cancellable);
	EXPECT_EQ(_log.Delivered().Count("downloading .NET SDK 10.0.999 into"), 1u);

	ASSERT_EQ(Settle(), CSharpSetup::State::Ready) << _log.Delivered().Joined();
	EXPECT_TRUE(Tasks().empty());
	EXPECT_EQ(_started, 1u);
	EXPECT_EQ(_startedWith, Installed().generic_string());
	EXPECT_EQ(_editor->getSetupSDKRoot(), Installed().generic_string());
	EXPECT_NE(ReadProjectFile("global.json").find(Installed().generic_string()), str::npos);
	EXPECT_TRUE(fs::is_regular_file(_project / "Project.Scripts.csproj"));
	EXPECT_EQ(_log.Delivered().Count(".NET SDK 10.0.999 installed into"), 1u);
	EXPECT_EQ(_log.Delivered().Count("Checking .NET SDK 10.0.999"), 1u);
	EXPECT_EQ(_log.Delivered().Count("Extracting .NET SDK 10.0.999"), 1u);
	EXPECT_EQ(_log.Delivered().Count("Downloading .NET SDK 10.0.999"), 0u);
}

TEST_F(CSharpSetupTest, CancelFromTheTaskStopsTheDownload) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	_editor->beginSetup(SetupOptions(), &_setupOwner);
	const Vector<EditorTasks::TaskView> tasks = Tasks();
	ASSERT_EQ(tasks.size(), 1u);

	EXPECT_TRUE(EditorTasks::Cancel(tasks[0].id));
	EXPECT_EQ(Settle(), CSharpSetup::State::Cancelled);
	EXPECT_TRUE(Tasks().empty());
	EXPECT_EQ(_started, 0u);
	EXPECT_FALSE(fs::exists(Installed()));
	EXPECT_FALSE(fs::exists(Partial()));
	EXPECT_FALSE(fs::exists(_project / "global.json"));
	EXPECT_EQ(_log.Delivered().Count(".NET SDK 10.0.999 download cancelled. Reopen the project to download it"), 1u);
}

TEST_F(CSharpSetupTest, FailedDownloadIsReported) {
	MakeArchive(_pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	_pin.archiveSHA512 = str(128, '0');
	_editor->beginSetup(SetupOptions(), &_setupOwner);

	EXPECT_EQ(Settle(), CSharpSetup::State::Failed);
	EXPECT_TRUE(Tasks().empty());
	EXPECT_EQ(_started, 0u);
	EXPECT_EQ(_log.Delivered().Count(("expected " + _pin.archiveSHA512).c_str()), 1u) << _log.Delivered().Joined();
}

TEST_F(CSharpSetupTest, NoCacheFolderNamesWhereItLooked) {
	CSharpSetup::Options options = SetupOptions();
	options.discovery.cacheRoot.clear();
	options.discovery.overrideRoot = (_root / "elsewhere").generic_string();
	options.acquire.cacheRoot.clear();
	_editor->beginSetup(options, &_setupOwner);

	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Failed);
	EXPECT_TRUE(Tasks().empty());
	EXPECT_EQ(_started, 0u);
	const str looked = ".NET SDK 10.0.999 was not found (looked in '" + options.discovery.overrideRoot + "', PATH)";
	EXPECT_EQ(_log.Delivered().Count(looked.c_str()), 1u) << _log.Delivered().Joined();
}

TEST_F(CSharpSetupTest, OverrideWithoutThePinIsNamed) {
	const str cached = MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	CSharpSetup::Options options = SetupOptions();
	options.discovery.overrideRoot = MakeSDK(_root / "override", OTHER_SDK, OTHER_RUNTIME);
	_editor->beginSetup(options, &_setupOwner);
	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	EXPECT_EQ(_editor->getSetupSDKRoot(), cached);
	const str warning = "SDK Path '" + options.discovery.overrideRoot + "' has no .NET SDK 10.0.999 (ignored)";
	EXPECT_EQ(_log.Delivered().Count(warning.c_str()), 1u) << _log.Delivered().Joined();

	MakeSDK(_root / "override", _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	_editor->beginSetup(options, &_setupOwner);
	EXPECT_EQ(_editor->getSetupSDKRoot(), options.discovery.overrideRoot);
	EXPECT_EQ(_log.Delivered().Count(warning.c_str()), 1u);
}

TEST_F(CSharpSetupTest, RuntimeFailureIsReported) {
	MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	_startResult = false;
	_editor->beginSetup(SetupOptions(), &_setupOwner);

	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Failed);
	EXPECT_EQ(_started, 1u);
	EXPECT_EQ(_log.Delivered().Count("runtime refused"), 1u);
	EXPECT_EQ(_log.Delivered().Count("C# runtime ready"), 0u);
}

TEST_F(CSharpSetupTest, ChildrenGetTheToolchainEnvironment) {
	DeviceLossTest::ScopedEnvironment inherited("DOTNET_ROOT", "inherited-root");
	const str root = MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	const str path = TestPlatform::GetEnvironment("PATH");
	_editor->beginSetup(SetupOptions(), &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);

	EXPECT_EQ(Reported({"DOTNET_ROOT", "DOTNET_CLI_TELEMETRY_OPTOUT", "DOTNET_NOLOGO", "PATH"}),
			  "DOTNET_ROOT=" + root + "\nDOTNET_CLI_TELEMETRY_OPTOUT=1\nDOTNET_NOLOGO=1\nPATH=" + root + TestPlatform::GetSearchPathSeparator() + path +
				  "\n");

	_editor->shutdownSetup();
	EXPECT_EQ(_editor->getSetupState(), CSharpSetup::State::Idle);
	EXPECT_EQ(Reported({"DOTNET_ROOT", "PATH"}), "DOTNET_ROOT=inherited-root\nPATH=" + path + "\n");
}

TEST_F(CSharpSetupTest, SettingsPagesNameEveryField) {
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_setupOwner);
	Vector<SettingsCategoryInfo> categories;
	Settings::GetCategories(categories);
	u32 seen = 0;
	for (const SettingsCategoryInfo& category : categories) {
		if (category.path != CSharpSettings::ProjectCategory && category.path != CSharpSettings::UserCategory) {
			continue;
		}
		++seen;
		EXPECT_EQ(category.scope, category.path == CSharpSettings::ProjectCategory ? SettingScope::Project : SettingScope::User);
		const SettingsCategoryPresentation* page = SettingsPresentation::FindCategory(category.path);
		ASSERT_NE(page, nullptr);
		EXPECT_EQ(page->label, "C#");
		EXPECT_FALSE(page->tooltip.empty());
		ASSERT_NE(category.type, nullptr);
		for (u32 index = 0; index < category.type->fieldCount; ++index) {
			const char* name = category.type->fields[index].displayName;
			const SettingsFieldPresentation* words = SettingsPresentation::FindField(category.path, name);
			ASSERT_NE(words, nullptr) << name;
			EXPECT_FALSE(words->label.empty()) << name;
			EXPECT_FALSE(words->tooltip.empty()) << name;
		}
	}
	EXPECT_EQ(seen, 2u);
	EXPECT_EQ(SettingsPresentation::UnregisterOwnedBy(&_setupOwner), 10u);
}

TEST_F(CSharpSetupTest, ChangedSettingRewritesProjectFiles) {
	MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_setupOwner);
	_editor->beginSetup(SetupOptions(), &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	const char* paths = "Library/CSharp/Generated/Shalltear.Paths.props";
	EXPECT_NE(ReadProjectFile(paths).find("<ShalltearAllowUnsafeBlocks>false</ShalltearAllowUnsafeBlocks>"), str::npos);

	Vector<SettingsCategoryInfo> categories;
	Settings::GetCategories(categories);
	CSharpProjectSettings* live = nullptr;
	for (const SettingsCategoryInfo& category : categories) {
		if (category.path == CSharpSettings::ProjectCategory) {
			live = static_cast<CSharpProjectSettings*>(category.live);
		}
	}
	ASSERT_NE(live, nullptr);
	live->allowUnsafeCode = true;
	Settings::NotifyChanged(CSharpSettings::UserCategory, "codeOptimization");
	EXPECT_NE(ReadProjectFile(paths).find("<ShalltearAllowUnsafeBlocks>false</ShalltearAllowUnsafeBlocks>"), str::npos);
	Settings::NotifyChanged(CSharpSettings::ProjectCategory, "allowUnsafeCode");
	EXPECT_NE(ReadProjectFile(paths).find("<ShalltearAllowUnsafeBlocks>true</ShalltearAllowUnsafeBlocks>"), str::npos);

	_editor->shutdownSetup();
	live->allowUnsafeCode = false;
	Settings::NotifyChanged(CSharpSettings::ProjectCategory, "allowUnsafeCode");
	EXPECT_NE(ReadProjectFile(paths).find("<ShalltearAllowUnsafeBlocks>true</ShalltearAllowUnsafeBlocks>"), str::npos);
	EXPECT_GT(SettingsPresentation::UnregisterOwnedBy(&_setupOwner), 0u);
}

TEST_F(CSharpSetupTest, SettingsReachTheCompileOnceItRuns) {
	MakeSDK(Installed(), _pin.sdkVersion.c_str(), _pin.runtimeVersion.c_str());
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_setupOwner);
	const CSharpCompile& compile = _editor->getAuthoringCompile();

	_editor->beginSetup(SetupOptions(), &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	EXPECT_EQ(ScriptSourceHeading(), "none");
	Settings::NotifyChanged(CSharpSettings::ProjectCategory, "allowUnsafeCode");
	EXPECT_EQ(compile.GetRestoreCount(), 0u);

	CSharpSetup::Options options = SetupOptions();
	options.authoring = true;
	_startResult = false;
	_editor->beginSetup(options, &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Failed);
	EXPECT_EQ(ScriptSourceHeading(), "none");

	_startResult = true;
	CSharpSetup::Options homeless = options;
	homeless.projectPath.clear();
	_editor->beginSetup(homeless, &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	EXPECT_EQ(ScriptSourceHeading(), "none");

	_editor->beginSetup(options, &_setupOwner);
	ASSERT_EQ(_editor->getSetupState(), CSharpSetup::State::Ready);
	EXPECT_EQ(ScriptSourceHeading(), "Project.Scripts");
	EXPECT_EQ(compile.GetRestoreCount(), 0u);

	Settings::NotifyChanged(CSharpSettings::UserCategory, "sdkPath");
	EXPECT_EQ(compile.GetRestoreCount(), 0u);
	Settings::NotifyChanged(CSharpSettings::ProjectCategory, "allowUnsafeCode");
	EXPECT_EQ(compile.GetRestoreCount(), 1u);
	Settings::NotifyChanged(CSharpSettings::UserCategory, "codeOptimization");
	EXPECT_EQ(compile.GetRestoreCount(), 2u);
	Settings::NotifyChanged(CSharpSettings::UserCategory, "aotAnalysis");
	EXPECT_EQ(compile.GetRestoreCount(), 3u);
	Settings::NotifyChanged(CSharpSettings::UserCategory, "autoCompile");
	EXPECT_EQ(compile.GetRestoreCount(), 3u);

	_editor->shutdownSetup();
	EXPECT_EQ(ScriptSourceHeading(), "none");
	Settings::NotifyChanged(CSharpSettings::ProjectCategory, "allowUnsafeCode");
	EXPECT_EQ(compile.GetRestoreCount(), 3u);
	SettingsPresentation::UnregisterOwnedBy(&_setupOwner);
}

TEST_F(CSharpSetupTest, ProjectOptionsFollowTheStoreInUse) {
	const str project = (_root / "project").generic_string();
	EXPECT_FALSE(_editor->getDefaultProjectOptions(project, "sdk").allowUnsafeCode);
	{
		ScopedSettingsStore store{_root / "settings"};
		_editor->declareSettings(&_setupOwner);
		Vector<SettingsCategoryInfo> categories;
		Settings::GetCategories(categories);
		for (const SettingsCategoryInfo& category : categories) {
			if (category.path == CSharpSettings::ProjectCategory) {
				static_cast<CSharpProjectSettings*>(category.live)->allowUnsafeCode = true;
				static_cast<CSharpProjectSettings*>(category.live)->invariantGlobalization = false;
			}
		}
		EXPECT_TRUE(_editor->getDefaultProjectOptions(project, "sdk").allowUnsafeCode);
		EXPECT_FALSE(_editor->getDefaultProjectOptions(project, "sdk").invariantGlobalization);
		SettingsPresentation::UnregisterOwnedBy(&_setupOwner);
	}
	EXPECT_FALSE(_editor->getDefaultProjectOptions(project, "sdk").allowUnsafeCode);
	EXPECT_TRUE(_editor->getDefaultProjectOptions(project, "sdk").invariantGlobalization);

	ScopedSettingsStore other{_root / "other"};
	ForeignCategory* foreign = Settings::DeclareCategory<ForeignCategory>(CSharpSettings::ProjectCategory, SettingScope::Project);
	ASSERT_NE(foreign, nullptr);
	foreign->taken = true;
	EXPECT_FALSE(_editor->getDefaultProjectOptions(project, "sdk").allowUnsafeCode);
}

#endif
