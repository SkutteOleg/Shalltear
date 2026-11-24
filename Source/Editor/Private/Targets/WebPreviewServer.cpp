#include "Shalltear/Editor/WebPreviewServer.h"

#include "Platform/PreviewSocket.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/StringUtil.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

namespace Shalltear::WebPreviewServer {

	namespace {

		namespace fs = std::filesystem;

		struct ServerState {
			std::mutex configLock;
			str rootDirectory;
			bool crossOriginIsolated = false;

			std::mutex lifeLock;
			EditorPlatform::PreviewSocket::Handle listener = EditorPlatform::PreviewSocket::INVALID;
			std::thread thread;
			std::atomic<bool> stop{false};
			std::atomic<u16> port{0};
		};

		ServerState& State() {
			static ServerState state;
			return state;
		}

		bool DecodePercentPath(std::string_view encoded, str& out) {
			out.clear();
			out.reserve(encoded.size());
			auto hexValue = [](char c) -> i32 {
				if (c >= '0' && c <= '9') {
					return c - '0';
				}
				if (c >= 'a' && c <= 'f') {
					return 10 + (c - 'a');
				}
				if (c >= 'A' && c <= 'F') {
					return 10 + (c - 'A');
				}
				return -1;
			};
			for (size_t i = 0; i < encoded.size(); ++i) {
				if (encoded[i] == '%' && i + 2 < encoded.size()) {
					const i32 hi = hexValue(encoded[i + 1]);
					const i32 lo = hexValue(encoded[i + 2]);
					if (hi >= 0 && lo >= 0) {
						out.push_back(static_cast<char>((hi << 4) | lo));
						i += 2;
						continue;
					}
				}
				out.push_back(encoded[i]);
			}
			return true;
		}

		bool IsSafeRelativePath(std::string_view relative) {
			if (relative.empty() || relative.front() == '/' || relative.front() == '\\') {
				return false;
			}
			if (relative.find('\0') != std::string_view::npos || relative.find('\\') != std::string_view::npos) {
				return false;
			}
			size_t start = 0;
			while (start < relative.size()) {
				size_t end = relative.find('/', start);
				if (end == std::string_view::npos) {
					end = relative.size();
				}
				const std::string_view segment = relative.substr(start, end - start);
				if (segment == "..") {
					return false;
				}
				start = end + 1;
			}
			return true;
		}

		void SendResponse(EditorPlatform::PreviewSocket::Handle client, u32 statusCode, std::string_view statusText,
						  std::string_view contentType, std::span<const u8> body, bool crossOriginIsolated,
						  bool headOnly) {
			str header = std::format(
				"HTTP/1.1 {} {}\r\n"
				"Content-Type: {}\r\n"
				"Content-Length: {}\r\n"
				"Cache-Control: no-store\r\n"
				"Connection: close\r\n",
				statusCode, statusText, contentType, body.size());
			if (crossOriginIsolated) {
				header += "Cross-Origin-Opener-Policy: same-origin\r\n";
				header += "Cross-Origin-Embedder-Policy: require-corp\r\n";
			}
			header += "\r\n";

			if (!EditorPlatform::PreviewSocket::SendAll(
					client, {reinterpret_cast<const u8*>(header.data()), header.size()})) {
				return;
			}
			if (!headOnly && !body.empty()) {
				EditorPlatform::PreviewSocket::SendAll(client, body);
			}
		}

		void HandleClient(EditorPlatform::PreviewSocket::Handle client) {
			u8 buffer[8192];
			const i32 received = EditorPlatform::PreviewSocket::Receive(client, buffer);
			if (received <= 0) {
				return;
			}
			const std::string_view request(reinterpret_cast<const char*>(buffer), static_cast<size_t>(received));
			const size_t lineEnd = request.find("\r\n");
			const std::string_view firstLine = request.substr(0, lineEnd);
			const size_t sp1 = firstLine.find(' ');
			if (sp1 == std::string_view::npos) {
				return;
			}
			const std::string_view method = firstLine.substr(0, sp1);
			const size_t sp2 = firstLine.find(' ', sp1 + 1);
			if (sp2 == std::string_view::npos) {
				return;
			}

			str rootDir;
			bool isolated = false;
			{
				std::lock_guard<std::mutex> guard(State().configLock);
				rootDir = State().rootDirectory;
				isolated = State().crossOriginIsolated;
			}

			const bool isGet = method == "GET";
			const bool isHead = method == "HEAD";
			if (!isGet && !isHead) {
				constexpr std::string_view MSG = "Method Not Allowed";
				SendResponse(client, 405, "Method Not Allowed", "text/plain; charset=utf-8",
							 {reinterpret_cast<const u8*>(MSG.data()), MSG.size()}, isolated, false);
				return;
			}

			std::string_view target = firstLine.substr(sp1 + 1, sp2 - (sp1 + 1));
			if (const size_t q = target.find('?'); q != std::string_view::npos) {
				target = target.substr(0, q);
			}
			str decoded;
			DecodePercentPath(target, decoded);
			if (decoded.empty() || decoded == "/") {
				decoded = "/index.html";
			}
			if (decoded.front() == '/') {
				decoded.erase(decoded.begin());
			}
			if (!IsSafeRelativePath(decoded)) {
				constexpr std::string_view MSG = "Forbidden";
				SendResponse(client, 403, "Forbidden", "text/plain; charset=utf-8",
							 {reinterpret_cast<const u8*>(MSG.data()), MSG.size()}, isolated, isHead);
				return;
			}

			const fs::path fullPath = fs::path(reinterpret_cast<const char8_t*>(rootDir.c_str())) /
									  fs::path(reinterpret_cast<const char8_t*>(decoded.c_str()));
			std::error_code ec;
			if (!fs::is_regular_file(fullPath, ec) || ec) {
				constexpr std::string_view MSG = "Not Found";
				SendResponse(client, 404, "Not Found", "text/plain; charset=utf-8",
							 {reinterpret_cast<const u8*>(MSG.data()), MSG.size()}, isolated, isHead);
				return;
			}

			const std::u8string u8Full = fullPath.generic_u8string();
			const str pathStr(reinterpret_cast<const char*>(u8Full.data()), u8Full.size());
			Buffer data = FileSystem::ReadFile(Path(pathStr), MemTag::Editor);
			if (!data.data && data.size > 0) {
				constexpr std::string_view MSG = "Internal Server Error";
				SendResponse(client, 500, "Internal Server Error", "text/plain; charset=utf-8",
							 {reinterpret_cast<const u8*>(MSG.data()), MSG.size()}, isolated, isHead);
				return;
			}
			const std::span<const u8> span = data.data ? std::span<const u8>(data.data, data.size) : std::span<const u8>{};
			SendResponse(client, 200, "OK", MIMETypeForPath(decoded), span, isolated, isHead);
		}

