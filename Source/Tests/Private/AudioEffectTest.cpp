#include "TestsPCH.h"

#ifndef SHALLTEAR_SHARED

#include "AudioEffectRegistry.h"
#include "AudioLiveFixture.h"
#include "AudioOfflineMixer.h"
#include "AudioSettings.h"

#include "Shalltear/Audio/Audio.h"
#include "Shalltear/Audio/AudioEffect.h"
#include "Shalltear/Audio/AudioGenerator.h"
#include "Shalltear/Audio/AudioMixer.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Module/ModuleEvents.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace AudioTesting;

namespace {

	constexpr u32 BLOCK = OfflineMixer::BLOCK;

	std::vector<f32> Constant(u32 frames, f32 value) { return std::vector<f32>(frames, value); }

	constexpr u32 DENORMAL_BITS = 0x8000u | 0x0040u;

	class ChainProbe final : public IAudioEffect {
	public:
		enum class Kind : u8 { Clip,
							   Gain,
							   Silence,
							   Tail };

		ChainProbe(Kind kind, f32 value) : _kind(kind), _value(value) {}
		~ChainProbe() = default;

		void Process(AudioBlock& block, const AudioBlock* sidechain) [[clang::nonblocking]] override {
			++processed;
			mxcsr = __builtin_ia32_stmxcsr();
			sidechainHanded = sidechain != nullptr;
			sidechainLoudest = 0.0f;
			for (u32 channel = 0; sidechain && channel < sidechain->channels; ++channel) {
				for (u32 frame = 0; frame < sidechain->frames; ++frame) {
					const f32 in = sidechain->data[static_cast<size_t>(channel) * sidechain->stride + frame];
					const f32 magnitude = in < 0.0f ? -in : in;
					sidechainLoudest = magnitude > sidechainLoudest ? magnitude : sidechainLoudest;
				}
			}
			bool heard = false;
			for (u32 channel = 0; channel < block.channels; ++channel) {
				f32* row = block.data + static_cast<size_t>(channel) * block.stride;
				for (u32 frame = 0; frame < block.frames; ++frame) {
					const f32 in = row[frame];
					const f32 magnitude = in < 0.0f ? -in : in;
					loudest = magnitude > loudest ? magnitude : loudest;
					heard = heard || in != 0.0f;
					switch (_kind) {
					case Kind::Clip:
						row[frame] = in > _value ? _value : (in < -_value ? -_value : in);
						break;
					case Kind::Gain:
						row[frame] = in * _value;
						break;
					case Kind::Silence:
						row[frame] = 0.0f;
						break;
					case Kind::Tail:
						break;
					}
				}
			}
			if (_kind != Kind::Tail) {
				return;
			}
			if (heard) {
				_tail = 3;
				return;
			}
			if (_tail > 0) {
				--_tail;
				for (u32 channel = 0; channel < block.channels; ++channel) {
					for (u32 frame = 0; frame < block.frames; ++frame) {
						block.data[static_cast<size_t>(channel) * block.stride + frame] = _value;
					}
				}
			}
		}

		void SetParameter(u32 index, f32 value) [[clang::nonblocking]] override {
			if (index == 0) {
				_value = value;
			}
		}

		bool IsQuiet() const [[clang::nonblocking]] override { return _kind == Kind::Tail ? _tail == 0 : false; }

		void Release() override { released = true; }

		u32 processed = 0;
		f32 loudest = 0.0f;
		u32 mxcsr = 0;
		bool released = false;
		bool sidechainHanded = false;
		f32 sidechainLoudest = 0.0f;
	private:
		Kind _kind;
		f32 _value;
		u32 _tail = 0;
	};

	class RequeueProbe final : public IAudioEffect {
	public:
		void Process(AudioBlock& block, const AudioBlock* sidechain) [[clang::nonblocking]] override {
			(void)block;
			(void)sidechain;
		}

		void SetParameter(u32 index, f32 value) [[clang::nonblocking]] override {
			(void)index;
			(void)value;
			if (++calls < 100) {
				commands->TryPush(next);
			}
		}

		bool IsQuiet() const [[clang::nonblocking]] override { return true; }

		void Release() override {}

		AudioRing<AudioCommand>* commands = nullptr;
		AudioCommand next;
		u32 calls = 0;
	};

	AudioCommand EffectRow(OfflineBus bus, u32 index, IAudioEffect* effect, bool bypass = false, bool wetMixing = false, f32 wet = 1.0f) {
		return OfflineMixer::Row(bus, index, AudioBusRowKind::Effect, {}, wet, effect, bypass, wetMixing);
	}

	bool Chain(OfflineMixer& mixer, OfflineBus bus, AudioBusRowKind first, AudioBusRowKind second, IAudioEffect* effect) {
		const auto row = [&](u32 index, AudioBusRowKind kind) {
			return kind == AudioBusRowKind::Effect ? EffectRow(bus, index, effect) : OfflineMixer::Row(bus, index, kind);
		};
		return mixer.Push(row(0, first)) && mixer.Push(row(1, second)) && mixer.Push(OfflineMixer::RowCount(bus, 2));
	}

} // namespace

TEST(AudioEffectChainTest, EffectOrderAroundFader) {
	for (const bool above : {true, false}) {
		ChainProbe clip(ChainProbe::Kind::Clip, 0.3f);
		OfflineMixer mixer(1);
		ClipBlock source(MakePCM(Constant(4 * BLOCK, 0.5f), 1, 48000));
		const OfflineBus bus = mixer.Bus(1, {}, 0.5f);
		ASSERT_TRUE(above ? Chain(mixer, bus, AudioBusRowKind::Effect, AudioBusRowKind::Fader, &clip)
						  : Chain(mixer, bus, AudioBusRowKind::Fader, AudioBusRowKind::Effect, &clip));
		ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
		const f32 expected = above ? 0.3f * 0.5f : 0.25f;
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, expected) << (above ? "above" : "below") << " the fader";
		}
		EXPECT_EQ(clip.loudest, above ? 0.5f : 0.25f);
	}
}

