#include "TestsPCH.h"

#include "Shalltear/Network/Socket.h"
#include "Shalltear/Network/WebSocket.h"

#include <chrono>
#include <format>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::Net;

namespace {

	template <typename TPredicate>
	bool WaitUntil(TPredicate&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			Net::Dispatch();
			if (predicate()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return predicate();
	}

} // anonymous namespace

TEST(WebSocketTest, ClientServerTextBinaryAndClose) {
	ASSERT_TRUE(WebSocket::IsAvailable());

	str listenError;
	std::unique_ptr<TCPListener> listener = TCPListener::Listen(Address::Loopback(0), &listenError);
	ASSERT_NE(listener, nullptr) << listenError;
	const u16 port = listener->GetPort();
	ASSERT_GT(port, 0u);

	const str url = std::format("ws://127.0.0.1:{}/game", port);
	std::unique_ptr<WebSocket> client = WebSocket::Connect(url);
	ASSERT_NE(client, nullptr);

	std::unique_ptr<TCPSocket> accepted;
	ASSERT_TRUE(WaitUntil([&] {
		accepted = listener->Accept();
		return accepted != nullptr;
	}));

	std::unique_ptr<WebSocket> server = WebSocket::Accept(std::move(accepted));
	ASSERT_NE(server, nullptr);

	ASSERT_TRUE(WaitUntil([&] {
		return client->GetState() == WebSocketState::Open && server->GetState() == WebSocketState::Open;
	})) << "client="
		<< static_cast<int>(client->GetState()) << " err=" << client->GetError()
		<< " server=" << static_cast<int>(server->GetState()) << " err=" << server->GetError();

	// Text round-trip (client -> server)
	ASSERT_TRUE(client->SendText("hello shalltear"));
	WebSocketMessage received;
	ASSERT_TRUE(WaitUntil([&] { return server->PollMessage(received); }));
	EXPECT_EQ(received.kind, WebSocketMessageKind::Text);
	EXPECT_EQ(received.AsText(), "hello shalltear");

	// Binary round-trip (> 125 bytes to exercise 16-bit extended length framing, server -> client)
	std::vector<u8> payload(2048);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<u8>((i * 31 + 7) & 0xFFu);
	}
	ASSERT_TRUE(server->SendBinary(payload));

	bool callbackFired = false;
	WebSocketMessage clientMsg;
	client->SetCallback([&](const WebSocketEvent& ev) {
		if (ev.type == WebSocketEventType::Message) {
			callbackFired = true;
			clientMsg = ev.message;
		}
	});
	ASSERT_TRUE(WaitUntil([&] { return callbackFired; }));
	EXPECT_EQ(clientMsg.kind, WebSocketMessageKind::Binary);
	ASSERT_EQ(clientMsg.payload.size(), payload.size());
	EXPECT_EQ(std::memcmp(clientMsg.payload.data(), payload.data(), payload.size()), 0);

	// Clean close handshake
	client->Close(1000, "bye");
	ASSERT_TRUE(WaitUntil([&] {
		return client->GetState() == WebSocketState::Closed && server->GetState() == WebSocketState::Closed;
	}));
	EXPECT_EQ(server->GetCloseCode(), 1000u);
}

TEST(WebSocketTest, RejectsInvalidURLScheme) {
	std::unique_ptr<WebSocket> bad = WebSocket::Connect("http://127.0.0.1:8080/");
	ASSERT_NE(bad, nullptr);
	EXPECT_EQ(bad->GetState(), WebSocketState::Failed);
	EXPECT_FALSE(bad->GetError().empty());
}
