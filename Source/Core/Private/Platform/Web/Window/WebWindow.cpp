#include "Platform/Web/Window/WebKeyMap.h"

#include "Input/InputInternal.h"
#include "Window/WindowInternal.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Window/WindowEvents.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

namespace Shalltear {

	struct Window::Impl {
		Handle<Window> self;
		WindowFlags flags = WindowFlags::None;
		str title;
		Vec2I clientSize = {1280, 720};
		Vec2I renderSize{};
		f32 scale = 1.0f;
		SystemCursor cursor = SystemCursor::Arrow;
		CursorMode cursorMode = CursorMode::Normal;
		Window::HitTestFn hitTest = nullptr;
		void* hitTestUser = nullptr;
		bool vsyncHint = true;
		bool visible = true;
		bool focused = true;
		bool minimized = false;
		bool maximized = false;
		bool pointerLocked = false;
		Vec2I lastMousePx{};
	};

	namespace {

		constexpr const char* CANVAS_SELECTOR = "#canvas";

		Window::Impl* _activeWindow = nullptr;
		str _clipboardText;
		// DOM callbacks fire between requestAnimationFrame ticks, so queue and flush inside WindowBackend::Pump
		Vector<InputEvent> _pendingEvents{GetMemoryResource(MemTag::Platform)};
		Vec2 _pendingRawDelta{};
		Vec2I _pendingMouseScreenPos{};
		bool _hasPendingMouseScreenPos = false;

		void QueueInputEvent(const InputEvent& ev) {
			_pendingEvents.push_back(ev);
		}

		KeyMod ModsFromKeyboard(const EmscriptenKeyboardEvent& e) {
			KeyMod mods = KeyMod::None;
			if (e.ctrlKey) {
				mods |= KeyMod::Ctrl;
			}
			if (e.shiftKey) {
				mods |= KeyMod::Shift;
			}
			if (e.altKey) {
				mods |= KeyMod::Alt;
			}
			if (e.metaKey) {
				mods |= KeyMod::Super;
			}
			return mods;
		}

		KeyMod ModsFromMouse(const EmscriptenMouseEvent& e) {
			KeyMod mods = KeyMod::None;
			if (e.ctrlKey) {
				mods |= KeyMod::Ctrl;
			}
			if (e.shiftKey) {
				mods |= KeyMod::Shift;
			}
			if (e.altKey) {
				mods |= KeyMod::Alt;
			}
			if (e.metaKey) {
				mods |= KeyMod::Super;
			}
			return mods;
		}

		MouseButton ButtonFromWeb(unsigned short button) {
			switch (button) {
			case 0:
				return MouseButton::Left;
			case 1:
				return MouseButton::Middle;
			case 2:
				return MouseButton::Right;
			case 3:
				return MouseButton::X1;
			case 4:
				return MouseButton::X2;
			default:
				return MouseButton::Left;
			}
		}

		const char* CursorStyleName(SystemCursor cursor, CursorMode mode) {
			if (mode == CursorMode::Hidden || mode == CursorMode::Locked) {
				return "none";
			}
			switch (cursor) {
			case SystemCursor::IBeam:
				return "text";
			case SystemCursor::Crosshair:
				return "crosshair";
			case SystemCursor::Hand:
				return "pointer";
			case SystemCursor::ResizeEW:
				return "ew-resize";
			case SystemCursor::ResizeNS:
				return "ns-resize";
			case SystemCursor::ResizeNWSE:
				return "nwse-resize";
			case SystemCursor::ResizeNESW:
				return "nesw-resize";
			case SystemCursor::ResizeAll:
				return "move";
			case SystemCursor::NotAllowed:
				return "not-allowed";
			default:
				return "default";
			}
		}

		void ApplyCanvasCursor(const Window::Impl& impl) {
			const char* style = CursorStyleName(impl.cursor, impl.cursorMode);
			EM_ASM({
				var canvas = document.querySelector('#canvas');
				if (canvas) {
					canvas.style.cursor = UTF8ToString($0);
				} }, style);
		}

