#pragma once

#include "WebSocketBackend.h"

namespace Shalltear::Net::WebSocketNative {

	std::shared_ptr<WebSocketCore> Connect(std::string_view url, TLSOptions tlsOptions);
	std::shared_ptr<WebSocketCore> AcceptTCP(std::unique_ptr<TCPSocket> socket);
	std::shared_ptr<WebSocketCore> AcceptTLS(std::unique_ptr<TLSStream> stream);

} // namespace Shalltear::Net::WebSocketNative
