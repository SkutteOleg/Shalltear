#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/NativeWindow.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

extern char** environ;

namespace TestPlatform {

	using namespace Shalltear;

	namespace {

		struct XlibAPI {
			void* library = nullptr;
			int (*sendEvent)(Display*, ::Window, int, long, XEvent*) = nullptr;
			int (*flush)(Display*) = nullptr;
			int (*sync)(Display*, int) = nullptr;
			KeyCode (*keysymToKeycode)(Display*, KeySym) = nullptr;
			Atom (*internAtom)(Display*, const char*, int) = nullptr;
			int (*displayKeycodes)(Display*, int*, int*) = nullptr;
			KeySym* (*getKeyboardMapping)(Display*, KeyCode, int, int*) = nullptr;
			int (*changeKeyboardMapping)(Display*, int, int, KeySym*, int) = nullptr;
			int (*freeData)(void*) = nullptr;
			int (*translateCoordinates)(Display*, ::Window, ::Window, int, int, int*, int*, ::Window*) = nullptr;
			Display* (*openDisplay)(const char*) = nullptr;
			int (*closeDisplay)(Display*) = nullptr;
			int (*queryTree)(Display*, ::Window, ::Window*, ::Window*, ::Window**, unsigned int*) = nullptr;
			int (*getWindowProperty)(Display*, ::Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*, unsigned long*,
									 unsigned char**) = nullptr;
			int (*getWindowAttributes)(Display*, ::Window, XWindowAttributes*) = nullptr;
		};

		template <typename TFunction>
		void Bind(void* library, const char* name, TFunction& out) {
			out = reinterpret_cast<TFunction>(::dlsym(library, name));
		}

		const XlibAPI* GetX() {
			static XlibAPI api;
			static bool tried = false;
			if (tried) {
				return api.library ? &api : nullptr;
			}
			tried = true;
			void* library = ::dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
			if (!library) {
				return nullptr;
			}
			Bind(library, "XSendEvent", api.sendEvent);
			Bind(library, "XFlush", api.flush);
			Bind(library, "XSync", api.sync);
			Bind(library, "XKeysymToKeycode", api.keysymToKeycode);
			Bind(library, "XInternAtom", api.internAtom);
			Bind(library, "XDisplayKeycodes", api.displayKeycodes);
			Bind(library, "XGetKeyboardMapping", api.getKeyboardMapping);
			Bind(library, "XChangeKeyboardMapping", api.changeKeyboardMapping);
			Bind(library, "XFree", api.freeData);
			Bind(library, "XTranslateCoordinates", api.translateCoordinates);
			Bind(library, "XOpenDisplay", api.openDisplay);
			Bind(library, "XCloseDisplay", api.closeDisplay);
			Bind(library, "XQueryTree", api.queryTree);
			Bind(library, "XGetWindowProperty", api.getWindowProperty);
			Bind(library, "XGetWindowAttributes", api.getWindowAttributes);
			api.library = library;
			return &api;
		}

		struct Target {
			const XlibAPI* x = nullptr;
			Display* display = nullptr;
			::Window window = 0;
			::Window root = 0;
		};

		void ListenForTestEnd();

		bool Resolve(Handle<Window> window, Target& out) {
			if (!window.IsValid()) {
				ADD_FAILURE() << "posting to a dead window";
				return false;
			}
			const NativeWindowHandle native = window->GetNativeHandle();
			if (native.system != NativeWindowSystem::Xlib) {
				ADD_FAILURE() << "posted input needs an X11 window";
				return false;
			}
			out.x = GetX();
			if (!out.x) {
				ADD_FAILURE() << "libX11.so.6 didn't load";
				return false;
			}
			out.display = static_cast<Display*>(native.display);
			out.window = static_cast<::Window>(reinterpret_cast<uintptr_t>(native.window));
			out.root = DefaultRootWindow(out.display);
			ListenForTestEnd();
			return true;
		}

		bool Between(Key key, Key first, Key last, u32& outIndex) {
			if (key < first || key > last) {
				return false;
			}
			outIndex = static_cast<u32>(key) - static_cast<u32>(first);
			return true;
		}

