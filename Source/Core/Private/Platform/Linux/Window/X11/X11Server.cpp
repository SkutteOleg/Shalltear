#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Window/WindowEvents.h"

#include "Input/InputInternal.h"
#include "Platform/Library.h"

#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <unistd.h>

#include "Platform/Linux/Window/X11/X11Server.h"

namespace Shalltear::Linux {

	namespace {

		constexpr long EVENT_MASK = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
									EnterWindowMask | LeaveWindowMask | FocusChangeMask | StructureNotifyMask | PropertyChangeMask;

		constexpr long NET_WM_STATE_REMOVE = 0;
		constexpr long NET_WM_STATE_ADD = 1;

		struct MotifHints {
			unsigned long flags = 2;
			unsigned long functions = 0;
			unsigned long decorations = 0;
			long inputMode = 0;
			unsigned long status = 0;
		};

		int OnError(Display*, XErrorEvent* error) {
			SLT_LOG_WARN("Platform", "X11 request {} failed (error {})", static_cast<int>(error->request_code),
						 static_cast<int>(error->error_code));
			return 0;
		}

		template <typename TFunction>
		bool Resolve(void* library, const char* name, TFunction& out) {
			out = reinterpret_cast<TFunction>(Platform::FindSymbol(library, name));
			return out != nullptr;
		}

	} // anonymous namespace

	bool X11API::Load(str& outWhy) {
		x11 = Platform::OpenLibrary("libX11.so.6", &outWhy);
		if (!x11) {
			return false;
		}
		bool complete = true;
#define SLT_X11_LOAD(name) complete = Resolve(x11, #name, name) && complete;
		SLT_X11_FUNCS(SLT_X11_LOAD)
#undef SLT_X11_LOAD
		if (!complete) {
			outWhy = "libX11.so.6 lacks a function this backend calls";
			Unload();
			return false;
		}

		xi = Platform::OpenLibrary("libXi.so.6");
		if (xi) {
			bool have = true;
#define SLT_X11_LOAD(name) have = Resolve(xi, #name, name) && have;
			SLT_XI_FUNCS(SLT_X11_LOAD)
#undef SLT_X11_LOAD
			if (!have) {
				Platform::CloseLibrary(xi);
				xi = nullptr;
			}
		}
		xrandr = Platform::OpenLibrary("libXrandr.so.2");
		if (xrandr) {
			bool have = true;
#define SLT_X11_LOAD(name) have = Resolve(xrandr, #name, name) && have;
			SLT_XRANDR_FUNCS(SLT_X11_LOAD)
#undef SLT_X11_LOAD
			if (!have) {
				Platform::CloseLibrary(xrandr);
				xrandr = nullptr;
			}
		}
		xcursor = Platform::OpenLibrary("libXcursor.so.1");
		if (xcursor && !Resolve(xcursor, "XcursorLibraryLoadCursor", XcursorLibraryLoadCursor)) {
			Platform::CloseLibrary(xcursor);
			xcursor = nullptr;
		}
		return true;
	}

	void X11API::Unload() {
		for (void** library : {&xcursor, &xrandr, &xi, &x11}) {
			if (*library) {
				Platform::CloseLibrary(*library);
				*library = nullptr;
			}
		}
	}

