#include "WebSocketBackend.h"

#include <cstring>
#include <deque>
#include <mutex>

#include <emscripten/websocket.h>

namespace Shalltear::Net::WebSocketBackend {

	namespace {

		struct BrowserWebSocketCore final : WebSocketCore, std::enable_shared_from_this<BrowserWebSocketCore> {
			~BrowserWebSocketCore() override {
				DeleteSocketLocked();
			}

			SocketBackend::Handle PollHandle() override {
				return SocketBackend::INVALID;
			}

			void Pump(bool, bool) override {
				Vector<WebSocketEvent> events{GetMemoryResource(MemTag::Platform)};
				WebSocketCallback fire;
				{
					std::lock_guard<std::mutex> guard(lock);
					if (callback) {
						if ((state == WebSocketState::Open || state == WebSocketState::Closing) && !openedReported) {
							openedReported = true;
							WebSocketEvent ev;
							ev.type = WebSocketEventType::Opened;
							events.push_back(std::move(ev));
						}
						while (!queuedMessages.empty()) {
							WebSocketEvent ev;
							ev.type = WebSocketEventType::Message;
							ev.message = std::move(queuedMessages.front());
							queuedMessages.pop_front();
							events.push_back(std::move(ev));
						}
						if ((state == WebSocketState::Closed || state == WebSocketState::Failed) && !endReported) {
							endReported = true;
							WebSocketEvent ev;
							ev.type = state == WebSocketState::Closed ? WebSocketEventType::Closed : WebSocketEventType::Failed;
							ev.closeCode = closeCode;
							ev.reason = state == WebSocketState::Closed ? closeReason : error;
							events.push_back(std::move(ev));
						}
						fire = callback;
					}
				}
				if (fire) {
					for (const WebSocketEvent& ev : events) {
						fire(ev);
					}
				}
			}

			WebSocketState GetState() override {
				std::lock_guard<std::mutex> guard(lock);
				return state;
			}

			bool SendText(std::string_view text) override {
				std::lock_guard<std::mutex> guard(lock);
				if (state != WebSocketState::Open || socket <= 0) {
					return false;
				}
				const str copy(text);
				return emscripten_websocket_send_utf8_text(socket, copy.c_str()) == EMSCRIPTEN_RESULT_SUCCESS;
			}

			bool SendBinary(std::span<const u8> data) override {
				std::lock_guard<std::mutex> guard(lock);
				if (state != WebSocketState::Open || socket <= 0) {
					return false;
				}
				return emscripten_websocket_send_binary(socket, const_cast<u8*>(data.data()),
														static_cast<u32>(data.size())) == EMSCRIPTEN_RESULT_SUCCESS;
			}

			void Close(u16 code, std::string_view reason) override {
				std::lock_guard<std::mutex> guard(lock);
				if (state == WebSocketState::Closed || state == WebSocketState::Failed) {
					return;
				}
				closeCode = code;
				closeReason = str(reason);
				state = WebSocketState::Closing;
				if (socket > 0) {
					const str copy(reason);
					emscripten_websocket_close(socket, code, copy.c_str());
				}
			}

			bool PollMessage(WebSocketMessage& outMessage) override {
				std::lock_guard<std::mutex> guard(lock);
				if (queuedMessages.empty()) {
					return false;
				}
				outMessage = std::move(queuedMessages.front());
				queuedMessages.pop_front();
				return true;
			}

			str GetError() const override {
				std::lock_guard<std::mutex> guard(lock);
				return error;
			}

			u16 GetCloseCode() const override {
				std::lock_guard<std::mutex> guard(lock);
				return closeCode;
			}

			void SetCallback(WebSocketCallback cb) override {
				std::lock_guard<std::mutex> guard(lock);
				callback = std::move(cb);
			}

			void DetachOwner() override {
				std::lock_guard<std::mutex> guard(lock);
				callback = nullptr;
				endReported = true;
				if (socket > 0) {
					emscripten_websocket_set_onopen_callback(socket, nullptr, nullptr);
					emscripten_websocket_set_onmessage_callback(socket, nullptr, nullptr);
					emscripten_websocket_set_onclose_callback(socket, nullptr, nullptr);
					emscripten_websocket_set_onerror_callback(socket, nullptr, nullptr);
					emscripten_websocket_close(socket, 1000, "closed");
					DeleteSocketLocked();
				}
				if (state != WebSocketState::Failed) {
					state = WebSocketState::Closed;
				}
			}

			void DeleteSocketLocked() {
				if (socket > 0) {
					emscripten_websocket_delete(socket);
					socket = 0;
				}
				if (selfBox) {
					delete selfBox;
					selfBox = nullptr;
				}
			}