		KeySym ToKeysym(Key key) {
			u32 index = 0;
			if (Between(key, Key::A, Key::Z, index)) {
				return XK_a + index;
			}
			if (Between(key, Key::D0, Key::D9, index)) {
				return XK_0 + index;
			}
			if (Between(key, Key::F1, Key::F24, index)) {
				return XK_F1 + index;
			}
			if (Between(key, Key::Kp0, Key::Kp9, index)) {
				return XK_KP_0 + index;
			}
			switch (key) {
			case Key::KpDecimal:
				return XK_KP_Decimal;
			case Key::KpDivide:
				return XK_KP_Divide;
			case Key::KpMultiply:
				return XK_KP_Multiply;
			case Key::KpSubtract:
				return XK_KP_Subtract;
			case Key::KpAdd:
				return XK_KP_Add;
			case Key::KpEnter:
				return XK_KP_Enter;
			case Key::KpEqual:
				return XK_KP_Equal;
			case Key::Escape:
				return XK_Escape;
			case Key::Enter:
				return XK_Return;
			case Key::Tab:
				return XK_Tab;
			case Key::Backspace:
				return XK_BackSpace;
			case Key::Insert:
				return XK_Insert;
			case Key::Delete:
				return XK_Delete;
			case Key::Home:
				return XK_Home;
			case Key::End:
				return XK_End;
			case Key::PageUp:
				return XK_Page_Up;
			case Key::PageDown:
				return XK_Page_Down;
			case Key::Left:
				return XK_Left;
			case Key::Right:
				return XK_Right;
			case Key::Up:
				return XK_Up;
			case Key::Down:
				return XK_Down;
			case Key::Space:
				return XK_space;
			case Key::CapsLock:
				return XK_Caps_Lock;
			case Key::ScrollLock:
				return XK_Scroll_Lock;
			case Key::NumLock:
				return XK_Num_Lock;
			case Key::PrintScreen:
				return XK_Print;
			case Key::Pause:
				return XK_Pause;
			case Key::Menu:
				return XK_Menu;
			case Key::LeftShift:
				return XK_Shift_L;
			case Key::RightShift:
				return XK_Shift_R;
			case Key::LeftCtrl:
				return XK_Control_L;
			case Key::RightCtrl:
				return XK_Control_R;
			case Key::LeftAlt:
				return XK_Alt_L;
			case Key::RightAlt:
				return XK_Alt_R;
			case Key::LeftSuper:
				return XK_Super_L;
			case Key::RightSuper:
				return XK_Super_R;
			case Key::Apostrophe:
				return XK_apostrophe;
			case Key::Comma:
				return XK_comma;
			case Key::Minus:
				return XK_minus;
			case Key::Period:
				return XK_period;
			case Key::Slash:
				return XK_slash;
			case Key::Semicolon:
				return XK_semicolon;
			case Key::Equal:
				return XK_equal;
			case Key::LeftBracket:
				return XK_bracketleft;
			case Key::Backslash:
				return XK_backslash;
			case Key::RightBracket:
				return XK_bracketright;
			case Key::GraveAccent:
				return XK_grave;
			default:
				return 0;
			}
		}

		unsigned int ModifierBit(Key key) {
			switch (key) {
			case Key::LeftShift:
			case Key::RightShift:
				return ShiftMask;
			case Key::LeftCtrl:
			case Key::RightCtrl:
				return ControlMask;
			case Key::LeftAlt:
			case Key::RightAlt:
				return Mod1Mask;
			case Key::LeftSuper:
			case Key::RightSuper:
				return Mod4Mask;
			default:
				return 0;
			}
		}

		unsigned int _heldModifiers = 0;
		unsigned int _heldButtons = 0;
		bool _heldKeycodes[256] = {};

		void Send(const Target& target, long mask, XEvent& event) {
			target.x->sendEvent(target.display, target.window, 0, mask, &event);
			target.x->sync(target.display, 0);
		}

		void SendKeycode(const Target& target, KeyCode keycode, bool down, unsigned int state) {
			XEvent event{};
			event.xkey.type = down ? KeyPress : KeyRelease;
			event.xkey.display = target.display;
			event.xkey.window = target.window;
			event.xkey.root = target.root;
			event.xkey.time = CurrentTime;
			event.xkey.state = state;
			event.xkey.keycode = keycode;
			event.xkey.same_screen = 1;
			Send(target, down ? KeyPressMask : KeyReleaseMask, event);
		}

		Vec2I ToRoot(const Target& target, Vec2I clientPx) {
			Vec2I rootPx = clientPx;
			::Window child = 0;
			target.x->translateCoordinates(target.display, target.window, target.root, clientPx.x, clientPx.y, &rootPx.x, &rootPx.y,
										   &child);
			return rootPx;
		}

		void SendButton(const Target& target, unsigned int button, bool down, Vec2I clientPx) {
			const Vec2I rootPx = ToRoot(target, clientPx);
			XEvent event{};
			event.xbutton.type = down ? ButtonPress : ButtonRelease;
			event.xbutton.display = target.display;
			event.xbutton.window = target.window;
			event.xbutton.root = target.root;
			event.xbutton.time = CurrentTime;
			event.xbutton.x = clientPx.x;
			event.xbutton.y = clientPx.y;
			event.xbutton.x_root = rootPx.x;
			event.xbutton.y_root = rootPx.y;
			event.xbutton.state = _heldModifiers | _heldButtons;
			event.xbutton.button = button;
			event.xbutton.same_screen = 1;
			Send(target, down ? ButtonPressMask : ButtonReleaseMask, event);
		}

		void SendFocus(Handle<Window> window, bool focused) {
			Target target;
			if (!Resolve(window, target)) {
				return;
			}
			XEvent event{};
			event.xfocus.type = focused ? FocusIn : FocusOut;
			event.xfocus.display = target.display;
			event.xfocus.window = target.window;
			event.xfocus.mode = NotifyNormal;
			event.xfocus.detail = NotifyNonlinear;
			Send(target, FocusChangeMask, event);
		}