		bool DecodeSingleCodepoint(const char* text, char32_t& out) {
			if (!text || text[0] == '\0') {
				return false;
			}
			const u8 b0 = static_cast<u8>(text[0]);
			size_t len = 1;
			char32_t cp = 0;
			if (b0 < 0x80) {
				cp = b0;
			} else if ((b0 & 0xE0) == 0xC0) {
				len = 2;
				cp = b0 & 0x1F;
			} else if ((b0 & 0xF0) == 0xE0) {
				len = 3;
				cp = b0 & 0x0F;
			} else if ((b0 & 0xF8) == 0xF0) {
				len = 4;
				cp = b0 & 0x07;
			} else {
				return false;
			}
			for (size_t i = 1; i < len; ++i) {
				const u8 bi = static_cast<u8>(text[i]);
				if ((bi & 0xC0) != 0x80) {
					return false;
				}
				cp = (cp << 6) | (bi & 0x3F);
			}
			if (text[len] != '\0' || cp < 0x20 || cp == 0x7F) {
				return false;
			}
			out = cp;
			return true;
		}

		Vec2I QueryCanvasPhysicalSize(f32& outScale) {
			double dpr = emscripten_get_device_pixel_ratio();
			if (dpr <= 0.0) {
				dpr = 1.0;
			}
			outScale = static_cast<f32>(dpr);

			double cssWidth = 0.0;
			double cssHeight = 0.0;
			if (emscripten_get_element_css_size(CANVAS_SELECTOR, &cssWidth, &cssHeight) != EMSCRIPTEN_RESULT_SUCCESS ||
				cssWidth <= 0.0 || cssHeight <= 0.0) {
				int w = 0;
				int h = 0;
				emscripten_get_canvas_element_size(CANVAS_SELECTOR, &w, &h);
				return {std::max(1, w), std::max(1, h)};
			}
			const i32 physW = std::max(1, static_cast<i32>(std::lround(cssWidth * dpr)));
			const i32 physH = std::max(1, static_cast<i32>(std::lround(cssHeight * dpr)));
			return {physW, physH};
		}

		void SyncCanvasSize(Window::Impl& impl, bool publishEvents) {
			f32 newScale = 1.0f;
			const Vec2I newSize = QueryCanvasPhysicalSize(newScale);
			const bool scaleChanged = std::abs(newScale - impl.scale) > 0.001f;
			const bool sizeChanged = newSize.x != impl.clientSize.x || newSize.y != impl.clientSize.y;

			impl.scale = newScale;
			impl.clientSize = newSize;
			emscripten_set_canvas_element_size(CANVAS_SELECTOR, newSize.x, newSize.y);

			if (!WindowInternal::GetState().monitors.empty()) {
				MonitorInfo& mon = WindowInternal::GetState().monitors.front();
				mon.bounds = RectI{0, 0, newSize.x, newSize.y};
				mon.workArea = mon.bounds;
				mon.scale = newScale;
			}

			if (publishEvents) {
				if (scaleChanged) {
					WindowEvents::DPIChanged.Publish(EventBus::GetGlobal(),
													 WindowDPIChangedEvent{impl.self, newScale, RectI{0, 0, newSize.x, newSize.y}});
				}
				if (sizeChanged) {
					WindowEvents::Resized.Publish(EventBus::GetGlobal(),
												  WindowResizedEvent{impl.self, newSize, false});
				}
			}
		}

		bool ShouldPreventDefaultKey(Key key) {
			switch (key) {
			case Key::Space:
			case Key::Up:
			case Key::Down:
			case Key::Left:
			case Key::Right:
			case Key::Tab:
			case Key::Backspace:
			case Key::PageUp:
			case Key::PageDown:
				return true;
			default:
				return false;
			}
		}

		EM_BOOL OnKeyEvent(int eventType, const EmscriptenKeyboardEvent* e, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (!impl || !e) {
				return EM_FALSE;
			}
			const Key key = Web::KeyFromWebCode(e->code);
			const KeyMod mods = ModsFromKeyboard(*e);

			if (key != Key::Unknown) {
				InputEvent ev;
				ev.type = eventType == EMSCRIPTEN_EVENT_KEYDOWN ? InputEventType::KeyDown : InputEventType::KeyUp;
				ev.window = impl->self;
				ev.mods = mods;
				ev.key = key;
				ev.repeat = e->repeat != 0;
				QueueInputEvent(ev);
			}

			if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && !e->ctrlKey && !e->altKey && !e->metaKey) {
				char32_t cp = 0;
				if (DecodeSingleCodepoint(e->key, cp)) {
					InputEvent textEv;
					textEv.type = InputEventType::Text;
					textEv.window = impl->self;
					textEv.mods = mods;
					textEv.codepoint = cp;
					QueueInputEvent(textEv);
				}
			}

