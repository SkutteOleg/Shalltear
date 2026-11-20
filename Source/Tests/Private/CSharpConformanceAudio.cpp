#include "TestsPCH.h"

#include "CSharpConformance.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"

#include <atomic>
#include <chrono>
#include <fstream>

#ifndef SHALLTEAR_SHARED
#include "AudioChannels.h"
#include "AudioImportOps.h"
#include "AudioPackage.h"

#include "Shalltear/Audio/AudioClip.h"

SLT_DECLARE_LINKED_PACKAGE(Audio)
SLT_DECLARE_LINKED_PACKAGE(AudioCSharp)
#endif

namespace Shalltear::CSharpConformance {

	namespace {

		constexpr u32 TONE_RATE = 8000;
		constexpr u32 TONE_FRAMES = 4000;
		constexpr const char* TONE_PATH = "project://Assets/Tone.wav";

		constexpr const char* MIXER = R"({
	"startSnapshot": "Explore",
	"groups": [
		{ "id": "Master" },
		{ "id": "Music", "parent": "Master" },
		{ "id": "SFX", "parent": "Master", "clock": "Gameplay" },
		{ "id": "Reverb", "parent": "Master" }
	],
	"snapshots": [
		{ "name": "Explore", "values": [ { "group": "Music", "parameter": "Volume", "value": -6 } ] },
		{ "name": "Combat", "values": [ { "group": "Music", "parameter": "Volume", "value": -20 } ] }
	],
	"exposedParameters": [ { "name": "MusicVolume", "group": "Music", "parameter": "Volume" } ]
})";

		i32 Drive(i32 op, i32 first, i32) {
			if (op != 0) {
				return -1;
			}
			for (i32 frame = 0; frame < first; ++frame) {
				FrameTime state;
				state.maximumDeltaTime = 1000.0f;
				const Time::FrameScope clock{state};
				Time::BeginFrame(1.0f / 60.0f);
				FrameStages::Run(FrameStage::Update, FrameStageArgs{});
				FrameStages::Run(FrameStage::LateUpdate, FrameStageArgs{});
				FrameStages::Run(FrameStage::EndFrame, FrameStageArgs{});
			}
			return 0;
		}

		std::filesystem::path MakeRoot(const char* label) {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			std::filesystem::path root =
				std::filesystem::temp_directory_path() / (str("shalltear_csharp_") + label + "_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(root);
			return root;
		}

#ifdef SHALLTEAR_SHARED
		ModuleHandle _audio;
		ModuleHandle _bridge;

		void Put(std::ofstream& file, u32 value, u32 bytes) {
			for (u32 index = 0; index < bytes; ++index) {
				file.put(static_cast<char>((value >> (8 * index)) & 0xFF));
			}
		}

		bool WriteTone(const std::filesystem::path& path) {
			std::ofstream file(path, std::ios::binary);
			file << "RIFF";
			Put(file, 36 + TONE_FRAMES * 2, 4);
			file << "WAVEfmt ";
			Put(file, 16, 4);
			Put(file, 1, 2);
			Put(file, 1, 2);
			Put(file, TONE_RATE, 4);
			Put(file, TONE_RATE * 2, 4);
			Put(file, 2, 2);
			Put(file, 16, 2);
			file << "data";
			Put(file, TONE_FRAMES * 2, 4);
			for (u32 frame = 0; frame < TONE_FRAMES; ++frame) {
				Put(file, frame % 40 < 20 ? 8000u : 0xE0C0u, 2);
			}
			return file.good();
		}
#else
		AudioClip* _tone = nullptr;

		u64 FindTone(std::string_view path) {
			return _tone && path == TONE_PATH ? _tone->GetInstanceId() : 0;
		}
#endif

	} // anonymous namespace

	AudioStore::AudioStore() : _root(MakeRoot("audio_store")) {
		SettingsPaths paths;
		paths.project = Path((_root / "ProjectSettings.ini").generic_string());
		paths.user = Path((_root / "UserSettings.ini").generic_string());
		paths.userProject = Path((_root / "UserProjectSettings.ini").generic_string());
		FileSystem::WriteTextFile(paths.project, "[Audio]\ndevice = None\n");
		Settings::Initialize(paths);
	}

	AudioStore::~AudioStore() {
		Settings::Terminate();
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	bool StandUpAudio(const void* owner) {
#ifdef SHALLTEAR_SHARED
		(void)owner;
		_audio = ModuleSystem::Load(Path(str("Audio") + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace);
		_bridge = ModuleSystem::IsLoaded(_audio) ? ModuleSystem::Load(Path(str("AudioCSharp") + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace) : ModuleHandle{};
		return ModuleSystem::IsLoaded(_bridge);
#else
		SLT_CALL_LINKED_PACKAGE(Audio, owner);
		SLT_CALL_LINKED_PACKAGE(AudioCSharp, owner);
		return true;
#endif
	}

	void ShutDownAudio() {
#ifndef SHALLTEAR_SHARED
		AudioImpl::ShutdownPackage();
#endif
	}

	AudioRig::AudioRig(void (*setDriver)(DriverFn), void (*setAssetFinder)(AssetFinderFn))
		: _setDriver(setDriver), _setAssetFinder(setAssetFinder), _root(MakeRoot("audio_assets")) {
		std::filesystem::create_directories(_root / "Assets");
		FileSystem::Mount("project", Path(_root.generic_string()));
		AssetDatabase::Initialize();
		_staged = FileSystem::WriteTextFile(Path("project://Assets/Game.mixer"), MIXER) &&
				  FileSystem::WriteTextFile(Path("project://Assets/Limit.concurrency"), "{}");
#ifdef SHALLTEAR_SHARED
		_staged = WriteTone(_root / "Assets" / "Tone.wav") && _staged;
#else
		AudioImpl::DecodedAudio decoded;
		decoded.layout = AudioImpl::MakeStandardLayout(1);
		decoded.sampleRate = TONE_RATE;
		for (u32 frame = 0; frame < TONE_FRAMES; ++frame) {
			decoded.samples.push_back(frame % 40 < 20 ? 0.25f : -0.25f);
		}
		_tone = New<AudioClip>(MemTag::Asset);
		str error;
		_staged = AudioImpl::BuildClip(decoded, AudioImpl::AudioImportSettings{}, *_tone, error) && _staged;
		_setAssetFinder(&FindTone);
#endif
		AssetDatabase::ScanProjectDirectory();
		_setDriver(&Drive);
	}

	AudioRig::~AudioRig() {
		_setDriver(nullptr);
		_setAssetFinder(nullptr);
#ifndef SHALLTEAR_SHARED
		Delete(_tone);
		_tone = nullptr;
#endif
		AssetDatabase::CollectGarbage();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

} // namespace Shalltear::CSharpConformance