		struct TextKey {
			KeyCode keycode = 0;
			KeySym keysym = NoSymbol;
		};
		constexpr int TEXT_LEVELS = 2;
		std::vector<TextKey> _textKeys;
		size_t _nextTextKey = 0;
		Display* _keymapDisplay = nullptr;
		char32_t _highSurrogate = 0;

		void ForgetPosted() {
			_heldModifiers = 0;
			_heldButtons = 0;
			std::memset(_heldKeycodes, 0, sizeof(_heldKeycodes));
			_highSurrogate = 0;
			_nextTextKey = 0;
			if (!_keymapDisplay) {
				return;
			}
			const XlibAPI* x = GetX();
			KeySym none[TEXT_LEVELS] = {};
			for (TextKey& key : _textKeys) {
				if (key.keysym != NoSymbol) {
					x->changeKeyboardMapping(_keymapDisplay, key.keycode, TEXT_LEVELS, none, 1);
					key.keysym = NoSymbol;
				}
			}
			x->sync(_keymapDisplay, 0);
		}

		class PostedStateListener final : public testing::EmptyTestEventListener {
		public:
			void OnTestEnd(const testing::TestInfo&) override { ForgetPosted(); }

			void OnTestProgramEnd(const testing::UnitTest&) override {
				if (_keymapDisplay) {
					GetX()->closeDisplay(_keymapDisplay);
					_keymapDisplay = nullptr;
				}
			}
		};

		void ListenForTestEnd() {
			static bool listening = false;
			if (!listening) {
				listening = true;
				testing::UnitTest::GetInstance()->listeners().Append(new PostedStateListener);
			}
		}

		void CollectTextKeys(const XlibAPI& x) {
			constexpr size_t MAX_TEXT_KEYS = 64;
			int first = 0;
			int last = 0;
			x.displayKeycodes(_keymapDisplay, &first, &last);
			int perKeycode = 0;
			KeySym* map = x.getKeyboardMapping(_keymapDisplay, static_cast<KeyCode>(first), last - first + 1, &perKeycode);
			if (!map) {
				return;
			}
			for (int keycode = last; keycode >= first && _textKeys.size() < MAX_TEXT_KEYS; --keycode) {
				bool empty = true;
				for (int level = 0; level < perKeycode; ++level) {
					empty = empty && map[(keycode - first) * perKeycode + level] == NoSymbol;
				}
				if (empty) {
					_textKeys.push_back({static_cast<KeyCode>(keycode), NoSymbol});
				}
			}
			x.freeData(map);
		}

		KeyCode KeycodeForText(const Target& target, KeySym keysym) {
			if (!_keymapDisplay) {
				_keymapDisplay = target.x->openDisplay(DisplayString(target.display));
				if (!_keymapDisplay) {
					return 0;
				}
				CollectTextKeys(*target.x);
			}
			for (const TextKey& key : _textKeys) {
				if (key.keysym == keysym) {
					return key.keycode;
				}
			}
			if (_textKeys.empty()) {
				return 0;
			}
			TextKey& key = _textKeys[_nextTextKey++ % _textKeys.size()];
			key.keysym = keysym;
			KeySym levels[TEXT_LEVELS] = {keysym, keysym};
			target.x->changeKeyboardMapping(_keymapDisplay, key.keycode, TEXT_LEVELS, levels, 1);
			target.x->sync(_keymapDisplay, 0);
			return key.keycode;
		}

		KeyCode PositionalKeycode(Key key) {
			constexpr u32 EVDEV_F13 = 183;
			constexpr u32 X11_OFFSET = 8;
			if (key >= Key::F13 && key <= Key::F24) {
				return static_cast<KeyCode>(EVDEV_F13 + (static_cast<u32>(key) - static_cast<u32>(Key::F13)) + X11_OFFSET);
			}
			return 0;
		}

		void SleepMs(u32 milliseconds) {
			timespec duration{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000L};
			::nanosleep(&duration, nullptr);
		}

		std::vector<str> SplitCommandLine(const str& commandLine) {
			std::vector<str> arguments;
			str current;
			bool quoted = false;
			bool any = false;
			for (char c : commandLine) {
				if (c == '"') {
					quoted = !quoted;
					any = true;
				} else if ((c == ' ' || c == '\t') && !quoted) {
					if (any) {
						arguments.push_back(current);
						current.clear();
						any = false;
					}
				} else {
					current += c;
					any = true;
				}
			}
			if (any) {
				arguments.push_back(current);
			}
			return arguments;
		}

		pid_t Spawn(const str& commandLine, bool dieWithParent) {
			std::vector<str> arguments = SplitCommandLine(commandLine);
			if (arguments.empty()) {
				return -1;
			}
			const pid_t child = ::fork();
			if (child != 0) {
				return child;
			}
			if (dieWithParent) {
				::prctl(PR_SET_PDEATHSIG, SIGKILL);
			}
			std::vector<char*> argv;
			for (str& argument : arguments) {
				argv.push_back(argument.data());
			}
			argv.push_back(nullptr);
			::execvp(argv[0], argv.data());
			::_exit(127);
		}