TEST(AudioEffectChainTest, SendBelowEffectCarriesWhatEffectMade) {
	for (const bool sendFirst : {false, true}) {
		ChainProbe clip(ChainProbe::Kind::Clip, 0.3f);
		OfflineMixer mixer(1);
		ClipBlock source(MakePCM(Constant(4 * BLOCK, 0.5f), 1, 48000));
		const OfflineBus reverb = mixer.Bus(2);
		const OfflineBus dry = mixer.Bus(1, {}, 0.0f);
		const u32 effectRow = sendFirst ? 1 : 0;
		for (u32 index = 0; index < 2; ++index) {
			const AudioCommand command = index == effectRow ? EffectRow(dry, index, &clip) : OfflineMixer::Row(dry, index, AudioBusRowKind::Send, reverb, 1.0f);
			ASSERT_TRUE(mixer.Push(command));
		}
		ASSERT_TRUE(mixer.Push(OfflineMixer::Row(dry, 2, AudioBusRowKind::Fader)));
		ASSERT_TRUE(mixer.Push(OfflineMixer::RowCount(dry, 3)));
		ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), dry)));
		const f32 expected = sendFirst ? 0.5f : 0.3f;
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, expected) << (sendFirst ? "a send above the effect" : "a send below the effect");
		}
	}
}

TEST(AudioEffectChainTest, BypassPassesSoundThrough) {
	ChainProbe clip(ChainProbe::Kind::Clip, 0.3f);
	OfflineMixer mixer(1);
	ClipBlock source(MakePCM(Constant(16 * BLOCK, 0.5f), 1, 48000));
	const OfflineBus bus = mixer.Bus(1);
	ASSERT_TRUE(mixer.Push(EffectRow(bus, 0, &clip, true)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Row(bus, 1, AudioBusRowKind::Fader)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::RowCount(bus, 2)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
	const auto block = [&](f32 expected, const char* what) {
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, expected) << what;
		}
	};
	block(0.5f, "a bypassed unit touched the sound");
	EXPECT_EQ(clip.processed, 0u);

	AudioCommand unbypass = OfflineMixer::Flag(AudioCommandKind::SetBusRowBypass, bus, false);
	unbypass.mode = 0;
	ASSERT_TRUE(mixer.Push(unbypass));
	block(0.3f, "the unit's bypass did not come off");

	ASSERT_TRUE(mixer.Push(OfflineMixer::Flag(AudioCommandKind::SetBusBypassEffects, bus, true)));
	const u32 before = clip.processed;
	block(0.5f, "the group's bypass left an effect running");
	EXPECT_EQ(clip.processed, before);

	ASSERT_TRUE(mixer.Push(OfflineMixer::Flag(AudioCommandKind::SetBusBypassEffects, bus, false)));
	block(0.3f, "the group's bypass did not come off");
}

TEST(AudioEffectChainTest, WetMixBlendsWithDry) {
	ChainProbe silence(ChainProbe::Kind::Silence, 0.0f);
	OfflineMixer mixer(1);
	ClipBlock source(MakePCM(Constant(16 * BLOCK, 0.5f), 1, 48000));
	const OfflineBus bus = mixer.Bus(1);
	ASSERT_TRUE(mixer.Push(EffectRow(bus, 0, &silence, false, true, 0.25f)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Row(bus, 1, AudioBusRowKind::Fader)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::RowCount(bus, 2)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.5f * 0.75f);
	}

	AudioCommand wet = OfflineMixer::BusCommand(AudioCommandKind::SetBusRowLevel, bus, 1.0f, BLOCK);
	wet.mode = 0;
	ASSERT_TRUE(mixer.Push(wet));
	const std::vector<f32> walk = mixer.Render(BLOCK);
	EXPECT_LT(walk[0], 0.5f * 0.75f);
	EXPECT_GT(walk[0], 0.0f);
	EXPECT_EQ(walk[BLOCK - 1], 0.0f);
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.0f);
	}
}

TEST(AudioEffectChainTest, SilentGroupRunsUntilQuiet) {
	ChainProbe tail(ChainProbe::Kind::Tail, 0.125f);
	OfflineMixer mixer(1);
	ClipBlock source(MakePCM(Constant(BLOCK, 0.5f), 1, 48000));
	const OfflineBus bus = mixer.Bus(1);
	ASSERT_TRUE(Chain(mixer, bus, AudioBusRowKind::Effect, AudioBusRowKind::Fader, &tail));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.0f);
	}
	EXPECT_EQ(tail.processed, 0u);

	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.5f);
	}
	for (u32 block = 0; block < 3; ++block) {
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, 0.125f);
		}
	}
	const u32 rung = tail.processed;
	for (u32 block = 0; block < 4; ++block) {
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, 0.0f);
		}
	}
	EXPECT_EQ(tail.processed, rung);
}

TEST(AudioEffectChainTest, AParameterReachesTheInstanceBetweenBlocks) {
	ChainProbe gain(ChainProbe::Kind::Gain, 1.0f);
	OfflineMixer mixer(1);
	ClipBlock source(MakePCM(Constant(8 * BLOCK, 0.5f), 1, 48000));
	const OfflineBus bus = mixer.Bus(1);
	ASSERT_TRUE(Chain(mixer, bus, AudioBusRowKind::Effect, AudioBusRowKind::Fader, &gain));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.5f);
	}
	AudioCommand parameter = OfflineMixer::BusCommand(AudioCommandKind::SetBusEffectParameter, bus, 0.5f, 0);
	parameter.mode = 0;
	ASSERT_TRUE(mixer.Push(parameter));
	AudioCommand astray = parameter;
	astray.mode = 1;
	astray.value = 4.0f;
	ASSERT_TRUE(mixer.Push(astray));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.25f);
	}
}

TEST(AudioEffectChainTest, CommandsQueuedMidBlockWaitForNext) {
	RequeueProbe probe;
	OfflineMixer mixer(1);
	const OfflineBus bus = mixer.Bus(1);
	ASSERT_TRUE(Chain(mixer, bus, AudioBusRowKind::Effect, AudioBusRowKind::Fader, &probe));
	probe.commands = &mixer.Get().GetCommands();
	probe.next = OfflineMixer::BusCommand(AudioCommandKind::SetBusEffectParameter, bus, 0.5f, 0);
	probe.next.mode = 0;
	ASSERT_TRUE(mixer.Push(probe.next));
	mixer.Render(BLOCK);
	EXPECT_EQ(probe.calls, 1u);
	mixer.Render(BLOCK);
	EXPECT_EQ(probe.calls, 2u);
}

TEST(AudioEffectChainTest, EffectsFlushDenormalsRestoreMode) {
	const u32 saved = __builtin_ia32_stmxcsr();
	__builtin_ia32_ldmxcsr(saved & ~DENORMAL_BITS);
	const u32 before = __builtin_ia32_stmxcsr();
	{
		ChainProbe gain(ChainProbe::Kind::Gain, 1.0f);
		OfflineMixer mixer(1);
		ClipBlock source(MakePCM(Constant(4 * BLOCK, 0.5f), 1, 48000));
		const OfflineBus bus = mixer.Bus(1);
		ASSERT_TRUE(Chain(mixer, bus, AudioBusRowKind::Effect, AudioBusRowKind::Fader, &gain));
		ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), bus)));
		mixer.Render(BLOCK);
		ASSERT_GT(gain.processed, 0u);
		EXPECT_EQ(gain.mxcsr & DENORMAL_BITS, DENORMAL_BITS);
		EXPECT_EQ(__builtin_ia32_stmxcsr(), before);
	}
	__builtin_ia32_ldmxcsr(saved);
}

