#include "Shalltear/Core/Window/FrameLoop.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Window/Window.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include "Graphics/PresentPolicy.h"
#include "Platform/Process.h"
#include "ScreenInternal.h"
#include "Window/FramePacing.h"
#include "Window/WindowInternal.h"

#include <algorithm>

namespace Shalltear {

	namespace FramePacingInternal {

		Pacing ResolvePacing(const PacingInputs& inputs) {
			Pacing pacing;
			if (!inputs.focused && !inputs.runInBackground) {
				pacing.paused = true;
				return pacing;
			}
			if (inputs.vsyncWindowsPresented == 0 && inputs.targetFrameRate > 0) {
				pacing.intervalSeconds = 1.0 / static_cast<f64>(inputs.targetFrameRate);
			}
			if (!inputs.focused && inputs.backgroundFrameRate > 0) {
				pacing.intervalSeconds = std::max(pacing.intervalSeconds, 1.0 / static_cast<f64>(inputs.backgroundFrameRate));
			}
			if (inputs.windowsPresented == 0) {
				const f32 hz = inputs.refreshRate > 0.0f ? inputs.refreshRate : FALLBACK_REFRESH_RATE;
				pacing.intervalSeconds = std::max(pacing.intervalSeconds, 1.0 / static_cast<f64>(hz));
			}
			return pacing;
		}

	} // namespace FramePacingInternal

	FramePacer::FramePacer(bool unattended) noexcept : _unattended(unattended) {}

	bool FramePacer::BeginFrame() {
		const Handle<Window> focused = WindowSystem::GetFocusedWindow();
		const GraphicsInternal::PresentCounts presents = GraphicsInternal::GetLastFramePresents();
		FramePacingInternal::PacingInputs inputs;
		inputs.targetFrameRate = Time::GetTargetFrameRate();
		inputs.windowsPresented = presents.windows;
		inputs.vsyncWindowsPresented = presents.vsyncWindows;
		inputs.focused = focused.IsValid();
		inputs.runInBackground = _unattended || ScreenInternal::RunsInBackground();
		inputs.backgroundFrameRate = _unattended ? 0 : ScreenInternal::BackgroundFrameRate();
		if (const Window* window = (inputs.focused ? focused : WindowSystem::GetPrimaryWindow()).Get()) {
			inputs.refreshRate = window->GetMonitor().refreshRate;
		}
		const FramePacingInternal::Pacing pacing = FramePacingInternal::ResolvePacing(inputs);

		const bool externallyPaced = Platform::IsExternallyPaced();

		if (pacing.paused) {
			if (!_paused) {
				_paused = true;
				SLT_LOG_INFO("Screen", "paused: game window doesn't have focus (Run In Background is off)");
			}
			if (externallyPaced) {
				WindowSystem::PumpEvents();
			} else {
				WindowSystem::WaitEvents(FramePacingInternal::PAUSED_WAIT_SECONDS);
			}
			return false;
		}
		const bool resumed = _paused;
		if (resumed) {
			_paused = false;
			_anchorTicks = 0;
			SLT_LOG_INFO("Screen", "resumed: game window has focus");
		}

		if (!externallyPaced) {
			const u64 frequency = Time::GetTicksPerSecond();
			const u64 interval = static_cast<u64>(pacing.intervalSeconds * static_cast<f64>(frequency));
			u64 now = Time::GetTicks();
			if (interval > 0 && _anchorTicks != 0) {
				const u64 due = _anchorTicks + interval;
				while (now < due) {
					WindowBackend::Sleep(static_cast<f64>(due - now) / static_cast<f64>(frequency));
					now = Time::GetTicks();
				}
				_anchorTicks = now - due < interval ? due : now;
			} else {
				_anchorTicks = now;
			}
		}

		WindowSystem::PumpEvents();
		if (resumed) {
			_timer = FrameTimer();
		}
		_timer.Tick();
		return true;
	}

	void RunMainLoop(bool (*step)(void*), void (*onExit)(int, void*), void* user) {
		Platform::RunMainLoop(step, onExit, user);
	}

} // namespace Shalltear
