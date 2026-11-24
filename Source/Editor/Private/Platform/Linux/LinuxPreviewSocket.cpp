#include "Platform/PreviewSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Shalltear::EditorPlatform::PreviewSocket {

	Handle ListenLoopback(u16& outPort) {
		outPort = 0;
		int fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return INVALID;
		}
		int reuse = 1;
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
			::listen(fd, 16) != 0) {
			::close(fd);
			return INVALID;
		}

		sockaddr_in bound{};
		socklen_t boundLen = sizeof(bound);
		if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
			::close(fd);
			return INVALID;
		}
		outPort = ::ntohs(bound.sin_port);
		return static_cast<Handle>(fd);
	}

	Handle AcceptWithTimeout(Handle listener, i32 timeoutMs) {
		if (listener == INVALID) {
			return INVALID;
		}
		pollfd item{};
		item.fd = static_cast<int>(listener);
		item.events = POLLIN;
		if (::poll(&item, 1, timeoutMs) <= 0 || !(item.revents & POLLIN)) {
			return INVALID;
		}
		int client = ::accept(static_cast<int>(listener), nullptr, nullptr);
		if (client < 0) {
			return INVALID;
		}
		timeval tv{};
		tv.tv_sec = 2;
		::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		return static_cast<Handle>(client);
	}

	i32 Receive(Handle socket, std::span<u8> buffer) {
		if (socket == INVALID || buffer.empty()) {
			return -1;
		}
		return static_cast<i32>(::recv(static_cast<int>(socket), buffer.data(), buffer.size(), 0));
	}

	bool SendAll(Handle socket, std::span<const u8> data) {
		if (socket == INVALID) {
			return false;
		}
		size_t sentTotal = 0;
		while (sentTotal < data.size()) {
			const ssize_t sent = ::send(static_cast<int>(socket), data.data() + sentTotal,
										data.size() - sentTotal, MSG_NOSIGNAL);
			if (sent <= 0) {
				return false;
			}
			sentTotal += static_cast<size_t>(sent);
		}
		return true;
	}

	void Close(Handle socket) {
		if (socket != INVALID) {
			::close(static_cast<int>(socket));
		}
	}

} // namespace Shalltear::EditorPlatform::PreviewSocket