	bool X11Server::Connect(str& outWhy) {
		const char* name = std::getenv("DISPLAY");
		if (!name || !*name) {
			outWhy = "DISPLAY is not set";
			return false;
		}
		if (!x.Load(outWhy)) {
			return false;
		}
		x.XInitThreads();
		// dead keys + input methods need user's locale
		if (std::strcmp(std::setlocale(LC_CTYPE, nullptr), "C") == 0) {
			std::setlocale(LC_CTYPE, "");
		}
		display = x.XOpenDisplay(name);
		if (!display) {
			outWhy = "no X server answers at " + str(name);
			x.Unload();
			return false;
		}
		x.XSetErrorHandler(&OnError);
		screen = DefaultScreen(display);
		root = RootWindow(display, screen);

		auto atom = [this](const char* text) { return x.XInternAtom(display, text, False); };
		atoms.wmProtocols = atom("WM_PROTOCOLS");
		atoms.wmDeleteWindow = atom("WM_DELETE_WINDOW");
		atoms.wmState = atom("WM_STATE");
		atoms.wmChangeState = atom("WM_CHANGE_STATE");
		atoms.netWmPing = atom("_NET_WM_PING");
		atoms.netWmName = atom("_NET_WM_NAME");
		atoms.netWmPid = atom("_NET_WM_PID");
		atoms.netWmIcon = atom("_NET_WM_ICON");
		atoms.netWmState = atom("_NET_WM_STATE");
		atoms.netWmStateMaximizedVert = atom("_NET_WM_STATE_MAXIMIZED_VERT");
		atoms.netWmStateMaximizedHorz = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
		atoms.netWmStateFullscreen = atom("_NET_WM_STATE_FULLSCREEN");
		atoms.netWmStateHidden = atom("_NET_WM_STATE_HIDDEN");
		atoms.netWmStateAbove = atom("_NET_WM_STATE_ABOVE");
		atoms.netWmStateSkipTaskbar = atom("_NET_WM_STATE_SKIP_TASKBAR");
		atoms.netActiveWindow = atom("_NET_ACTIVE_WINDOW");
		atoms.netFrameExtents = atom("_NET_FRAME_EXTENTS");
		atoms.netWmMoveResize = atom("_NET_WM_MOVERESIZE");
		atoms.netWorkArea = atom("_NET_WORKAREA");
		atoms.netClientListStacking = atom("_NET_CLIENT_LIST_STACKING");
		atoms.netSupportingWmCheck = atom("_NET_SUPPORTING_WM_CHECK");
		atoms.motifWmHints = atom("_MOTIF_WM_HINTS");
		atoms.gtkShowWindowMenu = atom("_GTK_SHOW_WINDOW_MENU");
		atoms.resourceManager = atom("RESOURCE_MANAGER");
		atoms.utf8String = atom("UTF8_STRING");
		atoms.clipboard = atom("CLIPBOARD");
		atoms.targets = atom("TARGETS");
		atoms.incr = atom("INCR");
		atoms.selectionProperty = atom("SHALLTEAR_SELECTION");
		atoms.textPlainUTF8 = atom("text/plain;charset=utf-8");
		atoms.xdndAware = atom("XdndAware");
		atoms.xdndEnter = atom("XdndEnter");
		atoms.xdndPosition = atom("XdndPosition");
		atoms.xdndStatus = atom("XdndStatus");
		atoms.xdndLeave = atom("XdndLeave");
		atoms.xdndDrop = atom("XdndDrop");
		atoms.xdndFinished = atom("XdndFinished");
		atoms.xdndSelection = atom("XdndSelection");
		atoms.xdndActionCopy = atom("XdndActionCopy");
		atoms.xdndTypeList = atom("XdndTypeList");
		atoms.textUriList = atom("text/uri-list");

		XSetWindowAttributes helperAttributes{};
		helperAttributes.event_mask = PropertyChangeMask;
		helper = x.XCreateWindow(display, root, -10, -10, 1, 1, 0, CopyFromParent, InputOutput, nullptr, CWEventMask, &helperAttributes);
		x.XSelectInput(display, root, PropertyChangeMask);

		int supported = 0;
		x.XkbSetDetectableAutoRepeat(display, True, &supported);
		x.XSetLocaleModifiers("");
		inputMethod = x.XOpenIM(display, nullptr, nullptr, nullptr);

		int event = 0;
		int error = 0;
		if (x.xi && x.XQueryExtension(display, "XInputExtension", &xiOpcode, &event, &error)) {
			int major = 2;
			int minor = 0;
			haveRawMotion = x.XIQueryVersion(display, &major, &minor) == 0;
		}
		if (x.xrandr && x.XRRQueryExtension(display, &randrEventBase, &error)) {
			int major = 0;
			int minor = 0;
			haveRandr = x.XRRQueryVersion(display, &major, &minor) && (major > 1 || minor >= 5);
			if (haveRandr) {
				x.XRRSelectInput(display, root, RRScreenChangeNotifyMask);
			}
		}

		static const struct {
			const char* theme;
			unsigned int font;
		} cursorNames[static_cast<size_t>(SystemCursor::Count)] = {
			{"left_ptr", XC_left_ptr},
			{"xterm", XC_xterm},
			{"crosshair", XC_crosshair},
			{"hand2", XC_hand2},
			{"sb_h_double_arrow", XC_sb_h_double_arrow},
			{"sb_v_double_arrow", XC_sb_v_double_arrow},
			{"nwse-resize", XC_bottom_right_corner},
			{"nesw-resize", XC_bottom_left_corner},
			{"fleur", XC_fleur},
			{"not-allowed", XC_X_cursor},
		};
		for (size_t i = 0; i < static_cast<size_t>(SystemCursor::Count); ++i) {
			cursors[i] = x.xcursor ? x.XcursorLibraryLoadCursor(display, cursorNames[i].theme) : 0;
			if (!cursors[i]) {
				cursors[i] = x.XCreateFontCursor(display, cursorNames[i].font);
			}
		}
		const char empty[1] = {0};
		Pixmap bitmap = x.XCreateBitmapFromData(display, root, empty, 1, 1);
		XColor black{};
		blankCursor = x.XCreatePixmapCursor(display, bitmap, bitmap, &black, &black, 0, 0);
		x.XFreePixmap(display, bitmap);

		scale = ReadScale();
		HandleRootProperty(atoms.netSupportingWmCheck);
		return true;
	}