			return ShouldPreventDefaultKey(key) ? EM_TRUE : EM_FALSE;
		}

		Vec2I MousePosPhysical(const Window::Impl& impl, const EmscriptenMouseEvent& e) {
			const i32 x = static_cast<i32>(std::lround(static_cast<f64>(e.targetX) * impl.scale));
			const i32 y = static_cast<i32>(std::lround(static_cast<f64>(e.targetY) * impl.scale));
			return {x, y};
		}

		EM_BOOL OnMouseEvent(int eventType, const EmscriptenMouseEvent* e, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (!impl || !e) {
				return EM_FALSE;
			}
			const Vec2I pos = MousePosPhysical(*impl, *e);
			impl->lastMousePx = pos;
			_pendingMouseScreenPos = pos;
			_hasPendingMouseScreenPos = true;

			InputEvent ev;
			ev.window = impl->self;
			ev.mods = ModsFromMouse(*e);
			ev.mousePosPx = pos;

			switch (eventType) {
			case EMSCRIPTEN_EVENT_MOUSEMOVE:
				ev.type = InputEventType::MouseMove;
				if (impl->pointerLocked) {
					_pendingRawDelta.x += static_cast<f32>(e->movementX) * impl->scale;
					_pendingRawDelta.y += static_cast<f32>(e->movementY) * impl->scale;
				}
				QueueInputEvent(ev);
				return EM_FALSE;
			case EMSCRIPTEN_EVENT_MOUSEDOWN:
				ev.type = InputEventType::MouseDown;
				ev.button = ButtonFromWeb(e->button);
				QueueInputEvent(ev);
				return EM_TRUE;
			case EMSCRIPTEN_EVENT_MOUSEUP:
				ev.type = InputEventType::MouseUp;
				ev.button = ButtonFromWeb(e->button);
				QueueInputEvent(ev);
				return EM_FALSE;
			case EMSCRIPTEN_EVENT_MOUSEENTER:
				ev.type = InputEventType::MouseEnter;
				QueueInputEvent(ev);
				return EM_FALSE;
			case EMSCRIPTEN_EVENT_MOUSELEAVE:
				ev.type = InputEventType::MouseLeave;
				QueueInputEvent(ev);
				return EM_FALSE;
			default:
				return EM_FALSE;
			}
		}

		EM_BOOL OnWheelEvent(int, const EmscriptenWheelEvent* e, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (!impl || !e) {
				return EM_FALSE;
			}
			f64 scale = 1.0 / 100.0;
			if (e->deltaMode == DOM_DELTA_LINE) {
				scale = 1.0 / 3.0;
			} else if (e->deltaMode == DOM_DELTA_PAGE) {
				scale = 3.0;
			}

			InputEvent ev;
			ev.type = InputEventType::MouseWheel;
			ev.window = impl->self;
			ev.mods = ModsFromMouse(e->mouse);
			ev.mousePosPx = MousePosPhysical(*impl, e->mouse);
			ev.wheel = {static_cast<f32>(e->deltaX * scale), static_cast<f32>(-e->deltaY * scale)};
			QueueInputEvent(ev);
			return EM_TRUE;
		}

		EM_BOOL OnResizeEvent(int, const EmscriptenUiEvent*, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (impl) {
				SyncCanvasSize(*impl, true);
			}
			return EM_FALSE;
		}

		void SetFocusedState(Window::Impl& impl, bool focused) {
			if (impl.focused == focused) {
				return;
			}
			impl.focused = focused;
			WindowInternal::GetState().focused = focused ? impl.self : Handle<Window>{};
			if (!focused) {
				InputInternal::ReleaseHeldInput(impl.self, impl.lastMousePx);
			}
			WindowEvents::Focus.Publish(EventBus::GetGlobal(), WindowFocusEvent{impl.self, focused});
		}

