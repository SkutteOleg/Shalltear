#ifdef SLT_CSHARP_TEST_HOOKS

#include "AudioProbe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace Shalltear::CSharpImpl::AudioProbe {

	namespace {

		constexpr u32 SAMPLE_RATE = 48000;

		f32 Average(const std::vector<f32>& samples, f64 sum, u32 blocks) {
			return samples.empty() || blocks == 0 ? 0.0f : static_cast<f32>(sum / (static_cast<f64>(samples.size()) * blocks));
		}

		IAudioEffect* MakeEffect(IAudioEffectDefinition& definition, u32 channels, u32 frames) {
			AudioEffectContext context;
			context.sampleRate = SAMPLE_RATE;
			context.blockFrames = frames;
			context.channels = channels;
			return definition.CreateEffect(context);
		}

		struct Loop {
			std::thread worker;
			std::atomic<bool> running{false};
			IAudioEffect* effect = nullptr;
			u64 calls = 0;
			u64 slowest = 0;
			u64 total = 0;
		};

		Loop _loop;

	} // anonymous namespace

	f32 RunEffect(IAudioEffectDefinition& definition, u32 channels, u32 frames, u32 blocks, bool sidechain) {
		IAudioEffect* const effect = MakeEffect(definition, channels, frames);
		if (!effect) {
			return -1.0f;
		}
		std::vector<f32> samples(static_cast<size_t>(channels) * frames);
		std::vector<f32> side(samples.size(), 0.25f);
		f64 sum = 0.0;
		std::thread worker([&] {
			for (u32 index = 0; index < blocks; ++index) {
				std::fill(samples.begin(), samples.end(), 1.0f);
				AudioBlock block{samples.data(), channels, frames, frames};
				const AudioBlock key{side.data(), channels, frames, frames};
				effect->Process(block, sidechain ? &key : nullptr);
				for (const f32 sample : samples) {
					sum += sample;
				}
			}
		});
		worker.join();
		effect->Release();
		return Average(samples, sum, blocks);
	}

	f32 RunGenerator(IAudioGeneratorInstance& instance, u32 frames, u32 blocks) {
		std::vector<f32> samples(frames);
		f64 sum = 0.0;
		std::thread worker([&] {
			for (u32 index = 0; index < blocks; ++index) {
				std::fill(samples.begin(), samples.end(), 0.0f);
				AudioBlock block{samples.data(), 1, frames, frames};
				const u32 made = instance.Process(block, static_cast<u64>(index) * frames);
				for (u32 frame = 0; frame < made && frame < frames; ++frame) {
					sum += samples[frame];
				}
			}
		});
		worker.join();
		return Average(samples, sum, blocks);
	}

	f32 RunSpatializer(IAudioSpatializer& spatializer, f32 attenuation, u32 frames, u32 blocks) {
		AudioSpatializerContext context;
		context.sampleRate = SAMPLE_RATE;
		context.blockFrames = frames;
		context.inputChannels = 1;
		context.outputChannels = 2;
		void* const state = spatializer.CreateVoiceState(context);
		std::vector<f32> input(frames, 1.0f);
		std::vector<f32> output(static_cast<size_t>(frames) * 2);
		f64 sum = 0.0;
		std::thread worker([&] {
			AudioSpatialParams params;
			params.distanceAttenuation = attenuation;
			for (u32 index = 0; index < blocks; ++index) {
				std::fill(output.begin(), output.end(), 0.0f);
				const AudioBlock in{input.data(), 1, frames, frames};
				AudioBlock out{output.data(), 2, frames, frames};
				spatializer.Process(state, params, in, out);
				for (const f32 sample : output) {
					sum += sample;
				}
			}
		});
		worker.join();
		spatializer.DestroyVoiceState(state);
		return Average(output, sum, blocks);
	}

	bool StartEffectLoop(IAudioEffectDefinition& definition, u32 frames) {
		if (_loop.running.load()) {
			return false;
		}
		_loop.effect = MakeEffect(definition, 1, frames);
		if (!_loop.effect) {
			return false;
		}
		_loop.calls = _loop.slowest = _loop.total = 0;
		_loop.running.store(true);
		_loop.worker = std::thread([frames] {
			std::vector<f32> samples(frames);
			while (_loop.running.load()) {
				std::fill(samples.begin(), samples.end(), 1.0f);
				AudioBlock block{samples.data(), 1, frames, frames};
				const auto begin = std::chrono::steady_clock::now();
				_loop.effect->Process(block, nullptr);
				const u64 spent = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
				++_loop.calls;
				_loop.total += spent;
				_loop.slowest = std::max(_loop.slowest, spent);
				std::this_thread::yield();
			}
		});
		return true;
	}

	void StopEffectLoop(std::span<u64> stats) {
		if (!_loop.running.exchange(false)) {
			return;
		}
		_loop.worker.join();
		_loop.effect->Release();
		_loop.effect = nullptr;
		const u64 values[] = {_loop.calls, _loop.slowest, _loop.total};
		for (size_t index = 0; index < stats.size() && index < std::size(values); ++index) {
			stats[index] = values[index];
		}
	}

} // namespace Shalltear::CSharpImpl::AudioProbe

#endif
