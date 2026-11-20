#include "TestsPCH.h"

#include "CSharpConformance.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Input.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Window.h"

namespace Shalltear::CSharpConformance {

	namespace {

		Handle<Window> _window;
		Vec2I _pointer{20, 20};

		i32 Drive(i32 op, i32 first, i32 second) {
			switch (op) {
			case 0:
				TestPlatform::PostKey(_window, static_cast<Key>(first), second != 0);
				return 0;
			case 1:
				TestPlatform::PostMouseButton(_window, static_cast<MouseButton>(first), second != 0, _pointer);
				return 0;
			case 2:
				_pointer = Vec2I{first, second};
				TestPlatform::PostMouseMove(_window, _pointer);
				return 0;
			case 3:
				TestPlatform::PostMouseWheel(_window, Vec2(static_cast<f32>(first), static_cast<f32>(second)), _pointer);
				return 0;
			case 4:
				TestPlatform::PostText(_window, static_cast<char32_t>(first));
				return 0;
			case 5:
				WindowSystem::PumpEvents();
				return 0;
			case 6: {
				FrameTime fixed;
				fixed.fixedDeltaTime = 1.0f / 60.0f;
				fixed.maximumDeltaTime = 1000.0f;
				const Time::FrameScope clock{fixed};
				Time::BeginFrame(fixed.fixedDeltaTime);
				for (i32 step = 0; step < first; ++step) {
					FrameStages::Run(FrameStage::FixedUpdate, FrameStageArgs{});
				}
				return 0;
			}
			default:
				return -1;
			}
		}

	} // anonymous namespace

	InputRig::InputRig(void (*setDriver)(DriverFn)) : _setDriver(setDriver) {
		WindowSystem::Initialize();
		WindowDesc desc;
		desc.title = "CSharpConformance";
		desc.clientSize = {320, 240};
		desc.flags = WindowFlags::Hidden;
		_window = WindowSystem::Create(desc);
		TestPlatform::PostFocus(_window, true);
		WindowSystem::PumpEvents();
		_ready = _window.IsValid() && WindowSystem::GetFocusedWindow().IsValid();
		_setDriver(&Drive);
	}

	InputRig::~InputRig() {
		_setDriver(nullptr);
		_window = {};
		WindowSystem::Terminate();
	}

} // namespace Shalltear::CSharpConformance
