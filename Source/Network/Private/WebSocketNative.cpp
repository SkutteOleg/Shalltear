#include "WebSocketNative.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <random>

namespace Shalltear::Net::WebSocketNative {

	namespace {

		constexpr std::string_view HANDSHAKE_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		constexpr size_t MAX_HANDSHAKE_BYTES = 64 * 1024;
		constexpr size_t MAX_MESSAGE_BYTES = 32 * 1024 * 1024;

		constexpr u8 OPCODE_CONTINUATION = 0x0;
		constexpr u8 OPCODE_TEXT = 0x1;
		constexpr u8 OPCODE_BINARY = 0x2;
		constexpr u8 OPCODE_CLOSE = 0x8;
		constexpr u8 OPCODE_PING = 0x9;
		constexpr u8 OPCODE_PONG = 0xA;

		u32 RotateLeft(u32 value, u32 bits) {
			return (value << bits) | (value >> (32u - bits));
		}

		// RFC 3174 SHA-1 solely for RFC 6455 Sec-WebSocket-Accept handshake
		std::array<u8, 20> ComputeHandshakeHash(std::string_view data) {
			u32 h0 = 0x67452301u;
			u32 h1 = 0xEFCDAB89u;
			u32 h2 = 0x98BADCFEu;
			u32 h3 = 0x10325476u;
			u32 h4 = 0xC3D2E1F0u;

			const u64 bitLen = static_cast<u64>(data.size()) * 8ull;
			Vector<u8> padded{GetMemoryResource(MemTag::Platform)};
			padded.reserve(data.size() + 72);
			for (char c : data) {
				padded.push_back(static_cast<u8>(c));
			}
			padded.push_back(0x80u);
			while ((padded.size() % 64) != 56) {
				padded.push_back(0);
			}
			for (int shift = 56; shift >= 0; shift -= 8) {
				padded.push_back(static_cast<u8>((bitLen >> shift) & 0xFFu));
			}

			u32 w[80];
			for (size_t offset = 0; offset < padded.size(); offset += 64) {
				for (u32 i = 0; i < 16; ++i) {
					const size_t at = offset + i * 4;
					w[i] = (static_cast<u32>(padded[at]) << 24) |
						   (static_cast<u32>(padded[at + 1]) << 16) |
						   (static_cast<u32>(padded[at + 2]) << 8) |
						   static_cast<u32>(padded[at + 3]);
				}
				for (u32 i = 16; i < 80; ++i) {
					w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
				}

				u32 a = h0;
				u32 b = h1;
				u32 c = h2;
				u32 d = h3;
				u32 e = h4;
				for (u32 i = 0; i < 80; ++i) {
					u32 f = 0;
					u32 k = 0;
					if (i < 20) {
						f = (b & c) | ((~b) & d);
						k = 0x5A827999u;
					} else if (i < 40) {
						f = b ^ c ^ d;
						k = 0x6ED9EBA1u;
					} else if (i < 60) {
						f = (b & c) | (b & d) | (c & d);
						k = 0x8F1BBCDCu;
					} else {
						f = b ^ c ^ d;
						k = 0xCA62C1D6u;
					}
					const u32 temp = RotateLeft(a, 5) + f + e + k + w[i];
					e = d;
					d = c;
					c = RotateLeft(b, 30);
					b = a;
					a = temp;
				}
				h0 += a;
				h1 += b;
				h2 += c;
				h3 += d;
				h4 += e;
			}

			std::array<u8, 20> digest{};
			const u32 words[5] = {h0, h1, h2, h3, h4};
			for (size_t i = 0; i < 5; ++i) {
				digest[i * 4 + 0] = static_cast<u8>((words[i] >> 24) & 0xFFu);
				digest[i * 4 + 1] = static_cast<u8>((words[i] >> 16) & 0xFFu);
				digest[i * 4 + 2] = static_cast<u8>((words[i] >> 8) & 0xFFu);
				digest[i * 4 + 3] = static_cast<u8>(words[i] & 0xFFu);
			}
			return digest;
		}

