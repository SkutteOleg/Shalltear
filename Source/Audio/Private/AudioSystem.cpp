#include "AudioSystem.h"

#include "AudioBuiltinSpatializer.h"
#include "AudioClipData.h"
#include "AudioComposite.h"
#include "AudioEffectRegistry.h"
#include "AudioGeneratorAccess.h"
#include "AudioMixerLive.h"
#include "AudioOutputs.h"
#include "AudioScenePass.h"
#include "AudioSettings.h"
#include "AudioSpatializerRegistry.h"
#include "AudioStream.h"
#include "AudioVoiceEffects.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>
#include <thread>

namespace Shalltear {

	namespace AudioImpl {

		class AudioEventWatcher : public Object {
		public:
			void OnImageUnloading(const ImageUnloadingEvent& event) { GetAudioSystem().OnImageUnloading(event.image); }

			void OnSettingChanged(const SettingChangedEvent& event) {
				if (event.categoryPath != SETTINGS_CATEGORY_PATH) {
					return;
				}
				if (event.fieldName == "device" || event.fieldName == "periodMilliseconds" || event.fieldName == "maxVirtualVoices" ||
					event.fieldName == "maxRealVoices") {
					GetAudioSystem().Reapply();
				} else if (event.fieldName == "globalVolume") {
					GetAudioSystem().SendGlobalVolume();
				} else if (event.fieldName == "spatializer") {
					GetAudioSystem().ReselectSpatializers();
				}
			}
		};

		namespace {

			AudioSystem _audioSystem;

			void OnDevicePrepared(void* user) {
				static_cast<AudioSystem*>(user)->PrepareForDevice();
			}

			f64 TicksToMilliseconds(u64 ticks) {
				const u64 perSecond = Time::GetTicksPerSecond();
				return perSecond ? static_cast<f64>(ticks) * 1000.0 / static_cast<f64>(perSecond) : 0.0;
			}

			f64 FramesToMilliseconds(u32 frames, u32 sampleRate) {
				return sampleRate ? static_cast<f64>(frames) * 1000.0 / static_cast<f64>(sampleRate) : 0.0;
			}

			f32 Clamp(f32 value, f32 low, f32 high) {
				return value >= low ? (value <= high ? value : high) : low;
			}

			u32 ClampCount(i32 value, i32 low, i32 high) {
				return static_cast<u32>(value < low ? low : (value > high ? high : value));
			}

			u32 HeadroomFor(u32 limit, u32 real) {
				const u32 quarter = limit / 4;
				return quarter > real ? quarter : real;
			}

			u32 DecodersFor(u32 real) {
				return 2 * real;
			}

			u32 VoiceLimitOf(const AudioSettings& settings) {
				return ClampCount(settings.maxVirtualVoices, 1, 4096);
			}

			u32 RealVoicesOf(const AudioSettings& settings) {
				return ClampCount(settings.maxRealVoices, 1, 256);
			}

			u32 StreamsFor(u32 real) {
				return 2 * DecodersFor(real);
			}

		} // anonymous namespace

		AudioSystem& GetAudioSystem() { return _audioSystem; }

		AudioBus GetDeviceOutput() { return _audioSystem.GetDeviceOutput(); }

		AudioSystem::~AudioSystem() {
		}

		void AudioSystem::Up() {
			if (_up) {
				return;
			}
			_up = true;

			const u32 limit = VoiceLimitOf(GetSettings());
			_maxRealVoices = RealVoicesOf(GetSettings());
			_renderer.Initialize(limit + HeadroomFor(limit, _maxRealVoices), DecodersFor(_maxRealVoices));
			_voices.Initialize(limit, HeadroomFor(limit, _maxRealVoices));
			_buses.Initialize(AudioRenderer::MAX_BUSES);
			_streamer.Start(StreamsFor(_maxRealVoices), JobSystem::GetWorkerCount() > 0);
			_retiringStreams = New<Vector<RetiringStream>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			_retiringStreams->reserve(StreamsFor(_maxRealVoices));
			_liveStreams = 0;
			_warnedStreamRoom = false;
			_warnedStreamFailures = false;
			_ranks = New<Vector<AudioVoiceRank>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			_rankOrder = New<Vector<u32>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			_members = New<Vector<AudioConcurrencyMember>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			_retiring = New<Vector<Retiring>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			ReserveRankingScratch();
			_budgetCounts = BudgetCounts{};
			_warnedPoolFull = false;
			_lastTime = 0;
			_lastDeviceFrames = 0;
			_deviceOutput = {};
			_warnedListenerless = false;
			_refusing = false;
			_warnedBusPool = false;
			_warnedUnnamedSpatializer = false;
			_warnedMissingSpatializer = false;
			_warnedRefusedState = false;
			_listenersRouted = false;
			_listenersSent = false;
			_gameplayThisFrame = false;
			_sentRunning = true;
			_sentTimeScale = 1.0f;

			_watcher = New<AudioEventWatcher>(MemTag::Audio);
			SettingsEvents::Changed.Subscribe(EventBus::GetGlobal(), Handle<AudioEventWatcher>(_watcher), &AudioEventWatcher::OnSettingChanged);
			ModuleEvents::ImageUnloading.Subscribe(EventBus::GetGlobal(), Handle<AudioEventWatcher>(_watcher), &AudioEventWatcher::OnImageUnloading);

			Open(GetSettings());

			_listenerPresent = IsAnyListenerEnabled();
			SendListenerGate();
			SendGlobalVolume();
		}

		void AudioSystem::Down() {
			if (!_up) {
				return;
			}
			_up = false;

			if (_watcher) {
				EventBus::GetGlobal().UnsubscribeAll(_watcher->GetInstanceId());
				Delete(_watcher);
				_watcher = nullptr;
			}

			ReleaseComposites();
			ReleaseContainerState();

			const bool wasOpen = _device.IsOpen();
			const AudioRenderCounters& counters = _renderer.GetCounters();
			const u64 callbacks = counters.callbacks.load(std::memory_order_relaxed);
			const u64 shortest = counters.shortestIntervalTicks.load(std::memory_order_relaxed);
			const u64 longest = counters.longestIntervalTicks.load(std::memory_order_relaxed);
			const u64 cost = counters.costTicks.load(std::memory_order_relaxed);
			const u64 worst = counters.worstCostTicks.load(std::memory_order_relaxed);
			const u64 overruns = counters.overruns.load(std::memory_order_relaxed);
			const u64 frames = counters.frames.load(std::memory_order_relaxed);
			const u64 underruns = counters.streamUnderruns.load(std::memory_order_relaxed);
			const f32 peak = std::bit_cast<f32>(counters.peak.load(std::memory_order_relaxed));

			CloseDevice();
			GetSettings().obtainedPeriod.clear();
			const u64 streamFailures = _streamer.GetFailures();
			_streamer.Stop();

			if (wasOpen && callbacks > 1) {
				const str level = peak > 0.0f ? std::format("{:.1f} dBFS", 20.0 * std::log10(static_cast<f64>(peak))) : str("silence");
				SLT_LOG_INFO("Audio", "device closed after {} callback(s) ({} frame(s)): period paced {:.2f}-{:.2f} ms, render {:.3f} ms mean and {:.3f} ms worst, {} overrun(s), {} stream underrun(s), {} stream chunk(s) unreadable, output peak {}",
							 callbacks, frames, TicksToMilliseconds(shortest), TicksToMilliseconds(longest),
							 TicksToMilliseconds(cost) / static_cast<f64>(callbacks), TicksToMilliseconds(worst), overruns, underruns, streamFailures, level);
			}

			DestroyRetiredStates(true);
			DrainEvents();
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				RetireVoiceStream(_voices.At(slot), true);
			}
			DestroyRetiredStreams(true);
			_voices.Shutdown();
			_buses.Shutdown();
			_renderer.Shutdown();
			if (_retiringStreams) {
				Delete(_retiringStreams);
				_retiringStreams = nullptr;
			}
			_listenerPresent = false;
			if (_batch) {
				Delete(_batch);
				_batch = nullptr;
			}
			_batchDepth = 0;
			if (_batchRetiring) {
				for (const Retiring& retiring : *_batchRetiring) {
					ReleaseRetired(retiring);
				}
				Delete(_batchRetiring);
				_batchRetiring = nullptr;
			}
			if (_closing) {
				Delete(_closing);
				_closing = nullptr;
			}
			_reopenAfterDetach = false;
			if (_listeners) {
				Delete(_listeners);
				_listeners = nullptr;
			}
			if (_ranks) {
				Delete(_ranks);
				_ranks = nullptr;
			}
			if (_rankOrder) {
				Delete(_rankOrder);
				_rankOrder = nullptr;
			}
			if (_members) {
				Delete(_members);
				_members = nullptr;
			}
			if (_retiring) {
				Delete(_retiring);
				_retiring = nullptr;
			}
		}

		void AudioSystem::Reapply() {
			if (!_up) {
				return;
			}
			CloseDevice();
			DestroyRetiredStates(true);
			ResizeVoicePools();
			Open(GetSettings());
		}

		void AudioSystem::Open(const AudioSettings& settings) {
			i32 period = settings.periodMilliseconds;
			period = period < 1 ? 1 : (period > 100 ? 100 : period);

			bool opened = false;
			if (settings.device == AudioOutputDevice::Default) {
				opened = _device.Open(AudioBackendKind::System, static_cast<u32>(period), _renderer, &OnDevicePrepared, this);
				if (!opened) {
					SLT_LOG_WARN("Audio", "no output device could be opened (null device until the Audio settings change or a restart)");
				}
			}
			if (!opened) {
				opened = _device.Open(AudioBackendKind::Null, static_cast<u32>(period), _renderer, &OnDevicePrepared, this);
				if (!opened) {
					SLT_LOG_ERROR("Audio", "even the null device failed to open");
				}
			}

			_seen = _device.GetEvents();
			WriteReport(GetSettings());

			if (opened) {
				SLT_LOG_INFO("Audio", "device open: {} (asked for {} ms)", GetSettings().obtainedPeriod, period);
			}
		}

