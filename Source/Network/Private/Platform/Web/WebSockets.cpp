#include "SocketBackend.h"

#include <arpa/inet.h>
#include <cstring>

namespace Shalltear::Net::SocketBackend {

	namespace {

		constexpr const char* UNSUPPORTED_MESSAGE = "raw sockets are unsupported on Web";

	} // anonymous namespace

	bool Startup(str&) {
		return true;
	}

	Handle Open(bool, bool, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return INVALID;
	}

	void Close(Handle) {
	}

	Result Connect(Handle, const Address&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	Result ConnectResult(Handle, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	Result Send(Handle, std::span<const u8>, size_t& sent, str& error) {
		sent = 0;
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	Result Receive(Handle, std::span<u8>, size_t& received, str& error) {
		received = 0;
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	bool Bind(Handle, const Address&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

	bool Listen(Handle, u32, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

	Result Accept(Handle, Handle& accepted, Address&, str& error) {
		accepted = INVALID;
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	Result SendTo(Handle, const Address&, std::span<const u8>, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	Result ReceiveFrom(Handle, std::span<u8>, size_t& received, Address&, str& error) {
		received = 0;
		error = UNSUPPORTED_MESSAGE;
		return Result::Error;
	}

	bool LocalAddress(Handle, Address&) {
		return false;
	}

	bool PeerAddress(Handle, Address&) {
		return false;
	}

	void SetNoDelay(Handle, bool) {
	}

	void PollReadable(std::span<PollItem>) {
	}

	bool Resolve(std::string_view, u16, Vector<Address>&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

	bool ParseIP(std::string_view text, Address& out) {
		str copy(text);
		out = Address{};
		if (::inet_pton(AF_INET, copy.c_str(), out.bytes) == 1) {
			out.v6 = false;
			return true;
		}
		if (::inet_pton(AF_INET6, copy.c_str(), out.bytes) == 1) {
			out.v6 = true;
			return true;
		}
		return false;
	}

	str FormatIP(const Address& address) {
		char buffer[INET6_ADDRSTRLEN] = {};
		const char* result = ::inet_ntop(address.v6 ? AF_INET6 : AF_INET, address.bytes, buffer, sizeof(buffer));
		return result ? str(result) : str{};
	}

} // namespace Shalltear::Net::SocketBackend