		bool WaitPid(pid_t child, u32 timeoutMs, i32& outExitCode) {
			for (u32 waited = 0;; waited += 10) {
				int status = 0;
				const pid_t reaped = ::waitpid(child, &status, WNOHANG);
				if (reaped == child) {
					outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
					return true;
				}
				if (reaped < 0 || waited >= timeoutMs) {
					return false;
				}
				SleepMs(10);
			}
		}

		struct OpenedLibrary {
			void* base = nullptr;
			void* handle = nullptr;
		};
		std::vector<OpenedLibrary> _opened;

		struct ImageQuery {
			const void* address = nullptr;
			void* base = nullptr;
			std::string_view fileName;
			bool found = false;
		};

		bool Names(std::string_view file, std::string_view wanted) {
			if (file.starts_with("lib") && !wanted.starts_with("lib")) {
				file.remove_prefix(3);
			}
			return file == wanted || (file.starts_with(wanted) && file.substr(wanted.size()).starts_with("."));
		}

		void* BaseOf(const dl_phdr_info* info) {
			for (int index = 0; index < info->dlpi_phnum; ++index) {
				const ElfW(Phdr) & segment = info->dlpi_phdr[index];
				if (segment.p_type == PT_LOAD) {
					Dl_info resolved{};
					const void* inside = reinterpret_cast<const void*>(info->dlpi_addr + segment.p_vaddr);
					return ::dladdr(inside, &resolved) != 0 ? resolved.dli_fbase : nullptr;
				}
			}
			return nullptr;
		}

		int VisitImage(dl_phdr_info* info, size_t, void* data) {
			ImageQuery* query = static_cast<ImageQuery*>(data);
			if (!query->fileName.empty()) {
				const std::string_view path = info->dlpi_name ? info->dlpi_name : "";
				const size_t slash = path.rfind('/');
				if (Names(slash == std::string_view::npos ? path : path.substr(slash + 1), query->fileName)) {
					query->base = BaseOf(info);
					query->found = true;
					return 1;
				}
				return 0;
			}
			for (int index = 0; index < info->dlpi_phnum; ++index) {
				const ElfW(Phdr) & segment = info->dlpi_phdr[index];
				if (segment.p_type != PT_LOAD) {
					continue;
				}
				const uintptr_t begin = info->dlpi_addr + segment.p_vaddr;
				const uintptr_t at = reinterpret_cast<uintptr_t>(query->address);
				if (at >= begin && at < begin + segment.p_memsz) {
					Dl_info resolved{};
					if (::dladdr(query->address, &resolved) != 0) {
						query->base = resolved.dli_fbase;
					}
					query->found = true;
					return 1;
				}
			}
			return 0;
		}

	} // anonymous namespace

	struct LaunchedProcess {
		pid_t id = -1;
		bool reaped = false;
		i32 exitCode = -1;
		Display* display = nullptr;
		::Window window = 0;
	};

	struct HeldFile {
		int descriptor = -1;
	};

