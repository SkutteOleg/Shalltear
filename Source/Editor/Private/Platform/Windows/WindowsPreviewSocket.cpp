#include "Platform/PreviewSocket.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>

namespace Shalltear::EditorPlatform::PreviewSocket {

	namespace {

		std::once_flag _wsaOnce;

		void EnsureWinsock() {
			std::call_once(_wsaOnce, [] {
				WSADATA data{};
				::WSAStartup(MAKEWORD(2, 2), &data);
			});
		}

		SOCKET ToSocket(Handle handle) {
			return handle == INVALID ? INVALID_SOCKET : static_cast<SOCKET>(handle);
		}

	} // anonymous namespace

	Handle ListenLoopback(u16& outPort) {
		EnsureWinsock();
		outPort = 0;

		SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET) {
			return INVALID;
		}
		BOOL reuse = TRUE;
		::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
			::listen(sock, 16) != 0) {
			::closesocket(sock);
			return INVALID;
		}

		sockaddr_in bound{};
		int boundLen = sizeof(bound);
		if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
			::closesocket(sock);
			return INVALID;
		}
		outPort = ::ntohs(bound.sin_port);
		return static_cast<Handle>(sock);
	}

	Handle AcceptWithTimeout(Handle listener, i32 timeoutMs) {
		SOCKET sock = ToSocket(listener);
		if (sock == INVALID_SOCKET) {
			return INVALID;
		}
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(sock, &readSet);
		timeval tv{};
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;
		const int ready = ::select(0, &readSet, nullptr, nullptr, &tv);
		if (ready <= 0) {
			return INVALID;
		}
		SOCKET client = ::accept(sock, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			return INVALID;
		}
		DWORD timeout = 2000;
		::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		return static_cast<Handle>(client);
	}

	i32 Receive(Handle socket, std::span<u8> buffer) {
		SOCKET sock = ToSocket(socket);
		if (sock == INVALID_SOCKET || buffer.empty()) {
			return -1;
		}
		return ::recv(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
	}

	bool SendAll(Handle socket, std::span<const u8> data) {
		SOCKET sock = ToSocket(socket);
		if (sock == INVALID_SOCKET) {
			return false;
		}
		size_t sentTotal = 0;
		while (sentTotal < data.size()) {
			const int sent = ::send(sock, reinterpret_cast<const char*>(data.data() + sentTotal),
									static_cast<int>(data.size() - sentTotal), 0);
			if (sent <= 0) {
				return false;
			}
			sentTotal += static_cast<size_t>(sent);
		}
		return true;
	}

	void Close(Handle socket) {
		SOCKET sock = ToSocket(socket);
		if (sock != INVALID_SOCKET) {
			::closesocket(sock);
		}
	}

} // namespace Shalltear::EditorPlatform::PreviewSocket