	X11Server::~X11Server() {
		if (!display) {
			return;
		}
		for (Cursor cursor : cursors) {
			if (cursor) {
				x.XFreeCursor(display, cursor);
			}
		}
		if (blankCursor) {
			x.XFreeCursor(display, blankCursor);
		}
		if (inputMethod) {
			x.XCloseIM(inputMethod);
		}
		x.XDestroyWindow(display, helper);
		x.XCloseDisplay(display);
		display = nullptr;
		x.Unload();
	}

	Window::Impl* X11Server::FindImpl(::Window handle) {
		for (Window* window : WindowInternal::GetState().windows) {
			Window::Impl* impl = WindowInternalAccess::GetImpl(*window);
			if (impl && impl->native && Native(*impl).handle == handle) {
				return impl;
			}
		}
		return nullptr;
	}

	bool X11Server::ReadProperty(::Window window, Atom property, Atom type, Vector<u8>& out, Atom* outType) {
		out.clear();
		Atom actualType = 0;
		int format = 0;
		unsigned long count = 0;
		unsigned long remaining = 0;
		unsigned char* data = nullptr;
		if (x.XGetWindowProperty(display, window, property, 0, 0x1FFFFFFF, False, type, &actualType, &format, &count, &remaining, &data) != 0 ||
			!data) {
			return false;
		}
		// format 32 items stored as long (wtf Xlib)
		const size_t itemBytes = format == 32 ? sizeof(long) : static_cast<size_t>(format / 8);
		out.assign(data, data + count * itemBytes);
		x.XFree(data);
		if (outType) {
			*outType = actualType;
		}
		return actualType != 0;
	}

	f32 X11Server::ReadScale() {
		Vector<u8> text{GetMemoryResource(MemTag::Platform)};
		if (ReadProperty(root, atoms.resourceManager, XA_STRING, text)) {
			const std::string_view resources(reinterpret_cast<const char*>(text.data()), text.size());
			constexpr std::string_view Key = "Xft.dpi:";
			const size_t at = resources.find(Key);
			if (at != std::string_view::npos) {
				const f64 dpi = std::strtod(str(resources.substr(at + Key.size(), 16)).c_str(), nullptr);
				if (dpi >= 48.0 && dpi <= 960.0) {
					return static_cast<f32>(dpi / 96.0);
				}
			}
		}
		return 1.0f;
	}