	void PostKey(Handle<Window> window, Key key, bool down, bool repeat) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		const KeySym keysym = ToKeysym(key);
		KeyCode keycode = keysym ? target.x->keysymToKeycode(target.display, keysym) : 0;
		if (keycode == 0) {
			keycode = PositionalKeycode(key);
		}
		if (keycode == 0) {
			ADD_FAILURE() << "this keyboard has no key for Key " << static_cast<u32>(key);
			return;
		}
		const unsigned int bit = ModifierBit(key);
		if (down && !repeat && _heldKeycodes[keycode]) {
			SendKeycode(target, keycode, false, _heldModifiers & ~bit);
		}
		if (!down) {
			_heldModifiers &= ~bit;
		}
		SendKeycode(target, keycode, down, _heldModifiers);
		_heldKeycodes[keycode] = down;
		if (down) {
			_heldModifiers |= bit;
		}
	}

	void PostText(Handle<Window> window, char32_t codepoint) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		constexpr char32_t HIGH_FIRST = 0xD800;
		constexpr char32_t LOW_FIRST = 0xDC00;
		constexpr char32_t LOW_LAST = 0xDFFF;
		if (codepoint >= HIGH_FIRST && codepoint < LOW_FIRST) {
			_highSurrogate = codepoint;
			return;
		}
		if (codepoint >= LOW_FIRST && codepoint <= LOW_LAST) {
			if (_highSurrogate == 0) {
				return;
			}
			codepoint = 0x10000 + ((_highSurrogate - HIGH_FIRST) << 10) + (codepoint - LOW_FIRST);
			_highSurrogate = 0;
		}
		KeySym keysym = codepoint < 0x100 ? static_cast<KeySym>(codepoint) : static_cast<KeySym>(0x01000000u | codepoint);
		if (codepoint == 0x08) {
			keysym = XK_BackSpace;
		}
		const KeyCode keycode = KeycodeForText(target, keysym);
		if (keycode == 0) {
			ADD_FAILURE() << "this keyboard has no spare keycode";
			return;
		}
		SendKeycode(target, keycode, true, 0);
		SendKeycode(target, keycode, false, 0);
	}

	void PostMouseMove(Handle<Window> window, Vec2I clientPx) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		const Vec2I rootPx = ToRoot(target, clientPx);
		XEvent event{};
		event.xmotion.type = MotionNotify;
		event.xmotion.display = target.display;
		event.xmotion.window = target.window;
		event.xmotion.root = target.root;
		event.xmotion.time = CurrentTime;
		event.xmotion.x = clientPx.x;
		event.xmotion.y = clientPx.y;
		event.xmotion.x_root = rootPx.x;
		event.xmotion.y_root = rootPx.y;
		event.xmotion.state = _heldModifiers | _heldButtons;
		event.xmotion.same_screen = 1;
		Send(target, PointerMotionMask, event);
	}

	void PostMouseButton(Handle<Window> window, MouseButton button, bool down, Vec2I clientPx) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		unsigned int native = 0;
		unsigned int held = 0;
		switch (button) {
		case MouseButton::Left:
			native = Button1;
			held = Button1Mask;
			break;
		case MouseButton::Middle:
			native = Button2;
			held = Button2Mask;
			break;
		case MouseButton::Right:
			native = Button3;
			held = Button3Mask;
			break;
		case MouseButton::X1:
			native = 8;
			break;
		case MouseButton::X2:
			native = 9;
			break;
		default:
			ADD_FAILURE() << "no native button for MouseButton " << static_cast<u32>(button);
			return;
		}
		if (!down) {
			_heldButtons &= ~held;
		}
		SendButton(target, native, down, clientPx);
		if (down) {
			_heldButtons |= held;
		}
	}

	void PostMouseWheel(Handle<Window> window, Vec2 notches, Vec2I clientPx) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		const struct {
			f32 amount;
			unsigned int positive;
			unsigned int negative;
		} axes[] = {{notches.y, 4, 5}, {notches.x, 7, 6}};
		for (const auto& axis : axes) {
			const unsigned int button = axis.amount > 0.0f ? axis.positive : axis.negative;
			const i32 steps = static_cast<i32>(axis.amount > 0.0f ? axis.amount : -axis.amount);
			for (i32 step = 0; step < steps; ++step) {
				SendButton(target, button, true, clientPx);
				SendButton(target, button, false, clientPx);
			}
		}
	}

	void PostFocus(Handle<Window> window, bool focused) {
		SendFocus(window, focused);
		if (!focused) {
			_heldModifiers = 0;
			_heldButtons = 0;
			std::memset(_heldKeycodes, 0, sizeof(_heldKeycodes));
		}
	}

	void PostFocusMove(Handle<Window> from, Handle<Window> to) {
		SendFocus(from, false);
		SendFocus(to, true);
	}

	void PostClose(Handle<Window> window) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		XEvent event{};
		event.xclient.type = ClientMessage;
		event.xclient.display = target.display;
		event.xclient.window = target.window;
		event.xclient.message_type = target.x->internAtom(target.display, "WM_PROTOCOLS", 0);
		event.xclient.format = 32;
		event.xclient.data.l[0] = static_cast<long>(target.x->internAtom(target.display, "WM_DELETE_WINDOW", 0));
		event.xclient.data.l[1] = CurrentTime;
		Send(target, NoEventMask, event);
	}

	void PostWake(Handle<Window> window) {
		Target target;
		if (!Resolve(window, target)) {
			return;
		}
		XEvent event{};
		event.xclient.type = ClientMessage;
		event.xclient.display = target.display;
		event.xclient.window = target.window;
		event.xclient.message_type = target.x->internAtom(target.display, "_SHALLTEAR_TEST_WAKE", 0);
		event.xclient.format = 32;
		Send(target, NoEventMask, event);
	}

	str GetEnvironment(const char* name) {
		const char* value = std::getenv(name);
		return value ? str(value) : str();
	}

	void SetEnvironment(const char* name, const str& value) {
		if (value.empty()) {
			::unsetenv(name);
		} else {
			::setenv(name, value.c_str(), 1);
		}
	}

	const char* GetUserDataVariable() {
		return "XDG_CONFIG_HOME";
	}

	str GetSystemSearchPath() {
		return "/usr/bin:/bin";
	}

	char GetSearchPathSeparator() {
		return ':';
	}

	bool ToolDirectoriesFollowEnvironment() {
		return false;
	}

	str GetAbsoluteRoot() {
		return "/";
	}

	size_t GetPageSize() {
		return static_cast<size_t>(::sysconf(_SC_PAGESIZE));
	}

	u8* AllocateGuarded(size_t committedBytes) {
		void* base = ::mmap(nullptr, committedBytes + GetPageSize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			return nullptr;
		}
		::mprotect(base, committedBytes, PROT_READ | PROT_WRITE);
		return static_cast<u8*>(base);
	}

	void FreeGuarded(u8* base, size_t committedBytes) {
		if (base) {
			::munmap(base, committedBytes + GetPageSize());
		}
	}

	void* ImageOf(const void* address) {
		ImageQuery query;
		query.address = address;
		::dl_iterate_phdr(&VisitImage, &query);
		return query.base;
	}

	void* FindLoadedLibrary(std::string_view fileName) {
		ImageQuery query;
		query.fileName = fileName;
		::dl_iterate_phdr(&VisitImage, &query);
		return query.base;
	}

	void* GetSystemImage() {
		return FindLoadedLibrary("libc.so.6");
	}

	void* OpenLibrary(const str& path) {
		const str resolved = path.find('/') == str::npos ? "./" + path : path;
		void* handle = ::dlopen(resolved.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle) {
			return nullptr;
		}
		link_map* map = nullptr;
		void* base = nullptr;
		if (::dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_ld) {
			base = ImageOf(map->l_ld);
		}
		if (!base) {
			::dlclose(handle);
			return nullptr;
		}
		_opened.push_back({base, handle});
		return base;
	}

	void CloseLibrary(void* library) {
		for (size_t index = 0; index < _opened.size(); ++index) {
			if (_opened[index].base == library) {
				::dlclose(_opened[index].handle);
				_opened.erase(_opened.begin() + static_cast<ptrdiff_t>(index));
				return;
			}
		}
	}

	void* FindSymbol(void* library, const char* name) {
		for (const OpenedLibrary& opened : _opened) {
			if (opened.base == library) {
				return ::dlsym(opened.handle, name);
			}
		}
		return nullptr;
	}

	bool MonitorsAlwaysReportRefreshRate() {
		return false;
	}

	bool ChangesDisplayModes() {
		return false;
	}

	bool BuildsCSharpPlayers() {
		return true;
	}

	void* AllocateAligned(size_t size, size_t alignment) {
		void* block = nullptr;
		const size_t accepted = alignment < sizeof(void*) ? sizeof(void*) : alignment;
		return ::posix_memalign(&block, accepted, size) == 0 ? block : nullptr;
	}

	void FreeAligned(void* block) {
		std::free(block);
	}

	bool FileNamesFoldCase() {
		return false;
	}

	const char* GetCommonImportName() {
#ifdef SHALLTEAR_SHARED
		return "ShalltearCore";
#else
		return "";
#endif
	}

	str MakeCompilerAlias(const str& compilerPath, const str& directory) {
		const str alias = directory + "/alias-clang++";
		::unlink(alias.c_str());
		return ::symlink(compilerPath.c_str(), alias.c_str()) == 0 ? alias : str();
	}

	bool StaticModulesShareTypeInfo() {
		return false;
	}

	bool DenyDirectoryWrites(const str& directory) {
		int exitCode = 0;
		RunCommand("chmod -R a-w \"" + directory + "\"", exitCode);
		return exitCode == 0;
	}

	void AllowDirectoryWrites(const str& directory) {
		int exitCode = 0;
		RunCommand("chmod -R u+w \"" + directory + "\"", exitCode);
	}

	bool DenyFileReads(const str& file) {
		int exitCode = 0;
		RunCommand("chmod a-r \"" + file + "\"", exitCode);
		return exitCode == 0;
	}

	void AllowFileReads(const str& file) {
		int exitCode = 0;
		RunCommand("chmod u+r \"" + file + "\"", exitCode);
	}

	bool OpenFileRefusesDelete() {
		return false;
	}

	str FindSystemFont(std::string_view fileName) {
		int exitCode = 0;
		str found = RunCommand("fc-list --format '%{file}\\n' | grep -i -m1 '/" + str(fileName) + "$'", exitCode);
		while (!found.empty() && (found.back() == '\n' || found.back() == '\r')) {
			found.pop_back();
		}
		return exitCode == 0 ? found : str();
	}

	bool SystemFontsCover(char32_t codepoint) {
		int exitCode = 0;
		const str listed = RunCommand(std::format("fc-list ':charset={:x}' file", static_cast<u32>(codepoint)), exitCode);
		return exitCode == 0 && listed.find('/') != str::npos;
	}

	HeldFile* HoldFile(const str& path) {
		const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (descriptor < 0) {
			return nullptr;
		}
		if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
			::close(descriptor);
			return nullptr;
		}
		HeldFile* held = new HeldFile;
		held->descriptor = descriptor;
		return held;
	}

	void ReleaseFile(HeldFile* file) {
		if (file) {
			::close(file->descriptor);
			delete file;
		}
	}

	u32 GetProcessId() {
		return static_cast<u32>(::getpid());
	}

	u32 CountProcessThreads() {
		DIR* tasks = ::opendir("/proc/self/task");
		if (!tasks) {
			return 0;
		}
		u32 count = 0;
		while (const dirent* entry = ::readdir(tasks)) {
			if (entry->d_name[0] != '.') {
				++count;
			}
		}
		::closedir(tasks);
		return count;
	}

	bool IsArtifactKind(const str& file, bool, str& outWhy) {
		std::ifstream stream(file, std::ios::binary);
		Elf64_Ehdr header{};
		stream.read(reinterpret_cast<char*>(&header), sizeof(header));
		const bool isElf = stream.gcount() == sizeof(header) && std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0;
		outWhy = isElf ? "ELF type " + std::to_string(header.e_type) + " machine " + std::to_string(header.e_machine) : "not an ELF file";
		return isElf && header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_machine == EM_X86_64 &&
			   (header.e_type == ET_DYN || header.e_type == ET_EXEC);
	}

	bool ReadsTextAsUTF8(const str&, str& outWhy) {
		outWhy.clear();
		return true;
	}

	str FindForeignDependency(const str& playerFile) {
		std::ifstream stream(playerFile, std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
		if (bytes.size() < sizeof(Elf64_Ehdr)) {
			return "<unreadable>";
		}
		const Elf64_Ehdr& header = *reinterpret_cast<const Elf64_Ehdr*>(bytes.data());
		if (header.e_shoff == 0 || header.e_shoff + static_cast<size_t>(header.e_shnum) * sizeof(Elf64_Shdr) > bytes.size()) {
			return "<no sections>";
		}
		const Elf64_Shdr* sections = reinterpret_cast<const Elf64_Shdr*>(bytes.data() + header.e_shoff);
		constexpr std::string_view Allowed[] = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0",
												"librt.so.1", "libgcc_s.so.1", "ld-linux-x86-64.so.2"};
		for (u32 index = 0; index < header.e_shnum; ++index) {
			const Elf64_Shdr& section = sections[index];
			if (section.sh_type != SHT_DYNAMIC || section.sh_link >= header.e_shnum) {
				continue;
			}
			const Elf64_Shdr& strings = sections[section.sh_link];
			const Elf64_Dyn* entries = reinterpret_cast<const Elf64_Dyn*>(bytes.data() + section.sh_offset);
			for (size_t at = 0; at < section.sh_size / sizeof(Elf64_Dyn) && entries[at].d_tag != DT_NULL; ++at) {
				if (entries[at].d_tag != DT_NEEDED) {
					continue;
				}
				const std::string_view needed = bytes.data() + strings.sh_offset + entries[at].d_un.d_val;
				bool allowed = false;
				for (std::string_view name : Allowed) {
					allowed = allowed || needed == name;
				}
				if (!allowed) {
					return str(needed);
				}
			}
		}
		return {};
	}

	ProcessResult RunProcess(const str& commandLine, u32 timeoutMs) {
		ProcessResult result;
		const pid_t child = Spawn(commandLine, true);
		if (child < 0) {
			return result;
		}
		result.started = true;
		result.exited = WaitPid(child, timeoutMs, result.exitCode);
		if (!result.exited) {
			::kill(child, SIGKILL);
			int status = 0;
			::waitpid(child, &status, 0);
		}
		return result;
	}

	str RunCommand(const str& commandLine, int& outExitCode) {
		const str command = commandLine + " 2>&1";
		str output;
		FILE* pipe = ::popen(command.c_str(), "r");
		if (!pipe) {
			outExitCode = -1;
			return output;
		}
		char buffer[512];
		while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			output += buffer;
		}
		const int status = ::pclose(pipe);
		outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		return output;
	}

	bool RunHelperProcess(int, char**, int&) {
		return false;
	}

	str GetEnvironmentProbeCommand(const char* name, const str& receiptPath, str& outUnsetText) {
		outUnsetText = "[]";
		return "sh -c \"echo [$" + str(name) + "] > '" + receiptPath + "'\"";
	}

	str GetEditorProbeCommand(const str& receiptPath) {
		return "sh -c \"echo '[{file}] [{line}]' > '" + receiptPath + "'\"";
	}

	str StageExternalEditor(const str& root, str& outVariable) {
		const std::filesystem::path programs = std::filesystem::path(root) / "home" / ".local" / "bin";
		std::filesystem::create_directories(programs);
		std::ofstream stub(programs / "code", std::ios::binary);
		stub << "stub";
		stub.close();
		std::filesystem::permissions(programs / "code", std::filesystem::perms::owner_all);
		outVariable = "HOME";
		return (std::filesystem::path(root) / "home").generic_string();
	}

	LaunchedProcess* Launch(const str& commandLine) {
		const pid_t child = Spawn(commandLine, true);
		if (child < 0) {
			return nullptr;
		}
		LaunchedProcess* launched = new LaunchedProcess;
		launched->id = child;
		return launched;
	}

	void Terminate(LaunchedProcess* process) {
		if (!process) {
			return;
		}
		if (!process->reaped) {
			::kill(process->id, SIGKILL);
			int status = 0;
			::waitpid(process->id, &status, 0);
		}
		if (process->display) {
			GetX()->closeDisplay(process->display);
		}
		delete process;
	}

	bool WaitForExit(LaunchedProcess* process, u32 timeoutMs, i32& outExitCode) {
		if (!process) {
			return false;
		}
		if (!process->reaped) {
			process->reaped = WaitPid(process->id, timeoutMs, process->exitCode);
		}
		outExitCode = process->exitCode;
		return process->reaped;
	}

	bool WaitForWindow(LaunchedProcess* process, std::string_view title, u32 timeoutMs, str& outSeen) {
		const XlibAPI* x = GetX();
		if (!process || !x) {
			return false;
		}
		if (!process->display) {
			process->display = x->openDisplay(nullptr);
		}
		Display* display = process->display;
		if (!display) {
			return false;
		}
		const ::Window root = DefaultRootWindow(display);
		const Atom pidAtom = x->internAtom(display, "_NET_WM_PID", 0);
		const Atom nameAtom = x->internAtom(display, "_NET_WM_NAME", 0);
		const Atom utf8 = x->internAtom(display, "UTF8_STRING", 0);

		for (u32 waited = 0; waited < timeoutMs; waited += 100) {
			outSeen.clear();
			std::vector<::Window> pending{root};
			while (!pending.empty()) {
				const ::Window current = pending.back();
				pending.pop_back();
				::Window rootOut = 0;
				::Window parent = 0;
				::Window* children = nullptr;
				unsigned int count = 0;
				if (x->queryTree(display, current, &rootOut, &parent, &children, &count) != 0 && children) {
					pending.insert(pending.end(), children, children + count);
					x->freeData(children);
				}

				Atom type = 0;
				int format = 0;
				unsigned long items = 0;
				unsigned long left = 0;
				unsigned char* data = nullptr;
				if (x->getWindowProperty(display, current, pidAtom, 0, 1, 0, XA_CARDINAL, &type, &format, &items, &left, &data) != 0 ||
					!data) {
					continue;
				}
				const bool owned = items == 1 && static_cast<pid_t>(*reinterpret_cast<unsigned long*>(data)) == process->id;
				x->freeData(data);
				XWindowAttributes attributes{};
				if (!owned || x->getWindowAttributes(display, current, &attributes) == 0 || attributes.map_state != IsViewable) {
					continue;
				}
				data = nullptr;
				if (x->getWindowProperty(display, current, nameAtom, 0, 256, 0, utf8, &type, &format, &items, &left, &data) != 0 ||
					!data) {
					continue;
				}
				const str name(reinterpret_cast<const char*>(data), items);
				x->freeData(data);
				if (name == title) {
					process->window = current;
					return true;
				}
				outSeen += "\n    \"" + name + "\"";
			}
			SleepMs(100);
		}
		return false;
	}

	bool CloseWindow(LaunchedProcess* process) {
		const XlibAPI* x = GetX();
		if (!process || !x || !process->display || process->window == 0) {
			return false;
		}
		XEvent event{};
		event.xclient.type = ClientMessage;
		event.xclient.display = process->display;
		event.xclient.window = process->window;
		event.xclient.message_type = x->internAtom(process->display, "WM_PROTOCOLS", 0);
		event.xclient.format = 32;
		event.xclient.data.l[0] = static_cast<long>(x->internAtom(process->display, "WM_DELETE_WINDOW", 0));
		event.xclient.data.l[1] = CurrentTime;
		x->sendEvent(process->display, process->window, 0, NoEventMask, &event);
		x->flush(process->display);
		return true;
	}

	namespace {

		std::vector<IconImage> ReadNetWmIcon(const XlibAPI* x, Display* display, ::Window window) {
			std::vector<IconImage> out;
			if (!x || !display || window == 0) {
				return out;
			}
			const Atom iconAtom = x->internAtom(display, "_NET_WM_ICON", 0);
			Atom type = 0;
			int format = 0;
			unsigned long items = 0;
			unsigned long left = 0;
			unsigned char* data = nullptr;
			if (x->getWindowProperty(display, window, iconAtom, 0, 1L << 24, 0, XA_CARDINAL, &type, &format, &items, &left,
									 &data) != 0 ||
				!data) {
				return out;
			}
			const unsigned long* values = reinterpret_cast<const unsigned long*>(data);
			unsigned long at = 0;
			while (at + 2 <= items) {
				const unsigned long width = values[at];
				const unsigned long height = values[at + 1];
				at += 2;
				if (width == 0 || height == 0 || width * height > items - at) {
					break;
				}
				IconImage image;
				image.size = {static_cast<i32>(width), static_cast<i32>(height)};
				image.rgba.resize(width * height * 4);
				for (unsigned long i = 0; i < width * height; ++i) {
					const unsigned long argb = values[at + i];
					image.rgba[i * 4] = static_cast<u8>(argb >> 16);
					image.rgba[i * 4 + 1] = static_cast<u8>(argb >> 8);
					image.rgba[i * 4 + 2] = static_cast<u8>(argb);
					image.rgba[i * 4 + 3] = static_cast<u8>(argb >> 24);
				}
				at += width * height;
				out.push_back(std::move(image));
			}
			x->freeData(data);
			return out;
		}

	} // anonymous namespace

	std::vector<IconImage> ReadWindowIcon(Handle<Window> window) {
		Target target;
		if (!Resolve(window, target)) {
			return {};
		}
		return ReadNetWmIcon(target.x, target.display, target.window);
	}

	std::vector<IconImage> ReadWindowIcon(LaunchedProcess* process) {
		return process ? ReadNetWmIcon(GetX(), process->display, process->window) : std::vector<IconImage>{};
	}

	bool ExecutablesCarryIcons() {
		return false;
	}

	std::vector<IconImage> ReadExecutableIcon(const str&) {
		return {};
	}

} // namespace TestPlatform
