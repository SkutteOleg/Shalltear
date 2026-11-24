#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Network/NetworkAPI.h"
#include "Shalltear/Network/Socket.h"
#include "Shalltear/Network/TLS.h"

#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace Shalltear::Net {

	enum class WebSocketState : u8 {
		Connecting,
		Open,
		Closing,
		Closed,
		Failed,
	};

	enum class WebSocketMessageKind : u8 {
		Text,
		Binary,
	};

	struct SHALLTEAR_NETWORK_API WebSocketMessage {
		WebSocketMessageKind kind = WebSocketMessageKind::Binary;
		Vector<u8> payload{GetMemoryResource(MemTag::Platform)};

		std::string_view AsText() const {
			return {reinterpret_cast<const char*>(payload.data()), payload.size()};
		}
	};

	enum class WebSocketEventType : u8 {
		Opened,
		Message,
		Closed,
		Failed,
	};

	struct WebSocketEvent {
		WebSocketEventType type = WebSocketEventType::Opened;
		WebSocketMessage message;
		u16 closeCode = 1000;
		str reason;
	};

	// invoked from Net::Dispatch (never from background threads)
	using WebSocketCallback = std::function<void(const WebSocketEvent& event)>;

	struct WebSocketCore;

	class SHALLTEAR_NETWORK_API WebSocket {
	public:
		explicit WebSocket(std::shared_ptr<WebSocketCore> core);
		~WebSocket();
		WebSocket(const WebSocket&) = delete;
		WebSocket& operator=(const WebSocket&) = delete;

		// ws:// or wss:// client connection
		static std::unique_ptr<WebSocket> Connect(std::string_view url, TLSOptions tlsOptions = {});
		// server-side upgrade from accepted stream
		static std::unique_ptr<WebSocket> Accept(std::unique_ptr<TCPSocket> socket);
		static std::unique_ptr<WebSocket> Accept(std::unique_ptr<TLSStream> stream);
		static bool IsAvailable();

		WebSocketState GetState() const;
		bool SendText(std::string_view text);
		bool SendBinary(std::span<const u8> data);
		void Close(u16 code = 1000, std::string_view reason = {});

		// pops next queued message when no callback is consuming them
		bool PollMessage(WebSocketMessage& outMessage);
		str GetError() const;
		u16 GetCloseCode() const;
		void SetCallback(WebSocketCallback callback);
	private:
		std::shared_ptr<WebSocketCore> _core;
	};

} // namespace Shalltear::Net
