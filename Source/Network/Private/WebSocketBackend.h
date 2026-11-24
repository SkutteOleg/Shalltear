#pragma once

#include "NetworkInternal.h"

#include "Shalltear/Network/WebSocket.h"

#include <memory>
#include <span>
#include <string_view>

namespace Shalltear::Net {

	struct WebSocketCore : WatchedCore {
		virtual WebSocketState GetState() = 0;
		virtual bool SendText(std::string_view text) = 0;
		virtual bool SendBinary(std::span<const u8> data) = 0;
		virtual void Close(u16 code, std::string_view reason) = 0;
		virtual bool PollMessage(WebSocketMessage& outMessage) = 0;
		virtual str GetError() const = 0;
		virtual u16 GetCloseCode() const = 0;
		virtual void SetCallback(WebSocketCallback callback) = 0;
		virtual void DetachOwner() = 0;
	};

	namespace WebSocketBackend {

		bool IsAvailable();
		std::shared_ptr<WebSocketCore> Connect(std::string_view url, TLSOptions tlsOptions);
		std::shared_ptr<WebSocketCore> AcceptTCP(std::unique_ptr<TCPSocket> socket);
		std::shared_ptr<WebSocketCore> AcceptTLS(std::unique_ptr<TLSStream> stream);

	} // namespace WebSocketBackend

} // namespace Shalltear::Net