		EM_BOOL OnFocusEvent(int eventType, const EmscriptenFocusEvent*, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (impl) {
				SetFocusedState(*impl, eventType == EMSCRIPTEN_EVENT_FOCUS);
			}
			return EM_FALSE;
		}

		EM_BOOL OnVisibilityEvent(int, const EmscriptenVisibilityChangeEvent* e, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (impl && e) {
				impl->minimized = e->hidden != 0;
				SetFocusedState(*impl, !e->hidden);
			}
			return EM_FALSE;
		}

		EM_BOOL OnPointerLockChange(int, const EmscriptenPointerlockChangeEvent* e, void* userData) {
			auto* impl = static_cast<Window::Impl*>(userData);
			if (impl && e) {
				impl->pointerLocked = e->isActive != 0;
				InputInternal::SetRawDeltaActive(impl->pointerLocked);
			}
			return EM_FALSE;
		}

		void RegisterCallbacks(Window::Impl* impl) {
			emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnKeyEvent);
			emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnKeyEvent);
			emscripten_set_mousedown_callback(CANVAS_SELECTOR, impl, true, &OnMouseEvent);
			emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnMouseEvent);
			emscripten_set_mousemove_callback(CANVAS_SELECTOR, impl, true, &OnMouseEvent);
			emscripten_set_mouseenter_callback(CANVAS_SELECTOR, impl, true, &OnMouseEvent);
			emscripten_set_mouseleave_callback(CANVAS_SELECTOR, impl, true, &OnMouseEvent);
			emscripten_set_wheel_callback(CANVAS_SELECTOR, impl, true, &OnWheelEvent);
			emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnResizeEvent);
			emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnFocusEvent);
			emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl, true, &OnFocusEvent);
			emscripten_set_visibilitychange_callback(impl, true, &OnVisibilityEvent);
			emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, impl, true, &OnPointerLockChange);
		}

		void UnregisterCallbacks() {
			emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_mousedown_callback(CANVAS_SELECTOR, nullptr, false, nullptr);
			emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_mousemove_callback(CANVAS_SELECTOR, nullptr, false, nullptr);
			emscripten_set_mouseenter_callback(CANVAS_SELECTOR, nullptr, false, nullptr);
			emscripten_set_mouseleave_callback(CANVAS_SELECTOR, nullptr, false, nullptr);
			emscripten_set_wheel_callback(CANVAS_SELECTOR, nullptr, false, nullptr);
			emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_visibilitychange_callback(nullptr, false, nullptr);
			emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, false, nullptr);
		}

	} // anonymous namespace

	namespace WindowBackend {

		void Initialize() {
		}

		void Terminate() {
			_activeWindow = nullptr;
			_clipboardText.clear();
			_pendingEvents.clear();
			_pendingRawDelta = {};
			_hasPendingMouseScreenPos = false;
		}

		bool CreateNative(Window& window, const WindowDesc& desc) {
			auto* impl = New<Window::Impl>(MemTag::Platform);
			impl->self = Handle<Window>(&window);
			impl->flags = desc.flags;
			impl->title = desc.title;
			impl->visible = !HasFlag(desc.flags, WindowFlags::Hidden);
			WindowInternalAccess::GetImpl(window) = impl;

			SyncCanvasSize(*impl, false);
			if (!desc.title.empty()) {
				emscripten_set_window_title(desc.title.c_str());
			}
			ApplyCanvasCursor(*impl);

			EmscriptenVisibilityChangeEvent vis{};
			if (emscripten_get_visibility_status(&vis) == EMSCRIPTEN_RESULT_SUCCESS && vis.hidden) {
				impl->focused = false;
				impl->minimized = true;
			} else {
				impl->focused = true;
				WindowInternal::GetState().focused = impl->self;
			}

			_activeWindow = impl;
			RegisterCallbacks(impl);
			return true;
		}

		void DestroyNative(Window& window) {
			Window::Impl* impl = WindowInternalAccess::GetImpl(window);
			if (!impl) {
				return;
			}
			if (_activeWindow == impl) {
				UnregisterCallbacks();
				_activeWindow = nullptr;
			}
			if (impl->pointerLocked) {
				emscripten_exit_pointerlock();
				InputInternal::SetRawDeltaActive(false);
			}
			WindowInternalAccess::GetImpl(window) = nullptr;
			Delete(impl);
		}

		void Pump() {
			if (_activeWindow) {
				f32 scale = 1.0f;
				const Vec2I size = QueryCanvasPhysicalSize(scale);
				if (size.x != _activeWindow->clientSize.x || size.y != _activeWindow->clientSize.y ||
					std::abs(scale - _activeWindow->scale) > 0.001f) {
					SyncCanvasSize(*_activeWindow, true);
				}
			}
			if (_hasPendingMouseScreenPos) {
				InputInternal::SetMouseScreenPosition(_pendingMouseScreenPos);
				_hasPendingMouseScreenPos = false;
			}
			if (_pendingRawDelta.x != 0.0f || _pendingRawDelta.y != 0.0f) {
				InputInternal::AddRawMouseDelta(_pendingRawDelta);
				_pendingRawDelta = {};
			}
			for (const InputEvent& ev : _pendingEvents) {
				InputInternal::PushEvent(ev);
			}
			_pendingEvents.clear();
		}

		void Wait(f64) {
		}

		void Sleep(f64) {
		}

		void EnumerateMonitors(Vector<MonitorInfo>& outMonitors) {
			outMonitors.clear();
			f32 scale = 1.0f;
			const Vec2I size = QueryCanvasPhysicalSize(scale);
			MonitorInfo info;
			info.id = 1;
			info.name = "Web Canvas";
			info.bounds = RectI{0, 0, size.x, size.y};
			info.workArea = info.bounds;
			info.scale = scale;
			info.primary = true;
			info.refreshRate = 60.0f;
			outMonitors.push_back(std::move(info));
		}

		void EnumerateDisplayModes(u64, Vector<WindowInternal::DisplayMode>& outModes) {
			outModes.clear();
			f32 scale = 1.0f;
			outModes.push_back(WindowInternal::DisplayMode{QueryCanvasPhysicalSize(scale), 60.0f});
		}

		bool SetDisplayMode(u64, Vec2I, f32, bool) {
			return false;
		}

		void RestoreDisplayMode(u64) {
		}

		Handle<Window> WindowAtPoint(Vec2I, Handle<Window> ignore) {
			if (_activeWindow && _activeWindow->self != ignore) {
				return _activeWindow->self;
			}
			return {};
		}

		bool SetClipboardText(std::string_view utf8) {
			_clipboardText = str(utf8);
			EM_ASM({
				if (navigator.clipboard && navigator.clipboard.writeText) {
					navigator.clipboard.writeText(UTF8ToString($0, $1)).catch(function() {});
				} }, utf8.data(), static_cast<int>(utf8.size()));
			return true;
		}

		str GetClipboardText() {
			return _clipboardText;
		}

	} // namespace WindowBackend