		str EncodeBase64(std::span<const u8> bytes) {
			static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			str out;
			out.reserve(((bytes.size() + 2) / 3) * 4);
			for (size_t i = 0; i < bytes.size(); i += 3) {
				const u32 b0 = bytes[i];
				const u32 b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
				const u32 b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
				const u32 triple = (b0 << 16) | (b1 << 8) | b2;
				out.push_back(ALPHABET[(triple >> 18) & 0x3Fu]);
				out.push_back(ALPHABET[(triple >> 12) & 0x3Fu]);
				out.push_back(i + 1 < bytes.size() ? ALPHABET[(triple >> 6) & 0x3Fu] : '=');
				out.push_back(i + 2 < bytes.size() ? ALPHABET[triple & 0x3Fu] : '=');
			}
			return out;
		}

		str ComputeAcceptKey(std::string_view clientKey) {
			str combined;
			combined.reserve(clientKey.size() + HANDSHAKE_GUID.size());
			combined.append(clientKey);
			combined.append(HANDSHAKE_GUID);
			const std::array<u8, 20> digest = ComputeHandshakeHash(combined);
			return EncodeBase64(digest);
		}

		std::array<u8, 4> NextMaskKey() {
			static thread_local std::mt19937_64 generator(std::random_device{}());
			const u32 value = static_cast<u32>(generator());
			return {
				static_cast<u8>((value >> 24) & 0xFFu),
				static_cast<u8>((value >> 16) & 0xFFu),
				static_cast<u8>((value >> 8) & 0xFFu),
				static_cast<u8>(value & 0xFFu),
			};
		}

		str GenerateClientKey() {
			static thread_local std::mt19937_64 generator(std::random_device{}());
			u8 bytes[16];
			const u64 a = generator();
			const u64 b = generator();
			std::memcpy(bytes, &a, 8);
			std::memcpy(bytes + 8, &b, 8);
			return EncodeBase64(bytes);
		}