	void X11Server::SendRootMessage(::Window window, Atom type, long l0, long l1, long l2, long l3, long l4) {
		XEvent event{};
		event.xclient.type = ClientMessage;
		event.xclient.window = window;
		event.xclient.message_type = type;
		event.xclient.format = 32;
		event.xclient.data.l[0] = l0;
		event.xclient.data.l[1] = l1;
		event.xclient.data.l[2] = l2;
		event.xclient.data.l[3] = l3;
		event.xclient.data.l[4] = l4;
		x.XSendEvent(display, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
	}

	void X11Server::SetNetState(Window::Impl& impl, bool add, Atom first, Atom second) {
		X11Window& w = Native(impl);
		if (w.mapped) {
			SendRootMessage(w.handle, atoms.netWmState, add ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE,
							static_cast<long>(first), static_cast<long>(second), 1, 0);
			return;
		}
		// unmapped window's state read by WM from property at map
		Vector<u8> raw{GetMemoryResource(MemTag::Platform)};
		Vector<Atom> states{GetMemoryResource(MemTag::Platform)};
		if (ReadProperty(w.handle, atoms.netWmState, XA_ATOM, raw)) {
			const Atom* values = reinterpret_cast<const Atom*>(raw.data());
			states.assign(values, values + raw.size() / sizeof(Atom));
		}
		for (Atom state : {first, second}) {
			if (!state) {
				continue;
			}
			auto found = std::find(states.begin(), states.end(), state);
			if (add && found == states.end()) {
				states.push_back(state);
			} else if (!add && found != states.end()) {
				states.erase(found);
			}
		}
		x.XChangeProperty(display, w.handle, atoms.netWmState, XA_ATOM, 32, PropModeReplace,
						  reinterpret_cast<const unsigned char*>(states.data()), static_cast<int>(states.size()));
	}

	Vec2I X11Server::QueryPointer(unsigned int* outMask) {
		::Window rootReturn = 0;
		::Window child = 0;
		int rootX = 0;
		int rootY = 0;
		int windowX = 0;
		int windowY = 0;
		unsigned int mask = 0;
		x.XQueryPointer(display, root, &rootReturn, &child, &rootX, &rootY, &windowX, &windowY, &mask);
		if (outMask) {
			*outMask = mask;
		}
		return {rootX, rootY};
	}

	bool X11Server::CreateWindow(Window::Impl& impl, const WindowDesc& desc) {
		auto* w = New<X11Window>(MemTag::Platform);
		impl.native = w;
		impl.scale = scale;

		Vec2I clientSize = desc.clientSize;
		if (HasFlag(desc.flags, WindowFlags::ScaleToMonitor)) {
			clientSize = Vec2I(static_cast<i32>(clientSize.x * scale + 0.5f), static_cast<i32>(clientSize.y * scale + 0.5f));
		}
		clientSize.x = clientSize.x > 0 ? clientSize.x : 1;
		clientSize.y = clientSize.y > 0 ? clientSize.y : 1;
		const Vec2I position = desc.position.value_or(Vec2I{0, 0});

		XSetWindowAttributes attributes{};
		attributes.event_mask = EVENT_MASK;
		attributes.background_pixel = BlackPixel(display, screen);
		w->handle = x.XCreateWindow(display, root, position.x, position.y, static_cast<unsigned int>(clientSize.x),
									static_cast<unsigned int>(clientSize.y), 0, CopyFromParent, InputOutput, nullptr,
									CWEventMask | CWBackPixel, &attributes);
		if (!w->handle) {
			SLT_LOG_ERROR("Platform", "XCreateWindow failed");
			impl.native = nullptr;
			Delete(w);
			return false;
		}
		w->clientSize = clientSize;
		w->clientOrigin = position;
		w->normalPosition = position;
		w->normalSize = clientSize;

		Atom protocols[2] = {atoms.wmDeleteWindow, atoms.netWmPing};
		x.XSetWMProtocols(display, w->handle, protocols, 2);
		const long pid = static_cast<long>(::getpid());
		x.XChangeProperty(display, w->handle, atoms.netWmPid, XA_CARDINAL, 32, PropModeReplace,
						  reinterpret_cast<const unsigned char*>(&pid), 1);

		if (XClassHint* classHint = x.XAllocClassHint()) {
			char name[] = "shalltear";
			char className[] = "Shalltear";
			classHint->res_name = name;
			classHint->res_class = className;
			x.XSetClassHint(display, w->handle, classHint);
			x.XFree(classHint);
		}

		if (XSizeHints* hints = x.XAllocSizeHints()) {
			hints->flags = PMinSize | PWinGravity;
			hints->win_gravity = NorthWestGravity;
			hints->min_width = impl.minClientSize.x;
			hints->min_height = impl.minClientSize.y;
			if (impl.maxClientSize.x > 0 && impl.maxClientSize.y > 0) {
				hints->flags |= PMaxSize;
				hints->max_width = impl.maxClientSize.x;
				hints->max_height = impl.maxClientSize.y;
			}
			if (!HasFlag(desc.flags, WindowFlags::Resizable)) {
				hints->flags |= PMaxSize;
				hints->min_width = hints->max_width = clientSize.x;
				hints->min_height = hints->max_height = clientSize.y;
			}
			if (desc.position.has_value()) {
				hints->flags |= USPosition | PPosition;
				hints->x = position.x;
				hints->y = position.y;
			}
			x.XSetWMNormalHints(display, w->handle, hints);
			x.XFree(hints);
		}

		if (HasFlag(desc.flags, WindowFlags::Borderless)) {
			const MotifHints hints;
			x.XChangeProperty(display, w->handle, atoms.motifWmHints, atoms.motifWmHints, 32, PropModeReplace,
							  reinterpret_cast<const unsigned char*>(&hints), 5);
		}
		if (HasFlag(desc.flags, WindowFlags::AlwaysOnTop)) {
			SetNetState(impl, true, atoms.netWmStateAbove, 0);
		}
		if (HasFlag(desc.flags, WindowFlags::NoTaskbarIcon)) {
			SetNetState(impl, true, atoms.netWmStateSkipTaskbar, 0);
		}
		if (Window* owner = desc.owner.Get()) {
			if (Window::Impl* ownerImpl = WindowInternalAccess::GetImpl(*owner)) {
				x.XSetTransientForHint(display, w->handle, Native(*ownerImpl).handle);
			}
		}
		if (HasFlag(desc.flags, WindowFlags::AcceptFileDrops)) {
			const Atom version = 5;
			x.XChangeProperty(display, w->handle, atoms.xdndAware, XA_ATOM, 32, PropModeReplace,
							  reinterpret_cast<const unsigned char*>(&version), 1);
		}
		if (inputMethod) {
			w->inputContext = x.XCreateIC(inputMethod, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, w->handle,
										  XNFocusWindow, w->handle, nullptr);
		}
		SetTitle(impl);
		return true;
	}

	void X11Server::DestroyWindow(Window::Impl& impl) {
		if (!impl.native) {
			return;
		}
		X11Window& w = Native(impl);
		ReleaseCursorLock(impl);
		EndCursorWrap(impl);
		if (focusLost == &impl) {
			focusLost = nullptr;
		}
		if (w.inputContext) {
			x.XDestroyIC(w.inputContext);
		}
		x.XDestroyWindow(display, w.handle);
		x.XFlush(display);
		Delete(&w);
		impl.native = nullptr;
	}

	void X11Server::Wait(f64 timeoutSeconds) {
		x.XFlush(display);
		if (x.XPending(display) > 0) {
			return;
		}
		int milliseconds = timeoutSeconds < 0.0 ? -1 : static_cast<int>(std::ceil(timeoutSeconds * 1000.0));
		for (Window* window : WindowInternal::GetState().windows) {
			const Window::Impl* impl = WindowInternalAccess::GetImpl(*window);
			// no events while WM moves a window, pointer followed by PollMoves
			if (impl && impl->native && Native(*impl).moving && (milliseconds < 0 || milliseconds > 8)) {
				milliseconds = 8;
			}
		}
		pollfd waits{ConnectionNumber(display), POLLIN, 0};
		::poll(&waits, 1, milliseconds);
	}

	void X11Server::Pump() {
		while (x.XPending(display) > 0) {
			XEvent event;
			x.XNextEvent(display, &event);
			HandleEvent(event);
		}
		SettleFocus();
		PollMoves();
		x.XFlush(display);
	}

	namespace {

		int IsStructureEvent(Display*, XEvent* event, XPointer) {
			switch (event->type) {
			case ConfigureNotify:
			case MapNotify:
			case UnmapNotify:
			case ReparentNotify:
			case PropertyNotify:
				return True;
			default:
				return False;
			}
		}

	} // anonymous namespace

	template <typename TDone>
	void X11Server::Settle(TDone&& done) {
		constexpr int WAIT_MS = 2;
		const int attempts = hasWindowManager ? 60 : 0;
		for (int attempt = 0;; ++attempt) {
			x.XSync(display, False);
			XEvent event;
			while (x.XCheckIfEvent(display, &event, &IsStructureEvent, nullptr)) {
				HandleEvent(event);
			}
			if (done() || attempt >= attempts) {
				return;
			}
			pollfd waits{ConnectionNumber(display), POLLIN, 0};
			::poll(&waits, 1, WAIT_MS);
		}
	}

	void X11Server::ApplyGeometry(Window::Impl& impl, Vec2I position, Vec2I clientSize) {
		const X11Window& w = Native(impl);
		XConfigureEvent settled{};
		settled.send_event = True;
		settled.x = position.x + w.frameLeftTop.x;
		settled.y = position.y + w.frameLeftTop.y;
		settled.width = clientSize.x;
		settled.height = clientSize.y;
		HandleConfigure(impl, settled);
	}

	void X11Server::MoveResize(Window::Impl& impl, Vec2I position, Vec2I clientSize) {
		X11Window& w = Native(impl);
		const Vec2I size{clientSize.x > 0 ? clientSize.x : 1, clientSize.y > 0 ? clientSize.y : 1};
		x.XMoveResizeWindow(display, w.handle, position.x, position.y, static_cast<unsigned int>(size.x), static_cast<unsigned int>(size.y));
		// size may be announced after a move or decoration change (bloody openbox)
		Settle([&] { return w.clientSize == size && GetPosition(impl) == position; });
	}

	void X11Server::ApplyUnmappedState(Window::Impl& impl) {
		X11Window& w = Native(impl);
		RectI target{w.normalPosition.x, w.normalPosition.y, w.normalSize.x, w.normalSize.y};
		if (w.fullscreen || w.maximized) {
			if (const MonitorInfo* monitor = GetMonitor(impl)) {
				target = w.fullscreen ? monitor->bounds : monitor->workArea;
			}
		}
		x.XMoveResizeWindow(display, w.handle, target.position.x, target.position.y, static_cast<unsigned int>(target.size.x > 0 ? target.size.x : 1),
							static_cast<unsigned int>(target.size.y > 0 ? target.size.y : 1));
		const u32 before = w.configures;
		Settle([&] { return w.configures != before; });
		if (w.clientSize != target.size || GetPosition(impl) != target.position) {
			ApplyGeometry(impl, target.position, target.size);
		}
		RefreshState(impl);
	}

	Vec2I X11Server::GetClientSize(const Window::Impl& impl) {
		return Native(impl).clientSize;
	}

	void X11Server::SetClientSize(Window::Impl& impl, Vec2I sizePx) {
		X11Window& w = Native(impl);
		w.normalSize = sizePx;
		if (w.fullscreen || w.maximized) {
			return;
		}
		if (!HasFlag(impl.flags, WindowFlags::Resizable)) {
			if (XSizeHints* hints = x.XAllocSizeHints()) {
				hints->flags = PMinSize | PMaxSize;
				hints->min_width = hints->max_width = sizePx.x;
				hints->min_height = hints->max_height = sizePx.y;
				x.XSetWMNormalHints(display, w.handle, hints);
				x.XFree(hints);
			}
		}
		if (sizePx == w.clientSize) {
			return;
		}
		const u32 before = w.configures;
		x.XResizeWindow(display, w.handle, static_cast<unsigned int>(sizePx.x > 0 ? sizePx.x : 1),
						static_cast<unsigned int>(sizePx.y > 0 ? sizePx.y : 1));
		Settle([&] { return w.configures != before; });
		if (!w.mapped && w.clientSize != sizePx) {
			ApplyGeometry(impl, GetPosition(impl), sizePx);
		}
	}

	Vec2I X11Server::GetPosition(const Window::Impl& impl) {
		const X11Window& w = Native(impl);
		return w.clientOrigin - w.frameLeftTop;
	}

	void X11Server::SetPosition(Window::Impl& impl, Vec2I posPx) {
		X11Window& w = Native(impl);
		w.normalPosition = posPx;
		if (w.fullscreen || w.maximized) {
			return;
		}
		if (posPx == GetPosition(impl)) {
			return;
		}
		const u32 before = w.configures;
		x.XMoveWindow(display, w.handle, posPx.x, posPx.y);
		Settle([&] { return w.configures != before; });
		if (!w.mapped && GetPosition(impl) != posPx) {
			ApplyGeometry(impl, posPx, w.clientSize);
		}
	}

	RectI X11Server::GetRect(const Window::Impl& impl) {
		const X11Window& w = Native(impl);
		const Vec2I position = w.clientOrigin - w.frameLeftTop;
		const Vec2I size = w.clientSize + w.frameLeftTop + w.frameRightBottom;
		return {position.x, position.y, size.x, size.y};
	}

	Vec2I X11Server::ClientToScreen(const Window::Impl& impl, Vec2I clientPx) {
		return clientPx + Native(impl).clientOrigin;
	}

	Vec2I X11Server::ScreenToClient(const Window::Impl& impl, Vec2I screenPx) {
		return screenPx - Native(impl).clientOrigin;
	}

	const MonitorInfo* X11Server::GetMonitor(const Window::Impl& impl) {
		const X11Window& w = Native(impl);
		const Vec2I center = w.clientOrigin + Vec2I{w.clientSize.x / 2, w.clientSize.y / 2};
		const Vector<MonitorInfo>& monitors = WindowInternal::GetState().monitors;
		const MonitorInfo* primary = nullptr;
		for (const MonitorInfo& monitor : monitors) {
			const RectI& bounds = monitor.bounds;
			if (center.x >= bounds.position.x && center.x < bounds.position.x + bounds.size.x &&
				center.y >= bounds.position.y && center.y < bounds.position.y + bounds.size.y) {
				return &monitor;
			}
			if (monitor.primary) {
				primary = &monitor;
			}
		}
		return primary;
	}

	WindowPlacement X11Server::GetPlacement(const Window::Impl& impl) {
		const X11Window& w = Native(impl);
		WindowPlacement placement;
		placement.position = w.normalPosition;
		placement.clientSize = w.normalSize;
		placement.state = w.iconic		 ? WindowState::Minimized
						  : w.fullscreen ? WindowState::Fullscreen
						  : w.maximized	 ? WindowState::Maximized
										 : WindowState::Normal;
		return placement;
	}

	void X11Server::SetPlacement(Window::Impl& impl, const WindowPlacement& placement) {
		X11Window& w = Native(impl);
		w.normalPosition = placement.position;
		w.normalSize = placement.clientSize;

		const bool wantFullscreen = placement.state == WindowState::Fullscreen;
		const bool wantMaximized = placement.state == WindowState::Maximized;
		if (!w.mapped) {
			if (w.fullscreen != wantFullscreen) {
				SetNetState(impl, wantFullscreen, atoms.netWmStateFullscreen, 0);
				w.fullscreen = wantFullscreen;
			}
			if (w.maximized != wantMaximized) {
				SetNetState(impl, wantMaximized, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
				w.maximized = wantMaximized;
			}
			ApplyUnmappedState(impl);
			return;
		}

		if (!wantFullscreen && w.fullscreen) {
			SetNetState(impl, false, atoms.netWmStateFullscreen, 0);
		}
		if (!wantMaximized && w.maximized) {
			SetNetState(impl, false, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
		}
		Settle([&] { return (wantFullscreen || !w.fullscreen) && (wantMaximized || !w.maximized); });

		// maximized window restored by window manager to rect it had before
		if (!w.fullscreen && !w.maximized && (GetPosition(impl) != placement.position || w.clientSize != placement.clientSize)) {
			MoveResize(impl, placement.position, placement.clientSize);
		}
		if (wantFullscreen && !w.fullscreen) {
			const u32 before = w.configures;
			SetNetState(impl, true, atoms.netWmStateFullscreen, 0);
			Settle([&] { return w.fullscreen && w.configures != before; });
		} else if (wantMaximized && !w.maximized) {
			const u32 before = w.configures;
			SetNetState(impl, true, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
			Settle([&] { return w.maximized && w.configures != before; });
		} else if (placement.state == WindowState::Minimized && !w.iconic) {
			x.XIconifyWindow(display, w.handle, screen);
			Settle([&] { return w.iconic; });
		}
		if (w.fullscreen || w.maximized) {
			w.normalPosition = placement.position;
			w.normalSize = placement.clientSize;
		}
	}

	void X11Server::Show(Window::Impl& impl) {
		X11Window& w = Native(impl);
		w.shown = true;
		x.XMapRaised(display, w.handle);
		Settle([&] { return w.mapped; });
	}

	void X11Server::Hide(Window::Impl& impl) {
		X11Window& w = Native(impl);
		w.shown = false;
		x.XWithdrawWindow(display, w.handle, screen);
		Settle([&] { return !w.mapped; });
		w.iconic = false;
	}

	void X11Server::Minimize(Window::Impl& impl) {
		X11Window& w = Native(impl);
		if (!w.mapped && !w.iconic) {
			Show(impl);
		}
		x.XIconifyWindow(display, w.handle, screen);
		Settle([&] { return w.iconic; });
	}

	void X11Server::Maximize(Window::Impl& impl) {
		X11Window& w = Native(impl);
		if (!w.mapped) {
			if (w.fullscreen) {
				SetNetState(impl, false, atoms.netWmStateFullscreen, 0);
				w.fullscreen = false;
			}
			SetNetState(impl, true, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
			w.maximized = true;
			Show(impl);
			Settle([&] { return w.maximized; });
			return;
		}
		if (w.fullscreen) {
			SetNetState(impl, false, atoms.netWmStateFullscreen, 0);
		}
		const Vec2I position = w.normalPosition;
		const Vec2I size = w.normalSize;
		const u32 before = w.configures;
		SetNetState(impl, true, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
		Settle([&] { return w.maximized && !w.fullscreen && w.configures != before; });
		if (w.maximized) {
			w.normalPosition = position;
			w.normalSize = size;
		}
	}

	void X11Server::Restore(Window::Impl& impl) {
		X11Window& w = Native(impl);
		if (w.iconic || !w.mapped) {
			Show(impl);
			return;
		}
		if (w.maximized) {
			const Vec2I position = w.normalPosition;
			const Vec2I size = w.normalSize;
			const u32 before = w.configures;
			SetNetState(impl, false, atoms.netWmStateMaximizedVert, atoms.netWmStateMaximizedHorz);
			Settle([&] { return !w.maximized && w.configures != before; });
			if (!w.maximized && (GetPosition(impl) != position || w.clientSize != size)) {
				MoveResize(impl, position, size);
			}
		}
	}

	void X11Server::Focus(Window::Impl& impl) {
		X11Window& w = Native(impl);
		if (!w.mapped) {
			return;
		}
		SendRootMessage(w.handle, atoms.netActiveWindow, 1, CurrentTime, 0, 0, 0);
		x.XRaiseWindow(display, w.handle);
		x.XSetInputFocus(display, w.handle, RevertToParent, CurrentTime);
	}

	bool X11Server::IsVisible(const Window::Impl& impl) {
		return Native(impl).shown;
	}

	bool X11Server::IsMinimized(const Window::Impl& impl) {
		return Native(impl).iconic;
	}

	bool X11Server::IsMaximized(const Window::Impl& impl) {
		return Native(impl).maximized;
	}

	void X11Server::SetTitle(Window::Impl& impl) {
		X11Window& w = Native(impl);
		x.XChangeProperty(display, w.handle, atoms.netWmName, atoms.utf8String, 8, PropModeReplace,
						  reinterpret_cast<const unsigned char*>(impl.title.data()), static_cast<int>(impl.title.size()));
		x.XStoreName(display, w.handle, impl.title.c_str());
	}

	void X11Server::SetIcon(Window::Impl& impl, std::span<const WindowIconImage> images) {
		X11Window& w = Native(impl);
		size_t total = 0;
		for (const WindowIconImage& image : images) {
			if (WindowInternal::IsUsableIcon(image)) {
				total += 2 + static_cast<size_t>(image.size.x) * static_cast<size_t>(image.size.y);
			}
		}
		if (total == 0) {
			x.XDeleteProperty(display, w.handle, atoms.netWmIcon);
			return;
		}
		// Xlib format 32 data = long (8 bytes on 64 bit)
		Vector<long> cardinals{GetMemoryResource(MemTag::Platform)};
		cardinals.reserve(total);
		for (const WindowIconImage& image : images) {
			if (!WindowInternal::IsUsableIcon(image)) {
				continue;
			}
			cardinals.push_back(image.size.x);
			cardinals.push_back(image.size.y);
			const size_t count = static_cast<size_t>(image.size.x) * static_cast<size_t>(image.size.y);
			for (size_t i = 0; i < count; ++i) {
				const u8* texel = image.rgba.data() + i * 4;
				const u32 argb = (static_cast<u32>(texel[3]) << 24) | (static_cast<u32>(texel[0]) << 16) |
								 (static_cast<u32>(texel[1]) << 8) | static_cast<u32>(texel[2]);
				cardinals.push_back(static_cast<long>(argb));
			}
		}
		x.XChangeProperty(display, w.handle, atoms.netWmIcon, XA_CARDINAL, 32, PropModeReplace,
						  reinterpret_cast<const unsigned char*>(cardinals.data()), static_cast<int>(cardinals.size()));
	}

	NativeWindowHandle X11Server::GetNativeHandle(const Window::Impl& impl) {
		return {NativeWindowSystem::Xlib, display, reinterpret_cast<void*>(static_cast<std::uintptr_t>(Native(impl).handle))};
	}

	NativeWindowHandle X11Server::GetPresentationProbe() {
		const VisualID visual = DefaultVisual(display, screen)->visualid;
		return {NativeWindowSystem::Xlib, display, reinterpret_cast<void*>(static_cast<std::uintptr_t>(visual))};
	}

	void X11Server::RefreshFrameExtents(Window::Impl& impl) {
		X11Window& w = Native(impl);
		Vector<u8> raw{GetMemoryResource(MemTag::Platform)};
		if (ReadProperty(w.handle, atoms.netFrameExtents, XA_CARDINAL, raw) && raw.size() >= 4 * sizeof(long)) {
			const long* extents = reinterpret_cast<const long*>(raw.data());
			w.frameLeftTop = {static_cast<i32>(extents[0]), static_cast<i32>(extents[2])};
			w.frameRightBottom = {static_cast<i32>(extents[1]), static_cast<i32>(extents[3])};
		}
	}

	void X11Server::RefreshState(Window::Impl& impl) {
		X11Window& w = Native(impl);
		if (w.mapped) {
			bool vertical = false;
			bool horizontal = false;
			bool fullscreen = false;
			bool hidden = false;
			Vector<u8> raw{GetMemoryResource(MemTag::Platform)};
			if (ReadProperty(w.handle, atoms.netWmState, XA_ATOM, raw)) {
				const Atom* values = reinterpret_cast<const Atom*>(raw.data());
				for (size_t i = 0; i < raw.size() / sizeof(Atom); ++i) {
					vertical = vertical || values[i] == atoms.netWmStateMaximizedVert;
					horizontal = horizontal || values[i] == atoms.netWmStateMaximizedHorz;
					fullscreen = fullscreen || values[i] == atoms.netWmStateFullscreen;
					hidden = hidden || values[i] == atoms.netWmStateHidden;
				}
			}
			w.maximized = vertical && horizontal;
			w.fullscreen = fullscreen;
			w.iconic = hidden;
		}
		const WindowState state = w.iconic		 ? WindowState::Minimized
								  : w.fullscreen ? WindowState::Fullscreen
								  : w.maximized	 ? WindowState::Maximized
												 : WindowState::Normal;
		if (state == impl.stateCache) {
			return;
		}
		impl.stateCache = state;
		if (CanPublish(impl)) {
			WindowEvents::StateChanged.Publish(EventBus::GetGlobal(), WindowStateEvent{impl.self, state});
		}
	}

	namespace {

		DisplayServer* ConnectX11(str& outWhy) {
			auto* server = New<X11Server>(MemTag::Platform);
			if (!server->Connect(outWhy)) {
				Delete(server);
				return nullptr;
			}
			return server;
		}

	} // anonymous namespace

	extern const ServerCandidate X11Candidate = {"x11", &ConnectX11};

} // namespace Shalltear::Linux