namespace {

	bool Rows(OfflineMixer& mixer, OfflineBus bus, std::initializer_list<AudioCommand> rows) {
		for (const AudioCommand& row : rows) {
			if (!mixer.Push(row)) {
				return false;
			}
		}
		return mixer.Push(OfflineMixer::RowCount(bus, static_cast<u32>(rows.size())));
	}

	OfflineBus Sender(OfflineMixer& mixer, u32 slot, OfflineBus target, u8 row, f32 level = 1.0f) {
		const OfflineBus sender = mixer.Bus(slot, {}, 0.0f);
		EXPECT_TRUE(Rows(mixer, sender,
						 {OfflineMixer::Row(sender, 0, AudioBusRowKind::Send, target, level, nullptr, false, false, row),
						  OfflineMixer::Row(sender, 1, AudioBusRowKind::Fader)}));
		return sender;
	}

} // namespace

TEST(AudioEffectChainTest, SendEntersAtReceive) {
	enum class Aim : u8 { Input,
						  Receive,
						  Fader };
	for (const Aim aim : {Aim::Input, Aim::Receive, Aim::Fader}) {
		ChainProbe clip(ChainProbe::Kind::Clip, 0.3f);
		OfflineMixer mixer(1);
		ClipBlock source(MakePCM(Constant(4 * BLOCK, 0.5f), 1, 48000));
		const OfflineBus target = mixer.Bus(2);
		ASSERT_TRUE(mixer.Receives(target, 1));
		ASSERT_TRUE(Rows(mixer, target,
						 {EffectRow(target, 0, &clip), OfflineMixer::Row(target, 1, AudioBusRowKind::Receive, {}, 0.0f, nullptr, false, false, 0),
						  OfflineMixer::Row(target, 2, AudioBusRowKind::Fader)}));
		const u8 row = aim == Aim::Input ? NO_BUS_ROW : (aim == Aim::Receive ? 1 : 2);
		const OfflineBus sender = Sender(mixer, 1, target, row);
		ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), sender)));
		const f32 expected = aim == Aim::Input ? 0.3f : (aim == Aim::Receive ? 0.5f : 0.0f);
		for (f32 sample : mixer.Render(BLOCK)) {
			ASSERT_EQ(sample, expected) << "aimed at " << static_cast<u32>(aim);
		}
		EXPECT_EQ(clip.loudest, aim == Aim::Input ? 0.5f : 0.0f);
		if (aim == Aim::Receive) {
			EXPECT_GT(clip.processed, 0u);
		}
	}
}

TEST(AudioEffectChainTest, SendFeedsTargetSidechain) {
	ChainProbe keyed(ChainProbe::Kind::Gain, 1.0f);
	ChainProbe plain(ChainProbe::Kind::Gain, 1.0f);
	OfflineMixer mixer(1);
	ClipBlock own(MakePCM(Constant(8 * BLOCK, 0.25f), 1, 48000));
	ClipBlock sent(MakePCM(Constant(BLOCK, 0.5f), 1, 48000));
	const OfflineBus target = mixer.Bus(2);
	ASSERT_TRUE(mixer.Receives(target, 1));
	ASSERT_TRUE(Rows(mixer, target,
					 {OfflineMixer::Row(target, 0, AudioBusRowKind::Effect, {}, 1.0f, &keyed, false, false, 0), EffectRow(target, 1, &plain),
					  OfflineMixer::Row(target, 2, AudioBusRowKind::Fader)}));
	const OfflineBus sender = Sender(mixer, 1, target, 0, 0.5f);
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, own.Get()), target)));
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(1, sent.Get()), sender)));

	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.25f);
	}
	EXPECT_TRUE(keyed.sidechainHanded);
	EXPECT_EQ(keyed.sidechainLoudest, 0.25f);
	EXPECT_FALSE(plain.sidechainHanded);

	mixer.Render(BLOCK);
	EXPECT_TRUE(keyed.sidechainHanded);
	EXPECT_EQ(keyed.sidechainLoudest, 0.0f);
}

TEST(AudioEffectChainTest, GroupMuteHoldsReceive) {
	OfflineMixer mixer(1);
	ClipBlock source(MakePCM(Constant(8 * BLOCK, 0.5f), 1, 48000));
	const OfflineBus target = mixer.Bus(2);
	ASSERT_TRUE(mixer.Receives(target, 1));
	ASSERT_TRUE(Rows(mixer, target,
					 {OfflineMixer::Row(target, 0, AudioBusRowKind::Receive, {}, 0.0f, nullptr, false, false, 0),
					  OfflineMixer::Row(target, 1, AudioBusRowKind::Fader)}));
	const OfflineBus sender = Sender(mixer, 1, target, 0);
	ASSERT_TRUE(mixer.Push(OfflineMixer::Into(mixer.Play(0, source.Get()), sender)));
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.5f);
	}
	ASSERT_TRUE(mixer.Push(OfflineMixer::Flag(AudioCommandKind::SetBusMute, target, true)));
	mixer.Render(BLOCK);
	for (f32 sample : mixer.Render(BLOCK)) {
		ASSERT_EQ(sample, 0.0f);
	}
}

namespace {

	constexpr const char* PROBE = "Test.Probe";

	struct ProbeCounters {
		std::atomic<u32> made{0};
		std::atomic<u32> released{0};
		std::atomic<u64> processed{0};
		std::atomic<u32> violations{0};
		std::atomic<f32> parameter{0.0f};
		std::atomic<f32> sidechainPeak{-1.0f};
		std::atomic<bool> stall{false};
		std::atomic<bool> inside{false};
		std::atomic<u32> releasedInside{0};
	};

	constexpr std::chrono::milliseconds StallDuration{50};

	void Stall(std::atomic<bool>& armed, std::atomic<bool>& inside) {
		if (!armed.exchange(false)) {
			return;
		}
		inside.store(true);
		const auto until = std::chrono::steady_clock::now() + StallDuration;
		while (std::chrono::steady_clock::now() < until) {
		}
		inside.store(false);
	}

	class ProbeInstance final : public IAudioEffect {
	public:
		ProbeInstance(ProbeCounters& counters, f32 gain) : _counters(counters), _gain(gain) {}
		~ProbeInstance() = default;