			static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent*, void* userData) {
				auto* box = static_cast<std::weak_ptr<BrowserWebSocketCore>*>(userData);
				if (std::shared_ptr<BrowserWebSocketCore> self = box ? box->lock() : nullptr) {
					std::lock_guard<std::mutex> guard(self->lock);
					if (self->state == WebSocketState::Connecting) {
						self->state = WebSocketState::Open;
					}
				}
				return EM_TRUE;
			}

			static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent* e, void* userData) {
				auto* box = static_cast<std::weak_ptr<BrowserWebSocketCore>*>(userData);
				if (std::shared_ptr<BrowserWebSocketCore> self = box ? box->lock() : nullptr; self && e) {
					WebSocketMessage msg;
					msg.kind = e->isText ? WebSocketMessageKind::Text : WebSocketMessageKind::Binary;
					u32 length = e->numBytes;
					if (e->isText && length > 0 && e->data && e->data[length - 1] == '\0') {
						--length;
					}
					if (e->data && length > 0) {
						msg.payload.assign(e->data, e->data + length);
					}
					std::lock_guard<std::mutex> guard(self->lock);
					self->queuedMessages.push_back(std::move(msg));
				}
				return EM_TRUE;
			}

			static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent* e, void* userData) {
				auto* box = static_cast<std::weak_ptr<BrowserWebSocketCore>*>(userData);
				if (std::shared_ptr<BrowserWebSocketCore> self = box ? box->lock() : nullptr) {
					std::lock_guard<std::mutex> guard(self->lock);
					if (e) {
						self->closeCode = e->code;
						if (e->reason[0]) {
							self->closeReason = e->reason;
						}
					}
					if (self->state != WebSocketState::Failed) {
						self->state = WebSocketState::Closed;
					}
				}
				return EM_TRUE;
			}

			static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent*, void* userData) {
				auto* box = static_cast<std::weak_ptr<BrowserWebSocketCore>*>(userData);
				if (std::shared_ptr<BrowserWebSocketCore> self = box ? box->lock() : nullptr) {
					std::lock_guard<std::mutex> guard(self->lock);
					self->error = "WebSocket connection error";
					self->state = WebSocketState::Failed;
				}
				return EM_TRUE;
			}

			mutable std::mutex lock;
			EMSCRIPTEN_WEBSOCKET_T socket = 0;
			std::weak_ptr<BrowserWebSocketCore>* selfBox = nullptr;
			WebSocketState state = WebSocketState::Connecting;
			str error;
			str closeReason;
			u16 closeCode = 1000;
			bool openedReported = false;
			bool endReported = false;
			std::deque<WebSocketMessage> queuedMessages;
			WebSocketCallback callback;
		};

	} // anonymous namespace

	bool IsAvailable() {
		return emscripten_websocket_is_supported() != 0;
	}

	std::shared_ptr<WebSocketCore> Connect(std::string_view url, TLSOptions) {
		auto core = std::make_shared<BrowserWebSocketCore>();
		if (!IsAvailable()) {
			core->error = "WebSockets are not supported in this browser environment";
			core->state = WebSocketState::Failed;
			return core;
		}
		const str urlCopy(url);
		EmscriptenWebSocketCreateAttributes attrs = {
			urlCopy.c_str(),
			nullptr,
			EM_TRUE,
		};
		const EMSCRIPTEN_WEBSOCKET_T handle = emscripten_websocket_new(&attrs);
		if (handle <= 0) {
			core->error = "emscripten_websocket_new failed";
			core->state = WebSocketState::Failed;
			return core;
		}
		core->socket = handle;
		core->selfBox = new std::weak_ptr<BrowserWebSocketCore>(core);
		emscripten_websocket_set_onopen_callback(handle, core->selfBox, &BrowserWebSocketCore::OnOpen);
		emscripten_websocket_set_onmessage_callback(handle, core->selfBox, &BrowserWebSocketCore::OnMessage);
		emscripten_websocket_set_onclose_callback(handle, core->selfBox, &BrowserWebSocketCore::OnClose);
		emscripten_websocket_set_onerror_callback(handle, core->selfBox, &BrowserWebSocketCore::OnError);
		return core;
	}

	std::shared_ptr<WebSocketCore> AcceptTCP(std::unique_ptr<TCPSocket>) {
		auto core = std::make_shared<BrowserWebSocketCore>();
		core->error = "WebSocket server accept is unsupported on Web";
		core->state = WebSocketState::Failed;
		return core;
	}

	std::shared_ptr<WebSocketCore> AcceptTLS(std::unique_ptr<TLSStream>) {
		auto core = std::make_shared<BrowserWebSocketCore>();
		core->error = "WebSocket server accept is unsupported on Web";
		core->state = WebSocketState::Failed;
		return core;
	}

} // namespace Shalltear::Net::WebSocketBackend
