#pragma once

#include "Shalltear/Audio/AudioEffect.h"
#include "Shalltear/Audio/AudioGenerator.h"
#include "Shalltear/Audio/AudioSpatializer.h"

#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear::CSharpImpl::AudioProbe {

	f32 RunEffect(IAudioEffectDefinition& definition, u32 channels, u32 frames, u32 blocks, bool sidechain);

	f32 RunGenerator(IAudioGeneratorInstance& instance, u32 frames, u32 blocks);

	f32 RunSpatializer(IAudioSpatializer& spatializer, f32 attenuation, u32 frames, u32 blocks);

	bool StartEffectLoop(IAudioEffectDefinition& definition, u32 frames);

	// stats = calls, slowest call ns, total ns
	void StopEffectLoop(std::span<u64> stats);

} // namespace Shalltear::CSharpImpl::AudioProbe