		void Process(AudioBlock& block, const AudioBlock* sidechain) [[clang::nonblocking]] override {
			if (released.load(std::memory_order_acquire)) {
				_counters.violations.fetch_add(1, std::memory_order_relaxed);
			}
			f32 peak = sidechain ? 0.0f : -1.0f;
			for (u32 channel = 0; sidechain && channel < sidechain->channels; ++channel) {
				for (u32 frame = 0; frame < sidechain->frames; ++frame) {
					const f32 in = sidechain->data[static_cast<size_t>(channel) * sidechain->stride + frame];
					peak = (in < 0.0f ? -in : in) > peak ? (in < 0.0f ? -in : in) : peak;
				}
			}
			_counters.sidechainPeak.store(peak, std::memory_order_relaxed);
			Stall(_counters.stall, _counters.inside);
			_counters.processed.fetch_add(1, std::memory_order_relaxed);
			for (u32 channel = 0; channel < block.channels; ++channel) {
				for (u32 frame = 0; frame < block.frames; ++frame) {
					block.data[static_cast<size_t>(channel) * block.stride + frame] *= _gain;
				}
			}
		}

		void SetParameter(u32 index, f32 value) [[clang::nonblocking]] override {
			if (index == 0) {
				_gain = value;
				_counters.parameter.store(value, std::memory_order_relaxed);
			}
		}

		void Release() override {
			if (_counters.inside.load()) {
				_counters.releasedInside.fetch_add(1);
			}
			released.store(true, std::memory_order_release);
			_counters.released.fetch_add(1, std::memory_order_relaxed);
		}

		std::atomic<bool> released{false};
	private:
		ProbeCounters& _counters;
		f32 _gain;
	};

	class ProbeDefinition final : public IAudioEffectDefinition {
	public:
		static constexpr AudioParameterDefinition Parameters[] = {{"Gain", "x", 0.0f, 4.0f, 1.0f}};

		std::span<const AudioParameterDefinition> GetParameters() const override { return Parameters; }

		IAudioEffect* CreateEffect(const AudioEffectContext& context) override {
			if (refuse) {
				return nullptr;
			}
			lastContext = context;
			lastParameters.assign(context.parameters.begin(), context.parameters.end());
			lastContext.parameters = {};
			instances.push_back(std::make_unique<ProbeInstance>(counters, context.parameters.empty() ? 1.0f : context.parameters[0]));
			counters.made.fetch_add(1, std::memory_order_relaxed);
			return instances.back().get();
		}

		ProbeCounters counters;
		std::vector<std::unique_ptr<ProbeInstance>> instances;
		AudioEffectContext lastContext;
		std::vector<f32> lastParameters;
		bool refuse = false;
	};

	class TableDefinition final : public IAudioEffectDefinition {
	public:
		explicit TableDefinition(std::span<const AudioParameterDefinition> table) : _table(table) {}
		std::span<const AudioParameterDefinition> GetParameters() const override { return _table; }
		IAudioEffect* CreateEffect(const AudioEffectContext&) override { return nullptr; }
	private:
		std::span<const AudioParameterDefinition> _table;
	};

	f32 CentredPeak(f32 value) { return value * std::sqrt(0.5f); }

	class AudioEffectFixture : public LivePackageFixture {
	protected:
		ProbeDefinition _probe;
		char _module = 0;
		Vector<AudioMixer*> _mixers{GetMemoryResource(MemTag::Asset)};

		void SetUp() override {
			LivePackageFixture::SetUp();
			GetAudioSystem().SetListenerPresent(true);
		}

		void TearDown() override {
			for (AudioMixer* mixer : _mixers) {
				Delete(mixer);
			}
			_mixers.clear();
			LivePackageFixture::TearDown();
		}

		AudioMixer* ProbeMixer(f32 gain = 0.5f, const char* effect = PROBE, bool wetMixing = false, f32 wetPercent = 100.0f) {
			AudioMixer* mixer = New<AudioMixer>(MemTag::Asset);
			_mixers.push_back(mixer);
			AudioMixer::Group master;
			master.id = "Master";
			master.name = "Master";
			AudioMixer::Unit unit;
			unit.kind = AudioMixerUnitKind::Effect;
			unit.id = "Probe";
			unit.effect = effect;
			unit.wetMixing = wetMixing;
			master.units.push_back(unit);
			master.units.push_back(AudioMixer::Unit{});
			mixer->groups.push_back(std::move(master));
			const auto snapshot = [&](const char* name, f32 value) {
				AudioMixer::Snapshot row;
				row.id = name;
				row.name = name;
				AudioMixer::Value set;
				set.group = "Master";
				set.parameter = AudioMixerParameter::EffectParameter;
				set.target = "Probe";
				set.effectParameter = "Gain";
				set.value = value;
				row.values.push_back(set);
				if (wetMixing) {
					AudioMixer::Value wet;
					wet.group = "Master";
					wet.parameter = AudioMixerParameter::WetLevel;
					wet.target = "Probe";
					wet.value = wetPercent;
					row.values.push_back(wet);
				}
				mixer->snapshots.push_back(std::move(row));
			};
			snapshot("Start", gain);
			snapshot("Loud", 2.0f);
			mixer->startSnapshot = "Start";
			AudioMixer::ExposedParameter exposed;
			exposed.name = "ProbeGain";
			exposed.group = "Master";
			exposed.parameter = AudioMixerParameter::EffectParameter;
			exposed.target = "Probe";
			exposed.effectParameter = "Gain";
			mixer->exposedParameters.push_back(std::move(exposed));
			EXPECT_TRUE(mixer->ApplyChanges());
			return mixer;
		}

		AudioVoice LoopInto(f32 value, AudioBus bus) {
			AudioPlayParams params;
			params.loop = true;
			params.output = bus;
			return Audio::Play(ResourceOf(MakeClip(96000, 1, value)), params);
		}

		static bool PeakNear(f32 expected) {
			return WaitFor([expected]() { return std::fabs(LastBlockPeak() - expected) < 1e-4f; });
		}

		bool Processing() {
			const u64 before = _probe.counters.processed.load();
			return WaitFor([&]() { return _probe.counters.processed.load() > before + 4; });
		}

		bool RegisterProbe() { return AudioEffects::Register(PROBE, &_probe, &_module); }

		static void Unloading(const void* image) { ModuleEvents::ImageUnloading.Publish(EventBus::GetGlobal(), ImageUnloadingEvent{image}); }
	};

} // namespace