		void AudioSystem::Pump() {
			if (!_up) {
				return;
			}
			if (JobSystem::GetWorkerCount() == 0) {
				_streamer.Pump();
			}
			DrainEvents();
			UpdateComposites();
			UpdateGenerators();
			RankVoices();
			DestroyRetiredStates(!_device.IsOpen());
			DestroyRetiredStreams(false);
			if (!_warnedStreamFailures && _streamer.GetFailures() > 0) {
				_warnedStreamFailures = true;
				SLT_LOG_WARN("Audio", "a streamed clip's bytes couldn't be read or decoded");
			}
			if (!_device.IsOpen()) {
				return;
			}
			const AudioDeviceEvents now = _device.GetEvents();

			if (now.stopped != _seen.stopped) {
				SLT_LOG_WARN("Audio", "the output device stopped (unplugged, disabled or taken), restarts when the system has one");
			}
			if (now.rerouted != _seen.rerouted) {
				_device.RefreshFormat();
				WriteReport(GetSettings());
				SLT_LOG_INFO("Audio", "device rerouted: {}", GetSettings().obtainedPeriod);
			}
			if (now.Restarts() != _seen.Restarts()) {
				SLT_LOG_INFO("Audio", "the output device is running again");
			}
			if (now.interrupted != _seen.interrupted) {
				SLT_LOG_WARN("Audio", "the output device was interrupted by the system");
			}
			_seen = now;
		}

		bool AudioSystem::Send(const AudioCommand& command) {
			if (!_up || !_renderer.IsInitialized()) {
				return false;
			}
			if (_batchDepth > 0) {
				_batch->push_back(command);
				return true;
			}
			return Publish(&command, 1);
		}

		bool AudioSystem::SendWhole(std::span<const AudioCommand> commands) {
			if (!_up || !_renderer.IsInitialized()) {
				return false;
			}
			if (_batchDepth > 0) {
				_batch->insert(_batch->end(), commands.begin(), commands.end());
				return true;
			}
			return commands.empty() || Publish(commands.data(), static_cast<u32>(commands.size()));
		}

		bool AudioSystem::Publish(const AudioCommand* commands, u32 count) {
			if (_renderer.GetCommands().TryPushAll(commands, count)) {
				_refusing = false;
				return true;
			}
			if (!_device.IsOpen() || _preparing) {
				_renderer.ApplyPendingCommands();
				if (_renderer.GetCommands().TryPushAll(commands, count)) {
					return true;
				}
			}
			if (!_refusing) {
				_refusing = true;
				SLT_LOG_WARN("Audio", "more than {} audio calls before the audio thread's next block (calls until then dropped)",
							 _renderer.GetCommands().GetCapacity());
			}
			return false;
		}

		void AudioSystem::DrainEvents() {
			if (!_renderer.IsInitialized() || !_voices.IsInitialized()) {
				return;
			}
			AudioEvent event;
			while (_renderer.GetEvents().TryPop(event)) {
				if (event.kind == AudioEventKind::VoiceEnded) {
					if (AudioVoiceRecord* record = _voices.Find(event.slot, event.generation)) {
						RetireVoiceStream(*record, true);
					}
					_voices.OnEnded(event.slot, event.generation);
				} else if (event.kind == AudioEventKind::BusReleased) {
					_buses.OnReleased(event.slot, event.generation);
				}
			}
		}

		void AudioSystem::SetListenerPresent(bool present) {
			if (present == _listenerPresent) {
				return;
			}
			_listenerPresent = present;
			if (present) {
				_warnedListenerless = false;
			}
			SendListenerGate();
		}

		void AudioSystem::SendListenerGate() {
			AudioCommand command;
			command.kind = AudioCommandKind::SetListenerPresent;
			command.value = _listenerPresent ? 1.0f : 0.0f;
			Send(command);
		}

		void AudioSystem::NoteVoiceStarted(u32 busSlot, u32 busGeneration) {
			if (_listenerPresent || _warnedListenerless || IsDeviceClocked(busSlot, busGeneration)) {
				return;
			}
			_warnedListenerless = true;
			SLT_LOG_WARN("Audio", "a sound started, but no Audio Listener is enabled in a loaded scene. Add one (usually to the main camera)");
		}

		u64 AudioSystem::GetTimeFrames() {
			AudioClockStamp stamp;
			if (!_renderer.IsInitialized() || !_renderer.ReadClock(stamp)) {
				return _lastTime;
			}
			u64 frames = stamp.time;
			if (stamp.running && _device.IsOpen()) {
				const AudioDeviceFormat& format = _device.GetFormat();
				const u64 perSecond = Time::GetTicksPerSecond();
				const u64 now = Time::GetTicks();
				if (perSecond != 0 && format.sampleRate != 0 && now > stamp.ticks) {
					const u64 ahead = (now - stamp.ticks) * format.sampleRate / perSecond;
					const u64 cap = format.periodFrames != 0 ? format.periodFrames : AudioRenderer::BLOCK_FRAMES;
					frames += ahead < cap ? ahead : cap;
				}
			}
			if (frames > _lastTime) {
				_lastTime = frames;
			}
			return _lastTime;
		}

		u64 AudioSystem::GetDeviceFrames() {
			AudioClockStamp stamp;
			if (!_renderer.IsInitialized() || !_renderer.ReadClock(stamp)) {
				return _lastDeviceFrames;
			}
			u64 frames = stamp.frames;
			if (_device.IsOpen()) {
				const AudioDeviceFormat& format = _device.GetFormat();
				const u64 perSecond = Time::GetTicksPerSecond();
				const u64 now = Time::GetTicks();
				if (perSecond != 0 && format.sampleRate != 0 && now > stamp.ticks) {
					const u64 ahead = (now - stamp.ticks) * format.sampleRate / perSecond;
					const u64 cap = format.periodFrames != 0 ? format.periodFrames : AudioRenderer::BLOCK_FRAMES;
					frames += ahead < cap ? ahead : cap;
				}
			}
			if (frames > _lastDeviceFrames) {
				_lastDeviceFrames = frames;
			}
			return _lastDeviceFrames;
		}

		AudioBus AudioSystem::GetDeviceOutput() {
			if (!_up) {
				return {};
			}
			if (!FindBus(_deviceOutput)) {
				AudioBusDesc desc;
				desc.clock = AudioBusClock::Device;
				_deviceOutput = CreateBus(desc);
			}
			return _deviceOutput;
		}

		bool AudioSystem::IsDeviceClocked(AudioBus bus) const {
			return IsDeviceClocked(AudioBusAccess::GetSlot(bus), AudioBusAccess::GetGeneration(bus));
		}

		bool AudioSystem::IsDeviceClocked(u32 slot, u32 generation) const {
			const AudioBusRecord* record = _buses.Find(slot, generation);
			return record && record->clock == AudioBusClock::Device;
		}

		void AudioSystem::BeginBatch() {
			if (!_up) {
				return;
			}
			if (_batchDepth++ == 0) {
				if (!_batch) {
					_batch = New<Vector<AudioCommand>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
				}
				_batch->clear();
			}
		}

		bool AudioSystem::EndBatch() {
			if (_batchDepth == 0 || --_batchDepth > 0) {
				return true;
			}
			bool delivered = true;
			if (!_batch->empty() && !_renderer.GetCommands().TryPushAll(_batch->data(), static_cast<u32>(_batch->size()))) {
				const Vector<AudioCommand> pending = *_batch;
				_batch->clear();
				for (const AudioCommand& command : pending) {
					delivered = Send(command) && delivered;
				}
			}
			_batch->clear();

			if (_batchRetiring && !_batchRetiring->empty()) {
				Vector<Retiring> retiring = std::move(*_batchRetiring);
				_batchRetiring->clear();
				if (delivered) {
					for (const Retiring& entry : retiring) {
						Retire(entry);
					}
				} else {
					SLT_LOG_WARN("Audio", "{} effect instance(s) or buffer(s) not freed (command ring overflowed while a mixer changed)",
								 retiring.size());
				}
			}
			return delivered;
		}

		bool AudioSystem::SendNow(const AudioCommand& command) {
			if (!_up || !_renderer.IsInitialized()) {
				return false;
			}
			if (_renderer.GetCommands().TryPush(command)) {
				return true;
			}
			if (_device.IsOpen() && !_preparing) {
				CloseDevice();
				_reopenAfterDetach = true;
			}
			_renderer.ApplyPendingCommands();
			return _renderer.GetCommands().TryPush(command);
		}

		void AudioSystem::CloseDevice() {
			_device.Close();
			if (_closing) {
				for (IAudioEffect* effect : *_closing) {
					effect->Release();
				}
				_closing->clear();
			}
		}

		void AudioSystem::ReleaseAtClose(IAudioEffect* effect) {
			if (!effect) {
				return;
			}
			if (!_device.IsOpen()) {
				effect->Release();
				return;
			}
			if (!_closing) {
				_closing = New<Vector<IAudioEffect*>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			}
			_closing->push_back(effect);
		}

		AudioBusRecord* AudioSystem::FindBus(AudioBus bus) {
			return _buses.Find(AudioBusAccess::GetSlot(bus), AudioBusAccess::GetGeneration(bus));
		}

		bool AudioSystem::IsBusLive(AudioBus bus) const {
			return _buses.Find(AudioBusAccess::GetSlot(bus), AudioBusAccess::GetGeneration(bus)) != nullptr;
		}

