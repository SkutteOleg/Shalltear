#include "AudioRenderer.h"

#include "AudioStream.h"
#include "Realtime.h"

#include "Shalltear/Audio/AudioEffect.h"
#include "Shalltear/Audio/AudioGenerator.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Time.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace Shalltear {

	namespace AudioImpl {

		namespace {

			static_assert(std::atomic<std::thread::id>::is_always_lock_free,
						  "the render thread publishes its id through this atomic (a locked one would wait on the audio thread)");
			static_assert(std::atomic<u64>::is_always_lock_free, "counters must be lock-free on the target");

			constexpr u32 COMMAND_CAPACITY = 4096;

			u32 CommandCapacityFor(u32 voices) {
				const u32 needed = 2 * voices + 1024;
				return needed > COMMAND_CAPACITY ? needed : COMMAND_CAPACITY;
			}

			u32 EventCapacityFor(u32 voices) {
				return voices + AudioRenderer::MAX_BUSES;
			}

			u32 ActiveWordsFor(u32 voices) {
				return (voices + 63) / 64;
			}

			void Bump(std::atomic<u64>& counter, u64 by) [[clang::nonblocking]] {
				counter.fetch_add(by, std::memory_order_relaxed);
			}

			void RaiseTo(std::atomic<u64>& counter, u64 value) [[clang::nonblocking]] {
				if (value > counter.load(std::memory_order_relaxed)) {
					counter.store(value, std::memory_order_relaxed);
				}
			}

			void LowerTo(std::atomic<u64>& counter, u64 value) [[clang::nonblocking]] {
				if (value < counter.load(std::memory_order_relaxed)) {
					counter.store(value, std::memory_order_relaxed);
				}
			}

			f32 Clamp(f32 value, f32 low, f32 high) [[clang::nonblocking]] {
				return value >= low ? (value <= high ? value : high) : low;
			}

			void ApplyRamp(f32* buffer, u32 channels, const AudioRamp& ramp, u32 frames) [[clang::nonblocking]] {
				if (ramp.IsFlat()) {
					if (ramp.end == 1.0f) {
						return;
					}
					const f32 gain = ramp.end;
					for (u32 channel = 0; channel < channels; ++channel) {
						f32* row = buffer + static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES;
						for (u32 frame = 0; frame < frames; ++frame) {
							row[frame] *= gain;
						}
					}
					return;
				}
				for (u32 frame = 0; frame < frames; ++frame) {
					const f32 gain = ramp.At(frame);
					for (u32 channel = 0; channel < channels; ++channel) {
						buffer[static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES + frame] *= gain;
					}
				}
			}

			void AddScaled(f32* to, const f32* from, u32 channels, const AudioRamp& ramp, u32 frames) [[clang::nonblocking]] {
				if (ramp.IsFlat()) {
					const f32 gain = ramp.end;
					if (gain == 0.0f) {
						return;
					}
					for (u32 channel = 0; channel < channels; ++channel) {
						const size_t offset = static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES;
						for (u32 frame = 0; frame < frames; ++frame) {
							to[offset + frame] += from[offset + frame] * gain;
						}
					}
					return;
				}
				for (u32 frame = 0; frame < frames; ++frame) {
					const f32 gain = ramp.At(frame);
					for (u32 channel = 0; channel < channels; ++channel) {
						const size_t index = static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES + frame;
						to[index] += from[index] * gain;
					}
				}
			}

			void AddWalked(f32* to, const f32* from, u32 channels, f32 start, f32 end, u32 frames) [[clang::nonblocking]] {
				const f32 step = (end - start) / static_cast<f32>(frames);
				for (u32 channel = 0; channel < channels; ++channel) {
					const size_t offset = static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES;
					for (u32 frame = 0; frame < frames; ++frame) {
						to[offset + frame] += from[offset + frame] * (start + step * static_cast<f32>(frame + 1));
					}
				}
			}

			void RestartVoiceStream(AudioVoiceState& voice) [[clang::nonblocking]] {
				if (voice.stream) {
					RestartStream(*voice.stream, voice.position > 0.0 ? static_cast<u64>(voice.position) : 0, voice.targetPitch < 0.0f ? -1 : 1,
								  voice.loop);
				}
			}

			void Add(f32* to, const f32* from, u32 channels, u32 frames) [[clang::nonblocking]] {
				for (u32 channel = 0; channel < channels; ++channel) {
					const size_t offset = static_cast<size_t>(channel) * AudioRenderer::BLOCK_FRAMES;
					for (u32 frame = 0; frame < frames; ++frame) {
						to[offset + frame] += from[offset + frame];
					}
				}
			}

		} // anonymous namespace

		AudioRenderer::~AudioRenderer() {
			Release();
			Shutdown();
		}

		void AudioRenderer::AllocateVoicePools(u32 voices, u32 decoders) {
			voices = voices > 0 ? voices : 1;
			_voiceCapacity = voices;
			_voices = static_cast<AudioVoiceState*>(Allocate(sizeof(AudioVoiceState) * voices, MemTag::Audio, 64));
			_positions = static_cast<std::atomic<u64>*>(Allocate(sizeof(std::atomic<u64>) * voices, MemTag::Audio, 64));
			_placements = static_cast<AudioMailbox<AudioVoicePlacement>*>(
				Allocate(sizeof(AudioMailbox<AudioVoicePlacement>) * voices, MemTag::Audio, 64));
			for (u32 slot = 0; slot < voices; ++slot) {
				new (&_voices[slot]) AudioVoiceState();
				new (&_positions[slot]) std::atomic<u64>(0);
				new (&_placements[slot]) AudioMailbox<AudioVoicePlacement>();
			}
			_activeWords = ActiveWordsFor(voices);
			_active = static_cast<u64*>(Allocate(sizeof(u64) * _activeWords, MemTag::Audio, 64));
			std::memset(_active, 0, sizeof(u64) * _activeWords);

			decoders = decoders > 0 ? decoders : 1;
			_decoderCapacity = decoders;
			_decoders = static_cast<AudioVoiceDecoder*>(Allocate(sizeof(AudioVoiceDecoder) * decoders, MemTag::Audio, 64));
			_freeDecoders = static_cast<u32*>(Allocate(sizeof(u32) * decoders, MemTag::Audio, 64));
			for (u32 index = 0; index < decoders; ++index) {
				new (&_decoders[index]) AudioVoiceDecoder();
				_freeDecoders[index] = decoders - 1 - index;
			}
			_freeDecoderCount = decoders;
		}

		void AudioRenderer::FreeVoicePools() {
			if (!_voices) {
				return;
			}
			for (u32 slot = 0; slot < _voiceCapacity; ++slot) {
				_voices[slot].~AudioVoiceState();
				_positions[slot].~atomic();
				_placements[slot].~AudioMailbox();
			}
			for (u32 index = 0; index < _decoderCapacity; ++index) {
				_decoders[index].~AudioVoiceDecoder();
			}
			Free(_voices);
			Free(_positions);
			Free(_placements);
			Free(_active);
			Free(_decoders);
			Free(_freeDecoders);
			_voices = nullptr;
			_positions = nullptr;
			_placements = nullptr;
			_active = nullptr;
			_decoders = nullptr;
			_freeDecoders = nullptr;
			_voiceCapacity = 0;
			_activeWords = 0;
			_decoderCapacity = 0;
			_freeDecoderCount = 0;
		}

		void AudioRenderer::Initialize(u32 voices, u32 decoders) {
			if (_voices) {
				return;
			}
			AllocateVoicePools(voices, decoders);
			const size_t scratchBytes = sizeof(f32) * MAX_AUDIO_CHANNELS * BLOCK_FRAMES;
			_dry = static_cast<f32*>(Allocate(scratchBytes, MemTag::Audio, 64));
			_wet = static_cast<f32*>(Allocate(scratchBytes, MemTag::Audio, 64));
			_post = static_cast<f32*>(Allocate(scratchBytes, MemTag::Audio, 64));
			_gather = static_cast<f32*>(Allocate(scratchBytes, MemTag::Audio, 64));
			std::memset(_dry, 0, scratchBytes);
			std::memset(_wet, 0, scratchBytes);
			std::memset(_post, 0, scratchBytes);
			std::memset(_gather, 0, scratchBytes);
			_buses = static_cast<AudioBusState*>(Allocate(sizeof(AudioBusState) * MAX_BUSES, MemTag::Audio, 64));
			for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
				new (&_buses[slot]) AudioBusState();
			}
			AudioBusState& root = _buses[ROOT_BUS];
			root.live = true;
			root.generation = 1;
			root.parent = ROOT_BUS;
			_order[0] = ROOT_BUS;
			_orderCount = 1;
			_graphDirty = false;
			_anySolo = false;

			_commands.Reserve(CommandCapacityFor(_voiceCapacity));
			_events.Reserve(EventCapacityFor(_voiceCapacity));
			_mixerFrames = 0;
			_stoppedFrames = 0;
			_listenerGain = 0.0f;
			_listenerTarget = 0.0f;
			_globalVolume.Jump(1.0f);
			_gameplayRunning = true;
			_timeScale.Jump(1.0f);
			_timeScaleTarget = 1.0f;
			_clockSequence.store(0, std::memory_order_relaxed);
			_clockFrames.store(0, std::memory_order_relaxed);
			_clockTime.store(0, std::memory_order_relaxed);
			_clockTicks.store(0, std::memory_order_relaxed);
			_clockRunning.store(false, std::memory_order_relaxed);
		}

		void AudioRenderer::Shutdown() {
			if (!_voices) {
				return;
			}
			FreeVoicePools();
			for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
				_buses[slot].~AudioBusState();
			}
			Free(_buses);
			Free(_dry);
			Free(_wet);
			Free(_post);
			Free(_gather);
			_buses = nullptr;
			_dry = nullptr;
			_wet = nullptr;
			_post = nullptr;
			_gather = nullptr;
			_orderCount = 0;
			_commands.Release();
			_events.Release();
			_metered = nullptr;
			if (_meters) {
				for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
					_meters[slot].~AudioBusMeter();
				}
				Free(_meters);
				_meters = nullptr;
			}
		}

		bool AudioRenderer::Prepare(const ChannelLayout& layout, u32 sampleRate, u32 primingFrames) {
			Release();
			if (layout.count == 0 || layout.count > MAX_AUDIO_CHANNELS || sampleRate == 0) {
				return false;
			}

			const size_t bytes = static_cast<size_t>(layout.count) * BLOCK_FRAMES * sizeof(f32);
			_block = static_cast<f32*>(Allocate(bytes, MemTag::Audio, 64));
			std::memset(_block, 0, bytes);
			if (_buses) {
				_buses[ROOT_BUS].buffer = _block;
			}

			_layout = layout;
			_sampleRate = sampleRate;
			_cursor = BLOCK_FRAMES;
			_ticksPerSecond = Time::GetTicksPerSecond();
			_lastCallbackTicks = 0;
			_primingFrames = primingFrames;
			_framesDelivered = 0;

			const f64 blockSeconds = static_cast<f64>(BLOCK_FRAMES) / static_cast<f64>(sampleRate);
			_meterAverage = static_cast<f32>(1.0 - std::exp(-blockSeconds / 0.3));
			_meterRelease = static_cast<f32>(std::pow(10.0, -(20.0 / 1.7) * blockSeconds / 20.0));

			if (_voices) {
				for (u32 slot = 0; slot < _voiceCapacity; ++slot) {
					AudioVoiceState& voice = _voices[slot];
					if (voice.phase != VoicePhase::Free && voice.clip) {
						voice.rateRatio = static_cast<f64>(voice.clip->sampleRate) / static_cast<f64>(sampleRate);
					}
				}
				for (u32 index = 0; index < _decoderCapacity; ++index) {
					_decoders[index].gainsValid = false;
				}
			}

			_counters.callbacks.store(0, std::memory_order_relaxed);
			_counters.frames.store(0, std::memory_order_relaxed);
			_counters.blocks.store(0, std::memory_order_relaxed);
			_counters.costTicks.store(0, std::memory_order_relaxed);
			_counters.worstCostTicks.store(0, std::memory_order_relaxed);
			_counters.shortestIntervalTicks.store(~0ull, std::memory_order_relaxed);
			_counters.longestIntervalTicks.store(0, std::memory_order_relaxed);
			_counters.overruns.store(0, std::memory_order_relaxed);
			_counters.streamUnderruns.store(0, std::memory_order_relaxed);
			_counters.lastBlockPeak.store(0, std::memory_order_relaxed);
			_counters.peak.store(0, std::memory_order_relaxed);
			_counters.renderThread.store(std::thread::id{}, std::memory_order_relaxed);
			return true;
		}

		void AudioRenderer::Release() {
			if (_block) {
				Free(_block);
				_block = nullptr;
			}
			if (_buses) {
				_buses[ROOT_BUS].buffer = nullptr;
			}
			_layout = {};
			_sampleRate = 0;
			_cursor = BLOCK_FRAMES;
		}

		bool AudioRenderer::ReadClock(AudioClockStamp& stamp) const {
			for (;;) {
				const u32 before = _clockSequence.load(std::memory_order_acquire);
				if (before & 1u) {
					continue;
				}
				stamp.frames = _clockFrames.load(std::memory_order_relaxed);
				stamp.time = _clockTime.load(std::memory_order_relaxed);
				stamp.ticks = _clockTicks.load(std::memory_order_relaxed);
				stamp.running = _clockRunning.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if (_clockSequence.load(std::memory_order_relaxed) == before) {
					return before != 0;
				}
			}
		}

		u64 AudioRenderer::GetVoicePosition(u32 slot) const {
			return _positions && slot < _voiceCapacity ? _positions[slot].load(std::memory_order_relaxed) : 0;
		}

		void AudioRenderer::ApplyPendingCommands() {
			if (!_voices) {
				return;
			}
			AudioCommand command;
			while (_commands.TryPop(command)) {
				Apply(command, _mixerFrames);
			}
		}

		void AudioRenderer::Resize(u32 voices, u32 decoders) {
			if (!_voices) {
				Initialize(voices, decoders);
				return;
			}
			const u32 oldCapacity = _voiceCapacity;
			AudioVoiceState* oldVoices = _voices;
			std::atomic<u64>* oldPositions = _positions;
			const u32 kept = oldCapacity < voices ? oldCapacity : voices;
			AudioVoiceState* saved = static_cast<AudioVoiceState*>(Allocate(sizeof(AudioVoiceState) * (kept > 0 ? kept : 1), MemTag::Audio, 64));
			u64* savedPositions = static_cast<u64*>(Allocate(sizeof(u64) * (kept > 0 ? kept : 1), MemTag::Audio, 64));
			for (u32 slot = 0; slot < kept; ++slot) {
				new (&saved[slot]) AudioVoiceState(oldVoices[slot]);
				savedPositions[slot] = oldPositions[slot].load(std::memory_order_relaxed);
			}

			FreeVoicePools();
			AllocateVoicePools(voices, decoders);
			_commands.Reserve(CommandCapacityFor(_voiceCapacity));
			_events.Reserve(EventCapacityFor(_voiceCapacity));

			for (u32 slot = 0; slot < kept; ++slot) {
				AudioVoiceState& voice = _voices[slot];
				voice = saved[slot];
				_positions[slot].store(savedPositions[slot], std::memory_order_relaxed);
				voice.decoder = NO_DECODER;
				voice.wantsReal = false;
				LetGoOfVirtualState(voice);
				voice.chain = nullptr;
				if (voice.phase == VoicePhase::Free) {
					continue;
				}
				_active[slot / 64] |= 1ull << (slot % 64);
				if (voice.phase != VoicePhase::Scheduled) {
					voice.volume = 0.0f;
				}
				if (voice.phase == VoicePhase::Stopping) {
					EndVoice(slot);
				}
			}
			for (u32 slot = 0; slot < kept; ++slot) {
				saved[slot].~AudioVoiceState();
			}
			Free(saved);
			Free(savedPositions);
		}

		void AudioRenderer::Render(f32* interleaved, u32 frameCount) {
			const u64 startTicks = Realtime::GetTicks();
			const u32 channels = _layout.count;
			const Realtime::DenormalsFlushed flushed;

			u32 written = 0;
			while (written < frameCount) {
				if (_cursor == BLOCK_FRAMES) {
					RenderBlock();
					_cursor = 0;
					Bump(_counters.blocks, 1);
				}

				const u32 remaining = frameCount - written;
				const u32 available = BLOCK_FRAMES - _cursor;
				const u32 run = remaining < available ? remaining : available;

				f32* out = interleaved + static_cast<size_t>(written) * channels;
				const f32* source = _block + _cursor;
				for (u32 frame = 0; frame < run; ++frame) {
					for (u32 channel = 0; channel < channels; ++channel) {
						out[static_cast<size_t>(frame) * channels + channel] = source[static_cast<size_t>(channel) * BLOCK_FRAMES + frame];
					}
				}

				_cursor += run;
				written += run;
			}

			NoteCallback(startTicks, Realtime::GetTicks(), frameCount);
		}

		f32* AudioRenderer::Touch(AudioBusState& bus) {
			if (!bus.touched) {
				bus.touched = true;
				std::memset(bus.buffer, 0, static_cast<size_t>(_layout.count) * BLOCK_FRAMES * sizeof(f32));
			}
			return bus.buffer;
		}

		void AudioRenderer::RenderBlock() {
			const u64 blockStart = _mixerFrames;
			const u32 channels = _layout.count;

			if (_voices) {
				AudioCommand command;
				// bounded (producer refilling ring would stall block forever)
				for (u32 queued = _commands.CountApproximate(); queued > 0 && _commands.TryPop(command); --queued) {
					Apply(command, blockStart);
				}
				if (_graphDirty) {
					RebuildOrder();
				}
				DeriveBusStates();
			}

			AudioBusState* root = _buses ? &_buses[ROOT_BUS] : nullptr;
			if (root) {
				root->touched = false;
				Touch(*root);
			} else {
				std::memset(_block, 0, static_cast<size_t>(channels) * BLOCK_FRAMES * sizeof(f32));
			}

			u32 active = 0;
			u32 real = 0;
			if (_voices) {
				const AudioMixScratch scratch{_dry, _wet, BLOCK_FRAMES, _post};
				AudioVoicePlacement placement;
				for (u32 word = 0; word < _activeWords; ++word) {
					u64 bits = _active[word];
					while (bits != 0) {
						const u32 slot = word * 64 + static_cast<u32>(std::countr_zero(bits));
						bits &= bits - 1;
						AudioVoiceState& voice = _voices[slot];
						++active;
						if (_placements[slot].Take(placement) && placement.generation == voice.generation) {
							PlaceVoice(voice, placement, false);
						}
						AudioBusState& bus = _buses[voice.bus < MAX_BUSES && _buses[voice.bus].live ? voice.bus : ROOT_BUS];
						AudioVoiceClock clock;
						clock.hold = bus.hold;
						clock.rateStart = bus.rateStart;
						clock.rateEnd = bus.rateEnd;

						AudioVoiceDecoder* decoder = voice.decoder != NO_DECODER ? &_decoders[voice.decoder] : nullptr;
						bool fadeOut = false;
						if (voice.wantsReal && !decoder) {
							decoder = AcquireDecoder(voice);
							if (!decoder) {
								Bump(_counters.decoderShortfalls, 1);
							}
						} else if (!voice.wantsReal && decoder && voice.phase != VoicePhase::Stopping) {
							if (voice.phase == VoicePhase::Playing && !voice.paused && !voice.held) {
								fadeOut = true;
							} else {
								ReleaseDecoder(voice);
								decoder = nullptr;
								LetGoOfVirtualState(voice);
							}
						} else if (!voice.wantsReal && !decoder) {
							LetGoOfVirtualState(voice);
						}
						real += decoder ? 1u : 0u;

						AudioBusState* send = nullptr;
						if (decoder && voice.sendGeneration != 0 && voice.send < MAX_BUSES && _buses[voice.send].live &&
							_buses[voice.send].generation == voice.sendGeneration && (voice.sendLevel > 0.0f || voice.targetSendLevel > 0.0f)) {
							send = &_buses[voice.send];
						}
						f32* into = nullptr;
						if (send) {
							std::memset(_gather, 0, static_cast<size_t>(channels) * BLOCK_FRAMES * sizeof(f32));
							into = _gather;
						} else if (decoder) {
							into = Touch(bus);
						}
						AudioStream* const stream = decoder ? voice.stream : nullptr;
						if (stream) {
							stream->starved = false;
							stream->underrun = false;
						}
						const bool ended = MixVoice(voice, decoder, fadeOut, _layout, blockStart, BLOCK_FRAMES, into, BLOCK_FRAMES, clock, scratch);
						if (stream && stream->underrun) {
							Bump(_counters.streamUnderruns, 1);
						}
						if (send) {
							Add(Touch(bus), _gather, channels, BLOCK_FRAMES);
							AddWalked(Touch(*send), _gather, channels, voice.sendLevel, voice.targetSendLevel, BLOCK_FRAMES);
						}
						voice.sendLevel = voice.targetSendLevel;
						_positions[slot].store(voice.position > 0.0 ? static_cast<u64>(voice.position) : 0, std::memory_order_relaxed);
						if (ended) {
							EndVoice(slot);
							continue;
						}
						if (fadeOut) {
							ReleaseDecoder(voice);
							LetGoOfVirtualState(voice);
						}
						if (voice.decoder == NO_DECODER && voice.phase != VoicePhase::Scheduled) {
							voice.volume = 0.0f;
						}
						if (stream && stream->starved) {
							voice.volume = 0.0f;
						}
					}
				}
			}

			if (_buses) {
				for (u32 index = 0; index < _orderCount; ++index) {
					AudioBusState& bus = _buses[_order[index]];
					if (bus.touched) {
						ApplyRamp(bus.buffer, channels, bus.soloGain, BLOCK_FRAMES);
					}
				}

				for (u32 index = 0; index < _orderCount; ++index) {
					const u32 slot = _order[index];
					if (slot == ROOT_BUS) {
						continue;
					}
					AudioBusState& bus = _buses[slot];
					if (!bus.touched) {
						if (bus.receiveTouched == 0 && IsChainQuiet(bus)) {
							if (_metered) {
								Meter(_metered[slot].output, nullptr);
								Meter(_metered[slot].fader, nullptr);
							}
							continue;
						}
						Touch(bus);
					}
					ApplyRamp(bus.buffer, channels, bus.mute, BLOCK_FRAMES);
					ProcessChain(bus, _metered ? &_metered[slot] : nullptr);
					if (_metered) {
						Meter(_metered[slot].output, bus.buffer);
					}
					if (bus.clock != AudioBusClock::Device) {
						Add(Touch(_buses[bus.parent]), bus.buffer, channels, BLOCK_FRAMES);
					}
				}
			}

			if (_listenerGain != _listenerTarget) {
				const f32 step = (_listenerTarget - _listenerGain) / static_cast<f32>(BLOCK_FRAMES);
				for (u32 frame = 0; frame < BLOCK_FRAMES; ++frame) {
					const f32 gain = _listenerGain + step * static_cast<f32>(frame + 1);
					for (u32 channel = 0; channel < channels; ++channel) {
						_block[channel * BLOCK_FRAMES + frame] *= gain;
					}
				}
				_listenerGain = _listenerTarget;
			} else if (_listenerGain == 0.0f) {
				std::memset(_block, 0, static_cast<size_t>(channels) * BLOCK_FRAMES * sizeof(f32));
			}

			_globalVolume.Advance(BLOCK_FRAMES);
			ApplyRamp(_block, channels, _globalVolume, BLOCK_FRAMES);

			if (_buses) {
				for (u32 index = 0; index < _orderCount; ++index) {
					const AudioBusState& bus = _buses[_order[index]];
					if (bus.clock == AudioBusClock::Device && bus.touched) {
						Add(_block, bus.buffer, channels, BLOCK_FRAMES);
					}
				}
			}

			f32 loudest = 0.0f;
			const size_t samples = static_cast<size_t>(channels) * BLOCK_FRAMES;
			for (size_t index = 0; index < samples; ++index) {
				const f32 magnitude = _block[index] < 0.0f ? -_block[index] : _block[index];
				loudest = magnitude > loudest ? magnitude : loudest;
			}
			const u32 loudestBits = std::bit_cast<u32>(loudest);
			_counters.lastBlockPeak.store(loudestBits, std::memory_order_relaxed);
			if (loudestBits > _counters.peak.load(std::memory_order_relaxed)) {
				_counters.peak.store(loudestBits, std::memory_order_relaxed);
			}

			_mixerFrames += BLOCK_FRAMES;
			if (!_gameplayRunning) {
				_stoppedFrames += BLOCK_FRAMES;
			}
			_counters.voices.store(active, std::memory_order_relaxed);
			_counters.realVoices.store(real, std::memory_order_relaxed);
			PublishClock(Realtime::GetTicks());
		}

		u32 AudioRenderer::LiveOrRoot(u32 slot, u32 generation) const {
			if (slot < MAX_BUSES && _buses[slot].live && _buses[slot].generation == generation) {
				return slot;
			}
			return ROOT_BUS;
		}

		void AudioRenderer::RebuildOrder() {
			_graphDirty = false;
			for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
				_pending[slot] = 0;
			}
			for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
				const AudioBusState& bus = _buses[slot];
				if (!bus.live || slot == ROOT_BUS) {
					continue;
				}
				++_pending[bus.parent];
				for (u32 row = 0; row < bus.rowCount; ++row) {
					if (bus.rows[row].kind == AudioBusRowKind::Send && bus.rows[row].target != NO_BUS_TARGET) {
						++_pending[bus.rows[row].target];
					}
				}
			}
			u32 tail = 0;
			for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
				if (_buses[slot].live && _pending[slot] == 0) {
					_order[tail++] = slot;
				}
			}
			for (u32 head = 0; head < tail; ++head) {
				const u32 slot = _order[head];
				if (slot == ROOT_BUS) {
					continue;
				}
				const AudioBusState& bus = _buses[slot];
				if (--_pending[bus.parent] == 0) {
					_order[tail++] = bus.parent;
				}
				for (u32 row = 0; row < bus.rowCount; ++row) {
					const u32 target = bus.rows[row].target;
					if (bus.rows[row].kind != AudioBusRowKind::Send || target == NO_BUS_TARGET) {
						continue;
					}
					if (--_pending[target] == 0) {
						_order[tail++] = target;
					}
				}
			}
			_orderCount = tail;

			_anySolo = false;
			for (u32 index = 0; index < _orderCount; ++index) {
				_anySolo = _anySolo || _buses[_order[index]].solo;
			}
		}

		void AudioRenderer::DeriveBusStates() {
			_timeScale.Advance(BLOCK_FRAMES);
			const bool scaleStopped = _timeScaleTarget == 0.0f;

			for (u32 index = _orderCount; index-- > 0;) {
				const u32 slot = _order[index];
				AudioBusState& bus = _buses[slot];
				bus.touched = false;
				bus.receiveTouched = 0;

				if (bus.clock == AudioBusClock::Device) {
					bus.hold = false;
					bus.follows = false;
				} else if (bus.clock == AudioBusClock::Inherit && slot != ROOT_BUS) {
					bus.hold = _buses[bus.parent].hold;
					bus.follows = _buses[bus.parent].follows;
				} else {
					bus.follows = bus.followTimeScale;
					bus.hold = !_gameplayRunning || (scaleStopped && (bus.clock == AudioBusClock::Gameplay || bus.follows));
				}

				bus.fader.Advance(BLOCK_FRAMES);
				bus.mute.Advance(BLOCK_FRAMES);
				bus.pitch.Advance(BLOCK_FRAMES);
				for (u32 row = 0; row < bus.rowCount; ++row) {
					bus.rows[row].level.Advance(BLOCK_FRAMES);
				}

				if (slot == ROOT_BUS) {
					bus.pitchStart = 1.0f;
					bus.pitchEnd = 1.0f;
					bus.rateStart = 1.0f;
					bus.rateEnd = 1.0f;
					bus.inSoloPath = false;
				} else {
					const AudioBusState& parent = _buses[bus.parent];
					bus.pitchStart = Clamp(parent.pitchStart * bus.pitch.start, 0.0f, MAX_BUS_RATE);
					bus.pitchEnd = Clamp(parent.pitchEnd * bus.pitch.end, 0.0f, MAX_BUS_RATE);
					f32 start = bus.pitchStart;
					f32 end = bus.pitchEnd;
					if (bus.follows) {
						start *= _timeScale.start;
						end *= _timeScale.end;
					}
					bus.rateStart = Clamp(start, 0.0f, MAX_BUS_RATE);
					bus.rateEnd = Clamp(end, 0.0f, MAX_BUS_RATE);
					bus.inSoloPath = bus.solo || parent.inSoloPath;
				}

				const f32 solo = (!_anySolo || bus.inSoloPath || bus.clock == AudioBusClock::Device) ? 1.0f : 0.0f;
				if (solo != bus.soloGain.target) {
					bus.soloGain.Walk(solo, BLOCK_FRAMES);
				}
				bus.soloGain.Advance(BLOCK_FRAMES);
			}
		}

		void AudioRenderer::DestroyBus(u32 slot) {
			AudioBusState& gone = _buses[slot];
			const u32 parent = gone.parent;
			for (u32 other = 0; other < MAX_BUSES; ++other) {
				AudioBusState& bus = _buses[other];
				if (!bus.live || other == slot) {
					continue;
				}
				if (bus.parent == slot) {
					bus.parent = parent;
				}
				for (u32 row = 0; row < bus.rowCount; ++row) {
					if (bus.rows[row].kind == AudioBusRowKind::Send && bus.rows[row].target == slot) {
						bus.rows[row].target = NO_BUS_TARGET;
					}
				}
			}
			for (u32 voice = 0; voice < _voiceCapacity; ++voice) {
				if (_voices[voice].phase != VoicePhase::Free && _voices[voice].bus == slot) {
					_voices[voice].bus = parent;
				}
			}

			AudioEvent event;
			event.kind = AudioEventKind::BusReleased;
			event.slot = slot;
			event.generation = gone.generation;
			gone.live = false;
			gone.buffer = nullptr;
			gone.rows = nullptr;
			gone.rowCount = 0;
			gone.receives = nullptr;
			gone.receiveCapacity = 0;
			_graphDirty = true;
			if (!_events.TryPush(event)) {
				Bump(_counters.droppedEvents, 1);
			}
		}

		void AudioRenderer::ApplyBus(const AudioCommand& command) {
			if (command.kind == AudioCommandKind::CreateBus) {
				const u32 slot = command.bus;
				if (slot == ROOT_BUS || slot >= MAX_BUSES || _buses[slot].live || !command.block) {
					return;
				}
				AudioBusState& bus = _buses[slot];
				bus = AudioBusState{};
				bus.live = true;
				bus.generation = command.busGeneration;
				bus.parent = LiveOrRoot(command.other, command.otherGeneration);
				bus.buffer = BusSamplesOf(command.block);
				bus.rows = BusRowsOf(command.block);
				bus.rows[0] = AudioBusRow{};
				bus.rowCount = 1;
				bus.clock = static_cast<AudioBusClock>(command.mode);
				bus.followTimeScale = command.flag;
				bus.fader.Jump(command.volume);
				bus.pitch.Jump(command.pitch);
				bus.mute.Jump(1.0f);
				bus.soloGain.Jump(1.0f);
				ClearMeter(slot);
				_graphDirty = true;
				return;
			}

			if (command.bus >= MAX_BUSES || command.bus == ROOT_BUS) {
				return;
			}
			AudioBusState& bus = _buses[command.bus];
			if (!bus.live || bus.generation != command.busGeneration) {
				return;
			}
			switch (command.kind) {
			case AudioCommandKind::DestroyBus:
				DestroyBus(command.bus);
				break;
			case AudioCommandKind::SetBusParent:
				bus.parent = LiveOrRoot(command.other, command.otherGeneration);
				_graphDirty = true;
				break;
			case AudioCommandKind::SetBusFader:
				bus.fader.Walk(command.value, command.frames);
				break;
			case AudioCommandKind::SetBusPitch:
				bus.pitch.Walk(command.value, command.frames);
				break;
			case AudioCommandKind::SetBusMute:
				bus.mute.Walk(command.flag ? 0.0f : 1.0f, BLOCK_FRAMES);
				break;
			case AudioCommandKind::SetBusSolo:
				bus.solo = command.flag;
				_graphDirty = true;
				break;
			case AudioCommandKind::SetBusClock:
				bus.clock = static_cast<AudioBusClock>(command.mode);
				bus.followTimeScale = command.flag;
				break;
			case AudioCommandKind::SetBusRow: {
				if (command.mode >= MAX_BUS_ROWS || command.mode > bus.rowCount) {
					break;
				}
				AudioBusRow row;
				row.kind = static_cast<AudioBusRowKind>(command.frames);
				const u8 index = command.frame < NO_BUS_ROW ? static_cast<u8>(command.frame) : NO_BUS_ROW;
				if (row.kind == AudioBusRowKind::Send) {
					const u32 target = LiveOrRoot(command.other, command.otherGeneration);
					row.target = target == ROOT_BUS || target == command.bus ? NO_BUS_TARGET : target;
					row.targetRow = index;
					row.level.Jump(command.value);
				} else if (row.kind == AudioBusRowKind::Effect) {
					row.effect = command.effect;
					row.bypass = command.flag;
					row.wetMixing = command.loop;
					row.receive = index;
					row.level.Jump(command.value);
				} else if (row.kind == AudioBusRowKind::Receive) {
					row.receive = index;
				}
				bus.rows[command.mode] = row;
				if (command.mode == bus.rowCount) {
					++bus.rowCount;
				}
				_graphDirty = true;
				break;
			}
			case AudioCommandKind::SetBusRowCount:
				if (command.mode < bus.rowCount) {
					bus.rowCount = command.mode;
					_graphDirty = true;
				}
				break;
			case AudioCommandKind::SetBusRowLevel:
				if (command.mode < bus.rowCount) {
					bus.rows[command.mode].level.Walk(command.value, command.frames);
				}
				break;
			case AudioCommandKind::SetBusRowBypass:
				if (command.mode < bus.rowCount) {
					bus.rows[command.mode].bypass = command.flag;
				}
				break;
			case AudioCommandKind::SetBusRowEffect:
				if (command.mode < bus.rowCount && bus.rows[command.mode].kind == AudioBusRowKind::Effect) {
					bus.rows[command.mode].effect = command.effect;
				}
				break;
			case AudioCommandKind::SetBusBypassEffects:
				bus.bypassEffects = command.flag;
				break;
			case AudioCommandKind::SetBusEffectParameter:
				if (command.mode < bus.rowCount && bus.rows[command.mode].kind == AudioBusRowKind::Effect && bus.rows[command.mode].effect) {
					bus.rows[command.mode].effect->SetParameter(command.frames, command.value);
				}
				break;
			case AudioCommandKind::SetBusReceives:
				bus.receives = static_cast<f32*>(command.block);
				bus.receiveCapacity = command.block && command.mode <= MAX_BUS_ROWS ? command.mode : 0;
				break;
			default:
				break;
			}
		}

		void AudioRenderer::ProcessChain(AudioBusState& bus, AudioBusMeter* meter) {
			const u32 channels = _layout.count;
			for (u32 index = 0; index < bus.rowCount; ++index) {
				AudioBusRow& row = bus.rows[index];
				switch (row.kind) {
				case AudioBusRowKind::Fader:
					ApplyRamp(bus.buffer, channels, bus.fader, BLOCK_FRAMES);
					if (meter) {
						Meter(meter->fader, bus.buffer);
					}
					break;
				case AudioBusRowKind::Send: {
					if (row.target == NO_BUS_TARGET) {
						break;
					}
					AudioBusState& target = _buses[row.target];
					f32* into = row.targetRow == NO_BUS_ROW ? Touch(target) : TouchReceive(target, row.targetRow);
					if (into) {
						AddScaled(into, bus.buffer, channels, row.level, BLOCK_FRAMES);
					}
					break;
				}
				case AudioBusRowKind::Receive:
					if (row.receive < bus.receiveCapacity && (bus.receiveTouched & (1u << row.receive)) != 0) {
						AddScaled(bus.buffer, bus.receives + static_cast<size_t>(row.receive) * BUS_BUFFER_SAMPLES, channels, bus.mute, BLOCK_FRAMES);
					}
					break;
				case AudioBusRowKind::Effect:
					if (row.effect && !row.bypass && !bus.bypassEffects) {
						RunEffect(row, bus.buffer, row.receive != NO_BUS_ROW ? TouchReceive(bus, index) : nullptr);
					}
					break;
				}
			}
		}

		f32* AudioRenderer::TouchReceive(AudioBusState& bus, u32 row) {
			if (row >= bus.rowCount) {
				return nullptr;
			}
			const u32 receive = bus.rows[row].receive;
			if (receive >= bus.receiveCapacity) {
				return nullptr;
			}
			f32* buffer = bus.receives + static_cast<size_t>(receive) * BUS_BUFFER_SAMPLES;
			const u32 bit = 1u << receive;
			if ((bus.receiveTouched & bit) == 0) {
				bus.receiveTouched |= bit;
				std::memset(buffer, 0, static_cast<size_t>(_layout.count) * BLOCK_FRAMES * sizeof(f32));
			}
			return buffer;
		}

		void AudioRenderer::RunEffect(AudioBusRow& row, f32* buffer, const f32* sidechain) {
			const u32 channels = _layout.count;
			AudioBlock block{buffer, channels, BLOCK_FRAMES, BLOCK_FRAMES};
			const AudioBlock side{const_cast<f32*>(sidechain), channels, BLOCK_FRAMES, BLOCK_FRAMES};
			const AudioBlock* handed = sidechain ? &side : nullptr;
			if (!row.wetMixing) {
				row.effect->Process(block, handed);
				return;
			}
			const size_t samples = static_cast<size_t>(channels) * BLOCK_FRAMES;
			std::memcpy(_dry, buffer, samples * sizeof(f32));
			row.effect->Process(block, handed);
			if (row.level.IsFlat()) {
				const f32 wet = row.level.end;
				for (size_t index = 0; index < samples; ++index) {
					buffer[index] = _dry[index] + (buffer[index] - _dry[index]) * wet;
				}
				return;
			}
			for (u32 frame = 0; frame < BLOCK_FRAMES; ++frame) {
				const f32 wet = row.level.At(frame);
				for (u32 channel = 0; channel < channels; ++channel) {
					const size_t index = static_cast<size_t>(channel) * BLOCK_FRAMES + frame;
					buffer[index] = _dry[index] + (buffer[index] - _dry[index]) * wet;
				}
			}
		}

		bool AudioRenderer::IsChainQuiet(const AudioBusState& bus) const {
			if (bus.bypassEffects) {
				return true;
			}
			for (u32 index = 0; index < bus.rowCount; ++index) {
				const AudioBusRow& row = bus.rows[index];
				if (row.kind == AudioBusRowKind::Effect && row.effect && !row.bypass && !row.effect->IsQuiet()) {
					return false;
				}
			}
			return true;
		}

		void AudioRenderer::Apply(const AudioCommand& command, u64 blockStart) {
			switch (command.kind) {
			case AudioCommandKind::SetListenerPresent:
				_listenerTarget = command.value > 0.0f ? 1.0f : 0.0f;
				return;
			case AudioCommandKind::SetGameClock:
				_gameplayRunning = command.flag;
				_timeScaleTarget = command.value > 0.0f ? command.value : 0.0f;
				_timeScale.Walk(_timeScaleTarget, BLOCK_FRAMES);
				return;
			case AudioCommandKind::SetGlobalVolume:
				_globalVolume.Walk(Clamp(command.value, 0.0f, 1.0f), BLOCK_FRAMES);
				return;
			case AudioCommandKind::SetMeters:
				_metered = static_cast<AudioBusMeter*>(command.block);
				if (_metered) {
					for (u32 slot = 0; slot < MAX_BUSES; ++slot) {
						ClearMeter(slot);
					}
				}
				return;
			case AudioCommandKind::CreateBus:
			case AudioCommandKind::DestroyBus:
			case AudioCommandKind::SetBusParent:
			case AudioCommandKind::SetBusFader:
			case AudioCommandKind::SetBusPitch:
			case AudioCommandKind::SetBusMute:
			case AudioCommandKind::SetBusSolo:
			case AudioCommandKind::SetBusClock:
			case AudioCommandKind::SetBusRow:
			case AudioCommandKind::SetBusRowCount:
			case AudioCommandKind::SetBusRowLevel:
			case AudioCommandKind::SetBusRowBypass:
			case AudioCommandKind::SetBusRowEffect:
			case AudioCommandKind::SetBusBypassEffects:
			case AudioCommandKind::SetBusEffectParameter:
			case AudioCommandKind::SetBusReceives:
				ApplyBus(command);
				return;
			default:
				break;
			}

			if (command.voice >= _voiceCapacity) {
				return;
			}
			AudioVoiceState& voice = _voices[command.voice];

			if (command.kind == AudioCommandKind::Play) {
				if (!command.clip) {
					return;
				}
				const u32 bus = LiveOrRoot(command.bus, command.busGeneration);
				u64 startAt = MixerFrameOf(command.at, bus);
				if (startAt < blockStart) {
					if (command.at != 0) {
						Bump(_counters.lateStarts, 1);
					}
					startAt = blockStart;
				}
				StartVoice(voice, command.clip, command.generation, _sampleRate, startAt, command.frame, command.volume, command.pitch,
						   command.stereoPan, command.loop, bus);
				voice.wantsReal = !command.flag;
				voice.decoder = NO_DECODER;
				_active[command.voice / 64] |= 1ull << (command.voice % 64);
				voice.send = command.other;
				voice.sendGeneration = command.otherGeneration;
				voice.sendLevel = voice.targetSendLevel = Clamp(command.value, 0.0f, MAX_VOICE_GAIN);
				AudioVoicePlacement placement;
				if (_placements[command.voice].Take(placement) && placement.generation == command.generation) {
					PlaceVoice(voice, placement, true);
				}
				return;
			}

			if (voice.phase == VoicePhase::Free || voice.generation != command.generation) {
				return;
			}
			switch (command.kind) {
			case AudioCommandKind::Stop:
				if (command.at == 0) {
					if (voice.phase == VoicePhase::Scheduled || voice.paused || voice.held || voice.decoder == NO_DECODER) {
						EndVoice(command.voice);
					} else {
						voice.phase = VoicePhase::Stopping;
						voice.stopFade = command.frames;
					}
				} else if (voice.phase == VoicePhase::Scheduled && MixerFrameOf(command.at, voice.bus) <= voice.startAt) {
					EndVoice(command.voice);
				} else {
					voice.stopAt = MixerFrameOf(command.at, voice.bus);
				}
				break;
			case AudioCommandKind::Pause:
				if (voice.phase == VoicePhase::Scheduled || voice.held) {
					voice.paused = true;
				} else if (voice.phase == VoicePhase::Playing && !voice.paused) {
					voice.pausing = true;
				}
				break;
			case AudioCommandKind::Resume:
				voice.paused = false;
				voice.pausing = false;
				break;
			case AudioCommandKind::SetVolume:
				voice.targetVolume = Clamp(command.value, 0.0f, MAX_VOICE_GAIN);
				break;
			case AudioCommandKind::SetPitch:
				voice.targetPitch = Clamp(command.value, MIN_PITCH, MAX_PITCH);
				if (voice.stream && voice.targetPitch != 0.0f && (voice.targetPitch < 0.0f ? -1 : 1) != voice.stream->direction) {
					RestartVoiceStream(voice);
				}
				break;
			case AudioCommandKind::SetStereoPan:
				voice.targetStereoPan = Clamp(command.value, -1.0f, 1.0f);
				break;
			case AudioCommandKind::SetLoop: {
				const bool looped = voice.loop;
				voice.loop = command.value != 0.0f;
				if (voice.loop != looped) {
					RestartVoiceStream(voice);
				}
				break;
			}
			case AudioCommandKind::Seek:
				SeekVoice(voice, command.frame);
				RestartVoiceStream(voice);
				break;
			case AudioCommandKind::Reschedule:
				if (voice.phase == VoicePhase::Scheduled) {
					const u64 startAt = MixerFrameOf(command.at, voice.bus);
					voice.startAt = startAt < blockStart ? blockStart : startAt;
				}
				break;
			case AudioCommandKind::SetVoiceOutput:
				voice.bus = LiveOrRoot(command.bus, command.busGeneration);
				break;
			case AudioCommandKind::SetVoiceReal:
				voice.wantsReal = command.flag;
				break;
			case AudioCommandKind::SetVoiceGenerator:
				if (voice.clip && voice.clip->encoding == AudioClipEncoding::Generator) {
					voice.clip->stream->instance = command.generator;
					voice.clip->stream->count = 0;
				}
				break;
			case AudioCommandKind::SetGeneratorParameter:
				if (voice.clip && voice.clip->encoding == AudioClipEncoding::Generator && voice.clip->stream->instance) {
					voice.clip->stream->instance->SetParameter(command.frames, command.value);
				}
				break;
			case AudioCommandKind::SetVoiceEffects:
				voice.chain = command.chain;
				break;
			case AudioCommandKind::SetVoiceEffectParameter:
				if (voice.chain && command.other < voice.chain->count && voice.chain->effects[command.other]) {
					voice.chain->effects[command.other]->SetParameter(command.frames, command.value);
				}
				break;
			case AudioCommandKind::SetVoiceBypassEffects:
				voice.bypassEffects = command.flag;
				break;
			case AudioCommandKind::SetVoiceStream:
				voice.stream = command.stream;
				RestartVoiceStream(voice);
				break;
			case AudioCommandKind::SetVoiceSend:
				if (command.bus != voice.send || command.busGeneration != voice.sendGeneration) {
					voice.send = command.bus;
					voice.sendGeneration = command.busGeneration;
					voice.sendLevel = Clamp(command.value, 0.0f, MAX_VOICE_GAIN);
				}
				voice.targetSendLevel = Clamp(command.value, 0.0f, MAX_VOICE_GAIN);
				break;
			default:
				break;
			}
		}

		AudioVoiceDecoder* AudioRenderer::AcquireDecoder(AudioVoiceState& voice) {
			if (_freeDecoderCount == 0) {
				return nullptr;
			}
			const u32 index = _freeDecoders[--_freeDecoderCount];
			AudioVoiceDecoder& decoder = _decoders[index];
			decoder.gainsValid = false;
			decoder.qoa = QOACursor{};
			voice.decoder = index;
			return &decoder;
		}

		void AudioRenderer::ReleaseDecoder(AudioVoiceState& voice) {
			if (voice.decoder == NO_DECODER) {
				return;
			}
			_freeDecoders[_freeDecoderCount++] = voice.decoder;
			voice.decoder = NO_DECODER;
		}

		void AudioRenderer::LetGoOfVirtualState(AudioVoiceState& voice) {
			voice.spatializer = nullptr;
			voice.spatializerState = nullptr;
			if (voice.chain && !voice.chain->keptWhileVirtual) {
				voice.chain = nullptr;
			}
			voice.stream = nullptr;
		}

		void AudioRenderer::EndVoice(u32 slot) {
			AudioVoiceState& voice = _voices[slot];
			AudioEvent event;
			event.kind = AudioEventKind::VoiceEnded;
			event.slot = slot;
			event.generation = voice.generation;
			voice.phase = VoicePhase::Free;
			voice.clip = nullptr;
			voice.spatializer = nullptr;
			voice.spatializerState = nullptr;
			voice.chain = nullptr;
			voice.stream = nullptr;
			voice.sendGeneration = 0;
			ReleaseDecoder(voice);
			_active[slot / 64] &= ~(1ull << (slot % 64));
			if (!_events.TryPush(event)) {
				Bump(_counters.droppedEvents, 1);
			}
		}

		void AudioRenderer::PostPlacement(u32 slot, const AudioVoicePlacement& placement) {
			if (_placements && slot < _voiceCapacity) {
				_placements[slot].Post(placement);
			}
		}

		void AudioRenderer::NoteCallback(u64 startTicks, u64 endTicks, u32 frameCount) {
			const u64 cost = endTicks - startTicks;
			Bump(_counters.callbacks, 1);
			Bump(_counters.frames, frameCount);
			Bump(_counters.costTicks, cost);
			RaiseTo(_counters.worstCostTicks, cost);

			if (_lastCallbackTicks != 0 && _framesDelivered >= _primingFrames) {
				const u64 interval = startTicks - _lastCallbackTicks;
				LowerTo(_counters.shortestIntervalTicks, interval);
				RaiseTo(_counters.longestIntervalTicks, interval);
			}
			_lastCallbackTicks = startTicks;
			_framesDelivered += frameCount;

			if (cost * _sampleRate > static_cast<u64>(frameCount) * _ticksPerSecond) {
				Bump(_counters.overruns, 1);
			}

			_counters.renderThread.store(Realtime::GetThreadId(), std::memory_order_relaxed);
		}

		void AudioRenderer::ClearMeter(u32 slot) {
			if (!_metered || slot >= MAX_BUSES) {
				return;
			}
			for (AudioMeterTap* tap : {&_metered[slot].output, &_metered[slot].fader}) {
				for (u32 channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel) {
					tap->meanSquare[channel].store(0, std::memory_order_relaxed);
					tap->peak[channel].store(0, std::memory_order_relaxed);
				}
			}
		}

		void AudioRenderer::Meter(AudioMeterTap& meter, const f32* buffer) {
			const u32 channels = _layout.count;
			for (u32 channel = 0; channel < channels; ++channel) {
				f32 loudest = 0.0f;
				f32 sum = 0.0f;
				if (buffer) {
					const f32* row = buffer + static_cast<size_t>(channel) * BLOCK_FRAMES;
					for (u32 frame = 0; frame < BLOCK_FRAMES; ++frame) {
						const f32 magnitude = row[frame] < 0.0f ? -row[frame] : row[frame];
						loudest = magnitude > loudest ? magnitude : loudest;
						sum += row[frame] * row[frame];
					}
				}
				f32 meanSquare = std::bit_cast<f32>(meter.meanSquare[channel].load(std::memory_order_relaxed));
				meanSquare += (sum / static_cast<f32>(BLOCK_FRAMES) - meanSquare) * _meterAverage;
				f32 peak = std::bit_cast<f32>(meter.peak[channel].load(std::memory_order_relaxed)) * _meterRelease;
				peak = loudest > peak ? loudest : peak;
				meanSquare = meanSquare > 1.0e-15f ? meanSquare : 0.0f;
				peak = peak > 3.2e-8f ? peak : 0.0f;
				meter.meanSquare[channel].store(std::bit_cast<u32>(meanSquare), std::memory_order_relaxed);
				meter.peak[channel].store(std::bit_cast<u32>(peak), std::memory_order_relaxed);
			}
		}

		void AudioRenderer::PublishClock(u64 ticks) {
			const u32 sequence = _clockSequence.load(std::memory_order_relaxed);
			_clockSequence.store(sequence + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			_clockFrames.store(_mixerFrames, std::memory_order_relaxed);
			_clockTime.store(_mixerFrames - _stoppedFrames, std::memory_order_relaxed);
			_clockTicks.store(ticks, std::memory_order_relaxed);
			_clockRunning.store(_gameplayRunning, std::memory_order_relaxed);
			_clockSequence.store(sequence + 2, std::memory_order_release);
		}

	} // namespace AudioImpl

} // namespace Shalltear
