#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	class SHALLTEAR_CORE_API FixedTimestep {
	public:
		explicit FixedTimestep(f64 stepSeconds, f64 maximumDeltaSeconds);

		void Configure(f64 stepSeconds, f64 maximumDeltaSeconds);

		u32 Advance(f64 frameDeltaSeconds);

		f32 GetAlpha() const;

		f64 GetStepSeconds() const { return _stepSeconds; }
		u32 GetMaxStepsPerFrame() const { return _maxStepsPerFrame; }

		void Reset();
	private:
		f64 _stepSeconds;
		f64 _accumulator = 0.0;
		u32 _maxStepsPerFrame;
	};

	class SHALLTEAR_CORE_API FramePacer {
	public:
		// unattended (no user to focus a window) -> never pauses
		explicit FramePacer(bool unattended = false) noexcept;

		// pumps window events. false while paused (caller should skip the frame)
		bool BeginFrame();

		f32 GetUnscaledDeltaTime() const noexcept { return _timer.GetUnscaledDeltaTime(); }
	private:
		FrameTimer _timer;
		u64 _anchorTicks = 0;
		bool _unattended;
		bool _paused = false;
	};

	// drives frame stepping (loops synchronously on desktop, yields to requestAnimationFrame on Web)
	SHALLTEAR_CORE_API void RunMainLoop(bool (*step)(void*), void (*onExit)(int, void*), void* user);

} // namespace Shalltear