		AudioCommand AudioSystem::BusCommand(AudioCommandKind kind, AudioBus bus) const {
			AudioCommand command;
			command.kind = kind;
			command.bus = AudioBusAccess::GetSlot(bus);
			command.busGeneration = AudioBusAccess::GetGeneration(bus);
			return command;
		}

		AudioBus AudioSystem::CreateBus(const AudioBusDesc& desc) {
			if (!_up || !_buses.IsInitialized()) {
				return {};
			}
			u32 parent = AudioRenderer::ROOT_BUS;
			u32 parentGeneration = 1;
			if (FindBus(desc.parent)) {
				parent = AudioBusAccess::GetSlot(desc.parent);
				parentGeneration = AudioBusAccess::GetGeneration(desc.parent);
			}

			DrainEvents();
			u32 slot = 0;
			u32 generation = 0;
			void* block = nullptr;
			if (!_buses.Claim(slot, generation, block)) {
				if (!_warnedBusPool) {
					_warnedBusPool = true;
					SLT_LOG_WARN("Audio", "all {} mixer buses are in use (extra groups play straight to the listener)",
								 _buses.GetCapacity() - 1);
				}
				return {};
			}
			_buses.SetParent(slot, parent);
			if (AudioBusRecord* record = _buses.Find(slot, generation)) {
				record->fader = desc.fader;
				record->clock = desc.clock;
				record->followTimeScale = desc.followTimeScale;
			}
			_busAudibilityDirty = true;

			AudioCommand command = BusCommand(AudioCommandKind::CreateBus, AudioBusAccess::Make(slot, generation));
			command.other = parent;
			command.otherGeneration = parentGeneration;
			command.block = block;
			command.mode = static_cast<u8>(desc.clock);
			command.flag = desc.followTimeScale;
			command.volume = desc.fader;
			command.pitch = desc.pitch;
			if (!Send(command)) {
				_buses.Abandon(slot);
				return {};
			}
			return AudioBusAccess::Make(slot, generation);
		}

		void AudioSystem::DestroyBus(AudioBus bus) {
			if (!FindBus(bus) || AudioBusAccess::GetSlot(bus) == AudioRenderer::ROOT_BUS) {
				return;
			}
			if (Send(BusCommand(AudioCommandKind::DestroyBus, bus))) {
				_buses.MarkDestroyed(AudioBusAccess::GetSlot(bus));
				_busAudibilityDirty = true;
			}
		}

		bool AudioSystem::SetBusParent(AudioBus bus, AudioBus parent) {
			AudioBusRecord* record = FindBus(bus);
			if (!record || AudioBusAccess::GetSlot(bus) == AudioRenderer::ROOT_BUS) {
				return false;
			}
			u32 slot = AudioRenderer::ROOT_BUS;
			u32 generation = 1;
			if (FindBus(parent)) {
				slot = AudioBusAccess::GetSlot(parent);
				generation = AudioBusAccess::GetGeneration(parent);
			}
			if (record->parent == slot) {
				return true;
			}
			if (_buses.WouldCycle(AudioBusAccess::GetSlot(bus), slot)) {
				return false;
			}
			AudioCommand command = BusCommand(AudioCommandKind::SetBusParent, bus);
			command.other = slot;
			command.otherGeneration = generation;
			if (!Send(command)) {
				return false;
			}
			_buses.SetParent(AudioBusAccess::GetSlot(bus), slot);
			_busAudibilityDirty = true;
			return true;
		}