TEST_F(AudioEffectFixture, RegisterRefusesUnnameable) {
	const u32 before = GetRegisteredEffectCount();
	EXPECT_FALSE(AudioEffects::Register("", &_probe, &_module));
	EXPECT_FALSE(AudioEffects::Register(PROBE, nullptr, &_module));
	ASSERT_TRUE(RegisterProbe());
	EXPECT_FALSE(RegisterProbe());

	static constexpr AudioParameterDefinition Unnamed[] = {{"", "x", 0.0f, 1.0f, 0.5f}};
	static constexpr AudioParameterDefinition Repeated[] = {{"Mix", "%", 0.0f, 100.0f, 50.0f}, {"Mix", "%", 0.0f, 100.0f, 50.0f}};
	static constexpr AudioParameterDefinition Outside[] = {{"Cutoff", "Hz", 20.0f, 20000.0f, 10.0f}};
	TableDefinition unnamed(Unnamed);
	TableDefinition repeated(Repeated);
	TableDefinition outside(Outside);
	EXPECT_FALSE(AudioEffects::Register("Test.Unnamed", &unnamed, &_module));
	EXPECT_FALSE(AudioEffects::Register("Test.Repeated", &repeated, &_module));
	EXPECT_FALSE(AudioEffects::Register("Test.Outside", &outside, &_module));
	EXPECT_EQ(GetRegisteredEffectCount(), before + 1);

	EXPECT_EQ(AudioEffects::UnregisterOwnedBy(&_module), 1u);
	EXPECT_EQ(GetRegisteredEffectCount(), before);
	EXPECT_EQ(FindEffect(PROBE), nullptr);
	EXPECT_TRUE(RegisterProbe());
}

TEST_F(AudioEffectFixture, UnregisteredUnitPassesSound) {
	AudioMixer* mixer = ProbeMixer(0.5f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();
	EXPECT_EQ(_probe.counters.made.load(), 0u);

	ASSERT_TRUE(RegisterProbe());
	EXPECT_EQ(_probe.counters.made.load(), 1u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f)) << LastBlockPeak();

	const AudioRenderer& renderer = GetAudioSystem().GetRenderer();
	EXPECT_EQ(_probe.lastContext.sampleRate, renderer.GetSampleRate());
	EXPECT_EQ(_probe.lastContext.channels, renderer.GetLayout().count);
	EXPECT_EQ(_probe.lastContext.blockFrames, AudioRenderer::BLOCK_FRAMES);
	ASSERT_EQ(_probe.lastParameters.size(), 1u);
	EXPECT_EQ(_probe.lastParameters[0], 0.5f);
}

TEST_F(AudioEffectFixture, UnregisterClearsAudioThreadFirst) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* heard = ProbeMixer(0.5f);
	ProbeMixer(0.5f);
	ASSERT_EQ(_probe.counters.made.load(), 2u);
	LoopInto(0.5f, heard->GetBus("Master"));
	ASSERT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f));
	ASSERT_TRUE(Processing());

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(AudioEffects::UnregisterOwnedBy(&_module), 1u);
	const f64 held = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
	std::printf("[ READING  ] the fence held the main thread %.2f ms. Device's period is %s\n", held, GetSettings().obtainedPeriod.c_str());
	EXPECT_EQ(_probe.counters.released.load(), 2u);
	ASSERT_TRUE(WaitForBlocks(8));
	EXPECT_EQ(_probe.counters.violations.load(), 0u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();

	ASSERT_TRUE(RegisterProbe());
	EXPECT_EQ(_probe.counters.made.load(), 4u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f));
}

TEST_F(AudioEffectFixture, UnloadingImageTakesEffectsBackBehindFence) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = ProbeMixer(0.5f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	ASSERT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f));
	ASSERT_TRUE(Processing());

	char elsewhere = 0;
	Unloading(&elsewhere);
	EXPECT_NE(FindEffect(PROBE), nullptr);
	EXPECT_EQ(_probe.counters.released.load(), 0u);

	Unloading(&_module);
	EXPECT_EQ(FindEffect(PROBE), nullptr);
	EXPECT_EQ(_probe.counters.released.load(), 1u);
	ASSERT_TRUE(WaitForBlocks(8));
	EXPECT_EQ(_probe.counters.violations.load(), 0u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f)));
}

TEST_F(AudioEffectFixture, UnregisterWaitsForAudioThread) {
	ASSERT_TRUE(RegisterProbe());
	ProbeMixer(0.5f);
	ASSERT_TRUE(Processing());
	_probe.counters.stall.store(true);
	ASSERT_TRUE(WaitFor([&]() { return _probe.counters.inside.load(); }));

	EXPECT_EQ(AudioEffects::UnregisterOwnedBy(&_module), 1u);
	EXPECT_EQ(_probe.counters.released.load(), 1u);
	EXPECT_EQ(_probe.counters.releasedInside.load(), 0u);
}

TEST_F(AudioEffectFixture, ParametersReachEffectInRange) {
	AudioMixer* mixer = ProbeMixer(0.5f);
	ASSERT_TRUE(mixer->SetFloat("ProbeGain", 0.25f));
	ASSERT_TRUE(RegisterProbe());
	ASSERT_EQ(_probe.lastParameters.size(), 1u);
	EXPECT_EQ(_probe.lastParameters[0], 0.25f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.25f)) << LastBlockPeak();

	ASSERT_TRUE(mixer->ClearFloat("ProbeGain"));
	RunFrame();
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f)) << LastBlockPeak();

	ASSERT_TRUE(mixer->TransitionToSnapshot("Loud", 0.0f));
	RunFrame();
	f32 value = 0.0f;
	ASSERT_TRUE(mixer->GetFloat("ProbeGain", value));
	EXPECT_EQ(value, 2.0f);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 2.0f)) << LastBlockPeak();

	ASSERT_TRUE(mixer->SetFloat("ProbeGain", 10.0f));
	EXPECT_TRUE(WaitFor([&]() { return _probe.counters.parameter.load() == 4.0f; }));
}

TEST_F(AudioEffectFixture, WetLevelIsSnapshotParameterInPercent) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = ProbeMixer(0.0f, PROBE, true, 25.0f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.75f)) << LastBlockPeak();
}

TEST_F(AudioEffectFixture, ABypassAppliedLiveKeepsTheInstance) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = ProbeMixer(0.5f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	ASSERT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f));

	mixer->groups[0].bypassEffects = true;
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();
	mixer->groups[0].bypassEffects = false;
	mixer->groups[0].units[0].bypass = true;
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();
	mixer->groups[0].units[0].bypass = false;
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f) * 0.5f));
	EXPECT_EQ(_probe.counters.made.load(), 1u);
}