		void WorkerLoop() {
			ServerState& state = State();
			while (!state.stop.load(std::memory_order_relaxed)) {
				EditorPlatform::PreviewSocket::Handle client =
					EditorPlatform::PreviewSocket::AcceptWithTimeout(state.listener, 100);
				if (client == EditorPlatform::PreviewSocket::INVALID) {
					continue;
				}
				HandleClient(client);
				EditorPlatform::PreviewSocket::Close(client);
			}
		}

	} // anonymous namespace

	std::string_view MIMETypeForPath(std::string_view path) {
		const size_t dot = path.rfind('.');
		if (dot == std::string_view::npos) {
			return "application/octet-stream";
		}
		const str ext = StringUtil::ToLower(str(path.substr(dot)));
		if (ext == ".html" || ext == ".htm") {
			return "text/html; charset=utf-8";
		}
		if (ext == ".js" || ext == ".mjs") {
			return "application/javascript; charset=utf-8";
		}
		if (ext == ".wasm") {
			return "application/wasm";
		}
		if (ext == ".css") {
			return "text/css; charset=utf-8";
		}
		if (ext == ".json") {
			return "application/json; charset=utf-8";
		}
		if (ext == ".png") {
			return "image/png";
		}
		if (ext == ".jpg" || ext == ".jpeg") {
			return "image/jpeg";
		}
		if (ext == ".svg") {
			return "image/svg+xml";
		}
		if (ext == ".ico") {
			return "image/x-icon";
		}
		if (ext == ".txt") {
			return "text/plain; charset=utf-8";
		}
		return "application/octet-stream";
	}

	str Serve(const str& rootDirectory, bool crossOriginIsolated) {
		ServerState& state = State();
		{
			std::lock_guard<std::mutex> guard(state.configLock);
			state.rootDirectory = rootDirectory;
			state.crossOriginIsolated = crossOriginIsolated;
		}

		std::lock_guard<std::mutex> lifeGuard(state.lifeLock);
		if (state.listener == EditorPlatform::PreviewSocket::INVALID) {
			u16 boundPort = 0;
			state.listener = EditorPlatform::PreviewSocket::ListenLoopback(boundPort);
			if (state.listener == EditorPlatform::PreviewSocket::INVALID || boundPort == 0) {
				SLT_LOG_ERROR("Build", "WebPreviewServer: failed to bind loopback HTTP port");
				return {};
			}
			state.port.store(boundPort, std::memory_order_release);
			state.stop.store(false, std::memory_order_release);
			state.thread = std::thread(&WorkerLoop);
			SLT_LOG_INFO("Build", "WebPreviewServer listening on http://127.0.0.1:{}/ (COOP/COEP={})",
						 boundPort, crossOriginIsolated ? "on" : "off");
		}
		return std::format("http://127.0.0.1:{}/", state.port.load(std::memory_order_acquire));
	}

	void Shutdown() {
		ServerState& state = State();
		std::thread joining;
		EditorPlatform::PreviewSocket::Handle closing = EditorPlatform::PreviewSocket::INVALID;
		{
			std::lock_guard<std::mutex> lifeGuard(state.lifeLock);
			state.stop.store(true, std::memory_order_release);
			closing = state.listener;
			state.listener = EditorPlatform::PreviewSocket::INVALID;
			state.port.store(0, std::memory_order_release);
			joining.swap(state.thread);
		}
		EditorPlatform::PreviewSocket::Close(closing);
		if (joining.joinable()) {
			joining.join();
		}
	}

	u16 GetPort() {
		return State().port.load(std::memory_order_acquire);
	}

	bool IsRunning() {
		return GetPort() != 0;
	}

} // namespace Shalltear::WebPreviewServer
