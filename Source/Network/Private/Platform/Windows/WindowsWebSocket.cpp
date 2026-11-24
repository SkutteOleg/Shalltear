#include "WebSocketBackend.h"

#include "WebSocketNative.h"

namespace Shalltear::Net::WebSocketBackend {

	bool IsAvailable() {
		return true;
	}

	std::shared_ptr<WebSocketCore> Connect(std::string_view url, TLSOptions tlsOptions) {
		return WebSocketNative::Connect(url, std::move(tlsOptions));
	}

	std::shared_ptr<WebSocketCore> AcceptTCP(std::unique_ptr<TCPSocket> socket) {
		return WebSocketNative::AcceptTCP(std::move(socket));
	}

	std::shared_ptr<WebSocketCore> AcceptTLS(std::unique_ptr<TLSStream> stream) {
		return WebSocketNative::AcceptTLS(std::move(stream));
	}

} // namespace Shalltear::Net::WebSocketBackend