TEST_F(AudioEffectFixture, RefusedInstancePassesSound) {
	_probe.refuse = true;
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = ProbeMixer(0.5f);
	LoopInto(0.5f, mixer->GetBus("Master"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();

	_probe.refuse = false;
	mixer->groups[0].units[0].id = "Renamed";
	mixer->snapshots[0].values.clear();
	mixer->snapshots[1].values.clear();
	mixer->exposedParameters.clear();
	ASSERT_TRUE(mixer->ApplyChanges());
	ASSERT_EQ(_probe.counters.made.load(), 1u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f)));

	mixer->groups[0].units.erase(mixer->groups[0].units.begin());
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(WaitFor([&]() {
		RunFrame();
		return _probe.counters.released.load() == 1u;
	}));
	ASSERT_TRUE(WaitForBlocks(8));
	EXPECT_EQ(_probe.counters.violations.load(), 0u);
}

namespace {

	class AudioSendFixture : public AudioEffectFixture {
	protected:
		AudioMixer* SendMixer(const char* unit, f32 gain) {
			AudioMixer* mixer = New<AudioMixer>(MemTag::Asset);
			_mixers.push_back(mixer);
			const auto group = [](const char* id, const char* parent) {
				AudioMixer::Group made;
				made.id = id;
				made.name = id;
				made.parent = parent;
				return made;
			};
			AudioMixer::Group master = group("Master", "");
			master.units.push_back(AudioMixer::Unit{});
			AudioMixer::Group music = group("Music", "Master");
			AudioMixer::Unit probe;
			probe.kind = AudioMixerUnitKind::Effect;
			probe.id = "Probe";
			probe.effect = PROBE;
			AudioMixer::Unit receive;
			receive.kind = AudioMixerUnitKind::Receive;
			receive.id = "Return";
			music.units.push_back(probe);
			music.units.push_back(receive);
			music.units.push_back(AudioMixer::Unit{});
			AudioMixer::Group sfx = group("SFX", "Master");
			AudioMixer::Unit send;
			send.kind = AudioMixerUnitKind::Send;
			send.target = "Music";
			send.targetUnit = unit;
			sfx.units.push_back(send);
			sfx.units.push_back(AudioMixer::Unit{});
			mixer->groups.push_back(std::move(master));
			mixer->groups.push_back(std::move(music));
			mixer->groups.push_back(std::move(sfx));

			AudioMixer::Snapshot start;
			start.id = "Start";
			start.name = "Start";
			const auto value = [](const char* group, AudioMixerParameter parameter, const char* target, const char* effectParameter, f32 set) {
				AudioMixer::Value row;
				row.group = group;
				row.parameter = parameter;
				row.target = target;
				row.effectParameter = effectParameter;
				row.value = set;
				return row;
			};
			start.values.push_back(value("SFX", AudioMixerParameter::Volume, "", "", -80.0f));
			start.values.push_back(value("SFX", AudioMixerParameter::SendLevel, "Music", "", 0.0f));
			start.values.push_back(value("Music", AudioMixerParameter::EffectParameter, "Probe", "Gain", gain));
			mixer->snapshots.push_back(std::move(start));
			mixer->startSnapshot = "Start";
			EXPECT_TRUE(mixer->ApplyChanges());
			return mixer;
		}

		bool SidechainNear(f32 expected) {
			return WaitFor([&]() { return std::fabs(_probe.counters.sidechainPeak.load() - expected) < 1e-4f; });
		}
	};

} // namespace

TEST_F(AudioSendFixture, SidechainFollowsEffectAcrossEdit) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = SendMixer("Probe", 1.0f);
	LoopInto(0.5f, mixer->GetBus("SFX"));
	EXPECT_TRUE(SidechainNear(CentredPeak(0.5f))) << _probe.counters.sidechainPeak.load();
	EXPECT_TRUE(PeakNear(0.0f)) << LastBlockPeak();

	AudioMixer::Unit above;
	above.kind = AudioMixerUnitKind::Receive;
	above.id = "Above";
	mixer->groups[1].units.insert(mixer->groups[1].units.begin(), above);
	ASSERT_TRUE(mixer->ApplyChanges());
	_probe.counters.sidechainPeak.store(-2.0f);
	EXPECT_TRUE(SidechainNear(CentredPeak(0.5f))) << _probe.counters.sidechainPeak.load();
	EXPECT_EQ(_probe.counters.made.load(), 1u);

	mixer->groups[2].units[0].targetUnit = "";
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(SidechainNear(-1.0f));
}

TEST_F(AudioSendFixture, SecondReceiveGrowsBuffers) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = SendMixer("Probe", 1.0f);
	LoopInto(0.5f, mixer->GetBus("SFX"));
	ASSERT_TRUE(SidechainNear(CentredPeak(0.5f)));
	const auto capacity = [&]() {
		const AudioBus music = mixer->GetBus("Music");
		const AudioBusRecord* record = GetAudioSystem().GetBuses().Find(AudioBusAccess::GetSlot(music), AudioBusAccess::GetGeneration(music));
		return record ? record->receiveCapacity : ~0u;
	};
	EXPECT_EQ(capacity(), 1u);

	AudioMixer::Group voice;
	voice.id = "Voice";
	voice.name = "Voice";
	voice.parent = "Master";
	AudioMixer::Unit send;
	send.kind = AudioMixerUnitKind::Send;
	send.target = "Music";
	send.targetUnit = "Return";
	voice.units.push_back(send);
	voice.units.push_back(AudioMixer::Unit{});
	mixer->groups.push_back(std::move(voice));
	AudioMixer::Value level;
	level.group = "Voice";
	level.parameter = AudioMixerParameter::SendLevel;
	level.target = "Music";
	level.value = 0.0f;
	mixer->snapshots[0].values.push_back(level);
	AudioMixer::Value shut = level;
	shut.parameter = AudioMixerParameter::Volume;
	shut.target = "";
	shut.value = -80.0f;
	mixer->snapshots[0].values.push_back(shut);
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_EQ(capacity(), 2u);
	ASSERT_TRUE(WaitForBlocks(4));
	RunFrame();

	LoopInto(0.25f, mixer->GetBus("Voice"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.25f))) << LastBlockPeak();
	_probe.counters.sidechainPeak.store(-2.0f);
	EXPECT_TRUE(SidechainNear(CentredPeak(0.5f))) << _probe.counters.sidechainPeak.load();
}

TEST_F(AudioSendFixture, SendHeardBelowEffectsAbove) {
	ASSERT_TRUE(RegisterProbe());
	AudioMixer* mixer = SendMixer("Return", 0.0f);
	LoopInto(0.5f, mixer->GetBus("SFX"));
	EXPECT_TRUE(PeakNear(CentredPeak(0.5f))) << LastBlockPeak();

	mixer->groups[2].units[0].targetUnit = "";
	ASSERT_TRUE(mixer->ApplyChanges());
	EXPECT_TRUE(PeakNear(0.0f)) << LastBlockPeak();
}