		char LowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				if (LowerAscii(a[i]) != LowerAscii(b[i])) {
					return false;
				}
			}
			return true;
		}

		std::string_view TrimAscii(std::string_view value) {
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
				value.remove_prefix(1);
			}
			while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
				value.remove_suffix(1);
			}
			return value;
		}

		std::string_view FindHeaderValue(std::string_view block, std::string_view name) {
			size_t at = block.find("\r\n");
			while (at != std::string_view::npos && at + 2 < block.size()) {
				const size_t start = at + 2;
				const size_t end = block.find("\r\n", start);
				const std::string_view line = block.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				const size_t colon = line.find(':');
				if (colon != std::string_view::npos && EqualsIgnoreCase(TrimAscii(line.substr(0, colon)), name)) {
					return TrimAscii(line.substr(colon + 1));
				}
				if (end == std::string_view::npos) {
					break;
				}
				at = end;
			}
			return {};
		}

		struct ParsedURL {
			bool secure = false;
			str host;
			str hostHeader;
			u16 port = 80;
			str target = "/";
		};

		bool ParseWebSocketURL(std::string_view url, ParsedURL& out, str& outError) {
			std::string_view rest = url;
			if (rest.starts_with("ws://")) {
				out.secure = false;
				out.port = 80;
				rest.remove_prefix(5);
			} else if (rest.starts_with("wss://")) {
				out.secure = true;
				out.port = 443;
				rest.remove_prefix(6);
			} else {
				outError = "WebSocket URL must start with ws:// or wss://";
				return false;
			}

			const size_t slash = rest.find_first_of("/?");
			std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
			if (slash != std::string_view::npos) {
				if (rest[slash] == '?') {
					out.target = "/" + str(rest.substr(slash));
				} else {
					out.target = str(rest.substr(slash));
				}
			} else {
				out.target = "/";
			}
			if (authority.empty()) {
				outError = "WebSocket URL has empty host";
				return false;
			}

			out.hostHeader = str(authority);
			std::string_view hostPart = authority;
			std::string_view portPart;
			if (authority.front() == '[') {
				const size_t close = authority.find(']');
				if (close == std::string_view::npos) {
					outError = "WebSocket URL has unclosed IPv6 host";
					return false;
				}
				hostPart = authority.substr(1, close - 1);
				if (close + 1 < authority.size()) {
					if (authority[close + 1] != ':') {
						outError = "WebSocket URL has malformed port";
						return false;
					}
					portPart = authority.substr(close + 2);
				}
			} else {
				const size_t colon = authority.rfind(':');
				if (colon != std::string_view::npos) {
					hostPart = authority.substr(0, colon);
					portPart = authority.substr(colon + 1);
				}
			}
			if (hostPart.empty()) {
				outError = "WebSocket URL has empty host";
				return false;
			}
			out.host = str(hostPart);
			if (!portPart.empty()) {
				char* endPtr = nullptr;
				str portCopy(portPart);
				const unsigned long parsed = std::strtoul(portCopy.c_str(), &endPtr, 10);
				if (!endPtr || *endPtr != '\0' || parsed == 0 || parsed > 65535) {
					outError = "WebSocket URL has invalid port";
					return false;
				}
				out.port = static_cast<u16>(parsed);
			}
			return true;
		}

		void AppendFrame(Vector<u8>& out, u8 opcode, std::span<const u8> payload, bool mask) {
			out.push_back(static_cast<u8>(0x80u | (opcode & 0x0Fu)));
			const u8 maskBit = mask ? 0x80u : 0x00u;
			const u64 len = payload.size();
			if (len <= 125) {
				out.push_back(static_cast<u8>(maskBit | len));
			} else if (len <= 65535) {
				out.push_back(static_cast<u8>(maskBit | 126u));
				out.push_back(static_cast<u8>((len >> 8) & 0xFFu));
				out.push_back(static_cast<u8>(len & 0xFFu));
			} else {
				out.push_back(static_cast<u8>(maskBit | 127u));
				for (int shift = 56; shift >= 0; shift -= 8) {
					out.push_back(static_cast<u8>((len >> shift) & 0xFFu));
				}
			}
			if (mask) {
				const std::array<u8, 4> key = NextMaskKey();
				out.insert(out.end(), key.begin(), key.end());
				const size_t base = out.size();
				out.resize(base + payload.size());
				for (size_t i = 0; i < payload.size(); ++i) {
					out[base + i] = static_cast<u8>(payload[i] ^ key[i & 3u]);
				}
			} else if (!payload.empty()) {
				out.insert(out.end(), payload.begin(), payload.end());
			}
		}

		struct NativeWebSocketCore final : WebSocketCore {
			enum class Role : u8 {
				Client,
				Server,
			};

			explicit NativeWebSocketCore(Role socketRole) : role(socketRole) {}

			SocketBackend::Handle PollHandle() override {
				return SocketBackend::INVALID;
			}

			void Pump(bool, bool) override {
				Vector<WebSocketEvent> events{GetMemoryResource(MemTag::Platform)};
				WebSocketCallback fire;
				{
					std::lock_guard<std::mutex> guard(lock);
					AdvanceLocked();
					CollectPendingEventsLocked(events);
					fire = callback;
				}
				if (fire) {
					for (const WebSocketEvent& ev : events) {
						fire(ev);
					}
				}
			}

			WebSocketState GetState() override {
				std::lock_guard<std::mutex> guard(lock);
				AdvanceLocked();
				return state;
			}

			bool SendText(std::string_view text) override {
				return SendDataFrame(OPCODE_TEXT,
									 {reinterpret_cast<const u8*>(text.data()), text.size()});
			}

			bool SendBinary(std::span<const u8> data) override {
				return SendDataFrame(OPCODE_BINARY, data);
			}

			void Close(u16 code, std::string_view reason) override {
				std::lock_guard<std::mutex> guard(lock);
				if (state == WebSocketState::Closed || state == WebSocketState::Failed) {
					return;
				}
				closeCode = code;
				closeReason = str(reason);
				if (state == WebSocketState::Open && !closeSent) {
					Vector<u8> payload{GetMemoryResource(MemTag::Platform)};
					payload.reserve(2 + reason.size());
					payload.push_back(static_cast<u8>((code >> 8) & 0xFFu));
					payload.push_back(static_cast<u8>(code & 0xFFu));
					for (char c : reason) {
						payload.push_back(static_cast<u8>(c));
					}
					AppendFrame(txBuffer, OPCODE_CLOSE, payload, role == Role::Client);
					closeSent = true;
					state = WebSocketState::Closing;
					FlushTxLocked();
				} else {
					CloseTransportLocked();
					state = WebSocketState::Closed;
				}
			}

			bool PollMessage(WebSocketMessage& outMessage) override {
				std::lock_guard<std::mutex> guard(lock);
				AdvanceLocked();
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
				CloseTransportLocked();
				if (state != WebSocketState::Failed) {
					state = WebSocketState::Closed;
				}
			}

			bool SendDataFrame(u8 opcode, std::span<const u8> data) {
				std::lock_guard<std::mutex> guard(lock);
				AdvanceLocked();
				if (state != WebSocketState::Open) {
					return false;
				}
				AppendFrame(txBuffer, opcode, data, role == Role::Client);
				FlushTxLocked();
				return state == WebSocketState::Open;
			}

			SocketState TransportStateLocked() const {
				if (tls) {
					return tls->GetState();
				}
				if (tcp) {
					return tcp->GetState();
				}
				return SocketState::Closed;
			}

			str TransportErrorLocked() const {
				if (tls) {
					return tls->GetError();
				}
				if (tcp) {
					return tcp->GetError();
				}
				return {};
			}

			size_t TransportSendLocked(std::span<const u8> data) {
				if (tls) {
					return tls->Send(data);
				}
				if (tcp) {
					return tcp->Send(data);
				}
				return 0;
			}

			size_t TransportReceiveLocked(std::span<u8> out) {
				if (tls) {
					return tls->Receive(out);
				}
				if (tcp) {
					return tcp->Receive(out);
				}
				return 0;
			}

			void CloseTransportLocked() {
				if (tls) {
					tls->Close();
				}
				if (tcp) {
					tcp->Close();
				}
			}

			void FailLocked(str message) {
				error = std::move(message);
				CloseTransportLocked();
				state = WebSocketState::Failed;
			}

			void FlushTxLocked() {
				while (!txBuffer.empty()) {
					const size_t sent = TransportSendLocked(txBuffer);
					if (sent == 0) {
						break;
					}
					txBuffer.erase(txBuffer.begin(), txBuffer.begin() + static_cast<std::ptrdiff_t>(sent));
				}
				if (state == WebSocketState::Closing && txBuffer.empty() && closeReceived) {
					CloseTransportLocked();
					state = WebSocketState::Closed;
				}
			}

			void DrainRxLocked() {
				u8 chunk[16384];
				for (;;) {
					const size_t got = TransportReceiveLocked(chunk);
					if (got == 0) {
						break;
					}
					rxBuffer.insert(rxBuffer.end(), chunk, chunk + got);
				}
			}

			void AdvanceLocked() {
				if (state == WebSocketState::Closed || state == WebSocketState::Failed) {
					return;
				}

				const SocketState tState = TransportStateLocked();
				if (tState == SocketState::Connecting) {
					return;
				}
				if (tState == SocketState::Failed) {
					str err = TransportErrorLocked();
					FailLocked(err.empty() ? str("WebSocket transport failed") : std::move(err));
					return;
				}

				if (role == Role::Client && !handshakeSent) {
					handshakeSent = true;
					const str req = std::format(
						"GET {} HTTP/1.1\r\n"
						"Host: {}\r\n"
						"Upgrade: websocket\r\n"
						"Connection: Upgrade\r\n"
						"Sec-WebSocket-Key: {}\r\n"
						"Sec-WebSocket-Version: 13\r\n"
						"\r\n",
						requestTarget, hostHeader, clientKey);
					txBuffer.insert(txBuffer.end(), req.begin(), req.end());
				}

				FlushTxLocked();
				DrainRxLocked();

				if (state == WebSocketState::Connecting) {
					if (role == Role::Client) {
						AdvanceClientHandshakeLocked();
					} else {
						AdvanceServerHandshakeLocked();
					}
				}

				if (state == WebSocketState::Open || state == WebSocketState::Closing) {
					DecodeFramesLocked();
					FlushTxLocked();
				}

				if (TransportStateLocked() == SocketState::Closed &&
					state != WebSocketState::Closed && state != WebSocketState::Failed) {
					if (state == WebSocketState::Connecting) {
						FailLocked("connection closed during WebSocket handshake");
					} else {
						state = WebSocketState::Closed;
					}
				}
			}

			void AdvanceClientHandshakeLocked() {
				if (rxBuffer.size() > MAX_HANDSHAKE_BYTES) {
					FailLocked("WebSocket handshake response too large");
					return;
				}
				const std::string_view view(reinterpret_cast<const char*>(rxBuffer.data()), rxBuffer.size());
				const size_t end = view.find("\r\n\r\n");
				if (end == std::string_view::npos) {
					return;
				}
				const std::string_view headers = view.substr(0, end);
				const size_t firstEol = headers.find("\r\n");
				const std::string_view statusLine = headers.substr(0, firstEol);
				if (statusLine.find(" 101") == std::string_view::npos) {
					FailLocked("WebSocket server rejected upgrade: " + str(statusLine));
					return;
				}
				const std::string_view accept = FindHeaderValue(headers, "Sec-WebSocket-Accept");
				if (accept != expectedAccept) {
					FailLocked("WebSocket server returned invalid Sec-WebSocket-Accept");
					return;
				}
				rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + static_cast<std::ptrdiff_t>(end + 4));
				state = WebSocketState::Open;
			}

			void AdvanceServerHandshakeLocked() {
				if (rxBuffer.size() > MAX_HANDSHAKE_BYTES) {
					FailLocked("WebSocket client handshake too large");
					return;
				}
				const std::string_view view(reinterpret_cast<const char*>(rxBuffer.data()), rxBuffer.size());
				const size_t end = view.find("\r\n\r\n");
				if (end == std::string_view::npos) {
					return;
				}
				const std::string_view headers = view.substr(0, end);
				const std::string_view upgrade = FindHeaderValue(headers, "Upgrade");
				const std::string_view key = FindHeaderValue(headers, "Sec-WebSocket-Key");
				if (!EqualsIgnoreCase(upgrade, "websocket") || key.empty()) {
					FailLocked("invalid WebSocket upgrade request");
					return;
				}
				const str accept = ComputeAcceptKey(key);
				const str response = std::format(
					"HTTP/1.1 101 Switching Protocols\r\n"
					"Upgrade: websocket\r\n"
					"Connection: Upgrade\r\n"
					"Sec-WebSocket-Accept: {}\r\n"
					"\r\n",
					accept);
				rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + static_cast<std::ptrdiff_t>(end + 4));
				txBuffer.insert(txBuffer.end(), response.begin(), response.end());
				state = WebSocketState::Open;
				FlushTxLocked();
			}

			void DecodeFramesLocked() {
				while (rxBuffer.size() >= 2 && (state == WebSocketState::Open || state == WebSocketState::Closing)) {
					const u8 b0 = rxBuffer[0];
					const u8 b1 = rxBuffer[1];
					const bool fin = (b0 & 0x80u) != 0;
					const u8 opcode = b0 & 0x0Fu;
					const bool masked = (b1 & 0x80u) != 0;
					u64 payloadLen = b1 & 0x7Fu;
					size_t headerLen = 2;

					if (payloadLen == 126) {
						if (rxBuffer.size() < 4) {
							return;
						}
						payloadLen = (static_cast<u64>(rxBuffer[2]) << 8) | static_cast<u64>(rxBuffer[3]);
						headerLen = 4;
					} else if (payloadLen == 127) {
						if (rxBuffer.size() < 10) {
							return;
						}
						payloadLen = 0;
						for (size_t i = 0; i < 8; ++i) {
							payloadLen = (payloadLen << 8) | static_cast<u64>(rxBuffer[2 + i]);
						}
						headerLen = 10;
					}

					if (payloadLen > MAX_MESSAGE_BYTES) {
						FailLocked("WebSocket frame exceeds maximum message size");
						return;
					}
					const size_t maskOffset = headerLen;
					if (masked) {
						headerLen += 4;
					}
					if (rxBuffer.size() < headerLen + static_cast<size_t>(payloadLen)) {
						return;
					}

					Vector<u8> payload{GetMemoryResource(MemTag::Platform)};
					payload.resize(static_cast<size_t>(payloadLen));
					if (payloadLen > 0) {
						std::memcpy(payload.data(), rxBuffer.data() + headerLen, static_cast<size_t>(payloadLen));
						if (masked) {
							const u8* key = rxBuffer.data() + maskOffset;
							for (size_t i = 0; i < payload.size(); ++i) {
								payload[i] ^= key[i & 3u];
							}
						}
					}
					rxBuffer.erase(rxBuffer.begin(),
								   rxBuffer.begin() + static_cast<std::ptrdiff_t>(headerLen + static_cast<size_t>(payloadLen)));

					if (opcode == OPCODE_PING) {
						AppendFrame(txBuffer, OPCODE_PONG, payload, role == Role::Client);
						continue;
					}
					if (opcode == OPCODE_PONG) {
						continue;
					}
					if (opcode == OPCODE_CLOSE) {
						closeReceived = true;
						if (payload.size() >= 2) {
							closeCode = static_cast<u16>((static_cast<u16>(payload[0]) << 8) | payload[1]);
							if (payload.size() > 2) {
								closeReason.assign(reinterpret_cast<const char*>(payload.data() + 2), payload.size() - 2);
							}
						}
						if (!closeSent) {
							AppendFrame(txBuffer, OPCODE_CLOSE, payload, role == Role::Client);
							closeSent = true;
							FlushTxLocked();
						}
						CloseTransportLocked();
						state = WebSocketState::Closed;
						return;
					}

					if (opcode == OPCODE_TEXT || opcode == OPCODE_BINARY) {
						fragmentKind = opcode == OPCODE_TEXT ? WebSocketMessageKind::Text : WebSocketMessageKind::Binary;
						fragmentBuffer = std::move(payload);
						inFragment = !fin;
						if (fin) {
							WebSocketMessage msg;
							msg.kind = fragmentKind;
							msg.payload = std::move(fragmentBuffer);
							queuedMessages.push_back(std::move(msg));
						}
					} else if (opcode == OPCODE_CONTINUATION && inFragment) {
						if (fragmentBuffer.size() + payload.size() > MAX_MESSAGE_BYTES) {
							FailLocked("WebSocket fragmented message exceeds limit");
							return;
						}
						fragmentBuffer.insert(fragmentBuffer.end(), payload.begin(), payload.end());
						if (fin) {
							inFragment = false;
							WebSocketMessage msg;
							msg.kind = fragmentKind;
							msg.payload = std::move(fragmentBuffer);
							queuedMessages.push_back(std::move(msg));
						}
					}
				}
			}

			void CollectPendingEventsLocked(Vector<WebSocketEvent>& out) {
				if (!callback) {
					return;
				}
				if ((state == WebSocketState::Open || state == WebSocketState::Closing) && !openedReported) {
					openedReported = true;
					WebSocketEvent ev;
					ev.type = WebSocketEventType::Opened;
					out.push_back(std::move(ev));
				}
				while (!queuedMessages.empty()) {
					WebSocketEvent ev;
					ev.type = WebSocketEventType::Message;
					ev.message = std::move(queuedMessages.front());
					queuedMessages.pop_front();
					out.push_back(std::move(ev));
				}
				if ((state == WebSocketState::Closed || state == WebSocketState::Failed) && !endReported) {
					endReported = true;
					WebSocketEvent ev;
					ev.type = state == WebSocketState::Closed ? WebSocketEventType::Closed : WebSocketEventType::Failed;
					ev.closeCode = closeCode;
					ev.reason = state == WebSocketState::Closed ? closeReason : error;
					out.push_back(std::move(ev));
				}
			}

			const Role role;
			mutable std::mutex lock;
			std::unique_ptr<TCPSocket> tcp;
			std::unique_ptr<TLSStream> tls;
			WebSocketState state = WebSocketState::Connecting;
			str hostHeader;
			str requestTarget;
			str clientKey;
			str expectedAccept;
			str error;
			str closeReason;
			u16 closeCode = 1000;
			bool handshakeSent = false;
			bool closeSent = false;
			bool closeReceived = false;
			bool openedReported = false;
			bool endReported = false;
			bool inFragment = false;
			WebSocketMessageKind fragmentKind = WebSocketMessageKind::Binary;
			Vector<u8> fragmentBuffer{GetMemoryResource(MemTag::Platform)};
			Vector<u8> rxBuffer{GetMemoryResource(MemTag::Platform)};
			Vector<u8> txBuffer{GetMemoryResource(MemTag::Platform)};
			std::deque<WebSocketMessage> queuedMessages;
			WebSocketCallback callback;
		};

	} // anonymous namespace

	std::shared_ptr<WebSocketCore> Connect(std::string_view url, TLSOptions tlsOptions) {
		auto core = std::make_shared<NativeWebSocketCore>(NativeWebSocketCore::Role::Client);
		ParsedURL parsed;
		str parseError;
		if (!ParseWebSocketURL(url, parsed, parseError)) {
			core->error = std::move(parseError);
			core->state = WebSocketState::Failed;
			return core;
		}
		core->hostHeader = std::move(parsed.hostHeader);
		core->requestTarget = std::move(parsed.target);
		core->clientKey = GenerateClientKey();
		core->expectedAccept = ComputeAcceptKey(core->clientKey);

		if (parsed.secure) {
			if (tlsOptions.serverName.empty()) {
				tlsOptions.serverName = parsed.host;
			}
			core->tls = TLSStream::Connect(parsed.host, parsed.port, std::move(tlsOptions));
		} else {
			core->tcp = TCPSocket::Connect(parsed.host, parsed.port);
		}
		return core;
	}

	std::shared_ptr<WebSocketCore> AcceptTCP(std::unique_ptr<TCPSocket> socket) {
		auto core = std::make_shared<NativeWebSocketCore>(NativeWebSocketCore::Role::Server);
		if (!socket) {
			core->error = "null TCP socket";
			core->state = WebSocketState::Failed;
			return core;
		}
		core->tcp = std::move(socket);
		return core;
	}

	std::shared_ptr<WebSocketCore> AcceptTLS(std::unique_ptr<TLSStream> stream) {
		auto core = std::make_shared<NativeWebSocketCore>(NativeWebSocketCore::Role::Server);
		if (!stream) {
			core->error = "null TLS stream";
			core->state = WebSocketState::Failed;
			return core;
		}
		core->tls = std::move(stream);
		return core;
	}

} // namespace Shalltear::Net::WebSocketNative