#define SLT_WINDOW_ASSERT_MAIN_THREAD() \
	SLT_CORE_ASSERT_MSG(WindowInternal::IsMainThread(), "Window mutators are main-thread-only")

	Vec2I Window::GetClientSize() const {
		return _impl ? _impl->clientSize : Vec2I{};
	}

	void Window::SetClientSize(Vec2I sizePx) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (!_impl || sizePx.x <= 0 || sizePx.y <= 0) {
			return;
		}
		_impl->clientSize = sizePx;
		emscripten_set_canvas_element_size(CANVAS_SELECTOR, sizePx.x, sizePx.y);
	}

	Vec2I Window::GetPosition() const {
		return {0, 0};
	}

	void Window::SetPosition(Vec2I) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
	}

	RectI Window::GetRect() const {
		return _impl ? RectI{0, 0, _impl->clientSize.x, _impl->clientSize.y} : RectI{};
	}

	f32 Window::GetContentScale() const {
		return _impl ? _impl->scale : 1.0f;
	}

	Vec2I Window::ClientToScreen(Vec2I clientPx) const {
		return clientPx;
	}

	Vec2I Window::ScreenToClient(Vec2I screenPx) const {
		return screenPx;
	}

	const MonitorInfo& Window::GetMonitor() const {
		static const MonitorInfo FALLBACK{};
		const Vector<MonitorInfo>& monitors = WindowInternal::GetState().monitors;
		return monitors.empty() ? FALLBACK : monitors.front();
	}

	WindowPlacement Window::GetPlacement() const {
		WindowPlacement placement;
		if (_impl) {
			placement.clientSize = _impl->clientSize;
			placement.state = _impl->minimized	 ? WindowState::Minimized
							  : _impl->maximized ? WindowState::Maximized
												 : WindowState::Normal;
		}
		return placement;
	}

	void Window::SetPlacement(const WindowPlacement& placement) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (placement.clientSize.x > 0 && placement.clientSize.y > 0) {
			SetClientSize(placement.clientSize);
		}
	}

	void Window::Show() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (_impl) {
			_impl->visible = true;
		}
	}

	void Window::Hide() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (_impl) {
			_impl->visible = false;
		}
	}

	void Window::Minimize() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
	}

	void Window::Maximize() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (_impl) {
			_impl->maximized = true;
		}
	}

	void Window::Restore() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (_impl) {
			_impl->minimized = false;
			_impl->maximized = false;
		}
	}

	void Window::Focus() {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		EM_ASM({
			var canvas = document.querySelector('#canvas');
			if (canvas && canvas.focus) {
				canvas.focus();
			}
		});
	}

	bool Window::IsVisible() const {
		return _impl && _impl->visible;
	}

	bool Window::IsFocused() const {
		return _impl && _impl->focused;
	}

	bool Window::IsMinimized() const {
		return _impl && _impl->minimized;
	}

	bool Window::IsMaximized() const {
		return _impl && _impl->maximized;
	}

	void Window::SetTitle(std::string_view utf8) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (!_impl) {
			return;
		}
		_impl->title = str(utf8);
		emscripten_set_window_title(_impl->title.c_str());
	}

	str Window::GetTitle() const {
		return _impl ? _impl->title : str{};
	}

	void Window::SetIcon(std::span<const WindowIconImage>) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
	}

	WindowFlags Window::GetFlags() const {
		return _impl ? _impl->flags : WindowFlags::None;
	}

	void Window::SetCursor(SystemCursor cursor) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (!_impl || cursor >= SystemCursor::Count) {
			return;
		}
		_impl->cursor = cursor;
		ApplyCanvasCursor(*_impl);
	}

	SystemCursor Window::GetCursor() const {
		return _impl ? _impl->cursor : SystemCursor::Arrow;
	}

	void Window::SetCursorMode(CursorMode mode) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (!_impl || _impl->cursorMode == mode) {
			return;
		}
		const CursorMode previous = _impl->cursorMode;
		_impl->cursorMode = mode;
		if (previous == CursorMode::Locked && mode != CursorMode::Locked) {
			emscripten_exit_pointerlock();
			_impl->pointerLocked = false;
			InputInternal::SetRawDeltaActive(false);
		} else if (mode == CursorMode::Locked) {
			emscripten_request_pointerlock(CANVAS_SELECTOR, EM_TRUE);
		}
		ApplyCanvasCursor(*_impl);
	}

	CursorMode Window::GetCursorMode() const {
		return _impl ? _impl->cursorMode : CursorMode::Normal;
	}

	void Window::SetHitTestCallback(HitTestFn fn, void* user) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (_impl) {
			_impl->hitTest = fn;
			_impl->hitTestUser = user;
		}
	}

	void WindowInternal::SetRenderSize(Window& window, Vec2I sizePx) {
		SLT_WINDOW_ASSERT_MAIN_THREAD();
		if (Window::Impl* impl = WindowInternalAccess::GetImpl(window)) {
			impl->renderSize = sizePx.x > 0 && sizePx.y > 0 ? sizePx : Vec2I{};
		}
	}

	Vec2I WindowInternal::GetRenderSizeOverride(const Window& window) {
		const Window::Impl* impl = WindowInternalAccess::GetImpl(window);
		return impl ? impl->renderSize : Vec2I{};
	}

	NativeWindowHandle Window::GetNativeHandle() const {
		return NativeWindowHandle{NativeWindowSystem::WebCanvas, nullptr, const_cast<char*>(CANVAS_SELECTOR)};
	}

	void Window::SetVSyncHint(bool on) {
		if (_impl) {
			_impl->vsyncHint = on;
		}
	}

	bool Window::GetVSyncHint() const {
		return _impl && _impl->vsyncHint;
	}

#undef SLT_WINDOW_ASSERT_MAIN_THREAD

} // namespace Shalltear