namespace {

	struct ToneCounters {
		std::atomic<u32> made{0};
		std::atomic<u32> released{0};
		std::atomic<u32> updates{0};
		std::atomic<u32> violations{0};
		std::atomic<bool> stall{false};
		std::atomic<bool> inside{false};
		std::atomic<u32> releasedInside{0};
	};

	class ToneInstance final : public IAudioGeneratorInstance {
	public:
		ToneInstance(ToneCounters& counters, u32 channels, f32 start, u64 length) : _counters(counters), _channels(channels), _length(length) {
			level.store(start, std::memory_order_relaxed);
		}
		~ToneInstance() = default;

		u32 Process(AudioBlock& out, u64 fromFrame) [[clang::nonblocking]] override {
			if (released.load(std::memory_order_acquire)) {
				_counters.violations.fetch_add(1, std::memory_order_relaxed);
			}
			Stall(_counters.stall, _counters.inside);
			if (firstFrom.load(std::memory_order_relaxed) == ~0ull) {
				firstFrom.store(fromFrame, std::memory_order_relaxed);
			}
			lastFrom.store(fromFrame, std::memory_order_relaxed);
			const u64 left = fromFrame < _length ? _length - fromFrame : 0;
			const u32 written = left < out.frames ? static_cast<u32>(left) : out.frames;
			const f32 value = level.load(std::memory_order_relaxed) * multiplier.load(std::memory_order_relaxed);
			for (u32 channel = 0; channel < out.channels && channel < _channels; ++channel) {
				for (u32 frame = 0; frame < written; ++frame) {
					out.data[static_cast<size_t>(channel) * out.stride + frame] = value;
				}
			}
			end.store(fromFrame + written, std::memory_order_relaxed);
			return written;
		}

		void SetParameter(u32 index, f32 value) [[clang::nonblocking]] override {
			if (index == 0) {
				level.store(value, std::memory_order_relaxed);
			} else if (index == 1) {
				multiplier.store(value, std::memory_order_relaxed);
			}
		}

		void Release() override {
			if (_counters.inside.load()) {
				_counters.releasedInside.fetch_add(1);
			}
			released.store(true, std::memory_order_release);
			_counters.released.fetch_add(1, std::memory_order_relaxed);
		}

		std::atomic<bool> released{false};
		std::atomic<f32> level{0.0f};
		std::atomic<f32> multiplier{1.0f};
		std::atomic<u64> firstFrom{~0ull};
		std::atomic<u64> lastFrom{0};
		std::atomic<u64> end{0};
	private:
		ToneCounters& _counters;
		u32 _channels;
		u64 _length;
	};

	struct ToneBank {
		ToneCounters counters;
		std::vector<std::unique_ptr<ToneInstance>> instances;
		AudioGeneratorFormat lastSuggested;
	};

	class EffectTestTone : public AudioGenerator {
	public:
		SLT_REFLECT_CLASS(EffectTestTone)
		SLT_REFLECT_PARENT(AudioGenerator)
		SLT_REFLECT_FIELD(level)
		SLT_END_REFLECT()

		f32 level = 0.25f;

		ToneBank* bank = nullptr;
		u32 rate = 0;
		u32 channels = 1;
		u64 length = ~0ull;
		bool refuse = false;

		IAudioGeneratorInstance* CreateInstance(const AudioGeneratorFormat& suggested, AudioGeneratorFormat& chosen) override {
			bank->lastSuggested = suggested;
			if (refuse) {
				return nullptr;
			}
			chosen.sampleRate = rate > 0 ? rate : suggested.sampleRate;
			chosen.channels = channels;
			bank->instances.push_back(std::make_unique<ToneInstance>(bank->counters, channels, level, length));
			bank->counters.made.fetch_add(1, std::memory_order_relaxed);
			return bank->instances.back().get();
		}

		void Update(AudioGeneratorControl& control) override {
			bank->counters.updates.fetch_add(1, std::memory_order_relaxed);
			control.SetParameter(0, level);
		}
	};

	class AudioGeneratorFixture : public AudioEffectFixture {
	protected:
		ToneBank _bank;
		Vector<EffectTestTone*> _tones{GetMemoryResource(MemTag::Asset)};

		void SetUp() override {
			AudioEffectFixture::SetUp();
			EffectTestTone::GetReflectType();
		}

		void TearDown() override {
			AudioEffectFixture::TearDown();
			for (EffectTestTone* tone : _tones) {
				Delete(tone);
			}
			_tones.clear();
		}

		EffectTestTone* MakeTone(f32 level = 0.25f) {
			EffectTestTone* tone = New<EffectTestTone>(MemTag::Asset);
			tone->level = level;
			tone->bank = &_bank;
			_tones.push_back(tone);
			return tone;
		}

		static AudioVoice PlayTone(EffectTestTone* tone, bool loop = false) {
			AudioPlayParams params;
			params.loop = loop;
			return Audio::Play(Handle<AudioResource>(tone), params);
		}

		ToneInstance& Instance(u32 index) { return *_bank.instances[index]; }
	};

} // namespace

TEST_F(AudioGeneratorFixture, GeneratorHeardInChosenFormat) {
	EffectTestTone* tone = MakeTone(0.25f);
	const AudioVoice voice = PlayTone(tone);
	ASSERT_TRUE(voice.IsValid());
	EXPECT_EQ(_bank.counters.made.load(), 1u);
	const AudioRenderer& renderer = GetAudioSystem().GetRenderer();
	EXPECT_EQ(_bank.lastSuggested.sampleRate, renderer.GetSampleRate());
	EXPECT_EQ(_bank.lastSuggested.channels, renderer.GetLayout().count);
	EXPECT_TRUE(PeakNear(CentredPeak(0.25f))) << LastBlockPeak();
	EXPECT_TRUE(Audio::IsPlaying(voice));
	EXPECT_EQ(Instance(0).firstFrom.load(), 0u);
}

TEST_F(AudioGeneratorFixture, GeneratorParameterReachesOneVoice) {
	EffectTestTone* tone = MakeTone(0.25f);
	const AudioVoice first = PlayTone(tone);
	const AudioVoice second = PlayTone(tone);
	ASSERT_TRUE(first.IsValid());
	ASSERT_TRUE(second.IsValid());
	Audio::SetGeneratorParameter(first, 1, 0.5f);
	EXPECT_TRUE(WaitFor([&]() { return Instance(0).multiplier.load() == 0.5f; }));
	ASSERT_TRUE(WaitForBlocks(4));
	EXPECT_EQ(Instance(1).multiplier.load(), 1.0f);
}

