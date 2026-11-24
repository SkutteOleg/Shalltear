#include "Shalltear/Network/WebSocket.h"

#include "WebSocketBackend.h"

namespace Shalltear::Net {

	WebSocket::WebSocket(std::shared_ptr<WebSocketCore> core) : _core(std::move(core)) {
		if (_core) {
			Dispatcher::Watch(_core);
		}
	}

	WebSocket::~WebSocket() {
		if (_core) {
			_core->DetachOwner();
		}
	}

	std::unique_ptr<WebSocket> WebSocket::Connect(std::string_view url, TLSOptions tlsOptions) {
		return std::make_unique<WebSocket>(WebSocketBackend::Connect(url, std::move(tlsOptions)));
	}

	std::unique_ptr<WebSocket> WebSocket::Accept(std::unique_ptr<TCPSocket> socket) {
		return std::make_unique<WebSocket>(WebSocketBackend::AcceptTCP(std::move(socket)));
	}

	std::unique_ptr<WebSocket> WebSocket::Accept(std::unique_ptr<TLSStream> stream) {
		return std::make_unique<WebSocket>(WebSocketBackend::AcceptTLS(std::move(stream)));
	}

	bool WebSocket::IsAvailable() {
		return WebSocketBackend::IsAvailable();
	}

	WebSocketState WebSocket::GetState() const {
		return _core ? _core->GetState() : WebSocketState::Closed;
	}

	bool WebSocket::SendText(std::string_view text) {
		return _core && _core->SendText(text);
	}

	bool WebSocket::SendBinary(std::span<const u8> data) {
		return _core && _core->SendBinary(data);
	}

	void WebSocket::Close(u16 code, std::string_view reason) {
		if (_core) {
			_core->Close(code, reason);
		}
	}

	bool WebSocket::PollMessage(WebSocketMessage& outMessage) {
		return _core && _core->PollMessage(outMessage);
	}

	str WebSocket::GetError() const {
		return _core ? _core->GetError() : str{};
	}

	u16 WebSocket::GetCloseCode() const {
		return _core ? _core->GetCloseCode() : 1000;
	}

	void WebSocket::SetCallback(WebSocketCallback callback) {
		if (_core) {
			_core->SetCallback(std::move(callback));
		}
	}

} // namespace Shalltear::Net