		bool AudioSystem::SetBusChain(AudioBus bus, const AudioBusRowDesc* rows, u32 count) {
			AudioBusRecord* record = FindBus(bus);
			const u32 slot = AudioBusAccess::GetSlot(bus);
			if (!record || slot == AudioRenderer::ROOT_BUS || count == 0 || count > MAX_BUS_ROWS) {
				return false;
			}
			u32 faders = 0;
			for (u32 row = 0; row < count; ++row) {
				faders += rows[row].kind == AudioBusRowKind::Fader ? 1u : 0u;
			}
			if (faders != 1) {
				return false;
			}
			u32 previous[MAX_BUS_ROWS];
			const u32 previousCount = record->sendCount;
			for (u32 send = 0; send < previousCount; ++send) {
				previous[send] = record->sendTargets[send];
			}
			_buses.SetSends(slot, nullptr, 0);
			u32 targets[MAX_BUS_ROWS];
			u32 targetCount = 0;
			for (u32 row = 0; row < count; ++row) {
				if (rows[row].kind != AudioBusRowKind::Send || !FindBus(rows[row].target)) {
					continue;
				}
				const u32 targetSlot = AudioBusAccess::GetSlot(rows[row].target);
				bool refused = targetSlot == AudioRenderer::ROOT_BUS || _buses.WouldCycle(slot, targetSlot);
				for (u32 earlier = 0; earlier < targetCount && !refused; ++earlier) {
					refused = targets[earlier] == targetSlot;
				}
				if (refused) {
					_buses.SetSends(slot, previous, previousCount);
					return false;
				}
				targets[targetCount++] = targetSlot;
				_buses.SetSends(slot, targets, targetCount);
			}

			BeginBatch();

			u32 receiving = 0;
			for (u32 row = 0; row < count; ++row) {
				const bool receives = rows[row].kind == AudioBusRowKind::Receive || rows[row].kind == AudioBusRowKind::Effect;
				receiving += receives && rows[row].receives ? 1u : 0u;
			}
			if (receiving > record->receiveCapacity) {
				void* grown = AudioBusTable::AllocateReceives(receiving);
				AudioCommand command = BusCommand(AudioCommandKind::SetBusReceives, bus);
				command.block = grown;
				command.mode = static_cast<u8>(receiving);
				Send(command);
				RetireMemory(record->receives);
				record->receives = grown;
				record->receiveCapacity = receiving;
			}

			u32 receive = 0;
			for (u32 row = 0; row < count; ++row) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusRow, bus);
				command.mode = static_cast<u8>(row);
				command.frames = static_cast<u32>(rows[row].kind);
				command.other = AudioBusAccess::GetSlot(rows[row].target);
				command.otherGeneration = AudioBusAccess::GetGeneration(rows[row].target);
				command.value = rows[row].level;
				command.effect = rows[row].effect;
				command.flag = rows[row].bypass;
				command.loop = rows[row].wetMixing;
				if (rows[row].kind == AudioBusRowKind::Send) {
					command.frame = rows[row].targetRow;
				} else {
					const bool receives = rows[row].receives && (rows[row].kind == AudioBusRowKind::Receive || rows[row].kind == AudioBusRowKind::Effect);
					command.frame = receives ? receive++ : NO_BUS_ROW;
				}
				Send(command);
			}
			AudioCommand trim = BusCommand(AudioCommandKind::SetBusRowCount, bus);
			trim.mode = static_cast<u8>(count);
			Send(trim);
			EndBatch();
			return true;
		}

		void AudioSystem::SetBusFader(AudioBus bus, f32 gain, u32 frames) {
			if (AudioBusRecord* record = FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusFader, bus);
				command.value = gain;
				command.frames = frames;
				if (Send(command)) {
					record->fader = gain;
					_busAudibilityDirty = true;
				}
			}
		}

		void AudioSystem::SetBusPitch(AudioBus bus, f32 pitch, u32 frames) {
			if (FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusPitch, bus);
				command.value = pitch;
				command.frames = frames;
				Send(command);
			}
		}

		void AudioSystem::SetBusRowLevel(AudioBus bus, u32 row, f32 level, u32 frames) {
			if (FindBus(bus) && row < MAX_BUS_ROWS) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusRowLevel, bus);
				command.mode = static_cast<u8>(row);
				command.value = level;
				command.frames = frames;
				Send(command);
			}
		}

		void AudioSystem::SetBusRowBypass(AudioBus bus, u32 row, bool bypass) {
			if (FindBus(bus) && row < MAX_BUS_ROWS) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusRowBypass, bus);
				command.mode = static_cast<u8>(row);
				command.flag = bypass;
				Send(command);
			}
		}

		void AudioSystem::SetBusBypassEffects(AudioBus bus, bool bypass) {
			if (FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusBypassEffects, bus);
				command.flag = bypass;
				Send(command);
			}
		}

		bool AudioSystem::SetBusRowEffect(AudioBus bus, u32 row, IAudioEffect* effect) {
			if (!FindBus(bus) || row >= MAX_BUS_ROWS) {
				return false;
			}
			AudioCommand command = BusCommand(AudioCommandKind::SetBusRowEffect, bus);
			command.mode = static_cast<u8>(row);
			command.effect = effect;
			return Send(command);
		}

		void AudioSystem::SetBusEffectParameter(AudioBus bus, u32 row, u32 index, f32 value) {
			if (FindBus(bus) && row < MAX_BUS_ROWS) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusEffectParameter, bus);
				command.mode = static_cast<u8>(row);
				command.frames = index;
				command.value = value;
				Send(command);
			}
		}

		AudioEffectContext AudioSystem::MakeBusEffectContext(std::span<const f32> parameters) const {
			AudioEffectContext context;
			context.sampleRate = _renderer.GetSampleRate();
			context.blockFrames = AudioRenderer::BLOCK_FRAMES;
			const ChannelLayout& output = _renderer.GetLayout();
			context.channels = output.count;
			for (u32 channel = 0; channel < output.count; ++channel) {
				context.speakers[channel] = output.positions[channel];
			}
			context.parameters = parameters;
			return context;
		}

		void AudioSystem::SetBusMute(AudioBus bus, bool mute) {
			if (AudioBusRecord* record = FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusMute, bus);
				command.flag = mute;
				if (Send(command)) {
					record->mute = mute;
					_busAudibilityDirty = true;
				}
			}
		}

		void AudioSystem::SetBusSolo(AudioBus bus, bool solo) {
			if (AudioBusRecord* record = FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusSolo, bus);
				command.flag = solo;
				if (Send(command)) {
					record->solo = solo;
					_busAudibilityDirty = true;
				}
			}
		}

		void AudioSystem::SetBusClock(AudioBus bus, AudioBusClock clock, bool followTimeScale) {
			if (AudioBusRecord* record = FindBus(bus)) {
				AudioCommand command = BusCommand(AudioCommandKind::SetBusClock, bus);
				command.mode = static_cast<u8>(clock);
				command.flag = followTimeScale;
				if (Send(command)) {
					record->clock = clock;
					record->followTimeScale = followTimeScale;
					_busAudibilityDirty = true;
				}
			}
		}

		void AudioSystem::NoteGameplay() {
			_gameplayThisFrame = true;
			SendGameClock();
		}

		void AudioSystem::EndFrameClock() {
			SendGameClock();
			_gameplayThisFrame = false;
		}

		void AudioSystem::SendGameClock() {
			if (!_up) {
				return;
			}
			const bool running = _gameplayThisFrame;
			const f32 scale = running ? Time::GetTimeScale() : _sentTimeScale;
			if (running == _sentRunning && scale == _sentTimeScale) {
				return;
			}
			AudioCommand command;
			command.kind = AudioCommandKind::SetGameClock;
			command.flag = running;
			command.value = scale;
			if (Send(command)) {
				_sentRunning = running;
				_sentTimeScale = scale;
				_busAudibilityDirty = true;
			}
		}

		void AudioSystem::SendGlobalVolume() {
			if (!_up) {
				return;
			}
			AudioCommand command;
			command.kind = AudioCommandKind::SetGlobalVolume;
			const f32 volume = GetSettings().globalVolume;
			command.value = _gameMuted ? 0.0f : (volume >= 0.0f ? (volume <= 1.0f ? volume : 1.0f) : 0.0f);
			Send(command);
		}

		void AudioSystem::SetGameMuted(bool muted) {
			if (_gameMuted == muted) {
				return;
			}
			_gameMuted = muted;
			SendGlobalVolume();
		}

		IAudioSpatializer* AudioSystem::ResolveSpatializer(bool spatialize) {
			if (!spatialize) {
				return &GetBuiltinSpatializer();
			}
			const str& name = GetSettings().spatializer;
			if (name.empty()) {
				if (!_warnedUnnamedSpatializer) {
					_warnedUnnamedSpatializer = true;
					SLT_LOG_WARN("Audio", "a sound asked to be spatialized, but no spatializer is selected in the Audio settings (using the built in one)");
				}
				return &GetBuiltinSpatializer();
			}
			if (IAudioSpatializer* found = FindSpatializer(name)) {
				return found;
			}
			if (!_warnedMissingSpatializer) {
				_warnedMissingSpatializer = true;
				SLT_LOG_WARN("Audio", "the spatializer '{}' selected in the Audio settings isn't registered (using the built in one)", name);
			}
			return &GetBuiltinSpatializer();
		}

		AudioSpatializerContext AudioSystem::MakeSpatializerContext(const AudioVoiceRecord& record) const {
			AudioSpatializerContext context;
			context.sampleRate = _renderer.GetSampleRate();
			context.blockFrames = AudioRenderer::BLOCK_FRAMES;
			if (record.clip) {
				context.inputChannels = record.clip->layout.count;
				for (u32 channel = 0; channel < record.clip->layout.count; ++channel) {
					context.inputSpeakers[channel] = record.clip->layout.positions[channel];
				}
			}
			const ChannelLayout& output = _renderer.GetLayout();
			context.outputChannels = output.count;
			for (u32 channel = 0; channel < output.count; ++channel) {
				context.outputSpeakers[channel] = output.positions[channel];
			}
			return context;
		}

		void AudioSystem::MakeSpatialState(AudioVoiceRecord& record, IAudioSpatializer* spatializer) {
			const AudioSpatializerContext context = MakeSpatializerContext(record);
			void* state = spatializer->CreateVoiceState(context);
			IAudioSpatializer* builtin = &GetBuiltinSpatializer();
			if (!state && spatializer != builtin) {
				if (!_warnedRefusedState) {
					_warnedRefusedState = true;
					SLT_LOG_WARN("Audio", "the selected spatializer rejected a sound with {} channel(s) onto {} (using the built-in one)", context.inputChannels,
								 context.outputChannels);
				}
				spatializer = builtin;
				state = spatializer->CreateVoiceState(context);
			}
			record.spatializer = state ? spatializer : nullptr;
			record.spatializerState = state;
		}

		bool AudioSystem::IsPlaced(const AudioVoiceRecord& record) {
			return record.spatializerState || record.spatial.spatialBlend > 0.0f || record.curves || (record.effects && record.effects->followsDistance);
		}

		bool AudioSystem::WantsSpatial(const AudioVoiceRecord& record) {
			return record.spatial.spatialBlend > 0.0f || (record.curves && !record.curves->spatialBlend.keys.empty());
		}

		f32 AudioSystem::EffectiveReverbZoneMix(const AudioVoiceRecord& record) {
			if (record.curves && !record.curves->reverbZoneMix.keys.empty()) {
				return Clamp(record.curves->reverbZoneMix.Evaluate(record.distanceShare), 0.0f, 1.1f);
			}
			return record.reverbZoneMix;
		}

		void AudioSystem::AttachSpatial(u32, AudioVoiceRecord& record) {
			if (record.spatializerState || !WantsSpatial(record)) {
				return;
			}
			MakeSpatialState(record, ResolveSpatializer(record.spatial.spatialize));
		}

		void AudioSystem::PlaceVoice(u32 slot, AudioVoiceRecord& record, std::span<const AudioListenerPose> listeners) {
			const AudioSettings& settings = GetSettings();
			const bool attenuates = record.spatializer && record.spatializer->AppliesDistanceAttenuation();
			AudioVoicePlacement placement = ComputePlacement(record.spatial, record.position, record.velocity, listeners, settings.volumeRolloffScale,
															 settings.dopplerFactor, attenuates, record.curves, &record.distanceShare);
			placement.generation = record.generation;
			placement.spatializer = record.spatializer;
			placement.state = record.spatializerState;
			record.placement = placement;
			_renderer.PostPlacement(slot, placement);
			FollowDistance(slot, record);
		}

		void AudioSystem::FollowDistance(u32 slot, AudioVoiceRecord& record) {
			if (!record.effects || !record.effects->followsDistance) {
				return;
			}
			const u32 rows = static_cast<u32>(record.effects->rows.size());
			for (u32 index = 0; index < rows; ++index) {
				const AudioVoiceEffectRow& row = record.effects->rows[index];
				if (row.distance.keys.empty() || !row.definition) {
					continue;
				}
				const std::span<const AudioParameterDefinition> parameters = row.definition->GetParameters();
				if (row.distanceParameter >= parameters.size()) {
					continue;
				}
				const AudioParameterDefinition& parameter = parameters[row.distanceParameter];
				const f32 share = Clamp(row.distance.Evaluate(record.distanceShare), 0.0f, 1.0f);
				SetVoiceEffectParameter(slot, record, index, row.distanceParameter, parameter.minimum + (parameter.maximum - parameter.minimum) * share);
			}
		}

		std::span<const AudioListenerPose> AudioSystem::GatherListeners() {
			if (!_listeners) {
				_listeners = New<Vector<AudioListenerPose>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
			}
			GatherListenerPoses(*_listeners);
			return {_listeners->data(), _listeners->size()};
		}

		void AudioSystem::PlaceVoices() {
			if (!_up || !_voices.IsInitialized()) {
				return;
			}
			DrainEvents();
			const bool mixing = IsAnyListenerMixing();
			const bool routing = mixing || _listenersRouted;
			_listenersRouted = mixing;
			const bool zoned = IsAnyListenerInZone();
			const bool sending = zoned || _listenersSent;
			_listenersSent = zoned;
			std::span<const AudioListenerPose> listeners;
			bool gathered = false;
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord* record = _voices.Find(slot, _voices.GetGeneration(slot));
				if (!record || (record->claim != VoiceClaim::Playing && record->claim != VoiceClaim::Paused)) {
					continue;
				}
				const bool placed = IsPlaced(*record);
				const bool routed = (routing && record->toListener) || sending;
				if (!placed && !routed) {
					continue;
				}
				if (!gathered) {
					listeners = GatherListeners();
					gathered = true;
				}
				if (placed) {
					if (record->real) {
						AttachSpatial(slot, *record);
					}
					PlaceVoice(slot, *record, listeners);
				}
				if (routed) {
					RouteToListener(slot, *record, listeners);
				}
			}
		}

		void AudioSystem::RouteToListener(u32 slot, AudioVoiceRecord& record, std::span<const AudioListenerPose> listeners) {
			const i32 hearing = FindHearingListener(listeners, WantsSpatial(record), record.position);
			const AudioListenerPose* listener = hearing >= 0 ? &listeners[static_cast<size_t>(hearing)] : nullptr;

			if (record.toListener) {
				const AudioBus route = listener ? RouteThrough(*listener, record.bypassEffects || record.bypassListenerEffects) : AudioBus{};
				const u32 bus = AudioBusAccess::GetSlot(route);
				const u32 generation = AudioBusAccess::GetGeneration(route);
				if (bus != record.bus || generation != record.busGeneration) {
					AudioCommand command;
					command.kind = AudioCommandKind::SetVoiceOutput;
					command.voice = slot;
					command.generation = record.generation;
					command.bus = bus;
					command.busGeneration = generation;
					if (Send(command)) {
						record.bus = bus;
						record.busGeneration = generation;
					}
				}
			}

			const f32 wanted = IsDeviceClocked(record.bus, record.busGeneration) ? 0.0f : ComputeZoneSendLevel(EffectiveReverbZoneMix(record));
			const AudioBus target = listener && wanted > 0.0f ? listener->zones : AudioBus{};
			const u32 send = AudioBusAccess::GetSlot(target);
			const u32 sendGeneration = AudioBusAccess::GetGeneration(target);
			const f32 level = sendGeneration != 0 ? wanted : 0.0f;
			if (send != record.send || sendGeneration != record.sendGeneration || level != record.sendLevel) {
				AudioCommand command;
				command.kind = AudioCommandKind::SetVoiceSend;
				command.voice = slot;
				command.generation = record.generation;
				command.bus = send;
				command.busGeneration = sendGeneration;
				command.value = level;
				if (Send(command)) {
					record.send = send;
					record.sendGeneration = sendGeneration;
					record.sendLevel = level;
				}
			}
		}

		void AudioSystem::RouteToListener(u32 slot, AudioVoiceRecord& record) {
			const bool gather = IsAnyListenerMixing() || IsAnyListenerInZone();
			RouteToListener(slot, record, gather ? GatherListeners() : std::span<const AudioListenerPose>{});
		}

		void AudioSystem::DetachSpatializer(IAudioSpatializer* spatializer, AudioDetachment& detachment) {
			if (!spatializer || !_voices.IsInitialized()) {
				return;
			}
			DrainEvents();
			Vector<u32> slots{GetMemoryResource(MemTag::Audio)};
			Vector<IAudioSpatializer*> to{GetMemoryResource(MemTag::Audio)};
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord* record = _voices.Find(slot, _voices.GetGeneration(slot));
				if (record && record->claim != VoiceClaim::Free && record->spatializer == spatializer) {
					slots.push_back(slot);
					to.push_back(&GetBuiltinSpatializer());
				}
			}
			PostSpatialSwaps(slots.data(), to.data(), static_cast<u32>(slots.size()), detachment);

			if (_retiring) {
				u32 kept = 0;
				for (const Retiring& retiring : *_retiring) {
					if (retiring.spatializer == spatializer) {
						detachment.states.push_back({retiring.spatializer, retiring.state});
						detachment.posted = true;
					} else {
						(*_retiring)[kept++] = retiring;
					}
				}
				_retiring->resize(kept);
			}
		}

		void AudioSystem::ReselectSpatializers() {
			_warnedUnnamedSpatializer = false;
			_warnedMissingSpatializer = false;
			if (!_voices.IsInitialized()) {
				return;
			}
			DrainEvents();
			Vector<u32> slots{GetMemoryResource(MemTag::Audio)};
			Vector<IAudioSpatializer*> to{GetMemoryResource(MemTag::Audio)};
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord* record = _voices.Find(slot, _voices.GetGeneration(slot));
				if (!record || record->claim == VoiceClaim::Free || !record->spatializerState || !record->spatial.spatialize) {
					continue;
				}
				IAudioSpatializer* wanted = ResolveSpatializer(true);
				if (wanted != record->spatializer) {
					slots.push_back(slot);
					to.push_back(wanted);
				}
			}
			AudioDetachment detachment;
			PostSpatialSwaps(slots.data(), to.data(), static_cast<u32>(slots.size()), detachment);
			CompleteDetachment(detachment);
		}

		void AudioSystem::PostSpatialSwaps(const u32* slots, IAudioSpatializer* const* to, u32 count, AudioDetachment& detachment) {
			for (u32 index = 0; index < count; ++index) {
				AudioVoiceRecord* record = _voices.Find(slots[index], _voices.GetGeneration(slots[index]));
				if (record->spatializer && record->spatializerState) {
					detachment.states.push_back({record->spatializer, record->spatializerState});
				}
				MakeSpatialState(*record, to[index]);
				record->placement.spatializer = record->spatializer;
				record->placement.state = record->spatializerState;
				_renderer.PostPlacement(slots[index], record->placement);
				detachment.posted = true;
			}
		}

		void AudioSystem::PrepareForDevice() {
			_preparing = true;
			_renderer.ApplyPendingCommands();
			RemakeSpatialStates();
			RemakeVoiceChains();
			RemakeMixerEffects();
			RemakeListenerEffects();
			_preparing = false;
		}

		void AudioSystem::RemakeSpatialStates() {
			if (!_voices.IsInitialized()) {
				return;
			}
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord* record = _voices.Find(slot, _voices.GetGeneration(slot));
				if (!record || record->claim == VoiceClaim::Free || !record->spatializerState) {
					continue;
				}
				IAudioSpatializer* spatializer = record->spatializer;
				void* old = record->spatializerState;
				MakeSpatialState(*record, spatializer);
				spatializer->DestroyVoiceState(old);
				record->placement.spatializer = record->spatializer;
				record->placement.state = record->spatializerState;
				_renderer.PostPlacement(slot, record->placement);
			}
		}

		void AudioSystem::FenceRenderThread() {
			if (!_device.IsOpen()) {
				return;
			}
			AudioClockStamp stamp;
			_renderer.ReadClock(stamp);
			const u64 until = stamp.frames + 2 * AudioRenderer::BLOCK_FRAMES;
			const u64 giveUp = Time::GetTicks() + Time::GetTicksPerSecond();
			for (;;) {
				if (_renderer.ReadClock(stamp) && stamp.frames >= until) {
					return;
				}
				if (Time::GetTicks() > giveUp) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			SLT_LOG_WARN("Audio", "the audio device rendered nothing for 1 s while code was taken off it (reopened)");
			CloseDevice();
			Open(GetSettings());
		}

		u32 AudioSystem::LiveBusOrRoot(u32 slot, u32 generation) const {
			return _buses.Find(slot, generation) ? slot : AudioRenderer::ROOT_BUS;
		}

		void AudioSystem::RefreshBusAudibility() {
			_busAudibilityDirty = false;
			const bool running = _sentRunning;
			const bool scaleStopped = !(_sentTimeScale > 0.0f);
			bool anySolo = false;
			for (u32 slot = 0; slot < AudioRenderer::MAX_BUSES; ++slot) {
				const AudioBusRecord* bus = _buses.FindLive(slot);
				anySolo = anySolo || (bus && bus->solo);
			}
			for (u32 slot = 0; slot < AudioRenderer::MAX_BUSES; ++slot) {
				const AudioBusRecord* bus = _buses.FindLive(slot);
				const AudioBusRecord* clocked = bus;
				for (u32 depth = 0; clocked && clocked->clock == AudioBusClock::Inherit && depth < AudioRenderer::MAX_BUSES; ++depth) {
					clocked = clocked->parent != AudioRenderer::ROOT_BUS ? _buses.FindLive(clocked->parent) : nullptr;
				}
				const bool gameTime = clocked && (clocked->clock == AudioBusClock::Gameplay || clocked->followTimeScale);
				if (!bus || !running || (scaleStopped && gameTime)) {
					_busAudibility[slot] = 0.0f;
					continue;
				}
				f32 gain = 1.0f;
				bool soloed = false;
				u32 walk = slot;
				for (u32 depth = 0; depth < AudioRenderer::MAX_BUSES; ++depth) {
					const AudioBusRecord* step = _buses.FindLive(walk);
					if (!step) {
						break;
					}
					soloed = soloed || step->solo;
					if (walk == AudioRenderer::ROOT_BUS) {
						break;
					}
					gain *= step->mute ? 0.0f : Clamp(step->fader, 0.0f, MAX_VOICE_GAIN);
					walk = step->parent;
				}
				_busAudibility[slot] = anySolo && !soloed ? 0.0f : gain;
			}
		}

		f32 AudioSystem::ComputeAudibility(f32 volume, f32 spatialBlend, f32 distanceAttenuation, u32 busSlot, u32 busGeneration) {
			const f32 blend = Clamp(spatialBlend, 0.0f, 1.0f);
			const f32 place = (1.0f - blend) + blend * Clamp(distanceAttenuation, 0.0f, 1.0f);
			if (_busAudibilityDirty) {
				RefreshBusAudibility();
			}
			return Clamp(volume, 0.0f, MAX_VOICE_GAIN) * place * _busAudibility[LiveBusOrRoot(busSlot, busGeneration)];
		}

		f32 AudioSystem::ComputeAudibility(const AudioVoiceRecord& record) {
			if (record.claim != VoiceClaim::Playing) {
				return 0.0f;
			}
			const f32 blend = record.curves ? record.placement.blend : record.spatial.spatialBlend;
			return ComputeAudibility(record.volume, blend, record.placement.params.distanceAttenuation, record.bus, record.busGeneration);
		}

		f32 AudioSystem::DistanceToNearest(const Vec3& position, std::span<const AudioListenerPose> listeners) {
			const i32 nearest = FindNearestListener(listeners, position);
			return nearest < 0 ? 0.0f : (listeners[static_cast<size_t>(nearest)].position - position).Length();
		}

		void AudioSystem::StopToMakeWay(u32 slot, AudioVoiceRecord& record, u32 fadeFrames) {
			AudioCommand command;
			command.kind = AudioCommandKind::Stop;
			command.voice = slot;
			command.generation = record.generation;
			command.frames = fadeFrames;
			if (Send(command)) {
				_voices.BeginStop(slot, record.real && fadeFrames > AudioRenderer::BLOCK_FRAMES);
			}
		}

		bool AudioSystem::AdmitVoice(const AudioConcurrency* concurrency, u64 owner, const Vec3& position, const AudioConcurrencyMember& incoming,
									 AudioConcurrencyGroup*& group) {
			group = nullptr;
			const u32 rate = _renderer.GetSampleRate();

			if (concurrency) {
				group = _voices.GetGroups().FindOrAdd(concurrency->GetInstanceId(), concurrency->limitToOwner ? owner : 0);
			}
			if (group) {
				if (group->started && GetTimeFrames() < group->lastStart + group->retriggerFrames) {
					++_budgetCounts.refusedByConcurrency;
					return false;
				}
				const u32 maxCount = concurrency->maxCount > 1 ? static_cast<u32>(concurrency->maxCount) : 1u;
				if (group->count >= maxCount) {
					std::span<const AudioListenerPose> listeners = GatherListeners();
					_members->clear();
					for (u32 slot = group->head; slot != NO_VOICE_SLOT; slot = _voices.At(slot).groupNext) {
						const AudioVoiceRecord& member = _voices.At(slot);
						_members->push_back(AudioConcurrencyMember{slot, member.priority, member.audibility, DistanceToNearest(member.position, listeners),
																   member.sequence});
					}
					AudioConcurrencyMember weighed = incoming;
					weighed.distance = DistanceToNearest(position, listeners);
					const u32 victim = ChooseConcurrencyVictim(concurrency->resolutionRule, {_members->data(), _members->size()}, weighed);
					if (victim == NO_VOICE_SLOT) {
						++_budgetCounts.refusedByConcurrency;
						return false;
					}
					const f32 release = concurrency->voiceStealReleaseTime > 0.0f ? concurrency->voiceStealReleaseTime : 0.0f;
					StopToMakeWay(victim, _voices.At(victim), static_cast<u32>(static_cast<f64>(release) * static_cast<f64>(rate)));
					++_budgetCounts.stoppedByConcurrency;
				}
			}

			if (_voices.CountLive() < _voices.GetLimit()) {
				_warnedPoolFull = false;
				return true;
			}
			const u32 victim = FindLeastImportant(true);
			if (victim != NO_VOICE_SLOT) {
				const AudioVoiceRecord& weakest = _voices.At(victim);
				const bool outranks = incoming.priority != weakest.priority ? incoming.priority < weakest.priority : incoming.audibility > weakest.audibility;
				if (outranks) {
					StopToMakeWay(victim, _voices.At(victim), 0);
					++_budgetCounts.stoppedByPool;
					return true;
				}
			}
			++_budgetCounts.refusedByPool;
			if (!_warnedPoolFull) {
				_warnedPoolFull = true;
				SLT_LOG_WARN("Audio", "all {} voices Max Virtual Voices allows are playing, none less important than the new one. Raise the limit in the Audio settings",
							 _voices.GetLimit());
			}
			return false;
		}

		u32 AudioSystem::FindLeastImportant(bool spareFirstPriority) {
			u32 victim = NO_VOICE_SLOT;
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				const AudioVoiceRecord& candidate = _voices.At(slot);
				if (candidate.claim != VoiceClaim::Playing && candidate.claim != VoiceClaim::Paused) {
					continue;
				}
				if (spareFirstPriority && candidate.priority <= MIN_PRIORITY) {
					continue;
				}
				if (victim == NO_VOICE_SLOT) {
					victim = slot;
					continue;
				}
				const AudioVoiceRecord& current = _voices.At(victim);
				const bool lessImportant = candidate.priority != current.priority		? candidate.priority > current.priority
										   : candidate.audibility != current.audibility ? candidate.audibility < current.audibility
																						: candidate.sequence < current.sequence;
				victim = lessImportant ? slot : victim;
			}
			return victim;
		}

		bool AudioSystem::DecideAtStart(u32 slot, AudioVoiceRecord& record) {
			if (_voices.CountBudgeted() < _maxRealVoices) {
				return true;
			}
			AudioVoiceRank incoming{slot, record.priority, record.audibility, record.sequence, false, false};
			u32 weakest = NO_VOICE_SLOT;
			AudioVoiceRank weakestRank;
			for (const u32 real : _voices.GetRealSlots()) {
				const AudioVoiceRecord& heard = _voices.At(real);
				if (heard.claim != VoiceClaim::Playing && heard.claim != VoiceClaim::Paused) {
					continue;
				}
				const AudioVoiceRank rank{real, heard.priority, heard.audibility, heard.sequence, true, true};
				if (weakest == NO_VOICE_SLOT || RanksAbove(weakestRank, rank)) {
					weakest = real;
					weakestRank = rank;
				}
			}
			if (weakest == NO_VOICE_SLOT || !RanksAbove(incoming, weakestRank)) {
				return false;
			}
			SetVoiceReal(weakest, _voices.At(weakest), false, {});
			return !_voices.At(weakest).real;
		}

		void AudioSystem::SetVoiceReal(u32 slot, AudioVoiceRecord& record, bool real, std::span<const AudioListenerPose> listeners) {
			if (record.real == real) {
				return;
			}
			if (real && IsPlaced(record)) {
				AttachSpatial(slot, record);
				PlaceVoice(slot, record, listeners.empty() ? GatherListeners() : listeners);
			}
			if (real) {
				AttachVoiceChain(slot, record);
			}
			AudioCommand commands[2];
			u32 count = 0;
			const bool streams = real && MakeVoiceStream(slot, record, commands[count]);
			count += streams ? 1u : 0u;
			AudioCommand& command = commands[count++];
			command.kind = AudioCommandKind::SetVoiceReal;
			command.voice = slot;
			command.generation = record.generation;
			command.flag = real;
			if (!SendWhole({commands, count})) {
				if (streams) {
					DiscardVoiceStream(record);
				}
				return;
			}
			if (streams) {
				CommitVoiceStream(record);
			}
			_voices.SetReal(slot, real);
			if (!real) {
				if (GetSettings().virtualizeEffects) {
					RetireSpatialState(record);
				}
				RetireVoiceChain(record);
				RetireVoiceStream(record, false);
			}
		}

		void AudioSystem::RetireSpatialState(AudioVoiceRecord& record) {
			if (!record.spatializerState || _retiring->size() >= _retiring->capacity()) {
				return;
			}
			AudioClockStamp stamp;
			_renderer.ReadClock(stamp);
			Retiring retiring;
			retiring.spatializer = record.spatializer;
			retiring.state = record.spatializerState;
			retiring.safeAt = stamp.frames + 2 * AudioRenderer::BLOCK_FRAMES;
			_retiring->push_back(retiring);
			record.spatializer = nullptr;
			record.spatializerState = nullptr;
			record.placement.spatializer = nullptr;
			record.placement.state = nullptr;
		}

		void AudioSystem::RetireEffect(IAudioEffect* effect) {
			if (effect) {
				Retiring retiring;
				retiring.effect = effect;
				Retire(retiring);
			}
		}

		void AudioSystem::RetireMemory(void* memory) {
			if (memory) {
				Retiring retiring;
				retiring.memory = memory;
				Retire(retiring);
			}
		}

		void AudioSystem::ReleaseRetired(const Retiring& retiring) {
			if (retiring.chain) {
				ReleaseVoiceChain(retiring.chain);
			} else if (retiring.effect) {
				retiring.effect->Release();
			} else if (retiring.memory) {
				Free(retiring.memory);
			} else if (retiring.spatializer) {
				retiring.spatializer->DestroyVoiceState(retiring.state);
			}
		}

		void AudioSystem::Retire(Retiring retiring) {
			if (_batchDepth > 0) {
				if (!_batchRetiring) {
					_batchRetiring = New<Vector<Retiring>>(MemTag::Audio, GetMemoryResource(MemTag::Audio));
				}
				_batchRetiring->push_back(retiring);
				return;
			}
			if (!_device.IsOpen() || _preparing || !_retiring) {
				ReleaseRetired(retiring);
				return;
			}
			if (_retiring->size() >= _retiring->capacity()) {
				FenceRenderThread();
				DestroyRetiredStates(true);
			}
			AudioClockStamp stamp;
			_renderer.ReadClock(stamp);
			retiring.safeAt = stamp.frames + 2 * AudioRenderer::BLOCK_FRAMES;
			_retiring->push_back(retiring);
		}

		void AudioSystem::DestroyRetiredStates(bool everything) {
			if (!_retiring || _retiring->empty()) {
				return;
			}
			AudioClockStamp stamp;
			const bool rendered = _renderer.ReadClock(stamp);
			u32 kept = 0;
			for (const Retiring& retiring : *_retiring) {
				if (everything || (rendered && stamp.frames >= retiring.safeAt)) {
					ReleaseRetired(retiring);
				} else {
					(*_retiring)[kept++] = retiring;
				}
			}
			_retiring->resize(kept);
		}

		bool AudioSystem::MakeVoiceStream(u32 slot, AudioVoiceRecord& record, AudioCommand& command) {
			AudioClipData* clip = record.clip;
			if (record.stream || !clip || clip->encoding != AudioClipEncoding::Streamed || clip->frames <= clip->primeFrames) {
				return false;
			}
			if (_liveStreams >= _streamer.GetCapacity()) {
				if (!_warnedStreamRoom) {
					_warnedStreamRoom = true;
					SLT_LOG_WARN("Audio", "{} streams are already being read (a voice made heard plays only its 1st frames)",
								 _liveStreams);
				}
				return false;
			}
			const i32 blocks = GetSettings().streamAheadBlocks;
			AudioStream* stream = CreateStream(clip, static_cast<u32>(blocks < 1 ? 1 : blocks) * AudioRenderer::BLOCK_FRAMES);
			if (!stream) {
				return false;
			}
			++_liveStreams;
			record.stream = stream;
			command = AudioCommand{};
			command.kind = AudioCommandKind::SetVoiceStream;
			command.voice = slot;
			command.generation = record.generation;
			command.stream = stream;
			return true;
		}

		void AudioSystem::CommitVoiceStream(AudioVoiceRecord& record) {
			if (record.stream && _streamer.Add(record.stream)) {
				_streamer.Wake();
			}
		}

		void AudioSystem::DiscardVoiceStream(AudioVoiceRecord& record) {
			if (record.stream) {
				DestroyStream(record.stream);
				record.stream = nullptr;
				--_liveStreams;
			}
		}

		void AudioSystem::RetireVoiceStream(AudioVoiceRecord& record, bool renderDone) {
			AudioStream* stream = record.stream;
			if (!stream || !_retiringStreams) {
				return;
			}
			record.stream = nullptr;
			AudioClockStamp stamp;
			_renderer.ReadClock(stamp);
			RetiringStream retiring;
			retiring.stream = stream;
			retiring.safeAt = renderDone ? 0 : stamp.frames + 2 * AudioRenderer::BLOCK_FRAMES;
			_retiringStreams->push_back(retiring);
			stream->retiring.store(true, std::memory_order_release);
			_streamer.Wake();
		}

		void AudioSystem::DestroyRetiredStreams(bool everything) {
			if (!_retiringStreams || _retiringStreams->empty()) {
				return;
			}
			AudioClockStamp stamp;
			const bool rendered = _renderer.ReadClock(stamp);
			u32 kept = 0;
			for (const RetiringStream& retiring : *_retiringStreams) {
				const bool renderDone = everything || retiring.safeAt == 0 || (rendered && stamp.frames >= retiring.safeAt);
				if (renderDone && retiring.stream->released.load(std::memory_order_acquire)) {
					DestroyStream(retiring.stream);
					--_liveStreams;
				} else {
					(*_retiringStreams)[kept++] = retiring;
				}
			}
			_retiringStreams->resize(kept);
		}

		void AudioSystem::CompleteDetachment(AudioDetachment& detachment) {
			if (detachment.IsEmpty()) {
				return;
			}
			if (detachment.posted) {
				FenceRenderThread();
			}
			for (const AudioDetachment::State& state : detachment.states) {
				if (state.spatializer && state.state) {
					state.spatializer->DestroyVoiceState(state.state);
				}
			}
			for (IAudioEffect* effect : detachment.effects) {
				effect->Release();
			}
			for (IAudioGeneratorInstance* generator : detachment.generators) {
				generator->Release();
			}
			if (detachment.posted) {
				DestroyRetiredStates(true);
			}
			detachment.states.clear();
			detachment.effects.clear();
			detachment.generators.clear();
			detachment.posted = false;
			if (_reopenAfterDetach) {
				_reopenAfterDetach = false;
				Open(GetSettings());
			}
		}

		void AudioSystem::OnImageUnloading(const void* image) {
			if (!image) {
				return;
			}
			AudioDetachment detachment;
			Vector<IAudioSpatializer*> spatializers{GetMemoryResource(MemTag::Audio)};
			TakeSpatializersOwnedBy(image, spatializers);
			for (IAudioSpatializer* spatializer : spatializers) {
				DetachSpatializer(spatializer, detachment);
			}
			Vector<IAudioEffectDefinition*> effects{GetMemoryResource(MemTag::Audio)};
			TakeEffectsOwnedBy(image, effects);
			for (IAudioEffectDefinition* definition : effects) {
				DetachMixerEffects(definition, detachment);
				DetachVoiceEffects(definition, detachment);
				DetachListenerEffects(definition, detachment);
			}
			const u32 generatorsBefore = static_cast<u32>(detachment.generators.size());
			DetachGenerators(image, detachment);
			const u32 generators = static_cast<u32>(detachment.generators.size()) - generatorsBefore;

			const u32 instances = static_cast<u32>(detachment.effects.size());
			CompleteDetachment(detachment);
			if (!spatializers.empty() || !effects.empty() || generators > 0) {
				SLT_LOG_INFO("Audio", "an image unloaded: {} spatializer(s), {} effect definition(s) with {} instance(s) and {} generator voice(s) taken off the audio thread",
							 spatializers.size(), effects.size(), instances, generators);
			}
		}

		void AudioSystem::SendGeneratorParameter(u32 slot, u32 generation, u32 index, f32 value) {
			AudioCommand command;
			command.kind = AudioCommandKind::SetGeneratorParameter;
			command.voice = slot;
			command.generation = generation;
			command.frames = index;
			command.value = value;
			Send(command);
		}

		void AudioSystem::DetachGenerators(const void* image, AudioDetachment& detachment) {
			if (!_voices.IsInitialized() || _voices.CountGenerators() == 0) {
				return;
			}
			DrainEvents();
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (!record.isGenerator || record.claim == VoiceClaim::Free || record.generatorImage != image || !record.generatorInstance) {
					continue;
				}
				AudioCommand command;
				command.kind = AudioCommandKind::SetVoiceGenerator;
				command.voice = slot;
				command.generation = record.generation;
				command.generator = nullptr;
				SendNow(command);
				detachment.generators.push_back(record.generatorInstance);
				detachment.posted = true;
				record.generatorInstance = nullptr;
				_voices.SetOrphaned(slot, true);
			}
		}

		void AudioSystem::UpdateGenerators() {
			if (!_up || !_voices.IsInitialized() || _voices.CountGenerators() == 0) {
				return;
			}
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (!record.isGenerator || (record.claim != VoiceClaim::Playing && record.claim != VoiceClaim::Paused)) {
					continue;
				}
				AudioGenerator* generator = AudioGeneratorAccess::Resolve(record.resource);
				if (!generator) {
					continue;
				}
				if (record.orphaned) {
					ResumeOrphan(slot, record, *generator);
					continue;
				}
				AudioGeneratorControl control = AudioGeneratorAccess::MakeControl(AudioVoiceAccess::Make(slot, record.generation));
				generator->Update(control);
			}
		}

		void AudioSystem::ResumeOrphan(u32 slot, AudioVoiceRecord& record, AudioGenerator& generator) {
			AudioGeneratorFormat suggested;
			suggested.sampleRate = record.clip ? record.clip->sampleRate : _renderer.GetSampleRate();
			suggested.channels = record.clip ? record.clip->channels : 1;
			AudioGeneratorFormat chosen;
			IAudioGeneratorInstance* instance = generator.CreateInstance(suggested, chosen);
			if (!instance) {
				return;
			}
			if (!record.clip || chosen.sampleRate != record.clip->sampleRate || chosen.channels != record.clip->channels) {
				instance->Release();
				SLT_LOG_WARN("Audio", "the generator '{}' chose another format ({} Hz x {}). Stopped its sound",
							 generator.virtualPath, chosen.sampleRate, chosen.channels);
				Audio::Stop(AudioVoiceAccess::Make(slot, record.generation));
				return;
			}
			AudioCommand command;
			command.kind = AudioCommandKind::SetVoiceGenerator;
			command.voice = slot;
			command.generation = record.generation;
			command.generator = instance;
			if (!Send(command)) {
				instance->Release();
				return;
			}
			_voices.SetGenerator(slot, instance, generator.GetType()->ownerModule);
			_voices.SetOrphaned(slot, false);
		}

		void AudioSystem::SetVoiceEffects(u32 slot, AudioVoiceRecord& record, std::span<const AudioVoiceEffect> effects) {
			if (!record.effects) {
				if (effects.empty()) {
					return;
				}
				record.effects = New<AudioVoiceEffects>(MemTag::Audio);
			}
			DescribeEffects(*record.effects, effects);
			ReadDistance(*record.effects, record.distanceShare);
			const bool now = record.real || !GetSettings().virtualizeEffects;
			AudioVoiceChain* chain = now ? MakeChainFor(record, nullptr) : nullptr;
			if (chain || record.effects->chain) {
				SwapVoiceChain(slot, record, chain);
			}
		}

		bool AudioSystem::MakeStartingEffects(u32 slot, AudioVoiceRecord& record, std::span<const AudioVoiceEffect> effects, AudioCommand& command) {
			if (effects.empty() || record.effects) {
				return false;
			}
			record.effects = New<AudioVoiceEffects>(MemTag::Audio);
			DescribeEffects(*record.effects, effects);
			ReadDistance(*record.effects, record.distanceShare);
			if (!record.real && GetSettings().virtualizeEffects) {
				return false;
			}
			record.effects->chain = MakeChainFor(record, nullptr);
			if (!record.effects->chain) {
				return false;
			}
			command = AudioCommand{};
			command.kind = AudioCommandKind::SetVoiceEffects;
			command.voice = slot;
			command.generation = record.generation;
			command.chain = record.effects->chain;
			return true;
		}

		void AudioSystem::SetVoiceEffectParameter(u32 slot, AudioVoiceRecord& record, u32 index, u32 parameter, f32 value) {
			if (!record.effects || index >= record.effects->rows.size()) {
				return;
			}
			AudioVoiceEffectRow& row = record.effects->rows[index];
			if (!row.definition) {
				if (parameter < row.values.size()) {
					row.values[parameter] = value;
				}
				return;
			}
			if (parameter >= row.definition->GetParameters().size()) {
				return;
			}
			const f32 clamped = ClampParameterValue(*row.definition, parameter, value);
			if (row.values[parameter] == clamped) {
				return;
			}
			row.values[parameter] = clamped;
			const AudioVoiceChain* chain = record.effects->chain;
			if (chain && index < chain->count && chain->effects[index]) {
				AudioCommand command;
				command.kind = AudioCommandKind::SetVoiceEffectParameter;
				command.voice = slot;
				command.generation = record.generation;
				command.other = index;
				command.frames = parameter;
				command.value = clamped;
				Send(command);
			}
		}

		void AudioSystem::OnVoiceSpatialChanged(u32 slot, AudioVoiceRecord& record) {
			if (!record.effects || !record.effects->chain) {
				return;
			}
			const bool onOutput = record.spatial.spatialize && !record.spatial.spatializePostEffects;
			if (record.effects->chain->onOutput != onOutput) {
				SwapVoiceChain(slot, record, MakeChainFor(record, nullptr));
			}
		}

		void AudioSystem::OnVoiceEffectRegistered(std::string_view name) {
			if (!_up || !_voices.IsInitialized()) {
				return;
			}
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if ((record.claim != VoiceClaim::Playing && record.claim != VoiceClaim::Paused) || !record.effects) {
					continue;
				}
				if (ResolveWaiting(*record.effects, name) && record.effects->chain) {
					SwapVoiceChain(slot, record, MakeChainFor(record, record.effects->chain));
				}
			}
		}

		void AudioSystem::DetachVoiceEffects(IAudioEffectDefinition* definition, AudioDetachment& detachment) {
			if (!_voices.IsInitialized() || !definition) {
				return;
			}
			DrainEvents();
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (record.claim == VoiceClaim::Free || !record.effects) {
					continue;
				}
				bool named = false;
				for (AudioVoiceEffectRow& row : record.effects->rows) {
					if (row.definition == definition) {
						row.definition = nullptr;
						named = true;
					}
				}
				AudioVoiceChain* old = record.effects->chain;
				if (!named || !old || !ChainRuns(*old, definition)) {
					continue;
				}
				AudioVoiceChain* fresh = MakeChainFor(record, old);
				AudioCommand command;
				command.kind = AudioCommandKind::SetVoiceEffects;
				command.voice = slot;
				command.generation = record.generation;
				command.chain = fresh;
				SendNow(command);
				for (u32 index = 0; index < old->count; ++index) {
					if (old->definitions[index] == definition && old->effects[index]) {
						detachment.effects.push_back(old->effects[index]);
					}
				}
				RetireMemory(old);
				record.effects->chain = fresh;
				detachment.posted = true;
			}
		}

		void AudioSystem::AttachVoiceChain(u32 slot, AudioVoiceRecord& record) {
			if (!record.effects || record.effects->rows.empty()) {
				return;
			}
			if (record.effects->chain && record.effects->chain->keptWhileVirtual) {
				return;
			}
			SwapVoiceChain(slot, record, MakeChainFor(record, nullptr));
		}

		void AudioSystem::RetireVoiceChain(AudioVoiceRecord& record) {
			AudioVoiceChain* chain = record.effects ? record.effects->chain : nullptr;
			if (!chain || chain->keptWhileVirtual || _retiring->size() >= _retiring->capacity()) {
				return;
			}
			AudioClockStamp stamp;
			_renderer.ReadClock(stamp);
			Retiring retiring;
			retiring.chain = chain;
			retiring.safeAt = stamp.frames + 2 * AudioRenderer::BLOCK_FRAMES;
			_retiring->push_back(retiring);
			record.effects->chain = nullptr;
		}

		AudioVoiceChain* AudioSystem::MakeChainFor(const AudioVoiceRecord& record, const AudioVoiceChain* carry) const {
			const bool onOutput = record.spatial.spatialize && !record.spatial.spatializePostEffects;
			const ChannelLayout& layout = onOutput || !record.clip ? _renderer.GetLayout() : record.clip->layout;
			return MakeVoiceChain(*record.effects, _renderer.GetSampleRate(), layout, onOutput, !GetSettings().virtualizeEffects, carry);
		}

		bool AudioSystem::SwapVoiceChain(u32 slot, AudioVoiceRecord& record, AudioVoiceChain* chain) {
			AudioVoiceChain* old = record.effects->chain;
			AudioCommand command;
			command.kind = AudioCommandKind::SetVoiceEffects;
			command.voice = slot;
			command.generation = record.generation;
			command.chain = chain;
			if (!Send(command)) {
				ReleaseVoiceChain(chain, old);
				return false;
			}
			record.effects->chain = chain;
			if (old) {
				for (u32 index = 0; index < old->count; ++index) {
					IAudioEffect* effect = old->effects[index];
					bool carried = false;
					for (u32 other = 0; chain && other < chain->count && !carried; ++other) {
						carried = chain->effects[other] == effect;
					}
					if (effect && !carried) {
						RetireEffect(effect);
					}
				}
				RetireMemory(old);
			}
			return true;
		}

		void AudioSystem::RemakeVoiceChains() {
			if (!_voices.IsInitialized()) {
				return;
			}
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (record.claim == VoiceClaim::Free || !record.effects || !record.effects->chain) {
					continue;
				}
				AudioVoiceChain* old = record.effects->chain;
				AudioVoiceChain* fresh = MakeChainFor(record, nullptr);
				AudioCommand command;
				command.kind = AudioCommandKind::SetVoiceEffects;
				command.voice = slot;
				command.generation = record.generation;
				command.chain = fresh;
				if (!Send(command)) {
					ReleaseVoiceChain(fresh);
					continue;
				}
				ReleaseVoiceChain(old);
				record.effects->chain = fresh;
			}
		}

		void AudioSystem::RankVoices() {
			if (!_up || !_voices.IsInitialized() || !_ranks) {
				return;
			}
			RefreshBusAudibility();

			_ranks->clear();
			u32 releasing = 0;
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (record.claim == VoiceClaim::Stopping) {
					releasing += record.real && record.releasing ? 1u : 0u;
					continue;
				}
				if (record.claim == VoiceClaim::Free) {
					continue;
				}
				record.audibility = ComputeAudibility(record);
				_ranks->push_back(AudioVoiceRank{slot, record.priority, record.audibility, record.sequence, record.real, false});
			}
			const u32 budget = _maxRealVoices > releasing ? _maxRealVoices - releasing : 0;
			SelectRealVoices({_ranks->data(), _ranks->size()}, budget, {_rankOrder->data(), _rankOrder->size()});

			BeginBatch();
			for (const AudioVoiceRank& rank : *_ranks) {
				if (rank.real && !rank.wantReal) {
					SetVoiceReal(rank.slot, _voices.At(rank.slot), false, {});
				}
			}
			std::span<const AudioListenerPose> listeners;
			bool gathered = false;
			for (const AudioVoiceRank& rank : *_ranks) {
				if (!rank.real && rank.wantReal) {
					AudioVoiceRecord& record = _voices.At(rank.slot);
					if (!gathered && IsPlaced(record)) {
						listeners = GatherListeners();
						gathered = true;
					}
					SetVoiceReal(rank.slot, record, true, listeners);
				}
			}
			EndBatch();

			DestroyRetiredStates(!_device.IsOpen());
			const bool virtualize = GetSettings().virtualizeEffects;
			for (const AudioVoiceRank& rank : *_ranks) {
				AudioVoiceRecord& record = _voices.At(rank.slot);
				if (record.real) {
					continue;
				}
				if (virtualize && record.spatializerState && _retiring->size() < _retiring->capacity()) {
					RetireSpatialState(record);
				}
				RetireVoiceChain(record);
			}

			_voices.GetGroups().Sweep(GetTimeFrames());
		}

		void AudioSystem::ReserveRankingScratch() {
			const u32 capacity = _voices.GetCapacity();
			_ranks->clear();
			_ranks->shrink_to_fit();
			_ranks->reserve(capacity);
			_rankOrder->assign(capacity, 0);
			_members->clear();
			_members->shrink_to_fit();
			_members->reserve(_voices.GetLimit());
			_retiring->reserve(2 * _maxRealVoices + 4 * MAX_BUS_ROWS);
		}

		void AudioSystem::ResizeVoicePools() {
			const u32 limit = VoiceLimitOf(GetSettings());
			const u32 real = RealVoicesOf(GetSettings());
			if (!_voices.IsInitialized() || (limit == _voices.GetLimit() && real == _maxRealVoices)) {
				return;
			}
			while (_voices.CountLive() > limit) {
				const u32 victim = FindLeastImportant(false);
				if (victim == NO_VOICE_SLOT) {
					break;
				}
				StopToMakeWay(victim, _voices.At(victim), 0);
				++_budgetCounts.stoppedByPool;
			}

			_renderer.ApplyPendingCommands();
			DrainEvents();

			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				RetireVoiceStream(_voices.At(slot), true);
			}
			_streamer.Stop();
			DestroyRetiredStreams(true);
			_streamer.Start(StreamsFor(real), JobSystem::GetWorkerCount() > 0);
			_retiringStreams->reserve(StreamsFor(real));

			u32 capacity = limit + HeadroomFor(limit, real);
			for (u32 slot = capacity; slot < _voices.GetCapacity(); ++slot) {
				const VoiceClaim claim = _voices.At(slot).claim;
				if (claim == VoiceClaim::Playing || claim == VoiceClaim::Paused) {
					capacity = slot + 1;
				}
			}
			_renderer.Resize(capacity, DecodersFor(real));
			_voices.Resize(limit, capacity - limit);
			for (u32 slot = 0; slot < _voices.GetCapacity(); ++slot) {
				AudioVoiceRecord& record = _voices.At(slot);
				if (record.spatializerState) {
					record.spatializer->DestroyVoiceState(record.spatializerState);
					record.spatializer = nullptr;
					record.spatializerState = nullptr;
					record.placement.spatializer = nullptr;
					record.placement.state = nullptr;
				}
				if (record.effects && record.effects->chain) {
					ReleaseVoiceChain(record.effects->chain);
					record.effects->chain = nullptr;
				}
			}
			_maxRealVoices = real;
			ReserveRankingScratch();
			SLT_LOG_INFO("Audio", "voice limits now {} playing and {} heard (playing voices aren't stopped)", limit, real);
			RankVoices();
		}

		void AudioSystem::WriteReport(AudioSettings& settings) const {
			if (!_device.IsOpen()) {
				settings.obtainedPeriod = "not open";
				return;
			}
			const AudioDeviceFormat& format = _device.GetFormat();
			const bool isNull = format.backend == AudioBackendKind::Null;
			settings.obtainedPeriod = std::format("{:.1f} ms: {} frame(s) at {} Hz x {} period(s), {} channel(s), {}",
												  FramesToMilliseconds(format.periodFrames, format.deviceSampleRate),
												  format.periodFrames, format.deviceSampleRate, format.periods, format.channels,
												  isNull ? str("the null device") : std::format("'{}'", format.name));
		}

	} // namespace AudioImpl

} // namespace Shalltear