TEST_F(AudioGeneratorFixture, UpdateRunsPerVoiceEachFrame) {
	EffectTestTone* tone = MakeTone(0.25f);
	ASSERT_TRUE(PlayTone(tone).IsValid());
	ASSERT_TRUE(PlayTone(tone).IsValid());
	for (u32 frame = 0; frame < 3; ++frame) {
		RunFrame();
	}
	EXPECT_EQ(_bank.counters.updates.load(), 6u);

	tone->level = 0.125f;
	RunFrame();
	EXPECT_TRUE(WaitFor([&]() { return Instance(0).level.load() == 0.125f && Instance(1).level.load() == 0.125f; }));
	EXPECT_TRUE(PeakNear(CentredPeak(0.125f) * 2.0f)) << LastBlockPeak();
}

TEST_F(AudioGeneratorFixture, ShortWriteEndsVoice) {
	EffectTestTone* tone = MakeTone(0.25f);
	tone->length = 4800;
	const AudioVoice voice = PlayTone(tone, true);
	ASSERT_TRUE(voice.IsValid());
	EXPECT_TRUE(WaitFor([&]() {
		RunFrame();
		return ClaimedVoices() == 0;
	}));
	EXPECT_FALSE(Audio::IsPlaying(voice));
	EXPECT_EQ(_bank.counters.released.load(), 1u);
	EXPECT_EQ(_bank.counters.violations.load(), 0u);
}

TEST_F(AudioGeneratorFixture, HalfRateAsksHalfFrames) {
	EffectTestTone* tone = MakeTone(0.25f);
	tone->rate = GetAudioSystem().GetRenderer().GetSampleRate() / 2;
	ASSERT_TRUE(PlayTone(tone).IsValid());
	ASSERT_TRUE(WaitFor([&]() { return !_bank.instances.empty() && Instance(0).end.load() > 0; }));
	const u64 blocks = BlocksRendered();
	const u64 end = Instance(0).end.load();
	ASSERT_TRUE(WaitForBlocks(200));
	const f64 rendered = static_cast<f64>(BlocksRendered() - blocks) * AudioRenderer::BLOCK_FRAMES;
	const f64 asked = static_cast<f64>(Instance(0).end.load() - end);
	EXPECT_NEAR(asked, rendered / 2.0, static_cast<f64>(GENERATOR_WINDOW_FRAMES) + 2.0 * AudioRenderer::BLOCK_FRAMES) << "rendered " << rendered << ' ' << asked;
}

TEST_F(AudioGeneratorFixture, SeekAsksGeneratorFromSoughtFrame) {
	EffectTestTone* tone = MakeTone(0.25f);
	const AudioVoice voice = PlayTone(tone);
	ASSERT_TRUE(WaitFor([&]() { return !_bank.instances.empty() && Instance(0).end.load() > 0; }));
	Audio::SetTimeSamples(voice, 480000);
	EXPECT_TRUE(WaitFor([&]() { return Instance(0).lastFrom.load() >= 480000 - GENERATOR_HISTORY_FRAMES; })) << "asked from " << Instance(0).lastFrom.load();
	EXPECT_LT(Instance(0).lastFrom.load(), 480000u + GENERATOR_WINDOW_FRAMES * 4u);
}

TEST_F(AudioGeneratorFixture, AssetUnloadedMidVoiceLeavesInstancePlaying) {
	EffectTestTone* tone = MakeTone(0.25f);
	const AudioVoice voice = PlayTone(tone, true);
	ASSERT_TRUE(PeakNear(CentredPeak(0.25f)));
	std::erase(_tones, tone);
	Delete(tone);
	RunFrame();
	ASSERT_TRUE(WaitForBlocks(4));
	EXPECT_TRUE(PeakNear(CentredPeak(0.25f)));
	EXPECT_EQ(_bank.counters.released.load(), 0u);

	Audio::Stop(voice);
	EXPECT_TRUE(WaitFor([&]() {
		RunFrame();
		return _bank.counters.released.load() == 1u;
	}));
	EXPECT_EQ(_bank.counters.violations.load(), 0u);
}

TEST_F(AudioGeneratorFixture, UnloadOrphansAssetResumes) {
	EffectTestTone* tone = MakeTone(0.25f);
	const AudioVoice voice = PlayTone(tone, true);
	ASSERT_TRUE(PeakNear(CentredPeak(0.25f)));

	Unloading(EffectTestTone::GetReflectType()->ownerModule);
	EXPECT_EQ(_bank.counters.released.load(), 1u);
	EXPECT_TRUE(Audio::IsPlaying(voice));
	EXPECT_TRUE(PeakNear(0.0f)) << LastBlockPeak();
	const u64 stopped = Instance(0).end.load();
	ASSERT_TRUE(WaitForBlocks(16));
	EXPECT_EQ(_bank.counters.violations.load(), 0u);

	RunFrame();
	ASSERT_EQ(_bank.counters.made.load(), 2u);
	EXPECT_TRUE(PeakNear(CentredPeak(0.25f))) << LastBlockPeak();
	ASSERT_TRUE(WaitFor([&]() { return Instance(1).firstFrom.load() != ~0ull; }));
	EXPECT_GT(Instance(1).firstFrom.load(), stopped);
}

TEST_F(AudioGeneratorFixture, UnloadWaitsForAudioThread) {
	EffectTestTone* tone = MakeTone(0.25f);
	ASSERT_TRUE(PlayTone(tone, true).IsValid());
	ASSERT_TRUE(PeakNear(CentredPeak(0.25f)));
	_bank.counters.stall.store(true);
	ASSERT_TRUE(WaitFor([&]() { return _bank.counters.inside.load(); }));

	Unloading(EffectTestTone::GetReflectType()->ownerModule);
	EXPECT_EQ(_bank.counters.released.load(), 1u);
	EXPECT_EQ(_bank.counters.releasedInside.load(), 0u);
}

TEST_F(AudioGeneratorFixture, RefusedGeneratorStartsNoVoice) {
	EffectTestTone* tone = MakeTone(0.25f);
	tone->refuse = true;
	EXPECT_FALSE(PlayTone(tone).IsValid());
	EXPECT_EQ(_bank.counters.made.load(), 0u);

	tone->refuse = false;
	tone->channels = 0;
	EXPECT_FALSE(PlayTone(tone).IsValid());
	tone->channels = AUDIO_MAX_CHANNELS + 1;
	EXPECT_FALSE(PlayTone(tone).IsValid());
	EXPECT_EQ(_bank.counters.made.load(), 2u);
	EXPECT_EQ(_bank.counters.released.load(), 2u);
	EXPECT_EQ(ClaimedVoices(), 0u);
}

#endif
