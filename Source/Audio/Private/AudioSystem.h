#pragma once

#include "AudioVoiceInternals.h"

#include "AudioBusTable.h"
#include "AudioDevice.h"
#include "AudioMessages.h"
#include "AudioPlacement.h"
#include "AudioRenderer.h"
#include "AudioStreamer.h"
#include "AudioVoiceManager.h"
#include "AudioVoiceTable.h"

#include "Shalltear/Audio/Audio.h"
#include "Shalltear/Audio/AudioConcurrency.h"
#include "Shalltear/Audio/AudioEffect.h"
#include "Shalltear/Audio/AudioGenerator.h"
#include "Shalltear/Audio/AudioSpatializer.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear {

	struct AudioSettings;

	namespace AudioImpl {

		class AudioEventWatcher;

		struct AudioVoiceAccess {
			static AudioVoice Make(u32 slot, u32 generation) {
				AudioVoice voice;
				voice._slot = slot;
				voice._generation = generation;
				return voice;
			}
			static u32 GetSlot(AudioVoice voice) { return voice._slot; }
			static u32 GetGeneration(AudioVoice voice) { return voice._generation; }
		};

		struct AudioBusAccess {
			static AudioBus Make(u32 slot, u32 generation) {
				AudioBus bus;
				bus._slot = slot;
				bus._generation = generation;
				return bus;
			}
			static u32 GetSlot(AudioBus bus) { return bus._slot; }
			static u32 GetGeneration(AudioBus bus) { return bus._generation; }
		};

		struct AudioBusDesc {
			AudioBus parent;
			AudioBusClock clock = AudioBusClock::Wall;
			bool followTimeScale = false;
			f32 fader = 1.0f;
			f32 pitch = 1.0f;
		};

		struct AudioBusRowDesc {
			AudioBusRowKind kind = AudioBusRowKind::Fader;
			AudioBus target;
			u8 targetRow = NO_BUS_ROW;
			bool receives = false;
			f32 level = 0.0f;
			IAudioEffect* effect = nullptr;
			bool bypass = false;
			bool wetMixing = false;
		};

		struct AudioDetachment {
			struct State {
				IAudioSpatializer* spatializer = nullptr;
				void* state = nullptr;
			};
			Vector<State> states{GetMemoryResource(MemTag::Audio)};
			Vector<IAudioEffect*> effects{GetMemoryResource(MemTag::Audio)};
			Vector<IAudioGeneratorInstance*> generators{GetMemoryResource(MemTag::Audio)};
			bool posted = false;

			bool IsEmpty() const { return !posted && states.empty() && effects.empty() && generators.empty(); }
		};

		class AudioSystem {
		public:
			AudioSystem() = default;
			~AudioSystem();
			AudioSystem(const AudioSystem&) = delete;
			AudioSystem& operator=(const AudioSystem&) = delete;

			void Up();

			void Down();

			void Reapply();

			void Pump();

			bool Send(const AudioCommand& command);

			bool SendWhole(std::span<const AudioCommand> commands);

			void DrainEvents();

			void SetListenerPresent(bool present);
			bool IsListenerPresent() const { return _listenerPresent; }

			void NoteVoiceStarted(u32 busSlot, u32 busGeneration);

			u64 GetTimeFrames();

			u64 GetDeviceFrames();

			AudioBus GetDeviceOutput();
			bool IsDeviceClocked(AudioBus bus) const;
			bool IsDeviceClocked(u32 slot, u32 generation) const;

			void BeginBatch();
			bool EndBatch();

			bool SendNow(const AudioCommand& command);

			AudioBus CreateBus(const AudioBusDesc& desc);

			void DestroyBus(AudioBus bus);

			bool SetBusParent(AudioBus bus, AudioBus parent);

			bool SetBusChain(AudioBus bus, const AudioBusRowDesc* rows, u32 count);

			bool SetBusFader(AudioBus bus, f32 gain, u32 frames);
			bool SetBusPitch(AudioBus bus, f32 pitch, u32 frames);
			bool SetBusRowLevel(AudioBus bus, u32 row, f32 level, u32 frames);
			void SetBusMute(AudioBus bus, bool mute);
			void SetBusSolo(AudioBus bus, bool solo);
			void SetBusClock(AudioBus bus, AudioBusClock clock, bool followTimeScale);

			void SetBusRowBypass(AudioBus bus, u32 row, bool bypass);
			void SetBusBypassEffects(AudioBus bus, bool bypass);
			bool SetBusRowEffect(AudioBus bus, u32 row, IAudioEffect* effect);

			bool SetBusEffectParameter(AudioBus bus, u32 row, u32 index, f32 value);

			AudioEffectContext MakeBusEffectContext(std::span<const f32> parameters) const;

			void RetireEffect(IAudioEffect* effect);

			void ReleaseAtClose(IAudioEffect* effect);

			void CompleteDetachment(AudioDetachment& detachment);

			void OnImageUnloading(const void* image);

			void DetachSpatializer(IAudioSpatializer* spatializer, AudioDetachment& detachment);

			void SendGeneratorParameter(u32 slot, u32 generation, u32 index, f32 value);

			void UpdateGenerators();

			void SetVoiceEffects(u32 slot, AudioVoiceRecord& record, std::span<const AudioVoiceEffect> effects);
			bool MakeStartingEffects(u32 slot, AudioVoiceRecord& record, std::span<const AudioVoiceEffect> effects, AudioCommand& command);
			void SetVoiceEffectParameter(u32 slot, AudioVoiceRecord& record, u32 index, u32 parameter, f32 value);
			void OnVoiceSpatialChanged(u32 slot, AudioVoiceRecord& record);
			void OnVoiceEffectRegistered(std::string_view name);
			void DetachVoiceEffects(IAudioEffectDefinition* definition, AudioDetachment& detachment);

			bool MakeVoiceStream(u32 slot, AudioVoiceRecord& record, AudioCommand& command);
			void CommitVoiceStream(AudioVoiceRecord& record);
			void DiscardVoiceStream(AudioVoiceRecord& record);
			void RetireVoiceStream(AudioVoiceRecord& record, bool renderDone);
			u32 CountStreams() const { return _liveStreams; }
			const AudioStreamer& GetStreamer() const { return _streamer; }

			bool IsBusLive(AudioBus bus) const;

			void AcquireMetering();
			void ReleaseMetering();
			bool IsMetering() const { return _metering > 0; }

			bool ReadBusLevel(AudioBus bus, AudioBusLevel& out, AudioMeterPoint point = AudioMeterPoint::Output) const;

			void NoteGameplay();

			void EndFrameClock();

			bool DidGameplayRun() const { return _gameplayThisFrame; }

			void SendGlobalVolume();

			void SetGameMuted(bool muted);

			IAudioSpatializer* ResolveSpatializer(bool spatialize);

			static bool IsPlaced(const AudioVoiceRecord& record);
			static bool WantsSpatial(const AudioVoiceRecord& record);

			static f32 EffectiveReverbZoneMix(const AudioVoiceRecord& record);

			void AttachSpatial(u32 slot, AudioVoiceRecord& record);

			void PlaceVoice(u32 slot, AudioVoiceRecord& record, std::span<const AudioListenerPose> listeners);

			void FollowDistance(u32 slot, AudioVoiceRecord& record);

			void PlaceVoices();

			void RouteToListener(u32 slot, AudioVoiceRecord& record, std::span<const AudioListenerPose> listeners);
			void RouteToListener(u32 slot, AudioVoiceRecord& record);

			std::span<const AudioListenerPose> GatherListeners();

			void ReselectSpatializers();

			void PrepareForDevice();

			void RemakeSpatialStates();

			void FenceRenderThread();

			f32 ComputeAudibility(f32 volume, f32 spatialBlend, f32 distanceAttenuation, u32 busSlot, u32 busGeneration);

			f32 ComputeAudibility(const AudioVoiceRecord& record);

			bool AdmitVoice(const AudioConcurrency* concurrency, u64 owner, const Vec3& position, const AudioConcurrencyMember& incoming,
							AudioConcurrencyGroup*& group);

			bool DecideAtStart(u32 slot, AudioVoiceRecord& record);

			void SetVoiceReal(u32 slot, AudioVoiceRecord& record, bool real, std::span<const AudioListenerPose> listeners);

			void RankVoices();

			void StopToMakeWay(u32 slot, AudioVoiceRecord& record, u32 fadeFrames);

			void ResizeVoicePools();

			u32 GetMaxRealVoices() const { return _maxRealVoices; }

			struct BudgetCounts {
				u64 refusedByPool = 0;
				u64 stoppedByPool = 0;
				u64 refusedByConcurrency = 0;
				u64 stoppedByConcurrency = 0;
			};
			const BudgetCounts& GetBudgetCounts() const { return _budgetCounts; }

			bool IsUp() const { return _up; }
			const AudioDevice& GetDevice() const { return _device; }
			const AudioRenderer& GetRenderer() const { return _renderer; }
			AudioRenderer& GetRenderer() { return _renderer; }
			AudioVoiceTable& GetVoices() { return _voices; }
			AudioBusTable& GetBuses() { return _buses; }
		private:
			void Open(const AudioSettings& settings);
			void WriteReport(AudioSettings& settings) const;
			void SendListenerGate();
			void SendGameClock();

			AudioSpatializerContext MakeSpatializerContext(const AudioVoiceRecord& record) const;

			void MakeSpatialState(AudioVoiceRecord& record, IAudioSpatializer* spatializer);

			void PostSpatialSwaps(const u32* slots, IAudioSpatializer* const* to, u32 count, AudioDetachment& detachment);

			void DetachGenerators(const void* image, AudioDetachment& detachment);

			void ResumeOrphan(u32 slot, AudioVoiceRecord& record, AudioGenerator& generator);

			AudioBusRecord* FindBus(AudioBus bus);
			AudioCommand BusCommand(AudioCommandKind kind, AudioBus bus) const;

			// declared before the device -> destroyed after it
			AudioRenderer _renderer;
			AudioDevice _device;

			AudioVoiceTable _voices;
			AudioBusTable _buses;

			Vector<AudioCommand>* _batch = nullptr;
			u32 _batchDepth = 0;
			Vector<IAudioEffect*>* _closing = nullptr;
			bool _reopenAfterDetach = false;
			bool _preparing = false;

			void CloseDevice();

			bool Publish(const AudioCommand* commands, u32 count);

			AudioDeviceEvents _seen;
			AudioEventWatcher* _watcher = nullptr;
			u32 _metering = 0;
			bool _up = false;
			bool _listenerPresent = false;
			bool _gameMuted = false;
			bool _warnedListenerless = false;
			bool _refusing = false;
			bool _warnedBusPool = false;
			u64 _lastTime = 0;
			u64 _lastDeviceFrames = 0;
			AudioBus _deviceOutput;

			bool _gameplayThisFrame = false;
			bool _sentRunning = true;
			f32 _sentTimeScale = 1.0f;

			Vector<AudioListenerPose>* _listeners = nullptr;
			bool _listenersRouted = false;
			bool _listenersSent = false;
			bool _warnedUnnamedSpatializer = false;
			bool _warnedMissingSpatializer = false;
			bool _warnedRefusedState = false;

			u32 _maxRealVoices = 0;
			f32 _busAudibility[AudioRenderer::MAX_BUSES]{};
			bool _busAudibilityDirty = true;
			struct Retiring {
				IAudioSpatializer* spatializer = nullptr;
				void* state = nullptr;
				IAudioEffect* effect = nullptr;
				void* memory = nullptr;
				AudioVoiceChain* chain = nullptr;
				u64 safeAt = 0;
			};
			Vector<Retiring>* _retiring = nullptr;
			Vector<Retiring>* _batchRetiring = nullptr;

			void RetireSpatialState(AudioVoiceRecord& record);

			void AttachVoiceChain(u32 slot, AudioVoiceRecord& record);
			void RetireVoiceChain(AudioVoiceRecord& record);
			AudioVoiceChain* MakeChainFor(const AudioVoiceRecord& record, const AudioVoiceChain* carry) const;
			bool SwapVoiceChain(u32 slot, AudioVoiceRecord& record, AudioVoiceChain* chain);
			void RemakeVoiceChains();

			void DestroyRetiredStreams(bool everything);

			struct RetiringStream {
				AudioStream* stream = nullptr;
				u64 safeAt = 0;
			};
			Vector<RetiringStream>* _retiringStreams = nullptr;
			u32 _liveStreams = 0;
			bool _warnedStreamRoom = false;
			bool _warnedStreamFailures = false;

			void Retire(Retiring retiring);

			void RetireMemory(void* memory);

			static void ReleaseRetired(const Retiring& retiring);

			void DestroyRetiredStates(bool everything);

			Vector<AudioVoiceRank>* _ranks = nullptr;
			Vector<u32>* _rankOrder = nullptr;
			Vector<AudioConcurrencyMember>* _members = nullptr;
			BudgetCounts _budgetCounts;
			bool _warnedPoolFull = false;

			void RefreshBusAudibility();

			u32 FindLeastImportant(bool spareFirstPriority);

			void ReserveRankingScratch();

			u32 LiveBusOrRoot(u32 slot, u32 generation) const;

			static f32 DistanceToNearest(const Vec3& position, std::span<const AudioListenerPose> listeners);

			// declared last -> destroyed 1st, joins at exit
			AudioStreamer _streamer;
		};

		AudioSystem& GetAudioSystem();

	} // namespace AudioImpl

} // namespace Shalltear
