#include "Platform/Process.h"

#include <emscripten/emscripten.h>

namespace Shalltear::Platform {

	namespace {

		struct MainLoopContext {
			bool (*step)(void*) = nullptr;
			void (*onExit)(int, void*) = nullptr;
			void* user = nullptr;
		};

		void TickMainLoop(void* arg) {
			auto* ctx = static_cast<MainLoopContext*>(arg);
			if (!ctx || !ctx->step || !ctx->step(ctx->user)) {
				emscripten_cancel_main_loop();
				auto onExit = ctx ? ctx->onExit : nullptr;
				void* user = ctx ? ctx->user : nullptr;
				delete ctx;
				if (onExit) {
					onExit(0, user);
				}
			}
		}

	} // anonymous namespace

	str GetExecutablePath() {
		return "/app/player";
	}

	str GetUserDataDirectory() {
		return "/persist";
	}

	str GetUserPicturesDirectory() {
		return "/persist/Pictures";
	}

	bool RunProcessCapture(const str&, std::span<const str>, str& outOutput, i32& outExitCode) {
		outOutput.clear();
		outExitCode = -1;
		return false;
	}

	bool SupportsBackgroundThreads() {
#ifdef __EMSCRIPTEN_PTHREADS__
		return true;
#else
		return false;
#endif
	}

	bool IsExternallyPaced() {
		return true;
	}

	void RunMainLoop(bool (*step)(void*), void (*onExit)(int, void*), void* user) {
		auto* ctx = new MainLoopContext{step, onExit, user};
		emscripten_set_main_loop_arg(&TickMainLoop, ctx, 0, false);
	}

} // namespace Shalltear::Platform
